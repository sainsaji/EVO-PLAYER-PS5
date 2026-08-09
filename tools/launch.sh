#!/usr/bin/env bash
# =============================================================================
# tools/launch.sh - launch the installed homebrew, with a guard against
# stacking instances.
#
#   ./tools/launch.sh                 launch, stream stdout for 15s
#   ./tools/launch.sh --timeout 30    stream for longer
#   ./tools/launch.sh --force         launch even if one was started recently
#   ./tools/launch.sh --quiet         do not echo the payload's output
#
# Why this exists
# ---------------
# The /hbldr app slot stays resident after a launch - install-homebrew.sh says
# so in its own comments, because the log pipe never EOFs. Launching again
# does not replace the running instance; it adds another one, and every
# instance opens videoout, an audio port, the pad and decoder threads.
#
# On 2026-08-09 a verification loop fired roughly ten launches in one session
# without exiting anything in between and the console kernel-panicked on
# launch, costing about fifty minutes of recovery. Nothing in the tooling
# pushed back, so the guard lives here now.
#
# The console offers no remote "kill app" that we have found - the websrv /fs
# endpoint is read-only and its API client only ever GETs - so this cannot
# stop the previous instance for you. What it can do is refuse to pile another
# one on without you having said so.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

WEB_PORT="${PS5_WEB_PORT:-8080}"
NAME="${EVO_HB_NAME:-EVOPlayer}"
ELF_PATH="/data/homebrew/${NAME}/eboot.elf"

TIMEOUT=15
FORCE=0
QUIET=0

# Minimum seconds between launches. Long enough that a reflexive re-run is
# caught, short enough not to obstruct deliberate iteration.
COOLDOWN="${EVO_LAUNCH_COOLDOWN:-90}"

STAMP="${OUTPUT_DIR}/.last-launch"

while (( $# )); do
    case "$1" in
        --timeout) shift; TIMEOUT="${1:?--timeout needs seconds}" ;;
        --force|-f) FORCE=1 ;;
        --quiet|-q) QUIET=1 ;;
        --name) shift; NAME="${1:?--name needs a value}"
                ELF_PATH="/data/homebrew/${NAME}/eboot.elf" ;;
        -h|--help) sed -n '3,10p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

require_ps5_host
need_cmd curl
mkdir -p "${OUTPUT_DIR}"

# -- the guard ----------------------------------------------------------------

if [[ -f "${STAMP}" ]] && (( ! FORCE )); then
    last="$(cat "${STAMP}" 2>/dev/null || echo 0)"
    now="$(date +%s)"
    age=$(( now - last ))

    if (( age < COOLDOWN )); then
        echo "" >&2
        warn "a launch was started ${age}s ago, and the app slot stays resident."
        cat >&2 <<EOF

   Launching again does not replace that instance - it adds a second one.
   Each holds videoout, an audio port, the pad and decoder threads, and
   stacking them has kernel-panicked this console before.

   Do one of these first:
     * exit EVO Player on the console (PS button -> close the application)
     * or wait $(( COOLDOWN - age ))s for this guard to lapse

   If you know the previous instance is gone:
     ./tools/launch.sh --force

EOF
        exit 1
    fi
fi

# -- launch -------------------------------------------------------------------

date +%s > "${STAMP}"

begin "launching ${NAME} (streaming stdout for ${TIMEOUT}s)"
echo "   exit it on the console before launching again"
echo ""

# curl exiting 28 is the normal outcome: the pipe never EOFs because the app
# slot stays alive, so --max-time is what ends the read. Anything printed
# before that is the payload's stdout - the only place it appears. It is NOT
# in klog, which is the kernel log.
crc=0
if (( QUIET )); then
    curl -sS --max-time "${TIMEOUT}" --get \
         --data-urlencode "path=${ELF_PATH}" \
         --data-urlencode "pipe=1" \
         "http://${PS5_HOST}:${WEB_PORT}/hbldr" >/dev/null 2>&1 || crc=$?
else
    curl -sS --max-time "${TIMEOUT}" --get \
         --data-urlencode "path=${ELF_PATH}" \
         --data-urlencode "pipe=1" \
         "http://${PS5_HOST}:${WEB_PORT}/hbldr" 2>/dev/null || crc=$?
fi

echo ""
case "${crc}" in
    0)  ok "${NAME} finished (stream closed)" ;;
    28) ok "${NAME} launched; detached after ${TIMEOUT}s" ;;
    7)  die "could not connect to websrv on ${PS5_HOST}:${WEB_PORT}.
       Re-run the jailbreak and start websrv." ;;
    *)  die "launch failed (curl exit ${crc})" ;;
esac
