#!/usr/bin/env bash
# =============================================================================
# scripts/build-evoplayer.sh - build the EVO Player fork.
#
#   ./scripts/build-evoplayer.sh              build
#   ./scripts/build-evoplayer.sh --run        build, install as homebrew, launch
#   ./scripts/build-evoplayer.sh --stage 0    build a specific 4K stage
#
# Same transitive-link fix as build-prosperoplayer.sh: pacbrew's FFmpeg is
# built with openssl/ass/freetype/fribidi/harfbuzz, and static archives carry
# no dependency metadata, so those libraries must appear on the link line.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DEST="${REPO_ROOT}/projects/evoplayer"
ELF_NAME="EVOPlayer.elf"
STAGE=""
DO_RUN=0
RUN_TIMEOUT=40

while (( $# )); do
    case "$1" in
        --run)     DO_RUN=1 ;;
        --stage)   shift; STAGE="${1:?--stage needs a value}" ;;
        --timeout) shift; RUN_TIMEOUT="${1:?--timeout needs seconds}" ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    FWD=()
    (( DO_RUN )) && FWD+=(--run --timeout "${RUN_TIMEOUT}")
    [[ -n "${STAGE}" ]] && FWD+=(--stage "${STAGE}")
    reexec_in_container "build-evoplayer.sh" "${FWD[@]+"${FWD[@]}"}"
fi

need_file "${DEST}/Makefile" "projects/evoplayer is empty. Fork it first:
       see docs/prosperoplayer-baseline.md"
load_sdk
mkdirs

HB="${PS5_SYSROOT}/user/homebrew"

UPSTREAM_LIBS=(
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

begin "building EVO Player${STAGE:+ (stage ${STAGE})}"
BUILD_LOG="${LOG_OUT}/evoplayer-$(date -u +%Y%m%dT%H%M%SZ).log"

# Force a relink every time.
#
# Upstream's rule is `$(ELF): main.c $(PP_SRCS)` - it tracks sources but not
# CFLAGS and not headers. Change a -D flag or a pp/include header and make
# reports "up to date", leaving the previous binary in place. That silently
# cost a debugging cycle: a build with a flag change was installed, launched,
# and drew the wrong conclusion because the ELF had never been rebuilt.
# A full relink of one translation unit is a few seconds; correctness wins.
rm -f "${DEST}/${ELF_NAME}"

if ! make -C "${DEST}" "${MAKE_ARGS[@]}" > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines ---"
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "EVO Player build failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi

validate_elf "${DEST}/${ELF_NAME}"
cp -f "${DEST}/${ELF_NAME}" "${ELF_OUT}/"
ok "-> output/elf/${ELF_NAME}"

if (( DO_RUN )); then
    "${SCRIPTS_DIR}/install-homebrew.sh" --run --timeout "${RUN_TIMEOUT}" \
        --name EVOPlayer "${ELF_OUT}/${ELF_NAME}"
else
    echo ""
    echo "   Install and launch:"
    echo "     ./scripts/build-evoplayer.sh --run"
fi
