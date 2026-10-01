#!/usr/bin/env bash
# tools/iptv_epg_host.sh — Host test runner for IPTV XMLTV EPG parsing
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/iptv_epg_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"
OUT="${REPO_ROOT}/output/iptv-epg-host"
mkdir -p "${OUT}"

echo "--- Building iptv_epg_host (-O2 -Wall -Wextra)"
gcc -O2 -g -Wall -Wextra -std=gnu11 -Wno-format-truncation -DNO_OPENSSL=1 \
    -Iprojects/evoplayer/addons/include \
    -Iprojects/evoplayer/include \
    -Iprojects/evoplayer/media/include \
    -Iprojects/evoplayer/ui/include \
    -Iprojects/evoplayer \
    -o "${OUT}/iptv_epg_host" \
    tools/iptv_epg_host.c

echo "--- Running iptv_epg_host"
"${OUT}/iptv_epg_host" "$@"
