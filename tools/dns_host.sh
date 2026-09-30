#!/usr/bin/env bash
# tools/dns_host.sh - run the #91 name-resolution test on the host.
#
#   ./tools/dns_host.sh
#
# Compiles tools/native-app/stubs/evo_dns.c - the exact file the app module
# links - against a mock libSceNet (tools/dns_host.c) under the address and
# undefined-behaviour sanitizers, then checks the shipped sources for the two
# things #91 removed: a hardcoded DNS server, and a hand-rolled DNS client.
# The mock's multi-record call is run well (several addresses), and badly: an
# error, a layout that is not the documented one, a write past the struct.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash ./tools/dns_host.sh "$@"
    exit $?
fi

cd "${REPO_ROOT}"
OUT="${REPO_ROOT}/output/dns-host"
mkdir -p "${OUT}"

echo "--- sources"
fail=0
# No dotted IPv4 literal in the resolver's code (its comments cite the address
# that motivated the multi-record lookup, which is documentation, not a server),
# and none of the old client's pieces anywhere it used to live.
if grep -nE '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' tools/native-app/stubs/evo_dns.c |
        grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)'; then
    echo "FAIL: evo_dns.c contains an IP literal in code" >&2; fail=1
fi
if grep -nE 'kDnsServers|dns_query_server|s_dns_txid|/etc/resolv\.conf|0x5a31' \
        tools/native-app/stubs/*.c projects/evoplayer/addons/src/*.c; then
    echo "FAIL: a hand-rolled DNS client is still in the tree" >&2; fail=1
fi
if [[ "$(grep -lE '^int getaddrinfo\(' tools/native-app/stubs/*.c projects/evoplayer/addons/src/*.c | wc -l)" != 1 ]]; then
    echo "FAIL: getaddrinfo must be defined in exactly one file" >&2; fail=1
fi
(( fail == 0 )) && echo "  ok: no IP literal, no DNS client, one getaddrinfo"

echo "--- building dns_host (-fsanitize=address,undefined)"
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itools -o "${OUT}/dns_host" tools/dns_host.c -lpthread

# Each way the multi-record call can go wrong is its own process: the module
# remembers for the rest of a run that it is unusable.
for scenario in main multifail garbage overrun killswitch; do
    echo "--- scenario: ${scenario}"
    "${OUT}/dns_host" "${scenario}" || fail=1
done
exit "${fail}"
