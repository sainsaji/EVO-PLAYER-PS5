# pp/shaders — hand-written GPU shaders for the AGC render path

There is no PSSL compiler (see [../../docs/evo-pro/agc-implementation.md](../../docs/evo-pro/agc-implementation.md) §1).
These `.s` files are AMD GCN / RDNA2 assembly for the PS5 GPU (`gfx1030`),
assembled to raw `.text` blobs with `tools/build-shader.sh` (which drives
`llvm-mc-18` — verified working 2026-09-02).

```
./tools/build-shader.sh pp/shaders/rgba_ps.s
```

## Status

**None of these have run on hardware.** They are written against the register
conventions observed by disassembling ProsperoLight's proven
`pixel.text.linear-buffer.bin` (`third_party/ProsperoLight/assets/private/`):

| what | where |
|---|---|
| descriptor-table base pointer | `s[28:29]` |
| image descriptors (T#) | `s[0:7]`, `s[8:15]` |
| sampler descriptors (S#) | `s[16:19]`, `s[20:23]` |
| barycentric I / J | `v0` / `v1` |
| interpolation state | `m0` ← `s30` |
| varyings | `attr0` = first, `attr1` = second, … |
| render-target export | `exp mrt0 …` |

## The `.header.bin` question

`sceAgcCreateShader(shader, header, code)` needs both halves. `build-shader.sh`
only produces `code`. The `header` (SPI_PS_INPUT_*, VGPR/SGPR counts, the
resource-count fields ProsperoLight's `render_frame` reads back at shader
offsets +91/+92) is **not** produced here.

Plan: **reuse ProsperoLight's `pixel.header.bin` and swap only the `.text`.**
An RGBA shader is a structural subset of its NV12 shader (fewer images, no
matrix, no gamma), so its header should over-describe rather than
mis-describe. First device test = confirm `sceAgcCreateShader` accepts the
pair. If not, the fallback is to hand-build the header from the RDNA2 ISA
reference + the KytyPS5 / shadPS5 `.sb` parsers.

## Files

| file | shader | needed by |
|---|---|---|
| `rgba_ps.s` | textured quad, sample RGBA × vertex colour, export MRT0 | Step 2 OSD composite, Step 3 |
| _(later)_ `solid_ps.s` | vertex colour only | Step 3 |
| _(later)_ `ui_vs.s` | 2D-ortho transform of a structured UI vertex | Step 3 |
