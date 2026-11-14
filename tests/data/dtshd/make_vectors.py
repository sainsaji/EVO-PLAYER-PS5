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
  <name>-t4.ref    `ffmpeg -c copy -f spdif -dtshd_rate 192000` of it (Type IV bursts); for
                   a stream made from another by changing header fields, FFmpeg's output
                   on the same change inside Matroska (see make_damaged)
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


def max_fsize(stream, index):
    """`stream` with the FSIZE of its extension substream number `index` set to all ones,
    as damage would leave it: 64 KiB (16-bit field) or 1 MiB (20-bit) past the packet."""
    x = bytearray(stream)
    at = [i for i in range(len(x) - 3) if x[i:i + 4] == b"\x64\x58\x20\x25"][index]
    if x[at + 5] & 0x20:  # 20-bit FSIZE: bits 1-20 of bytes 6-9
        x[at + 6] |= 0x01
        x[at + 7] = x[at + 8] = 0xFF
        x[at + 9] |= 0xE0
    else:  # 16-bit FSIZE: bits 3-18 of bytes 6-8
        x[at + 6] |= 0x1F
        x[at + 7] = 0xFF
        x[at + 8] |= 0xE0
    return bytes(x)


def set_samples(stream, index, samples):
    """`stream` with the NBLKS of core frame number `index` set for `samples` per frame."""
    x = bytearray(stream)
    at = [i for i in range(len(x) - 3) if x[i:i + 4] == b"\x7f\xfe\x80\x01"][index]
    nblks = samples // 32 - 1
    x[at + 4] = (x[at + 4] & ~1) | (nblks >> 6)
    x[at + 5] = (x[at + 5] & 3) | ((nblks & 0x3F) << 2)
    return bytes(x)


def set_core_size(stream, index, change):
    """`stream` with the FSIZE of core frame number `index` `change` bytes off."""
    x = bytearray(stream)
    at = [i for i in range(len(x) - 3) if x[i:i + 4] == b"\x7f\xfe\x80\x01"][index]
    fsize = (((x[at + 5] & 3) << 12) | (x[at + 6] << 4) | (x[at + 7] >> 4)) + change
    x[at + 5] = (x[at + 5] & ~3) | (fsize >> 12)
    x[at + 6] = (fsize >> 4) & 0xFF
    x[at + 7] = ((fsize & 0xF) << 4) | (x[at + 7] & 0xF)
    return bytes(x)


def set_samples_from(stream, first, end, samples):
    """`stream` with core frames `first` to `end` - 1 set for `samples` per frame."""
    for index in range(first, end):
        stream = set_samples(stream, index, samples)
    return stream


def make_damaged(tmp, name, source, damage):
    """<name>.es: <source>.es with `damage` applied to it. Its ref is FFmpeg's on the same
    damage inside a Matroska file (-write_crc32 0), whose packets keep the frames whole: the
    raw dts demuxer would split the damaged stream by the bad sizes instead."""
    with open(os.path.join(HERE, source + ".es"), "rb") as f:
        stream = f.read()
    mkv = os.path.join(tmp, "clean.mkv")
    ffmpeg("-f", "dts", "-i", os.path.join(HERE, source + ".es"), "-c", "copy", "-write_crc32", "0", mkv)
    with open(mkv, "rb") as f:
        container = f.read()
    damaged = damage(stream)
    with open(os.path.join(HERE, name + ".es"), "wb") as f:
        f.write(damaged)
    with open(mkv, "wb") as f:
        f.write(damage(container))
    out = os.path.join(tmp, "out.spdif")
    ffmpeg("-i", mkv, "-c", "copy", "-f", "spdif", "-dtshd_rate", "192000", out)
    with open(out, "rb") as f:
        ref = f.read()
    with open(os.path.join(HERE, name + "-t4.ref"), "wb") as f:
        f.write(sparse(ref))
    print(name + "-t4", len(damaged), "bytes in,", len(ref), "bytes of bursts")


def padded(core):
    """`core` as a core of 2 bytes less, padded to 4 bytes with 2 zeros (FFmpeg's encoder
    makes only cores of whole 4-byte words, a real encoder at 754.5 kbps makes 1006 B ones)."""
    x = bytearray(set_core_size(core, 0, -2))
    x[-2:] = b"\0\0"
    return bytes(x)


def make(tmp, name, plan, core_ref=True, t4_ref=True, rate=48000, pad=False):
    """plan: per core frame, the (size, wide) of each extension substream after it. pad:
    each core 2 bytes short of a 4-byte boundary, then 2 bytes of padding."""
    frames = core_frames(tmp, len(plan), rate)
    if pad:
        frames = [padded(core) for core in frames]
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
        # Damage: frame 0's 16-bit FSIZE and frame 1's 20-bit FSIZE at their largest. FFmpeg
        # sends each packet whole, the bad size in it, and every frame stays Type IV.
        make_damaged(tmp, "dtshd-bad-fsize", "dtshd", lambda s: max_fsize(max_fsize(s, 0), 1))
        # Core sizes 64 bytes too big (frame 1) and too small (frame 3), still valid and inside
        # the packet: FFmpeg sends each packet whole all the same.
        make_damaged(tmp, "dtshd-core-size", "dtshd", lambda s: set_core_size(set_core_size(s, 1, 64), 3, -64))
        # Spec-valid changes of the frame length: frames 3 and 4 of 1024 samples, a short last
        # frame of 256. FFmpeg sends each at the Type IV subtype its length gives.
        make_damaged(tmp, "dtshd-shape", "dtshd",
                     lambda s: set_samples(set_samples(set_samples(s, 3, 1024), 4, 1024), 5, 256))
        # Frames that are too big start FFmpeg's 60 s core-only fallback wherever they are:
        # the first frame of a stream (dtshd-big-first), or a frame whose length changes to
        # 256 samples, which halves its burst. dtshd-mid's frames 0 and 2 (5012 B) fit a
        # 512-sample burst but not a 256-sample one: -short0 changes frame 0 alone. dtshd-chg
        # changes for good: from frame 3 (too big, then frames that fit) or from frame 6
        # (two too big).
        make(tmp, "dtshd-big-first", [[(7800, True)], [(1500, False)], [(1200, False)], [(1000, False)]],
             core_ref=False)
        make(tmp, "dtshd-mid", [[(4500, True)], [(1500, False)], [(4500, True)], [(1200, False)]],
             core_ref=False, t4_ref=False)
        make_damaged(tmp, "dtshd-mid-short0", "dtshd-mid", lambda s: set_samples(s, 0, 256))
        make(tmp, "dtshd-chg", [[(1500, False)]] * 3 + [[(4500, True)], [(1500, False)], [(1500, False)],
                                                        [(4500, True)], [(4500, True)]], core_ref=False,
             t4_ref=False)
        make_damaged(tmp, "dtshd-chg-from3", "dtshd-chg", lambda s: set_samples_from(s, 3, 8, 256))
        make_damaged(tmp, "dtshd-chg-from6", "dtshd-chg", lambda s: set_samples_from(s, 6, 8, 256))
        # Clean, with each core FSIZE 2 bytes short of a 4-byte boundary and padded to it.
        # FFmpeg's parser looks for the EXSS at the core size rounded up to 4 bytes
        # (dca_parser.c, FFALIGN(frame_size, 4)) and sends each packet whole.
        make(tmp, "dtshd-padded", [[(1500, False)], [(3000, True)], [], [(2000, False), (1001, True)],
                                   [(1201, False)], [(800, True)]], pad=True)


if __name__ == "__main__":
    sys.exit(main())
