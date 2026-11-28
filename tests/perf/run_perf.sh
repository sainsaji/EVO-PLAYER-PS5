#!/usr/bin/env bash
# tests/perf/run_perf.sh - host performance models and benchmarks (no console).
# Runs inside the ps5-dev container: docker compose run --rm ps5-dev ./tests/perf/run_perf.sh
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${REPO_ROOT}/output/perf"
mkdir -p "${OUT}"
CC="${CC:-cc}"

echo "== shim_ring_bench: where the demux packet rings land"
"${CC}" -std=gnu11 -O2 -w -o "${OUT}/shim_ring_bench" "${REPO_ROOT}/tests/perf/shim_ring_bench.c"
"${OUT}/shim_ring_bench"
