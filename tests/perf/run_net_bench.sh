#!/usr/bin/env bash
# tests/perf/run_net_bench.sh - evo_net request latency against a local server
# with a simulated round trip (no console). Runs inside the ps5-dev container:
#   docker compose run --rm ps5-dev ./tests/perf/run_net_bench.sh
#
#   NET_SRC=path/to/evo_net.c   benchmark another evo_net.c (e.g. the one on main)
#   RTT_MS=30                   simulated round trip
#   NAGLE=1                     the server leaves Nagle on (worst case for keep-alive)
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${REPO_ROOT}/output/perf"
E="${REPO_ROOT}/projects/evoplayer"
mkdir -p "${OUT}"
CC="${CC:-cc}"
NET_SRC="${NET_SRC:-${E}/addons/src/evo_net.c}"
RTT_MS="${RTT_MS:-30}"
PORT="${PORT:-18080}"

echo "== net_bench: ${NET_SRC#${REPO_ROOT}/}, simulated RTT ${RTT_MS} ms${NAGLE:+, Nagle on}"
"${CC}" -std=gnu11 -O2 -w -DNO_OPENSSL=1 -DEVO_PLAYER_VERSION='"bench"' \
    -I"${E}/addons/include" -I"${E}/include" -I"${E}" -I"${E}/media/include" \
    -I"${E}/pp/include" -I"${E}/ui/include" \
    "${REPO_ROOT}/tests/perf/net_bench.c" "${NET_SRC}" "${E}/src/evo_error.c" \
    -pthread -o "${OUT}/net_bench"
python3 "${REPO_ROOT}/tests/perf/latency_server.py" "${PORT}" "${RTT_MS}" ${NAGLE:+nagle} &
SRV=$!
trap 'kill ${SRV} 2>/dev/null || true' EXIT
sleep 1
timeout 120 "${OUT}/net_bench" "${PORT}" 40 2>/dev/null
