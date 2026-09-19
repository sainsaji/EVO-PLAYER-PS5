#!/usr/bin/env bash
# =============================================================================
# scripts/build-prosperoplayer.sh
#
# Import the upstream ProsperoPlayer and build it UNMODIFIED, to establish the
# baseline EVO Player forks from.
#
#   ./scripts/build-prosperoplayer.sh              clone (if needed) + build
#   ./scripts/build-prosperoplayer.sh --clone-only
#   ./scripts/build-prosperoplayer.sh --stage 0    build a specific 4K stage
#   ./scripts/build-prosperoplayer.sh --audit      report deps, do not build
#
# ---------------------------------------------------------------------------
# WHY UNMODIFIED FIRST
#   The brief is explicit: do not touch the source during initial setup. Get
#   the existing project compiling in this container, record every error, fix
#   the ENVIRONMENT rather than the code, and only then start forking.
#
# WHAT UPSTREAM NEEDS (audited from its Makefile)
#   Static libs, all in $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib:
#       libSDL2.a libavformat.a libavcodec.a libswresample.a
#       libavutil.a libswscale.a
#   Headers under .../target/user/homebrew/include{,/SDL2}
#   SCE stubs: SceNotification SceSystemService SceUserService ScePad
#              SceAudioOut SceVideoOut SceKeyboard SceImeDialog
#   C++ runtime: -lc++ -lc++abi -lpthread
#   All of the above are supplied by the pacbrew sysroot baked into the image;
#   every one of those stubs exists in SDK v0.42's sce_stubs/.
# ---------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

UPSTREAM_URL="https://github.com/KINGDKAK/ProsperoPlayer"
DEST="${REPO_ROOT}/projects/prosperoplayer"
STAGE=""
CLONE_ONLY=0
AUDIT_ONLY=0

while (( $# )); do
    case "$1" in
        --clone-only) CLONE_ONLY=1 ;;
        --audit)      AUDIT_ONLY=1 ;;
        --stage)      shift; STAGE="${1:?--stage needs a value}" ;;
        -h|--help)    sed -n '2,32p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    reexec_in_container "build-prosperoplayer.sh" "$@"
fi

need_cmd git
load_sdk
mkdirs

# -----------------------------------------------------------------------------
begin "fetching upstream ProsperoPlayer"
if [[ -d "${DEST}/.git" ]]; then
    ok "already cloned at projects/prosperoplayer"
    git -C "${DEST}" log -1 --format='  HEAD %h %s (%ci)' || true
else
    # Keep the whole history: the reverse-engineering workflow benefits from
    # being able to bisect upstream behaviour.
    git clone "${UPSTREAM_URL}" "${DEST}" \
      || die "clone failed. Check network access from the container:
       curl -sS -o /dev/null -w '%{http_code}' ${UPSTREAM_URL}"
    git -C "${DEST}" log -1 --format='  HEAD %h %s (%ci)'
fi

(( CLONE_ONLY )) && { ok "clone only, stopping here."; exit 0; }

need_file "${DEST}/Makefile" "upstream layout changed - no Makefile at the root."

# =============================================================================
# Dependency audit. Runs before the build so a missing library produces a clear
# message instead of a wall of linker errors.
# =============================================================================
begin "auditing dependencies"

HB="${PS5_SYSROOT}/user/homebrew"
MISSING=()

echo "  static libraries:"
for l in libSDL2.a libavformat.a libavcodec.a libswresample.a libavutil.a libswscale.a; do
    if [[ -f "${HB}/lib/${l}" ]]; then
        printf '    [x] %-18s %8s KiB\n' "${l}" "$(( $(stat -c %s "${HB}/lib/${l}") / 1024 ))"
    else
        printf '    [ ] %-18s MISSING\n' "${l}"
        MISSING+=("${HB}/lib/${l}")
    fi
done

echo "  headers:"
for h in SDL2/SDL.h libavcodec/avcodec.h libavformat/avformat.h \
         libavutil/avutil.h libswresample/swresample.h libswscale/swscale.h; do
    if [[ -f "${HB}/include/${h}" ]]; then
        printf '    [x] %s\n' "${h}"
    else
        printf '    [ ] %s MISSING\n' "${h}"
        MISSING+=("${HB}/include/${h}")
    fi
done

echo "  SCE stubs:"
for s in SceNotification SceSystemService SceUserService ScePad \
         SceAudioOut SceVideoOut SceKeyboard SceImeDialog; do
    if [[ -f "${PS5_SYSROOT}/lib/lib${s}.so" ]]; then
        printf '    [x] lib%s.so\n' "${s}"
    else
        printf '    [ ] lib%s.so MISSING\n' "${s}"
        MISSING+=("${PS5_SYSROOT}/lib/lib${s}.so")
    fi
done

echo "  C++ runtime:"
for s in libc++.a libc++abi.a; do
    if [[ -f "${PS5_SYSROOT}/lib/${s}" ]]; then
        printf '    [x] %s\n' "${s}"
    else
        printf '    [ ] %s MISSING\n' "${s}"
        MISSING+=("${PS5_SYSROOT}/lib/${s}")
    fi
done

# The FFmpeg version actually present, which is what upstream links against.
if [[ -f "${HB}/include/libavutil/ffversion.h" ]]; then
    FFVER="$(sed -n 's/.*FFMPEG_VERSION *"\([^"]*\)".*/\1/p' "${HB}/include/libavutil/ffversion.h")"
    echo "  ffmpeg version: ${FFVER}"
fi

if (( ${#MISSING[@]} )); then
    die "${#MISSING[@]} dependency/ies missing from the sysroot:
$(printf '         %s\n' "${MISSING[@]}")

       These normally come from the prebuilt pacbrew sysroot baked into the
       image. If they are absent the image was probably built with
       INSTALL_PACBREW=0. Rebuild with:
           docker compose build --build-arg INSTALL_PACBREW=1"
fi
ok "all upstream dependencies present"

(( AUDIT_ONLY )) && { ok "audit only, stopping here."; exit 0; }

# =============================================================================
begin "building ProsperoPlayer (unmodified)"
BUILD_LOG="${LOG_OUT}/prosperoplayer-$(date -u +%Y%m%dT%H%M%SZ).log"

# -----------------------------------------------------------------------------
# ENVIRONMENT FIX: transitive static-link dependencies.
#
# Upstream's LIBS list names only the six libav*/SDL2 archives plus the SCE
# stubs. That is not enough to link against THIS sysroot, because pacbrew
# builds FFmpeg with external libraries enabled (see pacbrew-repo/ffmpeg/
# PKGBUILD: --enable-openssl --enable-libfreetype --enable-libfribidi
# --enable-libharfbuzz --enable-libass). Static archives carry no dependency
# information, so every one of those has to appear on the link line too.
#
# Without them the link fails with, in order as you add each one back:
#   BN_set_word, BN_num_bits, BN_rand, BN_CTX_new, BN_mod_exp   -> -lssl -lcrypto
#   libiconv, libiconv_open, libiconv_close                     -> -liconv
# plus ass/harfbuzz/freetype/fribidi/png and the compression libraries.
#
# This is fixed HERE, in the environment, rather than by editing upstream's
# Makefile - the whole point of this step is a faithful, unmodified baseline.
# `make LIBS=...` on the command line overrides the Makefile's `LIBS :=`.
#
# Set EVO_SKIP_LINK_FIX=1 to build with upstream's list verbatim and observe
# the failure for yourself.
# -----------------------------------------------------------------------------
MAKE_ARGS=()
[[ -n "${STAGE}" ]] && MAKE_ARGS+=("STAGE=${STAGE}")

if [[ "${EVO_SKIP_LINK_FIX:-0}" != "1" ]]; then
    HBLIB="${HB}/lib"
    UPSTREAM_LIBS=(
        "${HBLIB}/libSDL2.a"
        "${HBLIB}/libavformat.a"
        "${HBLIB}/libavcodec.a"
        "${HBLIB}/libswresample.a"
        "${HBLIB}/libavutil.a"
        "${HBLIB}/libswscale.a"
    )
    # Order matters for static linking: consumers before providers.
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
    MAKE_ARGS+=("LIBS=${UPSTREAM_LIBS[*]} ${TRANSITIVE_LIBS[*]} ${SCE_LIBS[*]}")
    echo "  applying transitive-dependency link fix (${#TRANSITIVE_LIBS[@]} extra libs)"
else
    warn "EVO_SKIP_LINK_FIX=1 - using upstream's LIBS verbatim; expect"
    warn "undefined OpenSSL/iconv symbols from libavformat.a"
fi

echo "  make ${MAKE_ARGS[*]:-} (log: ${BUILD_LOG#"${REPO_ROOT}/"})"

# Run inside an `if` condition rather than bracketing with `set +e`: bash fires
# the ERR trap even when errexit is off, so `set +e` alone would not stop the
# trap from aborting before we can report the compiler output. Commands tested
# by `if` are exempt from the ERR trap.
if ! make -C "${DEST}" "${MAKE_ARGS[@]}" > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 50 lines ---"
    tail -50 "${BUILD_LOG}" | sed 's/^/  /'
    echo "---------------------"
    echo ""
    # Categorise the failure, since the brief asks for these to be recorded.
    echo "Triage:"
    grep -qE "fatal error: .*: No such file" "${BUILD_LOG}" \
      && echo "  * MISSING HEADER - a dependency is not in the sysroot include path"
    grep -qE "undefined (reference|symbol)" "${BUILD_LOG}" \
      && echo "  * UNDEFINED SYMBOL - a missing SCE stub or an FFmpeg API change"
    grep -qE "cannot find -l" "${BUILD_LOG}" \
      && echo "  * MISSING LIBRARY - check the -l flags against the sysroot"
    grep -qE "error: (unknown type name|implicit declaration)" "${BUILD_LOG}" \
      && echo "  * API MISMATCH - likely an FFmpeg major-version incompatibility"
    echo ""
    die "ProsperoPlayer failed to build.
       Record the findings in docs/prosperoplayer-baseline.md, then fix the
       ENVIRONMENT rather than upstream's source - the point of this step is a
       faithful baseline."
fi

ELF_BUILT="${DEST}/PS5MediaPlayerPRO.elf"
if [[ -n "${STAGE}" ]]; then
    ok "stage ${STAGE} built"
fi

if [[ -f "${ELF_BUILT}" ]]; then
    validate_elf "${ELF_BUILT}"
    cp -f "${ELF_BUILT}" "${ELF_OUT}/"
    ok "-> output/elf/$(basename "${ELF_BUILT}")"
else
    warn "build reported success but ${ELF_BUILT} is absent"
fi

echo ""
ok "baseline established (compile reference only — not a deploy target)"
