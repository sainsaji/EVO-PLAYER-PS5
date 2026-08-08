#!/usr/bin/env bash
# =============================================================================
# scripts/build-ffmpeg.sh - build FFmpeg for the PS5.
#
#   ./scripts/build-ffmpeg.sh --profile baseline   reproduce pacbrew's build
#   ./scripts/build-ffmpeg.sh --profile minimal    production build (default)
#   ./scripts/build-ffmpeg.sh --profile full       compatibility research
#   ./scripts/build-ffmpeg.sh --install            install into the SDK sysroot
#   ./scripts/build-ffmpeg.sh --version 7.1.1      build a different release
#
# ---------------------------------------------------------------------------
# WHY 7.0.1 IS THE DEFAULT
#   ProsperoPlayer's Makefile does not pin an FFmpeg version - it links
#   prebuilt static libraries out of the SDK sysroot:
#       $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libav*.a
#   Those come from ps5-payload-dev/pacbrew-repo, whose ffmpeg/PKGBUILD says
#   pkgver=7.0.1. So 7.0.1 IS the version ProsperoPlayer currently uses, and
#   it is what EVO Player must reproduce before changing anything.
#
# THE THREE PROFILES
#   baseline  Byte-for-byte the configure line from pacbrew's PKGBUILD. Use it
#             to prove the toolchain can reproduce the environment that the
#             existing player already builds against. It intentionally still
#             builds ffmpeg/ffplay/ffprobe, exactly as upstream does.
#
#   minimal   What ships in the PKG. --disable-everything, then only the
#             codecs, demuxers and parsers the player actually needs. Smaller
#             binary, faster link, and a much shorter list of things that can
#             be wrong.
#
#   full      Everything FFmpeg can decode. Not for shipping - this is the
#             build you use to answer "does this file fail because the codec
#             is missing, or because our integration is broken?". Diffing a
#             failure between minimal and full localises the problem instantly.
# ---------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

FFMPEG_VERSION="7.0.1"
# Published by pacbrew-repo/ffmpeg/PKGBUILD for ffmpeg-7.0.1.tar.xz.
FFMPEG_SHA256="bce9eeb0f17ef8982390b1f37711a61b4290dc8c2a0c1a37b5857e85bfb0e4ff"
PROFILE="minimal"
DO_INSTALL=0
JOBS="$(nproc)"

while (( $# )); do
    case "$1" in
        --profile) shift; PROFILE="${1:?--profile needs a value}" ;;
        --version) shift; FFMPEG_VERSION="${1:?--version needs a value}"
                   FFMPEG_SHA256=""   # unknown for a custom version
                   ;;
        --install) DO_INSTALL=1 ;;
        --jobs|-j) shift; JOBS="${1:?--jobs needs a value}" ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

case "${PROFILE}" in
    baseline|minimal|full) ;;
    *) die "unknown profile '${PROFILE}'. Use: baseline | minimal | full" ;;
esac

if ! in_container; then
    FWD=(--profile "${PROFILE}" --version "${FFMPEG_VERSION}")
    (( DO_INSTALL )) && FWD+=(--install)
    reexec_in_container "build-ffmpeg.sh" "${FWD[@]}"
fi

load_sdk
need_cmd wget tar make nasm yasm pkg-config

SRC_ROOT="${REPO_ROOT}/third_party/ffmpeg"
TARBALL="${SRC_ROOT}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
SRC_DIR="${SRC_ROOT}/ffmpeg-${FFMPEG_VERSION}"
# Object trees live on a Linux volume, not the Windows bind mount - FFmpeg
# creates thousands of small files and this is 5-10x faster.
BUILD_DIR="/build/ffmpeg/${FFMPEG_VERSION}-${PROFILE}"
# Staged install so a broken build never corrupts the working sysroot.
STAGE_DIR="/build/ffmpeg/stage-${FFMPEG_VERSION}-${PROFILE}"

mkdir -p "${SRC_ROOT}" "${BUILD_DIR}" "${STAGE_DIR}"
mkdirs

log "FFmpeg ${FFMPEG_VERSION}, profile '${PROFILE}'"
echo "  source : ${SRC_DIR}"
echo "  build  : ${BUILD_DIR}"
echo "  jobs   : ${JOBS}"

# -----------------------------------------------------------------------------
begin "fetching source"
if [[ ! -f "${TARBALL}" ]]; then
    wget -q --show-progress -O "${TARBALL}.part" \
        "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
      || die "download failed for FFmpeg ${FFMPEG_VERSION}"
    mv "${TARBALL}.part" "${TARBALL}"
else
    ok "tarball already present"
fi

if [[ -n "${FFMPEG_SHA256}" ]]; then
    echo "${FFMPEG_SHA256}  ${TARBALL}" | sha256sum -c - >/dev/null \
      || die "checksum mismatch on ${TARBALL}.
       Delete it and re-run to re-download."
    ok "sha256 verified"
else
    warn "no known checksum for version ${FFMPEG_VERSION} - skipping verification"
fi

if [[ ! -d "${SRC_DIR}" ]]; then
    begin "extracting"
    tar -xf "${TARBALL}" -C "${SRC_ROOT}" || die "extract failed"
fi
need_file "${SRC_DIR}/configure" "FFmpeg source tree looks wrong."

# =============================================================================
# Configure flags.
#
# Cross-compilation settings common to every profile. These mirror what
# pacbrew's PKGBUILD does, because that is the configuration known to produce
# working PS5 libraries:
#   --target-os=freebsd   the PS5 kernel is a modified FreeBSD
#   --arch=x86_64         Zen 2 CPU
#   --cross-prefix        points configure at the prospero-* wrappers
#   --enable-static --disable-shared
#                         payloads link statically; there is no loader story
#                         for shared libav* on the console
# =============================================================================
COMMON_FLAGS=(
    --prefix="${PREFIX}"
    --enable-cross-compile
    --cross-prefix="${PS5_PAYLOAD_SDK}/bin/prospero-"
    --enable-static --disable-shared
    --arch=x86_64
    --target-os=freebsd
    --cc="${CC}" --cxx="${CXX}" --nm="${NM}" --strip="${STRIP}"
    --ar="${AR}" --ranlib="${RANLIB}" --pkg-config="${PKG_CONFIG}"
    --disable-debug
    --disable-doc
)

case "${PROFILE}" in
# -----------------------------------------------------------------------------
baseline)
    # Exactly pacbrew-repo/ffmpeg/PKGBUILD. Depends on openssl/freetype/
    # fribidi/harfbuzz/libass already being in the sysroot - they are, because
    # the image installs the full pacbrew tarball.
    CONFIGURE_FLAGS=(
        "${COMMON_FLAGS[@]}"
        --enable-openssl --enable-version3
        --enable-libfreetype --enable-libfribidi --enable-libharfbuzz
        --enable-libass
    )
    ;;

# -----------------------------------------------------------------------------
minimal)
    # Start from nothing and add back only what the player needs. This is the
    # production build.
    CONFIGURE_FLAGS=(
        "${COMMON_FLAGS[@]}"
        --disable-everything

        # -- components we do not ship ------------------------------------
        # The brief asks for these off explicitly. The CLI tools alone are
        # several MB and are useless inside a payload.
        --disable-programs          # ffmpeg, ffplay, ffprobe
        --disable-avdevice          # no capture devices on a console
        --disable-postproc          # legacy libpostproc, unused
        --disable-encoders          # playback only
        --disable-muxers            # playback only
        --disable-bsfs              # re-enabled selectively below
        --disable-devices
        --disable-filters           # re-enabled selectively below
        # Network protocols off: EVO Player plays local files from USB.
        # Turn this back on (and add --enable-protocol=http,tcp) if and when
        # streaming or DLNA playback is actually implemented.
        --disable-network
        --disable-iconv
        --disable-xlib
        --disable-sdl2              # the player owns SDL, FFmpeg must not

        # -- libraries we keep --------------------------------------------
        # swresample is NOT optional: AudioOut is hard-wired to 48 kHz stereo,
        # so anything decoded at 44.1 kHz or with a different channel layout
        # has to be resampled before it can be played.
        --enable-swresample
        --enable-swscale

        # -- audio decoders (brief section 13) -----------------------------
        --enable-decoder=aac
        --enable-decoder=aac_latm       # AAC inside MPEG-TS
        --enable-decoder=ac3
        --enable-decoder=eac3           # the "silent E-AC3" bug's home
        --enable-decoder=dca            # DTS
        --enable-decoder=mp3
        --enable-decoder=mp2
        --enable-decoder=flac
        --enable-decoder=opus
        --enable-decoder=vorbis
        --enable-decoder=alac
        --enable-decoder=pcm_s16le
        --enable-decoder=pcm_s16be
        --enable-decoder=pcm_s24le
        --enable-decoder=pcm_f32le

        # -- video decoders -------------------------------------------------
        --enable-decoder=h264
        --enable-decoder=hevc           # includes Main10 / 10-bit
        --enable-decoder=vp9
        --enable-decoder=vp8
        --enable-decoder=mpeg2video
        --enable-decoder=mpeg4
        --enable-decoder=av1            # native decoder; slow, but present

        # -- subtitles ------------------------------------------------------
        --enable-decoder=subrip
        --enable-decoder=ass
        --enable-decoder=srt
        --enable-decoder=movtext
        # Blu-ray bitmap subtitles. The component is named 'pgssub', not
        # 'hdmv_pgs_subtitle' (that is the AVCodecID); verified against
        # libavcodec/Makefile in 7.0.1.
        --enable-decoder=pgssub
        --enable-decoder=dvdsub
        --enable-decoder=dvbsub

        # -- still images: cover art and thumbnail scrubbing ---------------
        --enable-decoder=mjpeg
        --enable-decoder=png

        # -- containers (brief section 13) -----------------------------------
        --enable-demuxer=matroska       # MKV and WebM share this demuxer
        --enable-demuxer=mov            # MP4, MOV, M4A, 3GP
        --enable-demuxer=mpegts
        --enable-demuxer=mpegps
        --enable-demuxer=avi
        --enable-demuxer=flac
        --enable-demuxer=mp3
        --enable-demuxer=ogg
        --enable-demuxer=wav
        --enable-demuxer=aac
        --enable-demuxer=ac3
        --enable-demuxer=eac3
        --enable-demuxer=dts
        --enable-demuxer=h264
        --enable-demuxer=hevc
        --enable-demuxer=srt
        --enable-demuxer=ass
        --enable-demuxer=image2         # the thumbnail/cover-art path

        # -- parsers ----------------------------------------------------------
        # Without these, raw and MPEG-TS streams will not frame correctly -
        # a classic cause of "plays in VLC, fails on console".
        --enable-parser=h264
        --enable-parser=hevc
        --enable-parser=vp9
        --enable-parser=av1
        --enable-parser=aac
        --enable-parser=aac_latm
        --enable-parser=ac3
        --enable-parser=dca
        --enable-parser=flac
        --enable-parser=opus
        --enable-parser=vorbis
        --enable-parser=mpegaudio
        --enable-parser=mpegvideo
        --enable-parser=mpeg4video

        # -- bitstream filters -------------------------------------------------
        # Required to feed MP4/MKV-contained H.264/HEVC to a decoder that
        # expects Annex-B - and mandatory for any future hardware decoder.
        --enable-bsf=h264_mp4toannexb
        --enable-bsf=hevc_mp4toannexb
        --enable-bsf=extract_extradata
        --enable-bsf=aac_adtstoasc
        --enable-bsf=vp9_superframe

        # -- filters ------------------------------------------------------------
        # Only the graph plumbing plus format conversion; no effects.
        --enable-filter=aformat
        --enable-filter=aresample
        --enable-filter=anull
        --enable-filter=format
        --enable-filter=scale
        --enable-filter=null

        # -- protocols ------------------------------------------------------------
        --enable-protocol=file
        --enable-protocol=pipe
    )
    # NOTE: there is no '--enable-image2' option. image2 is a DEMUXER and is
    # enabled above via --enable-demuxer=image2; passing it as a bare option
    # makes configure abort. Component names are version specific - always
    # check `configure --help` / libav*/Makefile rather than guessing.
    ;;

# -----------------------------------------------------------------------------
full)
    # Everything the source tree can decode. Deliberately does NOT use
    # --enable-everything (which would also pull in encoders, muxers and
    # every external library we have not built). Instead: default component
    # set, all decoders/demuxers/parsers on, no encoders or muxers.
    CONFIGURE_FLAGS=(
        "${COMMON_FLAGS[@]}"
        --disable-programs
        --disable-avdevice
        --disable-encoders
        --disable-muxers
        --disable-devices
        --disable-network
        --disable-sdl2
        --enable-swresample
        --enable-swscale
        --enable-decoders
        --enable-demuxers
        --enable-parsers
        --enable-bsfs
        --enable-filters
        --enable-protocol=file
        --enable-protocol=pipe
        # Extra external decoders that ARE in the pacbrew sysroot. These give
        # better quality/speed than the native decoders and are worth having
        # in the research build to compare against.
        --enable-libvpx
        --enable-libopus
        --enable-libvorbis
        --enable-libmp3lame
        --enable-version3               # required by some GPL/v3 components
    )
    ;;
esac

# -----------------------------------------------------------------------------
begin "configure (${PROFILE})"
# FFmpeg's configure insists on running from the build directory for an
# out-of-tree build.
cd "${BUILD_DIR}" || die "cannot enter build directory ${BUILD_DIR}"

CONFIG_LOG="${LOG_OUT}/ffmpeg-${FFMPEG_VERSION}-${PROFILE}-configure.log"

if ! "${SRC_DIR}/configure" "${CONFIGURE_FLAGS[@]}" > "${CONFIG_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines of configure output ---"
    tail -40 "${CONFIG_LOG}" | sed 's/^/  /'
    echo "--- and from config.log ---"
    tail -30 "${BUILD_DIR}/ffbuild/config.log" 2>/dev/null | sed 's/^/  /'
    die "FFmpeg configure failed (profile: ${PROFILE}).
       Full log: ${CONFIG_LOG#"${REPO_ROOT}/"}
       Common causes:
         * a --enable-lib<x> whose library is not in the sysroot
         * nasm/yasm missing (they are in the image; check \$PATH)
         * an option name that changed between FFmpeg releases - option names
           are version specific, check ${SRC_DIR}/configure --help"
fi
ok "configured"
grep -E '^(External libraries|Enabled decoders)' -A2 "${CONFIG_LOG}" 2>/dev/null | head -6 || true

# -----------------------------------------------------------------------------
begin "compiling (-j${JOBS})"
BUILD_LOG="${LOG_OUT}/ffmpeg-${FFMPEG_VERSION}-${PROFILE}-build.log"
if ! make -j"${JOBS}" > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines ---"
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "FFmpeg build failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi
ok "compiled"

# -----------------------------------------------------------------------------
begin "staging install"
make install DESTDIR="${STAGE_DIR}" >> "${BUILD_LOG}" 2>&1 \
  || die "'make install' into the staging directory failed."

STAGED_LIB="${STAGE_DIR}${PREFIX}/lib"
for l in libavcodec.a libavformat.a libavutil.a libswresample.a libswscale.a; do
    need_file "${STAGED_LIB}/${l}" "FFmpeg did not produce ${l}."
    printf '  %-18s %8s KiB\n' "${l}" "$(( $(stat -c %s "${STAGED_LIB}/${l}") / 1024 ))"
done

# -----------------------------------------------------------------------------
# Record what this build can actually decode. This artifact is the whole point
# of keeping a minimal and a full profile side by side: when a file will not
# play, diff the two lists.
# -----------------------------------------------------------------------------
begin "recording codec inventory"
INVENTORY="${LOG_OUT}/ffmpeg-${FFMPEG_VERSION}-${PROFILE}-codecs.txt"

# FFmpeg 6.0 split the per-component CONFIG_* defines out of config.h into
# config_components.h. Search both so this works across versions, and tolerate
# a component class being entirely absent (grep exits 1 on no match, which
# under `set -o pipefail` would otherwise abort the whole script).
FF_CONFIG_HEADERS=()
for h in "${BUILD_DIR}/config_components.h" "${BUILD_DIR}/config.h"; do
    [[ -f "${h}" ]] && FF_CONFIG_HEADERS+=("${h}")
done

list_components() {
    local kind="$1"
    grep -hoE "^#define CONFIG_[A-Z0-9_]+_${kind} 1" "${FF_CONFIG_HEADERS[@]}" 2>/dev/null \
      | sed "s/#define CONFIG_//; s/_${kind} 1//" \
      | tr 'A-Z' 'a-z' | sort -u || true
}

if (( ${#FF_CONFIG_HEADERS[@]} == 0 )); then
    warn "no config header found in ${BUILD_DIR} - skipping codec inventory"
else
    {
        echo "# FFmpeg ${FFMPEG_VERSION} - profile ${PROFILE}"
        echo "# generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# source: ${FF_CONFIG_HEADERS[*]}"
        for kind in DECODER DEMUXER PARSER BSF PROTOCOL; do
            echo ""
            echo "## $(echo "${kind}" | tr 'A-Z' 'a-z')s"
            list_components "${kind}"
        done
    } > "${INVENTORY}"
    ok "$(grep -c '^[a-z0-9]' "${INVENTORY}" || true) components -> ${INVENTORY#"${REPO_ROOT}/"}"

    # A quick, readable confirmation that the codecs the brief cares about are
    # actually in this build.
    echo "  key codecs:"
    for c in aac ac3 eac3 dca mp3 flac opus vorbis alac h264 hevc vp9 mpeg2video av1; do
        if grep -qx "${c}" "${INVENTORY}"; then
            printf '    [x] %s\n' "${c}"
        else
            printf '    [ ] %s  MISSING\n' "${c}"
        fi
    done
fi

# -----------------------------------------------------------------------------
if (( DO_INSTALL )); then
    begin "installing into the SDK sysroot"
    # PS5_CROSS_FIX_ROOT rewrites absolute paths inside .pc and libtool files
    # so pkg-config resolves them relative to the sysroot rather than to the
    # on-console /user/homebrew prefix. pacbrew's PKGBUILD does the same.
    cp -a "${STAGE_DIR}${PREFIX}/." "${PS5_SYSROOT}${PREFIX}/"
    if [[ -x "${PS5_CROSS_FIX_ROOT:-}" ]]; then
        "${PS5_CROSS_FIX_ROOT}" "${PS5_SYSROOT}${PREFIX}"
    fi
    ok "installed to ${PS5_SYSROOT}${PREFIX}"
    warn "this REPLACED the pacbrew FFmpeg ${PROFILE} build in the sysroot."
    warn "to get the pristine one back: docker volume rm evoplayer_ps5_sdk"
else
    echo ""
    log "staged, not installed"
    echo "   libraries: ${STAGED_LIB}"
    echo "   install into the sysroot with:"
    echo "     ./scripts/build-ffmpeg.sh --profile ${PROFILE} --install"
fi

echo ""
ok "FFmpeg ${FFMPEG_VERSION} (${PROFILE}) done"
