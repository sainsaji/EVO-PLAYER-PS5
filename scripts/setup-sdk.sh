#!/usr/bin/env bash
# =============================================================================
# scripts/setup-sdk.sh - verify / (re)install the PS5 Payload SDK in-container.
#
#   ./scripts/setup-sdk.sh                 verify the installed SDK
#   ./scripts/setup-sdk.sh --from-release  reinstall the pinned release zip
#   ./scripts/setup-sdk.sh --from-source   clone + build the SDK from git
#   ./scripts/setup-sdk.sh --version v0.41 pick a different tag
#
# The Dockerfile already installs the SDK, so the default action is a health
# check: it proves every dependency in the task's validation checklist works
# and that `samples/hello_world` really produces a PS5 ELF.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require_container

SDK_VERSION="v0.42"
MODE="verify"

while (( $# )); do
    case "$1" in
        --from-release) MODE="release" ;;
        --from-source)  MODE="source" ;;
        --version)      shift; SDK_VERSION="${1:?--version needs a tag}" ;;
        -h|--help)      sed -n '2,16p' "$0"; exit 0 ;;
        *)              die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# -----------------------------------------------------------------------------
if [[ "${MODE}" != "verify" ]]; then
    begin "reinstalling SDK ${SDK_VERSION} (${MODE})"
    need_cmd sudo
    export PS5_SDK_VERSION="${SDK_VERSION}"
    if [[ "${MODE}" == "source" ]]; then
        BUILD_SDK_FROM_SOURCE=1
    else
        BUILD_SDK_FROM_SOURCE=0
    fi
    export BUILD_SDK_FROM_SOURCE
    export PS5_PAYLOAD_SDK

    # The SDK volume is owned by the dev user; the installer only needs sudo
    # if that ownership was lost.
    if [[ -w "${PS5_PAYLOAD_SDK}" ]]; then
        "${SCRIPTS_DIR}/install-sdk-image.sh"
    else
        sudo -E "${SCRIPTS_DIR}/install-sdk-image.sh"
        sudo chown -R "$(id -u):$(id -g)" "${PS5_PAYLOAD_SDK}"
    fi
fi

# =============================================================================
# Health check - mirrors the "Final validation checklist" from the brief.
# =============================================================================
begin "toolchain versions"

need_cmd clang ld.lld cmake ninja python3 make pkg-config socat nc file

printf '  %-14s %s\n' "clang"    "$(clang --version | head -1)"
printf '  %-14s %s\n' "ld.lld"   "$(ld.lld --version | head -1)"
printf '  %-14s %s\n' "llvm-config" "$(llvm-config --version)"
printf '  %-14s %s\n' "cmake"    "$(cmake --version | head -1)"
printf '  %-14s %s\n' "ninja"    "$(ninja --version)"
printf '  %-14s %s\n' "make"     "$(make --version | head -1)"
printf '  %-14s %s\n' "meson"    "$(meson --version 2>/dev/null || echo 'not installed')"
printf '  %-14s %s\n' "python3"  "$(python3 --version)"
printf '  %-14s %s\n' "socat"    "$(socat -V 2>&1 | head -1)"

begin "python pyelftools"
# genstub.py (SCE stub generation from .sprx) imports elftools.elf.elffile.
python3 - <<'PY' || die "pyelftools is missing or broken - SCE stub generation from SPRX will fail."
import elftools
from elftools.elf.elffile import ELFFile
print(f"  pyelftools     {getattr(elftools, '__version__', 'unknown')} (ELFFile import ok)")
PY

begin "SDK installation"
need_file "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
need_file "${PS5_PAYLOAD_SDK}/toolchain/prospero.mk"
need_file "${PS5_PAYLOAD_SDK}/bin/prospero-clang"
need_file "${PS5_PAYLOAD_SDK}/bin/prospero-deploy"
echo "  PS5_PAYLOAD_SDK = ${PS5_PAYLOAD_SDK}"
sdk_version_banner

begin "firmware 12.70 support"
# The CRT picks a kernel offset table from the running firmware version, which
# it encodes as 0xMMmm0000 - so 12.70 is 0x12700000. Upstream crt/kernel.c
# carries `case 0x12700000:` in the 12.00-12.70 group (see docs/sdk-audit.md);
# that source audit is the authoritative answer.
#
# The release zip ships compiled objects, not crt/kernel.c, so here we look for
# the constant as a little-endian immediate inside crt1.o (which is where
# kernel.c ends up). Two independent constants from the same switch arm are
# checked, because a single 4-byte pattern can match by chance at some
# unrelated offset.
FW_OK=0
CRT1="${PS5_PAYLOAD_SDK}/target/lib/crt1.o"
if [[ -f "${CRT1}" ]] && command -v xxd >/dev/null 2>&1; then
    CRT1_HEX="$(xxd -p "${CRT1}" | tr -d '\n')"
    #                 0x12700000 -> 00 00 70 12   (the firmware case label)
    #                 0x2885E00  -> 00 5e 88 02   (that arm's ALLPROC offset)
    if [[ "${CRT1_HEX}" == *00007012* ]] && [[ "${CRT1_HEX}" == *005e8802* ]]; then
        FW_OK=1
    fi
    unset CRT1_HEX
elif [[ -f "${PS5_PAYLOAD_SDK}/crt/kernel.c" ]]; then
    # Source install - check directly, which is unambiguous.
    grep -q "case 0x12700000:" "${PS5_PAYLOAD_SDK}/crt/kernel.c" && FW_OK=1
fi

if (( FW_OK )); then
    ok "SDK carries the 12.70 (0x12700000) kernel offset table"
else
    warn "could not confirm 0x12700000 in the installed binaries."
    warn "This is a heuristic scan, not proof - see docs/sdk-audit.md for the"
    warn "source-level audit. Confirm on hardware by running system_info,"
    warn "which prints the console's actual firmware word."
fi

begin "building samples/hello_world (SDK smoke test)"
load_sdk
# Build out-of-tree so the SDK volume stays clean.
SMOKE="$(mktemp -d)"
trap 'rm -rf -- "${SMOKE}"' EXIT
cp -r "${PS5_PAYLOAD_SDK}/samples/hello_world/." "${SMOKE}/"
make -C "${SMOKE}" >/dev/null || die "samples/hello_world failed to build.
       This is the SDK's own sample - the toolchain itself is broken."
validate_elf "${SMOKE}/hello_world.elf"

echo ""
log "${_C_GRN}SDK environment OK${_C_OFF}"
echo "   next:  ./scripts/build.sh          build the EVO Player sample suite"
echo "          ./scripts/deploy.sh <elf>   send a payload to the console"
