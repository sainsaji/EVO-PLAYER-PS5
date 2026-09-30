#!/usr/bin/env bash
# tools/netconnect_host.sh - run the evo_net bounded-connect test on the host.
#
#   ./tools/netconnect_host.sh
#
# Compiles projects/evoplayer/addons/src/evo_net.c - the exact file the app
# module builds, here with -DNO_OPENSSL=1 because nothing in this test speaks TLS
# - against tools/netconnect_host.c under the address and undefined-behaviour
# sanitizers. It builds listeners on loopback, including one that silently drops
# SYNs (a full accept queue), to reproduce a host with one unreachable address.
# Needs no FFmpeg.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/netconnect_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"
OUT="${REPO_ROOT}/output/netconnect-host"
mkdir -p "${OUT}"

echo "--- building netconnect_host (-fsanitize=address,undefined)"
gcc -O1 -g -Wall -Wextra -DNO_OPENSSL=1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iprojects/evoplayer/addons/include -Iprojects/evoplayer/include \
    -o "${OUT}/netconnect_host" tools/netconnect_host.c projects/evoplayer/addons/src/evo_net.c -lpthread

"${OUT}/netconnect_host" "$@"
