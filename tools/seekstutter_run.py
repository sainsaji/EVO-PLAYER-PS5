#!/usr/bin/env python3
"""
tools/seekstutter_run.py — drive a handful of individual seeks over FTP, each
far enough apart that its 3s trace window finishes before the next seek starts.

    PS5_HOST=192.168.0.7 python3 tools/seekstutter_run.py [--clip /mnt/usb0/...]

Requires the build to carry the seek-stutter trace and /mnt/usb0/evo_seektrace
to exist (see tools/seekstutter_report.py's docstring for what it reads).
"""

import io
import os
import re
import sys
import time
from ftplib import FTP, error_perm

HOST = os.environ.get("PS5_HOST", "192.168.0.7")
PORT = int(os.environ.get("FTP_PORT", "2121"))
CLIP = "/mnt/usb0/EVO_TEST_subsync_1080p.mkv"

GAP_S = 9.0   # > the 6s trace window (which now covers the OSD fade-out
              # at commit+4.2s), with margin for the seek itself


def connect():
    f = FTP(); f.connect(HOST, PORT, timeout=15); f.login()
    try: f.set_pasv(True)
    except Exception: pass
    return f


def put_cmd(line):
    with connect() as f:
        f.storbinary("STOR /mnt/usb0/evo_cmd", io.BytesIO((line + "\n").encode()))


def status():
    try:
        with connect() as f:
            b = []
            try: f.retrbinary("RETR /mnt/usb0/evo_status", b.append)
            except error_perm: return {}
            out = {}
            for tok in b and b"".join(b).decode("utf-8", "replace").split() or []:
                if "=" in tok:
                    k, v = tok.split("=", 1); out[k] = v
            return out
    except Exception:
        return {}


def wait_playing(timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = status()
        if st.get("active") == "1" and float(st.get("pos", "0") or 0) > 0.3:
            return st
        time.sleep(1.5)
    return None


def main():
    clip = CLIP
    if "--clip" in sys.argv:
        clip = sys.argv[sys.argv.index("--clip") + 1]
        # Same MSYS path-mangling guard as subsync_run.py's --clip.
        if re.match(r"^[A-Za-z]:[\\/].*[\\/]mnt[\\/]", clip):
            clip = "/mnt/" + clip.split("/mnt/", 1)[-1].split("\\mnt\\", 1)[-1]
            print("(reconstructed path: %s)" % clip, flush=True)

    st = status()
    if not st:
        sys.exit("no evo_status — is EVO launched and built --usb-remote?")
    print("build=%s\nclip=%s\n" % (st.get("build", "?"), clip), flush=True)

    put_cmd("play " + clip)
    if not wait_playing():
        sys.exit("clip never started playing")
    print("playing", flush=True)
    time.sleep(4)

    # A spread of seek shapes: short-hop forward (near GOP), long-hop forward
    # (many GOPs to skip), backward, and two in quick succession (the case
    # where the second seek's BEGIN can land inside the first's 3s window —
    # the report handles that by starting a new window at each COMMIT).
    seeks = [
        ("short forward (5s)",  "seek +5"),
        ("long forward (90s)",  "seek 150"),
        ("backward (40s)",      "seek 40"),
        ("back-to-back #1",     "seek 70"),
        ("back-to-back #2",     "seek 75"),
    ]
    for i, (label, cmd) in enumerate(seeks, 1):
        print("[%d/%d] %s  -> %s" % (i, len(seeks), label, cmd), flush=True)
        put_cmd(cmd)
        gap = 2.0 if "back-to-back #1" in label else GAP_S
        time.sleep(gap)

    put_cmd("stop")
    print("\nnow: tools/evo-remote.sh log && python3 tools/seekstutter_report.py")


if __name__ == "__main__":
    main()
