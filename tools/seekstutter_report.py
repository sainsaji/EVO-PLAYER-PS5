#!/usr/bin/env python3
"""
tools/seekstutter_report.py — measure the post-seek stutter from evo.log.

    python3 tools/seekstutter_report.py output/logs/evo.log

Reads the `seekstutter` lines EVO writes when /mnt/usb0/evo_seektrace exists:
one SEEK_BEGIN, one SEEK_COMMIT, and one `tick` line per render-loop iteration
for the 6s window after the seek commits (media/main.c, right next to the #8
present-timing hooks — reuses their microsecond clock).

Each tick already carries the three candidates for "where did the felt hitch
come from": osd_us (draw_player_screen CPU rasterise + RmlUi OSD doc),
blit_us (texture upload + YUV->RGB / composite draw), swap_us (eglSwapBuffers
— mostly the vblank wait, so NOT a hitch source, see docs/validation.md's #8
section on why that half is excluded from the sweep's GPU column too). This
script does not have to guess which one: it just finds the slow tick(s) and
prints what made them slow.

A frame period is display_pts_us delta between consecutive *new-video* ticks
under normal playback; a stutter is a wall-clock gap between presents that is
much larger than that — video visibly hitching, whatever caused it.
"""

import argparse
import re
import sys

BEGIN_RE = re.compile(
    r"\[(?P<t>\d+\.\d+)\]\s+seekstutter SEEK_BEGIN target_pts=(?P<target>-?\d+)")
COMMIT_RE = re.compile(
    r"\[(?P<t>\d+\.\d+)\]\s+seekstutter SEEK_COMMIT pts=(?P<pts>-?\d+) "
    r"to_first_frame_us=(?P<t2f>\d+)")
TICK_RE = re.compile(
    r"\[(?P<t>\d+\.\d+)\]\s+seekstutter tick swap=(?P<swap>\d) "
    r"newvid=(?P<newvid>\d) have=(?P<have>\d) held=(?P<held>\d) "
    r"disc=(?P<disc>\d) pts=(?P<pts>-?\d+) "
    r"osd_act=(?P<osd_act>\d) osd_chg=(?P<osd_chg>\d) "
    r"osd_us=(?P<osd_us>\d+) blit_us=(?P<blit_us>\d+) "
    r"swap_us=(?P<swap_us>\d+) tot_ms=(?P<tot_ms>\d+)")

# A tick this many ms after the previous *presented* tick counts as a visible
# hitch. 24-60fps content has a 16.7-41.7ms period; give it real margin so a
# normal frame doesn't trip it, but a felt "small stutter" (order ~100ms+)
# does.
STUTTER_MS = 80.0


def parse(path):
    events = []   # ('begin'|'commit'|'tick', t, fields)
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = BEGIN_RE.search(line)
            if m:
                events.append(("begin", float(m.group("t")),
                              {"target_pts": int(m.group("target"))}))
                continue
            m = COMMIT_RE.search(line)
            if m:
                events.append(("commit", float(m.group("t")),
                              {"pts": int(m.group("pts")),
                               "to_first_frame_us": int(m.group("t2f"))}))
                continue
            m = TICK_RE.search(line)
            if m:
                d = m.groupdict()
                events.append(("tick", float(d["t"]), {
                    "swap": d["swap"] == "1", "newvid": d["newvid"] == "1",
                    "have": d["have"] == "1", "held": d["held"] == "1",
                    "disc": d["disc"] == "1", "pts": int(d["pts"]),
                    "osd_act": d["osd_act"] == "1", "osd_chg": d["osd_chg"] == "1",
                    "osd_us": int(d["osd_us"]), "blit_us": int(d["blit_us"]),
                    "swap_us": int(d["swap_us"]), "tot_ms": int(d["tot_ms"]),
                }))
    return events


def windows(events):
    """
    Group into one window per SEEK_BEGIN: everything from that begin up to
    (not including) the next begin. A window therefore holds BOTH phases of
    one seek — the discard-window ticks (disc=1, hold_buf should be freezing
    the picture) up to SEEK_COMMIT, then the post-commit ticks (disc=0) out to
    the 6s trace cutoff. Grouping by begin rather than commit is what makes the
    discard-phase ticks attributable to the right seek instead of leaking into
    the previous window's tail.
    """
    out = []
    cur = None
    for kind, t, f in events:
        if kind == "begin":
            if cur:
                out.append(cur)
            cur = {"begin_t": t, "begin": f, "commit_t": None, "commit": None,
                  "ticks": []}
        elif kind == "commit" and cur is not None:
            cur["commit_t"] = t
            cur["commit"] = f
        elif kind == "tick" and cur is not None:
            cur["ticks"].append((t, f))
    if cur:
        out.append(cur)
    return [w for w in out if w["ticks"]]


def analyse_window(n, w):
    ticks = w["ticks"]

    if w["commit"] is None:
        print("seek %d  target_pts=%d  NEVER COMMITTED in this log window "
             "(still discarding, or cut off) — %d discard-phase ticks seen"
             % (n, w["begin"]["target_pts"], len(ticks)))
        return "inconclusive"

    print("seek %d  commit pts=%d  seek latency (begin->first frame) %.1f ms"
          % (n, w["commit"]["pts"], w["commit"]["to_first_frame_us"] / 1000.0))

    disc_ticks = [(t, f) for (t, f) in ticks if f["disc"]]
    post_ticks = [(t, f) for (t, f) in ticks if not f["disc"]]

    # The discard window: hold_buf is supposed to freeze the picture smoothly
    # here. A stall to worry about is a tick with have=0 (nothing to show —
    # a black frame, if this ever happens) or a swap-to-swap gap far above the
    # steady-state cadence while `held` is set, i.e. the freeze itself glitching.
    if disc_ticks:
        no_pic = sum(1 for _, f in disc_ticks if not f["have"])
        disc_presented = [(t, f) for (t, f) in disc_ticks if f["swap"]]
        disc_gaps = [(disc_presented[i][0] - disc_presented[i - 1][0]) * 1000.0
                    for i in range(1, len(disc_presented))]
        worst_disc = max(disc_gaps) if disc_gaps else 0.0
        print("    discard window: %d ticks (%d presented), %d with nothing to "
             "show, worst swap gap %.1fms"
             % (len(disc_ticks), len(disc_presented), no_pic, worst_disc))
        if no_pic:
            print("    ** %d tick(s) had NO picture during the freeze — a black "
                 "flash, not just a held frame **" % no_pic)
        if worst_disc > STUTTER_MS:
            print("    ** the frozen-frame hold itself has a %.1fms gap — the "
                 "hold_buf freeze is not smooth **" % worst_disc)

    ticks = post_ticks   # everything below judges the post-commit cadence only
    presented = [(t, f) for (t, f) in ticks if f["swap"]]
    print("    post-commit: %d render-loop ticks in the 6s window, %d actually "
         "swapped" % (len(ticks), len(presented)))

    if len(presented) < 2:
        print("    not enough presented frames to judge cadence")
        return "inconclusive"

    hitches = []
    for i in range(1, len(presented)):
        t0, f0 = presented[i - 1]
        t1, f1 = presented[i]
        gap_ms = (t1 - t0) * 1000.0
        if gap_ms > STUTTER_MS:
            hitches.append((i, t1, gap_ms, f0, f1))

    if not hitches:
        print("    smooth - no swap-to-swap gap over %.0f ms" % STUTTER_MS)
        return "smooth"

    print("    %d hitch(es) over %.0f ms:" % (len(hitches), STUTTER_MS))
    for i, t1, gap_ms, f0, f1 in hitches:
        # Blame the frame that landed AFTER the gap - it's the one that took
        # too long to get to screen (or the one before it stalled the loop;
        # both f0's and f1's own costs are printed so it's checkable either way).
        print("      #%-3d gap=%.1fms  after-frame: osd_us=%d blit_us=%d swap_us=%d "
             "newvid=%d osd_chg=%d pts_delta=%d"
             % (i, gap_ms, f1["osd_us"], f1["blit_us"], f1["swap_us"],
                f1["newvid"], f1["osd_chg"], f1["pts"] - f0["pts"]))

    # Attribute automatically where it's unambiguous: one cost dominates the gap.
    worst = max(hitches, key=lambda h: h[2])
    _, _, gap_ms, f0, f1 = worst
    costs = {"osd_draw": f1["osd_us"] / 1000.0, "blit": f1["blit_us"] / 1000.0,
             "swap(vblank)": f1["swap_us"] / 1000.0}
    dominant = max(costs, key=costs.get)
    if costs[dominant] > gap_ms * 0.4:
        print("    worst gap %.1fms is dominated by %s (%.1fms of it)"
             % (gap_ms, dominant, costs[dominant]))
    else:
        print("    worst gap %.1fms is NOT explained by this frame's own "
             "osd/blit/swap costs (largest is %s at %.1fms) - the loop was "
             "blocked or delayed by something outside these three, or a "
             "frame was silently coalesced (decode outpaced presentation)"
             % (gap_ms, dominant, costs[dominant]))
    return "hitch"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default="output/logs/evo.log")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass

    events = parse(args.log)
    ws = windows(events)
    if not ws:
        sys.exit("no `seekstutter` SEEK_COMMIT+tick windows found - was "
                 "/mnt/usb0/evo_seektrace created, and did a seek happen "
                 "on this build?")

    verdicts = []
    for n, w in enumerate(ws, 1):
        verdicts.append(analyse_window(n, w))
        print()

    print("=" * 62)
    print("%d seek(s): %d smooth, %d with a hitch, %d inconclusive"
          % (len(verdicts), verdicts.count("smooth"),
             verdicts.count("hitch"), verdicts.count("inconclusive")))
    sys.exit(1 if "hitch" in verdicts else 0)


if __name__ == "__main__":
    main()
