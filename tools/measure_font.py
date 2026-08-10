#!/usr/bin/env python3
"""
tools/measure_font.py - read the real metrics out of the RR font atlas.

Why this exists
---------------
Adding punctuation to the UI font means drawing glyphs that sit on the same
baseline, at the same weight, as the ones already in assets/renderer_reset_assets.h.
That header is a 4096x260 alpha atlas plus per-face metric tables; it carries no
baseline, no cap height and no x-height, and guessing them puts a comma
floating in the middle of the line.

So measure them. This walks the ink of glyphs whose shape is known - a capital,
an x-height lowercase, a descender, the period, the colon and the hyphen - and
reports, per face:

    baseline      the y the letters sit on, in cell coordinates
    cap top       top of a capital
    x top         top of a lowercase x
    descender     bottom of a p
    dot size      the period's ink box, which sets punctuation weight
    dot left      the period's left bearing inside its cell

The numbers it prints are pasted into gen_icons.py as calibration constants,
with this script kept so they can be re-derived rather than trusted.

    python3 tools/measure_font.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ATLAS = os.path.join(ROOT, "projects", "evoplayer", "assets",
                     "renderer_reset_assets.h")

# RR_CHARS, from the same header. Index into every metric table.
CHARS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
         "abcdefghijklmnopqrstuvwxyz"
         "0123456789 /._:-+")

FACES = ("TITLE", "MENU", "SUB", "SMALL")

# Glyphs whose vertical extents define the grid.
PROBES = [
    ("H", "cap"),        # flat-topped capital: cap height and baseline
    ("x", "xheight"),    # flat lowercase: x-height
    ("p", "descender"),  # descender depth
    (".", "period"),     # punctuation weight and bearing
    (":", "colon"),
    ("-", "hyphen"),
]


def load_atlas():
    """Pull RR_FONT_W/H and the byte array out of the C header."""
    if not os.path.exists(ATLAS):
        sys.exit(f"atlas not found: {ATLAS}")

    w = h = None
    metrics = {}
    data = None

    with open(ATLAS, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#define RR_FONT_W"):
                w = int(line.split()[2])
            elif line.startswith("#define RR_FONT_H"):
                h = int(line.split()[2])
            elif line.startswith("static const int RR_"):
                m = re.match(r'static const int (RR_\w+)\[\]=\{([^}]*)\};', line)
                if m:
                    metrics[m.group(1)] = [int(v) for v in m.group(2).split(",")]
            elif line.startswith("static const unsigned char RR_FONT[]"):
                # The array opens on this line and runs to the closing brace.
                body = line.split("{", 1)[1]
                chunks = [body]
                for rest in f:
                    chunks.append(rest)
                    if "}" in rest:
                        break
                blob = "".join(chunks)
                blob = blob[:blob.rindex("}")]
                data = bytes(int(v) for v in blob.replace("\n", "").split(",") if v.strip())
                break

    if w is None or h is None or data is None:
        sys.exit("could not parse the atlas header")
    return w, h, metrics, data


def ink_box(data, atlas_w, sx, sy, sw, sh, threshold=18):
    """Bounding box of pixels above the same threshold rr_text uses (ma > 18)."""
    x0 = y0 = 10**9
    x1 = y1 = -1
    for y in range(sh):
        for x in range(sw):
            if data[(sy + y) * atlas_w + (sx + x)] > threshold:
                x0, y0 = min(x0, x), min(y0, y)
                x1, y1 = max(x1, x), max(y1, y)
    if x1 < 0:
        return None
    return x0, y0, x1, y1


def main():
    atlas_w, atlas_h, metrics, data = load_atlas()
    print(f"atlas {atlas_w}x{atlas_h}, {len(data)} bytes\n")

    for face in FACES:
        X = metrics[f"RR_{face}_X"]
        Y = metrics[f"RR_{face}_Y"]
        W = metrics[f"RR_{face}_W"]
        H = metrics[f"RR_{face}_H"]
        A = metrics[f"RR_{face}_ADV"]

        print(f"=== {face}  cell {W[0]}x{H[0]}  (all cells share height {H[0]}) ===")
        rows = {}
        for ch, label in PROBES:
            i = CHARS.find(ch)
            if i < 0:
                continue
            box = ink_box(data, atlas_w, X[i], Y[i], W[i], H[i])
            if box is None:
                print(f"  {label:<10} (no ink)")
                continue
            x0, y0, x1, y1 = box
            rows[label] = box
            print(f"  {label:<10} '{ch}'  ink x {x0:>3}..{x1:<3} "
                  f"y {y0:>3}..{y1:<3}  w {x1-x0+1:>2} h {y1-y0+1:>2}  adv {A[i]}")

        if "cap" in rows and "period" in rows:
            baseline = rows["cap"][3] + 1          # first row below the capital
            cap_top = rows["cap"][1]
            print(f"  -> baseline y  = {baseline}")
            print(f"  -> cap height  = {baseline - cap_top}")
            if "xheight" in rows:
                print(f"  -> x height    = {baseline - rows['xheight'][1]}")
            if "descender" in rows:
                print(f"  -> descender   = {rows['descender'][3] + 1 - baseline}")
            px0, py0, px1, py1 = rows["period"]
            print(f"  -> dot size    = {px1-px0+1} x {py1-py0+1}")
            print(f"  -> dot left    = {px0}   (bearing inside the cell)")
            print(f"  -> dot bottom  = {py1 + 1}  (vs baseline {baseline})")
        if "hyphen" in rows:
            hx0, hy0, hx1, hy1 = rows["hyphen"]
            print(f"  -> hyphen bar  = {hx1-hx0+1} x {hy1-hy0+1} at y {hy0}")
        print()


if __name__ == "__main__":
    main()
