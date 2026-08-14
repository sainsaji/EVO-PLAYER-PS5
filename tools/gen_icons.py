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
    return render_box(shapes, size, size, peak=ALPHA)


def render_box(shapes, w, h, peak=255):
    """
    Rasterise into a non-square cell.

    The icons are all square; font glyphs are not - a cell is as tall as the
    line box and only as wide as the glyph needs. Same rasteriser, and `peak`
    exists because glyph coverage runs to full 255 while icons stop at ALPHA
    to match the card stroke they sit next to.
    """
    px_out = []
    for y in range(h):
        row = []
        for x in range(w):
            fx, fy = x + 0.5, y + 0.5
            d = min(eval_shape(s, fx, fy) for s in shapes)
            cov = 0.5 - d               # 1px analytic edge
            cov = max(0.0, min(1.0, cov))
            row.append(int(cov * peak + 0.5))
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


def icon_emby():
    # Emby's official logo mark:
    # Outer play-triangle polygon with an inner solid play chevron/arrow.
    return [
        shape("poly", [
            (C - 16.0, C - 23.0),
            (C + 23.0, C),
            (C - 16.0, C + 23.0),
        ], stroke=STROKE),
        shape("poly", [
            (C - 6.5, C - 10.0),
            (C + 10.5, C),
            (C - 6.5, C + 10.0),
        ]),
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
    ("EVO_ICON_EMBY", icon_emby, SIZE),
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
    "EVO_ICON_EMBY",            # 15
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


# ===========================================================================
# Supplementary font glyphs - the punctuation the UI font never had.
# ===========================================================================
#
# THE PROBLEM
#   The UI font is RR_FONT in assets/renderer_reset_assets.h: a 4096x260 alpha
#   atlas whose alphabet is
#
#       ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /._:-+
#
#   No comma, no apostrophe, no parenthesis, no question mark. rr_text draws an
#   unknown character as a 12px gap, which is survivable for a filename and
#   makes ordinary prose unreadable - and a text reader is nothing but prose.
#   That header is a pre-generated asset with no generator in the tree, so it
#   cannot be rebuilt; the missing glyphs are generated here instead and looked
#   up as a fallback when RR_CHARS misses.
#
# THE ALIGNMENT PROBLEM, AND WHY THESE NUMBERS ARE MEASURED
#   Punctuation that does not share the letters' baseline reads as broken. The
#   atlas carries no baseline, cap height or stroke weight, so tools/measure_font.py
#   walks the ink of known glyphs - H, x, p, period, colon, hyphen - and derives
#   them per face. FACE_METRICS below is that script's output. Re-run it rather
#   than trusting these if the atlas is ever replaced.
#
#   Glyphs are then defined once, in cap-height units with the baseline at y=0
#   and +y upward, and instantiated per face from its own measured baseline and
#   cap height. That is what keeps a comma sitting on the same line as a letter
#   at all four sizes.

# From tools/measure_font.py. cell_h/baseline/cap/xh are pixels in the cell;
# stroke is the measured hyphen bar thickness, which is the font's thin-stroke
# weight; dot is the period's ink box.
FACE_METRICS = [
    # name,    cell_h, baseline, cap, xh, stroke, dot_w, dot_h, pad
    ("SMALL",  25, 17, 11,  8, 2.0, 2, 2, 5),
    ("SUB",    29, 20, 13, 10, 2.0, 3, 3, 5),
    ("MENU",   46, 34, 24, 18, 3.0, 3, 4, 7),
    ("TITLE",  57, 43, 31, 23, 5.0, 4, 6, 8),
]

# Order is the C-side `face` parameter: 0 SMALL, 1 SUB, 2 MENU, 3 TITLE.


def arc(cx, cy, r, a0, a1, steps=14):
    """
    A circular arc as a chain of short segments.
    Angles in degrees, counter-clockwise, +y up (glyph space).

    The SDF set has no arc primitive and the icons never needed one - every
    curve there is a full circle. Parentheses, question marks and ampersands
    are all arcs, so this builds one out of the segment primitive.
    """
    pts = []
    for i in range(steps + 1):
        t = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + r * math.cos(t), cy + r * math.sin(t)))
    return [("segchain", pts)]


def seg(x1, y1, x2, y2, w=1.0):
    """A stroked segment. `w` is a multiple of the face's stroke weight."""
    return [("seg", (x1, y1, x2, y2), w)]


def dot(cx, cy, r=1.0):
    """A filled dot. `r` is a multiple of the face's period radius."""
    return [("dot", (cx, cy), r)]


# ---------------------------------------------------------------------------
# The glyphs. Coordinates are cap-height units: y=0 baseline, y=1 cap top.
# Each entry is (character, nominal width, shape builder).
#
# The middle column is advisory only - build_punct_face measures the rasterised
# ink and derives the real advance from it, because a declared advance and a
# drawn shape drift apart the moment either is adjusted.
#
# Descenders go to about -0.20, which is what measure_font.py reports for 'p'
# (9px against a 31px cap at TITLE).
# ---------------------------------------------------------------------------

def g_comma():
    return dot(0.13, 0.09) + seg(0.13, 0.06, 0.05, -0.19, 0.85)


def g_apostrophe():
    return seg(0.12, 1.00, 0.09, 0.70, 0.95)


def g_quote():
    return seg(0.12, 1.00, 0.09, 0.70, 0.95) + seg(0.34, 1.00, 0.31, 0.70, 0.95)


def g_bang():
    return seg(0.13, 1.00, 0.13, 0.27, 0.95) + dot(0.13, 0.09)


def g_question():
    # The bowl has to run far enough round that the stem meets it. Ending the
    # arc at the 3 o'clock position leaves a visible gap to a centred stem.
    return (arc(0.36, 0.76, 0.25, 195, -50, 16)
            + seg(0.52, 0.57, 0.36, 0.30)
            + dot(0.36, 0.09))


def g_lparen():
    return arc(0.46, 0.40, 0.62, 118, 242, 20)


def g_rparen():
    return arc(0.02, 0.40, 0.62, 62, -62, 20)


def g_semicolon():
    return dot(0.13, 0.47) + dot(0.13, 0.09) + seg(0.13, 0.06, 0.05, -0.19, 0.85)


def g_lbracket():
    return (seg(0.30, 1.02, 0.11, 1.02)
            + seg(0.11, 1.02, 0.11, -0.20)
            + seg(0.11, -0.20, 0.30, -0.20))


def g_rbracket():
    return (seg(0.06, 1.02, 0.25, 1.02)
            + seg(0.25, 1.02, 0.25, -0.20)
            + seg(0.25, -0.20, 0.06, -0.20))


def g_lbrace():
    return (arc(0.42, 0.83, 0.20, 180, 90, 8)
            + seg(0.22, 0.83, 0.22, 0.52)
            + arc(0.06, 0.52, 0.16, 0, 90, 8)
            + arc(0.06, 0.29, 0.16, 270, 360, 8)
            + seg(0.22, 0.29, 0.22, -0.02)
            + arc(0.42, -0.02, 0.20, 180, 270, 8))


def g_rbrace():
    return (arc(0.04, 0.83, 0.20, 0, 90, 8)
            + seg(0.24, 0.83, 0.24, 0.52)
            + arc(0.40, 0.52, 0.16, 180, 90, 8)
            + arc(0.40, 0.29, 0.16, 270, 180, 8)
            + seg(0.24, 0.29, 0.24, -0.02)
            + arc(0.04, -0.02, 0.20, 0, -90, 8))


def g_lt():
    return seg(0.58, 0.86, 0.12, 0.43) + seg(0.12, 0.43, 0.58, 0.00)


def g_gt():
    return seg(0.10, 0.86, 0.56, 0.43) + seg(0.56, 0.43, 0.10, 0.00)


def g_eq():
    return seg(0.10, 0.58, 0.74, 0.58) + seg(0.10, 0.28, 0.74, 0.28)


def g_star():
    cx, cy, r = 0.36, 0.74, 0.28
    out = []
    for a in (90, 150, 210, 270, 330, 30):
        t = math.radians(a)
        out += seg(cx, cy, cx + r * math.cos(t), cy + r * math.sin(t), 0.85)
    return out


def g_hash():
    return (seg(0.22, 1.00, 0.14, 0.00, 0.9) + seg(0.56, 1.00, 0.48, 0.00, 0.9)
            + seg(0.04, 0.68, 0.68, 0.68, 0.9) + seg(0.02, 0.32, 0.66, 0.32, 0.9))


def g_pipe():
    return seg(0.14, 1.05, 0.14, -0.22)


def g_backslash():
    return seg(0.08, 1.00, 0.54, -0.06)


def g_tilde():
    return (arc(0.24, 0.44, 0.15, 160, 20, 8)
            + arc(0.54, 0.56, 0.15, 340, 200, 8))


def g_caret():
    return seg(0.10, 0.60, 0.36, 0.96) + seg(0.36, 0.96, 0.62, 0.60)


def g_backtick():
    return seg(0.10, 1.00, 0.30, 0.76, 0.95)


def g_percent():
    return (arc(0.22, 0.76, 0.17, 0, 360, 16)
            + arc(0.72, 0.20, 0.17, 0, 360, 16)
            + seg(0.82, 0.98, 0.12, -0.02, 0.85))


def g_ampersand():
    # Small loop up top, larger bowl below, diagonal through, tail out.
    #
    # Honestly the weakest glyph in the set: a real ampersand is a Latin "et"
    # ligature and does not decompose into arcs and segments the way the rest
    # of these do. It is legible in context and rare enough in prose to leave
    # at that - check output/screenshots/punct_preview.png before assuming a
    # tweak here improved it.
    return (arc(0.34, 0.80, 0.16, 340, 190, 12)     # top bowl, right over to left
            + seg(0.19, 0.77, 0.56, 0.13)           # diagonal down to the right
            + arc(0.33, 0.27, 0.24, 170, 375, 16)   # bottom bowl, round the base
            + seg(0.56, 0.35, 0.84, 0.00))          # tail out to the baseline


def g_at():
    # The outer ring must stay OPEN at the lower right. Closed, it reads as a
    # target rather than an at-sign - which is exactly how the first pass came
    # out.
    return (arc(0.48, 0.44, 0.45, 25, 320, 24)
            + arc(0.43, 0.40, 0.18, 0, 360, 14)
            + seg(0.61, 0.57, 0.61, 0.29)
            + seg(0.61, 0.29, 0.78, 0.25))


def g_dollar():
    # Two arcs stacked into an S, then the bar through it. The first pass used
    # near-full circles, which read as a figure 8.
    return (arc(0.36, 0.66, 0.19, 0, 205, 12)
            + arc(0.36, 0.30, 0.19, 180, 385, 12)
            + seg(0.36, 1.06, 0.36, -0.12, 0.85))


PUNCT = [
    (",",  0.46, g_comma),
    ("'",  0.34, g_apostrophe),
    ('"',  0.56, g_quote),
    ("!",  0.40, g_bang),
    ("?",  0.76, g_question),
    ("(",  0.50, g_lparen),
    (")",  0.50, g_rparen),
    (";",  0.44, g_semicolon),
    ("[",  0.48, g_lbracket),
    ("]",  0.48, g_rbracket),
    ("{",  0.54, g_lbrace),
    ("}",  0.54, g_rbrace),
    ("<",  0.74, g_lt),
    (">",  0.72, g_gt),
    ("=",  0.88, g_eq),
    ("*",  0.66, g_star),
    ("#",  0.80, g_hash),
    ("|",  0.34, g_pipe),
    ("\\", 0.66, g_backslash),
    ("~",  0.82, g_tilde),
    ("^",  0.76, g_caret),
    ("`",  0.36, g_backtick),
    ("%",  1.00, g_percent),
    ("&",  0.94, g_ampersand),
    ("@",  1.06, g_at),
    ("$",  0.72, g_dollar),
]


def punct_shapes(builder, baseline, cap, stroke, dot_r, left):
    """
    Convert a glyph's cap-unit shapes into cell-pixel SDF shapes.

    x_px = left + u * cap        y_px = baseline - v * cap
    """
    def X(u):
        return left + u * cap

    def Y(v):
        return baseline - v * cap

    out = []
    for item in builder():
        kind = item[0]
        if kind == "seg":
            x1, y1, x2, y2 = item[1]
            out.append(shape("seg", X(x1), Y(y1), X(x2), Y(y2),
                             half=stroke * item[2] / 2.0))
        elif kind == "segchain":
            pts = item[1]
            for i in range(len(pts) - 1):
                a, b = pts[i], pts[i + 1]
                out.append(shape("seg", X(a[0]), Y(a[1]), X(b[0]), Y(b[1]),
                                 half=stroke / 2.0))
        elif kind == "dot":
            cx, cy = item[1]
            out.append(shape("circle", X(cx), Y(cy), dot_r * item[2]))
        else:
            raise ValueError(kind)
    return out


def build_punct_face(cell_h, baseline, cap, stroke, dot_w, dot_h, pad):
    """
    Rasterise every punctuation glyph for one face. Returns rows + metrics.

    Advances are MEASURED, not declared.

    The first version of this took a hand-written advance per glyph and drew
    the shape at a fixed left bearing. Both numbers were guesses, and they
    disagreed with where the ink actually landed: a parenthesis draws an arc
    whose leftmost point is left of its own origin, so "$4.50 (approx" came out
    with the bracket touching the zero while the gap after it was too wide.

    So: rasterise into a generous scratch box, find the ink, and crop to it
    with a symmetric side bearing. The cell then *is* the advance, which makes
    the gap between any two glyphs exactly two bearings - even, and correct by
    construction rather than by tuning.
    """
    # The period measures slightly taller than wide at the larger faces, so
    # take the radius from the mean rather than one axis.
    dot_r = (dot_w + dot_h) / 4.0

    # Sized from the period's own bearing in the real font, so generated
    # punctuation sits as loosely as the punctuation already there.
    side = max(1, int(round(0.15 * cap)))

    # Wide enough for the widest glyph plus room for ink left of the origin.
    scratch_w = int(cap * 2.6) + pad * 4
    origin = cap  # generous, so a negative-x arc still lands inside the box

    cells, xs, ws, advs = [], [], [], []
    for ch, _adv_u, builder in PUNCT:
        shapes = punct_shapes(builder, baseline, cap, stroke, dot_r, origin)
        scratch = render_box(shapes, scratch_w, cell_h, peak=255)

        # Ink bounds, at the same threshold rr_text treats as ink.
        x0, x1 = scratch_w, -1
        for row in scratch:
            for x, v in enumerate(row):
                if v > 18:
                    if x < x0:
                        x0 = x
                    if x > x1:
                        x1 = x
        if x1 < 0:                     # a glyph that drew nothing
            x0, x1 = origin, origin

        left = max(0, x0 - side)
        right = min(scratch_w - 1, x1 + side)
        w = right - left + 1

        cells.append([row[left:right + 1] for row in scratch])
        ws.append(w)
        advs.append(w)

    x = 0
    for w in ws:
        xs.append(x)
        x += w

    atlas_w = x
    rows = [[0] * atlas_w for _ in range(cell_h)]
    for cell, x0, w in zip(cells, xs, ws):
        for y in range(cell_h):
            rows[y][x0:x0 + w] = cell[y]

    return rows, xs, ws, advs, atlas_w


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

    write_punct(root)


def write_punct(root):
    """Emit the supplementary punctuation atlas, plus a sheet to eyeball it."""
    out_h = os.path.join(root, "projects", "evoplayer", "assets",
                         "evo_font_punct.h")
    prev = os.path.join(root, "output", "screenshots", "punct_preview.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)

    lines = [
        "/* Generated by tools/gen_icons.py - do not edit by hand.",
        " *",
        " * The punctuation the UI font never had. RR_CHARS covers letters,",
        " * digits and  /._:-+  and nothing else, so rr_text drew a comma or an",
        " * apostrophe as a 12px gap. That is survivable in a filename and",
        " * unreadable in prose, which is all a text reader renders.",
        " *",
        " * Glyphs are defined in cap-height units and instantiated per face",
        " * from baselines measured out of the real atlas by",
        " * tools/measure_font.py, so they sit on the same line as the letters.",
        " *",
        " * Coverage is 8-bit alpha, same as RR_FONT, and the lookup is the same",
        " * shape - x/width/advance tables into a single-row atlas per face.",
        " */",
        "#pragma once",
        "",
        "/* Face order matches rr_text()'s `face` argument. */",
        "typedef struct {",
        "    const unsigned char *pix;",
        "    int atlas_w;",
        "    int cell_h;",
        "    const short *x;",
        "    const short *w;",
        "    const short *adv;",
        "} evo_punct_face;",
        "",
    ]

    chars = "".join(ch for ch, _adv, _fn in PUNCT)
    c_escaped = chars.replace("\\", "\\\\").replace('"', '\\"')
    lines.append("/* Index -> character. Order is this table's order, nothing else. */")
    lines.append(f'#define EVO_PUNCT_CHARS "{c_escaped}"')
    lines.append(f"#define EVO_PUNCT_COUNT {len(PUNCT)}")
    lines.append("")

    sheets = []
    for name, cell_h, baseline, cap, xh, stroke, dw, dh, pad in FACE_METRICS:
        rows, xs, ws, advs, atlas_w = build_punct_face(
            cell_h, baseline, cap, stroke, dw, dh, pad)
        sheets.append((name, rows, atlas_w, cell_h))

        lines.append(f"/* {name}: cell {cell_h}px, baseline {baseline}, cap {cap} */")
        lines.append(f"static const short EVO_PUNCT_{name}_X[] = {{"
                     + ",".join(str(v) for v in xs) + "};")
        lines.append(f"static const short EVO_PUNCT_{name}_W[] = {{"
                     + ",".join(str(v) for v in ws) + "};")
        lines.append(f"static const short EVO_PUNCT_{name}_ADV[] = {{"
                     + ",".join(str(v) for v in advs) + "};")
        lines.append(f"static const unsigned char EVO_PUNCT_{name}_PIX[] = {{")
        flat = [v for row in rows for v in row]
        for i in range(0, len(flat), 32):
            lines.append(",".join(str(v) for v in flat[i:i + 32]) + ",")
        lines.append("};")
        lines.append("")

    lines.append("static const evo_punct_face EVO_PUNCT_FACES[4] = {")
    for name, _rows, atlas_w, cell_h in sheets:
        lines.append(f"    {{ EVO_PUNCT_{name}_PIX, {atlas_w}, {cell_h}, "
                     f"EVO_PUNCT_{name}_X, EVO_PUNCT_{name}_W, EVO_PUNCT_{name}_ADV }},")
    lines.append("};")
    lines.append("")

    with open(out_h, "w") as f:
        f.write("\n".join(lines))

    # The UI layer needs to know which characters render, but it deliberately
    # cannot include the atlas above - that is ~1MB of static arrays, and a
    # second translation unit including them would duplicate every byte in the
    # ELF. So the *alphabet* alone goes into its own tiny header that ui/ can
    # include, and evo_draw.h builds EVO_TEXT_CHARSET from it.
    #
    # Without this, evo_text_unsupported() goes on reporting a comma as
    # unsupported long after the comma was added.
    charset_h = os.path.join(root, "projects", "evoplayer", "ui", "include",
                             "evo_font_charset.h")
    with open(charset_h, "w") as f:
        f.write("\n".join([
            "/* Generated by tools/gen_icons.py - do not edit by hand.",
            " *",
            " * The punctuation the generated second atlas provides, as a plain",
            " * string. Pixels live in assets/evo_font_punct.h; this carries the",
            " * alphabet only, so the UI layer can check what renders without",
            " * pulling in a megabyte of glyph data.",
            " */",
            "#pragma once",
            "",
            f'#define EVO_FONT_PUNCT_CHARS "{c_escaped}"',
            "",
        ]))
    print(f"wrote {charset_h}")

    # Contact sheet: every face stacked, dark ground, so the glyphs can be
    # looked at rather than argued about.
    bg = (6, 10, 20)
    ink = (0xE8, 0xF2, 0xFF)
    total_h = sum(s[3] + 8 for s in sheets)
    total_w = max(s[2] for s in sheets)
    sheet = [[bg] * total_w for _ in range(total_h)]
    y0 = 0
    for _name, rows, atlas_w, cell_h in sheets:
        for y in range(cell_h):
            for x in range(atlas_w):
                a = rows[y][x]
                sheet[y0 + y][x] = tuple(
                    (ink[c] * a + bg[c] * (255 - a)) // 255 for c in range(3))
        y0 += cell_h + 8

    png_rows = [bytes(v for px in row for v in px) for row in sheet]
    write_png(prev, png_rows, total_w, total_h)

    print(f"wrote {out_h}")
    print(f"wrote {prev}")


if __name__ == "__main__":
    main()
