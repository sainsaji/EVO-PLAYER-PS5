#!/bin/bash
# usage: tools/webbench/loadtime.sh <url> [tag]
# Page-load check on the real Web section: opens <url>, then screenshots every
# 5 s for 40 s and prints how much of the page has been drawn (share of
# non-background pixels) at each time, plus the WebProcess IPC stalls from the
# log. Compare a run with and without `skia` in /data/prosperoweb/mesa.
cd "$(dirname "$0")/../.."
H=ftp://192.168.0.12:2121
URL=${1:-https://prosperostore.net}; TAG=${2:-run}
R=./tools/dx.sh
mkdir -p output/anim
$R ./tools/evo-remote.sh weburl "about:blank" >/dev/null 2>&1; sleep 3
$R ./tools/evo-remote.sh weburl "$URL" >/dev/null 2>&1
t0=$(date +%s)
for i in 1 2 3 4 5 6 7 8; do
  sleep 4
  $R ./tools/evo-remote.sh key l3 >/dev/null 2>&1; ./tools/shot.sh grab >/dev/null 2>&1
  cp output/screenshots/latest.png output/anim/load_${TAG}_$i.png
  echo "$(( $(date +%s) - t0 )) s" > output/anim/load_${TAG}_$i.txt
done
python3 -W ignore -I -c "
from PIL import Image
import statistics
for i in range(1,9):
    im=Image.open('output/anim/load_${TAG}_%d.png' % i).convert('RGB')
    t=open('output/anim/load_${TAG}_%d.txt' % i).read().strip()
    box=(130,230,1900,1010)
    px=list(im.crop(box).getdata())
    spread=statistics.pstdev([sum(p)/3 for p in px])
    print(t, 'page contrast', round(spread,1))
"
echo "== WebProcess IPC stalls since the load"
curl -s --max-time 90 -r -120000- $H/data/prosperoweb/log.txt | grep -a "WebProcess.*ipc dispatch 5s" | tail -8 | sed 's/.*ipc dispatch 5s: //' | cut -c1-120
