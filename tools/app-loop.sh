#!/usr/bin/env bash
# =============================================================================
# tools/app-loop.sh - unattended app-module bring-up cycle.
#
#   PS5_HOST=192.168.0.10 ./tools/app-loop.sh [window-seconds]
#
#   1. build the player          (scripts/package-app.sh --player)
#   2. FTP it to the console     (scripts/deploy-app.sh)
#   3. relaunch PPSA99039        (projects/app_ctl payload via /hbldr:
#                                 kills the old eboot.bin, sceSystemServiceLaunchApp)
#   4. record klog for <window>  (EVO_BOOT_TRACE breadcrumbs go to klog now)
#   5. print the boot trail + any fatal-signal block
#
# No TV, no ShadowMountPlus UI. The title must have been registered once via
# ShadowMountPlus; after that this drives it in place on every redeploy.
#
# Prereqs on the console: elfldr + websrv (/hbldr) + klogsrv, all standard.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

WINDOW="${1:-35}"
WEB_PORT="${PS5_WEB_PORT:-8080}"
APPCTL_ELF="/data/homebrew/app_ctl/eboot.elf"

if ! in_container; then
    reexec_in_container "../tools/app-loop.sh" "${WINDOW}"
fi

require_ps5_host
need_cmd curl nc make
load_sdk

step "1/5  building the player"
"${SCRIPTS_DIR}/package-app.sh" --player

step "2/5  deploying to ${PS5_HOST}"
"${SCRIPTS_DIR}/deploy-app.sh"

step "3/5  building + installing the app_ctl payload"
make -C "${REPO_ROOT}/projects/app_ctl" >/dev/null
"${SCRIPTS_DIR}/install-homebrew.sh" --name app_ctl \
    "${REPO_ROOT}/projects/app_ctl/app_ctl.elf" >/dev/null

step "4/5  relaunch + record klog for ${WINDOW}s"
# Background klog capture (quiet: file only). It survives the app restart.
"${SCRIPTS_DIR}/../tools/klog.sh" --quiet &
KLOG_PID=$!
sleep 1
MARK="$(date -u +%H:%M:%S)"

# hbldr runs the payload; curl exit 28 (timeout) is the normal "detached" case.
curl -sS --max-time 12 --get \
    --data-urlencode "path=${APPCTL_ELF}" \
    --data-urlencode "pipe=1" \
    "http://${PS5_HOST}:${WEB_PORT}/hbldr" 2>/dev/null || true

sleep "${WINDOW}"
kill "${KLOG_PID}" 2>/dev/null || true
wait "${KLOG_PID}" 2>/dev/null || true

step "5/5  result  (klog since ${MARK})"
AGG="${LOG_OUT}/klog/klog-all.log"
if [[ -s "${AGG}" ]]; then
    awk -v mark="${MARK}" '$1 >= mark' "${AGG}" \
      | grep -E "EVO boot:|app_ctl:|fatal signal|signal:|SIGSEGV|SIGSYS|SIGABRT|fault address| rip:|# [0-9a-f]{8,}|calls exit\(\)" \
      | sed 's/^/   /' \
      || echo "   (no matching klog lines - is klogsrv running? did the app launch?)"
else
    warn "no klog captured - check klogsrv on the console"
fi
echo ""
ok "full session: ${AGG#"${REPO_ROOT}/"}   (tools/klog.sh --tail N to browse)"
