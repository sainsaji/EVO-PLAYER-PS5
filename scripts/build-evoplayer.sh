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
ok "-> output/elf/${ELF_NAME}"

# Also sync assets if running
if (( DO_RUN )); then
    begin "syncing homebrew assets"
    python3 -c "
import ftplib, os, sys
host = '${PS5_HOST:-192.168.0.12}'
port = 2121
try:
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=5)
    ftp.login()
    ftp.set_pasv(True)
    def ensure_dir(d):
        cur = ''
        for p in d.strip('/').split('/'):
            cur += '/' + p
            try: ftp.mkd(cur)
            except Exception: pass
    def upload(src, dst):
        with open(src, 'rb') as f:
            ftp.storbinary(f'STOR {dst}', f)
    for root, dirs, files in os.walk('projects/evoplayer/assets'):
        rel = os.path.relpath(root, 'projects/evoplayer/assets').replace('\\\\', '/')
        r_dir = '/data/homebrew/EVOPlayer/assets' + ('/' + rel if rel != '.' else '')
        ensure_dir(r_dir)
        for f in files:
            upload(os.path.join(root, f), r_dir + '/' + f)
        r_dir2 = '/data/evoplayer/app/assets' + ('/' + rel if rel != '.' else '')
        ensure_dir(r_dir2)
        for f in files:
            upload(os.path.join(root, f), r_dir2 + '/' + f)
    ftp.quit()
except Exception as e:
    pass
" 2>/dev/null || true

    "${SCRIPTS_DIR}/install-homebrew.sh" --run --timeout "${RUN_TIMEOUT}" \
        --name EVOPlayer "${ELF_OUT}/${ELF_NAME}"
else
    echo ""
    echo "   Install and launch:"
    echo "     ./scripts/build-evoplayer.sh --run"
fi
