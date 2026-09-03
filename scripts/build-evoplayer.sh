#!/usr/bin/env bash
# =============================================================================
# scripts/build-evoplayer.sh - HOST COMPILE CHECK for the EVO Player fork.
#
#   ./scripts/build-evoplayer.sh              build the ELF (compile check only)
#   ./scripts/build-evoplayer.sh --stage 0    build a specific 4K stage
#
# This produces output/elf/EVOPlayer.elf purely so the payload / non-app-module
# code path stays green (issues #31 §4, #36, docs/modularisation-plan.md parity).
# It does NOT and MUST NOT deploy or launch anything on a console. The ONLY
# hardware path is the app module:
#     scripts/package-app.sh --ffpfsc  &&  scripts/deploy-app.sh --ffpfsc
# The ELF-push scripts (install-homebrew / launch / deploy) were deleted
# 2026-09-03 - see docs/tooling.md and the `never-elf` rule.
#
# Same transitive-link fix as build-prosperoplayer.sh: pacbrew's FFmpeg is
# built with openssl/ass/freetype/fribidi/harfbuzz, and static archives carry
# no dependency metadata, so those libraries must appear on the link line.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DEST="${REPO_ROOT}/projects/evoplayer"
ELF_NAME="EVOPlayer.elf"
STAGE=""

while (( $# )); do
    case "$1" in
        --stage)   shift; STAGE="${1:?--stage needs a value}" ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        --run)     die "--run is gone. EVO deploys via the app module only:
       scripts/package-app.sh --ffpfsc && scripts/deploy-app.sh --ffpfsc" ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    FWD=()
    [[ -n "${STAGE}" ]] && FWD+=(--stage "${STAGE}")
    reexec_in_container "build-evoplayer.sh" "${FWD[@]+"${FWD[@]}"}"
fi

need_file "${DEST}/Makefile" "projects/evoplayer is empty. Fork it first:
       see docs/prosperoplayer-baseline.md"
load_sdk
mkdirs

HB="${PS5_SYSROOT}/user/homebrew"

UPSTREAM_LIBS=(
    "${HB}/lib/librmlui.a"
    "${HB}/lib/libSDL2.a"
    "${HB}/lib/libavformat.a"
    "${HB}/lib/libavcodec.a"
    "${HB}/lib/libswresample.a"
    "${HB}/lib/libavutil.a"
    "${HB}/lib/libswscale.a"
)
TRANSITIVE_LIBS=(
    -lass -lharfbuzz -lharfbuzz-subset -lfreetype -lfribidi -lpng16
    -lsamplerate -lssl -lcrypto -liconv
    -lz -lbz2 -llzma -lzstd -lm
)
SCE_LIBS=(
    -lSceNotification -lSceSystemService -lSceUserService -lScePad
    -lSceAudioOut -lSceVideoOut -lSceKeyboard -lSceImeDialog
    -lc++ -lc++abi -lpthread
)

MAKE_ARGS=("ELF=${ELF_NAME}"
           "LIBS=${UPSTREAM_LIBS[*]} ${TRANSITIVE_LIBS[*]} ${SCE_LIBS[*]}")
[[ -n "${STAGE}" ]] && MAKE_ARGS+=("STAGE=${STAGE}")

# Development switches, e.g. EXTRA_CFLAGS="-DEVO_AUTOSHOT=4 -DEVO_START_SCREEN=10".
# Going through the script rather than calling make directly matters: the LIBS
# override above supplies transitive dependencies the project Makefile does not
# list, so a bare `make` fails to link.
[[ -n "${EXTRA_CFLAGS:-}" ]] && MAKE_ARGS+=("EXTRA_CFLAGS=${EXTRA_CFLAGS}")

begin "building EVO Player${STAGE:+ (stage ${STAGE})}"
BUILD_LOG="${LOG_OUT}/evoplayer-$(date -u +%Y%m%dT%H%M%SZ).log"

# Force a relink and rebuild of object files
rm -f "${DEST}/${ELF_NAME}"
find "${DEST}" -name "*.o" -delete
# These payload-flavoured .o would otherwise be silently reused by the next
# scripts/package-app.sh (its cflags stamp only tracks app-build -> app-build
# changes). Invalidate the stamp so the next app build force-cleans.
rm -f "${OUTPUT_DIR}/app/.build/app-cflags.stamp"

if ! make -C "${DEST}" "${MAKE_ARGS[@]}" > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines ---"
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "EVO Player build failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi

# Prove the switches asked for actually reached the compiler.
if [[ -n "${EXTRA_CFLAGS:-}" ]]; then
    for flag in ${EXTRA_CFLAGS}; do
        grep -qF -- "${flag}" "${BUILD_LOG}" \
            || die "EXTRA_CFLAGS was set but ${flag} never reached the compile
       line. Check reexec_in_container in scripts/common.sh.
       Log: ${BUILD_LOG#"${REPO_ROOT}/"}"
    done
    ok "EXTRA_CFLAGS applied: ${EXTRA_CFLAGS}"
fi

validate_elf "${DEST}/${ELF_NAME}"
cp -f "${DEST}/${ELF_NAME}" "${ELF_OUT}/"
ok "-> output/elf/${ELF_NAME}  (compile check only - not a deploy target)"
