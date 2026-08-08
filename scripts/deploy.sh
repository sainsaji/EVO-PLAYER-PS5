#!/usr/bin/env bash
# =============================================================================
# scripts/deploy.sh - send a payload ELF to a jailbroken PS5.
#
#   ./scripts/deploy.sh output/elf/hello_world.elf
#   PS5_HOST=192.168.1.50 ./scripts/deploy.sh output/elf/hello_world.elf
#   PS5_HOST=ps5 PS5_PORT=9021 ./scripts/deploy.sh output/elf/videoout_test.elf
#
# HOW IT WORKS
#   ps5-payload-elfldr listens on TCP 9021 and executes whatever ELF is written
#   to the socket. The SDK's own helper (host/bin/prospero-deploy) does:
#       socat -t 9999999 - TCP:$HOST:$PORT < payload.elf
#   We call that helper so behaviour matches `make test` in the SDK samples,
#   after doing friendlier validation than it does.
#
# The console's IP is never stored in this repository. Supply PS5_HOST via the
# environment or a git-ignored .env file.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] <payload.elf>

Options:
  -H, --host HOST   console address   (default: \$PS5_HOST)
  -p, --port PORT   loader port       (default: \$PS5_PORT, else 9021)
  -i, --interactive keep stdin attached (for payloads that read stdin)
      --no-check    skip the TCP reachability probe
  -h, --help        this message

Environment:
  PS5_HOST   required - console IP or hostname
  PS5_PORT   optional - defaults to 9021 (ps5-payload-elfldr)
EOF
}

ELF=""
INTERACTIVE=0
DO_CHECK=1

while (( $# )); do
    case "$1" in
        -H|--host)        shift; PS5_HOST="${1:?--host needs a value}" ;;
        -p|--port)        shift; PS5_PORT="${1:?--port needs a value}" ;;
        -i|--interactive) INTERACTIVE=1 ;;
        --no-check)       DO_CHECK=0 ;;
        -h|--help)        usage; exit 0 ;;
        -*)               die "unknown option: $1 (try --help)" ;;
        *)                [[ -z "${ELF}" ]] || die "more than one payload given"
                          ELF="$1" ;;
    esac
    shift
done

[[ -n "${ELF}" ]] || { usage >&2; echo "" >&2; die "no payload ELF given."; }

# Run on the host? Hand off to the container so socat and the SDK are present.
if ! in_container; then
    FWD=()
    (( INTERACTIVE )) && FWD+=(-i)
    (( DO_CHECK )) || FWD+=(--no-check)
    reexec_in_container "deploy.sh" "${FWD[@]+"${FWD[@]}"}" "${ELF}"
fi

# -----------------------------------------------------------------------------
# Validate inputs before touching the network.
# -----------------------------------------------------------------------------
begin "validating payload"

# Accept a repo-relative path, an absolute path, or a bare name in output/elf.
if [[ ! -f "${ELF}" ]]; then
    for cand in "${REPO_ROOT}/${ELF}" "${ELF_OUT}/${ELF}" "${ELF_OUT}/${ELF}.elf"; do
        if [[ -f "${cand}" ]]; then ELF="${cand}"; break; fi
    done
fi
[[ -f "${ELF}" ]] || die "payload not found: ${ELF}
       Build one first:  ./scripts/build.sh
       Available payloads in output/elf:
$(ls -1 "${ELF_OUT}" 2>/dev/null | sed 's/^/         /' || echo '         (none)')"

validate_elf "${ELF}"

# -----------------------------------------------------------------------------
begin "validating target"
require_ps5_host "${ELF}"

# Reject the obvious footgun of pointing the loader at the container itself.
case "${PS5_HOST}" in
    localhost|127.0.0.1|::1)
        die "PS5_HOST=${PS5_HOST} points at the container, not your console.
       On Docker Desktop the container's localhost is NOT your Windows machine
       and is certainly not the PS5. Use the console's LAN IP address." ;;
esac

echo "  host : ${PS5_HOST}"
echo "  port : ${PS5_PORT}"
echo "  elf  : ${ELF}"

if (( DO_CHECK )); then
    begin "probing ${PS5_HOST}:${PS5_PORT}"
    if ! check_ps5_reachable "${PS5_HOST}" "${PS5_PORT}" 5; then
        die "cannot reach ${PS5_HOST}:${PS5_PORT}.

       Checklist:
         1. Is the PS5 powered on, awake, and on the same LAN?
         2. Is the console jailbroken *right now*? The exploit must be re-run
            after every reboot, and ps5-payload-elfldr must be running.
         3. Is 9021 the right port? That is the elfldr default; the BD-J and
            ps5-jar-loader entry points may differ.
         4. From inside this container, try:
                nc -vz ${PS5_HOST} ${PS5_PORT}
                ping -c3 ${PS5_HOST}
         5. Docker Desktop networking: outbound LAN access works on the default
            bridge network. If ping succeeds but 9021 refuses, the loader is
            not listening - re-run the jailbreak. See docs/networking.md.
         6. Bypass this probe with --no-check if you know better."
    fi
fi

# -----------------------------------------------------------------------------
begin "deploying"
load_sdk
need_cmd socat

mkdirs
LOGFILE="${LOG_OUT}/deploy-$(basename "${ELF}" .elf)-$(date -u +%Y%m%dT%H%M%SZ).log"

{
    echo "timestamp : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "payload   : ${ELF}"
    echo "sha256    : $(sha256sum "${ELF}" | cut -d' ' -f1)"
    echo "size      : $(stat -c %s "${ELF}") bytes"
    echo "target    : ${PS5_HOST}:${PS5_PORT}"
    echo "---"
} | tee "${LOGFILE}"

DEPLOY_ARGS=(-h "${PS5_HOST}" -p "${PS5_PORT}")
(( INTERACTIVE )) && DEPLOY_ARGS+=(-i)

if "${PS5_DEPLOY}" "${DEPLOY_ARGS[@]}" "${ELF}" 2>&1 | tee -a "${LOGFILE}"; then
    echo ""
    ok "payload sent to ${PS5_HOST}:${PS5_PORT}"
    echo "   log: ${LOGFILE#"${REPO_ROOT}/"}"
    echo ""
    echo "   Where to look for output:"
    echo "     * on-screen notifications (sceKernelSendNotificationRequest)"
    echo "     * klog  - run ps5-payload-klogsrv and 'nc \$PS5_HOST 3232'"
    echo "     * stdio - only visible if the loader redirects it (websrv does)"
else
    die "prospero-deploy failed. The socket was open but the transfer did not
       complete - the loader may have crashed, or the console may have gone to
       sleep. Re-run the jailbreak and try again."
fi
