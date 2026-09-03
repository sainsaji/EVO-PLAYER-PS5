# pp/shaders — hand-written GPU shaders for the AGC render path

There is no PSSL compiler (see [../../docs/evo-pro/agc-implementation.md](../../docs/evo-pro/agc-implementation.md) §1).
These `.s` files are AMD GCN / RDNA2 assembly for the PS5 GPU (`gfx1030`),
assembled to raw `.text` blobs with `tools/build-shader.sh` (which drives
`llvm-mc-18` — verified working 2026-09-02).

```
./tools/build-shader.sh pp/shaders/rgba_ps.s   # one file
./tools/build-shader.sh --all                  # every *.s + refresh SHA256SUMS
```

The assembled `.bin` code halves and `SHA256SUMS` are checked in here;
`projects/evoplayer/pp/src/agc_ui_blobs.S` `.incbin`s them (paths
`../../pp/shaders/*.bin`) alongside the reused ProsperoLight headers.

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

**Pixel shaders** — reuse ProsperoLight's `pixel.header.bin` and swap only the
`.text`. `solid_ps` / `glyph_ps` / `rgba_ps` are each a structural subset of
the NV12 PS (≤ its images/samplers/CBs, no YUV matrix), so its header should
over-describe rather than mis-describe. First device test = confirm
`sceAgcCreateShader` accepts the pair.

**`ui_vs`** — the reuse trick does **not** hold: it needs a third fetched
attribute (per-vertex colour) and a second exported param vs
`geometry.text.bin`, which is a header change, not a subset. See the risk note
at the top of `ui_vs.s` — resolve at the Phase-1 hardware session (colour
smuggled through the 2-slot fetch / hand-built header / GLSL compiler).

Fallback for any of them: hand-build the header from the RDNA2 ISA reference +
the KytyPS5 / shadPS5 / SharpProspero `ShaderInfo.cs` `.sb` parsers.

## Files

| file | shader | needed by |
|---|---|---|
| `rgba_ps.s` | textured quad, sample RGBA × vertex colour, export MRT0 | Step 2 OSD composite, Step 3 |
| `solid_ps.s` | vertex colour only (rects, borders, gradient meshes) | Step 3 |
| `glyph_ps.s` | 1-channel coverage atlas × colour (text, R8 clip-mask sample) | Step 3 |
| `ui_vs.s` | 2D-ortho transform of a structured UI vertex (pos + colour + UV) | Step 3 — **needs its own header** |
