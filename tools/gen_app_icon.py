#!/usr/bin/env python3
"""
Generate EVO Player's home-screen application icon.

WHY THIS EXISTS
    The Media tile needs a 512x512 icon0.png, and the repository had none -
    the only app icon in the tree was ProsperoPlayer's branded artwork, which
    is upstream's and cannot ship on an EVO tile. package-pkg.sh was falling
    back to a 1x1 placeholder for the same reason.

    Rather than commit a binary nobody can regenerate, the icon is described
    here as vector shapes and rasterised with the same signed-distance
    machinery as tools/gen_icons.py, so it can be restyled by editing this
    file and re-rendering at any size.

OUTPUT
    projects/evoplayer/sce_sys/icon0.png                        the app icon
    output/screenshots/app_icon_preview.png                         same image

USAGE
    python3 tools/gen_app_icon.py [--size N]

No third-party dependencies - only the standard library, matching gen_icons.py.
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gen_icons import (sd_circle, sd_polygon, sd_round_rect, sd_segment,
                       write_png)

# The UI accent, matching CYAN in gen_icons.py and the built-in themes.
ACCENT = (0x00, 0xCD, 0xFF)
ACCENT_DEEP = (0x00, 0x6A, 0xA8)
# Background gradient: near-black at the top, deep navy at the bottom. Sony's
# Media row sits on a light-to-dark shell background, so a dark tile with a
# bright mark holds up in both.
BG_TOP = (10, 14, 24)
BG_BOTTOM = (4, 22, 42)


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    return tuple(lerp(c1[i], c2[i], t) for i in range(3))


def coverage(d):
    """Analytic 1px edge, matching render() in gen_icons.py."""
    return max(0.0, min(1.0, 0.5 - d))


def wordmark_evo(px, py, cx, cy, cap, half):
    """
    Signed distance to the letters E V O, drawn as geometry rather than text.

    There is no font available here - gen_icons.py is pure stdlib and the
    player's own atlas is a C header - so the three letters are described as
    strokes. E, V and O happen to be entirely straight lines and one circle,
    which is why the wordmark is the mark and not the full product name.

    `cap` is the cap height; letters sit centred on (cx, cy).
    """
    h = cap / 2.0
    w = cap * 0.62                 # letter width
    gap = cap * 0.34               # space between letters
    advance = w + gap
    ox = cx - advance              # centre the three-letter run

    # E - stem plus three arms.
    ex = ox - w / 2.0
    d = sd_segment(px, py, ex, cy - h, ex, cy + h)
    d = min(d, sd_segment(px, py, ex, cy - h, ex + w, cy - h))
    d = min(d, sd_segment(px, py, ex, cy, ex + w * 0.82, cy))
    d = min(d, sd_segment(px, py, ex, cy + h, ex + w, cy + h))

    # V - two strokes meeting at the baseline.
    vx = cx - w / 2.0
    d = min(d, sd_segment(px, py, vx, cy - h, vx + w / 2.0, cy + h))
    d = min(d, sd_segment(px, py, vx + w, cy - h, vx + w / 2.0, cy + h))

    d -= half

    # O - a ring, so it needs its own stroke treatment.
    d_o = abs(sd_circle(px, py, cx + advance, cy, h * 0.95)) - half
    return min(d, d_o)


def wordmark_pro(px, py, cx, cy, cap, half):
    """
    Signed distance to the letters P R O.

    EVO got away with straight strokes and one circle. P and R need a bowl, so
    each is a stem plus the right half of a ring - intersected with max(), the
    signed-distance equivalent of clipping - and R adds a diagonal leg. Same
    stroke weight and cap height convention as wordmark_evo().
    """
    h = cap / 2.0
    w = cap * 0.58
    gap = cap * 0.30
    advance = w + gap
    ox = cx - advance

    bowl_r = h * 0.52

    def bowl(sx):
        """Right half of a ring, sitting on the upper half of the stem."""
        d = abs(sd_circle(px, py, sx + bowl_r * 0.55, cy - h + bowl_r,
                          bowl_r)) - half
        return max(d, sx - px)          # keep only x >= stem

    # P - stem, then the bowl.
    pxs = ox - w / 2.0
    d = sd_segment(px, py, pxs, cy - h, pxs, cy + h) - half
    d = min(d, bowl(pxs))

    # R - the same, plus a leg from the bowl's foot to the baseline.
    rxs = cx - w / 2.0
    d_r = sd_segment(px, py, rxs, cy - h, rxs, cy + h) - half
    d_r = min(d_r, bowl(rxs))
    d_r = min(d_r, sd_segment(px, py, rxs + w * 0.12, cy,
                              rxs + w * 0.92, cy + h) - half)
    d = min(d, d_r)

    # O - a ring, as in the EVO wordmark.
    d_o = abs(sd_circle(px, py, cx + advance, cy, h * 0.92)) - half
    return min(d, d_o)


def render_icon(size):
    s = float(size)
    c = s / 2.0

    # Tile geometry. The corner radius is ~18% of the edge, which is what the
    # PS5 shell's own tiles use closely enough that ours does not read as a
    # different shape sitting in the row.
    tile_r = s * 0.18
    tile_hw = s / 2.0 - s * 0.012

    # The mark sits above centre to leave room for the wordmark beneath it.
    mark_cy = c - s * 0.072
    ring_r = s * 0.215
    ring_w = s * 0.048

    word_cy = c + s * 0.250
    word_cap = s * 0.135
    word_half = s * 0.020

    # PRO badge. A filled accent pill with the letters knocked out in the tile
    # colour, so at shell size it reads as one solid shape rather than three
    # thin glyphs - 0.10.0 is a rebuild rather than a point release, and the
    # tile should say so before the app is even opened.
    pro_cy = c + s * 0.396
    pro_cap = s * 0.070
    pro_half = s * 0.0125
    pill_hw = s * 0.150
    pill_hh = s * 0.058
    pill_r = pill_hh

    # Play triangle, nudged right so its optical centre sits on the ring's
    # centre - a geometrically centred triangle always looks left-heavy.
    tri_r = s * 0.114
    tri_cx = c + s * 0.015
    tri = [
        (tri_cx - tri_r * 0.72, mark_cy - tri_r),
        (tri_cx - tri_r * 0.72, mark_cy + tri_r),
        (tri_cx + tri_r * 0.95, mark_cy),
    ]

    rows = []
    for y in range(size):
        row = bytearray()
        py = y + 0.5
        for x in range(size):
            px = x + 0.5

            # --- background tile ------------------------------------------
            d_tile = sd_round_rect(px, py, c, c, tile_hw, tile_hw, tile_r)
            a_tile = coverage(d_tile)

            grad = mix(BG_TOP, BG_BOTTOM, py / s)

            # A soft diagonal sheen across the lower right, so the tile is not
            # a flat slab. Kept under 10% or it reads as a gradient error.
            sheen = max(0.0, (px + py) / (2.0 * s) - 0.45) * 0.18
            col = mix(grad, ACCENT_DEEP, sheen)

            # --- accent hairline just inside the edge ---------------------
            d_edge = abs(d_tile + s * 0.018) - s * 0.004
            a_edge = coverage(d_edge) * 0.55
            col = mix(col, ACCENT, a_edge)

            # --- ring -----------------------------------------------------
            d_ring = abs(sd_circle(px, py, c, mark_cy, ring_r)) - ring_w / 2.0
            a_ring = coverage(d_ring)
            col = mix(col, ACCENT, a_ring)

            # --- play triangle --------------------------------------------
            a_tri = coverage(sd_polygon(px, py, tri))
            col = mix(col, ACCENT, a_tri)

            # --- EVO wordmark ---------------------------------------------
            a_word = coverage(wordmark_evo(px, py, c, word_cy,
                                           word_cap, word_half))
            col = mix(col, (255, 255, 255), a_word)

            # --- PRO badge -------------------------------------------------
            a_pill = coverage(sd_round_rect(px, py, c, pro_cy,
                                            pill_hw, pill_hh, pill_r))
            col = mix(col, ACCENT, a_pill)
            # Knock the letters back out of the pill in the tile's own colour.
            a_pro = coverage(wordmark_pro(px, py, c, pro_cy,
                                          pro_cap, pro_half)) * a_pill
            col = mix(col, mix(grad, ACCENT_DEEP, sheen), a_pro)

            # Everything outside the rounded rect is transparent-as-black; the
            # PNG has no alpha channel, and the shell composites tiles on its
            # own background, so black is the safe ground.
            r, g, b = (int(v * a_tile + 0.5) for v in col)
            row += bytes((r, g, b))
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=512,
                    help="edge length in pixels (default 512)")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    # dest was referenced by write_png() below but never assigned, so this
    # script raised NameError and the icon in the tree could not be regenerated.
    dest = os.path.join(root, "projects", "evoplayer", "sce_sys", "icon0.png")
    prev = os.path.join(root, "output", "screenshots", "app_icon_preview.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)

    rows = render_icon(args.size)
    write_png(dest, rows, args.size, args.size)
    write_png(prev, rows, args.size, args.size)

    print("wrote %s (%dx%d)" % (dest, args.size, args.size))
    print("wrote %s" % prev)


if __name__ == "__main__":
    main()
