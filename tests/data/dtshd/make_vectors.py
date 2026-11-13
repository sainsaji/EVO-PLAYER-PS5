#!/usr/bin/env python3
"""Makes the DTS-HD test vectors in this folder with FFmpeg (run on the host, not in the
container: it needs an ffmpeg with the dca encoder and the spdif muxer; made with n9.0.2).

FFmpeg cannot encode DTS-HD, and a film's soundtrack does not belong in the repository, so
the streams are built: FFmpeg encodes a 440 Hz sine as DTS core (48 kHz, 512 samples per
frame; one stream at 44.1 kHz), and each core frame is followed by zero or more extension
substreams. Each substream has a real header (sync 0x64582025, the 8/16-bit or the 12/20-bit size fields)
and a filler payload that holds no sync word. Only the sizes matter to the packer and to
FFmpeg's spdif muxer; FFmpeg's DTS parser splits the frames by them and warns "Invalid
extension size in EXSS asset descriptor" because the payload is not real.

  <name>.es        the stream
  <name>-t4.ref    `ffmpeg -c copy -f spdif -dtshd_rate 192000` of it (Type IV bursts)
  <name>-core.ref  `ffmpeg -c copy -f spdif` of it (DTS core bursts, the path when the
                   sink has no DTS-HD, or when the core has no Type IV period)

A .ref is stored sparse: u32 total length (LE), then records of u32 offset (LE), u16
length (LE) and that many bytes; every byte not in a record is zero.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


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


def exss(size, wide):
    """One extension substream of `size` bytes (ETSI TS 102 114 7.5.1)."""
    header_size = 16 - 1
    fsize = size - 1
    h = bytearray(b"\x64\x58\x20\x25\x00")  # sync, 8 user-defined bits
    if not wide:  # 2-bit index 0, header size type 0: 8-bit header size, 16-bit FSIZE
        h += bytes([(header_size >> 3) & 0x1F,
                    ((header_size & 7) << 5) | ((fsize >> 11) & 0x1F),
                    (fsize >> 3) & 0xFF,
                    (fsize & 7) << 5])
    else:  # header size type 1: 12-bit header size, 20-bit FSIZE
        h += bytes([0x20 | ((header_size >> 7) & 0x1F),
                    ((header_size & 0x7F) << 1) | ((fsize >> 19) & 1),
                    (fsize >> 11) & 0xFF,
                    (fsize >> 3) & 0xFF,
                    (fsize & 7) << 5])
    while len(h) < size:
        h.append((len(h) * 7 + 3) & 0xFF)  # steps of 7: never 7F FE, 64 58 or FE 7F
    return bytes(h)


def core_frames(tmp, count, rate):
    path = os.path.join(tmp, "core.dts")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=%d:duration=%f" % (rate, count * 512 / rate),
           "-ac", "2", "-c:a", "dca", "-strict", "-2", "-b:a", "384k", "-f", "dts", path)
    with open(path, "rb") as f:
        data = f.read()
    frames = []
    while data:
        size = (((data[5] & 3) << 12) | (data[6] << 4) | (data[7] >> 4)) + 1
        frames.append(data[:size])
        data = data[size:]
    return frames[:count]


def spdif(tmp, es, *opts):
    out = os.path.join(tmp, "out.spdif")
    ffmpeg("-f", "dts", "-i", es, "-c", "copy", "-f", "spdif", *opts, out)
    with open(out, "rb") as f:
        return f.read()


def make(tmp, name, plan, core_ref=True, t4_ref=True, rate=48000):
    """plan: per core frame, the (size, wide) of each extension substream after it."""
    frames = core_frames(tmp, len(plan), rate)
    stream = b"".join(core + b"".join(exss(s, w) for s, w in exts) for core, exts in zip(frames, plan))
    es = os.path.join(HERE, name + ".es")
    with open(es, "wb") as f:
        f.write(stream)
    refs = ([("-t4", ("-dtshd_rate", "192000"))] if t4_ref else []) + ([("-core", ())] if core_ref else [])
    for suffix, opts in refs:
        ref = spdif(tmp, es, *opts)
        with open(os.path.join(HERE, name + suffix + ".ref"), "wb") as f:
            f.write(sparse(ref))
        print(name + suffix, len(stream), "bytes in,", len(ref), "bytes of bursts")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # Both size-field widths, a frame with no extension, a frame with two, odd sizes.
        make(tmp, "dtshd", [[(1500, False)], [(3000, True)], [], [(2000, False), (1001, True)],
                            [(1201, False)], [(800, True)]])
        # Frame 1 does not fit the 8192-byte burst (512 + 7800 + 12 > 8184): FFmpeg sends
        # its core, and the cores only of what follows for 60 s (dtshd_fallback_time).
        make(tmp, "dtshd-big", [[(1500, False)], [(7800, True)], [(1200, False)]], core_ref=False)
        # A 44.1 kHz core: 512 samples give no Type IV period on the 192 kHz port, and
        # FFmpeg stops ("impossible repetition period"). dts_route keeps such a stream on the
        # DTS core path, and the packer drops such frames met mid-stream on the DTS-HD path.
        make(tmp, "dtshd-44k", [[(1500, False)], [(3000, True)], [(1201, False)]], t4_ref=False,
             rate=44100)


if __name__ == "__main__":
    sys.exit(main())
