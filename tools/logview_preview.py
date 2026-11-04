#!/usr/bin/env python3
"""
Host preview of EVO's log viewer page (projects/evoplayer/assets/web/log.html)
without a console: serves the page and its fonts the way evo_log_server.c does,
and fakes the data routes from a real evo.log.

  python tools/logview_preview.py --log path/to/evo.log [--port 8791] [--demo]

  /log, /           the page from assets/web/
  /phones           every view in 390 px frames, at the top and scrolled
  /web/<file>       its fonts
  /info             a fixed PS5 Pro / MIDNIGHT answer
  /session.txt      the last session of --log (?run=last: the one before it)
  /stream           a few live lines every second (Server-Sent Events)
  /stats            a playing 4K stream whose download is slower than it needs

--shots answers /stream with 204, so headless Edge/Chrome can screenshot it
(--virtual-time-budget never ends while a stream is open).

--demo appends a made-up Jellyfin film to the session (pauses to load, a
dropped connection, a decoder stall) so every part of the page has something
to show. Plain Python, no packages.
"""
import argparse
import http.server
import json
import os
import re
import socketserver
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "projects", "evoplayer", "assets", "web")
TYPES = {".html": "text/html; charset=utf-8", ".woff2": "font/woff2", ".css": "text/css", ".js": "text/javascript"}


def sessions(text):
    """Split a log into sessions at each 'log session start' line."""
    starts = [m.start() for m in re.finditer(r"(?m)^\[[\d.]+\] INFO  log session start", text)]
    if not starts:
        return [text]
    return [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]


def demo_lines(t0):
    """A Jellyfin film that pauses to load twice, drops once and has a decoder stall."""
    url = "http://192.168.0.20:8096/Videos/abc/stream.mkv?api_key=***"
    seq = [
        (0.0, "INFO", "provider: stream 1 chosen for Dune: Part Two (2024): " + url),
        (0.0, "INFO", "bc P8_01a_SIO_ENTER " + url),
        (0.4, "INFO", "playback: video stream 0 of 6 (3840x2160, program -1)"),
        (0.5, "INFO", "PlaybackController: video decoder opened (backend=NATIVE (sceVideodec2), codec=173, 3840x2160 @ 23.976 fps)"),
        (0.5, "INFO", "audio: source 8ch -> port 8ch (output=auto, pt=1, boost=0%, delay=0ms) handle=536870913"),
        (0.6, "INFO", "  pb: read-ahead 30s (ring) -> video 720 pkt / 154 MB, audio 60000 pkt / 64 MB (video 41200 kbit/s @ 23.98 fps)"),
        (0.6, "INFO", "bc P8_03_PREBUFFER_ARM " + url),
        (2.1, "INFO", "bc P8_03_PREBUFFER_DONE filled packets=48 target=48"),
        (2.2, "INFO", "hdr10: ENTER maxcll=1000 maxfall=400"),
        (40.0, "WARN", "net: slow read 2.1 s from 192.168.0.20:8096"),
        (44.0, "INFO", "bc P8_04_REBUFFER_ARM video_s=0.8"),
        (47.4, "INFO", "bc P8_04_REBUFFER_DONE resumed"),
        (63.0, "ERROR", "vdec: decoder stalled 1500 ms, restarting at 0:58.1"),
        (63.6, "INFO", "vdec: decoder restart ok"),
        (90.0, "WARN", "demux: read failed (-5), reconnecting"),
        (92.5, "INFO", "demux: recovered at 1:27.0"),
        (130.0, "INFO", "bc P8_04_REBUFFER_ARM video_s=0.6"),
        (132.9, "INFO", "bc P8_04_REBUFFER_DONE resumed"),
        (150.0, "WARN", "hui: glyph U+0915 missing, drawn as ?"),
        (160.0, "INFO", "diag: Download is slower than this video needs - 38 of 41 Mbit/s. Try a wired connection."),
    ]
    out = []
    for dt, lv, msg in seq:
        out.append("[%.3f] %-5s %s" % (t0 + dt, lv, msg))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--demo", action="store_true")
    ap.add_argument("--shots", action="store_true", help="no live stream, for headless screenshots")
    a = ap.parse_args()

    with open(a.log, encoding="utf-8", errors="replace") as f:
        ses = sessions(f.read())
    cur = ses[-1]
    last = ses[-2] if len(ses) > 1 else ""
    ts = [float(x) for x in re.findall(r"(?m)^\[([\d.]+)\]", cur)]
    t_end = ts[-1] if ts else 0.0
    if a.demo:
        cur += demo_lines(t_end + 5)
        t_end += 170
    state = {"t": t_end, "bytes": 0, "pos": 1520.0}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *x):
            pass

        def send(self, code, body, ctype, extra=None):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path, _, q = self.path.partition("?")
            path = path.lower().rstrip("/") or "/"
            if path in ("/", "/log"):
                with open(os.path.join(WEB, "log.html"), "rb") as f:
                    return self.send(200, f.read(), TYPES[".html"])
            if path == "/phones":
                # Every view at phone width, once at the top and once scrolled:
                # sticky headers and overlaps only show when the page has moved.
                frames = ""
                for view in ("", "#story", "#log", "#last", "#report"):
                    for scroll in (0, 500):
                        frames += ('<div><p>%s scroll %d</p><iframe src="/log?noscroll%s" width="390" height="760" '
                                   'data-scroll="%d"></iframe></div>' % (view or "#now", scroll, view, scroll))
                page = ("<!doctype html><body style='margin:0;padding:12px;background:#333;color:#ccc;font:13px sans-serif;"
                        "display:flex;flex-wrap:wrap;gap:12px'>" + frames +
                        "<script>document.querySelectorAll('iframe').forEach(f=>f.onload=()=>setTimeout(()=>"
                        "f.contentWindow.scrollTo(0,+f.dataset.scroll),2500))</script></body>")
                return self.send(200, page, TYPES[".html"])
            if path.startswith("/web/"):
                name = os.path.basename(path)
                p = os.path.join(WEB, name)
                if not os.path.isfile(p):
                    return self.send(404, "not found\n", "text/plain")
                with open(p, "rb") as f:
                    return self.send(200, f.read(), TYPES.get(os.path.splitext(name)[1], "application/octet-stream"))
            if path == "/info":
                return self.send(200, json.dumps({
                    "version": "0.14.0", "build": "5b591081_1104-0708", "model": "PS5 Pro", "up_s": 3600,
                    "last_run": "crashed" if a.demo else "closed",
                    "theme": {"name": "MIDNIGHT", "bg_top": "#080D16FF", "bg_bottom": "#03050AFF", "surface": "#101A2CF0",
                              "surface_alt": "#0C1422F0", "surface_sel": "#16223AFA", "border": "#5A7DB433",
                              "border_sel": "#FFCD00FF", "accent": "#FFCD00FF", "accent_soft": "#FFCD0032",
                              "accent_alt": "#00CDFFFF", "danger": "#FF5C5CFF", "text_primary": "#EEF4FFFF",
                              "text_secondary": "#A3B6D0FF", "text_muted": "#5E718CFF", "radius": 16}}), "application/json")
            if path == "/session.txt":
                return self.send(200, last if "run=last" in q else cur, "text/plain; charset=utf-8")
            if path == "/stats":
                state["bytes"] += int(38e6 / 8 * (0.9 + 0.2 * ((time.time() * 7) % 1)))
                state["pos"] += 1
                buf = 4 + 3 * ((time.time() / 5) % 1)
                return self.send(200, json.dumps({
                    "t": state["t"], "build": "5b591081_1104-0708",
                    "mem": {"live_mb": 690, "peak_mb": 700, "flex_free_mb": 612, "flex_total_mb": 1024, "direct_mb": 671,
                            "direct_peak_mb": 680, "map_fail": 0, "gpu_pool_mb": 310, "gpu_pool_total_mb": 512},
                    "play": {"active": 1, "paused": 0, "eof": 0, "fatal": 0, "buffering": 0, "pos": state["pos"], "dur": 9960.0,
                             "fps": 23.976, "backend": 1, "render_fps": 60, "decode_fps": 24},
                    "queue": {"video_pkts": 98, "video_mb": 40, "video_cap_mb": 154, "video_s": buf, "audio_pkts": 400,
                              "audio_mb": 9, "audio_cap_mb": 64},
                    "net": {"pio": 1, "connections": 2, "ready_mb": 40, "window_mb": 128, "bytes": state["bytes"],
                            "chunk_failures": 0, "stall_ms": 0, "recoveries": 1, "recovering": 0}}), "application/json")
            if path == "/stream":
                if a.shots:
                    # 204 stops EventSource for good, so a headless screenshot settles
                    return self.send(204, b"", "text/plain")
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                try:
                    while True:
                        state["t"] += 1.0
                        line = "[%.3f] INFO    alloc [play] live=690MB peak=700MB map_fail=0 flex_avail=612MB" % state["t"]
                        self.wfile.write(("data: " + line + "\n\n").encode())
                        if int(state["t"]) % 7 == 0:
                            self.wfile.write(("data: [%.3f] WARN  net: slow read 1.6 s from 192.168.0.20:8096\n\n" % state["t"]).encode())
                        self.wfile.flush()
                        time.sleep(1)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    return
            return self.send(404, "not found\n", "text/plain")

    class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    print("log viewer preview: http://localhost:%d/log" % a.port)
    S(("127.0.0.1", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
