#!/usr/bin/env python3
"""
Candidate app icons, for picking one before it becomes icon0.png.

The wordmarks are set in the player's own font (Lato-Bold, the same face RmlUi
renders the interface with) via tools/ttf_text.py, rather than approximated
with line segments and circles as gen_app_icon.py did. The hand-drawn letters
were why the first PRO attempt looked wrong - P and R have a bowl that no
arrangement of primitives fakes.

USAGE
    python3 tools/gen_app_icon_variants.py            # contact sheet
    python3 tools/gen_app_icon_variants.py --pick 3   # one variant at 512
    python3 tools/gen_app_icon_variants.py --font Roboto-Bold

OUTPUT
    output/icons/app_icon_variants.png     contact sheet, tile size and small
    output/icons/app_icon_v<N>.png         one variant at 512 (with --pick)
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import ttf_text
from gen_icons import sd_circle, sd_polygon, sd_round_rect, write_png

ACCENT = (0x00, 0xCD, 0xFF)
ACCENT_DEEP = (0x00, 0x6A, 0xA8)
WHITE = (255, 255, 255)
BG_TOP = (13, 19, 32)
BG_BOTTOM = (6, 31, 56)

FONT_DIR = os.path.join(ROOT, "projects", "evoplayer", "assets", "fonts")
_font_cache = {}


def font(name):
    if name not in _font_cache:
        _font_cache[name] = ttf_text.Font(os.path.join(FONT_DIR, name + ".ttf"))
    return _font_cache[name]


def mix(c1, c2, t):
    return tuple(c1[i] + (c2[i] - c1[i]) * t for i in range(3))


def cov(d):
    return max(0.0, min(1.0, 0.5 - d))


# --------------------------------------------------------------------------

class Canvas:
    """RGB pixel buffer with the tile's alpha kept alongside, so text blitted
    after the shapes still gets clipped to the rounded rect."""

    def __init__(self, size, face, height=None, tile=True):
        self.n = size
        self.w = size
        self.h = height or size
        self.face = face
        s = float(size)
        c = s / 2.0
        self.s, self.c = s, c
        self.cy = self.h / 2.0
        self.px = [[(0.0, 0.0, 0.0)] * self.w for _ in range(self.h)]
        self.alpha = [[0.0] * self.w for _ in range(self.h)]
        self.ground = [[(0.0, 0.0, 0.0)] * self.w for _ in range(self.h)]
        if not tile:
            for y in range(self.h):
                g = mix(BG_TOP, BG_BOTTOM, y / float(self.h))
                for x in range(self.w):
                    sheen = max(0.0, (x + y) / float(self.w + self.h) - 0.45) * 0.18
                    col = mix(g, ACCENT_DEEP, sheen)
                    self.px[y][x] = col
                    self.ground[y][x] = col
                    self.alpha[y][x] = 1.0
            return

        tile_r = s * 0.18
        hw = s / 2.0 - s * 0.012
        for y in range(self.h):
            py = y + 0.5
            for x in range(self.w):
                px = x + 0.5
                d_tile = sd_round_rect(px, py, c, c, hw, hw, tile_r)
                grad = mix(BG_TOP, BG_BOTTOM, py / s)
                sheen = max(0.0, (px + py) / (2.0 * s) - 0.45) * 0.18
                col = mix(grad, ACCENT_DEEP, sheen)
                self.ground[y][x] = col
                d_edge = abs(d_tile + s * 0.018) - s * 0.004
                col = mix(col, ACCENT, cov(d_edge) * 0.55)
                self.px[y][x] = col
                self.alpha[y][x] = cov(d_tile)

    def shape(self, fn, colour, bbox=None, alpha=1.0):
        """
        Composite a signed-distance shape.

        bbox is (x0, y0, x1, y1) in pixels: without it every shape is evaluated
        against every pixel, which is tolerable for a 512px tile and far too
        slow for a 1920x1080 background. Shapes that know their own extent pass
        it; anything that does not still works, just slowly.
        """
        if bbox is None:
            x0, y0, x1, y1 = 0, 0, self.w, self.h
        else:
            x0 = max(0, int(bbox[0])); y0 = max(0, int(bbox[1]))
            x1 = min(self.w, int(bbox[2]) + 1); y1 = min(self.h, int(bbox[3]) + 1)
        for y in range(y0, y1):
            py = y + 0.5
            row = self.px[y]
            for x in range(x0, x1):
                a = cov(fn(x + 0.5, py)) * alpha
                if a > 0.0:
                    row[x] = mix(row[x], colour, a)

    def text(self, s, cap, cx, cy, colour, tracking=0.0, knockout=False):
        mask, mw, mh = font(self.face).text_mask(s, cap, tracking=tracking)
        ox = int(round(cx - mw / 2.0))
        oy = int(round(cy - mh / 2.0))
        for yy in range(mh):
            ty = oy + yy
            if not (0 <= ty < self.h):
                continue
            for xx in range(mw):
                tx = ox + xx
                if not (0 <= tx < self.w):
                    continue
                a = mask[yy * mw + xx]
                if a <= 0.0:
                    continue
                c = self.ground[ty][tx] if knockout else colour
                self.px[ty][tx] = mix(self.px[ty][tx], c, a)

    def rows(self):
        out = []
        for y in range(self.h):
            row = bytearray()
            for x in range(self.w):
                a = self.alpha[y][x]
                r, g, b = self.px[y][x]
                row += bytes((int(r * a + 0.5), int(g * a + 0.5), int(b * a + 0.5)))
            out.append(row)
        return out


def ring(cv, cy, r, w, cx=None, colour=ACCENT, alpha=1.0):
    cx = cv.c if cx is None else cx
    e = r + w
    cv.shape(lambda px, py: abs(sd_circle(px, py, cx, cy, r)) - w / 2.0, colour,
             (cx - e, cy - e, cx + e, cy + e), alpha)


def play(cv, cy, r, dx=0.0, base=None, colour=ACCENT, alpha=1.0):
    cx = (cv.c if base is None else base) + dx
    tri = [(cx - r * 0.70, cy - r), (cx - r * 0.70, cy + r), (cx + r * 0.98, cy)]
    cv.shape(lambda px, py: sd_polygon(px, py, tri), colour,
             (cx - r, cy - r, cx + r, cy + r), alpha)


def glow(cv, cy, r, cx=None, steps=5, strength=0.16):
    """
    Soft accent halo behind the mark.

    Concentric low-alpha discs rather than a real blur - there is no
    convolution here and none is needed: five steps at 16% read as a glow at
    tile size and cost almost nothing, because each is bbox-limited.
    """
    cx = cv.c if cx is None else cx
    for i in range(steps, 0, -1):
        rr = r * (1.0 + 0.22 * i)
        a = strength * (1.0 - i / float(steps + 1))
        cv.shape(lambda px, py, rr=rr: sd_circle(px, py, cx, cy, rr), ACCENT,
                 (cx - rr, cy - rr, cx + rr, cy + rr), a)


def pill(cv, cx, cy, hw, hh):
    cv.shape(lambda px, py: sd_round_rect(px, py, cx, cy, hw, hh, hh), ACCENT,
             (cx - hw - 2, cy - hh - 2, cx + hw + 2, cy + hh + 2))


# --------------------------------------------------------------------------
# candidates
# --------------------------------------------------------------------------

def v1(cv):
    """Mark only. No type at all."""
    s, c = cv.s, cv.c
    ring(cv, c, s * 0.275, s * 0.060)
    play(cv, c, s * 0.146, s * 0.015)


def v2(cv):
    """Mark with a PRO pill in the corner. Wordmark dropped."""
    s, c = cv.s, cv.c
    ring(cv, c - s * 0.030, s * 0.250, s * 0.056)
    play(cv, c - s * 0.030, s * 0.133, s * 0.015)
    bx, by = c + s * 0.250, c + s * 0.335
    pill(cv, bx, by, s * 0.155, s * 0.066)
    cv.text("PRO", s * 0.072, bx, by, WHITE, tracking=0.04, knockout=True)


def v3(cv):
    """Mark above, EVO PRO set on one line beneath, measured not guessed."""
    s, c = cv.s, cv.c
    ring(cv, c - s * 0.115, s * 0.205, s * 0.048)
    play(cv, c - s * 0.115, s * 0.109, s * 0.013)

    cap, tr = s * 0.140, 0.02
    f = font(cv.face)
    w_evo = f.measure("EVO", cap, tr)
    w_pro = f.measure("PRO", cap, tr)
    gap = cap * 0.34
    total = w_evo + gap + w_pro
    left = c - total / 2.0
    y = c + s * 0.250
    cv.text("EVO", cap, left + w_evo / 2.0, y, WHITE, tracking=tr)
    cv.text("PRO", cap, left + w_evo + gap + w_pro / 2.0, y, ACCENT, tracking=tr)


def v4(cv):
    """Wordmark-led: EVO large, PRO in an accent pill beneath."""
    s, c = cv.s, cv.c
    cv.text("EVO", s * 0.290, c, c - s * 0.115, WHITE, tracking=0.01)
    by = c + s * 0.190
    pill(cv, c, by, s * 0.235, s * 0.092)
    cv.text("PRO", s * 0.108, c, by, WHITE, tracking=0.06, knockout=True)


def v5(cv):
    """Oversized play triangle, PRO pill beneath. No ring."""
    s, c = cv.s, cv.c
    play(cv, c - s * 0.055, s * 0.245, s * 0.020)
    by = c + s * 0.330
    pill(cv, c, by, s * 0.195, s * 0.074)
    cv.text("PRO", s * 0.086, c, by, WHITE, tracking=0.06, knockout=True)


def v6(cv):
    """Mark, EVO under it, PRO as a small accent line under that."""
    s, c = cv.s, cv.c
    ring(cv, c - s * 0.140, s * 0.195, s * 0.046)
    play(cv, c - s * 0.140, s * 0.104, s * 0.012)
    cv.text("EVO", s * 0.165, c, c + s * 0.175, WHITE, tracking=0.02)
    cv.text("PRO", s * 0.090, c, c + s * 0.350, ACCENT, tracking=0.34)



# --------------------------------------------------------------------------
# refinements of candidate 2: PRO moved to the top, EVO wordmark added
# --------------------------------------------------------------------------

def _pro_top(cv, cx, cy, hw, hh, cap, tracking=0.05):
    pill(cv, cx, cy, hw, hh)
    cv.text("PRO", cap, cx, cy, WHITE, tracking=tracking, knockout=True)


def r1(cv):
    """PRO centred at the top, mark in the middle, EVO along the bottom."""
    s, c = cv.s, cv.c
    _pro_top(cv, c, c - s * 0.345, s * 0.150, s * 0.060, s * 0.070)
    ring(cv, c - s * 0.010, s * 0.200, s * 0.048)
    play(cv, c - s * 0.010, s * 0.107, s * 0.013)
    cv.text("EVO", s * 0.155, c, c + s * 0.320, WHITE, tracking=0.02)


def r2(cv):
    """
    The shipping tile. PRO top-right, mark centred, EVO across the bottom.

    The mark sits a little higher and the wordmark a little lower than the
    version this was picked from: at 512 they were ~15px apart and read as one
    crowded lump rather than a lockup. A soft halo sits behind the mark so it
    does not flatten into the background on the shell's own dark ground.
    """
    s, c = cv.s, cv.c
    _pro_top(cv, c + s * 0.245, c - s * 0.340, s * 0.150, s * 0.060, s * 0.070)
    ring(cv, c - s * 0.030, s * 0.198, s * 0.047)
    play(cv, c - s * 0.030, s * 0.105, s * 0.013)
    cv.text("EVO", s * 0.158, c, c + s * 0.335, WHITE, tracking=0.02)


def r3(cv):
    """PRO top-centre, larger mark, smaller EVO - mark-led."""
    s, c = cv.s, cv.c
    _pro_top(cv, c, c - s * 0.355, s * 0.135, s * 0.053, s * 0.062)
    ring(cv, c - s * 0.020, s * 0.228, s * 0.054)
    play(cv, c - s * 0.020, s * 0.121, s * 0.014)
    cv.text("EVO", s * 0.128, c, c + s * 0.330, WHITE, tracking=0.03)


def r4(cv):
    """PRO top-centre, smaller mark, larger EVO - wordmark-led."""
    s, c = cv.s, cv.c
    _pro_top(cv, c, c - s * 0.350, s * 0.145, s * 0.057, s * 0.066)
    ring(cv, c - s * 0.030, s * 0.175, s * 0.042)
    play(cv, c - s * 0.030, s * 0.093, s * 0.011)
    cv.text("EVO", s * 0.195, c, c + s * 0.300, WHITE, tracking=0.01)


REFINE = [r1, r2, r3, r4]
REFINE_NAMES = ["A PRO centred", "B PRO corner", "C mark-led", "D wordmark-led"]

VARIANTS = [v1, v2, v3, v4, v5, v6]
NAMES = ["1 mark only", "2 corner pill", "3 one line",
         "4 wordmark", "5 triangle", "6 stacked"]


def render(fn, size, face):
    cv = Canvas(size, face)
    fn(cv)
    return cv.rows()


def sheet(face, big=232, small=88, pad=16, which=None, names=None, cols=3):
    variants = which or VARIANTS
    labels = names or NAMES
    rows_n = (len(variants) + cols - 1) // cols
    cw, ch = big + pad, big + small + pad * 2
    W, H = cols * cw + pad, rows_n * ch + pad
    canvas = [bytearray(b"\x12\x14\x18" * W) for _ in range(H)]

    def blit(img, n, ox, oy):
        for yy in range(n):
            canvas[oy + yy][ox * 3:(ox + n) * 3] = img[yy]

    for i, fn in enumerate(variants):
        cx = pad + (i % cols) * cw
        cy = pad + (i // cols) * ch
        blit(render(fn, big, face), big, cx, cy)
        blit(render(fn, small, face), small, cx + (big - small) // 2, cy + big + pad)
        print("  rendered", labels[i])
    return canvas, W, H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pick", type=int)
    ap.add_argument("--font", default="Lato-Bold")
    ap.add_argument("--refine", action="store_true",
                    help="render the PRO-on-top refinements instead")
    args = ap.parse_args()

    out = os.path.join(ROOT, "output", "icons")
    os.makedirs(out, exist_ok=True)

    pool = REFINE if args.refine else VARIANTS
    if args.pick:
        tag = "r" if args.refine else "v"
        dst = os.path.join(out, "app_icon_%s%d.png" % (tag, args.pick))
        write_png(dst, render(pool[args.pick - 1], 512, args.font), 512, 512)
        print("wrote", dst)
        return

    if args.refine:
        canvas, W, H = sheet(args.font, which=REFINE, names=REFINE_NAMES, cols=4)
        dst = os.path.join(out, "app_icon_refine.png")
    else:
        canvas, W, H = sheet(args.font)
        dst = os.path.join(out, "app_icon_variants.png")
    write_png(dst, canvas, W, H)
    print("wrote", dst, "(%dx%d)" % (W, H))


if __name__ == "__main__":
    main()
