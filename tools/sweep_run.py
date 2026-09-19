#!/usr/bin/env python3
"""
tools/sweep_run.py — drive the #8 codec sweep over FTP.

Invoked by `tools/evo-remote.sh sweep`; not meant to be run directly (it needs
PS5_HOST / FTP_PORT in the environment). Lists a directory on the console, plays
each clip for a fixed window, and moves on. EVO does the measuring — this only
decides when a clip has been watched long enough and writes the next command.

Every FTP call is bounded by a timeout: the console's FTP server happily accepts
a connection it will never answer, and an unbounded sweep of 29 files is an
overnight hang waiting to happen.
"""

import io
import os
import sys
import time
from ftplib import FTP, error_perm

HOST = os.environ["PS5_HOST"]
PORT = int(os.environ.get("FTP_PORT", "2121"))
SWEEP_DIR = os.environ.get("SWEEP_DIR", "/mnt/usb0/test_files_aud_vid")
WINDOW_S = float(os.environ.get("SWEEP_SECS", "30"))
MAX_FILES = int(os.environ.get("SWEEP_MAX", "0"))

# Case-insensitive substrings; any match excludes the clip. A clip that is known
# to take the app down ends the whole run at that point and costs every row
# after it, so being able to fence one off is what makes a baseline obtainable
# while a crash is still open.
SKIP = [s.strip().lower()
        for s in os.environ.get("SWEEP_SKIP", "").split(",") if s.strip()]

CMD_PATH = "/mnt/usb0/evo_cmd"
STATUS_PATH = "/mnt/usb0/evo_status"

MEDIA_EXT = (
    ".mp4", ".mkv", ".m4v", ".mov", ".avi", ".webm", ".ts", ".m2ts",
    ".mpg", ".mpeg", ".wmv", ".flv", ".3gp", ".ogv",
)

# Hard ceiling per clip regardless of what the status line says. A file that
# never starts must not stall the run; it gets recorded as whatever EVO managed.
STALL_GRACE_S = 25.0


def connect():
    f = FTP()
    f.connect(HOST, PORT, timeout=15)
    f.login()
    try:
        f.set_pasv(True)
    except Exception:
        pass
    return f


def put_cmd(line):
    with connect() as f:
        f.storbinary("STOR " + CMD_PATH, io.BytesIO((line + "\n").encode()))


def get_status():
    try:
        with connect() as f:
            buf = []
            try:
                f.retrbinary("RETR " + STATUS_PATH, buf.append)
            except error_perm:
                return None
            return b"".join(buf).decode("utf-8", "replace").strip()
    except Exception:
        return None


def parse_status(line):
    out = {}
    if not line:
        return out
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def list_media(path):
    """
    ps5-payload-ftpsrv answers LIST but rejects NLST (502), and its MLSD ignores
    the path argument and lists the root — so LIST it is, parsed as the Unix
    listing it emits:

        -rwxrwxrwx 1 4294967295 4294967295 265257660 Mar 14 04:36 Some Clip.mp4
         0         1 2          3          4         5   6  7     8...

    Eight splits, because a file name can (and in this test set does) contain
    spaces.
    """
    lines = []
    with connect() as f:
        try:
            f.retrlines("LIST " + path, lines.append)
        except error_perm as e:
            sys.exit("cannot list %s: %s" % (path, e))

    out = []
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9:
            continue
        if parts[0].startswith("d"):
            continue
        base = parts[8].strip()
        if base in (".", "..") or not base:
            continue
        if not base.lower().endswith(MEDIA_EXT):
            continue
        hit = next((s for s in SKIP if s in base.lower()), None)
        if hit:
            print("  skip %s  (matches --skip %s)" % (base, hit), flush=True)
            continue
        out.append(path.rstrip("/") + "/" + base)
    return sorted(out)


def play_one(path, index, total):
    name = path.rsplit("/", 1)[-1]
    print("[%2d/%d] %s" % (index, total, name), flush=True)
    put_cmd("play " + path)

    t0 = time.time()
    started = False
    last_pos = -1.0
    last_progress = t0

    while True:
        time.sleep(2)
        st = parse_status(get_status())
        pos = float(st.get("pos", "0") or 0)
        dur = float(st.get("dur", "0") or 0)
        eof = st.get("eof") == "1"
        fatal = st.get("fatal") == "1"
        active = st.get("active") == "1"
        now = time.time()

        if active and not started:
            started = True
            print("        playing (dur=%.0fs)" % dur, flush=True)

        if pos > last_pos + 0.05:
            last_pos = pos
            last_progress = now

        if fatal:
            print("        decode fatal — moving on", flush=True)
            return
        if eof:
            print("        eof at %.1fs" % pos, flush=True)
            return
        if started and pos >= WINDOW_S:
            print("        window reached at %.1fs" % pos, flush=True)
            return
        if 0 < dur <= WINDOW_S and started and pos >= dur - 0.5:
            print("        short clip done at %.1fs" % pos, flush=True)
            return
        if now - last_progress > STALL_GRACE_S:
            print("        stalled (pos stuck at %.1fs) — moving on" % pos, flush=True)
            return
        if now - t0 > WINDOW_S + 2 * STALL_GRACE_S:
            print("        timed out — moving on", flush=True)
            return


def main():
    st = parse_status(get_status())
    if not st:
        sys.exit("no evo_status on the console — is EVO launched, "
                 "and built with --usb-remote?")
    print("build=%s  sweeping %s  window=%.0fs"
          % (st.get("build", "?"), SWEEP_DIR, WINDOW_S), flush=True)

    files = list_media(SWEEP_DIR)
    if not files:
        sys.exit("no media files under " + SWEEP_DIR)
    if MAX_FILES > 0:
        files = files[:MAX_FILES]
    print("%d clips\n" % len(files), flush=True)

    for i, p in enumerate(files, 1):
        try:
            play_one(p, i, len(files))
        except KeyboardInterrupt:
            raise
        except Exception as e:
            print("        error: %s — moving on" % e, flush=True)

    # Flush the final clip's row: it is written when the decoder closes, and
    # nothing else is going to close it.
    print("\nstopping playback (flushes the last sweep row)", flush=True)
    try:
        put_cmd("stop")
        time.sleep(3)
    except Exception as e:
        print("  warning: could not send stop (%s); the last row may be "
              "missing" % e, flush=True)


if __name__ == "__main__":
    main()
