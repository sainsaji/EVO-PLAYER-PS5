"""Makes the benchmark's image set: photo-like JPEGs and PNGs with alpha.

usage: python3 -I tools/webbench/make_images.py third_party/ProsperoWeb/tools/bench/img

Deterministic (fixed seeds): smooth colour fields plus fine noise, so JPEG
decode has real work to do, like a photograph.
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def photo(seed, w, h):
    rnd = random.Random(seed)
    base = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(base)
    for _ in range(40):
        x0, y0 = rnd.randrange(w), rnd.randrange(h)
        x1, y1 = x0 + rnd.randrange(60, w // 2), y0 + rnd.randrange(60, h // 2)
        d.ellipse((x0, y0, x1, y1), fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    base = base.filter(ImageFilter.GaussianBlur(18))
    noise = Image.effect_noise((w, h), 28).convert("RGB")
    return Image.blend(base, noise, 0.18)


for i in range(20):
    photo(i, 960, 600).save(os.path.join(out, f"p{i}.jpg"), quality=82)

for i in range(12):
    rnd = random.Random(100 + i)
    ic = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(ic)
    d.rounded_rectangle((8, 8, 248, 248), radius=56, fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255))
    d.ellipse((60, 60, 196, 196), fill=(255, 255, 255, 200))
    d.ellipse((96, 96, 160, 160), fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255))
    ic.filter(ImageFilter.GaussianBlur(0.6)).save(os.path.join(out, f"i{i}.png"))

print("wrote", len(os.listdir(out)), "images to", out)
