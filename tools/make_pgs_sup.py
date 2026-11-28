#!/usr/bin/env python3
"""Write a minimal valid PGS (.sup) subtitle stream: a white bar shown for
1.5 s every <every> seconds. Used to build a test file with picture subtitles
(FFmpeg has no PGS encoder), e.g. for #266:

    python3 tools/make_pgs_sup.py out.sup --duration 1800 --every 4
    ffmpeg -i video.mkv -i out.sup -map 0 -map 1 -c copy with_pgs.mkv
"""
import argparse
import struct

W, H = 1920, 1080
OW, OH = 600, 60


def seg(kind, payload, pts):
    t = int(pts * 90000) & 0xFFFFFFFF
    return b"PG" + struct.pack(">IIBH", t, t, kind, len(payload)) + payload


def rle_bar():
    out = bytearray()
    for _ in range(OH):
        # one run of colour 1, OW pixels long, then end of line
        out += bytes([0x00, 0xC0 | (OW >> 8), OW & 0xFF, 0x01, 0x00, 0x00])
    return bytes(out)


def display_set(pts, comp, show):
    s = bytearray()
    if show:
        pcs = struct.pack(">HHBHBBBB", W, H, 0x10, comp, 0x80, 0, 0, 1)
        pcs += struct.pack(">HBBHH", 0, 0, 0, (W - OW) // 2, H - OH - 80)
        s += seg(0x16, pcs, pts)
        s += seg(0x17, struct.pack(">BBHHHH", 1, 0, (W - OW) // 2, H - OH - 80, OW, OH), pts)
        pds = struct.pack(">BB", 0, 0) + bytes([1, 235, 128, 128, 255])
        s += seg(0x14, pds, pts)
        data = struct.pack(">HH", OW, OH) + rle_bar()
        ods = struct.pack(">HBB", 0, 0, 0xC0) + struct.pack(">I", len(data))[1:] + data
        s += seg(0x15, ods, pts)
    else:
        pcs = struct.pack(">HHBHBBBB", W, H, 0x10, comp, 0x00, 0, 0, 0)
        s += seg(0x16, pcs, pts)
        s += seg(0x17, struct.pack(">BBHHHH", 1, 0, (W - OW) // 2, H - OH - 80, OW, OH), pts)
    s += seg(0x80, b"", pts)
    return bytes(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--duration", type=float, default=1800.0)
    ap.add_argument("--every", type=float, default=4.0)
    a = ap.parse_args()
    comp = 0
    with open(a.out, "wb") as f:
        t = 1.0
        while t < a.duration:
            f.write(display_set(t, comp, True)); comp += 1
            f.write(display_set(t + 1.5, comp, False)); comp += 1
            t += a.every
    print(f"wrote {a.out}: {comp} display sets")


if __name__ == "__main__":
    main()
