#!/usr/bin/env python3
"""Generate the four video .pipe files from their shared vertex stage.

All four video pipelines draw the same fullscreen quad and differ only in the
fragment stage, so the vertex shader and the [ResourceMapping] skeleton live
here once rather than being copy-pasted four ways.

Two things differ from the UI pipeline and are easy to get wrong:

  * The vertex stage takes NO vertex buffer - it builds the quad from
    gl_VertexIndex - so there is no IndirectUserDataVaPtr node and no
    [VertexInputState] block. build_agc_pipes.py therefore has to tolerate a
    missing vertex-buffer table rather than failing the build.
  * The fragment samplers move to set 1. The vertex stage already uses
    set 0 binding 0 for its uniform block, and a fragment sampler at the same
    set+binding would collide.

Run inside the amdllpc image, then compile with tools/build_agc_pipes.py.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "projects/evoplayer/shaders/agc"

VS = """#version 450

layout(set = 0, binding = 0, std140) uniform VideoConstants {
    vec2 uCrop;
    vec2 uScale;
} video;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = vec2(p.x, 1.0 - p.y) * video.uCrop;
    gl_Position = vec4((p * 2.0 - 1.0) * video.uScale, 0.0, 1.0);
}
"""

# BT.601 limited-range YUV -> RGB, matching the pre-migration shaders.
NV12_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

void main() {
    float y = texture(uY, vUV).r;
    vec2 uv = texture(uUV, vUV).rg;
    float U = uv.x - 0.5;
    float V = uv.y - 0.5;

    float Y = (y - 0.0627451) * 1.1640625;
    vec3 rgb = vec3(
        Y + 1.59765625 * V,
        Y - 0.390625 * U - 0.8125 * V,
        Y + 2.015625 * U
    );
    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
"""

PLANAR_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uU;
layout(set = 1, binding = 2) uniform sampler2D uV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

void main() {
    float y = texture(uY, vUV).r;
    float U = texture(uU, vUV).r - 0.5;
    float V = texture(uV, vUV).r - 0.5;

    float Y = (y - 0.0627451) * 1.1640625;
    vec3 rgb = vec3(
        Y + 1.59765625 * V,
        Y - 0.390625 * U - 0.8125 * V,
        Y + 2.015625 * U
    );
    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
"""

# P010 10-bit SDR (BT.709 limited range, standard for HD 10-bit SDR)
P010_SDR_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844;
    float U = uv.x - 0.5;
    float V = uv.y - 0.5;

    float Y = (y - 0.0627451) * 1.1640625;
    vec3 rgb = vec3(
        Y + 1.79274107 * V,
        Y - 0.21324861 * U - 0.53290933 * V,
        Y + 2.11240179 * U
    );
    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
"""

# 10-bit paths: P010 stores 10 bits in the high bits of a 16-bit sample, so the
# R16/RG16 UNORM fetch comes back scaled by 65535/1023 = 64.0616 too small.
HDR_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

vec3 yuv2020(float y, float U, float V) {
    float Yp = (y - 0.0627451) * 1.1640625;
    return vec3(Yp + 1.4746 * V,
                Yp - 0.16455 * U - 0.57135 * V,
                Yp + 1.8814 * U);
}


/*
 * HDR -> SDR, the way broadcast does it (#hdr-brightness):
 *   linear BT.2020 light in nits
 *   -> BT.2020 to BT.709 primaries (without it every HDR colour reads washed out)
 *   -> ITU-R BT.2390 EETF on max(R,G,B), in the PQ domain: shadows and mids keep
 *      their mastered level, only highlights roll off towards the target peak
 *   -> divide by the SDR target (100 nits = SDR reference white) and encode.
 * The old curve was Reinhard at 100 nits, which put reference white at 50 %
 * and dimmed everything under it too - "HDR looks dark".
 */
const float SRC_PEAK = 1000.0;   /* assumed mastering peak - the common case */
const float SDR_PEAK = 100.0;    /* the SDR display the image is fitted to */

float pq_oetf(float nits) {
    float Y = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), 78.84375);
}

float pq_to_nits(float N) {
    float Np = pow(max(N, 0.0), 1.0 / 78.84375);
    return 10000.0 * pow(max(Np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * Np),
                         1.0 / 0.1593017578125);
}

/* BT.2390 EETF: source [0, SRC_PEAK] -> [0, SDR_PEAK], Hermite knee in PQ. */
float eetf(float nits) {
    float maxLum = pq_oetf(SRC_PEAK);
    float e1 = min(pq_oetf(nits) / maxLum, 1.0);
    float maxT = pq_oetf(SDR_PEAK) / maxLum;
    float ks = 1.5 * maxT - 0.5;
    float e2 = e1;
    if (e1 > ks) {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t, t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
           + (t3 - 2.0 * t2 + t) * (1.0 - ks)
           + (-2.0 * t3 + 3.0 * t2) * maxT;
    }
    return pq_to_nits(e2 * maxLum);
}

vec3 hdr_nits_to_sdr(vec3 nits2020) {
    /* BT.2020 -> BT.709 primaries (linear light, ITU-R BT.2087) */
    vec3 c = vec3(dot(nits2020, vec3( 1.6605, -0.5876, -0.0728)),
                  dot(nits2020, vec3(-0.1246,  1.1329, -0.0083)),
                  dot(nits2020, vec3(-0.0182, -0.1006,  1.1187)));
    c = max(c, vec3(0.0));
    /* tone-map each channel on its own. Below the knee the EETF is the
     * identity, so mids keep their colour; above it the channels converge,
     * so highlights roll off to white. Scaling all three by the max
     * channel's factor kept a highlight's small tint at full strength:
     * bright windows and skies came out pink/cyan mottled. */
    c = vec3(eetf(c.r), eetf(c.g), eetf(c.b));
    vec3 sdr = clamp(c / SDR_PEAK, 0.0, 1.0);
    return pow(sdr, vec3(1.0 / 2.2));
}

void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);

    vec3 pq_rgb = clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0);
    vec3 nits = vec3(pq_to_nits(pq_rgb.r), pq_to_nits(pq_rgb.g), pq_to_nits(pq_rgb.b));
    out_color = vec4(hdr_nits_to_sdr(nits), 1.0);
}
"""

HLG_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

vec3 yuv2020(float y, float U, float V) {
    float Yp = (y - 0.0627451) * 1.1640625;
    return vec3(Yp + 1.4746 * V,
                Yp - 0.16455 * U - 0.57135 * V,
                Yp + 1.8814 * U);
}

/* ARIB STD-B67 (HLG) electro-optical transfer function. */
float hlg_eotf(float e) {
    if (e <= 0.5)
        return (e * e) / 3.0;
    return (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}


/*
 * HDR -> SDR, the way broadcast does it (#hdr-brightness):
 *   linear BT.2020 light in nits
 *   -> BT.2020 to BT.709 primaries (without it every HDR colour reads washed out)
 *   -> ITU-R BT.2390 EETF on max(R,G,B), in the PQ domain: shadows and mids keep
 *      their mastered level, only highlights roll off towards the target peak
 *   -> divide by the SDR target (100 nits = SDR reference white) and encode.
 * The old curve was Reinhard at 100 nits, which put reference white at 50 %
 * and dimmed everything under it too - "HDR looks dark".
 */
const float SRC_PEAK = 1000.0;   /* assumed mastering peak - the common case */
const float SDR_PEAK = 100.0;    /* the SDR display the image is fitted to */

float pq_oetf(float nits) {
    float Y = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), 78.84375);
}

float pq_to_nits(float N) {
    float Np = pow(max(N, 0.0), 1.0 / 78.84375);
    return 10000.0 * pow(max(Np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * Np),
                         1.0 / 0.1593017578125);
}

/* BT.2390 EETF: source [0, SRC_PEAK] -> [0, SDR_PEAK], Hermite knee in PQ. */
float eetf(float nits) {
    float maxLum = pq_oetf(SRC_PEAK);
    float e1 = min(pq_oetf(nits) / maxLum, 1.0);
    float maxT = pq_oetf(SDR_PEAK) / maxLum;
    float ks = 1.5 * maxT - 0.5;
    float e2 = e1;
    if (e1 > ks) {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t, t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
           + (t3 - 2.0 * t2 + t) * (1.0 - ks)
           + (-2.0 * t3 + 3.0 * t2) * maxT;
    }
    return pq_to_nits(e2 * maxLum);
}

vec3 hdr_nits_to_sdr(vec3 nits2020) {
    /* BT.2020 -> BT.709 primaries (linear light, ITU-R BT.2087) */
    vec3 c = vec3(dot(nits2020, vec3( 1.6605, -0.5876, -0.0728)),
                  dot(nits2020, vec3(-0.1246,  1.1329, -0.0083)),
                  dot(nits2020, vec3(-0.0182, -0.1006,  1.1187)));
    c = max(c, vec3(0.0));
    /* tone-map each channel on its own. Below the knee the EETF is the
     * identity, so mids keep their colour; above it the channels converge,
     * so highlights roll off to white. Scaling all three by the max
     * channel's factor kept a highlight's small tint at full strength:
     * bright windows and skies came out pink/cyan mottled. */
    c = vec3(eetf(c.r), eetf(c.g), eetf(c.b));
    vec3 sdr = clamp(c / SDR_PEAK, 0.0, 1.0);
    return pow(sdr, vec3(1.0 / 2.2));
}

void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);

    vec3 hlg_rgb = clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0);
    vec3 scene = vec3(hlg_eotf(hlg_rgb.r), hlg_eotf(hlg_rgb.g), hlg_eotf(hlg_rgb.b));
    /* BT.2100 HLG OOTF for a 1000-nit display: system gamma 1.2, on luminance */
    float ys = max(dot(scene, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
    vec3 nits = 1000.0 * pow(ys, 0.2) * scene;
    out_color = vec4(hdr_nits_to_sdr(nits), 1.0);
}
"""


# ---------------------------------------------------------------------------
# Real HDR10 output (the display is registered Bgr10A2Bt2100Pq): these write
# BT.2020 PQ code values straight to a 10:10:10:2 scanout. Nothing is tone
# mapped - the TV does that for its own panel, which is the point of HDR10.
# ---------------------------------------------------------------------------

PQ_OUT_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

vec3 yuv2020(float y, float U, float V) {
    float Yp = (y - 0.0627451) * 1.1640625;
    return vec3(Yp + 1.4746 * V,
                Yp - 0.16455 * U - 0.57135 * V,
                Yp + 1.8814 * U);
}

/* HDR10 source -> HDR10 output: the decoded R'G'B' already IS BT.2020 PQ, so
 * the only work is YCbCr -> RGB. */
void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);
    out_color = vec4(clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0), 1.0);
}
"""

HLG_PQ_FS = """#version 450

layout(set = 1, binding = 0) uniform sampler2D uY;
layout(set = 1, binding = 1) uniform sampler2D uUV;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

vec3 yuv2020(float y, float U, float V) {
    float Yp = (y - 0.0627451) * 1.1640625;
    return vec3(Yp + 1.4746 * V,
                Yp - 0.16455 * U - 0.57135 * V,
                Yp + 1.8814 * U);
}

float hlg_eotf(float e) {
    if (e <= 0.5)
        return (e * e) / 3.0;
    return (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}

float pq_oetf(float nits) {
    float Y = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), 78.84375);
}

/* HLG -> HDR10: scene light, the BT.2100 OOTF for a 1000-nit reference
 * display (system gamma 1.2, on luminance), then PQ-encode. BT.2020 both
 * sides, so no primaries conversion. */
void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);
    vec3 hlg_rgb = clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0);
    vec3 scene = vec3(hlg_eotf(hlg_rgb.r), hlg_eotf(hlg_rgb.g), hlg_eotf(hlg_rgb.b));
    float ys = max(dot(scene, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
    vec3 nits = 1000.0 * pow(ys, 0.2) * scene;
    out_color = vec4(pq_oetf(nits.r), pq_oetf(nits.g), pq_oetf(nits.b), 1.0);
}
"""

def resource_mapping(sampler_count: int) -> str:
    """Vertex uniform block at set 0, fragment samplers at set 1.

    visibility 2 = vertex stage, 64 = fragment stage. The vertex stage has no
    IndirectUserDataVaPtr because it takes no vertex buffer.
    """
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
    for i in range(sampler_count):
        lines += [
            f"userDataNode[1].next[{i}].type = DescriptorCombinedTexture",
            f"userDataNode[1].next[{i}].offsetInDwords = {i * 12}",
            f"userDataNode[1].next[{i}].sizeInDwords = 12",
            f"userDataNode[1].next[{i}].set = 1",
            f"userDataNode[1].next[{i}].binding = {i}",
        ]
    return "\n".join(lines)


def build(name: str, fs: str, samplers: int, comment: str) -> None:
    text = f"""; {name} - {comment}
; Generated by tools/gen_video_pipes.py. Edit the generator, not this file.
;
; Fullscreen quad built from gl_VertexIndex, so there is no vertex buffer and no
; [VertexInputState]. Fragment samplers live at set 1 because the vertex stage
; already owns set 0 binding 0 for its uniform block.

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
; R8G8B8A8 because the shader exports plain RGBA; the BGRA ordering the scanout
; wants is done by COMP_SWAP=ALT in CB_COLOR0_INFO (see evo_agc_runtime.c).
colorBuffer[0].format = VK_FORMAT_R8G8B8A8_UNORM
colorBuffer[0].channelWriteMask = 15
colorBuffer[0].blendEnable = 0
"""
    path = SHADER_DIR / f"{name}.pipe"
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"  wrote {path.name} ({samplers} sampler(s))")


def main() -> int:
    print("Generating video .pipe files...")
    build("video_yuv_nv12", NV12_FS, 2, "NV12 8-bit SDR, BT.601 limited range")
    build("video_yuv_planar", PLANAR_FS, 3, "planar I420 8-bit SDR, BT.601 limited")
    build("video_yuv_p010_sdr", P010_SDR_FS, 2, "P010 10-bit SDR, BT.709 limited range")
    build("video_yuv_p010_hdr", HDR_FS, 2, "P010 10-bit PQ (ST.2084) -> SDR: BT.2390 EETF + BT.2020->709")
    build("video_yuv_p010_hlg", HLG_FS, 2, "P010 10-bit HLG (ARIB STD-B67) -> SDR: BT.2100 OOTF + BT.2390 EETF + BT.2020->709")
    build("video_yuv_p010_pq_out", PQ_OUT_FS, 2, "P010 HDR10 -> HDR10 output (PQ passthrough)")
    build("video_yuv_p010_hlg_pq_out", HLG_PQ_FS, 2, "P010 HLG -> HDR10 output (BT.2100 OOTF, PQ)")

    # 8-bit HDR: HEVC Main (8-bit) carrying HLG / PQ over BT.2020 exists -
    # broadcast 4K HLG channels do it. Same maths; the NV12 planes are R8/RG8
    # and already sample as 0..1, so drop the 10-bit-in-16 rescale.
    def eight_bit(fs):
        out = fs.replace(" * 64.0615844", "")
        assert "64.0615844" not in out
        return out

    build("video_yuv_nv12_hdr", eight_bit(HDR_FS), 2, "NV12 8-bit PQ -> SDR (BT.2390 EETF + BT.2020->709)")
    build("video_yuv_nv12_hlg", eight_bit(HLG_FS), 2, "NV12 8-bit HLG -> SDR (BT.2100 OOTF + BT.2390 EETF + BT.2020->709)")
    build("video_yuv_nv12_pq_out", eight_bit(PQ_OUT_FS), 2, "NV12 8-bit PQ -> HDR10 output (PQ passthrough)")
    build("video_yuv_nv12_hlg_pq_out", eight_bit(HLG_PQ_FS), 2, "NV12 8-bit HLG -> HDR10 output (BT.2100 OOTF, PQ)")
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
