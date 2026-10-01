#!/usr/bin/env python3
"""
tools/evo_ui.py - #115: read EVO's UI state as text (called by evo-remote.sh).

A --usb-remote build writes /mnt/usb0/evo_ui.json whenever the screen, the
focused element, a modal or a toast changes. This reads it over FTP, so the
remote can navigate without a screenshot.

  evo_ui.py show [--line]        pretty JSON (or just the one-line summary)
  evo_ui.py key <button>...      press each button in turn; after each, wait
                                 for the state it produced and print its line

Env: PS5_HOST, FTP_PORT (2121), EVO_KEY_WAIT (seconds per press, default 2.5).
"""
import io
import json
import os
import sys
import time
from ftplib import FTP, error_perm

HOST = os.environ["PS5_HOST"]
PORT = int(os.environ.get("FTP_PORT", "2121"))
UI_PATH = "/mnt/usb0/evo_ui.json"
CMD_PATH = "/mnt/usb0/evo_cmd"


def connect():
    f = FTP()
    f.connect(HOST, PORT, timeout=10)
    f.login()
    try:
        f.set_pasv(True)
    except Exception:
        pass
    return f


def read_state(f):
    buf = []
    try:
        f.retrbinary("RETR " + UI_PATH, buf.append)
    except error_perm:
        return None
    try:
        return json.loads(b"".join(buf).decode("utf-8", "replace"))
    except ValueError:
        return None   # caught between write and rename - next read is whole


def summary(st):
    """[focus] #id "text" (item i/n, doc) on Screen  [modal ...] [toast ...]"""
    if not st:
        return "[focus] (no evo_ui.json - launched? built --usb-remote?)"
    ui = st.get("ui", {})
    scr = ui.get("screen") or {}
    foc = ui.get("focused")
    if foc:
        idn = "#" + foc["id"] if foc.get("id") else "<" + foc.get("tag", "?") + ">"
        where = "item %d/%d" % (foc.get("index", 0), foc.get("total", 0))
        line = '[focus] %s "%s" (%s, %s)' % (idn, foc.get("text", ""), where, foc.get("doc", ""))
    else:
        line = "[focus] (none)"
    line += " on %s(%s)" % (scr.get("name", "?"), scr.get("id", "?"))
    modal = ui.get("modal")
    if modal:
        line += ' [modal %s: "%s"]' % (modal.get("kind"), modal.get("text", "")[:80])
    if ui.get("toast"):
        line += ' [toast: "%s"]' % ui["toast"][:80]
    np = ui.get("native_player")
    if np:
        line += " [player osd=%d paused=%d%s]" % (
            np.get("osd_visible", False), np.get("paused", False),
            " " + np["active_overlay"] if np.get("active_overlay") else "")
    return line + "  (seq %s)" % st.get("seq", "?")


def cmd_show(args):
    with connect() as f:
        st = read_state(f)
    if "--line" not in args and st:
        print(json.dumps(st, indent=2, ensure_ascii=False))
    print(summary(st))
    return 0 if st else 1


def cmd_key(buttons):
    wait = float(os.environ.get("EVO_KEY_WAIT", "2.5"))
    with connect() as f:
        st = read_state(f)
        for b in buttons:
            seq = st.get("seq") if st else None
            f.storbinary("STOR " + CMD_PATH, io.BytesIO(("key %s\n" % b).encode()))
            deadline = time.time() + wait
            changed = False
            while time.time() < deadline:
                time.sleep(0.15)
                cur = read_state(f)
                if cur and cur.get("seq") != seq:
                    # Let a burst of writes (an animation, a list refill)
                    # settle so the line shows where focus came to rest.
                    time.sleep(0.25)
                    st = read_state(f) or cur
                    changed = True
                    break
            else:
                st = read_state(f) or st
            print("%-8s %s%s" % (b, summary(st), "" if changed else "  (unchanged)"))
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "show":
        return cmd_show(sys.argv[2:])
    if sys.argv[1] == "key" and len(sys.argv) > 2:
        return cmd_key(sys.argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
