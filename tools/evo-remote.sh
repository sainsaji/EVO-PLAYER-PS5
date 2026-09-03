#!/usr/bin/env bash
# =============================================================================
# tools/evo-remote.sh — scriptable dev remote for the PPSA99039 app module.
#
# One deployed build (built with --usb-remote) is driven over FTP with no
# controller and no rebuild between tests. It writes /mnt/usb0/evo_status once
# a second and reads /mnt/usb0/evo_cmd for commands; evo_vdec_native.c's note()
# also appends to /mnt/usb0/evo_vdec.log.
#
#   PS5_HOST=192.168.0.6 ./tools/evo-remote.sh <subcommand>
#
#   build [--probe|--videodec2-probe ...]   package --usb-remote + deploy .ffpfsc
#   kill                                    SIGKILL the running eboot (app_ctl)
#   play <path>                             open <path> from the start
#   seek <sec> | seek +<sec> | seek -<sec>  seek
#   status                                  print /mnt/usb0/evo_status once
#   boot                                    print evo_boot.log (pre-unjail probe results)
#   watch [seconds]                         stream evo_status + new evo_vdec.log lines
#   log                                     pull evo_boot.log + evo_vdec.log + breadcrumb tail
#   clear                                   delete the USB status / log / breadcrumb files
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
USB_VDEC="/mnt/usb0/evo_vdec.log"
USB_BOOT="/mnt/usb0/evo_boot.log"
USB_BC="/mnt/usb0/pp_4k_stage_breadcrumb.txt"

case "${SUB}" in
build)
    "${SCRIPTS_DIR}/package-app.sh" --ffpfsc --usb-remote "$@"
    del_files "${USB_STATUS}" "${USB_VDEC}" "${USB_BOOT}" "${USB_BC}" \
              /mnt/usb0/pp_4k_stage_last.txt || true
    "${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc
    echo ""
    echo "  >>> launch PPSA99039 from the Games row (ShadowMount+ remounted) <<<"
    ;;
kill)
    make -C "${REPO_ROOT}/projects/app_ctl" >/dev/null 2>&1 && \
      "${SCRIPTS_DIR}/install-homebrew.sh" --name app_ctl \
        "${REPO_ROOT}/projects/app_ctl/app_ctl.elf" >/dev/null 2>&1 && \
      curl -sS --max-time 12 --get \
        --data-urlencode "path=/data/homebrew/app_ctl/eboot.elf" \
        --data-urlencode "args=kill" --data-urlencode "pipe=1" \
        "http://${PS5_HOST}:${WEB_PORT}/hbldr" 2>/dev/null || true
    echo "kill fired (relaunch from the Games row)"
    ;;
play)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh play <path>"; put_cmd "play $1" ;;
seek)   [[ -n "${1:-}" ]] || die "usage: evo-remote.sh seek <sec|+sec|-sec>"; put_cmd "seek $1" ;;
clear)  del_files "${USB_STATUS}" "${USB_VDEC}" "${USB_BOOT}" "${USB_BC}" /mnt/usb0/pp_4k_stage_last.txt ;;
status) get_file "${USB_STATUS}" || echo "(no evo_status — launched? built --usb-remote?)" ;;
boot)   get_file "${USB_BOOT}"   || echo "(no evo_boot.log — launched? sandbox open?)" ;;
log)
    mkdir -p "${LOG_OUT}"
    echo "=== evo_boot.log (pre-unjail probes) ==="
    get_file "${USB_BOOT}" 2>/dev/null | tee "${LOG_OUT}/evo_boot.log" || true
    echo "=== evo_vdec.log ==="
    get_file "${USB_VDEC}" 2>/dev/null | tee "${LOG_OUT}/evo_vdec.log" || true
    echo "=== breadcrumb tail ==="
    get_file "${USB_BC}" 2>/dev/null | grep -E 'REMOTE|SEEK|FLUSH|VDEC|012_|009_|AVLOG|RECONFIG|FINISH' | tail -40 || true
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
    v = get("/mnt/usb0/evo_vdec.log") or ""
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
