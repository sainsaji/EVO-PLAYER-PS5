#!/usr/bin/env python3
"""
tools/sweep_report.py — turn the `sweep` lines in evo.log into the
docs/validation.md codec table.

    python3 tools/sweep_report.py output/logs/evo.log -o output/logs/sweep.md

Runs on the host with nothing but a log file, so a sweep can be re-rendered
after the fact and a table regenerated from someone else's log.

Each played file writes exactly one `sweep v=1 ... file=<name>` line when its
decoder closes (projects/evoplayer/media/src/evo_sweep.c). A clip played more
than once in a log keeps its LAST row, so a re-run of one file corrects it
without re-running the whole sweep.
"""

import argparse
import re
import sys

# `file=` is last on the line precisely so a name with spaces survives parsing.
SWEEP_RE = re.compile(r"\bsweep v=1 (.*?)\bfile=(.*)$")
# evo.log's own timestamp prefix, e.g. "[11123.780] ". Seconds since boot.
STAMP_RE = re.compile(r"^\[(\d+(?:\.\d+)?)\]")


def parse_log(text, after=None):
    """
    evo.log is cumulative across launches, so a log can hold rows from several
    sweeps. `after` (console seconds, from the `t=` in evo_status) keeps only
    rows written since then — what you want when reporting one run rather than
    everything the console has ever played.
    """
    rows = {}
    order = []
    for line in text.splitlines():
        if after is not None:
            st = STAMP_RE.match(line)
            if not st or float(st.group(1)) < after:
                continue
        m = SWEEP_RE.search(line)
        if not m:
            continue
        fields = {}
        for tok in m.group(1).split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                fields[k] = v
        name = m.group(2).strip()
        fields["file"] = name
        if name not in rows:
            order.append(name)
        rows[name] = fields          # last one wins
    return [rows[n] for n in order]


def f(row, key, default=0.0):
    try:
        return float(row.get(key, default))
    except (TypeError, ValueError):
        return default


def i(row, key, default=0):
    try:
        return int(float(row.get(key, default)))
    except (TypeError, ValueError):
        return default


VERDICT_TEXT = {
    "realtime":      "✅ real-time",
    "slow_decode":   "🔴 decode too slow",
    "slow_pipeline": "🟠 drops frames",
    "no_decoder":    "⚫ no decoder",
    "no_frames":     "🔴 no frames",
    "decode_error":  "🔴 decode error",
    "failed":        "🔴 open failed",
}


def verdict_cell(row):
    v = row.get("verdict", "?")
    text = VERDICT_TEXT.get(v, v)
    # Say *why* it was too slow, in the units the reader needs to act on.
    if v == "slow_decode":
        budget = f(row, "budget_ms")
        p95 = f(row, "dec_ms_p95")
        if budget > 0:
            text += " (%.1fx budget)" % (p95 / budget)
    elif v == "slow_pipeline":
        pub, late = i(row, "pub"), i(row, "drop_late")
        if pub:
            text += " (%.0f%% late)" % (100.0 * late / pub)
    return text


def render(rows, colour_ref=None):
    out = []
    out.append("| Clip | Codec | Res | Backend | Decode ms/frame (avg / p95) | "
               "Budget | GPU ms (avg / p95) | Frames | Dropped (late / seek) | "
               "Colour | Verdict |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        name = r.get("file", "?")
        codec = r.get("codec", "?")
        res = r.get("res", "?")
        be = r.get("be", "-")
        open_res = r.get("open", "ok")
        if open_res == "downgraded":
            be += " ⚠"          # native was asked for and could not be given
        dec = ("%.2f / %.2f" % (f(r, "dec_ms_avg"), f(r, "dec_ms_p95"))
               if i(r, "dec_n") else "—")
        budget = "%.1f" % f(r, "budget_ms") if f(r, "budget_ms") else "—"
        # GPU = upload + YUV->RGB + OSD composite. The eglSwapBuffers half is
        # reported separately in the log (swap_ms_avg) and deliberately kept out
        # of the table: it is the vblank wait, so it reads ~16 ms for every
        # clip whatever the pipeline is doing.
        pres = ("%.2f / %.2f" % (f(r, "gpu_ms_avg"), f(r, "gpu_ms_p95"))
                if i(r, "pres_n") else "—")
        played = "dec_n" in r
        frames = str(i(r, "dec_n")) if played else "—"
        drops = ("%d / %d" % (i(r, "drop_late"), i(r, "drop_seek"))
                 if played else "—")
        if not played:
            be = "—"

        sig = r.get("sig", "00000000")
        rgb = r.get("rgb", "000000")
        if sig in (None, "00000000"):
            colour = "not probed"
        elif colour_ref and name in colour_ref:
            colour = "✅ match" if colour_ref[name] == sig else "🔴 **differs**"
            colour += " `%s`" % sig[:8]
        else:
            colour = "`%s` #%s" % (sig[:8], rgb)

        out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
                   % (name, codec, res, be, dec, budget, pres, frames, drops,
                      colour, verdict_cell(r)))
    return "\n".join(out)


def rgb_triple(row):
    try:
        v = int(row.get("rgb", "0"), 16)
    except ValueError:
        return None
    if not row.get("sig") or row.get("sig") == "00000000":
        return None
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def colour_crosscheck(rows, tol=6):
    """
    The `colour_*` clips are ONE source pattern encoded through several codecs,
    so on a correct player they land on the same picture. That makes them
    decidable on a single run: compare them against each other, and whichever
    disagrees owns the bug — no baseline from a previous run needed.

    Compares mean probe RGB, not the hash: the encodes are lossy and chroma-
    subsampled, so the sampled pixels differ by a couple of levels even when
    every path is correct. A real colour fault (wrong matrix, wrong transfer,
    swapped channels, wrong range) moves the mean by far more than `tol`.
    """
    group = [r for r in rows if "colour_" in r.get("file", "") and rgb_triple(r)]
    if len(group) < 2:
        return ""

    chans = list(zip(*(rgb_triple(r) for r in group)))
    median = tuple(sorted(c)[len(c) // 2] for c in chans)

    out = ["", "**Colour cross-check** — same source pattern, %d codecs. "
               "Reference is the per-channel median #%02x%02x%02x; a path that "
               "disagrees by more than %d/255 has a colour bug."
               % (len(group), median[0], median[1], median[2], tol), "",
           "| Clip | Codec | Mean RGB | Δ from median | |",
           "|---|---|---|---|---|"]
    worst = 0
    for r in sorted(group, key=lambda x: x.get("file", "")):
        t = rgb_triple(r)
        d = max(abs(a - b) for a, b in zip(t, median))
        worst = max(worst, d)
        out.append("| %s | %s | #%02x%02x%02x | %d | %s |"
                   % (r.get("file", "?"), r.get("codec", "?"), t[0], t[1], t[2],
                      d, "✅" if d <= tol else "🔴 **differs**"))
    out.append("")
    out.append("Worst disagreement: **%d/255** — %s."
               % (worst, "PASS" if worst <= tol else "FAIL, investigate the "
                  "flagged path"))
    return "\n".join(out)


def summarise(rows):
    n = len(rows)
    by = {}
    for r in rows:
        by[r.get("verdict", "?")] = by.get(r.get("verdict", "?"), 0) + 1
    parts = ["%d clips" % n]
    for k in ("realtime", "slow_decode", "slow_pipeline", "no_decoder",
              "no_frames", "decode_error", "failed"):
        if by.get(k):
            parts.append("%d %s" % (by[k], k.replace("_", " ")))
    native = sum(1 for r in rows if r.get("be") == "native")
    parts.append("%d native / %d software" % (native, n - native))
    return " · ".join(parts)


def load_colour_ref(path):
    """A previous run's table, used as the colour baseline: file -> sig."""
    ref = {}
    with open(path, "r", encoding="utf-8") as fh:
        for r in parse_log(fh.read()):
            if r.get("sig") and r["sig"] != "00000000":
                ref[r["file"]] = r["sig"]
    return ref


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="evo.log containing the sweep lines")
    ap.add_argument("-o", "--out", help="write the markdown here (default stdout)")
    ap.add_argument("--after", type=float, metavar="T",
                    help="keep only rows written after console time T "
                         "(the `t=` field in evo_status) - use it to report a "
                         "single sweep out of a cumulative log")
    ap.add_argument("--colour-ref", metavar="EVO.LOG",
                    help="an earlier evo.log to compare colour signatures "
                         "against — turns the Colour column into match/differs")
    args = ap.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
        rows = parse_log(fh.read(), args.after)
    if not rows:
        sys.exit("no `sweep v=1` lines in %s — was the build recent enough, "
                 "and did anything actually play?" % args.log)

    ref = load_colour_ref(args.colour_ref) if args.colour_ref else None
    body = render(rows, ref)
    text = body + "\n\n" + summarise(rows) + "\n" + colour_crosscheck(rows) + "\n"

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("wrote %s (%d clips)" % (args.out, len(rows)))
    else:
        # The table is full of status emoji and a Windows console defaults to
        # cp1252, which cannot encode them.
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except AttributeError:
            pass
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
