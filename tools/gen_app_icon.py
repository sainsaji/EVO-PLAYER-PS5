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

from gen_icons import write_png
import gen_app_icon_variants as variants



def render_icon(size, face="Lato-Bold"):
    """
    The shipping tile: PRO badge top-right, play mark centred, EVO across the
    bottom. Chosen from tools/gen_app_icon_variants.py, which renders the
    alternatives side by side at tile size.

    The wordmarks are set in the player's own UI font through ttf_text, not
    drawn as strokes. The previous version approximated E, V and O with line
    segments and a circle because this file claimed no font was available -
    it is, in assets/fonts/, and the approximation is what made the typography
    look wrong once a PRO badge needed a P and an R.
    """
    cv = variants.Canvas(size, face)
    variants.r2(cv)
    return cv.rows()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=512,
                    help="edge length in pixels (default 512)")
    ap.add_argument("--font", default="Lato-Bold",
                    help="face in assets/fonts (default Lato-Bold)")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    # dest was referenced by write_png() below but never assigned, so this
    # script raised NameError and the icon in the tree could not be regenerated.
    dest = os.path.join(root, "projects", "evoplayer", "sce_sys", "icon0.png")
    prev = os.path.join(root, "output", "screenshots", "app_icon_preview.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)

    rows = render_icon(args.size, args.font)
    write_png(dest, rows, args.size, args.size)
    write_png(prev, rows, args.size, args.size)

    print("wrote %s (%dx%d)" % (dest, args.size, args.size))
    print("wrote %s" % prev)


if __name__ == "__main__":
    main()
