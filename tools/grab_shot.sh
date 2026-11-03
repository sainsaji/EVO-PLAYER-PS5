#!/bin/bash
# tools/grab_shot.sh NAME - press L3 on the console, fetch the shot it wrote over
# FTP and save it as output/screenshots/NAME.png (1920x1080).
#
# EVO writes evo_shot_NNN.bmp to /mnt/usb0, or to /data/evoplayer when no USB
# stick is in. Both are listed before and after the press, and the file whose
# listing line changed is the new shot - FTP times are only to the minute, and
# the numbers wrap at 32, so neither "newest" nor "highest" is reliable.
# Runs on the host or inside the container.
set -u
cd "$(dirname "$0")/.."
NAME="${1:?usage: tools/grab_shot.sh NAME}"
if [ -z "${PS5_HOST:-}" ] && [ -f .env ]; then
    PS5_HOST=$(sed -n 's/^PS5_HOST=//p' .env | tr -d '\r' | head -1)
fi
HOST="${PS5_HOST:?PS5_HOST not set (.env)}"
FTP="ftp://${HOST}:${FTP_PORT:-2121}"
DIRS="/mnt/usb0 /data/evoplayer"

PY=$(command -v python3 || command -v python || true)
[ -n "$PY" ] || { echo "grab_shot: no python found" >&2; exit 1; }

list_shots() {
    for d in $DIRS; do
        curl -s --max-time 15 "$FTP$d/" | tr -d '\r' | grep -E 'evo_shot_[0-9]+\.bmp$' | sed "s#^#$d #"
    done
}

# EVO's own "screenshot: wrote <path>" line (log server, seconds resolution) is
# the best evidence; the listing diff covers a build without the log server.
last_wrote() {
    curl -s --max-time 5 "http://${HOST}:${PS5_LOG_PORT:-9780}/raw?grep=screenshot:%20wrote" |
        tr -d '\r' | grep 'screenshot: wrote' | tail -1
}

wrote_before=$(last_wrote)
before=$(list_shots)
if [ -f /.dockerenv ]; then
    timeout 20 ./tools/evo-remote.sh key l3 >/dev/null 2>&1
else
    timeout 60 docker compose run --rm ps5-dev ./tools/evo-remote.sh key l3 >/dev/null 2>&1
fi

path=""
for _ in 1 2 3 4 5 6; do
    sleep 2
    w=$(last_wrote)
    if [ -n "$w" ] && [ "$w" != "$wrote_before" ]; then
        path=$(printf '%s\n' "$w" | grep -oE '/[^ ]*evo_shot_[0-9]+\.bmp')
    else
        line=$(diff <(printf '%s\n' "$before") <(printf '%s\n' "$(list_shots)") | sed -n 's/^> //p' | tail -1)
        [ -n "$line" ] && path="${line%% *}/$(printf '%s\n' "$line" | grep -oE 'evo_shot_[0-9]+\.bmp$')"
    fi
    [ -n "$path" ] && break
done
if [ -z "$path" ]; then
    echo "grab_shot: no new evo_shot_*.bmp in $DIRS (is EVO running, and the screen not a system dialog?)" >&2
    exit 1
fi
echo "newest=$path"

mkdir -p output/screenshots
# The BMP is written in one go but large; give a slow write a moment to finish.
sleep 1
curl -s --max-time 60 -o output/screenshots/tmp.bmp "$FTP$path" || { echo "grab_shot: download failed" >&2; exit 1; }
"$PY" -c "
import sys
from PIL import Image
Image.open('output/screenshots/tmp.bmp').convert('RGB').resize((1920, 1080)).save(sys.argv[1])
" "output/screenshots/$NAME.png" && echo "saved output/screenshots/$NAME.png"
