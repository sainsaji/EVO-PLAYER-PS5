#!/usr/bin/env python3
"""
tools/subsync_run.py — drive the subtitle-sync test sequence over FTP.

    PS5_HOST=192.168.0.7 python3 tools/subsync_run.py [--clip /mnt/usb0/...]

Plays the generated sync clip and walks a fixed script of seeks, each followed
by enough playback to collect samples. The steps are deliberately separated:
a player can be perfectly in sync before a seek and wrong after, so "play",
"seek forward", "seek back" and "play through the seek point" each get their own
stretch that tools/subsync_report.py can judge on its own.

Requires the build to carry the subsync trace and /mnt/usb0/evo_subsync to
exist; without both, the log has nothing to report on.
"""

import io
import os
import re
import sys
import time
from ftplib import FTP, error_perm

HOST = os.environ.get("PS5_HOST", "192.168.0.7")
PORT = int(os.environ.get("FTP_PORT", "2121"))
CLIP = "/mnt/usb0/EVO_TEST_subsync.mkv"

SETTLE = 14.0          # seconds of playback to sample after each action


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


def sample(label, secs):
    """Let it play, printing the console clock so the log lines up with steps."""
    t0 = time.time()
    while time.time() - t0 < secs:
        time.sleep(3)
        st = status()
        print("      t=%-8s pos=%-8s %s"
              % (st.get("t", "?"), st.get("pos", "?"), label), flush=True)


def main():
    clip = CLIP
    if "--clip" in sys.argv:
        clip = sys.argv[sys.argv.index("--clip") + 1]
        # Git Bash rewrites a leading "/mnt/..." into a Windows path before this
        # script ever sees argv (MSYS path conversion) - "/mnt/usb0/x.mkv" comes
        # through as "C:/Program Files/Git/mnt/usb0/x.mkv", which the console
        # then can't open (avformat_open_input rc=-1330794744). Put it back.
        if re.match(r"^[A-Za-z]:[\\/].*[\\/]mnt[\\/]", clip):
            clip = "/mnt/" + clip.split("/mnt/", 1)[-1].split("\\mnt\\", 1)[-1]
            print("(reconstructed path after shell mangling: %s)" % clip, flush=True)

    st = status()
    if not st:
        sys.exit("no evo_status — is EVO launched and built --usb-remote?")
    print("build=%s\nclip=%s\n" % (st.get("build", "?"), clip), flush=True)

    steps = [
        ("play",        "play " + clip,  "1. baseline playback from 0"),
        ("seek 120",    "seek 120",      "2. seek FORWARD into un-demuxed video"),
        ("seek 60",     "seek 60",       "3. seek BACK (stale-ring case)"),
        ("seek +25",    "seek +25",      "4. small relative seek forward"),
        ("seek -40",    "seek -40",      "5. relative seek back, then play on"),
    ]

    for n, (name, cmd, why) in enumerate(steps, 1):
        print("[%d/%d] %s  -> %s" % (n, len(steps), why, cmd), flush=True)
        put_cmd(cmd)
        if n == 1:
            if not wait_playing():
                sys.exit("clip never started playing")
            print("      playing", flush=True)
        else:
            time.sleep(2.5)
        sample(name, SETTLE)
        print(flush=True)

    print("stopping", flush=True)
    put_cmd("stop")
    time.sleep(3)
    print("\nnow: tools/evo-remote.sh log && python3 tools/subsync_report.py")


if __name__ == "__main__":
    main()
