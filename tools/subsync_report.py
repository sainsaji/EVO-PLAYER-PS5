#!/usr/bin/env python3
"""
tools/subsync_report.py — measure subtitle drift from evo.log.

    python3 tools/subsync_report.py output/logs/evo.log

Reads the `subsync` lines EVO writes when /mnt/usb0/evo_subsync exists, and
reports how far each displayed cue was from where it should have been.

This only works because the test clip is built for it
(tools/make_subsync_clip.sh): every cue's TEXT states its own start time, so a
line of log carries both the claim and the answer. With an ordinary subtitle
file a log can only say "cue X was on screen at time T" and someone still has to
look up when cue X was supposed to start; here the arithmetic closes.

    cue "SUB 00:00:42.000" displayed at pos=42.310  ->  drift +0.310

A cue is authored to show for CUE_WINDOW seconds from its start, so a drift in
[0, CUE_WINDOW] means it was on screen during its own window. Negative means it
appeared early, greater than the window means it overstayed — both are desync.
"""

import argparse
import re
import sys

CUE_WINDOW = 1.6      # must match make_subsync_clip.sh
STEP = 2.0            # cue every STEP seconds

STAMP_RE = re.compile(r"^\[(\d+(?:\.\d+)?)\]")
SUB_RE = re.compile(
    r"\[(?P<t>\d+\.\d+)\]\s+subsync "
    r"pos=(?P<pos>-?\d+\.\d+) base=(?P<base>-?\d+\.\d+) clk=(?P<clk>-?\d+\.\d+) "
    r"src=(?P<src>\w+) delay=(?P<delay>-?\d+) mode=(?P<mode>\w+) "
    r"mclk=(?P<mclk>-?\d+\.\d+) cue=\"(?P<cue>.*)\"\s*$")

# "SUB 00:00:42.000  (cue 22)"
CUE_T_RE = re.compile(r"SUB (\d+):(\d+):(\d+\.\d+)")


def cue_start(text):
    m = CUE_T_RE.search(text or "")
    if not m:
        return None
    h, mm, s = int(m.group(1)), int(m.group(2)), float(m.group(3))
    return h * 3600 + mm * 60 + s


def parse(path, after=None):
    """
    evo.log is cumulative across launches and across every `play` command in
    one launch, so a single file can hold several subsync runs back to back
    (embedded, then external, with no relaunch between them). `after` (console
    seconds) keeps only lines at or after that point - use it to report one
    run out of a log that has more than one.
    """
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if after is not None:
                st = STAMP_RE.match(line)
                if not st or float(st.group(1)) < after:
                    continue
            m = SUB_RE.search(line)
            if not m:
                continue
            d = m.groupdict()
            rows.append({
                "t": float(d["t"]), "pos": float(d["pos"]),
                "base": float(d["base"]), "clk": float(d["clk"]),
                "src": d["src"], "delay": int(d["delay"]), "mode": d["mode"],
                "mclk": float(d["mclk"]), "cue": d["cue"],
                "cue_start": cue_start(d["cue"]),
            })
    return rows


def segments(rows, gap=3.0):
    """
    Split on a backwards jump or a hole in `pos` — i.e. a seek. Each segment is
    then a stretch of continuous playback that can be judged on its own, which
    is the point: a player can be perfectly in sync before a seek and wrong
    after, and one averaged number over the whole run hides exactly that.
    """
    segs, cur = [], []
    for r in rows:
        if cur:
            dp = r["pos"] - cur[-1]["pos"]
            if dp < -0.5 or dp > gap:
                segs.append(cur)
                cur = []
        cur.append(r)
    if cur:
        segs.append(cur)
    return segs


def report(rows):
    if not rows:
        sys.exit("no `subsync` lines found — was /mnt/usb0/evo_subsync created, "
                 "and is this a build with the trace in it?")

    print("%d subsync samples, mode=%s clock=%s delay=%dms"
          % (len(rows), rows[0]["mode"], rows[0]["src"], rows[0]["delay"]))

    segs = segments(rows)
    print("%d continuous segment(s) (split at seeks)\n" % len(segs))

    worst_overall = 0.0
    verdicts = []
    for n, seg in enumerate(segs, 1):
        scored = [r for r in seg if r["cue_start"] is not None]
        lo, hi = seg[0]["pos"], seg[-1]["pos"]
        if not scored:
            print("segment %d  pos %.1f..%.1f  %d samples — NO CUES ON SCREEN"
                  % (n, lo, hi, len(seg)))
            verdicts.append("no-cues")
            print()
            continue

        drifts = [r["pos"] - r["cue_start"] for r in scored]
        worst = max(drifts, key=abs)
        worst_overall = max(worst_overall, abs(worst))
        bad = [d for d in drifts if d < -0.25 or d > CUE_WINDOW + 0.25]

        print("segment %d  pos %.1f..%.1f  %d samples, %d with a cue"
              % (n, lo, hi, len(seg), len(scored)))
        print("    base=%.3f  drift min/median/max = %+.3f / %+.3f / %+.3f s"
              % (seg[0]["base"], min(drifts),
                 sorted(drifts)[len(drifts) // 2], max(drifts)))
        if bad:
            print("    %d/%d samples outside the cue's own [0, %.1fs] window"
                  % (len(bad), len(drifts), CUE_WINDOW))
            for r in scored:
                d = r["pos"] - r["cue_start"]
                if d < -0.25 or d > CUE_WINDOW + 0.25:
                    print("      pos=%8.3f base=%8.3f clk=%8.3f  cue=%-28s drift=%+.3f"
                          % (r["pos"], r["base"], r["clk"], r["cue"][:28], d))
                    break
            verdicts.append("DESYNC")
        else:
            print("    in sync")
            verdicts.append("ok")
        print()

    print("=" * 62)
    for n, v in enumerate(verdicts, 1):
        print("  segment %d: %s" % (n, v))
    ok = all(v == "ok" for v in verdicts)
    print("\n%s  (worst |drift| %.3f s)"
          % ("PASS" if ok else "FAIL", worst_overall))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default="output/logs/evo.log")
    ap.add_argument("--after", type=float, metavar="T",
                    help="keep only lines at or after console time T (the `t=` "
                         "field in evo_status) - isolate one run out of a log "
                         "that has more than one")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    sys.exit(report(parse(args.log, args.after)))


if __name__ == "__main__":
    main()
