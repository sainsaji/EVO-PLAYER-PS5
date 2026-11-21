#!/usr/bin/env python3
"""evo_log_perf.py - the performance numbers in an evo.log, one summary.

    python3 tests/perf/evo_log_perf.py output/logs/evo.log [more logs...]

Read-only. It prints, per log:
  * boot: main() -> frame loop, and flex_free / direct_total at boot
  * playback starts: flexible memory lost between decode-open and the first
    `alloc [play]` line (the packet rings), map_fail, the read-ahead line
  * cover decodes on the Home frame: thumb-start -> thumb-end durations
  * pacing: presents/s, gaps > 30 ms, slow loop iterations
  * ring fallbacks (a ring too small for the stream)
Use it before and after a change, on the same file and the same build flags.
"""
import re
import sys

TS = re.compile(r"^\[(\d+\.\d+)\]")


def ts(line):
    m = TS.match(line)
    return float(m.group(1)) if m else None


def field(line, name):
    m = re.search(r"\b" + name + r"=(-?\d+)", line)
    return int(m.group(1)) if m else None


def summarize(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    print("==", path)
    t_main = None
    boots, starts, thumbs, gaps, slow = [], [], [], [], []
    pending_open, thumb_t0, fallbacks = None, None, 0
    for ln in lines:
        t = ts(ln)
        if "EVO boot: main() entry" in ln:
            t_main = t
        elif "EVO boot: Application: entering frame loop" in ln and t_main is not None:
            boots.append(t - t_main)
            t_main = None
        elif "mem budget [boot]" in ln:
            print("  boot memory: direct_total=%sMB flex_total=%sMB flex_free=%sMB"
                  % (field(ln, "direct_total"), field(ln, "flex_total"), field(ln, "flex_free")))
        elif "alloc [decode-open]" in ln:
            pending_open = ln
        elif "alloc [play]" in ln and pending_open:
            a0, a1 = field(pending_open, "flex_avail"), field(ln, "flex_avail")
            starts.append((a0, a1, field(ln, "map_fail"), field(ln, "direct_live")))
            pending_open = None
        elif "pb: read-ahead" in ln and "(ring)" in ln:
            print("  " + ln.split("INFO", 1)[-1].strip())
        elif "alloc [thumb-start]" in ln:
            thumb_t0 = t
        elif "alloc [thumb-end]" in ln and thumb_t0 is not None:
            thumbs.append((t - thumb_t0) * 1000.0)
            thumb_t0 = None
        elif "pace: 5.0s presents=" in ln:
            m = re.search(r">30:(\d+)\]", ln)
            p = re.search(r"presents=(\d+)", ln)
            if m and p:
                gaps.append((int(p.group(1)), int(m.group(1))))
        elif "pace:   loop max ms" in ln:
            m = re.search(r"slow\(>30ms\) iters=(\d+)", ln)
            if m:
                slow.append(int(m.group(1)))
        elif "ring fallback #" in ln:
            fallbacks += 1
    if boots:
        print("  boot main()->frame loop: %s s" % ", ".join("%.3f" % b for b in boots))
    for a0, a1, mf, dl in starts:
        lost = (a0 - a1) if (a0 is not None and a1 is not None) else None
        print("  playback start: flex_avail %s -> %s MB (lost %s MB) map_fail=%s direct_live=%s MB"
              % (a0, a1, lost, mf, dl))
    if thumbs:
        print("  Home cover decodes: n=%d avg=%.1f ms max=%.1f ms"
              % (len(thumbs), sum(thumbs) / len(thumbs), max(thumbs)))
    if gaps:
        tot = sum(p for p, _ in gaps)
        late = sum(g for _, g in gaps)
        print("  pacing: %d presents, %d gaps > 30 ms (%.1f%%)" % (tot, late, 100.0 * late / max(tot, 1)))
    if slow:
        print("  slow loop iterations (>30 ms): %d over %d windows" % (sum(slow), len(slow)))
    print("  ring fallback lines: %d" % fallbacks)


if __name__ == "__main__":
    for p in sys.argv[1:] or ["output/logs/evo.log"]:
        summarize(p)
