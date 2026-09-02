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
# This does NOT launch anything - launch safety (never stack launches) is on
# you and ShadowMountPlus. See docs/evo-pro/phase-1b-app-module.md.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ACTION="deploy"
FFPFSC=0
while (( $# )); do
    case "$1" in
        --undeploy) ACTION="undeploy" ;;
        --ffpfsc)   FFPFSC=1 ;;
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
if (( FFPFSC )) && [[ "${ACTION}" == "deploy" ]]; then
    need_file "${FFPFSC_IMG}" "run ./scripts/package-app.sh --ffpfsc first"
    require_ps5_host
    begin "deploy ${TITLE_ID}.ffpfsc -> ftp://${PS5_HOST}:${FTP_PORT}/data/homebrew/"
    python3 - "${PS5_HOST}" "${FTP_PORT}" "${TITLE_ID}" "${FFPFSC_IMG}" <<'PY'
import sys
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


with FTP() as ftp:
    ftp.connect(host, int(port), timeout=15)
    ftp.login()
    try: ftp.set_pasv(True)
    except Exception: pass
    rmtree(ftp, folder)               # kill any stale loose folder for this TID
    print(f"cleared {folder} (if present)")
    for path in (tmp, remote):
        try: ftp.sendcmd(f"DELE {path}")
        except error_perm: pass
    with open(img, "rb") as fh:
        ftp.storbinary(f"STOR {tmp}", fh, blocksize=256 * 1024)
    ftp.rename(tmp, remote)
    print(f"done: ftp://{host}:{port}{remote}")
PY
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
import sys
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
