#!/usr/bin/env bash
# =============================================================================
# tools/evo-remote.sh — scriptable dev remote for the PPSA99039 app module.
#
# One deployed build (built with --usb-remote) is driven over FTP with no
# controller and no rebuild between tests. It writes /mnt/usb0/evo_status once
# a second and reads /mnt/usb0/evo_cmd for commands. Every build also writes
# ONE diagnostic log, /mnt/usb0/evo.log (boot trace + breadcrumbs + decoder
# notes + playback stats, timestamped).
#
#   PS5_HOST=192.168.0.6 ./tools/evo-remote.sh <subcommand>
#
#   build [--breadcrumbs ...]               package --usb-remote + deploy .ffpfsc
#   launch                                  launch PPSA99039 with no controller
#                                           (title-aware controller -> elfldr
#                                           :9021). Refused while ShadowMount
#                                           shows it resident or the heartbeat
#                                           is live - never stacks a launch
#   quit                                    soft close (= Settings -> QUIT EVO):
#                                           stop media, drain the GPU, park.
#                                           Waits for parked=1 in evo_status
#   close [--force]                         free the slot: the title-aware close
#                                           controller, then waits for
#                                           ShadowMount's "runtime layers
#                                           released". Only after `quit` has
#                                           parked it; --force when there is no
#                                           heartbeat to read (never if running)
#   cycle [--secs n] [--play <path>]        ONE bounded hardware cycle, the
#         [--no-build] [-- pkg-args...]     ps5-homebrew-dev-protocol runbook:
#                                           free slot -> package -> deploy ->
#                                           ShadowMount registered -> launched
#                                           (auto, else `launch`) -> klog + evo.log
#                                           for n s -> quit -> close -> classify.
#                                           Evidence + result.json in
#                                           output/cycles/<stamp>/, one line
#                                           appended to output/cycles/ledger.txt.
#                                           Stops at the first anomaly, never
#                                           retries, never chains
#   kill                                    = quit + close
#   play <path>                             open <path> from the start
#   seek <sec> | seek +<sec> | seek -<sec>  seek
#   stop                                    end playback, back to the browser
#   key <button>...                         synthetic pad presses (up down left
#                                           right cross circle square triangle
#                                           l1 r1 l2 r2 l3 r3 options touchpad
#                                           touchpad_left touchpad_right; `l3`/
#                                           `shot` takes a screenshot), in order.
#                                           After each it waits for the UI state
#                                           it produced and prints one line:
#                                           [focus] #id "text" (item i/n) on
#                                           Screen. `key --no-wait <b>` = one
#                                           press, no readback
#   type [--submit] <text>                  fill the virtual keyboard in one
#                                           shot (--submit also presses DONE),
#                                           instead of ~5 D-pad presses per
#                                           character. Requires Settings ->
#                                           Interface & Storage -> KEYBOARD
#                                           INPUT = VIRTUAL KEYBOARD: the
#                                           native PS5 IME is a system dialog
#                                           and nothing EVO injects reaches it
#   kbdone                                  press DONE on the virtual keyboard
#   ui [--line]                             #115: the UI state as text - screen,
#                                           focused element, its list, modal,
#                                           toast, player OSD - from
#                                           /mnt/usb0/evo_ui.json. Use this, not
#                                           a screenshot, to navigate
#   screen <id>                             go straight to a screen (an evo::
#                                           ScreenId number: 2 Player, 21 Text
#                                           Reader, 28 SurroundTest, 30 Image
#                                           Viewer, ...)
#   source <n>                              storage browser source: 0 USB,
#                                           1 Internal, 2 Favorites, 3 Recent
#   image <path>                            open <path> in the image viewer
#   text <path>                             open <path> in the text reader
#   upcompare [--crop x,y,w,h]              #103: pause, capture the same frame
#                                           with the upscaler Off/Sharp/AI (no
#                                           OSD) -> output/upcompare/
#   status                                  print /mnt/usb0/evo_status once
#   log                                     pull /mnt/usb0/evo.log (-> output/logs/)
#   watch [seconds]                         stream evo_status + new evo.log lines
#   clear                                   delete evo.log + evo_status
#   sweep [--dir p] [--secs n] [--max n]    #8 codec sweep: play every clip in a
#         [--skip substr]...                directory, harvest the per-file
#                                           metrics, render output/logs/sweep.md
#                                           (--skip fences off a clip that is
#                                           known to take the app down)
#   report [evo.log]                        re-render that table offline
#
# launch/close send the vendored controllers in tools/ps5-controllers/ (from
# ps5-homebrew-dev-protocol). Their evidence is ShadowMount+'s debug.log, read
# by tools/evo_lifecycle.py; elfldr accepting the bytes proves nothing.
# Protocol, safety rules, outcome classes: docs/build/tooling.md#hardware-cycle
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

SUB="${1:-}"; shift || true
FTP_PORT="${FTP_PORT:-2121}"
WEB_PORT="${PS5_WEB_PORT:-8080}"

if ! in_container; then
    reexec_in_container "../tools/evo-remote.sh" "${SUB}" "$@"
fi
require_ps5_host
need_cmd python3 curl nc timeout

ftp_py() { PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" python3 - "$@"; }

put_cmd() {   # put_cmd "<command line>"
    ftp_py "$1" <<'PY'
import os, io, sys
from ftplib import FTP
with FTP() as f:
    f.connect(os.environ["PS5_HOST"], int(os.environ["FTP_PORT"]), timeout=15)
    f.login()
    try: f.set_pasv(True)
    except Exception: pass
    f.storbinary("STOR /mnt/usb0/evo_cmd", io.BytesIO((sys.argv[1] + "\n").encode()))
print("sent:", sys.argv[1])
PY
}

get_file() {  # get_file <remote> -> stdout
    ftp_py "$1" <<'PY'
import os, sys
from ftplib import FTP, error_perm
try:
    with FTP() as f:
        f.connect(os.environ["PS5_HOST"], int(os.environ["FTP_PORT"]), timeout=15)
        f.login()
        try: f.set_pasv(True)
        except Exception: pass
        buf = []
        try: f.retrbinary("RETR " + sys.argv[1], buf.append)
        except error_perm: sys.exit(3)
        sys.stdout.write(b"".join(buf).decode("utf-8", "replace"))
except Exception as e:
    sys.stderr.write(str(e) + "\n"); sys.exit(1)
PY
}

del_files() { ftp_py "$@" <<'PY'
import os, sys
from ftplib import FTP, error_perm
with FTP() as f:
    f.connect(os.environ["PS5_HOST"], int(os.environ["FTP_PORT"]), timeout=15)
    f.login()
    try: f.set_pasv(True)
    except Exception: pass
    for p in sys.argv[1:]:
        try: f.sendcmd("DELE " + p); print("del", p)
        except error_perm: print("--", p)
PY
}

USB_STATUS="/mnt/usb0/evo_status"
USB_LOG="/mnt/usb0/evo.log"
USB_UI="/mnt/usb0/evo_ui.json"

evo_ui() { PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" \
           python3 "$(dirname "${BASH_SOURCE[0]}")/evo_ui.py" "$@"; }

# --- lifecycle: controllers + ShadowMount evidence ---------------------------
TITLE_ID="PPSA99039"
ELF_PORT="${PS5_ELF_PORT:-9021}"
CTRL_SRC="$(dirname "${BASH_SOURCE[0]}")/ps5-controllers"
CTRL_OUT="${OUTPUT_DIR}/controllers"

lc() { PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" TITLE_ID="${TITLE_ID}" \
       python3 "$(dirname "${BASH_SOURCE[0]}")/evo_lifecycle.py" "$@"; }

send_controller() {   # send_controller launch|close
    local mode="$1" elf="${CTRL_OUT}/${1}-${TITLE_ID}.elf"
    local libs=(-lSceSystemService)
    [[ "${mode}" == launch ]] && libs+=(-lSceUserService)
    mkdir -p "${CTRL_OUT}"
    "${PS5_PAYLOAD_SDK}/bin/prospero-clang" -Wall -Werror \
        -DBOOTSTRAP_TITLE_ID="\"${TITLE_ID}\"" "${libs[@]}" \
        -o "${elf}" "${CTRL_SRC}/${mode}.c" || die "could not build the ${mode} controller"
    timeout --signal=TERM 15s nc -q0 "${PS5_HOST}" "${ELF_PORT}" < "${elf}" \
        || die "elfldr ${PS5_HOST}:${ELF_PORT} did not take the ${mode} controller"
    echo "sent: ${mode} ${TITLE_ID} -> ${PS5_HOST}:${ELF_PORT}"
}

do_launch() {
    local slot hb base
    slot="$(lc slot)"; hb="$(lc heartbeat)"
    [[ "${hb}" == RUNNING ]] && die "EVO's heartbeat is advancing - it is running. Not stacking a launch."
    if [[ "${slot}" == UNKNOWN ]]; then
        # ShadowMount has no verdict, so the heartbeat is all there is, and a
        # parked EVO still holds the slot. (With a slot verdict this check is
        # wrong: `close` leaves /mnt/usb0/evo_status behind with parked=1, so a
        # freed slot still reads PARKED until the next launch deletes it.)
        [[ "${hb}" == PARKED ]] && die "EVO is parked (soft-closed) - the slot is still held. Not launching.
   Free it first: evo-remote.sh close."
        # A fresh boot: the slot stays UNKNOWN until something launches, and
        # nothing may launch while it is UNKNOWN. coldboot() breaks that by
        # proving the slot is free instead of assuming it - ShadowMount's log
        # reaches back to its own startup banner and records no launch of this
        # title since. The heartbeat has to agree: RUNNING and PARKED are
        # already out above, so only ABSENT (no evo_status at all, which is
        # what a deploy leaves) and STILL (a stale file) can get here.
        [[ "$(lc coldboot)" == COLDBOOT ]] \
            || die "could not read ShadowMount's log - cannot prove the slot is free. Not launching."
        warn "cold boot: ShadowMount has logged no launch of ${TITLE_ID} since it started, heartbeat ${hb} - slot is free"
    else
        [[ "${slot}" == FREE ]] || die "ShadowMount says ${TITLE_ID} is ${slot} (started, not released). Not launching.
   If it is parked: evo-remote.sh close. Otherwise PS-button-close it first."
    fi
    # Launching while ShadowMount is still busy (a fresh deploy: scan, verify,
    # remount) raises "Can't start game or app" on the PS5. Wait it out.
    [[ "$(lc wait-quiet 15 120)" == QUIET ]] \
        || die "ShadowMount's log is still moving after 120 s - it is not idle. Not launching."
    base="$(lc smlen)" || die "could not read ShadowMount's log"
    del_files "${USB_STATUS}" >/dev/null || true
    send_controller launch
    [[ "$(lc wait-started "${base}" 30)" == STARTED ]] \
        || die "no '[GAME] started: ${TITLE_ID}' within 30 s - launch did not take (0x80940005 class?).
   Look at the TV: if \"Can't start game or app\" is showing, press X on the pad to clear it
   (nothing here can) before launching again. Never retry automatically."
    ok "launched (ShadowMount: started)"
}

do_quit() {
    local hb; hb="$(lc heartbeat)"
    case "${hb}" in
        PARKED)  ok "already parked"; return 0 ;;
        RUNNING) ;;
        *) die "no live heartbeat (${hb}) - not a running --usb-remote build, so \`quit\` cannot reach it." ;;
    esac
    put_cmd "quit"
    [[ "$(lc wait-parked 30)" == PARKED ]] || die "EVO did not report parked=1 within 30 s. Do NOT close it remotely - check the TV."
    ok "parked - GPU drained, safe to close"
}

do_close() {
    local force=0 hb slot base
    [[ "${1:-}" == --force ]] && force=1
    slot="$(lc slot)"
    if [[ "${slot}" == FREE ]]; then ok "ShadowMount: ${TITLE_ID} already released"; return 0; fi
    hb="$(lc heartbeat)"
    case "${hb}" in
        PARKED) ;;
        RUNNING) die "EVO is running (heartbeat advancing). Killing a submitting GPU panicked the console
   on 2026-09-18. Run evo-remote.sh quit first; --force does not override this." ;;
        *) (( force )) || die "cannot see EVO's state (heartbeat ${hb}): not a --usb-remote build, or not parked.
   Park it first (Settings -> QUIT EVO), then: evo-remote.sh close --force"
           warn "closing without a parked=1 receipt (--force)" ;;
    esac
    base="$(lc smlen)" || die "could not read ShadowMount's log"
    send_controller close
    [[ "$(lc wait-released "${base}" 30)" == RELEASED ]] \
        || die "no '[LINK] runtime layers released: ${TITLE_ID}' within 30 s. STOP - check the console before anything else."
    ok "closed (ShadowMount: runtime layers released)"
}

# One bounded cycle (ps5-homebrew-dev-protocol docs/RUNBOOK.md, section 3).
# Every exit path writes result.json + one ledger line. Never retries.
do_cycle() {
    local secs=60 play="" build=1 pkg=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --secs)     secs="$2"; shift 2 ;;
            --play)     play="$2"; shift 2 ;;
            --no-build) build=0; shift ;;
            --)         shift; pkg=("$@"); break ;;
            *) die "usage: evo-remote.sh cycle [--secs n] [--play <path>] [--no-build] [-- package-app args]" ;;
        esac
    done
    local stamp dir commit dirty img sha="" base stage="none" klog_pid=""
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    dir="${OUTPUT_DIR}/cycles/${stamp}"
    mkdir -p "${dir}"
    commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    dirty="$(git -C "${REPO_ROOT}" status --porcelain --untracked-files=no 2>/dev/null | wc -l)"
    img="${OUTPUT_DIR}/app/${TITLE_ID}.ffpfsc"

    finish() {   # finish <outcome> <next-action>
        [[ -n "${klog_pid}" ]] && { kill "${klog_pid}" 2>/dev/null || true; }
        local line
        line="- $(date -u +%F) | ${commit}$( (( dirty )) && echo +dirty) | ${TITLE_ID} | $1: ${stage} | output/cycles/${stamp} | $2"
        python3 - "${dir}/result.json" "$1" "${stage}" "${commit}" "${dirty}" "${sha}" "$2" <<'PY'
import json, sys
p, outcome, stage, commit, dirty, sha, nxt = sys.argv[1:]
json.dump({"outcome": outcome, "highestStage": stage, "commit": commit,
           "dirtyFiles": int(dirty), "ffpfscSha256": sha, "next": nxt},
          open(p, "w"), indent=2)
PY
        echo "${line}" >> "${OUTPUT_DIR}/cycles/ledger.txt"
        echo ""; echo "${line}"
        [[ "$1" == pass ]] && return 0
        exit 1
    }

    # 1. preflight - only the declared services
    local port klog=1
    for port in "${FTP_PORT}" "${ELF_PORT}"; do
        nc -z -w 3 "${PS5_HOST}" "${port}" 2>/dev/null \
            || { stage="preflight"; finish no-run "service :${port} down - re-run the jailbreak chain"; }
    done
    # klog is evidence, not a requirement: klogsrv is not in every autoload chain
    nc -z -w 3 "${PS5_HOST}" 3232 2>/dev/null \
        || { klog=0; warn "klog :3232 closed - no klog.log this cycle; a crash then shows only as a lost heartbeat (inconclusive)"; }
    (( dirty )) && warn "${dirty} tracked file(s) uncommitted - the candidate is not frozen (ledger marks +dirty)"

    # 2. a free slot, by evidence: quit -> parked -> close -> released
    if [[ "$(lc slot)" != FREE ]]; then
        local hb; hb="$(lc heartbeat)"
        [[ "${hb}" == RUNNING ]] && { ( do_quit ) || { stage="preflight"; finish no-run "quit did not park"; }; hb=PARKED; }
        [[ "${hb}" == PARKED ]] || { stage="preflight"; finish no-run "resident with no heartbeat (${hb}) - park/close it by hand"; }
        ( do_close ) || { stage="preflight"; finish failed "close never released the slot - STOP, check the console"; }
    fi
    stage="slot-free"

    # 3. build the candidate
    if (( build )); then
        "${SCRIPTS_DIR}/package-app.sh" --ffpfsc --usb-remote "${pkg[@]+"${pkg[@]}"}" > "${dir}/package.log" 2>&1 \
            || finish no-run "package failed - see package.log"
    fi
    [[ -f "${img}" ]] || finish no-run "no ${img}"
    sha="$(sha256sum "${img}" | cut -d' ' -f1)"
    echo "${sha}  ${TITLE_ID}.ffpfsc" > "${dir}/sha256.txt"
    stage="packaged"

    # 4. deploy, then ShadowMount must register it before anything launches
    base="$(lc smlen)" || { finish transport-failure "ShadowMount log unreadable"; }
    "${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc > "${dir}/deploy.log" 2>&1 \
        || finish transport-failure "deploy failed - do NOT retry; see deploy.log"
    stage="deployed"
    [[ "$(lc wait-registered "${base}" 90)" == REGISTERED ]] \
        || { lc smslice "${base}" > "${dir}/shadowmount.log" 2>/dev/null || true; finish no-run "ShadowMount never registered the new image - not launching"; }
    stage="registered"

    # 5. observe: klog from before the launch, ShadowMount auto-launches on the
    #    image change - only send `launch` if it did not.
    : > "${dir}/klog.log"
    if (( klog )); then
        timeout --signal=TERM "$((secs + 90))s" nc "${PS5_HOST}" 3232 > "${dir}/klog.log" 2>/dev/null &
        klog_pid=$!
    fi
    if [[ "$(lc wait-started "${base}" 40)" != STARTED ]]; then
        ( do_launch ) > "${dir}/launch.log" 2>&1 \
            || { lc smslice "${base}" > "${dir}/shadowmount.log" 2>/dev/null || true; finish failed "never started - see launch.log + klog.log"; }
    fi
    stage="started"
    [[ "$(lc heartbeat)" =~ RUNNING|PARKED ]] && stage="app-checkpoint"
    [[ -n "${play}" ]] && { sleep 5; put_cmd "play ${play}" > /dev/null; }
    sleep "${secs}"
    get_file "${USB_STATUS}" > "${dir}/evo_status.txt" 2>/dev/null || true
    get_file "${USB_LOG}"    > "${dir}/evo.log"        2>/dev/null || true

    # 6. close: app-initiated soft close first, the kill controller only once parked
    local hb; hb="$(lc heartbeat)"
    if [[ "${hb}" != RUNNING && "${hb}" != PARKED ]]; then
        lc smslice "${base}" > "${dir}/shadowmount.log" 2>/dev/null || true
        grep -qaE 'A user thread receives a fatal signal|App Crash' "${dir}/klog.log" \
            && finish failed "runtime crash - see klog.log"
        finish inconclusive "started but no heartbeat (${hb}) - check the TV before the next cycle"
    fi
    ( do_quit ) > "${dir}/quit.log" 2>&1 || finish partial-pass "did not park - close by hand, check the TV"
    ( do_close ) > "${dir}/close.log" 2>&1 || finish failed "parked but never released - STOP, check the console"
    stage="teardown"
    kill "${klog_pid}" 2>/dev/null || true; klog_pid=""
    lc smslice "${base}" > "${dir}/shadowmount.log" 2>/dev/null || true

    # 7. classify
    grep -qaE 'A user thread receives a fatal signal|App Crash' "${dir}/klog.log" \
        && finish failed "crash in klog.log despite a clean teardown"
    grep -q 'fatal=1' "${dir}/evo_status.txt" 2>/dev/null \
        && finish failed "decoder fatal=1 - see evo.log"
    finish pass "-"
}

case "${SUB}" in
build)
    "${SCRIPTS_DIR}/package-app.sh" --ffpfsc --usb-remote "$@"
    del_files "${USB_STATUS}" "${USB_LOG}" || true
    "${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc
    echo ""
    echo "  >>> launch PPSA99039 from the Games row (ShadowMount+ remounted) <<<"
    ;;
launch) do_launch ;;
quit)   do_quit ;;
close)  do_close "${1:-}" ;;
kill)   do_quit; do_close ;;
cycle)  do_cycle "$@" ;;
play)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh play <path>"; put_cmd "play $1" ;;
seek)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh seek <sec|+sec|-sec>"; put_cmd "seek $1" ;;
stop)   put_cmd "stop" ;;
key)    [[ -n "${1:-}" ]] || die "usage: evo-remote.sh key <button>...  (e.g. cross, up, l1, l3)"
        if [[ "${1}" == --no-wait ]]; then shift; put_cmd "key $1"
        else evo_ui key "$@"; fi ;;
ui)     evo_ui show "$@" ;;
screen) [[ "${1:-}" =~ ^[0-9]+$ ]] || die "usage: evo-remote.sh screen <id>  (an evo::ScreenId number)"; put_cmd "screen $1" ;;
source) [[ "${1:-}" =~ ^[0-9]+$ ]] || die "usage: evo-remote.sh source <n>  (0 USB, 1 Internal, 2 Favorites, 3 Recent)"; put_cmd "source $1" ;;
iobench) put_cmd "iobench ${1:-}" ;;
image)  [[ -n "${1:-}" ]] || die "usage: evo-remote.sh image <path>"; put_cmd "image $1" ;;
text)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh text <path>"; put_cmd "text $1" ;;
# Fill EVO's virtual keyboard in one shot, rather than ~5 D-pad presses per
# character. --submit also presses DONE. Needs Settings -> Interface & Storage
# -> KEYBOARD INPUT = VIRTUAL KEYBOARD: the native PS5 IME is a system dialog
# and nothing EVO can inject will ever reach it.
type)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh type [--submit] <text>"
        _submit=0
        if [[ "${1}" == --submit ]]; then _submit=1; shift; fi
        [[ -n "${1:-}" ]] || die "usage: evo-remote.sh type [--submit] <text>"
        put_cmd "type $*"
        (( _submit )) && { sleep 1; put_cmd "kbdone"; }
        true ;;
kbdone) put_cmd "kbdone" ;;
upcompare)
    # #103: same paused frame with the upscaler Off / Sharp / AI, no OSD ->
    # output/upcompare/{off,sharp,ai}.bmp (+ compare.png where Pillow exists).
    PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}"     python3 "$(dirname "${BASH_SOURCE[0]}")/upcompare_run.py" "$@" || die "upcompare failed"
    ;;
sweep)
    # #8 — the codec sweep. Plays every clip in a directory for a fixed window,
    # then harvests the per-file `sweep` lines EVO wrote into evo.log and renders
    # the docs/validation.md table. Each `play` implicitly closes the previous
    # file (start_video_playback stops first), which is what flushes its row; the
    # trailing `stop` flushes the last one.
    SWEEP_DIR="/mnt/usb0/media/Test/Audio"
    SWEEP_SECS=30
    SWEEP_MAX=0
    SWEEP_SKIP=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dir)  SWEEP_DIR="$2"; shift 2 ;;
            --secs) SWEEP_SECS="$2"; shift 2 ;;
            --max)  SWEEP_MAX="$2"; shift 2 ;;
            # repeatable; a clip whose name contains any of these is not played
            --skip) SWEEP_SKIP="${SWEEP_SKIP:+${SWEEP_SKIP},}$2"; shift 2 ;;
            *) die "usage: evo-remote.sh sweep [--dir <path>] [--secs <n>] [--max <n>] [--skip <substr>]..." ;;
        esac
    done
    mkdir -p "${LOG_OUT}"
    PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" \
    SWEEP_DIR="${SWEEP_DIR}" SWEEP_SECS="${SWEEP_SECS}" SWEEP_MAX="${SWEEP_MAX}" \
    SWEEP_SKIP="${SWEEP_SKIP}" \
    python3 "$(dirname "${BASH_SOURCE[0]}")/sweep_run.py" || die "sweep aborted"
    get_file "${USB_LOG}" > "${LOG_OUT}/evo.log" || die "could not pull evo.log"
    python3 "$(dirname "${BASH_SOURCE[0]}")/sweep_report.py" \
        "${LOG_OUT}/evo.log" -o "${LOG_OUT}/sweep.md"
    echo ""
    echo "  table -> output/logs/sweep.md   (paste into docs/validation.md)"
    ;;
report)
    # Re-render the table from an evo.log already on disk; no console needed.
    mkdir -p "${LOG_OUT}"
    python3 "$(dirname "${BASH_SOURCE[0]}")/sweep_report.py" \
        "${1:-${LOG_OUT}/evo.log}" -o "${LOG_OUT}/sweep.md"
    ;;
clear)  del_files "${USB_STATUS}" "${USB_LOG}" "${USB_UI}" ;;
status) get_file "${USB_STATUS}" || echo "(no evo_status — launched? built --usb-remote?)" ;;
boot|log)
    mkdir -p "${LOG_OUT}"
    get_file "${USB_LOG}" 2>/dev/null | tee "${LOG_OUT}/evo.log" \
        || echo "(no evo.log — launched? sandbox open?)"
    ;;
logs|tail)
    # Live log from EVO's own server (port 9780), no FTP. Ctrl-C to stop.
    #   evo-remote.sh logs              whole ring, then live
    #   evo-remote.sh logs now          live only
    #   evo-remote.sh logs <text>       only lines containing <text>
    #   evo-remote.sh logs now <text>
    #   evo-remote.sh logs problems     only WARN / ERROR lines (also: now problems)
    Q=""
    if [[ "${1:-}" == "now" ]]; then Q="tail=0"; shift; fi
    if [[ "${1:-}" == "problems" ]]; then Q="${Q:+${Q}&}level=warn"; shift; fi
    if [[ -n "${1:-}" ]]; then Q="${Q:+${Q}&}grep=$(printf '%s' "$1" | sed 's/ /+/g')"; fi
    # Lines read "[seconds] LEVEL text". Shown as "mm:ss.mmm LEVEL text", WARN and
    # ERROR coloured when stdout is a terminal. LOGS_RAW=1 keeps the raw lines.
    if [[ "${LOGS_RAW:-}" == "1" ]]; then
        FMT=(cat)
    else
        FMT=(awk -v color="$([[ -t 1 ]] && echo 1 || echo 0)" '
            match($0, /^\[[0-9]+(\.[0-9]+)?\] (INFO |WARN |ERROR)/) {
                s = $1; gsub(/[\[\]]/, "", s); lv = $2
                m = int(s / 60); sec = s - m * 60
                rest = substr($0, index($0, lv) + length(lv) + 1); sub(/^ +/, "", rest)
                line = sprintf("%02d:%06.3f %-5s %s", m, sec, lv, rest)
                if (color && lv == "ERROR") line = "\033[31m" line "\033[0m"
                else if (color && lv == "WARN") line = "\033[33m" line "\033[0m"
                print line; fflush(); next }
            { print; fflush() }')
    fi
    curl -sN --connect-timeout 5 "http://${PS5_HOST}:9780/raw${Q:+?${Q}}" | "${FMT[@]}" \
        || echo "(no log server on ${PS5_HOST}:9780 - EVO running?)"
    ;;
watch)
    SECS="${1:-180}"
    ftp_py "${SECS}" <<'PY'
import os, sys, time
from ftplib import FTP, error_perm
H, P = os.environ["PS5_HOST"], int(os.environ["FTP_PORT"])
def get(p):
    try:
        with FTP() as f:
            f.connect(H, P, timeout=10); f.login()
            try: f.set_pasv(True)
            except Exception: pass
            b = []
            try: f.retrbinary("RETR " + p, b.append)
            except error_perm: return None
            return b"".join(b).decode("utf-8", "replace")
    except Exception:
        return None
dl = time.time() + int(sys.argv[1]); vlen = 0; last = ""
while time.time() < dl:
    st = get("/mnt/usb0/evo_status")
    if st and st.strip() and st.strip() != last:
        print(st.strip()); last = st.strip()
    v = get("/mnt/usb0/evo.log") or ""
    if len(v) > vlen:
        for ln in v[vlen:].splitlines():
            if ln.strip(): print("  " + ln)
        vlen = len(v)
    time.sleep(3)
PY
    ;;
*)
    sed -n '2,/^# ====/p' "$0"
    ;;
esac
