#!/usr/bin/env python3
"""
tools/evo_lifecycle.py - where PPSA99039 is in its lifecycle, from evidence.

Used by tools/evo-remote.sh (launch/quit/close/cycle) and scripts/deploy-app.sh.
Two sources, both read over FTP:

  /data/shadowmount/debug.log   ShadowMount+'s own lifecycle record:
        NOTIFY: installed game <TID>                 registered (first time)
        [LINK] mount.lnk created: /user/app/<TID>/   (re)mounted (v1.6)
        [SHELLCORE] launch mount ready: <TID>        mounted for a launch (v1.7)
        [IMG][LVD] unmount complete: source=/data/homebrew/<TID>.ffpfsc
                                                     updated image verified (v1.7:
                                                     it test-mounts, then unmounts,
                                                     and does NOT auto-launch)
        [GAME] started: <TID>                        launched
        [LINK] runtime layers released: <TID>        process gone, slot free
  /mnt/usb0/evo_status          EVO's own heartbeat (--usb-remote builds):
        t=<s> advancing                              running right now
        parked=1                                     soft-closed, GPU drained

Subcommands (print one word / line, exit 0 unless the FTP read failed):

  slot                     FREE | RESIDENT | UNKNOWN    (ShadowMount only)
  heartbeat                RUNNING | PARKED | STILL | ABSENT
  smlen                    current debug.log length, a baseline for wait-*
  wait-registered  BASE S  REGISTERED | TIMEOUT   (new install/mount.lnk after BASE)
  wait-started     BASE S  STARTED | TIMEOUT
  wait-released    BASE S  RELEASED | TIMEOUT
  wait-parked      S       PARKED | TIMEOUT
  smslice BASE             the debug.log lines after BASE that mention the title

Env: PS5_HOST, FTP_PORT (2121), TITLE_ID (PPSA99039).
"""
import os
import sys
import time
from ftplib import FTP, error_perm

HOST = os.environ["PS5_HOST"]
PORT = int(os.environ.get("FTP_PORT", "2121"))
TID = os.environ.get("TITLE_ID", "PPSA99039")
SM_LOG = "/data/shadowmount/debug.log"
SM_LOG_OLD = SM_LOG + ".1"      # ShadowMount rotates at ~16 KB
STATUS = "/mnt/usb0/evo_status"

STARTED = f"[GAME] started: {TID}"
RELEASED = f"[LINK] runtime layers released: {TID}"
INSTALLED = f"NOTIFY: installed game {TID}"
MOUNTED = f"[LINK] mount.lnk created: /user/app/{TID}/mount.lnk"
MOUNT_READY = f"[SHELLCORE] launch mount ready: {TID}"
IMAGE_CHECKED = f"unmount complete: source=/data/homebrew/{TID}.ffpfsc"


def fetch(path, tries=3):
    """File text, None if it does not exist. Raises once every try failed:
    one slow FTP read must not be taken for a verdict."""
    for i in range(tries):
        try:
            return _fetch_once(path)
        except Exception:
            if i == tries - 1:
                raise
            time.sleep(2)


def _fetch_once(path):
    with FTP() as f:
        f.connect(HOST, PORT, timeout=15)
        f.login()
        try:
            f.set_pasv(True)
        except Exception:
            pass
        buf = []
        try:
            f.retrbinary("RETR " + path, buf.append)
        except error_perm:
            return None
        return b"".join(buf).decode("utf-8", "replace")


def sm_text():
    t = fetch(SM_LOG)
    if t is None:
        raise RuntimeError(f"no {SM_LOG}")
    return t


def sm_delta(base):
    """Everything logged since `base` bytes, across one rotation: a log
    shorter than its baseline was rotated, and its old tail is in .1."""
    cur = sm_text()
    if len(cur) >= base:
        return cur[base:]
    old = fetch(SM_LOG_OLD) or ""
    return old[base:] + cur


def sm_both():
    """The rotated log then the live one - a restart or a rotation can leave
    the title's last lifecycle lines only in .1."""
    return (fetch(SM_LOG_OLD) or "") + "\n" + sm_text()


def slot(text):
    s, r = text.rfind(STARTED), text.rfind(RELEASED)
    if s < 0:
        return "FREE" if r >= 0 else "UNKNOWN"
    return "FREE" if r > s else "RESIDENT"


def status_fields():
    t = fetch(STATUS)
    if not t:
        return None
    return dict(tok.split("=", 1) for tok in t.split() if "=" in tok)


def heartbeat():
    a = status_fields()
    if a is None:
        return "ABSENT"
    if a.get("parked") == "1":
        return "PARKED"
    time.sleep(3)
    b = status_fields()
    if b is None:
        return "ABSENT"
    if b.get("parked") == "1":
        return "PARKED"
    return "RUNNING" if b.get("t") != a.get("t") else "STILL"


def wait_for(base, secs, needles, label):
    deadline = time.time() + secs
    while True:
        try:
            delta = sm_delta(base)
            if any(n in delta for n in needles):
                return label
        except Exception:
            pass          # a missed poll is not a verdict; keep waiting
        if time.time() >= deadline:
            return "TIMEOUT"
        time.sleep(3)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    try:
        if cmd == "slot":
            print(slot(sm_both()))
        elif cmd == "heartbeat":
            print(heartbeat())
        elif cmd == "smlen":
            print(len(sm_text()))
        elif cmd == "wait-registered":
            print(wait_for(int(argv[2]), int(argv[3]), (INSTALLED, MOUNTED, MOUNT_READY, IMAGE_CHECKED), "REGISTERED"))
        elif cmd == "wait-started":
            print(wait_for(int(argv[2]), int(argv[3]), (STARTED,), "STARTED"))
        elif cmd == "wait-released":
            print(wait_for(int(argv[2]), int(argv[3]), (RELEASED,), "RELEASED"))
        elif cmd == "wait-parked":
            deadline = time.time() + int(argv[2])
            while True:
                try:
                    a = status_fields()
                    if a and a.get("parked") == "1":
                        print("PARKED")
                        break
                except Exception:
                    pass
                if time.time() >= deadline:
                    print("TIMEOUT")
                    break
                time.sleep(2)
        elif cmd == "smslice":
            for ln in sm_delta(int(argv[2])).splitlines():
                if TID in ln or "[rtld]" in ln or "crash" in ln.lower():
                    print(ln)
        else:
            sys.exit(__doc__)
    except Exception as e:
        sys.stderr.write(f"evo_lifecycle {cmd}: {e}\n")
        if cmd in ("slot", "heartbeat"):
            print("UNKNOWN")   # a verdict, not a failure: callers branch on it
            sys.exit(0)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv)
