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

`make_vectors.py damaged` only remakes the damaged TrueHD streams from truehd48.es (the
other vectors stay as they are), as a cut or garbled download can leave them:
  truehd48-zerolen   unit 5's length word is 0
  truehd48-cut       unit 5 is cut in half and the data goes on from the middle of unit 6
  truehd48-overlong  unit 5's length word claims 512 bytes more than the unit holds
                     (check nibble fixed, so the length looks valid)
  truehd48-badsync   unit 16's major sync says 96 kHz (no check nibble covers a major
                     sync; its checksum no longer matches)
FFmpeg's TrueHD parser (libavcodec/mlp_parser.c) takes a unit by its length word. A length
of 0, a failed check nibble or a failed major sync checksum loses sync, and it drops data
up to the next major sync (unit 16, or 32 for badsync); the spdif muxer carries on from
there. For zerolen and badsync the .ref is FFmpeg's output for the .es itself. For cut and
overlong the parser hands over unit 5 at its stated length; the next "unit" has a garbled
length (5122 bytes in cut) and fails the check nibble, but n9.0.2 reads the nibble only
once that many bytes are in. On this short file it then sends nothing more, and on a
longer one it resumes at a later major sync that depends on how its input was read. So
their .ref is FFmpeg's output for the units it did hand over followed by the units from
the next major sync: the output when it resyncs at the first major sync.
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
    spdif_ref(name, fmt, es, es)


def spdif_ref(name, fmt, src, es):
    """Writes <name>.ref: what FFmpeg's spdif muxer makes of `src`."""
    tmp = es + ".spdif"
    ffmpeg("-f", fmt, "-i", src, "-c", "copy", "-f", "spdif", tmp)
    with open(tmp, "rb") as f:
        ref = f.read()
    os.remove(tmp)
    with open(os.path.join(HERE, name + ".ref"), "wb") as f:
        f.write(sparse(ref))
    print(name, os.path.getsize(es), "bytes in,", len(ref), "bytes of bursts")


def unit_words(u, i=0):
    """An access unit's length in 16-bit words, from its first word (the check nibble above it)."""
    return (u[i] & 0xF) << 8 | u[i + 1]


def units(data):
    out = []
    i = 0
    while i + 4 <= len(data):
        n = unit_words(data, i) * 2
        out.append(data[i:i + n])
        i += n
    return out


def make_damaged(name, damage, unit, resync_unit=None):
    """Damages access unit `unit` of truehd48.es. With `resync_unit`, the .ref is FFmpeg's
    output for the units its parser hands over up to and with the damaged one, then the
    units from `resync_unit` on; without, FFmpeg's output for the damaged stream."""
    with open(os.path.join(HERE, "truehd48.es"), "rb") as f:
        u = [bytearray(x) for x in units(f.read())]
    damaged = damage(u, unit)
    es = os.path.join(HERE, name + ".es")
    with open(es, "wb") as f:
        f.write(damaged)
    if resync_unit is None:
        spdif_ref(name, "truehd", es, es)
        return
    kept = os.path.join(HERE, name + ".kept")
    with open(kept, "wb") as f:
        f.write(b"".join(units(damaged)[:unit + 1]) + b"".join(u[resync_unit:]))
    spdif_ref(name, "truehd", kept, es)
    os.remove(kept)


def zero_length(u, k):
    u[k][0] &= 0xF0
    u[k][1] = 0
    return b"".join(u)


def cut(u, k):
    return b"".join(u[:k] + [u[k][:len(u[k]) // 2], u[k + 1][len(u[k + 1]) // 2:]] + u[k + 2:])


def overlong(u, k):
    n = unit_words(u[k]) + 256
    changed = (u[k][0] & 0xF) ^ (n >> 8) ^ u[k][1] ^ (n & 0xFF)
    u[k][0] = (u[k][0] & 0xF0) ^ (fold(changed) << 4) | (n >> 8)
    u[k][1] = n & 0xFF
    return b"".join(u)


def bad_major_sync(u, k):
    u[k][8] ^= 0x10  # rate nibble: 48 kHz -> 96 kHz
    return b"".join(u)


def main():
    if sys.argv[1:] == ["damaged"]:
        make_damaged("truehd48-zerolen", zero_length, 5)
        make_damaged("truehd48-cut", cut, 5, resync_unit=16)
        make_damaged("truehd48-overlong", overlong, 5, resync_unit=16)
        make_damaged("truehd48-badsync", bad_major_sync, 16)
        return 0
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
