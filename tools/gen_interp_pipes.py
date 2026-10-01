#!/usr/bin/env python3
"""Generate the #105 frame interpolation (motion smoothing) .pipe files.

Three pipelines:
  interp_me      Motion estimation via block matching on downscaled luma.
                 8x8 blocks at 1/4 resolution (32x32 px in source).
                 Outputs RGBA16F: (mv.x, mv.y, sad, confidence).
  interp_median  3x3 median filter on the motion vector field.
  interp_warp    Bidirectional frame synthesis (warp A forward by t*MV,
                 B backward by (1-t)*MV, blend, fallback to linear blend / nearest).

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
# interp_me: Motion estimation
# uIn0 = Frame A (source RGBA8), uIn1 = Frame B (source RGBA8)
# Target size: (src_w + 31)/32 x (src_h + 31)/32 (e.g. 60x34 for 1080p)
# ---------------------------------------------------------------------------
INTERP_ME_FS = fs_header(2) + """
float get_luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    ivec2 src_sz = textureSize(uIn0, 0);
    vec2 inv_sz = 1.0 / vec2(src_sz);

    // Each fragment corresponds to one 32x32 block in the source frame
    ivec2 num_blocks = ivec2((src_sz.x + 31) / 32, (src_sz.y + 31) / 32);
    vec2 block_idx = floor(vUV * vec2(num_blocks));
    vec2 center_px = (block_idx + vec2(0.5)) * 32.0;

    // 1. Sample 8x8 luma grid from Frame A
    float lumA[64];
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            vec2 pt = center_px + vec2(float(x) - 3.5, float(y) - 3.5) * 4.0;
            lumA[y * 8 + x] = get_luma(texture(uIn0, clamp(pt * inv_sz, vec2(0.0), vec2(1.0))).rgb);
        }
    }

    // 2. Coarse search in [-16, +16] with step 8 (5x5 = 25 candidates)
    vec2 best_disp = vec2(0.0);
    float best_sad = 1e6;

    for (int dy = -16; dy <= 16; dy += 8) {
        for (int dx = -16; dx <= 16; dx += 8) {
            vec2 disp = vec2(float(dx), float(dy));
            float sad = 0.0;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    vec2 ptB = center_px + vec2(float(x) - 3.5, float(y) - 3.5) * 4.0 + disp;
                    float b = get_luma(texture(uIn1, clamp(ptB * inv_sz, vec2(0.0), vec2(1.0))).rgb);
                    sad += abs(lumA[y * 8 + x] - b);
                }
            }
            if (sad < best_sad) {
                best_sad = sad;
                best_disp = disp;
            }
        }
    }

    // 3. Fine refinement in [-4, +4] with step 4 around best coarse vector
    for (int dy = -4; dy <= 4; dy += 4) {
        for (int dx = -4; dx <= 4; dx += 4) {
            if (dx == 0 && dy == 0) continue;
            vec2 disp = best_disp + vec2(float(dx), float(dy));
            float sad = 0.0;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    vec2 ptB = center_px + vec2(float(x) - 3.5, float(y) - 3.5) * 4.0 + disp;
                    float b = get_luma(texture(uIn1, clamp(ptB * inv_sz, vec2(0.0), vec2(1.0))).rgb);
                    sad += abs(lumA[y * 8 + x] - b);
                }
            }
            if (sad < best_sad) {
                best_sad = sad;
                best_disp = disp;
            }
        }
    }

    float mean_sad = best_sad / 64.0;
    float conf = clamp(1.0 - mean_sad * 4.0, 0.0, 1.0);
    out_color = vec4(best_disp.x, best_disp.y, mean_sad, conf);
}
"""

# ---------------------------------------------------------------------------
# interp_median: 3x3 median filter on motion vectors
# uIn0 = Raw MV texture (RGBA16F)
# ---------------------------------------------------------------------------
INTERP_MEDIAN_FS = fs_header(1) + """
void sort9(inout float a[9]) {
    for (int i = 0; i < 8; ++i) {
        for (int j = i + 1; j < 9; ++j) {
            if (a[j] < a[i]) {
                float tmp = a[i];
                a[i] = a[j];
                a[j] = tmp;
            }
        }
    }
}

void main() {
    ivec2 sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(sz)), ivec2(0), sz - 1);

    float xs[9];
    float ys[9];
    int idx = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 p = clamp(ip + ivec2(dx, dy), ivec2(0), sz - 1);
            vec4 v = texelFetch(uIn0, p, 0);
            xs[idx] = v.x;
            ys[idx] = v.y;
            idx++;
        }
    }

    sort9(xs);
    sort9(ys);

    vec4 center = texelFetch(uIn0, ip, 0);
    out_color = vec4(xs[4], ys[4], center.z, center.w);
}
"""

# ---------------------------------------------------------------------------
# interp_warp: Frame synthesis
# uIn0 = Frame A, uIn1 = Frame B, uIn2 = Filtered MV field (RGBA16F)
# uParams: x = phase t [0..1], y = mode (1=Low, 2=High)
# ---------------------------------------------------------------------------
INTERP_WARP_FS = fs_header(3) + """
void main() {
    float t = clamp(vParams.x, 0.0, 1.0);
    int mode = int(vParams.y + 0.5);

    if (t <= 0.001 || mode <= 0) {
        out_color = texture(uIn0, vUV);
        return;
    }
    if (t >= 0.999) {
        out_color = texture(uIn1, vUV);
        return;
    }

    ivec2 src_sz = textureSize(uIn0, 0);
    vec2 inv_sz = 1.0 / vec2(src_sz);

    vec4 mv_data = texture(uIn2, vUV);
    vec2 mv_px = mv_data.xy;
    float mean_sad = mv_data.z;
    float conf = mv_data.w;

    // Scene cut detection: if mean_sad is high, skip interpolation (nearest frame)
    if (mean_sad > 0.22) {
        out_color = (t < 0.5) ? texture(uIn0, vUV) : texture(uIn1, vUV);
        return;
    }

    // In Low mode, attenuate motion vector and warp contribution
    if (mode == 1) {
        mv_px *= 0.5;
        conf *= 0.5;
    }

    vec2 mv_uv = mv_px * inv_sz;

    // Forward warp for Frame A: sampled at vUV - t * MV
    vec2 uv_A = clamp(vUV - t * mv_uv, vec2(0.0), vec2(1.0));
    vec4 col_A_warp = texture(uIn0, uv_A);

    // Backward warp for Frame B: sampled at vUV + (1 - t) * MV
    vec2 uv_B = clamp(vUV + (1.0 - t) * mv_uv, vec2(0.0), vec2(1.0));
    vec4 col_B_warp = texture(uIn1, uv_B);

    vec4 col_warp = mix(col_A_warp, col_B_warp, t);

    // Plain linear blend fallback
    vec4 col_A = texture(uIn0, vUV);
    vec4 col_B = texture(uIn1, vUV);
    vec4 col_blend = mix(col_A, col_B, t);

    // Photometric consistency (edge and occlusion protection)
    float photo_err = length(col_A_warp.rgb - col_B_warp.rgb);
    float edge_conf = clamp(1.0 - photo_err * 2.5, 0.0, 1.0);
    float final_conf = conf * edge_conf;

    // Nearest frame fallback for very low confidence
    vec4 col_nearest = (t < 0.5) ? col_A : col_B;

    vec4 result;
    if (final_conf < 0.15) {
        result = mix(col_nearest, col_blend, final_conf / 0.15);
    } else {
        result = mix(col_blend, col_warp, final_conf);
    }

    out_color = vec4(clamp(result.rgb, 0.0, 1.0), 1.0);
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
    write_pipe("interp_me", INTERP_ME_FS, 2, RGBA16F, "Motion estimation block matching")
    write_pipe("interp_median", INTERP_MEDIAN_FS, 1, RGBA16F, "Motion vector 3x3 median filter")
    write_pipe("interp_warp", INTERP_WARP_FS, 3, RGBA8, "Bidirectional warp & frame synthesis")
    print("OK")
    return 0


if __name__ == "__main__":
    main()
