#!/usr/bin/env bash
# Fetch the newest auto-screenshot from the console and convert it to PNG so
# it can be viewed directly. Used while iterating on the UI.
set -euo pipefail
N="${1:-}"
if [ -z "$N" ]; then
  N=$(curl -s --connect-timeout 8 "http://$PS5_HOST:8080/fs/mnt/usb0/?fmt=json" \
      | tr ',' '\n' | grep -oE 'evo_shot_[0-9]+\.bmp' | sort -u | tail -1)
fi
mkdir -p /workspace/output/screenshots
curl -s --connect-timeout 15 --max-time 180 \
  -o "/workspace/output/screenshots/$N" \
  "http://$PS5_HOST:8080/fs/mnt/usb0/$N"
python3 - "$N" <<'PY'
import struct, zlib, sys, os
n = sys.argv[1]
src = f"/workspace/output/screenshots/{n}"
dst = "/workspace/output/screenshots/latest.png"
d = open(src, "rb").read()
off = struct.unpack_from("<I", d, 10)[0]
w, h = struct.unpack_from("<ii", d, 18)
rb = w * 3; pad = (4 - (rb % 4)) % 4
scale = 2
ow, oh = w // scale, h // scale
out = []
for oy in range(oh):
    y = oy * scale
    s = off + (h - 1 - y) * (rb + pad)
    row = d[s:s + rb]
    line = bytearray()
    for ox in range(ow):
        i = (ox * scale) * 3
        line += bytes((row[i+2], row[i+1], row[i]))
    out.append(bytes(line))
raw = b"".join(b"\x00" + l for l in out)
def chunk(t, dd):
    c = t + dd
    return struct.pack(">I", len(dd)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
open(dst, "wb").write(png)
print(f"{n} -> latest.png ({ow}x{oh})")
PY
