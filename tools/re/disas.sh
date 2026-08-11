#!/usr/bin/env bash
# =============================================================================
# tools/re/disas.sh - disassemble a dumped module segment.
#
#   ./tools/re/disas.sh libSceAvPlayer 0xd00 0x120
#   ./tools/re/disas.sh libSceVideoDecoderArbitration 0 0x400
#
# The dump is a raw mapped image with no ELF header - the loader does not map
# one - so the disassembler has to be told it is a flat x86-64 blob and where
# it lived. --adjust-vma makes the printed addresses match the addresses the
# console reported, so offsets in the log line up with what you read here.
#
# GNU objdump rather than llvm-objdump: llvm-objdump 18 has no raw-binary
# input mode (-b/--target=binary are both rejected), and wrapping the blob in
# an ELF just to disassemble it is more moving parts than this needs.
#
# Run inside the dev container (./scripts/shell.sh) or via
# docker compose run --rm -T ps5-dev.
# =============================================================================
set -euo pipefail

MOD="${1:?usage: disas.sh <module-shortname> <offset> [length] [segment]}"
OFF="${2:?need an offset within the segment, e.g. 0xd00}"
LEN="${3:-0x100}"
SEG="${4:-0}"

DUMP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/proprietary/dump"
BIN="${DUMP_DIR}/evo_dump_${MOD}_s${SEG}.bin"

[[ -f "${BIN}" ]] || {
    echo "no such dump: ${BIN}" >&2
    echo "available:" >&2
    ls -1 "${DUMP_DIR}"/*.bin 2>/dev/null | sed 's/^/  /' >&2
    exit 1
}

# The mapped base address of this segment, taken from the manifest the probe
# wrote, so nothing here hardcodes an address that changes every boot.
VADDR="$(awk -F'\t' -v m="${MOD}" -v s="seg${SEG}" '
    $1 ~ m && $2 == s { for (i = 3; i <= NF; i++)
                            if ($i ~ /^vaddr=/) { sub(/^vaddr=/, "", $i); print $i } }
' "${DUMP_DIR}/evo_dump_manifest.txt" | head -1)"

[[ -n "${VADDR}" ]] || { echo "no vaddr for ${MOD} seg${SEG} in the manifest" >&2; exit 1; }

START=$(( VADDR + OFF ))
STOP=$(( START + LEN ))

echo "# ${MOD} segment ${SEG} mapped at ${VADDR}, showing +${OFF} for ${LEN} bytes"
objdump -D -b binary -m i386:x86-64 -M intel --no-show-raw-insn \
    --adjust-vma="${VADDR}" \
    --start-address="$(printf '0x%x' "${START}")" \
    --stop-address="$(printf '0x%x' "${STOP}")" \
    "${BIN}" | tail -n +7
