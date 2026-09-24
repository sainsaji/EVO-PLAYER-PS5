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

# -----------------------------------------------------------------------------
# Audio: every codec FFmpeg 7.0.1 decodes natively.
#
# The old list was assembled from whatever the test corpus happened to hold,
# and every gap in it reached a user the same way - a file that plays with no
# sound, or does not play at all, and no reason offered anywhere. Dolby TrueHD
# was the third time. Curating this list is not a feature; it is a standing
# promise to rediscover the same bug once per format somebody owns.
#
# So: all of them. A decoder is a few KB of .text that allocates nothing until
# a stream uses it, none of these need an external library, and none carry a
# gpl/version3/nonfree dependency - checked against configure, not assumed.
# The one name in allcodecs.c's audio block that is NOT here is `hdr`: the
# Radiance RGBE *image* decoder, sitting alphabetically between hcom and iac.
#
# Video is deliberately left alone. It is gated by hardware decode and by the
# flexible-memory ceiling that makes a 4K software decode fault rather than
# fail, so it is a different decision with different risks. Audio has neither
# constraint: frames are kilobytes.
#
# Regenerate with:
#   sed -n '/audio codecs/,/subtitles/p' libavcodec/allcodecs.c | grep -oE 'ff_[a-z0-9_]+_decoder'
# then drop any whose .p.type is not AVMEDIA_TYPE_AUDIO (the pcm/adpcm/dpcm
# families define theirs through a macro, and are all audio).
# -----------------------------------------------------------------------------
AUDIO_DECODERS=(
    # -- named codecs: lossy, lossless, speech, game and console formats --
    aac_fixed ac3_fixed acelp_kelvin als amrnb amrwb apac ape aptx
    aptx_hd atrac1 atrac3 atrac3al atrac3p atrac3pal atrac9
    binkaudio_dct binkaudio_rdft bmv_audio bonk cook dfpwm dolby_e
    dsd_lsbf dsd_lsbf_planar dsd_msbf dsd_msbf_planar dsicinaudio dss_sp
    dst evrc fastaudio ffwavesynth ftr g723_1 g729 gsm gsm_ms hca hcom
    iac ilbc imc interplay_acm mace3 mace6 metasound misc4 mp1 mp1float
    mp2float mp3adu mp3adufloat mp3float mp3on4 mp3on4float mpc7 mpc8
    msnsiren nellymoser on2avc osq paf_audio qcelp qdm2 qdmc qoa ra_144
    ra_288 ralf sbc shorten sipr siren smackaud sonic tak truespeech tta
    twinvq vmdaudio wavarc wavpack wmalossless wmapro wmav1 wmav2
    wmavoice ws_snd1 xma1 xma2

    # -- raw PCM: every width, sign, endianness and planar layout ---------
    pcm_alaw pcm_bluray pcm_dvd pcm_f16le pcm_f24le pcm_f32be pcm_f64be
    pcm_f64le pcm_lxf pcm_mulaw pcm_s16be_planar pcm_s16le_planar
    pcm_s24be pcm_s24daud pcm_s24le_planar pcm_s32be pcm_s32le
    pcm_s32le_planar pcm_s64be pcm_s64le pcm_s8 pcm_s8_planar pcm_sga
    pcm_u16be pcm_u16le pcm_u24be pcm_u24le pcm_u32be pcm_u32le pcm_u8
    pcm_vidc

    # -- ADPCM: the container-specific variants AVI/WAV/MOV use, plus the
    #    game formats -----------------------------------------------------
    adpcm_4xm adpcm_adx adpcm_afc adpcm_agm adpcm_aica adpcm_argo
    adpcm_ct adpcm_dtk adpcm_ea adpcm_ea_maxis_xa adpcm_ea_r1
    adpcm_ea_r2 adpcm_ea_r3 adpcm_ea_xas adpcm_g722 adpcm_g726
    adpcm_g726le adpcm_ima_acorn adpcm_ima_alp adpcm_ima_amv
    adpcm_ima_apc adpcm_ima_apm adpcm_ima_cunning adpcm_ima_dat4
    adpcm_ima_dk3 adpcm_ima_dk4 adpcm_ima_ea_eacs adpcm_ima_ea_sead
    adpcm_ima_iss adpcm_ima_moflex adpcm_ima_mtf adpcm_ima_oki
    adpcm_ima_qt adpcm_ima_rad adpcm_ima_smjpeg adpcm_ima_ssi
    adpcm_ima_wav adpcm_ima_ws adpcm_ms adpcm_mtaf adpcm_psx
    adpcm_sbpro_2 adpcm_sbpro_3 adpcm_sbpro_4 adpcm_swf adpcm_thp
    adpcm_thp_le adpcm_vima adpcm_xa adpcm_xmd adpcm_yamaha adpcm_zork

    # -- DPCM --------------------------------------------------------------
    cbd2_dpcm derf_dpcm gremlin_dpcm interplay_dpcm roq_dpcm sdx2_dpcm
    sol_dpcm wady_dpcm xan_dpcm
)

# Containers and raw streams for the above. Without these an .ape or .wv is
# not "unsupported codec" - it does not open at all. asf is here for .wma;
# .wmv still needs the video half, which this pass does not add.
AUDIO_DEMUXERS=(
    adx aiff amr amrnb amrwb apac ape aptx aptx_hd argo_asf asf ast au
    bfstm bonk brstm caf codec2 dfpwm dsf dtshd g723_1 g729 gsm hca hcom
    iff ilbc ircam mlp mpc mpc8 nistsphere oma osq pcm_alaw pcm_f32be
    pcm_f32le pcm_f64be pcm_f64le pcm_mulaw pcm_s16be pcm_s16le
    pcm_s24be pcm_s24le pcm_s32be pcm_s32le pcm_s8 pcm_u16be pcm_u16le
    pcm_u24be pcm_u24le pcm_u32be pcm_u32le pcm_u8 pcm_vidc pvf qoa rso
    rm sbc shorten sln sox spdif tak tta voc vqf w64 wavarc wv wve xa
    xwma
)

# Framing. Without a parser, a stream in a container that does not carry frame
# boundaries decodes to noise, or to nothing.
AUDIO_PARSERS=(
    adx amr cook dolby_e dvaudio ftr g723_1 g729 gsm misc4 sbc sipr tak
    xma
)

# -----------------------------------------------------------------------------
# Video and subtitle components, approved 2026-09-24.
#
# Same reasoning as AUDIO_DECODERS: the old list was what the test corpus
# happened to hold. Two of these were worse than missing - the file browser
# already advertises .wmv and .flv as playable (FileSystemBrowser.cpp), so those
# files listed, got a poster attempt, and could not even be demuxed because
# neither ASF nor FLV was built. evo_subtitle.c likewise already handles
# AV_CODEC_ID_WEBVTT while no webvtt decoder existed.
#
# All native, no external libraries, no gpl/version3 dependency - each name
# checked against allcodecs.c / allformats.c / parsers.c rather than assumed.
#
# NOT here, deliberately: FFmpeg's own `av1` decoder. It is a hwaccel-only
# wrapper with no software path and dereferences null on a machine with no AV1
# hardware, which is every PS5. AV1 resolves to libdav1d only - see the note by
# --enable-libdav1d above.
# -----------------------------------------------------------------------------
VIDEO_DECODERS=(
    # WMV / VC-1 family (.wmv, .asf), MS-MPEG4 (old DivX in .avi)
    vc1 wmv1 wmv2 wmv3 msmpeg4v1 msmpeg4v2 msmpeg4v3 mpeg1video
    # FLV / Sorenson / VP6, H.263, and the old AVI + QuickTime codecs
    flv vp6 vp6a vp6f h263 h263i h263p msvideo1 cinepak indeo3 indeo4
    indeo5 huffyuv ffv1 utvideo qtrle rpza smc svq1 svq3 mjpegb theora
    # Theora, pro/intermediate formats, Chinese AVS, RealVideo, WebP stills
    prores dnxhd dvvideo cavs rv10 rv20 rv30 rv40 webp
)

# Text and bitmap subtitle formats. The container side is in VIDEO_DEMUXERS -
# an external .sub or .smi needs a demuxer as well as a decoder.
SUBTITLE_DECODERS=(
    webvtt text microdvd sami subviewer subviewer1 mpl2 vplayer pjs
    jacosub realtext stl xsub ccaption
)

VIDEO_DEMUXERS=(
    asf asf_o flv live_flv rm dv mxf webvtt microdvd sami subviewer
    subviewer1 mpl2 vplayer pjs jacosub realtext stl vc1 vc1t m4v ivf
    mpegvideo
)

VIDEO_PARSERS=(
    vc1 h263 webp
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
        # -- network ------------------------------------------------------
        # On since #90. Provider playback (IPTV, Emby, debrid) resolves to an
        # http(s) URL and hands it to avformat_open_input, so without this the
        # open returns AVERROR_PROTOCOL_NOT_FOUND and avformat_network_init()
        # is a no-op - which it was, every boot, for the whole 0.6-0.10 era.
        #
        # openssl rather than the native TLS: the sysroot's libssl/libcrypto
        # are already linked into the app module (projects/evoplayer/Makefile
        # -lssl -lcrypto, for addons/src/evo_net.c), the `baseline` profile has
        # proven --enable-openssl against this exact sysroot, and gnutls is not
        # in the pacbrew tarball at all.
        --enable-network
        --enable-openssl
        --disable-iconv
        --disable-xlib
        --disable-sdl2              # the player owns SDL, FFmpeg must not

        # -- libraries we keep --------------------------------------------
        # swresample is NOT optional: AudioOut is hard-wired to 48 kHz stereo,
        # so anything decoded at 44.1 kHz or with a different channel layout
        # has to be resampled before it can be played.
        --enable-swresample
        --enable-swscale

        # -- audio decoders -------------------------------------------------
        # The mainstream set stays spelled out: these are what the player is
        # tuned around, and what a silent build break would cost the most.
        --enable-decoder=aac
        --enable-decoder=aac_latm       # AAC inside MPEG-TS
        --enable-decoder=ac3
        --enable-decoder=eac3           # the "silent E-AC3" bug's home
        --enable-decoder=dca            # DTS, incl. DTS-HD MA lossless
        --enable-decoder=truehd         # Dolby TrueHD, and the MLP under it
        --enable-decoder=mlp
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

        # ...and then everything else FFmpeg decodes natively. See
        # AUDIO_DECODERS at the top for why this is not a curated list.
        "${AUDIO_DECODERS[@]/#/--enable-decoder=}"
        "${VIDEO_DECODERS[@]/#/--enable-decoder=}"
        "${SUBTITLE_DECODERS[@]/#/--enable-decoder=}"

        # -- video decoders -------------------------------------------------
        --enable-decoder=h264
        --enable-decoder=hevc           # includes Main10 / 10-bit
        --enable-decoder=vp9
        --enable-decoder=vp8
        --enable-decoder=mpeg2video
        --enable-decoder=mpeg4
        # AV1 = libdav1d ONLY. FFmpeg's own `av1` decoder (av1dec.c) is a
        # hwaccel-only wrapper with no software path: on a machine with no
        # AV1 hardware - every PS5 - it logs "Your platform doesn't
        # suppport hardware accelerated AV1 decoding" at the first frame and
        # then dereferences null inside libavcodec, taking the app with it.
        # It was enabled here for months behind the comment "native decoder;
        # slow, but present", which it is not. Deliberately NOT re-enabled:
        # leaving it out means AV1 can only ever resolve to libdav1d.
        --enable-libdav1d
        --enable-decoder=libdav1d

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
        --enable-demuxer=truehd         # raw .thd/.ac3 streams
        --enable-demuxer=h264
        --enable-demuxer=hevc
        --enable-demuxer=srt
        --enable-demuxer=ass
        --enable-demuxer=image2         # the thumbnail/cover-art path

        # -- streaming containers (#90) -------------------------------------
        # hls is what an IPTV .m3u8 actually is. It has no external dependency
        # and demuxes its segments through mpegts and mov, both already on.
        #
        # dash needs libxml2, which in turn has unresolved iconv_* symbols;
        # both libxml2.a and libiconv.a are in the pacbrew prefix and link,
        # so -lxml2 joins the transitive list in build-evoplayer.sh,
        # package-app.sh and the Makefile alongside the -liconv already there.
        --enable-demuxer=hls
        --enable-demuxer=dash
        --enable-libxml2
        "${AUDIO_DEMUXERS[@]/#/--enable-demuxer=}"
        "${VIDEO_DEMUXERS[@]/#/--enable-demuxer=}"

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
        --enable-parser=mlp             # TrueHD/MLP framing in MKV and .thd
        --enable-parser=flac
        --enable-parser=opus
        --enable-parser=vorbis
        --enable-parser=mpegaudio
        --enable-parser=mpegvideo
        --enable-parser=mpeg4video
        "${AUDIO_PARSERS[@]/#/--enable-parser=}"
        "${VIDEO_PARSERS[@]/#/--enable-parser=}"

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
        # tcp and tls are not optional extras: http is built on tcp, and https
        # is http over tls. Enabling only http,https silently produces a build
        # where every network open fails at the transport layer.
        --enable-protocol=file
        --enable-protocol=pipe
        --enable-protocol=http
        --enable-protocol=https
        --enable-protocol=tcp
        --enable-protocol=tls
        # AES-128 segment encryption is routine on IPTV playlists; without
        # the crypto protocol those channels open and then decode to noise.
        --enable-protocol=crypto
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
        --enable-network
        --enable-openssl
        --disable-sdl2
        --enable-swresample
        --enable-swscale
        --enable-decoders
        --enable-demuxers
        --enable-parsers
        --enable-bsfs
        --enable-filters
        --enable-protocols
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
    # libdav1d, not av1: a decoder named `av1` exists even when it is the
    # useless hwaccel-only stub, so checking that name proved nothing.
    for c in aac ac3 eac3 dca mp3 flac opus vorbis alac h264 hevc vp9 mpeg2video libdav1d; do
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
