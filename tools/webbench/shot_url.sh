#!/bin/bash
# usage: shot_url.sh <url> <wait-secs> <out.png>
# Loads the URL in the open Web section, waits, grabs one screenshot.
cd "$(dirname "$0")/../.."
./tools/dx.sh ./tools/evo-remote.sh weburl "$1" >/dev/null 2>&1
sleep "${2:-30}"
./tools/dx.sh ./tools/evo-remote.sh key l3 >/dev/null 2>&1
./tools/shot.sh grab >/dev/null 2>&1
mkdir -p output/anim
cp output/screenshots/latest.png "${3:-output/anim/shot.png}"
echo "saved ${3:-output/anim/shot.png}"
