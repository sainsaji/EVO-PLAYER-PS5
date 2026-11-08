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
#   launch [--no-sm-restart]                restart ShadowMount (smrestart: its
#                                           launch hook serves one launch), then
#                                           launch PPSA99039 with no controller
#                                           (title-aware controller -> elfldr
#                                           :9021). Refused while ShadowMount
#                                           shows it resident or the heartbeat
#                                           is live - never stacks a launch
#   smrestart                               re-send ShadowMount+ v1.7beta4
#                                           (output/shadowmountplusb4.elf, sha-
#                                           pinned) to elfldr and wait for its
#                                           "hooks installed ... launch=1". A
#                                           deploy kills the launch hook
#                                           (0x80940033); deploy-app.sh --ffpfsc
#                                           runs this itself. Refused while EVO
#                                           is running or resident
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
#   verify [--check <name>]... [--no-build] [--clip <path>]  #201: one cycle that ends in a hardware
#                                           check (tools/checks/<name>.sh) instead
#                                           of a timed wait: soft close, close at
#                                           parked=1, package + deploy, launch, run
#                                           the check(s), quit + close, print
#                                           PASS/FAIL and the evidence line. No
#                                           --check = every script in tools/checks/.
#                                           Same rules as cycle: first anomaly stops
#                                           it, never retries
#   kill                                    = quit + close
#   play <path>                             open <path> from the start
#   play --sample                           the 20 s test clip in /data/evoplayer/test
#                                           (sample-install puts it there)
#   play --share <share id> <path>          a file on a saved SMB/FTP server, through
#                                           the URL Browse builds (play --share
#                                           ps5_local /data/Media/x.mkv)
#   sample-install [file]                   upload output/sample/evo_sample.mkv (made
#                                           on the host by tools/make_sample.sh)
#   logq <text> [lines]                     evo.log lines with <text>, from the log
#                                           server (:9780), FTP only as a fallback
#   seek <sec> | seek +<sec> | seek -<sec>  seek
#   stop                                    end playback, back to the browser
#   go <name>                               jump to any screen by name: home browse
#                                           internal favorites recent providers
#                                           settings[-playback|-subtitles|-interface|
#                                           -system|-audio|-experimental] about
#                                           changelog themes surround devtools, a
#                                           provider id, or a ScreenId number
#   set <setting> <value>                   #201: write a setting, apply it live, print it
#                                           back (set audio-delay +100). A name with
#                                           no value prints the current one;
#                                           `set --list` prints every name and its
#                                           valid values. Names = the row title,
#                                           lower case, dashes (volume-boost)
#   focus <row>                             open the row's settings section and put
#                                           the cursor on it (a name from set --list)
#   provider <id>                           open a provider's own screen directly
#                                           (iptv xtream emby jellyfin plex stremio
#                                           nuvio), no D-pad walk from home
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
#   kbclose                                 cancel any open keyboard, the native IME too
#   weburl <url>                            Web section: open <url> in its tab
#                                           (data: URLs too) - engine tests
#                                           without typing an address
#   pointer <x> <y>                         Web section: move the free pointer to
#                                           (x, y) in 1920x1080 UI pixels (rail
#                                           included) - the left stick, which
#                                           `key` cannot send. `key cross` then
#                                           clicks there
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
#   dbcompare                               #119: pause, capture the same frame with
#                                           Deep Blacks Off/Low/High -> output/dbcompare/
#   upcompare [--crop x,y,w,h]            #103: pause, capture the same frame
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
# Protocol, safety rules, outcome classes: docs/console/hardware-cycle.md
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
    cmd_bytes = (sys.argv[1] + "\n").encode()
    sent = False
    for p in ["/data/evoplayer/evo_cmd", "/mnt/usb0/evo_cmd"]:
        try:
            f.storbinary("STOR " + p, io.BytesIO(cmd_bytes))
            sent = True
            break
        except Exception:
            pass
    if not sent:
        sys.exit(1)
print("sent:", sys.argv[1])
PY
}

get_file() {  # get_file <remote> -> stdout
    ftp_py "$1" <<'PY'
import os, sys
from ftplib import FTP, error_perm
paths = [sys.argv[1]]
if sys.argv[1].startswith("/mnt/usb0/"):
    alt = sys.argv[1].replace("/mnt/usb0/", "/data/evoplayer/")
    if "evo_status" in sys.argv[1] or "evo_ui" in sys.argv[1]:
        paths = [alt, sys.argv[1]]
    else:
        paths.append(alt)
try:
    with FTP() as f:
        f.connect(os.environ["PS5_HOST"], int(os.environ["FTP_PORT"]), timeout=15)
        f.login()
        try: f.set_pasv(True)
        except Exception: pass
        for p in paths:
            buf = []
            try:
                f.retrbinary("RETR " + p, buf.append)
                sys.stdout.write(b"".join(buf).decode("utf-8", "replace"))
                sys.exit(0)
            except error_perm:
                continue
        sys.exit(3)
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
        targets = [p]
        if p.startswith("/mnt/usb0/"):
            targets.append(p.replace("/mnt/usb0/", "/data/evoplayer/"))
        for t in targets:
            try: f.sendcmd("DELE " + t); print("del", t)
            except error_perm: print("--", t)
PY
}

# ask_set "<set|focus ...>": send it, then wait (max ~10 s) for the app to write
# its answer to evo_set.txt and print it. Exits 1 on an ERR answer or no answer.
ask_set() {
    local out="" i
    del_files /mnt/usb0/evo_set.txt >/dev/null 2>&1 || true
    put_cmd "$1" >/dev/null
    for i in $(seq 1 20); do
        sleep 0.5
        out="$(get_file /mnt/usb0/evo_set.txt 2>/dev/null)" && [[ -n "${out}" ]] && break
        out=""
    done
    [[ -n "${out}" ]] || die "no answer from EVO (running? built with --usb-remote?)"
    echo "${out}"
    [[ "${out}" != ERR* ]]
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

# ShadowMount+ restart. Every deploy (the image check that remounts the new
# .ffpfsc) leaves ShadowMount's launch hook dead: launchApp then answers
# 0x80940033 and no [SHELLCORE] line for the title appears, though its scanner
# keeps logging. Re-sending ShadowMount to elfldr stops the old instance
# ("[RESTART]") and installs the hooks again. deploy-app.sh --ffpfsc runs this
# itself. Only v1.7beta4 - the build the console runs - is accepted: the other
# ShadowMount ELFs are different versions.
SM_ELF="${SHADOWMOUNT_ELF:-${OUTPUT_DIR}/shadowmountplusb4.elf}"
SM_SHA="fc4e5f715e76660ce34bde2adb10eaf1d069cb2cdf06f7a380896dfb62bd0c4a"

do_smrestart() {
    local slot hb base rc=0
    [[ -f "${SM_ELF}" ]] || die "no ${SM_ELF}.
   Copy ShadowMount+ v1.7beta4 (shadowmountplusb4.elf) there, or set SHADOWMOUNT_ELF."
    [[ "$(sha256sum "${SM_ELF}" | cut -d' ' -f1)" == "${SM_SHA}" ]] \
        || die "${SM_ELF} is not ShadowMount+ v1.7beta4 (sha256 differs) - only b4 matches the console's."
    hb="$(lc heartbeat)"
    [[ "${hb}" == RUNNING ]] && die "EVO is running - quit + close it before restarting ShadowMount."
    slot="$(lc slot)"
    [[ "${slot}" == RESIDENT ]] && die "ShadowMount shows ${TITLE_ID} started and not released - close it first."
    base="$(lc smlen)" || die "could not read ShadowMount's log"
    # A resident payload never closes the socket: exit 124 (timeout) is normal.
    timeout --signal=TERM 15s nc -q0 "${PS5_HOST}" "${ELF_PORT}" < "${SM_ELF}" > /dev/null 2>&1 || rc=$?
    (( rc == 0 || rc == 124 )) || die "elfldr ${PS5_HOST}:${ELF_PORT} did not take ShadowMount (nc exit ${rc})"
    echo "sent: ShadowMount+ v1.7beta4 -> ${PS5_HOST}:${ELF_PORT}"
    [[ "$(lc wait-hooks "${base}" 45)" == HOOKED ]] \
        || die "no '[SHELLCORE] hooks installed ... launch=1' within 45 s of the restart. STOP - check the console."
    ok "ShadowMount restarted (launch hook installed)"
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
    # ShadowMount's launch hook serves one launch: after a deploy, and after
    # EVO has run and closed, the next launch gets 0x80940033 (2026-11-05). So
    # every launch restarts it first, then waits out the startup scan.
    if [[ "${1:-}" != --no-sm-restart ]]; then
        ( do_smrestart ) || die "ShadowMount restart failed - not launching."
        [[ "$(lc wait-quiet 15 120)" == QUIET ]] \
            || die "ShadowMount's log is still moving 120 s after its restart. Not launching."
    fi
    base="$(lc smlen)" || die "could not read ShadowMount's log"
    # Another ShadowMount game still running means the PS5 answers the launch
    # with a dialog no script can clear. Said up front, not after 30 s.
    local other
    other="$(lc foreign 2>/dev/null || echo NONE)"
    if [[ "${other}" != NONE ]]; then
        die "another app is running (${other}: started, not released in ShadowMount's log).
   Close it on the PS5 first - a launch now gets a dialog nothing here can answer."
    fi
    del_files "${USB_STATUS}" >/dev/null || true
    local sent code
    sent="$(send_controller launch 2>&1)" || { printf '%s\n' "${sent}"; exit 1; }
    printf '%s\n' "${sent}"
    # The system's own answer, echoed back by the controller (the getAppStatus
    # 0x80940004 line before it is chatter). Quote it rather than guess.
    # (No launchApp line on a clean launch: grep finding nothing must not end the
    # script under set -e/pipefail.)
    code="$(printf '%s\n' "${sent}" | grep -oE 'launchApp: LNC_ISOK::0x[0-9a-fA-F]+' | tail -1 | sed 's/.*:://' || true)"
    if [[ "$(lc wait-started "${base}" 30)" != STARTED ]]; then
        [[ "${code,,}" == 0x80940033 ]] && die "launch did not take: launchApp returned ${code} - ShadowMount's launch hook is dead
   even after the restart. Look at /data/shadowmount/debug.log before anything else.
   Never retry automatically."
        die "no '[GAME] started: ${TITLE_ID}' within 30 s - launch did not take${code:+ (launchApp returned ${code})}.
   Look at the TV: if \"Can't start game or app\" is showing, press X on the pad to clear it
   (nothing here can), or start EVO from the home screen. Never retry automatically."
    fi
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
    local secs=60 play="" build=1 pkg=() checks=() check_fail="" check_pass=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --secs)     secs="$2"; shift 2 ;;
            --play)     play="$2"; shift 2 ;;
            --no-build) build=0; shift ;;
            --check)    checks+=("$2"); shift 2 ;;
            --)         shift; pkg=("$@"); break ;;
            *) die "usage: evo-remote.sh cycle [--secs n] [--play <path>] [--no-build] [--check <name>]... [-- package-app args]" ;;
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
    if (( ${#checks[@]} )); then
        # #201: a hardware check replaces the fixed wait. A FAIL is an outcome,
        # not an anomaly: the teardown below still runs, so the slot is left clean.
        local c line
        for c in "${checks[@]}"; do
            [[ -x "${REPO_ROOT}/tools/checks/${c}.sh" ]] || { check_fail="no such check: ${c}"; break; }
            line="$(timeout 180 "${REPO_ROOT}/tools/checks/${c}.sh" 2>&1 | tee -a "${dir}/check.log" | tail -1)" || true
            echo "${line}"
            if [[ "${line}" == PASS* ]]; then check_pass="${check_pass:+${check_pass}; }${line}"
            else check_fail="${line:-${c}: no output (timed out?)}"; break; fi
        done
    else
        [[ -n "${play}" ]] && { sleep 5; put_cmd "play ${play}" > /dev/null; }
        sleep "${secs}"
    fi
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
    [[ -n "${check_fail}" ]] && finish failed "check ${check_fail}"
    finish pass "${check_pass:--}"
}

# verify [--check <name>]: the whole hardware loop for one change, ending in a
# check's PASS/FAIL line. No --check runs every script in tools/checks/.
do_verify() {
    local args=() named=0 f
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --check)    args+=(--check "$2"); named=1; shift 2 ;;
            --no-build) args+=(--no-build); shift ;;
            --clip)     export EVO_CHECK_CLIP="$2"; shift 2 ;;
            *) die "usage: evo-remote.sh verify [--check <name>]... [--no-build] [--clip <usb path>]" ;;
        esac
    done
    if (( ! named )); then
        for f in "${REPO_ROOT}"/tools/checks/*.sh; do
            [[ -e "${f}" ]] && args+=(--check "$(basename "${f}" .sh)")
        done
        (( ${#args[@]} )) || die "no checks in tools/checks/"
    fi
    do_cycle "${args[@]}"
}

# --- test media + log queries -------------------------------------------------
SAMPLE_REMOTE="/data/evoplayer/test/evo_sample.mkv"   # EVO's own data dir: always readable
SAMPLE_LOCAL="${OUTPUT_DIR}/sample/evo_sample.mkv"    # tools/make_sample.sh (host, ffmpeg)

do_play() {
    case "${1:-}" in
        --sample) put_cmd "play ${SAMPLE_REMOTE}" ;;
        --share)
            [[ -n "${2:-}" && -n "${3:-}" ]] || die "usage: evo-remote.sh play --share <share id> <path>"
            put_cmd "playshare $2 $3" ;;
        "") die "usage: evo-remote.sh play <path> | --sample | --share <share id> <path>" ;;
        *) put_cmd "play $1" ;;
    esac
}

do_sample_install() {
    local src="${1:-${SAMPLE_LOCAL}}"
    [[ -f "${src}" ]] || die "no ${src#"${REPO_ROOT}/"} - make it on the host first: tools/make_sample.sh"
    timeout 120 curl -sS --ftp-create-dirs -T "${src}" \
        "ftp://${PS5_HOST}:${FTP_PORT}${SAMPLE_REMOTE}" || die "upload to ${SAMPLE_REMOTE} failed"
    ok "installed ${SAMPLE_REMOTE} - play it with: evo-remote.sh play --sample"
}

# logq <text> [lines]: evo.log lines containing <text>, newest last. Asks EVO's
# log server (:9780/snapshot - the in-memory ring, and it closes; /raw stays
# open by design) and falls back to fetching the whole evo.log over FTP only
# when EVO is not answering.
do_logq() {
    local q="${1:?usage: evo-remote.sh logq <text> [lines]}" n="${2:-40}" out enc
    enc="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "${q}")"
    if out="$(timeout 8 curl -sf --max-time 6 "http://${PS5_HOST}:${PS5_LOG_PORT:-9780}/snapshot?grep=${enc}")"; then
        printf '%s\n' "${out}" | tail -n "${n}"
    else
        warn "log server not answering - reading evo.log over FTP"
        get_file "${USB_LOG}" | grep -F -- "${q}" | tail -n "${n}" || true
    fi
}

case "${SUB}" in
build)
    "${SCRIPTS_DIR}/package-app.sh" --ffpfsc --usb-remote "$@"
    del_files "${USB_STATUS}" "${USB_LOG}" || true
    "${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc
    echo ""
    echo "  >>> launch PPSA99039 from the Games row (ShadowMount+ remounted) <<<"
    ;;
launch) do_launch "${1:-}" ;;
smrestart) do_smrestart ;;
quit)   do_quit ;;
close)  do_close "${1:-}" ;;
kill)   do_quit; do_close ;;
cycle)  do_cycle "$@" ;;
verify) do_verify "$@" ;;
play)   do_play "$@" ;;
sample-install) do_sample_install "$@" ;;
logq)   do_logq "$@" ;;
seek)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh seek <sec|+sec|-sec>"; put_cmd "seek $1" ;;
stop)   put_cmd "stop" ;;
key)    [[ -n "${1:-}" ]] || die "usage: evo-remote.sh key <button>...  (e.g. cross, up, l1, l3)"
        if [[ "${1}" == --no-wait ]]; then shift; put_cmd "key $1"
        else evo_ui key "$@"; fi ;;
ui)     evo_ui show "$@" ;;
set)    [[ -n "${1:-}" ]] || die "usage: evo-remote.sh set <setting> <value> | set --list   (a name with no value prints the current one)"
        ask_set "set $*" ;;
focus)  [[ -n "${1:-}" ]] || die "usage: evo-remote.sh focus <row>   (a name from set --list)"
        ask_set "focus $1" ;;
screen) [[ "${1:-}" =~ ^[0-9]+$ ]] || die "usage: evo-remote.sh screen <id>  (an evo::ScreenId number)"; put_cmd "screen $1" ;;
# go <name>: one door to any screen, by name instead of a ScreenId number.
go)     case "${1:-}" in
          home)                put_cmd "screen 0" ;;
          browse|usb)          put_cmd "source 0" ;;
          internal)            put_cmd "source 1" ;;
          # BrowserScreen sources: 0 USB, 1 Internal, 2 Network, 3 Favorites, 4 Recent
          network)             put_cmd "source 2" ;;
          favorites)           put_cmd "source 3" ;;
          recent)              put_cmd "source 4" ;;
          providers|chooser)   put_cmd "screen 23" ;;
          iptv|xtream|emby|jellyfin|plex|stremio|nuvio) put_cmd "provider $1" ;;
          settings)            put_cmd "screen 10" ;;
          settings-playback)   put_cmd "screen 24" ;;
          settings-subtitles)  put_cmd "screen 25" ;;
          settings-interface)  put_cmd "screen 26" ;;
          settings-system)     put_cmd "screen 27" ;;
          settings-audio)      put_cmd "screen 33" ;;
          settings-experimental) put_cmd "screen 34" ;;
          about)               put_cmd "screen 14" ;;
          changelog)           put_cmd "screen 19" ;;
          themes)              put_cmd "screen 29" ;;
          surround)            put_cmd "screen 28" ;;
          devtools)            put_cmd "screen 15" ;;
          [0-9]*)              put_cmd "screen $1" ;;
          *) die "usage: evo-remote.sh go <home|browse|internal|network|favorites|recent|providers|iptv|xtream|emby|jellyfin|plex|stremio|nuvio|settings[-playback|-subtitles|-interface|-system|-audio|-experimental]|about|changelog|themes|surround|devtools|<ScreenId>>" ;;
        esac ;;
provider) [[ -n "${1:-}" ]] || die "usage: evo-remote.sh provider <id>  (iptv xtream emby jellyfin plex stremio nuvio)"; put_cmd "provider $1" ;;
source) [[ "${1:-}" =~ ^[0-9]+$ ]] || die "usage: evo-remote.sh source <n>  (0 USB, 1 Internal, 2 Favorites, 3 Recent)"; put_cmd "source $1" ;;
iobench) put_cmd "iobench ${1:-}" ;;
ptprobe) [[ -n "${1:-}" ]] || die "usage: evo-remote.sh ptprobe info | mode <n> [target] | reset | stream <pt|main> <secs> <path> | sweep <path> | sony <path>"; put_cmd "ptprobe $*" ;;
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
kbclose) put_cmd "kbclose" ;;
weburl) [[ -n "${1:-}" ]] || die "usage: evo-remote.sh weburl <url>  (opens in the Web section; data: URLs work)"
        put_cmd "weburl $*" ;;
pointer) [[ "${1:-}" =~ ^[0-9]+$ && "${2:-}" =~ ^[0-9]+$ ]] \
            || die "usage: evo-remote.sh pointer <x> <y>  (1920x1080 UI pixels, rail included; then key cross to click)"
         put_cmd "pointer $1 $2" ;;
upcompare)
    # #103: same paused frame with the upscaler Off / Sharp / AI, no OSD ->
    # output/upcompare/{off,sharp,ai}.bmp (+ compare.png where Pillow exists).
    PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}"     python3 "$(dirname "${BASH_SOURCE[0]}")/upcompare_run.py" "$@" || die "upcompare failed"
    ;;
dbcompare)
    # #119: same paused frame with Deep Blacks Off / Low / High, no OSD ->
    # output/dbcompare/{off,low,high}.bmp + compare.png / shadows.png + stats.
    PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" python3 "$(dirname "${BASH_SOURCE[0]}")/dbcompare_run.py" "$@" || die "dbcompare failed"
    ;;
sweep)
    # #8 — the codec sweep. Plays every clip in a directory for a fixed window,
    # then harvests the per-file `sweep` lines EVO wrote into evo.log and renders
    # the docs/build/validation.md table. Each `play` implicitly closes the previous
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
    echo "  table -> output/logs/sweep.md   (paste into docs/build/validation.md)"
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
    # Live log from EVO's own server (port 9780, or PS5_LOG_PORT), no FTP. Ctrl-C to stop.
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
    curl -sN --connect-timeout 5 "http://${PS5_HOST}:${PS5_LOG_PORT:-9780}/raw${Q:+?${Q}}" | "${FMT[@]}" \
        || echo "(no log server on ${PS5_HOST}:${PS5_LOG_PORT:-9780} - EVO running?)"
    ;;
watch)
    SECS="${1:-180}"
    ftp_py "${SECS}" <<'PY'
import os, sys, time
from ftplib import FTP, error_perm
H, P = os.environ["PS5_HOST"], int(os.environ["FTP_PORT"])
def get(p):
    try:
        paths = [p]
        if p.startswith("/mnt/usb0/"):
            paths.append(p.replace("/mnt/usb0/", "/data/evoplayer/"))
        with FTP() as f:
            f.connect(H, P, timeout=10); f.login()
            try: f.set_pasv(True)
            except Exception: pass
            for tgt in paths:
                b = []
                try:
                    f.retrbinary("RETR " + tgt, b.append)
                    return b"".join(b).decode("utf-8", "replace")
                except error_perm:
                    pass
            return None
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
