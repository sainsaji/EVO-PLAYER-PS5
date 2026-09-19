#!/usr/bin/env python3
"""
Minimal TrueType text rasteriser — standard library only.

WHY THIS EXISTS
    gen_app_icon.py drew E, V and O as line segments and a circle, with a
    comment explaining that no font was available. That was wrong: the player
    ships Lato, Roboto, Noto and DejaVu in assets/fonts/, and RmlUi renders the
    real interface with them. The hand-drawn letters were the reason the icon's
    typography looked wrong - they are not letterforms, they are an
    approximation of letterforms, and P and R in particular have a bowl that no
    arrangement of primitives fakes convincingly.

    The container has no PIL, no fontTools and no freetype-py, and pip is
    blocked by PEP 668. The rest of tools/ is deliberately stdlib-only, so
    rather than add a dependency to an icon script, this parses the font
    directly.

WHAT IT SUPPORTS
    Enough TrueType for Latin caps from the project's own fonts: the sfnt table
    directory, head, hhea, hmtx, cmap format 4, loca and glyf, including
    composite glyphs. Outlines are quadratic B-splines, flattened and filled
    with a supersampled nonzero-winding scanline.

    Not supported, because nothing here needs it: CFF/OpenType outlines,
    hinting, GPOS kerning, ligatures.

USAGE
    import ttf_text
    f = ttf_text.Font("projects/evoplayer/assets/fonts/Lato-Bold.ttf")
    mask, w, h = f.text_mask("PRO", 96)      # coverage 0..1, row-major
"""

import struct


def _u16(b, o): return struct.unpack_from(">H", b, o)[0]
def _s16(b, o): return struct.unpack_from(">h", b, o)[0]
def _u32(b, o): return struct.unpack_from(">I", b, o)[0]


class Font:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.d = fh.read()
        d = self.d
        num_tables = _u16(d, 4)
        self.tables = {}
        for i in range(num_tables):
            o = 12 + i * 16
            tag = d[o:o + 4].decode("latin-1")
            self.tables[tag] = (_u32(d, o + 8), _u32(d, o + 12))

        head = self.tables["head"][0]
        self.units_per_em = _u16(d, head + 18)
        self.index_to_loc = _s16(d, head + 50)

        self.num_glyphs = _u16(d, self.tables["maxp"][0] + 4)
        self.num_hmetrics = _u16(d, self.tables["hhea"][0] + 34)
        self._cmap = self._parse_cmap()
        self._loca = self._parse_loca()

    # -- tables ------------------------------------------------------------
    def _parse_cmap(self):
        d = self.d
        base = self.tables["cmap"][0]
        best = None
        for i in range(_u16(d, base + 2)):
            o = base + 4 + i * 8
            pid, eid, off = _u16(d, o), _u16(d, o + 2), _u32(d, o + 4)
            if (pid, eid) in ((3, 1), (3, 0), (0, 3), (0, 4)):
                best = base + off
                if (pid, eid) == (3, 1):
                    break
        if best is None:
            raise ValueError("no usable cmap subtable")
        if _u16(d, best) != 4:
            raise ValueError("only cmap format 4 is supported")

        segs = _u16(d, best + 6) // 2
        ends = best + 14
        starts = ends + segs * 2 + 2
        deltas = starts + segs * 2
        ranges = deltas + segs * 2
        return (segs, ends, starts, deltas, ranges)

    def glyph_id(self, ch):
        d = self.d
        segs, ends, starts, deltas, ranges = self._cmap
        c = ord(ch)
        for i in range(segs):
            if _u16(d, ends + i * 2) >= c:
                s = _u16(d, starts + i * 2)
                if s > c:
                    return 0
                ro = _u16(d, ranges + i * 2)
                if ro == 0:
                    return (c + _s16(d, deltas + i * 2)) & 0xFFFF
                addr = ranges + i * 2 + ro + (c - s) * 2
                g = _u16(d, addr)
                return 0 if g == 0 else (g + _s16(d, deltas + i * 2)) & 0xFFFF
        return 0

    def _parse_loca(self):
        d = self.d
        off, _ = self.tables["loca"]
        n = self.num_glyphs + 1
        if self.index_to_loc == 0:
            return [_u16(d, off + i * 2) * 2 for i in range(n)]
        return [_u32(d, off + i * 4) for i in range(n)]

    def advance(self, gid):
        d = self.d
        off = self.tables["hmtx"][0]
        i = min(gid, self.num_hmetrics - 1)
        return _u16(d, off + i * 4)

    # -- outlines ----------------------------------------------------------
    def contours(self, gid, dx=0.0, dy=0.0, scale=1.0, depth=0):
        """Glyph outline as a list of closed point loops, in font units."""
        d = self.d
        gbase = self.tables["glyf"][0]
        start, end = self._loca[gid], self._loca[gid + 1]
        if start == end or depth > 4:
            return []
        o = gbase + start
        ncont = _s16(d, o)

        if ncont < 0:                      # composite
            out = []
            p = o + 10
            while True:
                flags, sub = _u16(d, p), _u16(d, p + 2)
                p += 4
                if flags & 1:              # ARG_1_AND_2_ARE_WORDS
                    a1, a2 = _s16(d, p), _s16(d, p + 2); p += 4
                else:
                    a1, a2 = struct.unpack_from(">bb", d, p); p += 2
                sc = 1.0
                if flags & 8:
                    sc = _s16(d, p) / 16384.0; p += 2
                elif flags & 0x40:
                    p += 4
                elif flags & 0x80:
                    p += 8
                out += self.contours(sub, dx + a1, dy + a2, sc, depth + 1)
                if not (flags & 0x20):     # MORE_COMPONENTS
                    break
            return out

        ends = [_u16(d, o + 10 + i * 2) for i in range(ncont)]
        npts = (ends[-1] + 1) if ends else 0
        p = o + 10 + ncont * 2
        p += 2 + _u16(d, p)                # skip instructions

        flags = []
        while len(flags) < npts:
            f = d[p]; p += 1
            flags.append(f)
            if f & 8:
                r = d[p]; p += 1
                flags += [f] * r

        xs, v = [], 0
        for f in flags:
            if f & 2:
                delta = d[p]; p += 1
                v += delta if (f & 16) else -delta
            elif not (f & 16):
                v += _s16(d, p); p += 2
            xs.append(v)
        ys, v = [], 0
        for f in flags:
            if f & 4:
                delta = d[p]; p += 1
                v += delta if (f & 32) else -delta
            elif not (f & 32):
                v += _s16(d, p); p += 2
            ys.append(v)

        loops, s = [], 0
        for e in ends:
            pts = [((xs[i] * scale + dx), (ys[i] * scale + dy), bool(flags[i] & 1))
                   for i in range(s, e + 1)]
            s = e + 1
            if pts:
                loops.append(self._flatten(pts))
        return loops

    @staticmethod
    def _flatten(pts, steps=8):
        """Quadratic contour -> polygon, inserting the implied on-curve points."""
        # normalise so the loop starts on-curve
        if not pts[0][2]:
            on = next((i for i, q in enumerate(pts) if q[2]), None)
            if on is None:                 # all off-curve: synthesise a start
                x0 = (pts[0][0] + pts[-1][0]) / 2.0
                y0 = (pts[0][1] + pts[-1][1]) / 2.0
                pts = [(x0, y0, True)] + pts
            else:
                pts = pts[on:] + pts[:on]

        out = [(pts[0][0], pts[0][1])]
        i, n = 1, len(pts)
        cur = (pts[0][0], pts[0][1])
        while i <= n:
            px, py, on = pts[i % n]
            if on:
                out.append((px, py)); cur = (px, py); i += 1
                continue
            nx, ny, non = pts[(i + 1) % n]
            if not non:                    # implied midpoint
                nx, ny = (px + nx) / 2.0, (py + ny) / 2.0
                step = 1
            else:
                step = 2
            for t in range(1, steps + 1):
                u = t / steps
                a = (1 - u) * (1 - u)
                b = 2 * (1 - u) * u
                c = u * u
                out.append((a * cur[0] + b * px + c * nx,
                            a * cur[1] + b * py + c * ny))
            cur = (nx, ny); i += step
        return out

    # -- rasterise ---------------------------------------------------------
    def text_mask(self, text, cap_px, ss=4, tracking=0.0):
        """
        Coverage mask for `text`, cap height `cap_px`, as (mask, w, h).

        Scaled so the cap height of 'H' is exactly cap_px, which is how the
        icon layout thinks about type - not em size, which varies by font.
        """
        cap_units = self._cap_height()
        scale = float(cap_px) / cap_units

        polys, pen, adv_extra = [], 0.0, tracking * cap_px
        for ch in text:
            gid = self.glyph_id(ch)
            for loop in self.contours(gid):
                polys.append([((x * scale) + pen, y * scale) for x, y in loop])
            pen += self.advance(gid) * scale + adv_extra
        if not polys:
            return [], 0, 0

        xs = [p[0] for lp in polys for p in lp]
        ys = [p[1] for lp in polys for p in lp]
        minx, maxx = min(xs), max(xs)
        miny, maxy = min(ys), max(ys)
        w = max(1, int(maxx - minx + 2))
        h = max(1, int(maxy - miny + 2))
        polys = [[(x - minx + 1, maxy - y + 1) for x, y in lp] for lp in polys]

        edges = []
        for lp in polys:
            for i in range(len(lp)):
                x0, y0 = lp[i]
                x1, y1 = lp[(i + 1) % len(lp)]
                if y0 != y1:
                    edges.append((x0, y0, x1, y1))

        mask = [0.0] * (w * h)
        inv = 1.0 / (ss * ss)
        for sy in range(h * ss):
            yc = (sy + 0.5) / ss
            hits = []
            for x0, y0, x1, y1 in edges:
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    t = (yc - y0) / (y1 - y0)
                    hits.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
            if not hits:
                continue
            hits.sort()
            wind, row = 0, sy // ss
            for i in range(len(hits) - 1):
                wind += hits[i][1]
                if wind == 0:
                    continue
                xa, xb = hits[i][0], hits[i + 1][0]
                for sx in range(max(0, int(xa * ss)), min(w * ss, int(xb * ss) + 1)):
                    xc = (sx + 0.5) / ss
                    if xa <= xc < xb:
                        mask[row * w + (sx // ss)] += inv
        return [min(1.0, v) for v in mask], w, h

    def measure(self, text, cap_px, tracking=0.0):
        """Advance width of `text` at cap height `cap_px`, in pixels.

        Layout should position from this rather than guessing: the first
        attempt at a two-colour EVO PRO lockup placed each word by eye and they
        overlapped."""
        scale = float(cap_px) / self._cap_height()
        w = 0.0
        for ch in text:
            w += self.advance(self.glyph_id(ch)) * scale + tracking * cap_px
        return w

    def _cap_height(self):
        """Cap height in font units, measured from 'H' rather than trusted
        from OS/2 - not every font here fills that field in."""
        loops = self.contours(self.glyph_id("H"))
        if loops:
            return max(y for lp in loops for _, y in lp)
        return self.units_per_em * 0.7
