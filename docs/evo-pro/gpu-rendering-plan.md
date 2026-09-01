# GPU rendering — moving convert + composite + UI off the CPU

> **Status:** plan, not started. Sequenced **after
> [phase-1b-app-module.md](phase-1b-app-module.md) milestone 1** — Steps 2–3
> need the app-module context; Step 1 is CPU-only and can land any time.
>
> **Trigger:** the RmlUi UI runs at **~11 fps (~90 ms/frame)** against a design
> that targets 60 (16.6 ms). See [ui-handoff.md](../ui-handoff.md) and
> [rmlui-integration-guide.md](../rmlui-integration-guide.md) (§4 virtualisation,
> §5 rasteriser).

## 1. Why it's slow — and why packaging alone won't fix it

EVO has **no GPU path today**. `pp/src/pp_compute_pipeline.c` is misnamed — its
own header says *"AVX2 / SSE2 **CPU** SIMD Compute Kernel"*. Every frame:

- YUV→BGRA + swizzle convert runs on the CPU (`pp_converter_fused.c` is the
  current lever — see [converter-perf.md](../converter-perf.md))
- the full RmlUi tree is re-rasterised on the CPU, even when nothing changed
- the composite (UI over video) and the copy into the 4K `sceVideoOut`
  framebuffer are CPU byte-moves

At 3840×2160 that is tens of millions of CPU pixel ops per frame. Repackaging
EVO as an app module ([phase-1b-app-module.md](phase-1b-app-module.md)) changes
the *process context* — it does not move a single pixel onto the GPU.

**What the app-module context *does* change:** it is the environment in which
`sceVideodec2` started working (errno 5200 was a per-context gate), and it is
where **ProsperoLight runs a full `sceAgc` shader pipeline** — YUV→RGB convert,
UI compositing, and flip — from a fake-signed game-category title. That code is
now in the tree at
`third_party/ProsperoLight/src/native_agc_present.cpp` and is a working
reference for the exact context EVO is moving into. Whether `sceAgc` /
`sceAgcDriver` work from a *payload* was never confirmed, so this route is
genuinely more viable post-packaging.

## 2. The three GPU routes

| Route | Status |
|---|---|
| OpenGL / Vulkan / mesa (SDL accelerated renderer) | No hardware GL/Vulkan driver on this SDK ([gpu-notes.md](../gpu-notes.md)). SDL2's presence "says nothing about acceleration" — its renderer is software. Not an option. |
| Raw GNM (hand-assembled PM4) | `sceGnmAreSubmitsAllowed() → 1` on hardware, but no headers, no Gnmx, no PSSL compiler. "Large, speculative" ([gpu-notes.md](../gpu-notes.md)). |
| **AGC (`sceAgc` + `sceAgcDriver`)** | **ProsperoLight demonstrates the full pipeline** — `sceAgcCreateShader` / `sceAgcLinkShaders` / register setup / `sceAgcDcbDrawIndexAuto` / `sceAgcDriverSubmitDcb` / `sceAgcDcbSetFlip` — from a game-category app module. This is the route. |

## 3. How ProsperoLight splits the work

`third_party/ProsperoLight/src/main.cpp` (`SdlRenderInterface`) +
`native_agc_present.cpp`:

| Work | Where | Notes |
|---|---|---|
| RmlUi vertices / glyphs / layout | **CPU** | `SDL_RenderGeometry` into an `SDL_Surface`; the surface is only redrawn when the DOM changes, not per frame |
| YUV→RGB scale of the decoded frame | **GPU** | AGC pixel shader samples NV12, one fullscreen quad |
| Composite UI surface over video | **GPU** | second textured quad, alpha blend |
| HUD / overlay | **GPU** | third quad (`hud_ps_sh`) |
| Present | **GPU** | `sceAgcDcbSetFlip` + `sceAgcDriverSubmitDcb` |

The CPU keeps the cheap, bounded work (vector UI, only on change); the GPU does
the per-pixel work that scales with resolution.

## 4. Effort ladder

### Step 1 — dirty-flag the UI surface (CPU only, no GPU, ships independently)

Rasterise the RmlUi context to its backing surface **only when the DOM /
document changed** (RmlUi exposes this via the render interface and
`Context::Update` return signals). Composite the cached surface every frame.
For a static menu this drops per-frame UI cost to near zero.

- Touches: `ui_rml/src/evo_rmlui_render.cpp`, the per-frame loop in `main.c`.
- No console-context dependency — do this whenever, even before Phase 1b.
- Expected: the biggest single win if the profile (below) shows per-frame
  re-rasterisation dominates.

### Step 2 — AGC present + composite (needs the app module)

Lift `native_agc_present.cpp`. Move **YUV→BGRA convert**, the **final
composite**, and the **flip** onto the GPU. The CPU converter
(`pp_converter_*.c`) stops running on the hot path; `pp_videoout.c` submits an
AGC DCB instead of `sceVideoOutSubmitFlip` with a CPU-filled buffer.

- Shaders: ProsperoLight's NV12-sample + textured-quad shaders cover most of
  this. **No on-device PSSL compiler** — precompiled shader ISA blobs are
  embedded (as ProsperoLight does). This is the main new artifact.
- Touches: `pp/src/pp_videoout.c`, `pp/src/pp_converter*` (bypass on target),
  new `pp/src/pp_agc_present.c` + shader blobs.
- Guard behind `__PROSPERO__` so the host preview (`tools/uiview*`,
  `uiplay`) keeps the SDL/CPU path — no host regression.

### Step 3 — full RmlUi GPU geometry backend (optional, largest)

Bind `Rml::RenderInterface` (`RenderGeometry` / `CompileGeometry` /
`RenderCompiledGeometry` / scissor / transform) directly to AGC draw calls, so
UI triangles are rasterised on the GPU too. Removes the CPU UI raster entirely.

- More shaders (solid-colour, textured, scissored, transformed triangles) —
  still a bounded set.
- Only worth it if Step 1 + Step 2 don't reach the frame budget.

## 5. Interaction with native decode

Both tracks are AGC/compute consumers in the app slot and **coexist** —
ProsperoLight runs the `sceVideodec2` compute-queue decoder *and* the `sceAgc`
graphics pipeline in the same process. Ordering rule (carried from
[videodec2-abi.md](videodec2-abi.md) §6): bring the decoder up **after**
VideoOut / AGC init, tear it down **before**. The decoder uses a compute queue
(`sceVideodec2AllocateComputeQueue`); the renderer uses the graphics ring —
separate.

## 6. Before building anything — profile

`tools/uiview.sh` / `uiplay.sh` link the real drawing code against the real
assets on the host, no console. Instrument the RmlUi frame there and attribute
the ~90 ms:

- per-frame re-rasterisation vs. genuinely-changed regions
- glyph raster vs. cached atlas hits
- rasteriser fast-path (axis-aligned quads / SIMD span-fill) vs. slow path
  (transforms, sub-pixel positions, alpha)
- composite + framebuffer copy cost at 1080p vs 4K

The profile decides whether Step 1 alone is enough or Step 2 is required.

## 7. Validation

- Frame-time budget: UI-only screens ≤ 16.6 ms; playback screens hold 60 fps
  VSync cadence (the `rmlui-integration-guide.md` §7 target).
- Composited-output plane-hash parity vs. the current CPU path
  ([validation.md](../validation.md) / `tools/bench.sh`).
- Host preview unchanged (`__PROSPERO__` guard).
- No new panic vectors — AGC submit path is watchdogged like the decode thread
  ([hardware-decode-review.md](../hardware-decode-review.md) §7).

## 8. File-by-file (Steps 1–2)

| Path | Change |
|---|---|
| `ui_rml/src/evo_rmlui_render.cpp` | Step 1: dirty-flag the surface raster |
| `projects/evoplayer/main.c` | Step 1: gate UI raster on change signal |
| `pp/src/pp_agc_present.c` + `pp/src/shaders/*.sb` | **new** — AGC DCB build, shader blobs (adapted from `third_party/ProsperoLight/src/native_agc_present.cpp`) |
| `pp/src/pp_videoout.c` | Step 2: submit AGC DCB instead of CPU-buffer flip on target |
| `pp/src/pp_converter*.c` | Step 2: bypassed on `__PROSPERO__`; kept for host |
| `scripts/build-evoplayer.sh` / Makefile | link `-lSceAgc -lSceAgcDriver`; package shader blobs |
| [converter-perf.md](../converter-perf.md), [gpu-notes.md](../gpu-notes.md) | update the "no GPU path" framing once Step 2 lands |
