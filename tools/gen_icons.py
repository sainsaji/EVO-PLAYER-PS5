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


def render(shapes, size=None):
    """Rasterise a shape list to a coverage grid (packed to ABGR later)."""
    size = SIZE if size is None else size
    px_out = []
    for y in range(size):
        row = []
        for x in range(size):
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


def icon_resume():
    # Play triangle inside a ring. A first attempt added a "rewind" arrowhead
    # breaking the ring; at 72px the head merged with the stroke and read as a
    # rendering artefact rather than an arrow, so the ring is left closed.
    return [
        shape("circle", C, C, 26.0, stroke=STROKE),
        shape("poly", [(C - 8, C - 13), (C + 13, C), (C - 8, C + 13)],
              stroke=STROKE),
    ]


def icon_aspect():
    # A frame with an inner frame - "which shape does the picture take".
    return [
        shape("rrect", C, C, 29.0, 20.0, 4.0, stroke=STROKE),
        shape("rrect", C, C, 15.0, 11.0, 3.0, stroke=3.0),
    ]


def icon_subtitles():
    # Screen with two caption lines along the bottom, the lower one shorter.
    return [
        shape("rrect", C, C, 29.0, 21.0, 5.0, stroke=STROKE),
        shape("seg", C - 17, C + 5, C + 17, C + 5, half=2.6),
        shape("seg", C - 17, C + 13, C + 5, C + 13, half=2.6),
    ]


def icon_palette():
    # Painter's palette: outer ring plus three paint wells. Reads as "theme"
    # at 72px far better than a half-filled circle, which looks like a bug.
    shapes = [shape("circle", C, C, 26.0, stroke=STROKE)]
    for i in range(3):
        a = -math.pi / 2 + i * 2 * math.pi / 3
        shapes.append(shape("circle",
                            C + 13.0 * math.cos(a),
                            C + 13.0 * math.sin(a),
                            5.0))
    return shapes


def icon_folder():
    # One continuous folder silhouette: back panel steps up on the left to
    # form the tab, then the front panel spans the full width. Drawn as a
    # single polygon so the tab is part of the outline instead of a stray
    # line floating above the body, which is how the first version read.
    return [
        shape("poly", [
            (10.0, 54.0),   # bottom-left
            (10.0, 22.0),   # up the left edge
            (28.0, 22.0),   # across the tab
            (33.0, 29.0),   # tab shoulder
            (62.0, 29.0),   # top of the front panel
            (62.0, 54.0),   # down the right edge
        ], stroke=STROKE),
        shape("seg", 10.0, 29.0, 33.0, 29.0, half=2.0),   # panel divider
    ]


def icon_home():
    # A house: gable roof over a body, with a door. HOME used to borrow the
    # folder glyph, which sat directly above BROWSE's document icon in the
    # rail - two file-shaped marks meaning entirely different things, and the
    # one place the rail was genuinely ambiguous.
    #
    # Every shape here is a closed polygon because that is what sd_polygon
    # gives us; `stroke` turns each into an outline. The roof's base and the
    # body's top edge deliberately coincide, which reads as one silhouette
    # rather than as a triangle resting on a box.
    return [
        shape("poly", [
            (7.0,  35.0),   # left eave, overhanging the body
            (C,    13.0),   # ridge
            (65.0, 35.0),   # right eave
        ], stroke=STROKE),
        shape("poly", [
            (16.0, 35.0),
            (56.0, 35.0),
            (56.0, 60.0),
            (16.0, 60.0),
        ], stroke=STROKE),
        shape("poly", [
            (30.0, 60.0),
            (30.0, 46.0),
            (42.0, 46.0),
            (42.0, 60.0),
        ], stroke=STROKE),
    ]


def icon_logo():
    # The application mark, as one monochrome glyph.
    #
    # The rail and the launch header used to draw two concentric circles as a
    # stand-in for a logo, which read as a bullet point rather than as the
    # product. This is the ring-and-play mark from tools/gen_app_icon.py -
    # the same shape the Media tile shows on the home screen - reduced to a
    # single 72px icon so it tints with the theme like everything else in the
    # rail sitting under it.
    #
    # The triangle is filled and nudged right: a geometrically centred play
    # triangle always looks left-heavy inside a ring. Same offset ratio the
    # app icon uses, scaled to 72px.
    tri_r  = 13.0
    tri_cx = C + 1.5
    return [
        shape("circle", C, C, 25.0, stroke=6.0),
        shape("poly", [
            (tri_cx - tri_r * 0.72, C - tri_r),
            (tri_cx - tri_r * 0.72, C + tri_r),
            (tri_cx + tri_r * 0.95, C),
        ]),
    ]


def icon_trash():
    shapes = [
        shape("rrect", C, 44.0, 17.0, 20.0, 4.0, stroke=STROKE),  # bin
        shape("seg", C - 24, 22.0, C + 24, 22.0, half=2.8),       # lid
        shape("seg", C - 7, 18.0, C + 7, 18.0, half=2.8),         # handle
        shape("seg", C - 7, 18.0, C - 7, 22.0, half=2.8),
        shape("seg", C + 7, 18.0, C + 7, 22.0, half=2.8),
    ]
    for dx in (-7, 0, 7):                                          # ribs
        shapes.append(shape("seg", C + dx, 34.0, C + dx, 54.0, half=2.2))
    return shapes


# ---------------------------------------------------------------------------
# Controller prompts.
#
# The originals (RR_CONTROL_*) are 48px two-tone bitmaps - cyan strokes over a
# near-black disc, 103 distinct RGB values - so they cannot be recoloured by
# tinting the alpha channel the way the monochrome icons can. Under a
# greyscale theme they stayed stubbornly blue. Regenerated here as single-hue
# SDF glyphs at the same 48px so they follow the theme like everything else.
# ---------------------------------------------------------------------------
CTRL = 48
CC = CTRL / 2.0
CTRL_STROKE = 3.4
CTRL_RING = 20.0


def ctrl_x():
    d = 8.0
    return [
        shape("circle", CC, CC, CTRL_RING, stroke=CTRL_STROKE),
        shape("seg", CC - d, CC - d, CC + d, CC + d, half=CTRL_STROKE / 2),
        shape("seg", CC + d, CC - d, CC - d, CC + d, half=CTRL_STROKE / 2),
    ]


def ctrl_circle():
    return [
        shape("circle", CC, CC, CTRL_RING, stroke=CTRL_STROKE),
        shape("circle", CC, CC, 9.5, stroke=CTRL_STROKE),
    ]


def ctrl_triangle():
    return [
        shape("circle", CC, CC, CTRL_RING, stroke=CTRL_STROKE),
        shape("poly", [(CC, CC - 10.0), (CC + 9.5, CC + 7.0),
                       (CC - 9.5, CC + 7.0)], stroke=CTRL_STROKE),
    ]


def ctrl_square():
    return [
        shape("circle", CC, CC, CTRL_RING, stroke=CTRL_STROKE),
        shape("rrect", CC, CC, 8.5, 8.5, 1.5, stroke=CTRL_STROKE),
    ]


def ctrl_dpad():
    a, b = 4.0, 13.0
    return [
        shape("rrect", CC, CC, a, b, 1.5, stroke=CTRL_STROKE),
        shape("rrect", CC, CC, b, a, 1.5, stroke=CTRL_STROKE),
    ]


def ctrl_stick(side):
    # Stick top seen from above, with a nudge arrow on the given side.
    sx = 1.0 if side == "R" else -1.0
    return [
        shape("circle", CC, CC, CTRL_RING, stroke=CTRL_STROKE),
        shape("circle", CC, CC, 7.0),
        shape("seg", CC + sx * 11.0, CC, CC + sx * 17.0, CC,
              half=CTRL_STROKE / 2),
    ]


# (macro prefix, shape function, pixel size)
ICONS = [
    ("EVO_ICON_BROWSE_USB", icon_usb, SIZE),
    ("EVO_ICON_RECENT_FILES", icon_clock, SIZE),
    ("EVO_ICON_FAVORITES", icon_star, SIZE),
    ("EVO_ICON_SETTINGS", icon_gear, SIZE),
    ("EVO_ICON_DEVELOPER_TOOLS", icon_terminal, SIZE),
    ("EVO_ICON_ABOUT_SUPPORT", icon_info, SIZE),
    ("EVO_ICON_CHEVRON", icon_chevron, SIZE),
    # Settings rows. Indices 7.. - keep appending, rr_icon() switches on these.
    ("EVO_ICON_RESUME", icon_resume, SIZE),
    ("EVO_ICON_ASPECT", icon_aspect, SIZE),
    ("EVO_ICON_SUBTITLES", icon_subtitles, SIZE),
    ("EVO_ICON_PALETTE", icon_palette, SIZE),
    ("EVO_ICON_FOLDER", icon_folder, SIZE),
    ("EVO_ICON_TRASH", icon_trash, SIZE),
    ("EVO_ICON_HOME", icon_home, SIZE),
    ("EVO_ICON_LOGO", icon_logo, SIZE),
    # Controller prompts, 48px, same names as the RR_CONTROL_* they replace.
    ("EVO_CTRL_X", ctrl_x, CTRL),
    ("EVO_CTRL_CIRCLE", ctrl_circle, CTRL),
    ("EVO_CTRL_TRIANGLE", ctrl_triangle, CTRL),
    ("EVO_CTRL_SQUARE", ctrl_square, CTRL),
    ("EVO_CTRL_DPAD", ctrl_dpad, CTRL),
    ("EVO_CTRL_LEFT_STICK", lambda: ctrl_stick("L"), CTRL),
    ("EVO_CTRL_RIGHT_STICK", lambda: ctrl_stick("R"), CTRL),
]


# ---------------------------------------------------------------------------
# The index each drawing call passes.
#
# These were `switch (idx)` statements written out by hand in BOTH
# projects/evoplayer/main.c and tools/uiview.c - the player and the host mock -
# with one arm per icon, naming the same three macros each time. Nothing tied
# the two copies together, and they had already drifted: main.c's rr_icon()
# stopped at case 12 while every other copy went to 14.
#
# Emitting the tables here makes this file the single place an icon is added.
# Both programs index the table instead of switching, so a new icon cannot be
# half-added.
#
# ORDER IS API. Every call site passes a literal index, so these lists must
# only ever be appended to. Note that CTRL_TABLE is deliberately NOT the order
# the controller glyphs appear in ICONS above - it is the order the existing
# switches used, preserved so no call site has to change.
ICON_TABLE = [
    "EVO_ICON_BROWSE_USB",      # 0
    "EVO_ICON_RECENT_FILES",    # 1
    "EVO_ICON_FAVORITES",       # 2
    "EVO_ICON_SETTINGS",        # 3
    "EVO_ICON_DEVELOPER_TOOLS", # 4
    "EVO_ICON_ABOUT_SUPPORT",   # 5
    "EVO_ICON_CHEVRON",         # 6
    "EVO_ICON_RESUME",          # 7
    "EVO_ICON_ASPECT",          # 8
    "EVO_ICON_SUBTITLES",       # 9
    "EVO_ICON_PALETTE",         # 10
    "EVO_ICON_FOLDER",          # 11
    "EVO_ICON_TRASH",           # 12
    "EVO_ICON_HOME",            # 13
    "EVO_ICON_LOGO",            # 14
]

CTRL_TABLE = [
    "EVO_CTRL_X",               # 0
    "EVO_CTRL_DPAD",            # 1
    "EVO_CTRL_LEFT_STICK",      # 2
    "EVO_CTRL_RIGHT_STICK",     # 3
    "EVO_CTRL_CIRCLE",          # 4
    "EVO_CTRL_TRIANGLE",        # 5
    "EVO_CTRL_SQUARE",          # 6
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

    # Contact sheet: every icon in its own SIZE-wide cell, small ones centred.
    bg = (6, 10, 20)
    sheet = [[bg] * (SIZE * len(ICONS)) for _ in range(SIZE)]

    for idx, (name, fn, n) in enumerate(ICONS):
        cov = render(fn(), n)
        r, g, b = CYAN
        off = (SIZE - n) // 2
        lines.append(f"#define {name}_W {n}")
        lines.append(f"#define {name}_H {n}")
        lines.append(f"static const unsigned int {name}[{n*n}]={{")
        flat = []
        for y in range(n):
            for x in range(n):
                a = cov[y][x]
                flat.append(f"0x{(a << 24) | (b << 16) | (g << 8) | r:08X}")
                sheet[y + off][idx * SIZE + x + off] = (
                    (r * a + bg[0] * (255 - a)) // 255,
                    (g * a + bg[1] * (255 - a)) // 255,
                    (b * a + bg[2] * (255 - a)) // 255,
                )
        for i in range(0, len(flat), 12):
            lines.append(",".join(flat[i:i + 12]) + ",")
        lines.append("};")
        lines.append("")

    # Index tables. See the comment on ICON_TABLE for why these exist.
    known = {name for name, _fn, _n in ICONS}
    for label, table in (("ICON", ICON_TABLE), ("CTRL", CTRL_TABLE)):
        for name in table:
            if name not in known:
                raise SystemExit(f"{label}_TABLE names {name}, which ICONS does not define")

    lines += [
        "/* Index -> bitmap. The player and tools/uiview.c both draw through",
        " * these rather than through a hand-written switch per program.",
        " * Append only: every call site passes a literal index. */",
        "typedef struct {",
        "    const unsigned int *px;",
        "    int w, h;",
        "} evo_icon_bitmap;",
        "",
    ]

    for label, table in (("EVO_ICON_TABLE", ICON_TABLE), ("EVO_CTRL_TABLE", CTRL_TABLE)):
        lines.append(f"static const evo_icon_bitmap {label}[] = {{")
        for idx, name in enumerate(table):
            lines.append(f"    {{ {name}, {name}_W, {name}_H }}, /* {idx} */")
        lines.append("};")
        lines.append(f"#define {label}_COUNT {len(table)}")
        lines.append("")

    with open(out_h, "w") as f:
        f.write("\n".join(lines))

    rows = [bytes(v for px in row for v in px) for row in sheet]
    write_png(prev, rows, SIZE * len(ICONS), SIZE)

    print(f"wrote {out_h}")
    print(f"wrote {prev}")


if __name__ == "__main__":
    main()
