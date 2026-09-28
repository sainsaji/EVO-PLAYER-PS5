#!/usr/bin/env python3
"""
gen_surround_icons.py - the Surround Sound Studio's listener and speaker icons
(#106), rasterised from Lucide (https://lucide.dev, ISC licence - the icon
source approved in docs/ui/icon-swap-handoff.md).

    python3 tools/gen_surround_icons.py      # writes projects/evoplayer/assets/icons/

Every icon is WHITE on transparent, so the RmlUi document tints it per state
with `image-color` (EvoRmlApp::SetImageColor). The speaker is split into two
layers on one canvas so the woofer can light up on its own:

    icon_speaker_body.png   cabinet + tweeter          (Lucide "speaker")
    icon_speaker_cone.png   woofer cone + dust cap     (Lucide "speaker")
    icon_listener.png       head and shoulders         (Lucide "user-round")
    icon_armchair.png       the seat                   (Lucide "armchair")

No SVG library is needed: Lucide icons are 2 px round-capped strokes on a
24-unit grid, so each one is its path geometry (lines + circular arcs,
tessellated) turned into an anti-aliased signed-distance stroke. Pure stdlib.
"""
import math
import pathlib
import re
import struct
import zlib

SIZE = 96                 # 4x Lucide's 24-unit grid
SCALE = SIZE / 24.0
STROKE = 2.0              # Lucide stroke-width, in grid units
OUT = pathlib.Path(__file__).resolve().parent.parent / "projects/evoplayer/assets/icons"

# Geometry copied verbatim from lucide-icons/lucide icons/<name>.svg
SPEAKER_BODY = ['RECT 4 2 16 20 2', 'M12 6h.01']
SPEAKER_CONE = ['CIRCLE 12 14 4', 'M12 14h.01']
LISTENER = ['CIRCLE 12 8 5', 'M20 21a8 8 0 0 0-16 0']
ARMCHAIR = [
    'M19 9V6a2 2 0 0 0-2-2H7a2 2 0 0 0-2 2v3',
    'M3 16a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-5a2 2 0 0 0-4 0v1.5a.5.5 0 0 1-.5.5h-9'
    'a.5.5 0 0 1-.5-.5V11a2 2 0 0 0-4 0z',
    'M5 18v2',
    'M19 18v2',
]


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2, steps=24):
    """SVG endpoint arc -> points (F.6.5 of the SVG spec; Lucide arcs are
    circular and unrotated, but this is the general form)."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        px, py = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * px - sp * py + cx, sp * px + cp * py + cy))
    return pts


def parse_path(d):
    """M/L/H/V/A/Z (+ relative forms) -> list of polylines."""
    toks = re.findall(r'[MmLlHhVvAaZz]|-?(?:\d+\.?\d*|\.\d+)', d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    lines, cur = [], []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                cur.append((sx, sy))
                x, y = sx, sy
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == 'M':
            if cur:
                lines.append(cur)
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            sx, sy = x, y
            cur = [(x, y)]
            cmd = 'l' if rel else 'L'
        elif c == 'L':
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
        elif c == 'H':
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif c == 'V':
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif c == 'A':
            rx, ry, rot = num(), num(), num()
            large, sweep = int(num()), int(num())
            nx, ny = num(), num()
            ex, ey = (x + nx, y + ny) if rel else (nx, ny)
            cur.extend(arc_points(x, y, rx, ry, math.radians(rot), large, sweep, ex, ey))
            x, y = ex, ey
    if cur:
        lines.append(cur)
    return lines


def shapes_to_segments(shapes):
    segs = []  # ((ax, ay), (bx, by))
    for s in shapes:
        parts = s.split()
        if parts[0] == 'RECT':
            x0, y0, w, h, r = map(float, parts[1:])
            d = (f'M{x0 + r} {y0}H{x0 + w - r}a{r} {r} 0 0 1 {r} {r}V{y0 + h - r}'
                 f'a{r} {r} 0 0 1 -{r} {r}H{x0 + r}a{r} {r} 0 0 1 -{r} -{r}V{y0 + r}'
                 f'a{r} {r} 0 0 1 {r} -{r}z')
            polys = parse_path(d)
        elif parts[0] == 'CIRCLE':
            cx, cy, r = map(float, parts[1:])
            polys = [[(cx + r * math.cos(2 * math.pi * k / 64),
                       cy + r * math.sin(2 * math.pi * k / 64)) for k in range(65)]]
        else:
            polys = parse_path(s)
        for poly in polys:
            if len(poly) == 1:
                segs.append((poly[0], poly[0]))
            for a, b in zip(poly, poly[1:]):
                segs.append((a, b))
    return segs


def seg_dist(px, py, a, b):
    ax, ay = a
    bx, by = b
    vx, vy = bx - ax, by - ay
    ll = vx * vx + vy * vy
    t = 0.0 if ll == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / ll))
    dx, dy = px - (ax + vx * t), py - (ay + vy * t)
    return math.sqrt(dx * dx + dy * dy)


def render(shapes):
    segs = shapes_to_segments(shapes)
    half = STROKE / 2.0
    alpha = bytearray(SIZE * SIZE)
    for py in range(SIZE):
        gy = (py + 0.5) / SCALE
        near = [s for s in segs if min(s[0][1], s[1][1]) - 2 <= gy <= max(s[0][1], s[1][1]) + 2]
        for px in range(SIZE):
            gx = (px + 0.5) / SCALE
            d = min((seg_dist(gx, gy, a, b) for a, b in near), default=99.0)
            cov = max(0.0, min(1.0, 0.5 - (d - half) * SCALE))   # 1 px AA edge
            alpha[py * SIZE + px] = int(cov * 255 + 0.5)
    return alpha


def write_png(path, alpha):
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)
        for x in range(SIZE):
            raw += bytes((255, 255, 255, alpha[y * SIZE + x]))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    path.write_bytes(png)
    print(f'  ok -> {path.relative_to(OUT.parent.parent.parent.parent)}')


def main():
    for name, shapes in (('icon_speaker_body.png', SPEAKER_BODY),
                         ('icon_speaker_cone.png', SPEAKER_CONE),
                         ('icon_listener.png', LISTENER),
                         ('icon_armchair.png', ARMCHAIR)):
        write_png(OUT / name, render(shapes))


if __name__ == '__main__':
    main()
