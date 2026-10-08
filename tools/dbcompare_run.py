#!/usr/bin/env python3
"""
tools/dbcompare_run.py - the #119 Deep Blacks Off / Low / High capture.

Invoked by `tools/evo-remote.sh dbcompare` (needs PS5_HOST / FTP_PORT). With a
video playing on a --usb-remote build it:

  1. sends `dbcompare`: EVO pauses, redraws the SAME held frame with Deep Blacks
     Off, Low and High (OSD suppressed), saving the scanout after each to
     /mnt/usb0/evo_db_{off,low,high}.bmp, then restores Settings and pause;
  2. pulls the BMPs into output/dbcompare/;
  3. where Pillow is available writes PNGs, compare.png (the three frames side
     by side) and shadows.png (the same three with the darks lifted 6x, so a
     black-level change is visible on a monitor that would crush it), plus
     stats: share of pure-black pixels and mean of the darkest 10 % of pixels.

Rebuild the images from existing captures on the host:

    python tools/dbcompare_run.py --montage
"""

from __future__ import annotations

import argparse
import io
import sys
import time

from upcompare_run import LOG, ROOT, fetch, ftp, error_perm

OUT = ROOT / "output" / "dbcompare"
MODES = ("off", "low", "high")


def capture(timeout_s: float) -> int:
    seen = len((fetch(LOG) or b"").decode("utf-8", "replace").splitlines())
    with ftp() as f:
        for m in MODES:          # a stale capture must not pass for a new one
            try:
                f.sendcmd(f"DELE /mnt/usb0/evo_db_{m}.bmp")
            except error_perm:
                pass
        f.storbinary("STOR /mnt/usb0/evo_cmd", io.BytesIO(b"dbcompare\n"))
    print("sent: dbcompare - waiting for the three captures...")

    deadline = time.time() + timeout_s
    lines = []
    while time.time() < deadline:
        time.sleep(2)
        log = (fetch(LOG) or b"").decode("utf-8", "replace")
        lines = [l for l in log.splitlines()[seen:] if "dbcompare:" in l]
        if any(" high -> " in l for l in lines):
            break
    else:
        print("\n".join(lines[-4:]) or "(no dbcompare lines in evo.log)")
        print("timed out - is a video playing, and is this a --usb-remote build?")
        return 1
    print("\n".join(lines[-4:]))

    OUT.mkdir(parents=True, exist_ok=True)
    for m in MODES:
        data = fetch(f"/mnt/usb0/evo_db_{m}.bmp")
        if not data:
            print(f"missing capture: evo_db_{m}.bmp")
            return 1
        (OUT / f"{m}.bmp").write_bytes(data)
        print(f"  pulled {m}.bmp ({len(data) >> 20} MB)")
    return 0


def montage() -> int:
    try:
        from PIL import Image, ImageChops, ImageDraw, ImageStat
    except ImportError:
        print("Pillow not available here - run on the host:\n"
              "  python tools/dbcompare_run.py --montage")
        return 0
    imgs = {m: Image.open(OUT / f"{m}.bmp").convert("RGB") for m in MODES}
    w, h = imgs["off"].size
    for m, im in imgs.items():
        im.save(OUT / f"{m}.png")

    tw = 640
    th = round(h * tw / w)
    gap, head = 10, 36

    def sheet(name: str, tf, note: str) -> None:
        img = Image.new("RGB", (tw * 3 + 2 * gap, th + head), (16, 16, 16))
        d = ImageDraw.Draw(img)
        for i, m in enumerate(MODES):
            d.text((i * (tw + gap) + 8, 12), f"{m.upper()}  {note}", fill=(230, 230, 230))
            img.paste(tf(imgs[m]).resize((tw, th)), (i * (tw + gap), head))
        img.save(OUT / name)

    sheet("compare.png", lambda im: im, "")
    # Lift the darks 6x: 0..42 -> 0..252. Pure black stays black, a 0.02 floor
    # becomes a visible grey, so Off vs Low vs High reads on any monitor.
    sheet("shadows.png", lambda im: im.point(lambda v: min(255, v * 6)), "(darks x6)")
    print("  compare.png, shadows.png written")

    px = w * h
    for m in MODES:
        luma = imgs[m].convert("L")
        hist = luma.histogram()
        black = hist[0] / px * 100
        n10, acc, s = px // 10, 0, 0.0
        for v in range(256):          # mean of the darkest 10 % of pixels
            take = min(hist[v], n10 - acc)
            s += take * v
            acc += take
            if acc >= n10:
                break
        print(f"  {m:5s} pure black {black:5.1f}%   darkest-10% mean luma {s / max(acc, 1):5.2f}/255")
    for m in MODES[1:]:
        diff = ImageChops.difference(imgs["off"], imgs[m])
        mean = sum(ImageStat.Stat(diff).mean) / 3
        print(f"  {m:5s} vs off: mean |diff| {mean:.3f}/255")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--montage", action="store_true", help="only rebuild the PNGs/stats")
    ap.add_argument("--timeout", type=float, default=90)
    args = ap.parse_args()
    if not args.montage:
        rc = capture(args.timeout)
        if rc:
            return rc
    return montage()


if __name__ == "__main__":
    sys.exit(main())
