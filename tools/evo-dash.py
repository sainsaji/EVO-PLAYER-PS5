#!/usr/bin/env python3
"""
evo-dash.py - live health dashboard for EVO on the console, served on the PC.

    python tools/evo-dash.py            # then open http://localhost:8790
    python tools/evo-dash.py --host 192.168.0.12 --port 8790 --no-open

Runs on the HOST (plain Python 3, no packages, no Docker). It polls EVO's
GET /stats (port 9780, evo_stats.c) once a second and tails GET /raw for the
lines worth seeing - warnings, errors, and the read-ahead / network / memory
events. Both are kept here, not in the browser, so a reload or a second tab
sees the whole session. Every sample is also appended to
output/dash/<date>.jsonl for looking at later.

The console's address comes from --host, else PS5_HOST in the environment,
.env.local or .env (the same files the other tools read).
"""
import argparse
import collections
import datetime
import json
import os
import re
import sys
import threading
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "tools", "dash", "index.html")
LOG_PORT = 9780

LINE = re.compile(r"^\[(\d+(?:\.\d+)?)\] (INFO|WARN|ERROR)\s*(.*)$")


def _name_from_url(text):
    m = re.search(r"https?://\S+", text)
    if not m:
        return "a stream"
    base = m.group(0).split("?")[0].rstrip("/").rsplit("/", 1)[-1]
    return base if "." in base else "a network stream"


# (pattern, kind, sentence or function(match, text) -> sentence). First match wins.
# kind: good / info / warn / bad. Lines that match nothing are not events.
PLAIN = [
    (r"REMOTE_CMD play|P8_01a_SIO_ENTER", "info", lambda m, t: "Opening " + _name_from_url(t)),
    (r"P8_03_PREBUFFER_DONE", "good", "Ready - playback started"),
    (r"P8_04_REBUFFER_ARM", "warn", "Ran out of video ahead - paused to load more"),
    (r"P8_04_REBUFFER_DONE ended", "bad", "Stopped loading - the stream ended or failed"),
    (r"P8_04_REBUFFER_DONE", "good", "Loaded enough - playing again"),
    (r"pio: reader waiting", "bad", "Can't get data from the server - retrying"),
    (r"pio: reader has data again", "good", "Connection is back"),
    (r"pio: chunk \d+ not fetched", "bad", "The server didn't answer for 30 seconds"),
    (r"pio: first 128 MB at (\d+) Mbit/s", "info", lambda m, t: "Download speed: %s Mbit/s" % m.group(1)),
    (r"demux: read failed", "bad", "The stream dropped - reconnecting"),
    (r"demux: recovered", "good", "Reconnected - picking up where it stopped"),
    (r"demux: gave up", "bad", "Gave up reconnecting - the stream stopped"),
    (r"map_fail=[1-9]|malloc failure|get_buffer\(\) failed|ring fallback", "bad",
     "Out of memory - video may stop"),
    (r"OPEN FAIL|open failed", "bad", "Couldn't open the file"),
    (r"backend=NATIVE.*?(\d+x\d+) @ ([\d.]+) fps", "info",
     lambda m, t: "Hardware video decoder, %s at %s fps" % (m.group(1), m.group(2))),
    (r"backend=FFMPEG|backend=SOFTWARE|Software \(FFmpeg\)", "info", "Software video decoder"),
    (r"hdr10: ENTER", "info", "Switched the TV to HDR"),
    (r"fatal=1|decode fatal|FATAL", "bad", "The video decoder failed"),
]
PLAIN = [(re.compile(p), k, s) for p, k, s in PLAIN]


def plain(text, level):
    for rx, kind, say in PLAIN:
        m = rx.search(text)
        if m:
            return kind, (say(m, text) if callable(say) else say)
    if level == "ERROR":
        return "bad", "Error: " + text[:160]
    if level == "WARN":
        return "warn", "Warning: " + text[:160]
    return None


def find_host():
    if os.environ.get("PS5_HOST"):
        return os.environ["PS5_HOST"]
    for name in (".env.local", ".env"):
        try:
            with open(os.path.join(ROOT, name), encoding="utf-8") as f:
                for ln in f:
                    m = re.match(r"\s*PS5_HOST\s*=\s*['\"]?([^'\"\s#]+)", ln)
                    if m:
                        return m.group(1)
        except OSError:
            pass
    return None


class State:
    def __init__(self, host):
        self.host = host
        self.lock = threading.Lock()
        self.samples = collections.deque(maxlen=4 * 3600)   # 4 h at 1 Hz
        self.events = collections.deque(maxlen=1000)
        self.lines = collections.deque(maxlen=3000)
        self.seq = 0           # bumps on every new sample/event/line
        self.online = False
        self.log_online = False
        self.last_error = ""
        os.makedirs(os.path.join(ROOT, "output", "dash"), exist_ok=True)
        self.jsonl = os.path.join(ROOT, "output", "dash",
                                  datetime.datetime.now().strftime("%Y-%m-%d") + ".jsonl")

    def add_sample(self, s):
        s["wall"] = time.time()
        with self.lock:
            self.seq += 1
            s["seq"] = self.seq
            self.samples.append(s)
        try:
            with open(self.jsonl, "a", encoding="utf-8") as f:
                f.write(json.dumps(s) + "\n")
        except OSError:
            pass

    def add_line(self, raw):
        m = LINE.match(raw)
        level, text, t = ("INFO", raw, None)
        if m:
            t, level, text = float(m.group(1)), m.group(2), m.group(3)
        entry = {"wall": time.time(), "t": t, "level": level, "text": text[:600]}
        said = plain(text, level)
        with self.lock:
            self.seq += 1
            entry["seq"] = self.seq
            self.lines.append(entry)
            if not said:
                return
            kind, msg = said
            # The same sentence over and over (an allocation failing per frame)
            # is one event with a count, not a wall of them.
            last = self.events[-1] if self.events else None
            if last and last["msg"] == msg and entry["wall"] - last["wall_last"] < 15:
                last["count"] += 1
                last["wall_last"] = entry["wall"]
                last["seq"] = self.seq
                return
            self.events.append({"id": self.seq, "seq": self.seq, "wall": entry["wall"], "wall_last": entry["wall"],
                                "kind": kind, "msg": msg, "count": 1, "raw": text[:300]})


def poll_stats(st, stop):
    url = "http://%s:%d/stats" % (st.host, LOG_PORT)
    while not stop.is_set():
        t0 = time.time()
        try:
            with urllib.request.urlopen(url, timeout=3) as r:
                s = json.loads(r.read().decode("utf-8", "replace"))
            st.add_sample(s)
            st.online = True
            st.last_error = ""
        except Exception as e:  # console off, EVO closed, or an older build without /stats
            st.online = False
            st.last_error = str(e)[:200]
        stop.wait(max(0.0, 1.0 - (time.time() - t0)))


def tail_log(st, stop):
    url = "http://%s:%d/raw?tail=0" % (st.host, LOG_PORT)
    while not stop.is_set():
        try:
            # raw mode sends no keepalive, so a quiet log is a long read
            with urllib.request.urlopen(url, timeout=600) as r:
                st.log_online = True
                buf = b""
                while not stop.is_set():
                    chunk = r.read1(8192) if hasattr(r, "read1") else r.read(1)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        ln, buf = buf.split(b"\n", 1)
                        ln = ln.decode("utf-8", "replace").rstrip("\r")
                        if ln.strip():
                            st.add_line(ln)
        except Exception:
            pass
        st.log_online = False
        stop.wait(2.0)


def make_handler(st):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path, _, query = self.path.partition("?")
            if path in ("/", "/index.html"):
                try:
                    with open(PAGE, "rb") as f:
                        self.send(200, f.read(), "text/html; charset=utf-8")
                except OSError as e:
                    self.send(500, str(e).encode(), "text/plain")
                return
            if path == "/api/state":
                since = 0
                m = re.search(r"since=(\d+)", query)
                if m:
                    since = int(m.group(1))
                with st.lock:
                    body = {
                        "host": st.host,
                        "online": st.online,
                        "log_online": st.log_online,
                        "error": st.last_error,
                        "seq": st.seq,
                        "samples": [s for s in st.samples if s["seq"] > since],
                        "events": [e for e in st.events if e["seq"] > since],
                        "lines": [e for e in st.lines if e["seq"] > since][-500:],
                    }
                self.send(200, json.dumps(body).encode(), "application/json")
                return
            self.send(404, b"not found", "text/plain")

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="console address (default: PS5_HOST)")
    ap.add_argument("--port", type=int, default=8790, help="local port for the page")
    ap.add_argument("--no-open", action="store_true", help="don't open a browser")
    a = ap.parse_args()
    host = a.host or find_host()
    if not host:
        sys.exit("no console address: pass --host or set PS5_HOST in .env")
    st = State(host)
    stop = threading.Event()
    for fn in (poll_stats, tail_log):
        threading.Thread(target=fn, args=(st, stop), daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(st))
    url = "http://localhost:%d" % a.port
    print("evo-dash: console %s  ->  %s   (samples saved to %s)" % (host, url, st.jsonl))
    if not a.no_open:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    stop.set()


if __name__ == "__main__":
    main()
