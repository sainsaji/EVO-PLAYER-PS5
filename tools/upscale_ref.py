#!/usr/bin/env python3
"""Host reference for the #103 upscaler: the shader maths, in numpy.

Runs exactly what tools/gen_upscale_pipes.py emits - FSR1 EASU + RCAS, and
Anime4K CNN x2 S / M with the same weights, the same clamped texel fetches and
the same depth-to-space - on a still frame, so a console screenshot of that
frame can be diffed against it (tools/shot.py diff). A large difference means
wrong weights, wrong sampling or a wrong pass order on the GPU; a small one is
fp16 feature maps and 8-bit rounding.

    python tools/upscale_ref.py frame.png --size 3840x2160 --out output/upscale_ref
    python tools/upscale_ref.py --selftest

The input is the RGB frame at SOURCE resolution (what L0 holds after the YUV
pass). Outputs <mode>.png for bilinear, sharp, ai-s and ai-m at --size, which
is the image rectangle (no letterbox bars).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_upscale_pipes import ANIME4K, parse_hooks  # noqa: E402

RCAS_SHARPNESS = 0.8705506


# --------------------------------------------------------------------------
# Sampling helpers (GLSL semantics: texel centres at +0.5, clamp to edge)
# --------------------------------------------------------------------------

def fetch(img: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    return img[np.clip(y, 0, h - 1), np.clip(x, 0, w - 1)]


def bilinear(img: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    x = u * w - 0.5
    y = v * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    a = fetch(img, x0, y0) * (1 - fx) + fetch(img, x0 + 1, y0) * fx
    b = fetch(img, x0, y0 + 1) * (1 - fx) + fetch(img, x0 + 1, y0 + 1) * fx
    return a * (1 - fy) + b * fy


def out_uv(w: int, h: int, y0: int, y1: int):
    """vUV at the pixel centres of output rows [y0, y1)."""
    xs = (np.arange(w) + 0.5) / w
    ys = (np.arange(y0, y1) + 0.5) / h
    return np.meshgrid(xs, ys)


def luma(c: np.ndarray) -> np.ndarray:
    return c[..., 2] * 0.5 + (c[..., 0] * 0.5 + c[..., 1])


# --------------------------------------------------------------------------
# FSR1 EASU + RCAS
# --------------------------------------------------------------------------

def easu(src: np.ndarray, w: int, h: int, rows: int = 128) -> np.ndarray:
    out = np.empty((h, w, 3), np.float32)
    sh, sw = src.shape[:2]
    for r0 in range(0, h, rows):
        u, v = out_uv(w, h, r0, min(h, r0 + rows))
        ppx, ppy = u * sw - 0.5, v * sh - 0.5
        fx, fy = np.floor(ppx), np.floor(ppy)
        px, py = ppx - fx, ppy - fy
        ox, oy = fx.astype(np.int64), fy.astype(np.int64)

        def S(dx, dy):
            return fetch(src, ox + dx, oy + dy)

        C = {k: S(*d) for k, d in {
            "b": (0, -1), "c": (1, -1), "e": (-1, 0), "f": (0, 0), "g": (1, 0),
            "h": (2, 0), "i": (-1, 1), "j": (0, 1), "k": (1, 1), "l": (2, 1),
            "n": (0, 2), "o": (1, 2)}.items()}
        Lm = {k: luma(c) for k, c in C.items()}

        dirx = np.zeros_like(px)
        diry = np.zeros_like(px)
        ln = np.zeros_like(px)

        def setf(wt, a, b, c, d, e):
            nonlocal dirx, diry, ln
            lx = 1.0 / np.maximum(np.maximum(np.abs(d - c), np.abs(c - b)), 1 / 65536)
            dx = d - b
            dirx = dirx + dx * wt
            lx = np.clip(np.abs(dx) * lx, 0, 1)
            ln = ln + lx * lx * wt
            ly = 1.0 / np.maximum(np.maximum(np.abs(e - c), np.abs(c - a)), 1 / 65536)
            dy = e - a
            diry = diry + dy * wt
            ly = np.clip(np.abs(dy) * ly, 0, 1)
            ln = ln + ly * ly * wt

        setf((1 - px) * (1 - py), Lm["b"], Lm["e"], Lm["f"], Lm["g"], Lm["j"])
        setf(px * (1 - py), Lm["c"], Lm["f"], Lm["g"], Lm["h"], Lm["k"])
        setf((1 - px) * py, Lm["f"], Lm["i"], Lm["j"], Lm["k"], Lm["n"])
        setf(px * py, Lm["g"], Lm["j"], Lm["k"], Lm["l"], Lm["o"])

        dirR = dirx * dirx + diry * diry
        zro = dirR < 1 / 32768
        dirR = np.where(zro, 1.0, 1.0 / np.sqrt(np.maximum(dirR, 1e-30)))
        dirx = np.where(zro, 1.0, dirx)
        dirx, diry = dirx * dirR, diry * dirR
        ln = (ln * 0.5) ** 2
        stretch = (dirx * dirx + diry * diry) / np.maximum(np.abs(dirx), np.abs(diry))
        l2x = 1 + (stretch - 1) * ln
        l2y = 1 - 0.5 * ln
        lob = 0.5 + ((1 / 4 - 0.04) - 0.5) * ln
        clp = 1 / lob

        aC = np.zeros(px.shape + (3,), np.float64)
        aW = np.zeros_like(px)
        for k, (tx, ty) in {"b": (0, -1), "c": (1, -1), "i": (-1, 1), "j": (0, 1),
                            "f": (0, 0), "e": (-1, 0), "k": (1, 1), "l": (2, 1),
                            "h": (2, 0), "g": (1, 0), "o": (1, 2), "n": (0, 2)}.items():
            offx, offy = tx - px, ty - py
            vx = (offx * dirx + offy * diry) * l2x
            vy = (offx * -diry + offy * dirx) * l2y
            d2 = np.minimum(vx * vx + vy * vy, clp)
            wB = (0.4 * d2 - 1) ** 2
            wA = (lob * d2 - 1) ** 2
            wt = (1.5625 * wB - 0.5625) * wA
            aC += C[k] * wt[..., None]
            aW += wt
        mn = np.minimum(np.minimum(C["f"], C["g"]), np.minimum(C["j"], C["k"]))
        mx = np.maximum(np.maximum(C["f"], C["g"]), np.maximum(C["j"], C["k"]))
        out[r0:r0 + rows] = np.clip(aC / np.maximum(aW, 1 / 65536)[..., None], mn, mx)
    return out


def rcas(img: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    b = fetch(img, xx, yy - 1)
    d = fetch(img, xx - 1, yy)
    e = img
    f = fetch(img, xx + 1, yy)
    hh = fetch(img, xx, yy + 1)
    bL, dL, eL, fL, hL = (luma(t) for t in (b, d, e, f, hh))
    nz = 0.25 * (bL + dL + fL + hL) - eL
    lmax = np.max([bL, dL, eL, fL, hL], axis=0)
    lmin = np.min([bL, dL, eL, fL, hL], axis=0)
    nz = np.clip(np.abs(nz) / np.maximum(lmax - lmin, 1 / 65536), 0, 1)
    nz = -0.5 * nz + 1
    mn4 = np.minimum(np.minimum(b, d), np.minimum(f, hh))
    mx4 = np.maximum(np.maximum(b, d), np.maximum(f, hh))
    hit_min = np.minimum(mn4, e) / np.maximum(4 * mx4, 1 / 65536)
    hit_max = (1 - np.maximum(mx4, e)) / np.minimum(4 * mn4 - 4, -1 / 65536)
    lobe_rgb = np.maximum(-hit_min, hit_max)
    lobe = np.maximum(-(0.25 - 1 / 16), np.minimum(lobe_rgb.max(axis=-1), 0)) * RCAS_SHARPNESS
    lobe = (lobe * nz)[..., None]
    return np.clip((lobe * (b + d + f + hh) + e) / (4 * lobe + 1), 0, 1)


# --------------------------------------------------------------------------
# Anime4K CNN x2
# --------------------------------------------------------------------------

_MAT = re.compile(r"mat4\(([^)]*)\)\s*\*\s*(?:(go_(\d))\((-?[\d.]+), (-?[\d.]+)\)|g_(\d+))")
_BIAS = re.compile(r"result \+= vec4\(([^)]*)\)\s*;")


def mat4(text: str) -> np.ndarray:
    vals = [float(v) for v in text.split(",")]
    # GLSL mat4(...) is column-major; M @ v with M[row, col].
    return np.array(vals, np.float64).reshape(4, 4).T


def conv_layers(net: str):
    hooks = parse_hooks(ANIME4K / f"Anime4K_Upscale_CNN_x2_{net.upper()}.glsl")
    convs = [p for p in hooks if "-Conv-" in p["desc"] and len(p["binds"]) == 1]
    layers = []
    for p in convs:
        terms = []
        for m in _MAT.finditer(p["body"]):
            terms.append((mat4(m.group(1)), int(m.group(3)), int(float(m.group(4))),
                          int(float(m.group(5)))))
        bias = np.array([float(v) for v in _BIAS.search(p["body"]).group(1).split(",")])
        layers.append((terms, bias))
    last = next((p for p in hooks if len(p["binds"]) == 7), None)
    one = None
    if last:
        terms = {int(m.group(6)): mat4(m.group(1)) for m in _MAT.finditer(last["body"])}
        bias = np.array([float(v) for v in _BIAS.search(last["body"]).group(1).split(",")])
        one = ([terms[k] for k in range(14)], bias)
    return layers, one


def conv3x3(x: np.ndarray, terms, bias) -> np.ndarray:
    """The first layer: a plain 3x3 conv over MAIN (RGBA, alpha weights are 0)."""
    h, w = x.shape[:2]
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    out = np.broadcast_to(bias, (h, w, 4)).astype(np.float64).copy()
    for m, go, dx, dy in terms:
        t = fetch(x, xx + dx, yy + dy)
        out += t @ m.T
    return out


def crelu_conv(x: np.ndarray, terms, bias) -> np.ndarray:
    h, w = x.shape[:2]
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    out = np.broadcast_to(bias, (h, w, 4)).astype(np.float64).copy()
    for m, go, dx, dy in terms:
        t = fetch(x, xx + dx, yy + dy)
        t = np.maximum(t, 0) if go == 0 else np.maximum(-t, 0)
        out += t @ m.T
    return out


def anime4k(src: np.ndarray, net: str, fp16: bool = True, split_acc: bool = True):
    layers, one = conv_layers(net)
    q = (lambda a: a.astype(np.float16).astype(np.float64)) if fp16 else (lambda a: a)
    rgba = np.concatenate([src, np.ones_like(src[..., :1])], -1).astype(np.float64)
    feats = []
    x = q(conv3x3(rgba, *layers[0]))
    feats.append(x)
    for terms, bias in layers[1:]:
        x = q(crelu_conv(x, terms, bias))
        feats.append(x)
    if net == "s":
        return x
    mats, bias = one
    if split_acc:   # what the GPU does: one accumulate pass per layer
        acc = None
        for k, f in enumerate(feats):
            r = np.maximum(f, 0) @ mats[2 * k].T + np.maximum(-f, 0) @ mats[2 * k + 1].T
            acc = q(r + bias if acc is None else r + acc)
        return acc
    r = np.broadcast_to(bias, feats[0].shape).copy()
    for k, f in enumerate(feats):
        r += np.maximum(f, 0) @ mats[2 * k].T + np.maximum(-f, 0) @ mats[2 * k + 1].T
    return r


def depth_to_space(src: np.ndarray, res: np.ndarray, w: int, h: int) -> np.ndarray:
    u, v = out_uv(w, h, 0, h)
    base = bilinear(src, u, v)
    sh, sw = res.shape[:2]
    qx = u * sw * 2 - 0.5
    qy = v * sh * 2 - 0.5
    ix, iy = np.floor(qx).astype(np.int64), np.floor(qy).astype(np.int64)
    fx, fy = qx - ix, qy - iy

    def R(x, y):
        x = np.clip(x, 0, sw * 2 - 1)
        y = np.clip(y, 0, sh * 2 - 1)
        t = res[y >> 1, x >> 1]
        idx = (y & 1) * 2 + (x & 1)
        return np.take_along_axis(t, idx[..., None], -1)[..., 0]

    r = (R(ix, iy) * (1 - fx) + R(ix + 1, iy) * fx) * (1 - fy) + \
        (R(ix, iy + 1) * (1 - fx) + R(ix + 1, iy + 1) * fx) * fy
    return np.clip(base + r[..., None], 0, 1)


# --------------------------------------------------------------------------

def upscale(src: np.ndarray, mode: str, w: int, h: int) -> np.ndarray:
    if mode == "bilinear":
        return bilinear(src, *out_uv(w, h, 0, h))
    if mode == "sharp":
        return rcas(np.round(easu(src, w, h) * 255) / 255)   # E is RGBA8
    return depth_to_space(src, anime4k(src, mode[-1]), w, h)


def sharpness(img: np.ndarray) -> float:
    g = luma(img)
    return float(np.mean(np.abs(np.diff(g, axis=0))) + np.mean(np.abs(np.diff(g, axis=1))))


def selftest() -> int:
    rng = np.random.default_rng(103)
    h, w = 135, 240
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    img = np.zeros((h, w, 3))
    img[..., 0] = (xx // 12 % 2) * 0.8 + 0.1               # hard vertical bars
    img[..., 1] = ((xx + yy) // 9 % 2) * 0.7 + 0.15         # diagonal edges
    img[..., 2] = np.clip(yy / h + rng.normal(0, 0.02, (h, w)), 0, 1)
    img = img.astype(np.float64)
    W, H = w * 2, h * 2

    ok = True
    ref = sharpness(upscale(img, "bilinear", W, H))
    for mode in ("sharp", "ai-s", "ai-m"):
        out = upscale(img, mode, W, H)
        finite = bool(np.isfinite(out).all())
        inrange = bool(out.min() >= 0 and out.max() <= 1)
        s = sharpness(out)
        print(f"  {mode:8s} finite={finite} range=[{out.min():.3f},{out.max():.3f}] "
              f"sharpness={s:.4f} (bilinear {ref:.4f})")
        ok &= finite and inrange and s > ref
    a = anime4k(img, "m", fp16=False, split_acc=True)
    b = anime4k(img, "m", fp16=False, split_acc=False)
    err = float(np.abs(a - b).max())
    print(f"  ai-m split accumulate vs single 1x1 conv: max |diff| = {err:.2e}")
    ok &= err < 1e-9
    print("OK" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", nargs="?", type=Path)
    ap.add_argument("--size", default="3840x2160")
    ap.add_argument("--modes", default="bilinear,sharp,ai-s,ai-m")
    ap.add_argument("--out", type=Path, default=ROOT / "output/upscale_ref")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.image:
        ap.error("an input image is required (or --selftest)")

    from PIL import Image
    src = np.asarray(Image.open(args.image).convert("RGB"), np.float64) / 255.0
    w, h = (int(v) for v in args.size.lower().split("x"))
    args.out.mkdir(parents=True, exist_ok=True)
    for mode in args.modes.split(","):
        out = upscale(src, mode, w, h)
        path = args.out / f"{mode}.png"
        Image.fromarray(np.round(out * 255).astype(np.uint8)).save(path)
        print(f"  {mode:8s} -> {path}  sharpness={sharpness(out):.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
