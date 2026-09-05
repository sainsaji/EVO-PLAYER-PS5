#!/usr/bin/env bash
# =============================================================================
# tools/sandbox-unjail.sh - lift the app sandbox on the running EVO Player.
#
#   PS5_HOST=192.168.0.10 ./tools/sandbox-unjail.sh
#
# Phase 1b milestone-1 task 7 (docs/evo-pro/phase-1b-app-module.md section 5,
# Step B.2). Builds projects/sandbox_unjail and pushes it to ps5-payload-elfldr
# (port 9021), which runs it with kernel R/W. The payload finds the running
# "eboot.bin" (EVO / PPSA99039) process and clears fd_rdir / fd_jdir / uid /
# caps so the real /mnt/usb0 and /data become visible to it - no remount, no
# path changes.
#
# EVO keeps running; reopen its media browser afterwards. Launch-safety
# (docs/tooling.md) still applies: close any running game first - a game also
# runs as "eboot.bin".
# =============================================================================
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Re-run inside the dev container when invoked from the Windows host.
if [[ ! -f /opt/ps5-payload-sdk/toolchain/prospero.sh ]]; then
    command -v docker >/dev/null 2>&1 \
        || { echo "error: not in the dev container and 'docker' not found" >&2; exit 1; }
    exec docker compose -f "${REPO_ROOT}/docker-compose.yml" run --rm \
        -e "PS5_HOST=${PS5_HOST:-}" \
        -e "PS5_PORT=${PS5_PORT:-9021}" \
        ps5-dev bash "./tools/sandbox-unjail.sh"
fi

: "${PS5_HOST:?set PS5_HOST=<console ip>, e.g. PS5_HOST=192.168.0.10 ./tools/sandbox-unjail.sh}"
cd "${REPO_ROOT}"

echo "--- building + sending sandbox_unjail.elf -> elfldr ${PS5_HOST}:${PS5_PORT:-9021}"
make -C projects/sandbox_unjail test PS5_HOST="${PS5_HOST}"

echo ""
echo "--- sent. Read the on-screen notification, then reopen EVO's media browser."
