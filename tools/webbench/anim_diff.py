import sys
from PIL import Image, ImageChops

prefix = sys.argv[1]
n = int(sys.argv[2])
regions = {
    "spinner": (150, 85, 190, 122),
    "bar": (510, 92, 735, 112),
    "ticker": (745, 90, 1900, 118),
    "canvas": (1600, 815, 1900, 925),
    "fig bgmove": (1255, 350, 1540, 530),
}
imgs = [Image.open(f"output/anim/{prefix}_{i}.png").convert("RGB") for i in range(1, n + 1)]
for name, box in regions.items():
    diffs = []
    for a, b in zip(imgs, imgs[1:]):
        d = ImageChops.difference(a.crop(box), b.crop(box))
        px = list(d.getdata())
        changed = sum(1 for p in px if max(p) > 24)
        diffs.append(round(100.0 * changed / len(px), 1))
    print(f"{name:12s} changed % between shots: {diffs}")
