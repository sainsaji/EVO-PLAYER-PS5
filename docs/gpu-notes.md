# GPU on the PS5 — the `ps5-opengl` funnel

> **Superseded 2026-09-10 (GL-6 / #82).** This file used to conclude "there is
> no open hardware GL/Vulkan path on this SDK". That is no longer true. EVO now
> renders **everything** — menus, video, OSD, subtitles, keyboard, HUD — through
> a single OpenGL context on `third_party/ps5-opengl/` (Mesa + a bespoke PS5
> Gallium driver + a patched PSSL compiler → `sceAgc`). The hand-rolled `sceAgc`
> present path (`pp_agc*`, `pp_videoout`) and the CPU YUV→BGRA converters that
> this document weighed are all deleted. The reverse-engineering history below
> is kept for context.

## What runs today

```
decode (CPU, sceVideodec2 / FFmpeg) ─ NV12/P010 ─┐
                                                 ├─► GL: video quad (YUV→RGB fragment shader)
RmlUi context (all screens + overlays) ───────────┤        + UI pass + HUD pass
                                                 └─► eglSwapBuffers ─► ps5-opengl ─► sceAgc DCB + sceVideoOut flip
```

- **One graphics context**, created in `main()`'s pre-unjail slot
  (`ui_rml/src/evo_gl_context_device.cpp`) — `libSceAgc*` / `libSceVideoOut` go
  API-dead after the self-unjail credential swap, so GL cannot be lazily
  brought up on first draw. A second `sceVideoOut` open panics the console, so
  `ps5-opengl` is the sole owner of `sceAgc` **and** the flip queue.
- **Mesa 26.2 / GL 3.3 Core**, validated on FW 12.70 (GL-1, `--gl-smoke`
  receipt — [evo-pro/gl1-spike.md](evo-pro/gl1-spike.md)).
- The video path uploads NV12 as R8 + RG8 textures (zero-copy from the decode
  frame pool) and does YUV→RGB + scale + OSD composite in one GLSL pass — see
  [evo-pro/gl4-video-path-plan.md](evo-pro/gl4-video-path-plan.md).
- Full plan, phasing and the pixel-path inventory:
  [evo-pro/opengl-render-overhaul.md](evo-pro/opengl-render-overhaul.md).

## Build

`ps5-opengl` is a git submodule built from source through the opt-in toolchain
overlay — `scripts/build-ps5-opengl.sh` + `docker-compose.ps5-opengl.yml`. It
must have been built once before `scripts/package-app.sh --ffpfsc`. `--gl` is
the default (and only) app-module present path; `--no-gl` was retired by GL-4.

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

### `ps5-opengl` (the render overhaul)

`ps5-opengl` resolves exactly that wall: Mesa's PSBC path compiles ordinary
GLSL to working PS5 shaders. It is the *same* `sceAgc` route, so the hand-rolled
present path had to be **removed, not run alongside** (dual `sceVideoOut`
ownership panics). GL-1…GL-6 did that migration; GL-6 deleted `pp_agc*` /
`pp_videoout` / the CPU converters.
