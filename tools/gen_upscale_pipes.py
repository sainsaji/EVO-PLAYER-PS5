#!/usr/bin/env python3
"""Generate the #103 upscaler .pipe files.

Two upscaler families, all drawn as fullscreen quads by evo_agc_runtime.c's
upscale stage (see agc_upscale_* there):

  Sharp  FSR1 EASU (edge-adaptive spatial upsample) then RCAS (robust
         contrast-adaptive sharpening). A GLSL port of AMD's ffx_fsr1.h
         (MIT), using direct texel fetches instead of gathers. Deterministic
         and cheap: the baseline AI mode is measured against.

  AI     Anime4K's Upscale_CNN_x2 networks (MIT, bloc97), whose weights ship as
         literal mat4 constants inside mpv user-shader hooks. The hooks are
         vendored unchanged in third_party/anime4k/ and translated here:
           S (base PS5)  4 3x3 convs, 4 channels each, run in sequence.
           M (PS5 Pro)   7 3x3 convs, then a 1x1 conv over all seven outputs.
         The 1x1 conv is linear in its inputs, so it is split into one
         accumulate pass per conv layer (acc_k = acc_{k-1} + W_k * crelu(f_k))
         and the runtime never needs all seven feature maps alive at once.
         Both end in the same depth-to-space pass, which adds the network's
         2x luma residual to a bilinear upscale of the source.

Every pipe shares one vertex stage: a quad over the whole viewport whose UV
spans the rectangle `uUv` (origin.xy, size.zw) of the source. The runtime sets
the viewport to the target size, so a pass reads texel ivec2(vUV * size) and
never depends on gl_FragCoord (no shader in this toolchain has been
hardware-verified with it).

The resource mapping is the video pipes' layout, which is hardware-verified:
the VS uniform block at set 0 binding 0, and one fragment descriptor table of
combined textures at set 1. No pass needs fragment-stage constants.

Run inside the amdllpc image, then compile with tools/build_agc_pipes.py:

    docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml run --rm \\
        ps5-dev bash -lc 'python3 tools/gen_upscale_pipes.py && python3 tools/build_agc_pipes.py'
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "projects/evoplayer/shaders/agc"
ANIME4K = ROOT / "third_party/anime4k"

RGBA8 = "VK_FORMAT_R8G8B8A8_UNORM"
RGBA16F = "VK_FORMAT_R16G16B16A16_SFLOAT"

VS = """#version 450

layout(set = 0, binding = 0, std140) uniform PassConstants {
    vec4 uUv;       /* xy = source UV origin, zw = source UV extent */
} pass;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = pass.uUv.xy + vec2(p.x, 1.0 - p.y) * pass.uUv.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""


def fs_header(samplers: int) -> str:
    decl = "\n".join(
        f"layout(set = 1, binding = {i}) uniform sampler2D uIn{i};"
        for i in range(samplers)
    )
    return f"""#version 450

{decl}

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;
"""


# ---------------------------------------------------------------------------
# FSR1 EASU + RCAS (AMD FidelityFX Super Resolution 1.0, MIT).
# ---------------------------------------------------------------------------

EASU_FS = fs_header(1) + """
ivec2 g_sz;

vec3 S(ivec2 p) {
    return texelFetch(uIn0, clamp(p, ivec2(0), g_sz - 1), 0).rgb;
}

/* FSR's cheap luma: B/2 + R/2 + G. */
float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

/* Direction and length contribution of one of the four bilinear corners.
 * a = up, b = left, c = centre, d = right, e = down. */
void easu_set(inout vec2 dir, inout float len, float w,
              float a, float b, float c, float d, float e) {
    float dc = d - c;
    float cb = c - b;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1.0 / 65536.0);
    float dirX = d - b;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    len += lenX * lenX * w;

    float ec = e - c;
    float ca = c - a;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1.0 / 65536.0);
    float dirY = e - a;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}

/* Lanczos-2 approximation, stretched along the edge direction. */
void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
              float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y,
                  off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main() {
    g_sz = textureSize(uIn0, 0);
    vec2 pp = vUV * vec2(g_sz) - vec2(0.5);
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 o = ivec2(fp);

    /*      b c
     *    e f g h
     *    i j k l
     *      n o      f = o */
    vec3 bC = S(o + ivec2(0, -1)), cC = S(o + ivec2(1, -1));
    vec3 eC = S(o + ivec2(-1, 0)), fC = S(o),                gC = S(o + ivec2(1, 0)), hC = S(o + ivec2(2, 0));
    vec3 iC = S(o + ivec2(-1, 1)), jC = S(o + ivec2(0, 1)), kC = S(o + ivec2(1, 1)), lC = S(o + ivec2(2, 1));
    vec3 nC = S(o + ivec2(0, 2)),  oC = S(o + ivec2(1, 2));

    float bL = L(bC), cL = L(cC), eL = L(eC), fL = L(fC), gL = L(gC), hL = L(hC);
    float iL = L(iC), jL = L(jC), kL = L(kC), lL = L(lC), nL = L(nC), oL = L(oC);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);

    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    easu_tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    easu_tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    easu_tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    easu_tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    easu_tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    easu_tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    easu_tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    easu_tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    easu_tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    easu_tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    easu_tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);

    /* De-ring: clamp to the 2x2 neighbourhood. */
    vec3 mn = min(min(fC, gC), min(jC, kC));
    vec3 mx = max(max(fC, gC), max(jC, kC));
    vec3 pix = clamp(aC / max(aW, 1.0 / 65536.0), mn, mx);
    out_color = vec4(pix, 1.0);
}
"""

# RCAS sharpness in stops (FSR default 0.2): con = exp2(-0.2).
RCAS_SHARPNESS = 0.8705506
RCAS_FS = fs_header(1) + f"""
ivec2 g_sz;

vec3 S(ivec2 p) {{
    return texelFetch(uIn0, clamp(p, ivec2(0), g_sz - 1), 0).rgb;
}}

float L(vec3 c) {{ return c.b * 0.5 + (c.r * 0.5 + c.g); }}

void main() {{
    g_sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(g_sz)), ivec2(0), g_sz - 1);

    /*   b
     * d e f
     *   h   */
    vec3 b = S(ip + ivec2(0, -1));
    vec3 d = S(ip + ivec2(-1, 0));
    vec3 e = S(ip);
    vec3 f = S(ip + ivec2(1, 0));
    vec3 h = S(ip + ivec2(0, 1));

    float bL = L(b), dL = L(d), eL = L(e), fL = L(f), hL = L(h);

    /* Noise detection: back off on isolated single-pixel detail. */
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float lmax = max(max(max(bL, dL), max(eL, fL)), hL);
    float lmin = min(min(min(bL, dL), min(eL, fL)), hL);
    nz = clamp(abs(nz) / max(lmax - lmin, 1.0 / 65536.0), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0),
                     min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * {RCAS_SHARPNESS};
    lobe *= nz;

    vec3 pix = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    out_color = vec4(clamp(pix, 0.0, 1.0), 1.0);
}}
"""

# ---------------------------------------------------------------------------
# Anime4K depth-to-space, shared by S and M.
#   uIn0 = source RGB (bilinear sampler), uIn1 = conv_last (4 subpixel luma
#   residuals per source texel, index = y * 2 + x in the 2x grid).
# mpv upsamples the x2 result to the output with its own scaler; here the
# residual is bilinearly resampled from the virtual 2x grid instead, and added
# to a bilinear upsample of the source (Anime4K adds it to MAIN_tex at 2x).
# ---------------------------------------------------------------------------

FINAL_FS = fs_header(2) + """
ivec2 g_sz;

float R(ivec2 v) {
    v = clamp(v, ivec2(0), g_sz * 2 - 1);
    vec4 t = texelFetch(uIn1, v >> 1, 0);
    return t[(v.y & 1) * 2 + (v.x & 1)];
}

void main() {
    g_sz = textureSize(uIn1, 0);
    vec3 base = texture(uIn0, vUV).rgb;

    vec2 q = vUV * vec2(g_sz * 2) - vec2(0.5);
    vec2 f = fract(q);
    ivec2 i0 = ivec2(floor(q));
    float r = mix(mix(R(i0),               R(i0 + ivec2(1, 0)), f.x),
                  mix(R(i0 + ivec2(0, 1)), R(i0 + ivec2(1, 1)), f.x), f.y);
    out_color = vec4(clamp(base + vec3(r), 0.0, 1.0), 1.0);
}
"""


# ---------------------------------------------------------------------------
# mpv hook parsing
# ---------------------------------------------------------------------------

def parse_hooks(path: Path) -> list[dict]:
    """Split an mpv user shader into passes: directives + GLSL body."""
    text = path.read_text(encoding="utf-8")
    passes = []
    for chunk in text.split("//!DESC ")[1:]:
        lines = chunk.splitlines()
        desc = lines[0].strip()
        binds, save, body = [], None, []
        for line in lines[1:]:
            if line.startswith("//!BIND "):
                binds.append(line.split(None, 1)[1].strip())
            elif line.startswith("//!SAVE "):
                save = line.split(None, 1)[1].strip()
            elif line.startswith("//!"):
                continue
            else:
                body.append(line)
        passes.append({"desc": desc, "binds": binds, "save": save,
                       "body": "\n".join(body).strip()})
    return passes


def conv_pass_fs(p: dict) -> str:
    """A 3x3 conv hook -> fragment shader over texelFetch.

    `NAME_texOff(vec2(x, y))` is mpv's clamped fetch at a texel offset; y grows
    downward in both mpv and the layer memory, so offsets carry over as-is.
    """
    n = len(p["binds"])
    body = p["body"]
    for i, src in enumerate(p["binds"]):
        body = body.replace(f"{src}_texOff(vec2(x_off, y_off))", f"T{i}(x_off, y_off)")
    if "_texOff" in body or "_tex(" in body:
        raise SystemExit(f"untranslated texture access in {p['desc']}")
    fetch = "\n".join(
        f"#define T{i}(x, y) texelFetch(uIn{i}, "
        f"clamp(g_ip + ivec2(int(x), int(y)), ivec2(0), g_sz - 1), 0)"
        for i in range(n))
    return fs_header(n) + f"""
/* {p['desc']} */
ivec2 g_ip;
ivec2 g_sz;
{fetch}
{body}

void main() {{
    g_sz = textureSize(uIn0, 0);
    g_ip = clamp(ivec2(vUV * vec2(g_sz)), ivec2(0), g_sz - 1);
    out_color = hook();
}}
"""


_MAT_TERM = re.compile(r"(mat4\([^)]*\))\s*\*\s*g_(\d+)\s*;")
_BIAS = re.compile(r"result \+= (vec4\([^)]*\))\s*;")


def acc_pass_fs(last: dict, layer: int) -> str:
    """One layer's share of M's 1x1 conv over all seven feature maps.

    uIn0 = conv layer `layer`; uIn1 = the running sum (absent for layer 0,
    which carries the bias instead). g_{2k} / g_{2k+1} are the CReLU halves of
    feature map k, so each layer contributes two mat4 terms.
    """
    terms = {int(i): m for m, i in _MAT_TERM.findall(last["body"])}
    bias = _BIAS.search(last["body"])
    if len(terms) != 14 or bias is None:
        raise SystemExit(f"unexpected 1x1 conv layout in {last['desc']}")
    pos, neg = terms[2 * layer], terms[2 * layer + 1]
    first = layer == 0
    tail = f"r += {bias.group(1)};" if first else "r += texelFetch(uIn1, ip, 0);"
    return fs_header(1 if first else 2) + f"""
/* {last['desc']} - layer {layer} of 7 */
void main() {{
    ivec2 sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(sz)), ivec2(0), sz - 1);
    vec4 f = texelFetch(uIn0, ip, 0);
    vec4 r = {pos} * max(f, 0.0);
    r += {neg} * max(-f, 0.0);
    {tail}
    out_color = r;
}}
"""


_G_DEF = re.compile(r"#define g_(\d+) \(max\((-?)\((\w+)_tex\(\w+_pos\)\), 0\.0\)\)")


def wide_acc_pass_fs(last: dict, layer_binds: list[str], first: bool, what: str) -> str:
    """One layer's share of one output of a wide network's 1x1 conv.

    VL/UL end in three 1x1 convs (one per residual colour) over the CReLU of
    several layers' feature maps, each layer being `len(layer_binds)` RGBA
    textures. The conv is linear, so it runs as one accumulate pass per layer:
    uIn0..uIn{n-1} = that layer's textures, uIn{n} = the running sum (absent on
    the first layer, which carries the bias instead).
    """
    defs = {int(i): (sign == "-", name) for i, sign, name in _G_DEF.findall(last["body"])}
    terms = {int(i): m for m, i in _MAT_TERM.findall(last["body"])}
    bias = _BIAS.search(last["body"])
    if not defs or len(terms) != len(defs) or bias is None:
        raise SystemExit(f"unexpected 1x1 conv layout in {last['desc']}")
    n = len(layer_binds)
    lines = []
    for g, (neg, name) in sorted(defs.items()):
        if name not in layer_binds:
            continue
        t = f"t{layer_binds.index(name)}"
        act = f"max(-{t}, 0.0)" if neg else f"max({t}, 0.0)"
        lines.append(f"    r += {terms[g]} * {act};")
    if len(lines) != 2 * n:
        raise SystemExit(f"{last['desc']}: expected {2 * n} terms for {layer_binds}")
    loads = "\n".join(f"    vec4 t{i} = texelFetch(uIn{i}, ip, 0);" for i in range(n))
    tail = f"r += {bias.group(1)};" if first else f"r += texelFetch(uIn{n}, ip, 0);"
    return fs_header(n if first else n + 1) + f"""
/* {what} */
void main() {{
    ivec2 sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(sz)), ivec2(0), sz - 1);
{loads}
    vec4 r = vec4(0.0);
{chr(10).join(lines)}
    {tail}
    out_color = r;
}}
"""


# VL/UL depth-to-space: uIn0 = source RGB, uIn1..3 = residual for R, G, B,
# each holding the 4 subpixels of the 2x grid. Same resampling as FINAL_FS.
RGB_FINAL_FS = fs_header(4) + """
ivec2 g_sz;

vec3 R(ivec2 v) {
    v = clamp(v, ivec2(0), g_sz * 2 - 1);
    ivec2 s = v >> 1;
    int k = (v.y & 1) * 2 + (v.x & 1);
    return vec3(texelFetch(uIn1, s, 0)[k], texelFetch(uIn2, s, 0)[k],
                texelFetch(uIn3, s, 0)[k]);
}

void main() {
    g_sz = textureSize(uIn1, 0);
    vec3 base = texture(uIn0, vUV).rgb;

    vec2 q = vUV * vec2(g_sz * 2) - vec2(0.5);
    vec2 f = fract(q);
    ivec2 i0 = ivec2(floor(q));
    vec3 r = mix(mix(R(i0),               R(i0 + ivec2(1, 0)), f.x),
                 mix(R(i0 + ivec2(0, 1)), R(i0 + ivec2(1, 1)), f.x), f.y);
    out_color = vec4(clamp(base + r, 0.0, 1.0), 1.0);
}
"""

# Wide networks, and the runtime pipe range each one's passes occupy. Emitted
# as a C table (upscale_wide_pipes.inc) the runtime includes, so adding a
# network here does not mean hand-writing 36 table rows there.
WIDE_NETS = (("ul", "UL", "EVO_AGC_PIPE_UP_UL"),)


def gen_wide(net: str, upper: str, enum: str, inc: list[str]) -> None:
    hooks = parse_hooks(ANIME4K / f"Anime4K_Upscale_CNN_x2_{upper}.glsl")
    convs = [p for p in hooks if "-Conv-4x3x3x" in p["desc"]]
    lasts = [p for p in hooks if "-Conv-4x1x1x" in p["desc"]]
    width = sum(1 for p in convs if p["binds"] == ["MAIN"])
    if width < 2 or len(convs) % width or len(lasts) != 3:
        raise SystemExit(f"Anime4K {upper}: unexpected layout")
    layers = len(convs) // width
    for i, p in enumerate(convs):
        name = f"upscale_a4k_{net}_conv{i}"
        write_pipe(name, conv_pass_fs(p), len(p["binds"]), RGBA16F, p["desc"])
        inc.append(f"{{{enum}_CONV0 + {i}, &{name}_metadata, \"{name}\"}},")
    # Layers feeding the 1x1 conv, in order; each is `width` textures.
    fed = []
    for b in lasts[0]["binds"]:
        layer = int(b.split("_")[1]) if b.split("_")[1].isdigit() else 0
        if layer not in fed:
            fed.append(layer)
    k = 0
    for li, layer in enumerate(fed):
        tex = [b for b in lasts[0]["binds"]
               if (int(b.split("_")[1]) if b.split("_")[1].isdigit() else 0) == layer]
        for j, last in enumerate(lasts):
            name = f"upscale_a4k_{net}_acc{k}"
            write_pipe(name, wide_acc_pass_fs(last, tex, li == 0,
                                              f"{last['desc']} out {j} layer {layer}"),
                       width if li == 0 else width + 1, RGBA16F,
                       f"{last['desc']} out {j} layer {layer}")
            inc.append(f"{{{enum}_ACC0 + {k}, &{name}_metadata, \"{name}\"}},")
            k += 1
    print(f"  {upper}: {layers} layers x {width} textures, 1x1 conv over layers {fed}")


# ---------------------------------------------------------------------------
# .pipe emission
# ---------------------------------------------------------------------------

def resource_mapping(samplers: int) -> str:
    lines = [
        "userDataNode[0].visibility = 2",
        "userDataNode[0].type = DescriptorTableVaPtr",
        "userDataNode[0].offsetInDwords = 0",
        "userDataNode[0].sizeInDwords = 1",
        "userDataNode[0].next[0].type = DescriptorConstBuffer",
        "userDataNode[0].next[0].offsetInDwords = 0",
        "userDataNode[0].next[0].sizeInDwords = 4",
        "userDataNode[0].next[0].set = 0",
        "userDataNode[0].next[0].binding = 0",
        "userDataNode[1].visibility = 64",
        "userDataNode[1].type = DescriptorTableVaPtr",
        "userDataNode[1].offsetInDwords = 0",
        "userDataNode[1].sizeInDwords = 1",
    ]
    for i in range(samplers):
        lines += [
            f"userDataNode[1].next[{i}].type = DescriptorCombinedTexture",
            f"userDataNode[1].next[{i}].offsetInDwords = {i * 12}",
            f"userDataNode[1].next[{i}].sizeInDwords = 12",
            f"userDataNode[1].next[{i}].set = 1",
            f"userDataNode[1].next[{i}].binding = {i}",
        ]
    return "\n".join(lines)


def write_pipe(name: str, fs: str, samplers: int, fmt: str, comment: str) -> None:
    text = f"""; {name} - {comment}
; Generated by tools/gen_upscale_pipes.py. Edit the generator, not this file.

[Version]
version = 65

[VsGlsl]
{VS}
[VsInfo]
entryPoint = main

[FsGlsl]
{fs}
[FsInfo]
entryPoint = main

[ResourceMapping]
{resource_mapping(samplers)}

[GraphicsPipelineState]
topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
nggState.enableNgg = 1
nggState.enableGsUse = 0
colorBuffer[0].format = {fmt}
colorBuffer[0].channelWriteMask = 15
colorBuffer[0].blendEnable = 0
"""
    path = SHADER_DIR / f"{name}.pipe"
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"  wrote {path.name} ({samplers} sampler(s), {fmt.split('_', 2)[2]})")


def main() -> int:
    print("Generating upscaler .pipe files...")
    write_pipe("upscale_easu", EASU_FS, 1, RGBA8, "FSR1 EASU, source RGB -> output size")
    write_pipe("upscale_rcas", RCAS_FS, 1, RGBA8, "FSR1 RCAS sharpen -> scanout")
    write_pipe("upscale_a4k_final", FINAL_FS, 2, RGBA8,
               "Anime4K depth-to-space + bilinear base -> scanout")

    for net, count in (("s", 4), ("m", 7)):
        hooks = parse_hooks(ANIME4K / f"Anime4K_Upscale_CNN_x2_{net.upper()}.glsl")
        convs = [p for p in hooks if "-Conv-" in p["desc"] and len(p["binds"]) == 1]
        if len(convs) != count:
            raise SystemExit(f"Anime4K {net.upper()}: expected {count} 3x3 convs, "
                             f"found {len(convs)}")
        for i, p in enumerate(convs):
            write_pipe(f"upscale_a4k_{net}_conv{i}", conv_pass_fs(p), 1, RGBA16F, p["desc"])
        if net == "m":
            last = next(p for p in hooks if len(p["binds"]) == 7)
            for k in range(7):
                write_pipe(f"upscale_a4k_m_acc{k}", acc_pass_fs(last, k),
                           1 if k == 0 else 2, RGBA16F, f"{last['desc']} layer {k}")

    write_pipe("upscale_a4k_rgb_final", RGB_FINAL_FS, 4, RGBA8,
               "Anime4K VL/UL depth-to-space (RGB residual) + bilinear base -> scanout")
    inc = ["/* Generated by tools/gen_upscale_pipes.py - DO NOT EDIT. Rows of the",
           " * runtime's pipeline table for the wide Anime4K networks. */"]
    for net, upper, enum in WIDE_NETS:
        gen_wide(net, upper, enum, inc)
    (SHADER_DIR / "upscale_wide_pipes.inc").write_text("\n".join(inc) + "\n",
                                                      encoding="utf-8", newline="\n")
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
