#!/usr/bin/env bash
# =============================================================================
# scripts/deploy-app.sh - publish the packaged app module over FTP.
#
#   PS5_HOST=192.168.1.50 ./scripts/deploy-app.sh
#   ./scripts/deploy-app.sh --ffpfsc          # upload the PFS image instead
#   ./scripts/deploy-app.sh --undeploy        # remove the staged title (+ image)
#
# Uploads output/app/<TITLE_ID>/ to ftp://<host>:2121/data/homebrew/<TITLE_ID>/.
# Files go up under a temporary name and are renamed into place; eboot.bin and
# sce_sys/param.json are uploaded LAST so a half-finished folder is never
# mountable. After this, mount + launch from the Games row with ShadowMountPlus.
#
# The --ffpfsc deploy also DELETEs /mnt/usb0/{evo.log, evo_status,
# evo_compat_report.txt}, so each launch starts with a fresh log.
#
# This does NOT launch anything - launch safety (never stack launches) is on
# you and ShadowMountPlus. See docs/evo-pro/phase-1b-app-module.md.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ACTION="deploy"
FFPFSC=0
FORCE=0
while (( $# )); do
    case "$1" in
        --undeploy) ACTION="undeploy" ;;
        --ffpfsc)   FFPFSC=1 ;;
        --force)    FORCE=1 ;;
        -h|--help)  sed -n '2,16p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

FTP_PORT="${FTP_PORT:-2121}"

if ! in_container; then
    FWD=()
    [[ "${ACTION}" == undeploy ]] && FWD+=(--undeploy)
    (( FFPFSC )) && FWD+=(--ffpfsc)
    (( FORCE )) && FWD+=(--force)
    reexec_in_container "deploy-app.sh" "${FWD[@]+"${FWD[@]}"}"
fi

need_cmd python3
require_ps5_host

PARAM="${REPO_ROOT}/projects/evoplayer/sce_sys/param.json"
need_file "${PARAM}"
TITLE_ID="$(python3 -c 'import json,sys,re
t=json.load(open(sys.argv[1]))["titleId"]
sys.exit("bad titleId") if not re.fullmatch(r"PPSA\d{5}",t) else print(t)' "${PARAM}")"

APPDIR="${OUTPUT_DIR}/app/${TITLE_ID}"
FFPFSC_IMG="${OUTPUT_DIR}/app/${TITLE_ID}.ffpfsc"

# --- .ffpfsc image path: one file to /data/homebrew/<TITLE_ID>.ffpfsc --------
#
# Refuse to deploy on top of an EVO that has been launched.
#
# Deploying replaces the .ffpfsc that ShadowMountPlus has MOUNTED and that a
# live process has its code pages mapped from, and the file change then makes
# ShadowMountPlus auto-launch - stacking a second instance on the resident one.
# Either of those can panic the console; both have. Closing from the switcher
# is the only thing that frees the slot, and there is no remote equivalent.
#
# A soft close (Settings -> QUIT EVO) is NOT sufficient here and must not be
# treated as if it were: it parks the app and drains the GPU, which makes the
# subsequent switcher-close safe, but the process stays resident with the image
# still mounted.
#
# Two signals, because one was not enough.
#
# A deploy clears /mnt/usb0, so evo.log is absent until EVO next runs: its
# presence means EVO has been launched since, and may still hold the slot.
# That alone cannot tell "running now" from "ran and was closed", so it blocked
# every deploy after the first and --force became reflex - which is exactly the
# habit you do not want when the dangerous case comes along.
#
# evo_status carries a monotonic `t=`, rewritten every frame by a --usb-remote
# build. Sampling it twice says whether EVO is alive right now:
#
#   t advancing        RUNNING  - the panic case. Refused, and --force does not
#                                 override it; there is no reading of "deploy
#                                 over a live process" that ends well.
#   t frozen / absent  IDLE     - closed, or parked by QUIT with the slot still
#                                 held. Indistinguishable from here, so this is
#                                 where --force still applies.
#   evo.log absent     FREE     - nothing has run since the last deploy.
#
check_evo_not_resident() {
    local out
    out="$(python3 - "${PS5_HOST}" "${FTP_PORT}" <<'PY' 2>/dev/null || echo BROKEN
import sys, time
from ftplib import FTP


def verdict(host, port):
    f = FTP()
    f.connect(host, port, timeout=10)
    f.login()
    try:
        lines = []
        f.cwd("/mnt/usb0")
        f.retrlines("LIST", lines.append)
        if not any(" evo.log" in l or l.endswith("evo.log") for l in lines):
            return "ABSENT"

        def stamp():
            buf = []
            try:
                f.retrbinary("RETR /mnt/usb0/evo_status", buf.append)
            except Exception:
                return None
            for tok in b"".join(buf).decode("utf-8", "replace").split():
                if tok.startswith("t="):
                    return tok
            return None

        first = stamp()
        if first is not None:
            time.sleep(4)
            later = stamp()
            if later is not None and later != first:
                return "RUNNING"
        return "PRESENT"
    finally:
        try:
            f.quit()
        except Exception:
            pass


try:
    print(verdict(sys.argv[1], int(sys.argv[2])))
except Exception:
    print("UNREACHABLE")
PY
)"
    case "${out}" in
        ABSENT)  ok "EVO has not run since the last deploy - slot is free" ;;
        RUNNING)
            die "EVO is RUNNING on ${PS5_HOST} right now - its status heartbeat is advancing.

   Deploying now replaces the mounted image under a live process, which has
   kernel-panicked this console. --force does not override this.

   Close it from the switcher (PS button -> close the application) first." ;;
        PRESENT)
            if (( FORCE )); then
                warn "EVO has been launched since the last deploy - --force given, continuing"
            else
                die "EVO has run since the last deploy, and is not running now.

   One case this cannot see: Settings -> QUIT parks the app but does NOT free
   the slot, and a parked EVO looks exactly like a closed one from here. Deploying over a resident EVO replaces the
   mounted image under a live process and stacks an auto-launch on top of it -
   both have kernel-panicked this console.

   Close EVO from the switcher (PS button -> close the application), then
   deploy again. Settings -> QUIT EVO parks it and makes that close safe, but
   does NOT free the slot on its own.

   Override with --force if you know the slot is free."
            fi ;;
        *)
            if (( FORCE )); then
                warn "could not check ${PS5_HOST}:${FTP_PORT} for a resident EVO - --force given, continuing"
            else
                die "could not check ${PS5_HOST}:${FTP_PORT} for a resident EVO.

   Not reaching the console is not evidence that the app slot is free, and
   this check failing open is how a deploy once landed on top of a running
   EVO. Fix the connection, or pass --force if you know the slot is free."
            fi ;;
    esac
}

if (( FFPFSC )) && [[ "${ACTION}" == "deploy" ]]; then
    need_file "${FFPFSC_IMG}" "run ./scripts/package-app.sh --ffpfsc first"
    require_ps5_host
    check_evo_not_resident
    begin "deploy ${TITLE_ID}.ffpfsc -> ftp://${PS5_HOST}:${FTP_PORT}/data/homebrew/"
    python3 - "${PS5_HOST}" "${FTP_PORT}" "${TITLE_ID}" "${FFPFSC_IMG}" <<'PY'
import sys, time
from ftplib import FTP, error_perm
from posixpath import join

host, port, title_id, img = sys.argv[1:]
root = "/data/homebrew"
remote = join(root, f"{title_id}.ffpfsc")
folder = join(root, title_id)
tmp = remote + ".upload"


def rmtree(ftp, path):
    """ShadowMount+ ignores a .ffpfsc when a same-TITLE_ID folder exists, so
    the loose folder must go before the image is served."""
    try:
        ftp.sendcmd(f"DELE {path}"); return
    except error_perm:
        pass
    try:
        prev = ftp.pwd(); ftp.cwd(path)
        names = [n for n, _ in ftp.mlsd() if n not in (".", "..")]
        ftp.cwd(prev)
    except error_perm:
        return
    for n in names:
        rmtree(ftp, join(path, n))
    try:
        ftp.rmd(path)
    except error_perm:
        pass


# The runtime files the app module writes - cleared on each deploy so a launch
# always starts fresh (evo-remote.sh / evo-panel read these back).
USB_LOGS = ["evo.log", "evo_status", "evo_compat_report.txt"]

with FTP() as ftp:
    ftp.connect(host, int(port), timeout=15)
    ftp.login()
    try: ftp.set_pasv(True)
    except Exception: pass
    rmtree(ftp, folder)               # kill any stale loose folder for this TID
    print(f"cleared {folder} (if present)")
    cleared = 0
    for name in USB_LOGS:
        try: ftp.sendcmd(f"DELE /mnt/usb0/{name}"); cleared += 1
        except error_perm: pass
    print(f"cleared {cleared} stale /mnt/usb0 log file(s)")
    for path in (tmp, remote):
        try: ftp.sendcmd(f"DELE {path}")
        except error_perm: pass
    with open(img, "rb") as fh:
        ftp.storbinary(f"STOR {tmp}", fh, blocksize=256 * 1024)
    ftp.rename(tmp, remote)
    print(f"done: ftp://{host}:{port}{remote}")
PY

    # #60: the .ffpfsc is self-contained now - RmlUi's .rml/.rcss/.ttf/.png
    # assets are embedded in the binary (evo_rmlui_bundle_data.cpp) instead of
    # being read from disk at runtime, so the #44 out-of-band FTP push to
    # /data/evoplayer/app/assets/ is no longer needed. This block used to wipe
    # and re-upload that loose tree on every deploy; deleting
    # /data/evoplayer/app/assets/ on the console now has zero effect on the UI.
    ok "deploy complete"
    echo "   Mount + launch from the Games row (ShadowMountPlus). Never stack launches."
    exit 0
fi

if [[ "${ACTION}" == "deploy" ]]; then
    need_file "${APPDIR}/eboot.bin" "run ./scripts/package-app.sh --probe first"
    need_file "${APPDIR}/sce_sys/param.json"
fi

begin "${ACTION} ${TITLE_ID}  ->  ftp://${PS5_HOST}:${FTP_PORT}/data/homebrew/${TITLE_ID}/"
python3 - "${PS5_HOST}" "${FTP_PORT}" "${TITLE_ID}" "${APPDIR}" "${ACTION}" <<'PY'
import sys, time
from ftplib import FTP, error_perm
from pathlib import Path
from posixpath import join, dirname

host, port, title_id, appdir, action = sys.argv[1:]
root = "/data/homebrew"
target = join(root, title_id)


def code(e):        return str(e).split(maxsplit=1)[0]


def ensure_dir(ftp, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except error_perm as e:
            if code(e) != "550":
                raise


def rm(ftp, path):
    """Recursively remove a file or directory; tolerate absence."""
    try:
        ftp.sendcmd(f"DELE {path}")
        return True
    except error_perm as e:
        msg = str(e).lower()
        if code(e) == "550" and ("no such" in msg or "not found" in msg):
            return False
        # DELE refused (likely a non-empty directory) - recurse.
    try:
        prev = ftp.pwd()
        ftp.cwd(path)
        names = [n for n, _ in ftp.mlsd() if n not in (".", "..")]
        ftp.cwd(prev)
    except error_perm:
        return False
    for n in names:
        rm(ftp, join(path, n))
    try:
        ftp.rmd(path)
    except error_perm:
        pass
    return True


def upload_atomic(ftp, local, remote):
    d = dirname(remote)
    tmp = join(d, "." + remote.rsplit("/", 1)[-1] + ".upload")
    ensure_dir(ftp, d)
    rm(ftp, tmp)
    with open(local, "rb") as fh:
        ftp.storbinary(f"STOR {tmp}", fh, blocksize=256 * 1024)
    rm(ftp, remote)
    ftp.rename(tmp, remote)


with FTP() as ftp:
    ftp.connect(host, int(port), timeout=15)
    ftp.login()
    try:
        ftp.set_pasv(True)
    except Exception:
        pass

    if action == "undeploy":
        removed = rm(ftp, target)
        img_removed = rm(ftp, target + ".ffpfsc")
        if removed or img_removed:
            print(f"removed {target}{' + .ffpfsc' if img_removed else ''}")
        else:
            print(f"nothing staged for {title_id}")
        sys.exit(0)

    app = Path(appdir)
    critical = [app / "eboot.bin", app / "sce_sys/param.json"]
    files = sorted(p for p in app.rglob("*") if p.is_file() and p not in critical)
    files += [p for p in critical if p.is_file()]

    print(f"publishing {len(files)} files (eboot.bin + param.json last)")
    for i, local in enumerate(files, 1):
        rel = local.relative_to(app).as_posix()
        print(f"  [{i}/{len(files)}] {rel}")
        upload_atomic(ftp, local, join(target, rel))

    prev = ftp.pwd(); ftp.cwd(target)
    listed = {n for n, _ in ftp.mlsd()}
    ftp.cwd(prev)
    if "eboot.bin" not in listed:
        raise SystemExit("upload finished but eboot.bin is not listed")
    print(f"done: ftp://{host}:{port}{target}/")
PY

ok "${ACTION} complete"
[[ "${ACTION}" == deploy ]] && echo "   Mount + launch from the Games row (ShadowMountPlus). Never stack launches."
