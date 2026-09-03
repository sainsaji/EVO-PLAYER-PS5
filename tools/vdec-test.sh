#!/usr/bin/env bash
# =============================================================================
# tools/vdec-test.sh — unattended native-decode (sceVideodec2) test cycle.
#
#   PS5_HOST=192.168.0.6 ./tools/vdec-test.sh [/mnt/usb0/clip.mp4] [wait-seconds]
#
#   1. build the player with --autoplay <clip>   (scripts/package-app.sh)
#   2. clear /mnt/usb0/evo_vdec.log              (FTP)
#   3. deploy the .ffpfsc                        (scripts/deploy-app.sh --ffpfsc)
#      -> ShadowMount+ auto-remounts on the image change
#   4. relaunch PPSA99039                        (projects/app_ctl via /hbldr:
#      kill old eboot.bin + sceSystemServiceLaunchApp) — best effort
#   5. poll /mnt/usb0/evo_vdec.log until it stops growing
#   6. print evo_vdec.log + the tail of pp_4k_stage_breadcrumb.txt
#
# The app auto-opens <clip> ~4 s after the menu (main.c EVO_AUTOPLAY_FILE), so
# no controller input is needed. If step 4 can't launch it (title never
# registered via ShadowMount+, or LaunchApp refused), the script says so and
# you launch once from the Games row — everything else still runs.
#
# Default clip: /mnt/usb0/GTAVI_An_Extended_Look.mp4
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

CLIP="${1:-/mnt/usb0/GTAVI_An_Extended_Look.mp4}"
WAIT="${2:-180}"

# Git-Bash on Windows rewrites a leading /mnt/... arg to C:/Program Files/Git/mnt/...
# Undo that — the console path must stay absolute-unix.
case "${CLIP}" in
    */mnt/usb0/*)  CLIP="/mnt/usb0/${CLIP##*/mnt/usb0/}" ;;
    */data/*)      CLIP="/data/${CLIP##*/data/}" ;;
esac
FTP_PORT="${FTP_PORT:-2121}"
WEB_PORT="${PS5_WEB_PORT:-8080}"
LOGFILE="/mnt/usb0/evo_vdec.log"
BCFILE="/mnt/usb0/pp_4k_stage_breadcrumb.txt"
APPCTL_ELF="/data/homebrew/app_ctl/eboot.elf"

if ! in_container; then
    reexec_in_container "../tools/vdec-test.sh" "${CLIP}" "${WAIT}"
fi

require_ps5_host
need_cmd python3 curl make
load_sdk

ftp_py() {   # ftp_py <<'PY' ... PY   — HOST/PORT in env
    PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT}" python3 - "$@"
}

step "1/6  build  (--autoplay ${CLIP})"
"${SCRIPTS_DIR}/package-app.sh" --ffpfsc --autoplay "${CLIP}"

step "2/6  clear ${LOGFILE}"
ftp_py <<'PY'
import os
from ftplib import FTP, error_perm
with FTP() as f:
    f.connect(os.environ["PS5_HOST"], int(os.environ["FTP_PORT"]), timeout=15)
    f.login()
    try: f.set_pasv(True)
    except Exception: pass
    for p in ("/mnt/usb0/evo_vdec.log", "/mnt/usb0/pp_4k_stage_breadcrumb.txt",
              "/mnt/usb0/pp_4k_stage_last.txt", "/mnt/usb0/evo_autoplay_status.txt"):
        try: f.sendcmd(f"DELE {p}"); print("cleared", p)
        except error_perm: print("absent ", p)
PY

step "3/6  deploy .ffpfsc"
"${SCRIPTS_DIR}/deploy-app.sh" --ffpfsc

step "4/6  relaunch PPSA99039"
# sceSystemServiceLaunchApp from /hbldr returns 0x80940005 unless the title is
# live-registered by the ShadowMount+ UI — so remote relaunch is unreliable.
# Try it (harmless); if the log never appears the script tells you to launch.
if make -C "${REPO_ROOT}/projects/app_ctl" >/dev/null 2>&1 && \
   "${SCRIPTS_DIR}/install-homebrew.sh" --name app_ctl \
        "${REPO_ROOT}/projects/app_ctl/app_ctl.elf" >/dev/null 2>&1; then
    curl -sS --max-time 12 --get \
        --data-urlencode "path=${APPCTL_ELF}" \
        --data-urlencode "pipe=1" \
        "http://${PS5_HOST}:${WEB_PORT}/hbldr" 2>/dev/null || true
fi
cat <<EOF

   ########################################################################
   #  If EVO is not already coming up: on the console, ShadowMount+ ->     #
   #  mount PPSA99039 -> launch from the Games row. ONE button press.      #
   #  It auto-opens the clip ~4s after the menu. Then walk away.           #
   ########################################################################

EOF

step "5/6  waiting for playback (up to ${WAIT}s) — clip: ${CLIP}"
ftp_py "${WAIT}" <<'PY'
import os, sys, time
from ftplib import FTP, error_perm

host, port = os.environ["PS5_HOST"], int(os.environ["FTP_PORT"])
deadline = time.time() + int(sys.argv[1])
STATUS = "/mnt/usb0/evo_autoplay_status.txt"

def get(path):
    try:
        with FTP() as f:
            f.connect(host, port, timeout=10); f.login()
            try: f.set_pasv(True)
            except Exception: pass
            buf = []
            try: f.retrbinary(f"RETR {path}", buf.append)
            except error_perm: return None
            return b"".join(buf).decode("utf-8", "replace")
    except Exception:
        return None

seen = False
while time.time() < deadline:
    st = get(STATUS)
    if st:
        if not seen:
            print("   app is alive"); seen = True
        last = st.strip().splitlines()[-1] if st.strip() else ""
        print("   " + last)
        if "DONE" in st or "fatal=1" in st:
            print("   playback ended"); break
    time.sleep(3)
else:
    print("   (timeout — dumping whatever is there)")
PY

step "6/6  results"
mkdir -p "${LOG_OUT}"
OUT="${LOG_OUT}/evo_vdec.log"
BCOUT="${LOG_OUT}/pp_4k_stage_breadcrumb.txt"
STOUT="${LOG_OUT}/evo_autoplay_status.txt"
ftp_py "${OUT}" "${BCOUT}" "${STOUT}" <<'PY'
import os, sys
from ftplib import FTP, error_perm
host, port = os.environ["PS5_HOST"], int(os.environ["FTP_PORT"])
def pull(remote, local):
    try:
        with FTP() as f:
            f.connect(host, port, timeout=15); f.login()
            try: f.set_pasv(True)
            except Exception: pass
            with open(local, "wb") as fh:
                f.retrbinary(f"RETR {remote}", fh.write)
        return True
    except (error_perm, OSError) as e:
        print(f"   ({remote}: {e})"); return False
pull("/mnt/usb0/evo_vdec.log", sys.argv[1])
pull("/mnt/usb0/pp_4k_stage_breadcrumb.txt", sys.argv[2])
pull("/mnt/usb0/evo_autoplay_status.txt", sys.argv[3])
PY

echo ""
echo "=====  evo_autoplay_status.txt (pos should climb; be=1 is NATIVE)  ====="
[[ -s "${STOUT}" ]] && cat "${STOUT}" || echo "(none)"
echo ""
echo "=================  evo_vdec.log  ================="
[[ -s "${OUT}" ]] && cat "${OUT}" || echo "(empty — did the app launch + reach playback?)"
echo ""
echo "=====  pp_4k_stage_breadcrumb.txt (P8_* / AVLOG tail)  ====="
if [[ -s "${BCOUT}" ]]; then
    grep -E 'AUTOPLAY|P8_|VDEC|AVLOG|FIRST_FRAME|VO_RECONFIG|FINISH' "${BCOUT}" | tail -40
else
    echo "(empty)"
fi
echo ""
ok "saved: ${OUT#"${REPO_ROOT}/"}  +  ${BCOUT#"${REPO_ROOT}/"}"
