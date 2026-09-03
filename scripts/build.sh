#!/usr/bin/env bash
# =============================================================================
# scripts/build.sh - build the EVO Player project suite.
#
#   ./scripts/build.sh                  build every project
#   ./scripts/build.sh hello_world      build one (or several) by name
#   ./scripts/build.sh --list           show what is available
#   ./scripts/build.sh --clean          clean first
#   ./scripts/build.sh --cmake          also run the CMake/Ninja configure to
#                                       regenerate compile_commands.json
#
# Runs from Windows too - it re-executes itself inside the dev container.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Build order matters: earliest milestones first, so a failure stops you at the
# right place rather than burying the real problem under later noise.
ALL_PROJECTS=(
    hello_world      # milestone 1: toolchain -> ELF -> console
    system_info      # milestone 2: libkernel, firmware check
    videoout_test    # milestone 3: display path
    audioout_test    # milestone 4: audio path
    gpu_test         # milestone 5: GPU capability probe
    decoder_test     # research: native decoder reconnaissance
    avplayer_test    # research: libSceAvPlayer native-decode spike (Route A)
    yuv_gpu_test     # future:   GPU YUV renderer (placeholder)
)

DO_CLEAN=0
DO_CMAKE=0
SELECTED=()

while (( $# )); do
    case "$1" in
        --clean) DO_CLEAN=1 ;;
        --cmake) DO_CMAKE=1 ;;
        --list)
            echo "Available projects:"
            for p in "${ALL_PROJECTS[@]}"; do echo "  ${p}"; done
            exit 0 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        -*) die "unknown option: $1 (try --help)" ;;
        *)  SELECTED+=("$1") ;;
    esac
    shift
done

if ! in_container; then
    reexec_in_container "build.sh" "$@"
fi

(( ${#SELECTED[@]} )) || SELECTED=("${ALL_PROJECTS[@]}")

# Validate names before doing any work.
for p in "${SELECTED[@]}"; do
    [[ -d "${REPO_ROOT}/projects/${p}" ]] \
      || die "no such project: ${p}
       Try: ./scripts/build.sh --list"
done

mkdirs
load_sdk

log "EVO Player build"
sdk_version_banner
echo "  clang           $(clang --version | head -1 | sed 's/.*version //')"
echo "  projects        ${SELECTED[*]}"
echo ""

BUILD_LOG="${LOG_OUT}/build-$(date -u +%Y%m%dT%H%M%SZ).log"
FAILED=()
BUILT=()

for p in "${SELECTED[@]}"; do
    dir="${REPO_ROOT}/projects/${p}"

    if [[ ! -f "${dir}/Makefile" ]]; then
        warn "${p}: no Makefile, skipping"
        continue
    fi

    begin "building ${p}"

    if (( DO_CLEAN )); then
        make -C "${dir}" clean >>"${BUILD_LOG}" 2>&1 || true
    fi

    # Test with `if` rather than `set +e`: bash runs the ERR trap even when
    # errexit is disabled, so `set +e` would not prevent the trap from aborting
    # the whole run on the first failing project. Commands in an `if` condition
    # are exempt, which is what lets us collect every failure and report them
    # together at the end.
    rc=0
    if ! make -C "${dir}" install-elf >>"${BUILD_LOG}" 2>&1; then
        rc=1
    fi

    if (( rc != 0 )); then
        FAILED+=("${p}")
        echo "${_C_RED}  FAILED${_C_OFF} ${p} (exit ${rc})"
        # Show the actual compiler diagnostics - burying them in a log file
        # is exactly the "which step failed?" problem the brief warns about.
        echo "  ---- last 25 lines ----"
        tail -25 "${BUILD_LOG}" | sed 's/^/  /'
        echo "  -----------------------"
        continue
    fi

    # Confirm we produced a real PS5 payload, not a host binary.
    elf="$(find "${dir}" -maxdepth 1 -name '*.elf' -print -quit)"
    if [[ -n "${elf}" ]]; then
        validate_elf "${elf}"
        BUILT+=("${p}")
    else
        warn "${p}: build succeeded but produced no .elf"
    fi
done

echo ""
log "summary"
echo "  built   : ${#BUILT[@]}  (${BUILT[*]:-none})"
echo "  failed  : ${#FAILED[@]} (${FAILED[*]:-none})"
echo "  log     : ${BUILD_LOG#"${REPO_ROOT}/"}"
echo "  elfs    : output/elf/"

# -----------------------------------------------------------------------------
# compile_commands.json for clangd / VS Code IntelliSense.
# The Make-based projects do not emit one, so CMake generates it over the same
# sources. See CMakeLists.txt for why both build systems exist.
# -----------------------------------------------------------------------------
if (( DO_CMAKE )); then
    begin "generating compile_commands.json (CMake + Ninja)"
    "${SCRIPTS_DIR}/gen-compile-commands.sh"
fi

if (( ${#FAILED[@]} )); then
    die "${#FAILED[@]} project(s) failed to build: ${FAILED[*]}"
fi

echo ""
ok "all requested projects built"
