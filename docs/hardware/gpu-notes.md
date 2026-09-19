# GPU on the PS5 — the bare-metal `sceAgc` runtime

> **Where this landed.** This file used to conclude "there is no open hardware
> GL/Vulkan path on this SDK". EVO went on to try exactly that — a Mesa-based
> OpenGL stack (`ps5-opengl`) driving `sceAgc` — and then removed it again:
> RmlUi through that driver's GL3 path ran at ~1.2 s/frame. What ships is a
> bare-metal runtime that builds `sceAgc` command buffers itself
> (`media/src/evo_agc_runtime.c`), with shaders compiled by amdllpc from
> `.pipe` sources. The CPU YUV→BGRA converters this document weighed are gone
> either way. The reverse-engineering history below is kept for context.

## What runs today

```
decode (CPU, sceVideodec2 / FFmpeg) ─ NV12/P010 ─┐
                                                 ├─► AGC video pipelines (YUV→RGB on the GPU)
RmlUi context (all screens + overlays) ───────────┤   + EvoRenderInterfaceAGC UI pass + HUD
                                                 └─► sceAgc DCB submit ─► sceVideoOut flip
```

- **One graphics owner**, brought up in `main()`'s pre-unjail slot
  (`media/src/evo_agc_runtime.c`) — `libSceAgc*` / `libSceVideoOut` go API-dead
  after the self-unjail credential swap, so the device cannot be lazily brought
  up on first draw. A second `sceVideoOut` open panics the console, so the AGC
  runtime is the sole owner of `sceAgc` **and** the flip queue.
- The video path uploads NV12 as R8 + RG8 textures (zero-copy from the decode
  frame pool) and does YUV→RGB + scale + OSD composite in the AGC video
  pipelines.
- Render size comes from `sceVideoOutGetResolutionStatus` — the panel's own
  resolution, not a fixed 1080p.
- Shader toolchain, register model and hardware receipts:
  [evo-pro/agc-bare-metal-ui.md](../evo-pro/agc-bare-metal-ui.md).

## Build

Nothing extra to build: the AGC runtime talks to `libSceAgc` directly and
`scripts/package-app.sh --ffpfsc` is self-contained. (An OpenGL route via a
`ps5-opengl` submodule existed briefly and was removed; `--gl`, `--no-gl`,
`--gl-smoke` and `--gl-hdr-probe` now fail with that explanation.)

## HDR / 10-bit

10-bit (P010 / HEVC Main10) decodes and plays as SDR with BT.601-limited
unpack + a naive tone-map in the GL video shader (GL-5). A proper PQ/HLG
tone-map and `sceVideoOutSetHdrMetadata` HDR *output* are the remaining tail of
`#41`.

---

## History — the reverse-engineering that got here

### v0.42 audit: no Gnmx, no PSSL compiler, mesa ships software-only

`sce_stubs/libSceGnmDriver.c` gave link-time symbols
(`sceGnmSubmitCommandBuffers`, `sceGnmSubmitDone`, …) but there were **no GNM
headers, no Gnmx, no open PSSL compiler**, and the sysroot's `libGL.so` was
Off-Screen Mesa with only `llvmpipe` / `softpipe` / `swr` — `radeonsi` (the
hardware driver) absent, no `/dev/dri`. SDL2 was compiled with `dummy` /
`offscreen` video only. So `SDL_RenderCopy` on an NV12 texture would have
converted on the CPU through llvmpipe with no way to present.

### The hand-rolled `sceAgc` route (#27 / #28)

EVO then reverse-engineered ProsperoLight / SharpProspero's `sceAgc` usage and
built a hand-assembled DCB present path (`pp_agc.c`): `sceAgcInit`, shader
create/link from vendored blobs, a per-frame CX register block, NV12→RGB on the
GPU, `sceAgcDcbSetFlip`. This **worked** — GTA 4K at 982 µs/frame — but every
*textured* pixel shader failed `sceAgcCreateShader` validation (`0x8a6c001f`)
because Sony's compiler emits an `sl00` resource-metadata trailer that can't be
hand-authored. Textured UI (text, icons, art) as GPU geometry was blocked at
the toolchain level.

### How that wall came down

Two attempts. `ps5-opengl` (Mesa's PSBC path compiling ordinary GLSL to working
PS5 shaders) proved the shaders were obtainable, but RmlUi through its GL3
interface ran at ~1.2 s/frame. The answer was to keep the shader lesson and
drop the driver: `.pipe` sources compiled by **amdllpc**, fed to hand-built
`sceAgc` command buffers. Either way it is the *same* `sceAgc` route, so only
one present path can exist at a time — dual `sceVideoOut` ownership panics the
console.
