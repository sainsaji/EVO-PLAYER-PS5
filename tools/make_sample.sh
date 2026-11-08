#!/bin/bash
# tools/make_sample.sh [OUT] - make EVO's 20 s test clip (run on the HOST: needs ffmpeg).
#
# 1080p24 H.264 + stereo AAC + one English subtitle track, about 3 MB, written
# to output/sample/evo_sample.mkv. `evo-remote.sh sample-install` copies it to
# /data/evoplayer/test/ on the console, where EVO can always read it (the app
# sandbox cannot see /data/Media), and `evo-remote.sh play --sample` plays it.
# So a playback check needs no USB stick and no SMB server.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-output/sample/evo_sample.mkv}"
command -v ffmpeg >/dev/null 2>&1 || { echo "make_sample: ffmpeg not found - run this on the host" >&2; exit 1; }
mkdir -p "$(dirname "${OUT}")"
SRT="$(dirname "${OUT}")/evo_sample.srt"
{
    printf '1\n00:00:00,500 --> 00:00:04,500\nWe need to cross the valley before the storm hits.\n\n'
    printf '2\n00:00:05,000 --> 00:00:09,000\nAre you certain the mountain pass is still open?\n\n'
    printf '3\n00:00:09,500 --> 00:00:13,500\nYes, the scouts confirmed the route this morning.\n\n'
    printf '4\n00:00:14,000 --> 00:00:18,500\nThen gather everyone and prepare to move immediately.\n\n'
} > "${SRT}"
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=20" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=20" \
    -i "${SRT}" \
    -map 0:v -map 1:a -map 2:s \
    -c:v libx264 -profile:v high -pix_fmt yuv420p -preset veryfast -b:v 1M \
    -c:a aac -ac 2 -b:a 128k -c:s srt \
    -metadata title="EVO sample" -metadata:s:a:0 language=eng -metadata:s:s:0 language=eng \
    "${OUT}"
echo "made ${OUT} ($(du -h "${OUT}" | cut -f1))"
