#!/usr/bin/env bash
# tools/hls_host.sh - run the HLS master-playlist parser test on the host.
#
#   ./tools/hls_host.sh
#
# Compiles projects/evoplayer/addons/src/evo_hls_variants.c - the exact file the
# app module builds - against tools/hls_host.c under the address and
# undefined-behaviour sanitizers. Needs no FFmpeg: the parser and the relative
# URL join are pure, and the fetch runs against a stub of evo_net.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/hls_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"
OUT="${REPO_ROOT}/output/hls-host"
mkdir -p "${OUT}"

echo "--- building hls_host (-fsanitize=address,undefined)"
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iprojects/evoplayer/addons/include -Iprojects/evoplayer/include \
    -o "${OUT}/hls_host" tools/hls_host.c projects/evoplayer/addons/src/evo_hls_variants.c

"${OUT}/hls_host" "$@"
