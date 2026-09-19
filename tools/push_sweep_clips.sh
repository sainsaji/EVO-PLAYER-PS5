#!/usr/bin/env bash
# =============================================================================
# tools/push_sweep_clips.sh — upload the generated #8 corpus to /mnt/usb0.
#
#   PS5_HOST=192.168.0.7 ./tools/push_sweep_clips.sh [srcdir]
#
# Skips anything already on the console at the same size, so a re-run after
# adding one clip uploads only that clip. Safe to run with EVO closed or open —
# it writes to /mnt/usb0 and never touches /data/homebrew.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

SRC="${1:-output/sweep_clips}"

if ! in_container; then
    reexec_in_container "../tools/push_sweep_clips.sh" "${SRC}"
fi
require_ps5_host
need_cmd python3

[[ -d "${SRC}" ]] || die "no such directory: ${SRC} (run tools/make_sweep_clips.sh first)"

PS5_HOST="${PS5_HOST}" FTP_PORT="${FTP_PORT:-2121}" SRC="${SRC}" python3 - <<'PY'
import os, sys
from ftplib import FTP

HOST = os.environ["PS5_HOST"]
PORT = int(os.environ["FTP_PORT"])
SRC  = os.environ["SRC"]
DEST = "/mnt/usb0"

def connect():
    f = FTP(); f.connect(HOST, PORT, timeout=30); f.login()
    try: f.set_pasv(True)
    except Exception: pass
    return f

# One listing up front rather than a SIZE per file: the console's FTP server is
# slow to open connections and the whole point is to skip most of these.
remote = {}
with connect() as f:
    lines = []
    f.retrlines("LIST " + DEST, lines.append)
    for l in lines:
        p = l.split(None, 8)
        if len(p) >= 9 and not p[0].startswith("d"):
            try: remote[p[8].strip()] = int(p[4])
            except ValueError: pass

names = sorted(n for n in os.listdir(SRC) if ".part." not in n)
if not names:
    sys.exit("nothing to upload in " + SRC)

sent = skipped = 0
for n in names:
    local = os.path.join(SRC, n)
    size = os.path.getsize(local)
    if remote.get(n) == size:
        print("  skip %-46s (already there, %.1f MB)" % (n, size / 1048576.0))
        skipped += 1
        continue
    print("  put  %-46s %.1f MB" % (n, size / 1048576.0), flush=True)
    with connect() as f, open(local, "rb") as fh:
        f.storbinary("STOR %s/%s" % (DEST, n), fh, blocksize=1 << 16)
    sent += 1

print("\n%d uploaded, %d already present" % (sent, skipped))
PY
