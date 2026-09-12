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

/* SMPTE ST.2084 (PQ) electro-optical transfer function. */
float pq_eotf(float N) {
    float Np = pow(max(N, 0.0), 1.0 / 78.84375);
    return pow(max(Np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * Np),
               1.0 / 0.1593017578125);
}

void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);

    vec3 pq_rgb = clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0);
    vec3 linear_10k = vec3(pq_eotf(pq_rgb.r), pq_eotf(pq_rgb.g), pq_eotf(pq_rgb.b));
    vec3 nits = linear_10k * 100.0;
    vec3 sdr_linear = nits / (1.0 + nits);       /* Reinhard tone map to SDR */
    out_color = vec4(pow(sdr_linear, vec3(1.0 / 2.2)), 1.0);
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

void main() {
    float y = texture(uY, vUV).r * 64.0615844;
    vec2 uv = texture(uUV, vUV).rg * 64.0615844 - vec2(0.5);

    vec3 hlg_rgb = clamp(yuv2020(y, uv.x, uv.y), 0.0, 1.0);
    vec3 scene = vec3(hlg_eotf(hlg_rgb.r), hlg_eotf(hlg_rgb.g), hlg_eotf(hlg_rgb.b));
    vec3 nits = pow(max(scene, 0.0), vec3(1.2)) * 100.0;   /* OOTF, gamma 1.2 */
    vec3 sdr_linear = nits / (1.0 + nits);
    out_color = vec4(pow(sdr_linear, vec3(1.0 / 2.2)), 1.0);
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
    build("video_yuv_p010_hdr", HDR_FS, 2, "P010 10-bit PQ (ST.2084) tone-mapped to SDR")
    build("video_yuv_p010_hlg", HLG_FS, 2, "P010 10-bit HLG (ARIB STD-B67) tone-mapped to SDR")
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
