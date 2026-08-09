#!/usr/bin/env python3
"""
tools/shot.py - read the framebuffer dumps the player writes, and measure them.

EVO_AUTOSHOT makes the player write a raw 24/32-bit BMP of the framebuffer to
the USB stick. This module turns those into PNGs you can look at, and - more
usefully - lets you interrogate them numerically.

That second part is the point. Several UI questions during the theming pass
were settled by reading pixel values out of a dump and not by zooming in on a
screenshot: "are these icon edges aliased, or is the blitter wrong?" looks the
same either way on screen, but the coverage ramp in the pixels answers it
outright. Guessing from a JPEG had already sent one investigation down the
wrong path.

Subcommands are driven by tools/shot.sh; run that instead of this directly.
"""

import struct
import sys
import zlib


# --------------------------------------------------------------------------
# BMP
# --------------------------------------------------------------------------

class Frame:
    """A decoded framebuffer dump, addressed top-left origin."""

    def __init__(self, width, height, rows):
        self.w = width
        self.h = height
        self.rows = rows          # list of bytes, top row first, RGB triples

    @classmethod
    def load(cls, path):
        data = open(path, "rb").read()

        if data[:2] != b"BM":
            raise SystemExit(f"{path}: not a BMP (magic {data[:2]!r})")

        pixel_offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        bpp = struct.unpack_from("<H", data, 28)[0]
        compression = struct.unpack_from("<I", data, 30)[0]

        if compression != 0 or bpp not in (24, 32):
            raise SystemExit(
                f"{path}: unsupported BMP (bpp={bpp} compression={compression})"
            )

        # A negative height means the rows are already top-down.
        top_down = height < 0
        height = abs(height)

        stride = width * (bpp // 8)
        stride += (4 - (stride % 4)) % 4

        rows = []
        for y in range(height):
            src_y = y if top_down else (height - 1 - y)
            start = pixel_offset + src_y * stride
            raw = data[start:start + stride]

            if bpp == 24:
                rows.append(bytes(raw[:width * 3]))
            else:
                # BGRA -> RGB, dropping alpha: the dump is opaque.
                out = bytearray(width * 3)
                for x in range(width):
                    i = x * 4
                    out[x * 3 + 0] = raw[i + 2]
                    out[x * 3 + 1] = raw[i + 1]
                    out[x * 3 + 2] = raw[i + 0]
                rows.append(bytes(out))

        # BMP stores BGR; flip to RGB for the 24-bit path.
        if bpp == 24:
            flipped = []
            for row in rows:
                out = bytearray(len(row))
                out[0::3] = row[2::3]
                out[1::3] = row[1::3]
                out[2::3] = row[0::3]
                flipped.append(bytes(out))
            rows = flipped

        return cls(width, height, rows)

    def pixel(self, x, y):
        if not (0 <= x < self.w and 0 <= y < self.h):
            raise SystemExit(f"({x},{y}) is outside {self.w}x{self.h}")
        row = self.rows[y]
        i = x * 3
        return row[i], row[i + 1], row[i + 2]

    def crop(self, x, y, w, h):
        x = max(0, min(x, self.w))
        y = max(0, min(y, self.h))
        w = max(0, min(w, self.w - x))
        h = max(0, min(h, self.h - y))
        rows = [self.rows[y + j][x * 3:(x + w) * 3] for j in range(h)]
        return Frame(w, h, rows)

    def scaled(self, factor):
        """Nearest-neighbour downscale. Deliberately not averaging: this is a
        measurement tool, and an averaged pixel is not a pixel that was ever
        on screen."""
        if factor <= 1:
            return self
        ow, oh = self.w // factor, self.h // factor
        rows = []
        for oy in range(oh):
            src = self.rows[oy * factor]
            out = bytearray(ow * 3)
            for ox in range(ow):
                i = (ox * factor) * 3
                out[ox * 3:ox * 3 + 3] = src[i:i + 3]
            rows.append(bytes(out))
        return Frame(ow, oh, rows)

    def write_png(self, path):
        raw = b"".join(b"\x00" + r for r in self.rows)

        def chunk(tag, payload):
            body = tag + payload
            return (struct.pack(">I", len(payload)) + body
                    + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR",
                       struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(raw, 6))
               + chunk(b"IEND", b""))
        open(path, "wb").write(png)


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_png(args):
    """png <in.bmp> <out.png> [scale]"""
    src, dst = args[0], args[1]
    scale = int(args[2]) if len(args) > 2 else 2

    frame = Frame.load(src).scaled(scale)
    frame.write_png(dst)
    print(f"{src} -> {dst} ({frame.w}x{frame.h}, 1/{scale})")


def cmd_probe(args):
    """probe <in.bmp> <x> <y> [w h]

    Print the colour at a point, or a summary of a region. Coordinates are in
    1080p framebuffer space, which is what the UI source uses, so a value from
    evo_metrics.h can be pasted straight in.
    """
    src = args[0]
    x, y = int(args[1]), int(args[2])
    frame = Frame.load(src)

    if len(args) <= 3:
        r, g, b = frame.pixel(x, y)
        print(f"({x},{y}) = #{r:02X}{g:02X}{b:02X}  rgb({r},{g},{b})")
        return

    w, h = int(args[3]), int(args[4])
    region = frame.crop(x, y, w, h)

    counts = {}
    total_r = total_g = total_b = 0
    n = 0
    for row in region.rows:
        for i in range(0, len(row), 3):
            px = (row[i], row[i + 1], row[i + 2])
            counts[px] = counts.get(px, 0) + 1
            total_r += px[0]
            total_g += px[1]
            total_b += px[2]
            n += 1

    if n == 0:
        print("empty region")
        return

    print(f"region ({x},{y}) {region.w}x{region.h} - {n} px, "
          f"{len(counts)} distinct")
    print(f"  mean     #{total_r // n:02X}{total_g // n:02X}{total_b // n:02X}")
    for px, count in sorted(counts.items(), key=lambda kv: -kv[1])[:6]:
        pct = 100.0 * count / n
        print(f"  #{px[0]:02X}{px[1]:02X}{px[2]:02X}  {count:6d}  {pct:5.1f}%")


def cmd_scan(args):
    """scan <in.bmp> row|col <index> [start] [end]

    Print the pixel run along a line, collapsing repeats. This is how you check
    whether an edge carries an antialiasing ramp or steps straight from
    background to full intensity - the question that distinguished "the
    blitter is wrong" from "the artwork was exported aliased".
    """
    src = args[0]
    axis = args[1]
    index = int(args[2])
    frame = Frame.load(src)

    if axis == "row":
        start = int(args[3]) if len(args) > 3 else 0
        end = int(args[4]) if len(args) > 4 else frame.w
        samples = [(x, frame.pixel(x, index)) for x in range(start, min(end, frame.w))]
    elif axis == "col":
        start = int(args[3]) if len(args) > 3 else 0
        end = int(args[4]) if len(args) > 4 else frame.h
        samples = [(y, frame.pixel(index, y)) for y in range(start, min(end, frame.h))]
    else:
        raise SystemExit("scan axis must be 'row' or 'col'")

    run_start, run_colour, run_len = None, None, 0
    for pos, px in samples:
        if px == run_colour:
            run_len += 1
            continue
        if run_colour is not None:
            end_pos = run_start + run_len - 1
            span = f"{run_start}" if run_len == 1 else f"{run_start}..{end_pos}"
            print(f"  {span:>12}  x{run_len:<4} "
                  f"#{run_colour[0]:02X}{run_colour[1]:02X}{run_colour[2]:02X}")
        run_start, run_colour, run_len = pos, px, 1

    if run_colour is not None:
        end_pos = run_start + run_len - 1
        span = f"{run_start}" if run_len == 1 else f"{run_start}..{end_pos}"
        print(f"  {span:>12}  x{run_len:<4} "
              f"#{run_colour[0]:02X}{run_colour[1]:02X}{run_colour[2]:02X}")


def cmd_crop(args):
    """crop <in.bmp> <out.png> <x> <y> <w> <h>

    Cut a region out at full resolution. Useful for looking closely at one
    card or one icon without the surrounding 1920x1080 of context.
    """
    src, dst = args[0], args[1]
    x, y, w, h = (int(v) for v in args[2:6])
    Frame.load(src).crop(x, y, w, h).write_png(dst)
    print(f"{src} [{x},{y} {w}x{h}] -> {dst}")


def cmd_diff(args):
    """diff <a.bmp> <b.bmp> [threshold]

    Where two captures differ. Answers "did that change do anything?" without
    flicking between two images.
    """
    threshold = int(args[2]) if len(args) > 2 else 8
    a = Frame.load(args[0])
    b = Frame.load(args[1])

    if (a.w, a.h) != (b.w, b.h):
        raise SystemExit(f"size mismatch: {a.w}x{a.h} vs {b.w}x{b.h}")

    changed = 0
    min_x, min_y, max_x, max_y = a.w, a.h, -1, -1

    for y in range(a.h):
        ra, rb = a.rows[y], b.rows[y]
        if ra == rb:
            continue
        for x in range(a.w):
            i = x * 3
            if (abs(ra[i] - rb[i]) > threshold
                    or abs(ra[i + 1] - rb[i + 1]) > threshold
                    or abs(ra[i + 2] - rb[i + 2]) > threshold):
                changed += 1
                min_x = min(min_x, x); max_x = max(max_x, x)
                min_y = min(min_y, y); max_y = max(max_y, y)

    total = a.w * a.h
    print(f"{changed} of {total} px differ ({100.0 * changed / total:.2f}%) "
          f"at threshold {threshold}")
    if changed:
        print(f"  bounding box: ({min_x},{min_y}) to ({max_x},{max_y}) "
              f"= {max_x - min_x + 1}x{max_y - min_y + 1}")


COMMANDS = {
    "png": cmd_png,
    "probe": cmd_probe,
    "scan": cmd_scan,
    "crop": cmd_crop,
    "diff": cmd_diff,
}


def main(argv):
    if len(argv) < 2 or argv[1] not in COMMANDS:
        print(__doc__)
        print("commands:")
        for name, fn in COMMANDS.items():
            first = (fn.__doc__ or name).strip().splitlines()[0]
            print(f"  {first}")
        return 1

    COMMANDS[argv[1]](argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
