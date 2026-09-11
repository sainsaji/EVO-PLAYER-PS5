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
#   kill                                    SIGKILL the running eboot (app_ctl)
#   play <path>                             open <path> from the start
#   seek <sec> | seek +<sec> | seek -<sec>  seek
#   stop                                    end playback, back to the browser
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
# The one thing this can't do: launch the title (sceSystemServiceLaunchApp from
# a payload returns 0x80940005). After `build` / `kill`, launch once from the
# Games row via ShadowMount+; everything else is hands-off.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

SUB="${1:-}"; shift || true
FTP_PORT="${FTP_PORT:-2121}"
WEB_PORT="${PS5_WEB_PORT:-8080}"

if ! in_container; then
    reexec_in_container "../tools/evo-remote.sh" "${SUB}" "$@"
fi
require_ps5_host
need_cmd python3 curl

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

case "${SUB}" in
build)
    "${SCRIPTS_DIR}/package-app.sh" --ffpfsc --usb-remote "$@"
    del_files "${USB_STATUS}" "${USB_LOG}" || true
    "${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc
    echo ""
    echo "  >>> launch PPSA99039 from the Games row (ShadowMount+ remounted) <<<"
    ;;
kill)
    # The old app_ctl /hbldr helper is gone with the ELF-push scripts
    # (2026-09-03). PS-button close on the console is the only reliable way to
    # free the app slot; ShadowMount+ re-mounts + auto-launches on the next
    # `evo-remote.sh build`.
    die "no remote kill. PS-button-close EVO on the console, then re-deploy."
    ;;
play)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh play <path>"; put_cmd "play $1" ;;
seek)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh seek <sec|+sec|-sec>"; put_cmd "seek $1" ;;
stop)   put_cmd "stop" ;;
sweep)
    # #8 — the codec sweep. Plays every clip in a directory for a fixed window,
    # then harvests the per-file `sweep` lines EVO wrote into evo.log and renders
    # the docs/validation.md table. Each `play` implicitly closes the previous
    # file (start_video_playback stops first), which is what flushes its row; the
    # trailing `stop` flushes the last one.
    SWEEP_DIR="/mnt/usb0/test_files_aud_vid"
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
clear)  del_files "${USB_STATUS}" "${USB_LOG}" ;;
status) get_file "${USB_STATUS}" || echo "(no evo_status — launched? built --usb-remote?)" ;;
boot|log)
    mkdir -p "${LOG_OUT}"
    get_file "${USB_LOG}" 2>/dev/null | tee "${LOG_OUT}/evo.log" \
        || echo "(no evo.log — launched? sandbox open?)"
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
    sed -n '2,30p' "$0"
    ;;
esac
