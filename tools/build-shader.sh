#!/usr/bin/env bash
# =============================================================================
# tools/build-shader.sh - assemble a GCN/RDNA2 shader .s into a raw .text blob
# for sceAgcCreateShader (GPU rendering Step 2/3).
#
#   ./tools/build-shader.sh pp/shaders/rgba_ps.s [out.bin]
#   ./tools/build-shader.sh --all      # (re)build every pp/shaders/*.s + SHA256SUMS
#
# There is no PSSL compiler. This uses llvm-mc-18 (in the pinned container) to
# assemble hand-written AMD GCN assembly for the PS5 GPU (gfx1030 / RDNA2),
# then strips the ELF wrapper down to the bare .text section - the "code" half
# of the (header, code) pair sceAgcCreateShader consumes.
#
# The "header" half (SPI register config, GPR counts, resource layout) is NOT
# produced here - see docs/evo-pro/agc-implementation.md §1/§2: the plan is to
# reuse ProsperoLight's matching .header.bin and swap only this .text.
#
# Verified 2026-09-02: assembles + round-trips through llvm-objdump.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MCPU="gfx1030"                 # PS5 GPU: custom RDNA2, gfx1030-compatible ISA
TRIPLE="amdgcn--amdpal"
SHADER_DIR="pp/shaders"

if [[ ! -f /.dockerenv ]]; then
    exec docker compose run --rm ps5-dev bash ./tools/build-shader.sh "$@"
fi

cd "$REPO_ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

build_one() {
    local src="$1" out="$2"
    llvm-mc-18 --assemble --triple="$TRIPLE" --mcpu="$MCPU" --filetype=obj \
        -o "$TMP/s.o" "$src"
    llvm-objcopy-18 -O binary --only-section=.text "$TMP/s.o" "$out"
    echo "  $src -> $out  ($(stat -c %s "$out") bytes)"
}

disasm_one() {
    echo "  --- disassembly (verify) ---"
    xxd -p "$1" | tr -d '\n' | sed 's/../0x& /g' \
        | llvm-mc-18 --disassemble --triple="$TRIPLE" --mcpu="$MCPU" 2>/dev/null \
        | sed 's/^/    /'
}

if [[ "${1:-}" == "--all" ]]; then
    shopt -s nullglob
    for src in "$SHADER_DIR"/*.s; do
        build_one "$src" "${src%.s}.bin"
    done
    ( cd "$SHADER_DIR" && sha256sum *.bin > SHA256SUMS )
    echo "  wrote $SHADER_DIR/SHA256SUMS"
    exit 0
fi

SRC="${1:?usage: build-shader.sh <file.s> [out.bin]  |  build-shader.sh --all}"
OUT="${2:-${SRC%.s}.bin}"
[[ -f "$SRC" ]] || { echo "no such file: $SRC" >&2; exit 1; }

build_one "$SRC" "$OUT"
disasm_one "$OUT"
