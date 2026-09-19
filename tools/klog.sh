#!/usr/bin/env bash
# =============================================================================
# tools/klog.sh - capture the console kernel log, and never lose it.
#
#   ./tools/klog.sh                 follow the log, printing and recording it
#   ./tools/klog.sh --quiet         record only, no terminal output
#   ./tools/klog.sh --grep evo      follow, showing only matching lines
#   ./tools/klog.sh --once          drain what is buffered and exit
#   ./tools/klog.sh --tail 200      print the last 200 recorded lines and exit
#   ./tools/klog.sh --sessions      list recorded sessions
#
# Why this exists
# ---------------
# The documented way to read klog was `nc $PS5_HOST 3232`, straight to the
# terminal. Everything scrolled past and nothing was kept, so the log was only
# ever useful if you happened to be watching at the moment something went
# wrong - which, for a crash on launch, you are not. Worse, klogsrv drops the
# connection whenever the payload restarts, so a bare `nc` exits exactly when
# a crash investigation is starting.
#
# So: every line goes to a per-session file *and* to an append-only aggregate
# that is never rotated or truncated, and a dropped connection reconnects with
# backoff instead of ending the capture.
#
# Requires ps5-payload-klogsrv to be running on the console. See
# docs/tooling.md.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

KLOG_PORT="${KLOG_PORT:-3232}"
KLOG_DIR="${LOG_OUT}/klog"
AGGREGATE="${KLOG_DIR}/klog-all.log"

MODE="follow"
QUIET=0
PATTERN=""
TAIL_N=200
# Reconnect backoff, seconds. Capped so a console that is off does not turn
# into a busy loop, but a payload restart is picked up promptly.
BACKOFF_MIN=1
BACKOFF_MAX=15

while (( $# )); do
    case "$1" in
        --quiet|-q)   QUIET=1 ;;
        --grep|-g)    shift; PATTERN="${1:?--grep needs a pattern}" ;;
        --once)       MODE="once" ;;
        --tail)       MODE="tail"; shift; TAIL_N="${1:?--tail needs a count}" ;;
        --sessions)   MODE="sessions" ;;
        --port)       shift; KLOG_PORT="${1:?--port needs a value}" ;;
        -h|--help)    sed -n '3,20p' "$0"; exit 0 ;;
        *)            die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

mkdir -p "${KLOG_DIR}"

# -- offline modes ------------------------------------------------------------

if [[ "${MODE}" == "sessions" ]]; then
    if ! compgen -G "${KLOG_DIR}/klog-*.log" > /dev/null; then
        log "no klog sessions recorded yet"
        exit 0
    fi
    log "recorded sessions in ${KLOG_DIR#"${REPO_ROOT}/"}"
    for f in "${KLOG_DIR}"/klog-*.log; do
        [[ "$(basename "$f")" == "klog-all.log" ]] && continue
        printf '  %-34s %8s lines  %s\n' \
            "$(basename "$f")" \
            "$(wc -l < "$f")" \
            "$(stat -c %y "$f" | cut -d. -f1)"
    done
    if [[ -f "${AGGREGATE}" ]]; then
        printf '  %-34s %8s lines  (append-only, never rotated)\n' \
            "$(basename "${AGGREGATE}")" "$(wc -l < "${AGGREGATE}")"
    fi
    exit 0
fi

if [[ "${MODE}" == "tail" ]]; then
    need_file "${AGGREGATE}" "nothing captured yet - run ./tools/klog.sh first"
    if [[ -n "${PATTERN}" ]]; then
        grep -i -- "${PATTERN}" "${AGGREGATE}" | tail -n "${TAIL_N}"
    else
        tail -n "${TAIL_N}" "${AGGREGATE}"
    fi
    exit 0
fi

# -- live capture -------------------------------------------------------------

require_ps5_host
need_cmd nc

SESSION="${KLOG_DIR}/klog-$(date -u +%Y%m%dT%H%M%SZ).log"

# Stamp every line with a host-side wall clock. klog carries no timestamps of
# its own, so without this there is no way to line a message up against a
# build, a launch, or a screenshot.
stamp_and_store() {
    local line
    while IFS= read -r line || [[ -n "${line}" ]]; do
        local out
        out="$(date -u +%H:%M:%S) ${line}"

        printf '%s\n' "${out}" >> "${SESSION}"
        printf '%s\n' "${out}" >> "${AGGREGATE}"

        (( QUIET )) && continue

        if [[ -n "${PATTERN}" ]]; then
            printf '%s\n' "${out}" | grep -i --color=never -- "${PATTERN}" || true
        else
            printf '%s\n' "${out}"
        fi
    done
}

note() {
    local out
    out="$(date -u +%H:%M:%S) --- $* ---"
    printf '%s\n' "${out}" >> "${SESSION}"
    printf '%s\n' "${out}" >> "${AGGREGATE}"
    (( QUIET )) || echo "${_C_BLU}${out}${_C_OFF}"
}

if [[ "${MODE}" == "once" ]]; then
    log "draining klog from ${PS5_HOST}:${KLOG_PORT}"
    nc -w 3 "${PS5_HOST}" "${KLOG_PORT}" 2>/dev/null | stamp_and_store || true
    ok "-> ${SESSION#"${REPO_ROOT}/"}"
    exit 0
fi

log "following klog from ${PS5_HOST}:${KLOG_PORT}"
echo "   session   : ${SESSION#"${REPO_ROOT}/"}"
echo "   aggregate : ${AGGREGATE#"${REPO_ROOT}/"}"
echo "   stop with Ctrl-C; the capture survives payload restarts"
echo ""

# Ctrl-C should close the capture cleanly rather than leaving a half-written
# last line and no record of why the session ended.
trap 'note "capture stopped by user"; exit 0' INT TERM

backoff="${BACKOFF_MIN}"

while true; do
    if nc -w 5 "${PS5_HOST}" "${KLOG_PORT}" 2>/dev/null | stamp_and_store; then
        note "klogsrv closed the connection"
        # Almost always a payload restart. Retry immediately so the first
        # lines of the next boot - the ones that matter after a crash - are
        # not the ones we miss.
        backoff="${BACKOFF_MIN}"
    else
        note "cannot reach klogsrv on ${PS5_HOST}:${KLOG_PORT}"
        backoff=$(( backoff * 2 ))
        (( backoff > BACKOFF_MAX )) && backoff="${BACKOFF_MAX}"
    fi

    sleep "${backoff}"
done
