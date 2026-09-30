#!/usr/bin/env bash
# tools/streamopts_host.sh - run the network open-options test on the host.
#
#   ./tools/streamopts_host.sh
#
# Links projects/evoplayer/media/src/evo_stream_io.c - the exact file the app
# module compiles - into tools/streamopts_host.c against the host FFmpeg that
# tools/subsync_host.sh builds (run that once first; it is not rebuilt here).
# Checks which URLs count as playlists and which FFmpeg options a network open
# gets: reconnect_at_eof for a raw stream but not for an HLS/DASH playlist, and
# bounded retries.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/streamopts_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"

FF_OUT="${REPO_ROOT}/output/host-ffmpeg"
OUT="${REPO_ROOT}/output/streamopts-host"

[[ -f "${FF_OUT}/lib/libavformat.a" ]] || {
    echo "no host FFmpeg at output/host-ffmpeg - run ./tools/subsync_host.sh once first" >&2
    exit 1
}
mkdir -p "${OUT}"

echo "--- building streamopts_host (-fsanitize=address,undefined)"
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "${OUT}/streamopts_host" \
    tools/streamopts_host.c projects/evoplayer/media/src/evo_stream_io.c \
    -Iprojects/evoplayer/media/include -Iprojects/evoplayer/include \
    -I"${FF_OUT}/include" \
    "${FF_OUT}/lib/libavformat.a" "${FF_OUT}/lib/libavcodec.a" \
    "${FF_OUT}/lib/libswresample.a" "${FF_OUT}/lib/libavutil.a" \
    -lpthread -lm

"${OUT}/streamopts_host" "$@"
