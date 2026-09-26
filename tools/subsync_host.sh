#!/usr/bin/env bash
# tools/subsync_host.sh - run the #102 subtitle auto-sync analysis on the host.
#
#   ./tools/subsync_host.sh                          # synthetic self-test
#   ./tools/subsync_host.sh <media> <srt> [shift_s [scale]]
#   ./tools/subsync_host.sh <media> --stream <n>     # an embedded text track
#
# Links projects/evoplayer/media/src/evo_subsync.c - the exact file the app
# module compiles - into tools/subsync_host.c against a host FFmpeg. The dev
# image has no FFmpeg development libraries, so the first run builds a small
# static one (same 7.1.1 source tree as the console, audio decoders + the
# common demuxers only) into output/host-ffmpeg/ and later runs reuse it.
#
# Media paths are relative to the repo root (the container sees it as
# /workspace). The synthetic film is written to output/subsync-host/.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/subsync_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"

FF_SRC="third_party/ffmpeg/ffmpeg-7.1.1"
FF_OUT="${REPO_ROOT}/output/host-ffmpeg"
OUT="${REPO_ROOT}/output/subsync-host"
mkdir -p "${OUT}"

# The matroska muxer is only for the self-test: it muxes the synthetic film
# with a SubRip track to exercise the embedded-track path.
FF_FLAGS=(
    --disable-everything --disable-programs
    --disable-doc --disable-network --disable-autodetect --disable-x86asm
    --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter
    --enable-swresample --enable-protocol=file
    --enable-demuxer=wav,matroska,mov,mpegts,avi,flac,ogg,mp3,aac,ac3,eac3,dts,truehd
    --enable-decoder=pcm_s16le,pcm_s24le,pcm_f32le,aac,ac3,eac3,dca,truehd,mlp,flac,opus,vorbis,mp3
    --enable-parser=aac,ac3,dca,mlp,flac,opus,vorbis,mpegaudio
    --enable-muxer=matroska
)

if [[ ! -f "${FF_OUT}/lib/libavformat.a" ||
      "$(cat "${FF_OUT}/.flags" 2>/dev/null)" != "${FF_FLAGS[*]}" ]]; then
    [[ -d "${FF_SRC}" ]] || tar -C third_party/ffmpeg -xf "${FF_SRC}.tar.xz"
    echo "--- building host FFmpeg (once) -> output/host-ffmpeg"
    rm -rf "${FF_OUT}" /tmp/subsync-ffb && mkdir -p /tmp/subsync-ffb && cd /tmp/subsync-ffb
    "${REPO_ROOT}/${FF_SRC}/configure" --prefix="${FF_OUT}" "${FF_FLAGS[@]}" \
        > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
    make -j"$(nproc)" > make.log 2>&1 || { tail -30 make.log; exit 1; }
    make install > /dev/null
    echo "${FF_FLAGS[*]}" > "${FF_OUT}/.flags"
    cd "${REPO_ROOT}"
fi

echo "--- building subsync_host"
gcc -O2 -Wall -o "${OUT}/subsync_host" \
    tools/subsync_host.c projects/evoplayer/media/src/evo_subsync.c \
    -Iprojects/evoplayer/media/include -Iprojects/evoplayer/include \
    -I"${FF_OUT}/include" \
    "${FF_OUT}/lib/libavformat.a" "${FF_OUT}/lib/libavcodec.a" \
    "${FF_OUT}/lib/libswresample.a" "${FF_OUT}/lib/libavutil.a" \
    -lpthread -lm

if [[ $# -eq 0 ]]; then
    rc=0
    "${OUT}/subsync_host" synth "${OUT}" || rc=$?
    rm -f "${OUT}/synth.wav" "${OUT}"/synth_emb_*.mkv
    exit $rc
fi
"${OUT}/subsync_host" "$@"
