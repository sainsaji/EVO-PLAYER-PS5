#!/usr/bin/env python3
"""
tools/preview_icons.py - Contact-sheet preview for RmlUi icons onto the theme background (Issue #107)
"""
import os
import glob
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ICON_DIR = ROOT / "projects/evoplayer/assets/icons"
OUT_IMG = ROOT / "output/icons_contact_sheet.png"

BG_COLOR = (14, 22, 36, 255)       # Navy theme background
CARD_BG = (22, 33, 50, 255)        # Card surface
TEXT_COLOR = (200, 215, 235, 255)
ACCENT_COLOR = (255, 205, 0, 255)

def main():
    icons = sorted(glob.glob(str(ICON_DIR / "*.png")))
    if not icons:
        print("No icons found in", ICON_DIR)
        return 1

    cols = 6
    rows = (len(icons) + cols - 1) // cols
    cell_w, cell_h = 160, 160
    pad = 20

    sheet_w = cols * (cell_w + pad) + pad
    sheet_h = rows * (cell_h + pad) + pad + 60

    img = Image.new("RGBA", (sheet_w, sheet_h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # Title
    draw.text((pad, 20), "EVO PLAYER - ICONS CONTACT SHEET (ISSUE #107)", fill=ACCENT_COLOR)

    for idx, icon_path in enumerate(icons):
        r = idx // cols
        c = idx % cols
        x = pad + c * (cell_w + pad)
        y = 60 + pad + r * (cell_h + pad)

        # Card box
        draw.rounded_rectangle([x, y, x + cell_w, y + cell_h], radius=8, fill=CARD_BG)

        # Icon
        try:
            icon = Image.open(icon_path).convert("RGBA")
            # Resize for display if needed
            display_size = 64
            icon_thumb = icon.resize((display_size, display_size), Image.Resampling.LANCZOS)
            ix = x + (cell_w - display_size) // 2
            iy = y + 20
            img.alpha_composite(icon_thumb, (ix, iy))
        except Exception as e:
            print(f"Error loading {icon_path}: {e}")

        # Label
        name = os.path.basename(icon_path).replace(".png", "").replace("icon_", "").replace("btn_", "btn: ")
        draw.text((x + 10, y + cell_h - 30), name, fill=TEXT_COLOR)

    OUT_IMG.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT_IMG)
    print(f"Saved contact sheet to {OUT_IMG} ({len(icons)} icons)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
