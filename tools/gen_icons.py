#!/usr/bin/env python3
"""
Generate EVO Player's UI icons.

WHY THIS EXISTS
    The icons shipped in assets/renderer_reset_assets.h are 72x72 PNGs whose
    shape edges are effectively binary - the antialiasing they do contain is
    in the outer glow, not the strokes - so they show a visible 1px staircase
    on every curve. We do not have the original artwork to re-export.

    Rather than hand-patch bitmaps, the icons are described here as vector
    shapes and rasterised with signed-distance fields. Coverage along an edge
    is then an exact analytic value rather than a sample count, which gives
    clean edges at any size, and the set can be regenerated or restyled by
    editing this file.

OUTPUT
    projects/evoplayer/assets/evo_icons.h   C header, same layout as RR_ICON_*
    output/screenshots/icons_preview.png    contact sheet, for eyeballing

USAGE
    python3 tools/gen_icons.py

No third-party dependencies - only the standard library.
"""

import math
import struct
import zlib
import os

SIZE = 72                      # matches RR_ICON_*_W / _H
CYAN = (0x00, 0xCD, 0xFF)      # r, g, b - the UI accent
STROKE = 5.0                   # line width in pixels at 72px
ALPHA = 235                    # peak alpha, matching the card stroke

# ---------------------------------------------------------------------------
# Signed distance helpers. Each returns distance in pixels; negative = inside.
# ---------------------------------------------------------------------------


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    ax, ay = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def sd_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sd_segment(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    wx, wy = px - x1, py - y1
    L2 = vx * vx + vy * vy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / L2))
    return math.hypot(wx - t * vx, wy - t * vy)


def sd_polygon(px, py, pts):
    """Winding-based signed distance for a closed polygon."""
    d = float("inf")
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        d = min(d, sd_segment(px, py, x1, y1, x2, y2))
        if (y1 > py) != (y2 > py):
            xint = x1 + (py - y1) * (x2 - x1) / (y2 - y1)
            if px < xint:
                inside = not inside
    return -d if inside else d


# ---------------------------------------------------------------------------
# Shape wrappers. `stroke` renders the outline, otherwise the shape is filled.
# ---------------------------------------------------------------------------


def shape(kind, *args, stroke=None, **kw):
    return (kind, args, stroke, kw)


def eval_shape(sh, px, py):
    kind, args, stroke, kw = sh
    if kind == "rrect":
        d = sd_round_rect(px, py, *args)
    elif kind == "circle":
        d = sd_circle(px, py, *args)
    elif kind == "seg":
        d = sd_segment(px, py, *args) - kw.get("half", STROKE / 2)
        return d
    elif kind == "poly":
        d = sd_polygon(px, py, args[0])
    else:
        raise ValueError(kind)

    if stroke is not None:
        d = abs(d) - stroke / 2.0
    return d


def render(shapes):
    """Rasterise a shape list to an RGBA byte list (ABGR packed later)."""
    px_out = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            fx, fy = x + 0.5, y + 0.5
            d = min(eval_shape(s, fx, fy) for s in shapes)
            cov = 0.5 - d               # 1px analytic edge
            cov = max(0.0, min(1.0, cov))
            row.append(int(cov * ALPHA + 0.5))
        px_out.append(row)
    return px_out


# ---------------------------------------------------------------------------
# The icon set. Coordinates are in a 72x72 box.
# ---------------------------------------------------------------------------
C = SIZE / 2.0


def icon_usb():
    # A USB stick seen face-on: connector tab on top, body below, contact slots.
    # Body is taller than wide and the metal connector is a filled block, so
    # it reads as a USB stick rather than a bottle.
    return [
        shape("rrect", C, 46.0, 14.0, 19.0, 4.0, stroke=STROKE),   # body
        shape("rrect", C, 21.0, 9.0, 9.0, 2.0),                    # connector (filled)
        shape("seg", C - 6, 43.0, C + 6, 43.0, half=2.0),          # slot
        shape("seg", C - 6, 53.0, C + 6, 53.0, half=2.0),          # slot
    ]


def icon_clock():
    return [
        shape("circle", C, C, 26.0, stroke=STROKE),
        shape("seg", C, C, C, C - 14, half=2.5),      # minute hand
        shape("seg", C, C, C + 10, C + 6, half=2.5),  # hour hand
    ]


def icon_star():
    pts = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        r = 27.0 if i % 2 == 0 else 11.5
        pts.append((C + r * math.cos(ang), C + r * math.sin(ang)))
    return [shape("poly", pts, stroke=STROKE)]


def icon_gear():
    shapes = [
        shape("circle", C, C, 20.0, stroke=STROKE),   # rim
        shape("circle", C, C, 7.0, stroke=STROKE),    # hub
    ]
    # Teeth start inside the rim so they merge with it instead of reading as
    # detached spikes, and are wide enough to look like gear teeth.
    for i in range(8):
        a = i * math.pi / 4
        x1, y1 = C + 16.0 * math.cos(a), C + 16.0 * math.sin(a)
        x2, y2 = C + 26.5 * math.cos(a), C + 26.5 * math.sin(a)
        shapes.append(shape("seg", x1, y1, x2, y2, half=5.0))
    return shapes


def icon_terminal():
    return [
        shape("rrect", C, C, 28.0, 22.0, 5.0, stroke=STROKE),   # window
        shape("seg", 26.0, 32.0, 34.0, 38.0, half=2.5),         # '>' upper
        shape("seg", 34.0, 38.0, 26.0, 44.0, half=2.5),         # '>' lower
        shape("seg", 39.0, 45.0, 49.0, 45.0, half=2.5),         # cursor
    ]


def icon_info():
    return [
        shape("circle", C, C, 26.0, stroke=STROKE),
        shape("circle", C, C - 12.0, 2.6),            # dot
        shape("seg", C, C - 3.0, C, C + 15.0, half=2.6),
    ]


def icon_chevron():
    return [
        shape("seg", C - 6, C - 13, C + 7, C, half=3.0),
        shape("seg", C + 7, C, C - 6, C + 13, half=3.0),
    ]


ICONS = [
    ("EVO_ICON_BROWSE_USB", icon_usb),
    ("EVO_ICON_RECENT_FILES", icon_clock),
    ("EVO_ICON_FAVORITES", icon_star),
    ("EVO_ICON_SETTINGS", icon_gear),
    ("EVO_ICON_DEVELOPER_TOOLS", icon_terminal),
    ("EVO_ICON_ABOUT_SUPPORT", icon_info),
    ("EVO_ICON_CHEVRON", icon_chevron),
]


# ---------------------------------------------------------------------------
def write_png(path, rows_rgb, w, h):
    raw = b"".join(b"\x00" + bytes(r) for r in rows_rgb)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    out_h = os.path.join(root, "projects", "evoplayer", "assets", "evo_icons.h")
    prev = os.path.join(root, "output", "screenshots", "icons_preview.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)

    lines = [
        "/* Generated by tools/gen_icons.py - do not edit by hand.",
        " *",
        " * Signed-distance rasterised UI icons. Replaces the RR_ICON_* bitmaps,",
        " * whose shape edges were effectively binary and showed a 1px staircase",
        " * on every curve.",
        " *",
        " * Pixels are 0xAABBGGRR, matching RR_BGRA and the framebuffer.",
        " */",
        "#pragma once",
        "",
    ]

    sheet = [[(6, 10, 20)] * (SIZE * len(ICONS)) for _ in range(SIZE)]

    for idx, (name, fn) in enumerate(ICONS):
        cov = render(fn())
        r, g, b = CYAN
        lines.append(f"#define {name}_W {SIZE}")
        lines.append(f"#define {name}_H {SIZE}")
        lines.append(f"static const unsigned int {name}[{SIZE*SIZE}]={{")
        flat = []
        for y in range(SIZE):
            for x in range(SIZE):
                a = cov[y][x]
                flat.append(f"0x{(a << 24) | (b << 16) | (g << 8) | r:08X}")
                # preview: composite over the card fill colour
                bg = (6, 10, 20)
                sheet[y][idx * SIZE + x] = (
                    (r * a + bg[0] * (255 - a)) // 255,
                    (g * a + bg[1] * (255 - a)) // 255,
                    (b * a + bg[2] * (255 - a)) // 255,
                )
        for i in range(0, len(flat), 12):
            lines.append(",".join(flat[i:i + 12]) + ",")
        lines.append("};")
        lines.append("")

    with open(out_h, "w") as f:
        f.write("\n".join(lines))

    rows = [bytes(v for px in row for v in px) for row in sheet]
    write_png(prev, rows, SIZE * len(ICONS), SIZE)

    print(f"wrote {out_h}")
    print(f"wrote {prev}")


if __name__ == "__main__":
    main()
