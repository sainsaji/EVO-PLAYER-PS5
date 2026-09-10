# OpenGL render overhaul — one funnel for every pixel

> **Status (2026-09-09): GL-1 (#77) PASSED, GL-2 (#78) host pass DONE.** GL-1:
> the GL smoke rendered on FW-12.70 (Mesa 26.2, GL 3.3 Core, PS5 AGC backend,
> pixel-exact) — [gl1-spike.md §7](gl1-spike.md#7-hardware-receipt--go-2026-09-09).
> GL-2: RmlUi's `RenderInterface_GL3` + EVO adapters render every screen in the
> host harness (`UIVIEW_GL=1 uiview.sh --all`) within AA tolerance of the CPU
> rasteriser — [gl2-render-interface.md](gl2-render-interface.md). **GL-3 (#79)
> is unblocked** — device cutover for menu screens. Supersedes the hand-rolled
> `sceAgc` UI/geo route (`#28` closed, `#67`/`#68`/`#69`/`#70` open) and the CPU
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
8. **HDR / P010** — 10-bit sampling + SDR tone-map in the GL video shader.
   GL-4 shipped 8-bit; this moved to **GL-5** (2026-09-10). `#4` narrows to
   its HDR *output* metadata tail.

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
| **GL-2** (#78) ✅ host | RmlUi `RenderInterface_GL3` — host only. Vendored verbatim (`ui_rml/src/rmlui_gl3/`) + `EvoRenderInterfaceGL` adapters (`LoadTexture` bundle/`evo:mem/`/premultiply, `SetMemoryTexture`); geometry / stencil clip-mask / MVP transform / scissor inherited. `EvoRenderBridge` seam so `EvoRmlApp` holds either. Headless EGL-surfaceless + llvmpipe context (`evo_gl_context_host.cpp`). [gl2-render-interface.md](gl2-render-interface.md). | **MET (host).** `UIVIEW_GL=1 uiview.sh --all` renders every screen through GL; ≤0.2% pixels differ from the CPU rasteriser (MSAA on edges — the improvement). Overlays composite correctly. |
| **GL-3** (#79) | GL owns `sceVideoOut` for the **whole session** on device (single owner — `pp_videoout` / `pp_agc` present are removed, not run alongside). Menu screens render + swap through GL. **The player frame's *present* moves here too** — a GL texture blit of `pp/`'s already-CPU-converted RGBA frame + `eglSwapBuffers` — because there is no second flip queue for it. `pp/` keeps decode + the CPU convert + clock + seek. Delete the `m_surface` / `AgcGeoPresent` dual path + `#28` geo sink; retire present routes #2/#3; kill the `#32` scrub-overlay machine; fold in `#49`. Staged B1–B5 — see the [#79 alignment comment](https://github.com/sainsaji/EVO-PLAYER-PS5/issues/79). **B1 + B2 DONE + hw-verified 2026-09-09; B3 landed 2026-09-10 (`8056627`) — functional, not smooth.** B3: player frame present via GL — `pp/` decodes + CPU-converts to its 1080 `display` buffer, `draw_player_screen` composites video + OSD, `evo_gl_blit_bgra` presents it. `pp_product_request_vo()` early-returns on `EVO_GL_DEVICE` (forces `PP_BACKEND_1080_STANDARD`, no VO). 1080p + 4K decode + A/V sync + seek work; **video stutters at ~14 fps — the `glTexSubImage2D` wall, see the GL-4 row.** The `#32` scrub-overlay machine is left in place (inert on GL, would regress non-GL 4K if deleted → GL-6). B1: `--gl` links ps5-opengl and cuts `main()` over to a persistent `evo_gl_context_device.cpp` (EGL window surface, GL 3.3 Core) in place of `pp_agc_init` + the `PP_BACKEND` present path. B2: menu screens present through GL — **but RmlUi's own GL3 backend is ~1.2 s/frame on ps5-opengl G47** (its render-to-texture path is synchronous with a full CPU surface copy per draw; `update=0ms`, all in the draws + MSAA resolve). So the **default device path is CPU coverage rasteriser → one GL blit** (`evo_gl_blit_bgra`, a textured fullscreen quad, `.bgr` swizzle — EVO's rasteriser writes the BGRA VideoOut plane format): **~3 ms dispatch + ~8 ms swap per redraw**, change-gated (`GlNeedsFrame`/`GlSetActive`), ~430 Hz input poll when idle. RmlUi-native-GL3 stays selectable via `/mnt/usb0/evo_gl_rmlui` (panel: Remote tab → GL-3 render path) for when the driver's RTT path is fast. Also: `av_force_cpu_flags(0)` on the GL build — ps5-opengl enabling JIT flips FFmpeg swscale's legacy MMX scaler into reading its loop templates out of execute-only `.text` (`SYSTEM_XO_VIOLATION`, `ff_init_hscaler_mmxext`). | Every non-player screen + the player play through GL on hardware; `#49` items 1–5 landed; `uiview.sh --all` unchanged. |
| **GL-4** (#80) ✅ **CLOSED 2026-09-10** (`c49b317`, hw-verified) | Make the video path efficient, then delete what it replaced. **Stage 1 (G47->G55 bump + `--gl-smoke` upload re-measure) hw-verified 2026-09-10:** the `glTexSubImage2D` staging-copy wall is **RGBA8-only** - R8 / RG8 uploads are free (1080p luma 0.10 ms, chroma 0.03 ms; 4K 0.90 / 0.46 ms), so an NV12 two-plane upload is 0.13 ms/frame (1080p) / 1.36 ms/frame (4K) and **no zero-copy import was needed**. **Stage 2a-c hw-verified 2026-09-10:** zero-copy NV12 (decoder planes -> R8+RG8 textures, no copy) + GLSL YUV->RGB (BT.601 limited, `.bgr`-swizzled for the BGRA scanout) + aspect as a vertex-quad scale (closes `#76`) + OSD composited over the video, sample-hash-gated. GTA 4K native + 1080p smooth, `blit~5ms` (was ~65). **Stage 2d + Stage 3 hw-verified 2026-09-10:** the seek-discard window serves a frozen snapshot of the last published frame (`pp_playback` `hold_buf`, one copy per seek) with the scrub OSD held up over it, replacing the `#32` overlay machine outright; and `pp_converter*.c`, `pp_compute_pipeline.c`, `tile_copy.c`, `pp_product_path.h` + the 4K stage-ladder headers, `bench.sh` / `bench_converter.c`, `pp_videoout`'s CPU present, the 5-way present dispatch, the backend enum, the 4K VO reconfigure machinery and `--no-gl` are all deleted. `#62` became an exhaustive host parity sweep (`tools/gl_yuv_parity.py`, 99.390% bit-exact) which caught a real 16/256-vs-16/255 shader offset error. The hw pass also caught + fixed a subtitle-composite regression and a ~65 ms RGBA8-upload hitch (now RG8), and added a boot shader warm-up. **P010 moved to GL-5 (#81).** `#76` closed. Full plan: [gl4-video-path-plan.md](gl4-video-path-plan.md). | GTA 4K + 1080p through the GL video path real-time; `#62` parity in [validation.md](../validation.md); `#76` fixed; CPU converters + `tile_copy` + the backend enum gone. |
| **GL-5** (#81) | Fold in the strays. GL text pass for subtitles (Unicode + Noto — closes `#35`), keyboard (RmlUi doc), image viewer (textured quad), FPS/HUD overlay (`#63`), **P010 / 10-bit in the GL video shader + SDR tone-map** (moved from GL-4). Delete `draw_char`/`draw_text`, `evo_draw` bitmap font, `evo_widgets`, `evo_keyboard` immediate mode, `pp_map_yuv420p10_to_8`. | Every screen and overlay is GL; three font systems become one; 10-bit plays without the 8-bit pack; `#34`/`#35` closed. |
| **GL-6** | Cutover + demolition. Delete `pp_agc.c` present/geo/osd, `pp_agc_osd.c`, `pp_videoout.c` (GL-4 already took its CPU present; the rest is dead code), `evo_rmlui_render.cpp`, `evo_rmlui_render_agc.cpp`. Rewrite `gpu-notes.md`, `gpu-rendering-plan.md`, `rmlui-integration-guide.md`, `architecture.md`. Close superseded issues. (**Already done by GL-4:** promoting GL to the only build, and the `#32` overlay machine.) | `main.c` well under the modularisation target; one render path; docs match. |

Each story is `blocked-by` the previous. GL-1 → GL-2 can start before GL-1
finishes only for the pure-host adapter work.

## Existing issues this reshapes

| Issue | Change |
|---|---|
| `#67` (composite CPU text over GPU geo) | **Closed / superseded** — GL3 renders text + geometry in one pass |
| `#68` (UI fidelity: curves, shadows, AA) | **Rescoped** — goals kept, mechanism becomes GLSL + RCSS post-overhaul; `blocked-by` GL-3; absorbs `#70` |
| `#69` (4× MSAA color surface + resolve) | **Closed / superseded** — `EGL_SAMPLES` / multisample FBO + `glBlitFramebuffer` replaces the PA_SC register RE |
| `#70` (hand-authored SDF pixel shader) | **Closed / superseded** — becomes a GLSL fragment shader under `#68` |
| `#62` (plane-hash A/B: sceAgc vs CPU) | **Delivered by GL-4** - the CPU converter is deleted, so it became GL-video-shader-vs-CPU-matrix parity: `tools/gl_yuv_parity.py`, written up in [validation.md](../validation.md#gl-video-path-colour-parity-62-delivered-by-gl-4--80) |
| `#5` (speed up CPU colour conversion) | **Closed / superseded** — the CPU converter is deleted |
| `#4` (10-bit / HDR slow path) | **Split** — P010 sampling + SDR tone-map → GL-5 (#81); `#4` keeps only the HDR *output* metadata (`sceVideoOutSetHdrMetadata`) tail |
| `#35` (non-English subtitles → `?`) | Kept; delivered by GL-5 (Unicode GL text pass) |
| `#34` (keyboard crash) | Kept; keyboard moves to RmlUi under GL-5 |
| `#76` (aspect-ratio cycle corrupts 4K) | **Closed by GL-4** - aspect is a vertex-quad scale; `force_v3_fallback` and the linear-vs-tiled attr mismatch are deleted |
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
