#!/usr/bin/env python3
"""
gl_yuv_parity.py - #62 parity check for the GL-4 (#80) video path.

The CPU converter EVO shipped until GL-4 turned YUV 4:2:0 into BGRA with a
fixed-point BT.601 limited-range matrix. GL-4 deleted it and does the same
conversion in a GLSL fragment shader on the video quad. This compares the two
arithmetics exhaustively so "the colours look right" is a measurement rather
than an impression.

Reference  : pp/src/pp_converter.c :: yuv_to_bgra(), as of b8c42b7 (deleted by
             GL-4 Stage 3; recover with `git show b8c42b7:...`)
Under test : ui_rml/src/evo_gl_context_device.cpp :: YUV_MATRIX_GLSL

Both are sampled over every one of the 2^24 (Y,U,V) triples. Run:

    python3 tools/gl_yuv_parity.py            # summary
    python3 tools/gl_yuv_parity.py --verbose  # + the worst offenders

Exits non-zero if the two disagree by more than PARITY_MAX_DELTA on any
channel, so it can gate a change to either side.
"""
import argparse
import sys

# Largest per-channel 0-255 difference we accept between the two paths.
# 1 is the floor: the CPU path rounds a fixed-point integer (+128 >> 8), the
# shader rounds float -> unorm8 in the ROP. They cannot agree bit-exactly.
PARITY_MAX_DELTA = 1


def cpu_rgb(y, u, v):
    """pp_converter.c's integer matrix. Returns (r, g, b), each 0-255."""
    c = y - 16
    d = u - 128
    e = v - 128
    r = (298 * c + 409 * e + 128) >> 8
    g = (298 * c - 100 * d - 208 * e + 128) >> 8
    b = (298 * c + 516 * d + 128) >> 8
    clamp = lambda x: 0 if x < 0 else (255 if x > 255 else x)
    return clamp(r), clamp(g), clamp(b)


def gl_rgb(y, u, v):
    """The GLSL matrix, in the order the shader evaluates it.

    Y = (y - 16/255) * 1.1640625
    rgb = (Y + 1.59765625*V, Y - 0.390625*U - 0.8125*V, Y + 2.015625*U)
    with y/u/v arriving as unorm8 samples and the result clamped to [0,1] then
    written to an 8-bit render target (round-to-nearest).
    """
    yf = y / 255.0
    uf = u / 255.0 - 0.5019608
    vf = v / 255.0 - 0.5019608
    Y = (yf - 0.0627451) * 1.1640625
    rgb = (Y + 1.59765625 * vf,
           Y - 0.390625 * uf - 0.8125 * vf,
           Y + 2.015625 * uf)
    out = []
    for x in rgb:
        x = 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)
        out.append(int(x * 255.0 + 0.5))
    return tuple(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true",
                    help="list the triples that hit the maximum delta")
    ap.add_argument("--step", type=int, default=1,
                    help="chroma stride; 1 = exhaustive (default)")
    args = ap.parse_args()

    hist = {}
    worst = []
    worst_d = -1
    for u in range(0, 256, args.step):
        for v in range(0, 256, args.step):
            for y in range(256):
                a = cpu_rgb(y, u, v)
                b = gl_rgb(y, u, v)
                d = max(abs(a[i] - b[i]) for i in range(3))
                hist[d] = hist.get(d, 0) + 1
                if d > worst_d:
                    worst_d = d
                    worst = [(y, u, v, a, b)]
                elif d == worst_d and len(worst) < 8:
                    worst.append((y, u, v, a, b))

    total = sum(hist.values())
    print("#62 GL-4 YUV->RGB parity: CPU pp_converter.c vs the GLSL video shader")
    print("samples: %d  (Y 0-255 x U,V 0-255 step %d)" % (total, args.step))
    for d in sorted(hist):
        print("  delta %d : %10d  (%6.3f%%)" % (d, hist[d], 100.0 * hist[d] / total))
    print("max per-channel delta: %d  (budget %d)" % (worst_d, PARITY_MAX_DELTA))

    if args.verbose:
        print("worst cases (Y,U,V -> cpu / gl):")
        for y, u, v, a, b in worst:
            print("  %3d,%3d,%3d -> %s / %s" % (y, u, v, a, b))

    if worst_d > PARITY_MAX_DELTA:
        print("FAIL: the two paths disagree by more than the budget")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
