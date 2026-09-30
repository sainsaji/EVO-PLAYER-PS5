#!/usr/bin/env bash
# tools/dualsub_host.sh - run the #110 dual-subtitle engine test on the host.
#
#   ./tools/dualsub_host.sh              # address + undefined-behaviour sanitizers
#   SAN=thread ./tools/dualsub_host.sh   # the race detector instead
#   ./tools/dualsub_host.sh -v           # print the engine's toasts and log
#
# SAN=thread needs the kernel's vm.mmap_rnd_bits at 28 or lower. On a stock
# Docker Desktop VM it is 32 and the binary dies with a SEGV before main(); the
# container cannot change it (setarch -R is refused), so lower it on the VM
# (`sysctl -w vm.mmap_rnd_bits=28`) or run this script on a Linux host.
#
# Links projects/evoplayer/media/src/evo_subtitle.c - the exact file the app
# module compiles - into tools/dualsub_host.c against the host FFmpeg that
# tools/subsync_host.sh builds. Run that once first if output/host-ffmpeg is
# missing; it is not rebuilt here.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm -e SAN ps5-dev bash ./tools/dualsub_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"

FF_OUT="${REPO_ROOT}/output/host-ffmpeg"
OUT="${REPO_ROOT}/output/dualsub-host"
SAN="${SAN:-address,undefined}"

[[ -f "${FF_OUT}/lib/libavformat.a" ]] || {
    echo "no host FFmpeg at output/host-ffmpeg - run ./tools/subsync_host.sh once first" >&2
    exit 1
}
mkdir -p "${OUT}"

echo "--- building dualsub_host (-fsanitize=${SAN})"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize="${SAN}" -fno-omit-frame-pointer \
    -o "${OUT}/dualsub_host" \
    tools/dualsub_host.c \
    projects/evoplayer/media/src/evo_subtitle.c \
    projects/evoplayer/media/src/evo_subsync.c \
    -Iprojects/evoplayer/media/include -Iprojects/evoplayer/include \
    -I"${FF_OUT}/include" \
    "${FF_OUT}/lib/libavformat.a" "${FF_OUT}/lib/libavcodec.a" \
    "${FF_OUT}/lib/libswresample.a" "${FF_OUT}/lib/libavutil.a" \
    -lpthread -lm

"${OUT}/dualsub_host" "${OUT}" "$@"
