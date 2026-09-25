#!/usr/bin/env python3
"""
Rasterise Lucide's `power` glyph into projects/evoplayer/assets/icons/icon_power.png.

The RmlUi icons are 72x72 RGBA PNGs, white with alpha coverage, recoloured at
runtime by SetImageColor() (see the rail code in evo_rmlui_app.cpp), and the
concept icons are Lucide's (ISC licence, docs/ui/icon-swap-handoff.md). The
other Lucide PNGs were rasterised from their SVGs; this one is described
directly, because the glyph is two primitives:

    <path d="M12 2v10" />                        a vertical stroke
    <path d="M18.4 6.6a9 9 0 1 1-12.77.04" />    a circle of r=9 around (12,13),
                                                 open at the top

on a 24-unit grid with stroke-width 2 and round caps. Scaled x3 to 72 px, and
coverage is computed from the exact signed distance with 4x4 supersampling, so
the edges are antialiased the same way as the rest of the set.

No third-party dependencies - only the standard library.
"""

import math
import os
import struct
import zlib

SIZE = 72
SCALE = SIZE / 24.0
HALF_STROKE = 1.0 * SCALE           # stroke-width 2 on the 24 grid
SS = 4                              # supersamples per axis

# Geometry in 24-unit Lucide coordinates.
LINE_A = (12.0, 2.0)
LINE_B = (12.0, 12.0)
ARC_C = (12.0, 13.0)
ARC_R = 9.0
ARC_P0 = (18.4, 6.6)                # arc endpoints (round caps)
ARC_P1 = (18.4 - 12.77, 6.6 + 0.04)

OUT = os.path.join(os.path.dirname(__file__), "..", "projects", "evoplayer",
                   "assets", "icons", "icon_power.png")


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def arc_dist(px, py):
    """Distance to the open circle: the ring where it exists, else the nearer cap."""
    a0 = math.atan2(ARC_P0[1] - ARC_C[1], ARC_P0[0] - ARC_C[0])   # ~ -45 deg
    a1 = math.atan2(ARC_P1[1] - ARC_C[1], ARC_P1[0] - ARC_C[0])   # ~ -135 deg
    a = math.atan2(py - ARC_C[1], px - ARC_C[0])
    in_gap = a1 < a < a0            # the opening at the top (y grows downward)
    if not in_gap:
        return abs(math.hypot(px - ARC_C[0], py - ARC_C[1]) - ARC_R)
    return min(math.hypot(px - ARC_P0[0], py - ARC_P0[1]),
               math.hypot(px - ARC_P1[0], py - ARC_P1[1]))


def coverage(x, y):
    hits = 0
    for sy in range(SS):
        for sx in range(SS):
            # pixel-space sample -> Lucide units
            px = (x + (sx + 0.5) / SS) / SCALE
            py = (y + (sy + 0.5) / SS) / SCALE
            d = min(seg_dist(px, py, *LINE_A, *LINE_B), arc_dist(px, py))
            if d * SCALE <= HALF_STROKE:
                hits += 1
    return hits / float(SS * SS)


def write_png(path, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    rows = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            a = int(round(coverage(x, y) * 255))
            row += bytes((255, 255, 255, a))
        rows.append(row)
    write_png(os.path.normpath(OUT), rows)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
