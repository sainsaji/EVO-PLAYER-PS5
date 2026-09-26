#!/usr/bin/env python3
"""
tools/upcompare_run.py - the #103 upscaler A/B/C capture, driven over FTP.

Invoked by `tools/evo-remote.sh upcompare` (needs PS5_HOST / FTP_PORT). With a
video playing on a --usb-remote build it:

  1. sends `upcompare`: EVO pauses, then redraws the SAME held frame with the
     upscaler Off, Sharp, AI Standard and AI Large (OSD suppressed), saving the
     scanout after each to /mnt/usb0/evo_up_{off,sharp,ai,ai_large}.bmp, and restores Settings/pause state;
  2. waits for the last capture's line in evo.log;
  3. pulls the three BMPs into output/upcompare/ and, where Pillow is
     available, writes PNGs, a side-by-side 1:1 crop (compare.png) and
     per-mode difference stats against Off.

The montage step alone runs on the host too - handy because the container
may lack Pillow:

    python tools/upcompare_run.py --montage [--crop x,y,w,h]
"""

from __future__ import annotations

import argparse
import io
import os
import sys
import time
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "output" / "upcompare"
MODES = ("off", "sharp", "ai", "ai_large", "ai_max")
LOG = "/mnt/usb0/evo.log"


def ftp() -> FTP:
    f = FTP()
    f.connect(os.environ["PS5_HOST"], int(os.environ.get("FTP_PORT", "2121")), timeout=15)
    f.login()
    try:
        f.set_pasv(True)
    except Exception:
        pass
    return f


def fetch(path: str) -> bytes | None:
    buf = []
    try:
        with ftp() as f:
            f.retrbinary("RETR " + path, buf.append)
    except error_perm:
        return None
    return b"".join(buf)


def capture(timeout_s: float) -> int:
    # Only lines written after the command count - an earlier run's
    # "ai ->" line must not end the wait before this run has captured.
    seen = len((fetch(LOG) or b"").decode("utf-8", "replace").splitlines())
    with ftp() as f:
        for m in MODES:          # a stale capture must not pass for a new one
            try:
                f.sendcmd(f"DELE /mnt/usb0/evo_up_{m}.bmp")
            except error_perm:
                pass
        f.storbinary("STOR /mnt/usb0/evo_cmd", io.BytesIO(b"upcompare\n"))
    print("sent: upcompare - waiting for the three captures...")

    deadline = time.time() + timeout_s
    lines = []
    while time.time() < deadline:
        time.sleep(2)
        log = (fetch(LOG) or b"").decode("utf-8", "replace")
        lines = [l for l in log.splitlines()[seen:] if "upcompare:" in l]
        if any(" ai_max -> " in l for l in lines):
            break
    else:
        print("\n".join(lines[-4:]) or "(no upcompare lines in evo.log)")
        print("timed out - is a video playing, and is this a --usb-remote build?")
        return 1
    print("\n".join(lines[-4:]))

    OUT.mkdir(parents=True, exist_ok=True)
    for m in MODES:
        data = fetch(f"/mnt/usb0/evo_up_{m}.bmp")
        if not data:
            print(f"missing capture: evo_up_{m}.bmp")
            return 1
        (OUT / f"{m}.bmp").write_bytes(data)
        print(f"  pulled {m}.bmp ({len(data) >> 20} MB)")
    return 0


def busiest_window(img, cw: int, ch: int) -> tuple[int, int]:
    """Top-left of the cw x ch window with the most edge energy."""
    from PIL import ImageFilter, ImageStat
    edges = img.convert("L").filter(ImageFilter.FIND_EDGES)
    w, h = img.size
    best = (-1.0, 0, 0)
    for y in range(0, h - ch + 1, ch // 4):
        for x in range(0, w - cw + 1, cw // 4):
            e = ImageStat.Stat(edges.crop((x, y, x + cw, y + ch))).mean[0]
            if e > best[0]:
                best = (e, x, y)
    return best[1], best[2]


def montage(crop: str | None) -> int:
    try:
        from PIL import Image, ImageChops, ImageDraw, ImageStat
    except ImportError:
        print("Pillow not available here - run on the host:\n"
              "  python tools/upcompare_run.py --montage")
        return 0
    imgs = {m: Image.open(OUT / f"{m}.bmp").convert("RGB") for m in MODES}
    w, h = imgs["off"].size
    for m, im in imgs.items():
        im.save(OUT / f"{m}.png")

    # Default: the most detailed 1:1 window of the Off frame - a flat or
    # out-of-focus area shows no upscaler at all.
    if crop:
        x, y, cw, ch = (int(v) for v in crop.split(","))
    else:
        cw, ch = 640, 360
        x, y = busiest_window(imgs["off"], cw, ch)
    sheet = Image.new("RGB", (cw * len(MODES) + 10 * (len(MODES) - 1), ch + 40), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    for i, m in enumerate(MODES):
        sheet.paste(imgs[m].crop((x, y, x + cw, y + ch)), (i * (cw + 10), 40))
        draw.text((i * (cw + 10) + 8, 12), m.upper(), fill=(230, 230, 230))
    sheet.save(OUT / "compare.png")
    print(f"  compare.png: crop {x},{y} {cw}x{ch} of {w}x{h}, Off | Sharp | AI Standard | Large | Maximum")

    # grid.png: one row per AI network, Off | AI | Sharp in each. Off and
    # Sharp do not depend on the network, so they repeat.
    rows = (("STANDARD", "ai"), ("LARGE", "ai_large"), ("MAXIMUM (PRO)", "ai_max"))
    cols = lambda ai: (("OFF", "off"), ("AI", ai), ("SHARP", "sharp"))
    gap, head, side = 10, 40, 150
    grid = Image.new("RGB", (side + 3 * cw + 2 * gap, head + len(rows) * (ch + gap)),
                     (16, 16, 16))
    gd = ImageDraw.Draw(grid)
    for c, (label, _) in enumerate(cols("ai")):
        gd.text((side + c * (cw + gap) + 8, 14), label, fill=(230, 230, 230))
    for r, (rlabel, ai) in enumerate(rows):
        oy = head + r * (ch + gap)
        gd.text((10, oy + ch // 2), rlabel, fill=(230, 230, 230))
        for c, (_, m) in enumerate(cols(ai)):
            grid.paste(imgs[m].crop((x, y, x + cw, y + ch)), (side + c * (cw + gap), oy))
    grid.save(OUT / "grid.png")
    print(f"  grid.png: rows Standard / Large / Maximum, columns Off | AI | Sharp")

    # Same frame, so any difference is the upscaler. Off vs Off would be 0.
    for m in MODES[1:]:
        diff = ImageChops.difference(imgs["off"], imgs[m])
        mean = sum(ImageStat.Stat(diff).mean) / 3
        changed = diff.convert("L").point(lambda v: 255 if v > 8 else 0).histogram()[255]
        print(f"  {m:8s} vs off: mean |diff| {mean:.2f}/255, "
              f"{100.0 * changed / (w * h):.1f}% of pixels differ by > 8")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--montage", action="store_true", help="only rebuild the PNGs/montage")
    ap.add_argument("--crop", help="x,y,w,h of the 1:1 comparison crop")
    ap.add_argument("--timeout", type=float, default=90)
    args = ap.parse_args()
    if not args.montage:
        rc = capture(args.timeout)
        if rc:
            return rc
    return montage(args.crop)


if __name__ == "__main__":
    sys.exit(main())
