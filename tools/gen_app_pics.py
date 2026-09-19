#!/usr/bin/env python3
"""
Generate pic0.png and pic1.png — the shell artwork behind the app.

WHY THIS EXISTS
    package-app.sh has always copied these:

        for asset in icon0.png pic0.png pic1.png snd0.at9; do

    and neither has ever existed, so EVO sat on a plain shell background while
    every commercial title showed artwork. param.json even carries a
    loudnessSnd0 value, so the packaging expected a full asset set.

    Built from the same vector description and palette as the icon, in the
    player's own font, so the tile and the background are recognisably one
    thing rather than two designs.

DESIGN
    Backgrounds sit behind system UI, so this is deliberately quiet: the mark
    is oversized, low-contrast and bleeds off the right edge as a watermark,
    with the lockup set small at the lower left. Anything busier fights the
    shell's own text.

USAGE
    python3 tools/gen_app_pics.py [--width 1920] [--height 1080]

OUTPUT
    projects/evoplayer/sce_sys/pic1.png      background art
    projects/evoplayer/sce_sys/pic0.png      same image, as the packager wants both
    output/icons/pic1_preview.png
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_app_icon_variants as V
from gen_icons import write_png


def render_pic(w, h, face="Lato-Bold"):
    cv = V.Canvas(w, face, height=h, tile=False)
    s = float(h)

    # Oversized mark, bled off the right edge and held well back in contrast so
    # it reads as texture rather than a logo sitting on the wallpaper.
    mx = w * 0.80
    my = h * 0.46
    V.glow(cv, my, s * 0.30, cx=mx, steps=14, strength=0.030)
    V.ring(cv, my, s * 0.300, s * 0.052, cx=mx, alpha=0.30)
    V.play(cv, my, s * 0.158, dx=s * 0.018, base=mx, alpha=0.30)

    # The lockup, small, lower left - where a shell background is usually least
    # covered by system furniture.
    lx = w * 0.085
    ly = h * 0.760
    cap = s * 0.092
    f = V.font(face)
    w_evo = f.measure("EVO", cap, 0.02)
    w_pro = f.measure("PRO", cap, 0.02)
    gap = cap * 0.34
    cv.text("EVO", cap, lx + w_evo / 2.0, ly, V.WHITE, tracking=0.02)
    cv.text("PRO", cap, lx + w_evo + gap + w_pro / 2.0, ly, V.ACCENT, tracking=0.02)

    # A thin accent rule under the lockup, tying it to the tile's hairline edge.
    rule_w = w_evo + gap + w_pro
    cv.shape(
        lambda px, py: V.sd_round_rect(px, py, lx + rule_w / 2.0,
                                       ly + cap * 0.92, rule_w / 2.0,
                                       s * 0.0035, s * 0.0035),
        V.ACCENT,
        (lx - 2, ly + cap * 0.92 - s * 0.01, lx + rule_w + 2, ly + cap * 0.92 + s * 0.01),
        0.75)
    return cv.rows()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--font", default="Lato-Bold")
    args = ap.parse_args()

    rows = render_pic(args.width, args.height, args.font)
    sce = os.path.join(ROOT, "projects", "evoplayer", "sce_sys")
    prev = os.path.join(ROOT, "output", "icons")
    os.makedirs(prev, exist_ok=True)

    for name in ("pic1.png", "pic0.png"):
        dst = os.path.join(sce, name)
        write_png(dst, rows, args.width, args.height)
        print("wrote", dst)
    write_png(os.path.join(prev, "pic1_preview.png"), rows, args.width, args.height)


if __name__ == "__main__":
    main()
