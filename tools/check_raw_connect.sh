#!/bin/bash
# tools/check_raw_connect.sh - fail when EVO's own code calls connect() directly.
#
# On the console connect() ignores O_NONBLOCK and SO_SNDTIMEO: to an address
# that drops packets it sits in the kernel's SYN retry for ~75 s, whatever the
# socket options say (measured 2026-09-30 and 2026-11-03). A raw connect() in a
# network path is a 75 s freeze waiting for a dead server. Use
# evo_net_connect_addr() / evo_net_connect_list() (addons/include/evo_net.h),
# which connect on a worker and stop waiting at the deadline.
#
# A connect() that cannot hang (loopback, UDP, or the bounded worker itself)
# carries "connect-ok: <why>" on the same line.
# Run by scripts/build-evoplayer.sh and scripts/package-app.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=projects/evoplayer

hits=$(grep -rnE '(^|[^_[:alnum:].>])connect[[:space:]]*\(' \
          --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' \
          --exclude='main.c.legacy' "${SRC}" |
       grep -v 'connect-ok:' |
       grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' |
       grep -vE '/third_party/|_bundle_data\.cpp:' || true)
if [[ -n "${hits}" ]]; then
    echo "ERROR: raw connect() - it blocks ~75 s on a dead host on the PS5." >&2
    echo "${hits}" >&2
    echo "Use evo_net_connect_addr() (addons/include/evo_net.h), or mark a connect that cannot" >&2
    echo "hang (loopback, UDP) with 'connect-ok: <why>' on the same line." >&2
    exit 1
fi
echo "  ok no raw connect() in ${SRC}"
