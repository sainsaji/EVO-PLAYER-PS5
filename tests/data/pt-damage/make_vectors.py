#!/usr/bin/env python3
"""Makes the AC-3, E-AC-3, DTS core and AAC test vectors in this folder with FFmpeg (run on the host,
not in the container: it needs an ffmpeg with the ac3, eac3, dca and aac encoders and
the spdif muxer; made with n9.0.2). tests/test_pt_damage.cpp damages frames of each
stream and expects FFmpeg's bursts for the other frames. (TrueHD's stream is built in
the test: the TrueHD packer reads only each unit's length word.)

  <name>.es    a 440 Hz sine, 48 kHz stereo: 6 frames (ac3, dts, dts-full, eac3), 25 (aac, ADTS;
               the encoder gives one more than the 24 asked for), or 18 E-AC-3 frames
               whose headers say 1 audio block (eac3-1blk). FFmpeg's encoder only writes
               6-block frames; the packer and FFmpeg's spdif muxer read only the header,
               so 6 of these frames make one burst.
  <name>.ref   `ffmpeg -c copy -f spdif` of it

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


def frames_of(data):
    """The E-AC-3 frames of a raw stream, by frmsiz."""
    frames, at = [], 0
    while at < len(data):
        size = ((((data[at + 2] & 7) << 8) | data[at + 3]) + 1) * 2
        frames.append(data[at:at + size])
        at += size
    return frames


def one_block(frame):
    """The frame with numblkscod 0 (1 block) in its header."""
    return frame[:4] + bytes([frame[4] & 0xCF]) + frame[5:]


def make_eac3_1blk(tmp, name, frames):
    """E-AC-3 frames whose headers say 1 block. The raw eac3 demuxer finds no sample rate
    for them (FFmpeg's decoder rejects them), so the ref comes from a Matroska file of the
    real frames, patched in place: its track header gives the rate."""
    raw = os.path.join(tmp, "real.eac3")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=%f" % ((frames + 2) * 1536 / 48000),
           "-ac", "2", "-c:a", "eac3", "-b:a", "192k", "-frames:a", str(frames), "-f", "eac3", raw)
    mka = os.path.join(tmp, "real.mka")
    ffmpeg("-f", "eac3", "-i", raw, "-c", "copy", "-write_crc32", "0", mka)
    with open(raw, "rb") as f:
        real = frames_of(f.read())
    with open(mka, "rb") as f:
        box = bytearray(f.read())
    for frame in real:
        at = box.find(frame)
        box[at:at + len(frame)] = one_block(frame)
    with open(mka, "wb") as f:
        f.write(box)
    es = os.path.join(HERE, name + ".es")
    with open(es, "wb") as f:
        f.write(b"".join(one_block(frame) for frame in real))
    out = os.path.join(tmp, "out.spdif")
    # -loglevel fatal: the decoder that probes the stream complains about every frame.
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "fatal", "-y", "-i", mka, "-c", "copy", "-f", "spdif", out],
                   check=True)
    with open(out, "rb") as f:
        ref = f.read()
    with open(os.path.join(HERE, name + ".ref"), "wb") as f:
        f.write(sparse(ref))
    print(name, os.path.getsize(es), "bytes in,", len(ref), "bytes of bursts")


def make(tmp, name, fmt, encoder, samples, frames, *opts, demuxer=None):
    es = os.path.join(HERE, name + ".es")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=%f" % ((frames + 2) * samples / 48000),
           "-ac", "2", "-c:a", encoder, *opts, "-frames:a", str(frames), "-f", fmt, es)
    out = os.path.join(tmp, "out.spdif")
    ffmpeg("-f", demuxer or fmt, "-i", es, "-c", "copy", "-f", "spdif", out)
    with open(out, "rb") as f:
        ref = f.read()
    with open(os.path.join(HERE, name + ".ref"), "wb") as f:
        f.write(sparse(ref))
    print(name, os.path.getsize(es), "bytes in,", len(ref), "bytes of bursts")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        make(tmp, "ac3", "ac3", "ac3", 1536, 6, "-b:a", "192k")
        make(tmp, "dts", "dts", "dca", 512, 6, "-strict", "-2", "-b:a", "384k")
        # 1536 kbit/s: 2048-byte frames that fill their burst, which FFmpeg sends without a preamble
        make(tmp, "dts-full", "dts", "dca", 512, 6, "-strict", "-2", "-b:a", "1536k")
        make(tmp, "eac3", "eac3", "eac3", 1536, 6, "-b:a", "192k")
        make_eac3_1blk(tmp, "eac3-1blk", 18)
        make(tmp, "aac", "adts", "aac", 1024, 24, "-b:a", "128k", demuxer="aac")


if __name__ == "__main__":
    sys.exit(main())
