# OpenGL render overhaul — one funnel for every pixel

> **Status (2026-09-09): GL-1 (#77) PASSED — go/no-go is GO.** The GL smoke
> rendered on the FW-12.70 console (Mesa 26.2, GL 3.3 Core, GLSL 330, PS5 AGC
> backend, pixel-exact readback, no driver fail-stop) —
> [gl1-spike.md §7](gl1-spike.md#7-hardware-receipt--go-2026-09-09). `ps5-opengl`
> vendored as a submodule, `_Exit` patch + allocator + pre-unjail settled, the
> from-source SDK builds, and `package-app.sh --gl-smoke` links + signs a
> `.ffpfsc`. **GL-2 (#78) is unblocked.** Supersedes the hand-rolled `sceAgc`
> UI/geo route (`#28` closed, `#67`/`#68`/`#69`/`#70` open) and the CPU
> converter/present stack. Stories: the **`render-overhaul`** label
> (`GL-1` … `GL-6`).

## Why

EVO's rendering is spread across **five present routes**, **three font
systems**, **two CPU YUV→RGB converters plus a GPU one**, and a CPU coverage
rasteriser with a second "divert geometry to the GPU and composite a CPU text
layer back over it" path bolted beside it. Every screen-state interaction
(`#76` is the canonical example — aspect-ratio cycle corrupts the screen
because four subsystems disagree about linear-vs-tiled) has to be reasoned
about across all of them. It is the single biggest tax on working in this
codebase.

The fix is not another special case. It is **one funnel**: everything visible
is either an OpenGL draw call or an OpenGL texture, and `ps5-opengl` (Mesa +
a PS5 Gallium driver + a patched PSSL compiler) is the only thing that talks
to `sceAgc` / `sceVideoOut`.

```
decode (CPU) ─ NV12/P010 ─┐
                          ├─► GL:  video quad  (YUV→RGB fragment shader)
RmlUi Context ── GL3 ──────┤        + UI pass  (menus / OSD / subtitles / keyboard)
  (all screens + overlays) │        + debug pass (fps / HUD)
                          └─► eglSwapBuffers ─► ps5-opengl ─► sceAgc DCB + sceVideoOut flip
```

`pp/` collapses to decode + pace + clock. `ui_rml/` keeps `bridge` + `app` +
**one** render interface. `main.c` loses the frame loop's present dispatch,
both bitmap fonts, the subtitle rasteriser, the image-viewer blitter, and the
`#32` scrub-overlay state machine.

## What `ps5-opengl` is

| Layer | Detail |
|---|---|
| Frontend | pinned Mesa + `toolchain/mesa-ps5.patch` — GL 3.3 Core, GLSL 3.30, NIR |
| Driver | bespoke `src/gallium/ps5/` (**not** radeonsi/DRI) → `sceAgc` |
| Shader compiler | patched OpenGNM PSBC — GLSL/SPIR-V → PS5 machine code, offline **and** runtime |
| Native backend | `src/platform/` — `dlopen("libSceAgc.sprx" / "libSceAgcDriver.sprx" / "libSceVideoOut.sprx")`, `sceAgcInit`, owns the `sceVideoOut` flip queue |
| Heap | app-owned 128 MiB process-lifetime heap + `--wrap=malloc,calloc,realloc,free,posix_memalign,malloc_usable_size` |
| Validation FW | **6.02 research console** (EVO is 12.70 — see risks) |
| Perf (its own numbers) | small windowed scene ~119 fps @ 1080/1440/4K; frozen ImGui demo ~20 fps; render-to-texture 3.5 → 60 fps depending on format |

It is the **same `sceAgc` route** `#27`/`#28` already committed to — the win is
that Mesa's PSBC path compiles ordinary GLSL to working PS5 shaders, which
sidesteps the hand-assembled-RDNA2 / `sl00`-trailer / `0x8a6c001f` wall that
`#69`/`#70` are stuck against.

## Complete pixel-path inventory

### Moves into the GL funnel

| Path | Today | Under GL |
|---|---|---|
| RmlUi menu screens | `EvoRenderInterface` CPU coverage rasteriser, 1450 lines (`evo_rmlui_render.cpp`) | RmlUi upstream `RenderInterface_GL3` (~1k lines) |
| RmlUi `#28` AGC geo sink | `EvoAgcGeoSink` → hand-rolled DCB (`evo_rmlui_render_agc.cpp`) | **deleted** — GL3 backend replaces it |
| RmlUi toast overlay (`#75`) | own `Rml::Context`, same CPU rasteriser (`evo_toast.c`) | same context, GL3 backend |
| "native rendering rendered separately" = `AgcGeoPresent` / `RenderCachedScreen` dual path (`evo_rmlui_app.cpp:96–166`) | CPU surface cache **plus** a second path that diverts geometry to the AGC sink and composites `m_surface` as a "text layer" | one path: RmlUi renders to a GL FBO / default framebuffer. No `m_surface`, no divert, no text-layer composite |
| Video convert (NV12 / YUV420P → RGBA) | `pp_converter_fused.c` / `_parallel.c` CPU SIMD, or `pp_agc.c` partial GPU | one GLSL fragment shader sampling NV12/P010, fullscreen quad |
| Tile swizzle (`tile_copy.c`, linear→tiled) | CPU per frame on non-V8 paths | gone — GL writes the tiled RT directly |
| UI-over-video composite | CPU byte copy into `linear`, or `pp_agc_osd` decode-thread composite, or `pp_agc_present_geo` | GL: video quad → UI pass → flip, one DCB |
| Present / flip | 5 routes in the frame loop | one: `eglSwapBuffers` → `ps5-opengl` drives `sceAgcDcbSetFlip` + `sceVideoOut` |
| Subtitles | `prospero_subtitle_draw` → bitmap `draw_text`, ASCII-only (`#35`) | GL text pass (**is** the `#35` fix) |
| Virtual keyboard modal | `evo_keyboard.c` immediate-mode `evo_text` over `linear` — the only keyboard (native IME crashes, `#34`) | RmlUi document, or GL text |
| FPS / stats-for-nerds overlay | `draw_fps_overlay` → `evo_text` | GL text / RmlUi debug element |
| Photo / image viewer | `draw_image_screen` + `draw_bmp_to_fb` — CPU BMP decode + nearest-scale blit, outside RmlUi | GL textured quad |
| Marquee scrolling text | `evo_text_marquee` + manual bleed save/restore hack; parallel C++ marquee tick in `evo_rmlui_app.cpp` | RCSS `text-overflow` / transform animation — the hack dies |
| Cover art / poster / hero art | decoded to RGBA, fed as `SetMemoryTexture` | native `glTexImage2D` upload |
| Screenshot | reads `linear` (`evo_screenshot_write`) — on the 4K V8 path `linear` **does not contain the video** | `glReadPixels` of the composited framebuffer — fixes the 4K screenshot |

### Stays CPU (feeds GL as data, not pixels)

- **Video decode** — `sceVideodec2` / FFmpeg. Output stays NV12/YUV; GL samples it.
- **Demux, audio, clocks** — untouched.
- **swscale decode+downscale** for browser preview & scrub thumbnail
  (`prospero_thumbnail.c`, `prospero_browser_preview_*`) — low frequency; keep
  on CPU, upload the result as a GL texture. (Could become a GL blit later; not
  first-pass work.)
- **RmlUi layout / style** (`Context::Update`) — already 0.1 ms.
- **Theme tables** (`evo_theme.c`) — data driving RCSS properties.

### Dies entirely

`pp_converter_fused.c`, `pp_converter_parallel.c`, `pp_compute_pipeline.c`
(misnamed — CPU SIMD), `tile_copy.c`, `evo_rmlui_render.cpp` (CPU rasteriser +
clip-mask), `evo_rmlui_render_agc.cpp`, `pp_agc_osd.c`, the `draw_char` /
`draw_text` bitmap font in `main.c`, most of `evo_draw.c` +
`evo_font_charset.h`, `evo_widgets.c` (already mostly dead post-`#44`), the
`m_surface` cache machinery, `PP_BACKEND_4K_V8_FUSED` / `_V3_FALLBACK` /
`_1080_STANDARD` branching, the 5-way present dispatch, `pp_product_overlay_enter`
/ `_leave` + the `#32` `prospero_scrub_ovl_state` machine (the OSD composites in
GL over the held 4K frame — no 1080-VO drop, so the workaround is unnecessary),
`pp_videoout.c`'s linear-vs-tiled attr juggling and the `013_AGC_VO_RETILE`
recovery path.

## Things the first pass must not miss

1. **A third bitmap font** lives in `main.c` (`draw_char` / `draw_text`,
   ~L2661) — powers subtitles + FPS overlay, separate from both other font
   systems.
2. **The virtual keyboard is load-bearing and fully immediate-mode** — native
   IME crashes (`#34`), so `evo_keyboard.c` is the only text entry. It must
   move or text entry is lost.
3. **The photo/image viewer** (`draw_image_screen`, `draw_bmp_to_fb`) never
   went through RmlUi at all.
4. **Screenshot semantics change** to `glReadPixels`; this is where the
   4K-path screenshot bug (video not in `linear`) gets fixed for free.
5. **The `#32` scrub-overlay machine + `pp_product_overlay_enter/leave`** exist
   only because the OSD can't composite over the 4K plane on CPU. A GL
   compositor deletes the whole subsystem — a simplification, not a port.
6. **`evo_text_marquee`'s bleed save/restore hack** and the parallel C++
   marquee tick both collapse into RCSS.
7. **VideoOut buffer management** — `ps5-opengl` owns the flip queue, so the
   attr flip, 3-buffer rotation, and AGC-death retile in `pp_videoout.c` go
   away.
8. **HDR / P010** (`#4`) — the GL video shader is where 10-bit sampling and
   tone-mapping live; design the shader interface with a 10-bit path in mind.

## Risks / decisions (settle before GL-2)

- **Dual `sceAgc` / `sceVideoOut` ownership.** `ps5-opengl` calls `sceAgcInit`
  and owns the flip queue; so does `pp_agc.c`. `pp_agc.c`'s present path must be
  **removed**, not run alongside. This entangles the overhaul with `#27`/`#28`
  — it replaces that work rather than building beside it.
- **`_Exit(EXIT_FAILURE)` inside `ps5-opengl`'s Gallium driver** on submit
  errors (`src/gallium/ps5/ps5_screen.c` ×3, `src/platform/…` ×1) — violates
  the "never `_exit()` from the app module" rule. **RESOLVED (GL-1):**
  `patches/ps5-opengl/0001-recoverable-fail.patch` routes all four through a
  weak `ps5gl_fatal()`; EVO's strong override (`pp/src/pp_gl_fatal.c`) logs +
  `longjmp`s to a fence or parks, plus a `--wrap=_Exit` backstop.
  See [gl1-spike.md §2](gl1-spike.md#2-_exit--recoverable).
- **FW 6.02 → 12.70.** **RESOLVED (GL-1):** the GL smoke (EGL + GL 3.3 clear +
  triangle + `glReadPixels`) compiled into EVO's own `.ffpfsc` (`--gl-smoke`)
  rendered on the 12.70 console — `result=PASS`, `renderer="PS5 AGC"`,
  `gl="3.3 (Core Profile) Mesa 26.2.0"`, pixel-exact. See
  [gl1-spike.md §7](gl1-spike.md#7-hardware-receipt--go-2026-09-09).
- **Allocator.** **RESOLVED (GL-1):** EVO's `malloc_shim` already wraps the
  mandatory set and draws from the full flexible-memory budget, so it wins —
  `ps5-opengl`'s `app_heap.c` is **not** linked.
  See [gl1-spike.md §3](gl1-spike.md#3-allocator-coexistence--malloc_shim-wins).
- **Pre-unjail sequencing.** **RESOLVED (GL-1):** GL init slot is `main.c` ~L12100,
  immediately before `pp_agc_init`, inside `#if defined(EVO_APP_MODULE)`. GL
  cannot be lazily initialised on first UI draw.
  See [gl1-spike.md §4](gl1-spike.md#4-pre-unjail-init-slot--confirmed).
- **Build.** **DECIDED (GL-1):** submodule + build from source via
  `scripts/build-ps5-opengl.sh`, needing an opt-in toolchain overlay
  (`Dockerfile.ps5-opengl`: Clang 21.1.8 / Meson 1.10.1 / glslang / spirv-tools
  on top of the pinned base). Prebuilt `ps5-opengl-core33` archive is the
  fallback (`--frozen-archive`). See [gl1-spike.md §5](gl1-spike.md#5-build-toolchain-gap).
- **Perf floor for change-frames.** Menu re-raster is change-only (RmlUi
  retained mode + `RenderCachedScreen`), so a slow readback on a state change
  is tolerable; the every-frame paths (video, OSD, subtitles, scrub) must be
  real GPU passes, never readback.

## Phasing → stories (`render-overhaul` label)

| Story | Scope | Gate |
|---|---|---|
| **GL-1** (#77) ✅ | Spike + vendor + build. Submodule `ps5-opengl`; from-source SDK builds; GL smoke inside EVO's `.ffpfsc` (`--gl-smoke`) **rendered on 12.70** — Mesa 26.2 / GL 3.3 Core / PS5 AGC, pixel-exact. Allocator + `_Exit` + pre-unjail resolved. [gl1-spike.md](gl1-spike.md). | **GO.** Overhaul proceeds. |
| **GL-2** | RmlUi `RenderInterface_GL3` — host only. Adapt the upstream backend: `SetMemoryTexture` → `glTexImage2D`, clip-mask → stencil, `GenerateTexture`/`LoadTexture`. Prove in `tools/uiview_playback_rml` against `ps5-opengl`. | `uiview.sh --all` renders every screen through GL, byte-comparable to the CPU rasteriser within AA tolerance. |
| **GL-3** | GL owns the framebuffer for **menu (non-player) screens** on device. EGL surface, GL clear + swap. Delete the `m_surface` / `AgcGeoPresent` dual path and the `#28` geo sink. Player stays on `pp/` for now. | Menus render on hardware through GL; one present route retired; `#49` seam work lands in the same pass. |
| **GL-4** | Video into the funnel. Decoded NV12/P010 → GL texture → YUV→RGB fragment shader → composite UI → single flip. Delete `pp_converter_fused/_parallel`, `pp_compute_pipeline`, `tile_copy`, the CPU present path, the 5-way dispatch, the V8/V3/1080 backend enum. Aspect-ratio math moves into the vertex quad (closes `#76`). | GTA 4K + 1080p play through GL; `#62` parity check vs a reference frame; `#76` fixed. |
| **GL-5** | Fold in the strays. GL text pass for subtitles (Unicode + Noto — closes `#35`), keyboard (RmlUi doc), image viewer (textured quad), FPS/HUD overlay (`#63`). Delete `draw_char`/`draw_text`, `evo_draw` bitmap font, `evo_widgets`, `evo_keyboard` immediate mode. | Every screen and overlay is GL; three font systems become one; `#34`/`#35` closed. |
| **GL-6** | Cutover + demolition. Promote GL to the default build. Delete `pp_agc.c` present/geo/osd, `evo_rmlui_render.cpp`, `evo_rmlui_render_agc.cpp`, the `#32` overlay machine, `pp_videoout.c` attr juggling. Rewrite `gpu-notes.md`, `gpu-rendering-plan.md`, `rmlui-integration-guide.md`, `architecture.md`. Close superseded issues. | `main.c` well under the modularisation target; one render path; docs match. |

Each story is `blocked-by` the previous. GL-1 → GL-2 can start before GL-1
finishes only for the pure-host adapter work.

## Existing issues this reshapes

| Issue | Change |
|---|---|
| `#67` (composite CPU text over GPU geo) | **Closed / superseded** — GL3 renders text + geometry in one pass |
| `#68` (UI fidelity: curves, shadows, AA) | **Rescoped** — goals kept, mechanism becomes GLSL + RCSS post-overhaul; `blocked-by` GL-3; absorbs `#70` |
| `#69` (4× MSAA color surface + resolve) | **Closed / superseded** — `EGL_SAMPLES` / multisample FBO + `glBlitFramebuffer` replaces the PA_SC register RE |
| `#70` (hand-authored SDF pixel shader) | **Closed / superseded** — becomes a GLSL fragment shader under `#68` |
| `#62` (plane-hash A/B: sceAgc vs CPU) | **Rescoped** — the CPU converter is deleted; becomes GL-video-path parity vs a reference frame, under GL-4 |
| `#5` (speed up CPU colour conversion) | **Closed / superseded** — the CPU converter is deleted |
| `#4` (10-bit / HDR slow path) | **Rescoped** — P010 sampling + tone-map in the GL video shader; `blocked-by` GL-4 |
| `#35` (non-English subtitles → `?`) | Kept; delivered by GL-5 (Unicode GL text pass) |
| `#34` (keyboard crash) | Kept; keyboard moves to RmlUi under GL-5 |
| `#76` (aspect-ratio cycle corrupts 4K) | Kept; root causes removed by GL-4 — fix independently only if it lands first |
| `#49` (consolidate the RmlUi UI seam) | Sequenced into GL-3; "Not in scope: delete `evo_rmlui_render.cpp` → `#28`" updated to point at GL-6 |
| `#63` (Diagnostic HUD graphs) | Kept; implemented as the GL/RmlUi overlay from GL-5 |

## References

- `third_party/ps5-opengl/` — `docs/architecture.md`, `docs/consumer-build.md`,
  `docs/limitations.md`, `native-app/README.md`, `src/platform/ps5_agc_native_runtime.c`
- `third_party/ps5-gpu-research/` — FW-6.02 scope, shader toolchain notes
- `docs/gpu-notes.md` — the "no hardware GL path" claim this supersedes
- `docs/evo-pro/gpu-rendering-plan.md` — the hand-rolled `sceAgc` plan (`#27`/`#28`)
- `docs/evo-pro/agc-implementation.md` — the DCB / shader-wall context
- `docs/modularisation-plan.md` — Track B; the render code leaving `main.c`
- `projects/evoplayer/main.c` — frame loop (`~L12247–13332`), present dispatch,
  `draw_char`/`draw_text` (`~L2661`), `prospero_subtitle_draw` (`~L5161`),
  `draw_image_screen` (`~L3059`)
- `projects/evoplayer/pp/` — the converters, `pp_agc.c`, `pp_videoout.c`, `pp_playback.c`
- `projects/evoplayer/ui_rml/src/` — `evo_rmlui_render.cpp`,
  `evo_rmlui_render_agc.cpp`, `evo_rmlui_app.cpp`
