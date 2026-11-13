#!/usr/bin/env python3
"""Makes the IEC 61937 test vectors in this folder with FFmpeg (run on the host, not in
the container: it needs an ffmpeg with the truehd, ac3, eac3 and dca encoders and the
spdif muxer; made with n9.0.2).

For each codec:
  <name>.es   a short elementary stream FFmpeg encodes from a generated signal
  <name>.ref  what `ffmpeg -c copy -f spdif` makes of it, stored sparse: records of
              u32 offset (LE), u16 length (LE), then that many bytes; every byte not in
              a record is zero, and the first record's offset 0 / last record end give
              no size, so the file starts with a u32 total length (LE).

TrueHD: FFmpeg's encoder spaces access units exactly 1/1200 s apart, so every unit would
sit inside one gap between MAT codes. Real encoders do not: input_timing jumps, as in a
stream cut mid-film. retime() adds a fixed jump to every unit after the first (fixing the
check nibble) so units fall across the middle code, the end code and the frame boundary.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def fold(x):
    return ((x >> 4) ^ x) & 0xF


def retime(data, jump):
    d = bytearray(data)
    i = k = 0
    while i + 4 <= len(d):
        n = ((d[i] & 0xF) << 8 | d[i + 1]) * 2
        if k >= 1:
            t = ((d[i + 2] << 8 | d[i + 3]) + jump) & 0xFFFF
            changed = (d[i + 2] ^ (t >> 8)) ^ (d[i + 3] ^ (t & 0xFF))
            d[i + 2], d[i + 3] = t >> 8, t & 0xFF
            d[i] ^= fold(changed) << 4  # keep the access unit's parity check valid
        i += n
        k += 1
    return bytes(d)


def sparse(ref):
    out = bytearray(struct.pack("<I", len(ref)))
    i = 0
    while i < len(ref):
        if ref[i] == 0:
            i += 1
            continue
        j = i
        zeros = 0
        while j < len(ref) and zeros < 16 and j - i < 0xFFFF:
            zeros = zeros + 1 if ref[j] == 0 else 0
            j += 1
        j -= zeros
        out += struct.pack("<IH", i, j - i) + ref[i:j]
        i = j
    return bytes(out)


def make(name, src, enc, fmt, retime_jump=0):
    es = os.path.join(HERE, name + ".es")
    ffmpeg("-f", "lavfi", "-i", src, *enc, "-f", fmt, es)
    if retime_jump:
        with open(es, "rb") as f:
            data = retime(f.read(), retime_jump)
        with open(es, "wb") as f:
            f.write(data)
    tmp = es + ".spdif"
    ffmpeg("-f", fmt, "-i", es, "-c", "copy", "-f", "spdif", tmp)
    with open(tmp, "rb") as f:
        ref = f.read()
    os.remove(tmp)
    with open(os.path.join(HERE, name + ".ref"), "wb") as f:
        f.write(sparse(ref))
    print(name, os.path.getsize(es), "bytes in,", len(ref), "bytes of bursts")


def main():
    noise = "anoisesrc=sample_rate={rate}:amplitude=0.002:duration={dur}:seed=1"
    thd = ["-ac", "2", "-c:a", "truehd", "-strict", "-2"]
    # 48 units = two MAT frames; a 39-sample jump (2496 bytes at 64 bytes per sample)
    # puts unit 11 across the middle code and unit 23 across the end code.
    make("truehd48", noise.format(rate=48000, dur=0.04), thd, "truehd", retime_jump=39)
    # 96 kHz: 80 samples per unit, still 1/1200 s; 78 samples is the same 2496 bytes.
    make("truehd96", noise.format(rate=96000, dur=0.04), thd, "truehd", retime_jump=78)
    sine = "sine=frequency=440:sample_rate=48000:duration={dur}"
    make("ac3", sine.format(dur=0.2), ["-ac", "2", "-c:a", "ac3", "-b:a", "96k"], "ac3")
    make("eac3", sine.format(dur=0.2), ["-ac", "2", "-c:a", "eac3", "-b:a", "96k"], "eac3")
    make("dts", sine.format(dur=0.06), ["-ac", "2", "-c:a", "dca", "-strict", "-2", "-b:a", "384k"], "dts")


if __name__ == "__main__":
    sys.exit(main())
