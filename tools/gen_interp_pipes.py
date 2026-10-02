#!/usr/bin/env python3
"""Generate the #105 frame interpolation (motion smoothing) .pipe files.

Four pipelines, run per source frame pair except the last, which runs per
presented vsync:

  interp_pyr     Quarter-resolution luma pyramid for BOTH frames in one pass:
                 R = luma A, G = luma B, B/A = local activity of each. Every
                 texel is the mean of four 2x2 bilinear taps, so the coarse
                 search below matches prefiltered luma instead of aliased
                 point samples.
  interp_me      Motion estimation, one 16x16 source block per fragment.
                 Coarse logarithmic search on the pyramid (steps 8/4/2/1 of a
                 quarter-res texel = +-60 source px), then full-resolution
                 refinement down to quarter-pel. Outputs
                 (mv.x, mv.y, residual, confidence).
  interp_median  Confidence-weighted 3x3 VECTOR median (one real candidate
                 wins, so the vector and its residual stay paired) plus a 5x5
                 mean residual, which is what tells the warp a whole region
                 failed rather than one block.
  interp_warp    Frame synthesis. Edge-aware MV upsample, then warp distance
                 scaled by confidence so a half-trusted vector gives one
                 partly-displaced image rather than two superimposed ones.

Run inside the amdllpc image:
    docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml run --rm \
        ps5-dev bash -lc 'python3 tools/gen_interp_pipes.py && python3 tools/build_agc_pipes.py'
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "projects/evoplayer/shaders/agc"

RGBA8 = "VK_FORMAT_R8G8B8A8_UNORM"
RGBA16F = "VK_FORMAT_R16G16B16A16_SFLOAT"

VS = """#version 450

layout(set = 0, binding = 0, std140) uniform PassConstants {
    vec4 uUv;       /* xy = source UV origin, zw = source UV extent */
    vec4 uParams;   /* x = phase t [0..1], y = mode (1=Low, 2=High), z, w = extra */
} pass;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vParams;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = pass.uUv.xy + vec2(p.x, 1.0 - p.y) * pass.uUv.zw;
    vParams = pass.uParams;
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
layout(location = 1) in vec4 vParams;
layout(location = 0) out vec4 out_color;
"""

# ---------------------------------------------------------------------------
# interp_pyr: quarter-resolution prefiltered luma for both frames
# uIn0 = Frame A, uIn1 = Frame B (both bilinear)
# Target size: (src_w + 3)/4 x (src_h + 3)/4, RGBA16F
# ---------------------------------------------------------------------------
INTERP_PYR_FS = fs_header(2) + """
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    ivec2 src_sz = textureSize(uIn0, 0);
    vec2 inv = 1.0 / vec2(src_sz);
    ivec2 dst_sz = ivec2((src_sz.x + 3) / 4, (src_sz.y + 3) / 4);

    /* Source texels 4*ip .. 4*ip+3 on each axis. A bilinear tap on the
     * boundary between two texel centres averages them, so four taps at
     * texel coordinates 4*ip + {1,3} give four 2x2 box averages of the
     * 4x4 footprint for four fetches rather than sixteen. */
    vec2 base = floor(vUV * vec2(dst_sz)) * 4.0;

    float qa[4];
    float qb[4];
    int i = 0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 uv = (base + vec2(float(x) * 2.0 + 1.0, float(y) * 2.0 + 1.0)) * inv;
            uv = clamp(uv, vec2(0.0), vec2(1.0));
            qa[i] = luma(texture(uIn0, uv).rgb);
            qb[i] = luma(texture(uIn1, uv).rgb);
            i++;
        }
    }

    float ma = (qa[0] + qa[1] + qa[2] + qa[3]) * 0.25;
    float mb = (qb[0] + qb[1] + qb[2] + qb[3]) * 0.25;
    /* Mean absolute deviation of the four quads: local detail at a 2 px
     * scale. The ME pass needs it to tell "matched well" from "had nothing
     * to match". */
    float va = (abs(qa[0] - ma) + abs(qa[1] - ma) + abs(qa[2] - ma) + abs(qa[3] - ma)) * 0.25;
    float vb = (abs(qb[0] - mb) + abs(qb[1] - mb) + abs(qb[2] - mb) + abs(qb[3] - mb)) * 0.25;

    out_color = vec4(ma, mb, va, vb);
}
"""

# ---------------------------------------------------------------------------
# interp_me: motion estimation, one 16x16 source block per fragment
# uIn0 = Frame A, uIn1 = Frame B (full res, bilinear), uIn2 = interp_pyr
# Target size: (src_w + 15)/16 x (src_h + 15)/16, RGBA16F
# uParams.y = mode (1 = Low: coarse grid only, 2 = High: full refinement)
# ---------------------------------------------------------------------------
INTERP_ME_FS = fs_header(3) + """
#define TAPS 6                  /* TAPS x TAPS samples per block */
#define NTAP (TAPS * TAPS)
#define HALF (float(TAPS) * 0.5 - 0.5)

/* Ties are the whole game. SAD is flat across every candidate inside a sky or
 * a letterbox bar, so without a penalty the first candidate tested wins and a
 * whole region picks up a large bogus vector. These bias the search towards
 * the smallest displacement that explains the block. */
#define LAM_COARSE 0.010        /* per quarter-res texel of |d| */
#define LAM_FINE   0.020        /* per source px away from the coarse seed */

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

/* Coarse stage state: pyramid space, units of one quarter-res texel. */
vec2  g_pinv;
vec2  g_pctr;
float g_pa[NTAP];

/* Fine stage state: source space, units of one source pixel. */
vec2  g_inv;
vec2  g_fctr;
float g_fa[NTAP];

float coarse_cost(vec2 d) {
    float s = 0.0;
    for (int y = 0; y < TAPS; ++y) {
        for (int x = 0; x < TAPS; ++x) {
            vec2 pt = g_pctr + vec2(float(x) - HALF, float(y) - HALF) + d;
            s += abs(g_pa[y * TAPS + x] - texture(uIn2, clamp(pt * g_pinv, vec2(0.0), vec2(1.0))).g);
        }
    }
    return s;
}

float fine_cost(vec2 d) {
    float s = 0.0;
    for (int y = 0; y < TAPS; ++y) {
        for (int x = 0; x < TAPS; ++x) {
            vec2 pt = g_fctr + vec2(float(x) - HALF, float(y) - HALF) * 3.0 + d;
            s += abs(g_fa[y * TAPS + x] - luma(texture(uIn1, clamp(pt * g_inv, vec2(0.0), vec2(1.0))).rgb));
        }
    }
    return s;
}

void main() {
    int mode = int(vParams.y + 0.5);

    ivec2 src_sz = textureSize(uIn0, 0);
    ivec2 pyr_sz = textureSize(uIn2, 0);
    g_inv  = 1.0 / vec2(src_sz);
    g_pinv = 1.0 / vec2(pyr_sz);

    ivec2 num_blocks = ivec2((src_sz.x + 15) / 16, (src_sz.y + 15) / 16);
    vec2 block_idx = floor(vUV * vec2(num_blocks));
    /* Derive the centre from the same UV->block mapping the warp uses to look
     * the vector up again. Height is rarely a multiple of 16 (1080 is not), so
     * (idx + 0.5) * 16 drifts from the block the warp thinks this is - several
     * pixels of it by the bottom row. */
    g_fctr = (block_idx + vec2(0.5)) * vec2(src_sz) / vec2(num_blocks);
    g_pctr = g_fctr * 0.25;                    /* same point, pyramid texels */

    /* ---- coarse: logarithmic search on the prefiltered quarter-res luma.
     * 6x6 taps one texel apart = a 24x24 source window, wider than the block
     * so the match is stable; steps 8/4/2/1 reach +-15 texels = +-60 px. */
    for (int y = 0; y < TAPS; ++y)
        for (int x = 0; x < TAPS; ++x) {
            vec2 pt = g_pctr + vec2(float(x) - HALF, float(y) - HALF);
            g_pa[y * TAPS + x] = texture(uIn2, clamp(pt * g_pinv, vec2(0.0), vec2(1.0))).r;
        }

    vec2  cbest = vec2(0.0);
    float ccost = coarse_cost(vec2(0.0));
    for (int it = 0; it < 4; ++it) {
        float st = float(8 >> it);
        vec2 centre = cbest;
        for (int k = 0; k < 9; ++k) {
            if (k == 4) continue;
            vec2 d = centre + vec2(float(k % 3) - 1.0, float(k / 3) - 1.0) * st;
            float c = coarse_cost(d) + LAM_COARSE * (abs(d.x) + abs(d.y));
            if (c < ccost) { ccost = c; cbest = d; }
        }
    }

    /* ---- fine: full resolution, 6x6 taps three px apart = an 18x18 window.
     * Steps 2/1/0.5/0.25 px off the coarse seed. Quarter-pel matters: a
     * vector rounded to 4 px (what this used to do) doubles every edge it
     * moves. */
    for (int y = 0; y < TAPS; ++y)
        for (int x = 0; x < TAPS; ++x) {
            vec2 pt = g_fctr + vec2(float(x) - HALF, float(y) - HALF) * 3.0;
            g_fa[y * TAPS + x] = luma(texture(uIn0, clamp(pt * g_inv, vec2(0.0), vec2(1.0))).rgb);
        }

    vec2  seed = cbest * 4.0;
    vec2  fbest = seed;
    float fraw  = fine_cost(seed);
    float fcost = fraw;

    /* Zero always competes unpenalised: standing still is the safe answer. */
    float zraw = fine_cost(vec2(0.0));
    if (zraw <= fcost) { fcost = zraw; fraw = zraw; fbest = vec2(0.0); }

    /* High refines to quarter-pel; Low stops at whole pixels. That is where
     * most of the fine-stage cost is, and it is still nothing like the 4 px
     * rounding this used to ship. */
    int iters = (mode >= 2) ? 4 : 2;
    {
        for (int it = 0; it < 4; ++it) {
            if (it >= iters) break;
            float st = 2.0 / float(1 << it);
            vec2 centre = fbest;
            for (int k = 0; k < 9; ++k) {
                if (k == 4) continue;
                vec2 d = centre + vec2(float(k % 3) - 1.0, float(k / 3) - 1.0) * st;
                float raw = fine_cost(d);
                float c = raw + LAM_FINE * (abs(d.x - seed.x) + abs(d.y - seed.y));
                if (c < fcost) { fcost = c; fraw = raw; fbest = d; }
            }
        }
    }

    /* ---- confidence. Two independent questions, both needed:
     * does the residual look small for the amount of detail present, and does
     * the vector explain anything that standing still does not? */
    float amean = 0.0;
    for (int i = 0; i < NTAP; ++i) amean += g_fa[i];
    amean *= 1.0 / float(NTAP);
    float act = 0.0;
    for (int i = 0; i < NTAP; ++i) act += abs(g_fa[i] - amean);
    act *= 1.0 / float(NTAP);

    float res  = fraw * (1.0 / float(NTAP));
    float zres = zraw * (1.0 / float(NTAP));

    /* Nothing to track here. Any vector is a guess, and a guess drags
     * neighbouring detail across the flat area, so say so. */
    if (act < 0.012) {
        fbest = vec2(0.0);
        res = zres;
    }

    float quality = smoothstep(0.95, 0.28, res / (act + 0.015));
    float gain = clamp((zres - res) / max(zres, 0.004), 0.0, 1.0);
    float conf = quality * mix(0.30, 1.0, smoothstep(0.03, 0.25, gain));

    out_color = vec4(fbest.x, fbest.y, res, conf);
}
"""

# ---------------------------------------------------------------------------
# interp_median: vector median + area residual
# uIn0 = raw MV field (RGBA16F)
# ---------------------------------------------------------------------------
INTERP_MEDIAN_FS = fs_header(1) + """
/* Every tap is a named variable and every sum is written out. An array indexed
 * by a loop counter made amdllpc spill this shader to SCRATCH, and the AGC
 * runtime sets up no scratch ring - the GPU then writes private memory to an
 * unmapped address and the console kills the process with
 * GPU_FAULT_PAGE_FAULT_ASYNC and no log line of its own. */
#define TAP(dx, dy) texelFetch(uIn0, clamp(ip + ivec2(dx, dy), ivec2(0), sz - 1), 0)
/* Confidence-weighted L1 distance from a to b. */
#define D(a, b) ((b.w + 0.05) * (abs(a.x - b.x) + abs(a.y - b.y)))

void main() {
    ivec2 sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(sz)), ivec2(0), sz - 1);

    vec4 v0 = TAP(-1, -1);
    vec4 v1 = TAP(0, -1);
    vec4 v2 = TAP(1, -1);
    vec4 v3 = TAP(-1, 0);
    vec4 v4 = TAP(0, 0);
    vec4 v5 = TAP(1, 0);
    vec4 v6 = TAP(-1, 1);
    vec4 v7 = TAP(0, 1);
    vec4 v8 = TAP(1, 1);

    /* Confidence-weighted VECTOR median: the winner is one of the nine real
     * candidates, so its vector and its residual still belong together. A
     * per-component median invents a vector nobody measured and pairs it with
     * the centre block's residual. */
    float c0 = D(v0, v0) + D(v0, v1) + D(v0, v2) + D(v0, v3) + D(v0, v4) + D(v0, v5) + D(v0, v6) + D(v0, v7) + D(v0, v8);
    float c1 = D(v1, v0) + D(v1, v1) + D(v1, v2) + D(v1, v3) + D(v1, v4) + D(v1, v5) + D(v1, v6) + D(v1, v7) + D(v1, v8);
    float c2 = D(v2, v0) + D(v2, v1) + D(v2, v2) + D(v2, v3) + D(v2, v4) + D(v2, v5) + D(v2, v6) + D(v2, v7) + D(v2, v8);
    float c3 = D(v3, v0) + D(v3, v1) + D(v3, v2) + D(v3, v3) + D(v3, v4) + D(v3, v5) + D(v3, v6) + D(v3, v7) + D(v3, v8);
    float c4 = D(v4, v0) + D(v4, v1) + D(v4, v2) + D(v4, v3) + D(v4, v4) + D(v4, v5) + D(v4, v6) + D(v4, v7) + D(v4, v8);
    float c5 = D(v5, v0) + D(v5, v1) + D(v5, v2) + D(v5, v3) + D(v5, v4) + D(v5, v5) + D(v5, v6) + D(v5, v7) + D(v5, v8);
    float c6 = D(v6, v0) + D(v6, v1) + D(v6, v2) + D(v6, v3) + D(v6, v4) + D(v6, v5) + D(v6, v6) + D(v6, v7) + D(v6, v8);
    float c7 = D(v7, v0) + D(v7, v1) + D(v7, v2) + D(v7, v3) + D(v7, v4) + D(v7, v5) + D(v7, v6) + D(v7, v7) + D(v7, v8);
    float c8 = D(v8, v0) + D(v8, v1) + D(v8, v2) + D(v8, v3) + D(v8, v4) + D(v8, v5) + D(v8, v6) + D(v8, v7) + D(v8, v8);

    vec4 bv = v0;
    float bc = c0;
    if (c1 < bc) { bc = c1; bv = v1; }
    if (c2 < bc) { bc = c2; bv = v2; }
    if (c3 < bc) { bc = c3; bv = v3; }
    if (c4 < bc) { bc = c4; bv = v4; }
    if (c5 < bc) { bc = c5; bv = v5; }
    if (c6 < bc) { bc = c6; bv = v6; }
    if (c7 < bc) { bc = c7; bv = v7; }
    if (c8 < bc) { bc = c8; bv = v8; }

    /* Mean residual over 5x5. One block failing is an occlusion; a whole
     * region failing is a cut or motion the search cannot reach, and the two
     * want opposite fallbacks. The old per-pixel test switched frame inside
     * the picture and left a patchwork. */
    float area = (v0.z + v1.z + v2.z + v3.z + v4.z + v5.z + v6.z + v7.z + v8.z +
                  TAP(-2, -2).z +
                  TAP(-1, -2).z +
                  TAP(0, -2).z +
                  TAP(1, -2).z +
                  TAP(2, -2).z +
                  TAP(-2, -1).z +
                  TAP(2, -1).z +
                  TAP(-2, 0).z +
                  TAP(2, 0).z +
                  TAP(-2, 1).z +
                  TAP(2, 1).z +
                  TAP(-2, 2).z +
                  TAP(-1, 2).z +
                  TAP(0, 2).z +
                  TAP(1, 2).z +
                  TAP(2, 2).z) * (1.0 / 25.0);

    /* The mean over the 3x3, not the winner's own: confidence gates how much
     * of the warp is used, and a hard block-to-block step in that gate shows
     * up as a chequer of warped and unwarped squares. The vector stays the
     * median, so motion boundaries keep their edge. */
    float conf = (v0.w + v1.w + v2.w + v3.w + v4.w + v5.w + v6.w + v7.w + v8.w) * (1.0 / 9.0);
    out_color = vec4(bv.x, bv.y, area, conf);
}
"""

# ---------------------------------------------------------------------------
# interp_warp: frame synthesis, once per presented vsync
# uIn0 = Frame A, uIn1 = Frame B (bilinear), uIn2 = filtered MV field
# uParams: x = phase t [0..1], y = mode (0 = passthrough, 1 = Low, 2 = High)
# ---------------------------------------------------------------------------
INTERP_WARP_FS = fs_header(3) + """
float gray(vec3 c) { return dot(c, vec3(1.0 / 3.0)); }

void main() {
    float t = clamp(vParams.x, 0.0, 1.0);
    /* uParams.y carries BOTH the mode and the debug view, as mode + 10*view.
     * The two live in one channel because .x and .y are the only components of
     * this varying any EVO shader has ever read, and a debug switch that needs
     * its own debugging is no use.
     * /mnt/usb0/evo_interp_debug: 1 = vectors, 2 = confidence, 3 = warp weight.
     * Thresholds like the ones below cannot be tuned by looking at a correct
     * picture, so this shows what the warp is actually being told. */
    int packed = int(vParams.y + 0.5);
    int mode = packed % 10;
    float dbg = float(packed / 10);

    if (mode <= 0) { out_color = texture(uIn0, vUV); return; }
    if (dbg < 0.5) {
        if (t <= 0.002) { out_color = texture(uIn0, vUV); return; }
        if (t >= 0.998) { out_color = texture(uIn1, vUV); return; }
    }

    ivec2 src_sz = textureSize(uIn0, 0);
    vec2 inv = 1.0 / vec2(src_sz);
    ivec2 mv_sz = textureSize(uIn2, 0);

    /* Edge-aware MV upsample. Plain bilinear ramps between the vector of a
     * moving object and the vector of what it moves across, and that ramp is
     * what a halo is. Weight the four taps by bilinear weight AND by
     * confidence AND by agreement with the strongest of them, so a boundary
     * stays a boundary. */
    vec2 mvc = vUV * vec2(mv_sz) - vec2(0.5);
    ivec2 i0 = ivec2(floor(mvc));
    vec2 f = mvc - vec2(i0);
    vec4 s[4];
    float bw[4];
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        s[k] = texelFetch(uIn2, clamp(i0 + o, ivec2(0), mv_sz - 1), 0);
        vec2 w2 = mix(vec2(1.0) - f, f, vec2(o));
        bw[k] = w2.x * w2.y;
    }
    int ref = 0;
    float ref_w = -1.0;
    for (int k = 0; k < 4; ++k) {
        float m = bw[k] * (s[k].w + 0.02);
        if (m > ref_w) { ref_w = m; ref = k; }
    }
    vec2 mv = vec2(0.0);
    float conf = 0.0, area = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        float sim = exp2(-0.35 * (abs(s[k].x - s[ref].x) + abs(s[k].y - s[ref].y)));
        float w = bw[k] * (s[k].w + 0.05) * sim;
        mv += w * s[k].xy;
        conf += w * s[k].w;
        area += w * s[k].z;
        wsum += w;
    }
    wsum = max(wsum, 1e-5);
    mv /= wsum;
    conf /= wsum;
    area /= wsum;

    if (dbg > 0.5 && dbg < 1.5) {
        out_color = vec4(clamp(mv.x / 32.0 + 0.5, 0.0, 1.0),
                         clamp(mv.y / 32.0 + 0.5, 0.0, 1.0),
                         clamp(area * 6.0, 0.0, 1.0), 1.0);
        return;
    }
    if (dbg > 1.5 && dbg < 2.5) { out_color = vec4(vec3(clamp(conf, 0.0, 1.0)), 1.0); return; }

    /* Low keeps the warp short and half-strength; it is the setting for
     * "smoother, but do not invent much". Scaling the vector down - what this
     * used to do - just guarantees every block lands in the wrong place. */
    float strength = (mode == 1) ? 0.55 : 1.0;
    float mv_max = (mode == 1) ? 20.0 : 56.0;
    /* How far the two warped samples may disagree before this is read as an
     * occlusion. Low stays cautious; High has to reach further or most of a
     * dark, detailed frame just cross-fades. */
    float occ_hi = (mode == 1) ? 0.20 : 0.32;
    float occ_lo = (mode == 1) ? 0.045 : 0.075;
    float floor_w = (mode == 1) ? 0.35 : 0.45;
    float mlen = length(mv);
    conf *= smoothstep(mv_max * 1.6, mv_max, mlen);

    vec2 mv_uv = mv * inv;
    vec4 a0 = texture(uIn0, vUV);
    vec4 b0 = texture(uIn1, vUV);
    vec4 cA = texture(uIn0, clamp(vUV - t * mv_uv, vec2(0.0), vec2(1.0)));
    vec4 cB = texture(uIn1, clamp(vUV + (1.0 - t) * mv_uv, vec2(0.0), vec2(1.0)));

    /* Occlusion. Where one frame can see something the other cannot, the two
     * warped samples disagree however good the vector is. */
    float mis = gray(abs(cA.rgb - cB.rgb));
    float chg = gray(abs(a0.rgb - b0.rgb));
    float agree = smoothstep(occ_hi, occ_lo, mis);
    /* ...and the vector has to earn its keep: it should explain more of the
     * change between the frames than leaving the pixel where it is. */
    float gain = smoothstep(0.0, 0.5, (chg - mis) / max(chg, 0.02));
    float w = clamp(conf * agree * mix(floor_w, 1.0, gain) * strength, 0.0, 1.0);

    if (dbg > 2.5) { out_color = vec4(vec3(w), 1.0); return; }

    /* Scale the warp DISTANCE by the weight rather than cross-fading a warped
     * image with an unwarped one. Half weight then means one image displaced
     * half way, not two images on top of each other. */
    vec2 mv_w = mv_uv * w;
    vec3 wA = texture(uIn0, clamp(vUV - t * mv_w, vec2(0.0), vec2(1.0))).rgb;
    vec3 wB = texture(uIn1, clamp(vUV + (1.0 - t) * mv_w, vec2(0.0), vec2(1.0))).rgb;
    vec3 res = mix(wA, wB, t);

    /* Never leave the range the two frames actually hold around here: a
     * mis-sampled tap cannot become a bright speck on a dark edge. */
    vec3 lo = min(min(cA.rgb, cB.rgb), min(a0.rgb, b0.rgb)) - vec3(0.03);
    vec3 hi = max(max(cA.rgb, cB.rgb), max(a0.rgb, b0.rgb)) + vec3(0.03);
    res = clamp(res, lo, hi);

    /* A cut, or motion past the search range, pushes the residual up across a
     * whole region: hold the nearer real frame instead of dissolving. */
    float cut = smoothstep(0.12, 0.22, area);
    res = mix(res, (t < 0.5) ? a0.rgb : b0.rgb, cut);

    out_color = vec4(clamp(res, vec3(0.0), vec3(1.0)), 1.0);
}
"""

def resource_mapping(samplers: int) -> str:
    lines = [
        "userDataNode[0].visibility = 2",
        "userDataNode[0].type = DescriptorTableVaPtr",
        "userDataNode[0].offsetInDwords = 0",
        "userDataNode[0].sizeInDwords = 1",
        "userDataNode[0].next[0].type = DescriptorConstBuffer",
        "userDataNode[0].next[0].offsetInDwords = 0",
        "userDataNode[0].next[0].sizeInDwords = 8",
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
; Generated by tools/gen_interp_pipes.py. Edit the generator, not this file.

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
    print("Generating frame interpolation .pipe files...")
    write_pipe("interp_pyr", INTERP_PYR_FS, 2, RGBA16F, "Quarter-res prefiltered luma pyramid")
    write_pipe("interp_me", INTERP_ME_FS, 3, RGBA16F, "Motion estimation, coarse-to-fine")
    write_pipe("interp_median", INTERP_MEDIAN_FS, 1, RGBA16F, "Vector median + area residual")
    write_pipe("interp_warp", INTERP_WARP_FS, 3, RGBA8, "Bidirectional warp & frame synthesis")
    print("OK")
    return 0


if __name__ == "__main__":
    main()
