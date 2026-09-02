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
| **AGC (`sceAgc` + `sceAgcDriver`)** | **ProsperoLight demonstrates the full pipeline** — `sceAgcCreateShader` / `sceAgcLinkShaders` / register setup / `sceAgcDcbDrawIndexAuto` / `sceAgcDriverSubmitDcb` / `sceAgcDcbSetFlip` — from a game-category app module. This is the route. The ABI, the DCB struct, the render-target register model and a clean-room swizzle library are documented call-by-call in [sharpprospero-agc-reference.md](sharpprospero-agc-reference.md). |

## 3. How ProsperoLight splits the work

References: `third_party/ProsperoLight/src/native_agc_present.cpp` is the
**hardware-proven C++** implementation skeleton;
[sharpprospero-agc-reference.md](sharpprospero-agc-reference.md) is the
**annotated ABI + method spec** (`third_party/SharpProspero/`, git-ignored) —
use both, ProsperoLight for "what runs", SharpProspero for "what every call
means".

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

### Step 1 — dirty-flag the UI surface (CPU only, no GPU) — **DONE 2026-09-02**

Rasterise the RmlUi context into a retained surface **only when it actually
changed**, and blit that surface into the caller's rotating VideoOut buffer
every frame. For a static menu this drops per-frame UI cost to near zero.

Implemented in **`ui_rml/src/evo_rmlui_app.cpp`** (`EvoRmlApp::RenderCachedScreen`),
not `main.c` — the per-frame loop already calls `update` + `render` every
frame; the gate lives one layer down where the `m_frame_dirty` state-diff
already existed (it was previously wired to a `ShouldFullRender` stub that
always returned true). `main.c` is untouched.

- **Scope:** the seven full-screen opaque menu documents — launch, list
  (recent/favorites/emby), browser, settings, changelog, reader, surround.
  The RCSS carries no transitions or animations, so these never change
  between input events. Re-render triggers: `m_frame_dirty` (a real
  state / theme / nav-rail change set it), the visible screen switched, or
  the surface was resized.
- **Not cached:** overlay screens that compose over live video (playback OSD,
  dialog, subtitle picker, media info) still render straight into the
  framebuffer every frame — unchanged behaviour, zero parity risk.
- **Instrumentation:** `projects/evoplayer/ui_rml/include/evo_rmlui_prof.h` +
  `tools/prof_rmlui.{cpp,sh}` — a host frame profiler, compiled in only under
  `-DEVO_RML_PROFILE` (no shipping build defines it). Kept for validating
  Step 2.

**Host profile result (`tools/prof_rmlui.sh`, Docker/WSL — ratios are the
point, not absolute ms):**

| screen | before (idle) | after (idle) | navigation (unchanged) |
|---|--:|--:|--:|
| launch (hero + shelf + library) | 119 ms | **0.8 ms** | 120 ms |
| settings (4-row list) | 30 ms | **0.7 ms** | 31 ms |
| browser (12 rows + inspector) | 65 ms | **0.8 ms** | 68 ms |

`Context::Update` (style + layout) was **0.1 ms** throughout — the entire
frame is the render-interface raster, dominated by the coverage (triangle)
rasterizer (~1.4–2.3 ms per rounded-rect / gradient / non-axis-aligned call).
Glyph rasterisation is **0 ms** in steady state (warm FreeType atlas). The
memcpy Step 1 adds back is 0.45 ms at 1080p.

**Verdict:** Step 1 fixes the stated problem — idle menus were the ~11 fps /
missed-input case (see the `m_frame_dirty` comment in `evo_rmlui_app.h`).
Continuous D-pad scroll is untouched (every frame genuinely dirty); it holds
at ~8–15 fps on this host. Step 2 (AGC) is now only needed for smooth 60 fps
*during* held navigation and for the 4K video convert/composite/flip — not a
blocker for UI responsiveness.

### Step 2 — AGC present + composite (needs the app module)

Lift `native_agc_present.cpp`. Move **YUV→BGRA convert**, the **final
composite**, and the **flip** onto the GPU. The CPU converter
(`pp_converter_*.c`) stops running on the hot path; `pp_videoout.c` submits an
AGC DCB instead of `sceVideoOutSubmitFlip` with a CPU-filled buffer.

#### AGC reachability probe — `projects/agc_probe/` (in progress 2026-09-02)

Gate: can a payload reach `sceAgc` without repackaging as an app module?

- **elfldr payload context: NO.** `-lSceGnmDriver` → the payload SIGSEGVs on
  GNM init (`sce::Gnm::Initialize Error: Get CU Mask Fails (0)`).
  `sceKernelLoadStartModule("/system/common/lib/libSceAgc.sprx")` → **hangs,
  never returns** (left a wedged payload in the elfldr host; console stayed
  fully healthy — websrv/ftp/`/fs` kernel-R-W all responsive, no KP). The
  elfldr host process has no path to the graphics stack.
- **ELF track abandoned** (elfldr *and* hbldr are borrowed-process sandboxes —
  the same context class that never reached the `sceVideodec2` decoder). All
  GPU/decode work now targets the **registered app module `PPSA99039`**, the
  context where ProsperoLight runs the full pipeline.
- **App-module recon:** `scripts/package-app.sh --agc-probe` compiles
  `-DEVO_AGC_PROBE`, which runs `evo_agc_probe()` at `main()` entry —
  `sceKernelLoadStartModule("/system/common/lib/libSceAgc.sprx")` + NID
  resolution from inside `PPSA99039`, reported via the notification popup.
  The app module already boots to the menu, so this answers the Step 2 gate
  **before** task 8 (playback) is fixed. Deploy the current build and read
  the "EVO agc: ..." notification.
- If `libSceAgc` loads and the NIDs resolve → Step 2 is viable: lift
  `native_agc_present.cpp` + `third_party/ProsperoLight/assets/private/*.bin`
  into a new `pp/src/pp_agc_present.c`, built by `package-app.sh`.

- Shaders: ProsperoLight's NV12-sample + fullscreen-quad blobs cover the
  video path. **No PSSL compiler** — but `llvm-mc-18` (in the container)
  assembles/disassembles GCN for `gfx1030`, so a hand-written
  RGBA-passthrough PS (for compositing the RmlUi surface) is feasible.
- Touches: `pp/src/pp_videoout.c`, `pp/src/pp_converter*` (bypass on target),
  new `pp/src/pp_agc.c` + `agc_blobs.S`.
- Guard behind `__PROSPERO__` so the host preview (`tools/uiview*`,
  `uiplay`) keeps the SDL/CPU path — no host regression.
- **Full how-to (blobs disassembled, `render_frame` annotated, the port and
  wiring): [agc-implementation.md](agc-implementation.md).**

### Step 3 — complete UI rendering on the GPU (committed scope, largest)

Bind `Rml::RenderInterface` (`RenderGeometry` / `CompileGeometry` /
`RenderCompiledGeometry` / scissor / clip mask / transform / `GenerateTexture`)
directly to AGC draw calls, so UI triangles rasterise on the GPU too.
**Deletes `evo_rmlui_render.cpp`'s ~2000-line CPU coverage rasteriser.**

The destination: video + UI + composite + flip in one DCB per frame. Held-
scroll smooth, 4K UI native, UI-over-video correct by construction.

- New hand-written shaders (solid-colour, textured, colour-modulated,
  scissored triangles) — assembled with `llvm-mc-18`, headers adapted from
  ProsperoLight's. Bounded set.
- New `ui_rml/src/evo_rmlui_render_agc.cpp`; `evo_rmlui_app.cpp` picks it at
  runtime when `pp_agc_available()`, else the Step 1 CPU path (the fallback).
- Sequenced after Step 2 — which de-risks the shared AGC infrastructure on the
  simplest case first. Full plan: [agc-implementation.md](agc-implementation.md) §5.

## 5. Interaction with native decode

Both tracks are AGC/compute consumers in the app slot and **coexist** —
ProsperoLight runs the `sceVideodec2` compute-queue decoder *and* the `sceAgc`
graphics pipeline in the same process. Ordering rule (carried from
[videodec2-abi.md](videodec2-abi.md) §6): bring the decoder up **after**
VideoOut / AGC init, tear it down **before**. The decoder uses a compute queue
(`sceVideodec2AllocateComputeQueue`); the renderer uses the graphics ring —
separate.

## 6. Before building anything — profile — **DONE 2026-09-02**

`tools/prof_rmlui.sh` (host, no console — links the real drawing code) with
`evo_rmlui_prof.h` instrumentation. Findings:

- **per-frame re-rasterisation vs. genuinely-changed regions:** idle and
  navigation frames cost the same to within 1% — ~100% of the frame is
  re-rasterising an unchanged scene. There is no changed-region path;
  `Context::Render()` redraws the whole document. → Step 1 target.
- **glyph raster vs. atlas hits:** steady-state glyph raster is **0 ms / 0
  calls**. Atlas built once on the first frame after a text change; every
  later frame is pure atlas hits. Not a per-frame cost.
- **fast-path vs. slow path:** the coverage (triangle) rasterizer dominates
  everywhere — 58 ms/launch, 27 ms/settings, 53 ms/browser — at ~1.4–2.3 ms
  per rounded-rect / gradient / non-axis-aligned call. The "fast" AA-quad
  blitter is also heavy on launch (48 ms: hero bicubic magnify + poster
  box-filter + full-screen gradient fills).
- **composite + framebuffer copy:** memcpy 0.45 ms (1080p) / 3.0 ms (4K);
  scalar alpha-composite 3.4 / 13.3 ms (the SIMD `blend_span_constant` path
  roughly halves that).

Result: **Step 1 alone is sufficient for the UI-responsiveness problem** (idle
menus at ~11 fps, missed inputs). Step 2 is required only for 60 fps *during*
continuous scroll and for the 4K playback convert/composite/flip.

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
| `ui_rml/src/evo_rmlui_app.cpp` | Step 1 **DONE** — `RenderCachedScreen`: retained surface, re-raster on change only, blit every frame |
| `ui_rml/include/evo_rmlui_app.h` | Step 1 **DONE** — `m_surface` + members; retired the `ShouldFullRender` stub |
| `ui_rml/{include/evo_rmlui_prof.h,src/evo_rmlui_render.cpp}` + `tools/prof_rmlui.*` | Step 1 **DONE** — host frame profiler, `-DEVO_RML_PROFILE` only |
| `pp/src/pp_agc_present.c` + `pp/src/shaders/*.sb` | **new** — AGC DCB build, shader blobs (adapted from `third_party/ProsperoLight/src/native_agc_present.cpp`) |
| `pp/src/pp_videoout.c` | Step 2: submit AGC DCB instead of CPU-buffer flip on target |
| `pp/src/pp_converter*.c` | Step 2: bypassed on `__PROSPERO__`; kept for host |
| `scripts/build-evoplayer.sh` / Makefile | link `-lSceAgc -lSceAgcDriver`; package shader blobs |
| [converter-perf.md](../converter-perf.md), [gpu-notes.md](../gpu-notes.md) | update the "no GPU path" framing once Step 2 lands |
