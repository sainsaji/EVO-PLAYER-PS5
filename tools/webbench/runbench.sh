#!/bin/bash
# Serve third_party/ProsperoWeb/tools/bench on the PC first: (cd third_party/ProsperoWeb/tools/bench && python3 -m http.server 8123 --bind 0.0.0.0). The PC address (192.168.0.13) is hard-coded below.
# usage: tools/webbench/runbench.sh <article|cards> [speed] [secs]
# Opens the offline bench page in the Web section, runs the host's scroll
# benchmark and prints the numbers: WebProcess/host CPU for the exact window,
# surface stats, EVO frame counts, and a screenshot with the page's own report.
cd "$(dirname "$0")/../.."
H=ftp://192.168.0.12:2121
KIND=${1:-article}; SPEED=${2:-800}; SECS=${3:-30}
R=./tools/dx.sh
$R ./tools/evo-remote.sh weburl "http://192.168.0.13:8123/$KIND.html" >/dev/null 2>&1
sleep 2
[[ "${NAV:-0}" == 1 ]] && $R ./tools/evo-remote.sh key left down down down cross >/dev/null 2>&1
sleep 12
startm=$(curl -s --max-time 30 $H/data/prosperoweb/mesa.txt | wc -l)
printf "speed=$SPEED secs=$SECS warm=3" > output/bench_go.txt
curl -s --max-time 20 -T output/bench_go.txt $H/data/prosperoweb/bench_go
sleep $((SECS + 12))
$R ./tools/evo-remote.sh key l3 >/dev/null 2>&1; ./tools/shot.sh grab >/dev/null 2>&1
LOG=$(curl -s --max-time 90 -r -400000- $H/data/prosperoweb/log.txt)
echo "== host bench lines"; echo "$LOG" | grep -a "bench:" | tail -3 | cut -c1-220
echo "== CPU for the window (processes report on cpu_now)"
echo "$LOG" | grep -a "cpus busy" | tail -6 | sed 's/cpu: affinity.*| user/user/' | sed 's/ | top.*//' | cut -c1-200
echo "== surface stats"
curl -s --max-time 30 $H/data/prosperoweb/mesa.txt | tail -n +$((startm+1)) | grep -a "surface: [0-9]*s \|skia:" | cut -c1-240 | tail -9
echo "== EVO"
curl -s --max-time 30 -r -5000- $H/mnt/usb0/evo.log | grep -a "browser: frames" | tail -2 | cut -c1-200
