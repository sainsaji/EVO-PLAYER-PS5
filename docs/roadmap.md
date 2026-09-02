# Roadmap — implementation order & per-story references

The GitHub milestones (`v0.8.0` → `v0.9.0` → `v1.0.0`) with a dependency-ordered
plan. **Each open issue's body carries its own "References & sequencing" block**
pointing at the exact docs + files to read — this page is the map across all of
them.

To implement a story: open its issue, read the docs it names, then the files it
names, then go.

---

## Dependency graph

```
                 ┌─────────────────── independent, any order, no console ───────────────────┐
                 │  #17 CI/tests   #9 Emby URL   #8 codec metrics   #3 subtitles (meta)      │
                 │  #5 swscale MT  #16 UI text/overflow                                       │
                 └──────────────────────────────────────────────────────────────────────────┘

  #26 app-module playback crash  ──────────────┐        (needs console; gates the GPU track)
                                               │
  #6 rotate buffers → direct mem ──────────────┤        (touches pp_videoout; do before #27)
                                               │
  #4 10-bit fast path (CPU stopgap) ───────────┤        (real fix is #27; optional)
                                               ▼
  #27 GPU Step 2: sceAgc video convert+present  ──────►  #28 GPU Step 3: RmlUi on sceAgc,
                                                          delete the CPU rasteriser

  #25 RmlUi migration (umbrella) ── sign off before #28 replaces the renderer
```

---

## Order

### 0 · Independent — start any time, in parallel, no hardware

Tagged `independent`. No cross-dependencies; each touches an isolated subsystem.

| # | Story | Reads |
|---|---|---|
| **#17** | CI: unit tests, coverage, Sonar | `docs/tooling.md`, `docs/validation.md`, `docs/converter-perf.md`; `.github/workflows/`, `tools/bench.sh`, `tools/prof_rmlui.sh` |
| **#9** | Emby shows raw stream URL, not the title | `docs/addons-emby-nuvio.md`; `projects/evoplayer/addons/src/addon_emby.c`, `ui_rml` list rendering (`evo_rmlui_bridge.cpp` / `evo_rmlui_app.cpp` list path) |
| **#8** | Codec-sweep decode latency / drop metrics | `docs/converter-perf.md`, `docs/hardware-decode.md`, `docs/validation.md`; `pp/include/pp_pipeline_metrics.h`, `main.c` perf counters + `EVO_DIAG_FPS`, `projects/*_test/` |
| **#3** | Subtitle subsystem + precise overlay timing (meta) | `docs/validation.md`; `media/src/evo_subtitle.c`, `assets/rml/subtitles.rml`, `main.c` subtitle picker/overlay path |
| **#5** | Multi-thread the swscale fallback | `docs/converter-perf.md`; `media/src/evo_playback.c` (swscale fallback), `pp/src/pp_converter_parallel.c` (persistent-pool pattern) |
| **#16** | Text clamping / overflow / title collisions | `docs/rmlui-integration-guide.md`, `docs/theming.md`; `assets/rml/*.rcss`, `ui_rml/src/evo_rmlui_render.cpp` (text path), `tools/uiview.sh` to check every screen |

### 1 · `#26` — app-module playback crash *(needs console — do first of the hardware track)*

Gates everything in `v1.0.0`. Diagnostics already shipped; one console launch
names the failing call.

- Reads: `docs/evo-pro/status.md` (Branch B + the checklist), `docs/evo-pro/phase-1b-app-module.md` §8, `docs/validation.md`
- Files: `main.c` `start_video_playback` + the `EVO_P8()` macro, `pp/src/pp_stage_breadcrumb.c`, `tools/native-app/stubs/malloc_shim.c`, `media/src/evo_vdec_ffmpeg.c`
- Procedure: `scripts/package-app.sh --agc-probe --ffpfsc` → `scripts/deploy-app.sh --ffpfsc` → launch → read the last `P8_*` / `P8_AVLOG` notification before the crash.

### 2 · `#6` — rotate buffers → direct memory *(before #27)*

Small, memory hygiene, and it touches `pp_videoout.c` which #27 rewrites — land
it first to avoid a merge tangle.

- Reads: `docs/improvements-roadmap.md` §P2, `docs/converter-perf.md` Finding 7
- Files: `media/src/evo_direct_mem.c`, `pp/src/pp_videoout.c`, `pp/src/pp_playback.c`, `main.c` (`VIDEO_ROTATE_BUFFERS`)

### 3 · `#4` — 10-bit fast path *(optional CPU stopgap)*

The real fix is #27 (GPU P010 shader). Only do a CPU-side improvement here if
#27 slips.

- Reads: `docs/converter-perf.md`, `docs/hardware-decode.md`, `docs/evo-pro/gpu-rendering-plan.md`
- Files: `main.c` `start_video_playback` (`is10` / `bpp>8` gating), `pp/src/pp_v8_gate.c`, `pp/src/pp_compute_pipeline.c`

### 4 · `#27` — GPU Step 2: sceAgc video convert + present

Blocked by #26 + the AGC gate (`EVO agc: … VIABLE`). The big one.

- Reads: **`docs/evo-pro/agc-implementation.md`** (§0 what's proven, §1 shaders, §3 `render_frame` annotated, §4 the port), `docs/evo-pro/gpu-rendering-plan.md`, `docs/evo-pro/sharpprospero-agc-reference.md`, `docs/evo-pro/videodec2-abi.md` §6 (AGC/decoder ordering)
- Files: `third_party/ProsperoLight/src/native_agc_present.cpp` (+ `assets/private/*.bin`), `pp/src/pp_videoout.c`, `pp/src/pp_playback.c`, `projects/evoplayer/src/evo_agc_probe.c` (runtime NID resolution pattern), `tools/build-shader.sh`, `pp/shaders/rgba_ps.s`

### 5 · `#28` — GPU Step 3: complete RmlUi on sceAgc

Blocked by #27 (it reuses all of #27's plumbing). Deletes the CPU rasteriser.

- Reads: `docs/evo-pro/agc-implementation.md` §5, `docs/evo-pro/gpu-rendering-plan.md`, SharpProspero `Graphics/Agc/` (`Renderer3D`, `CxRenderTarget`, `AgcRenderTargetSetup`, `AgcBufferDescriptor` — the register model to transcribe)
- Files: `ui_rml/src/evo_rmlui_render.cpp` + `.h` (the interface being replaced), `ui_rml/src/evo_rmlui_app.cpp`, `pp/shaders/`, `tools/build-shader.sh`, `tools/prof_rmlui.sh` (parity check)

### Ongoing · `#25` — RmlUi migration (umbrella)

Not a single PR. Screens are built; the remaining work is parity sign-off vs
`main` and folding in #16. **Must be signed off before #28** replaces the
render interface.

- Reads: `docs/rmlui-integration-guide.md`, `docs/ui-handoff.md`, `docs/theming.md`, `docs/icon-swap-handoff.md`
- Files: `projects/evoplayer/ui_rml/`, `projects/evoplayer/assets/rml/`

---

## Done (for context)

- **RmlUi Step 1** — surface cache, re-raster only on change. Shipped, hardware-
  verified (idle menus 11 → ~60 fps). Commit `b3d00ac`. Stays as the
  AGC-unavailable / host fallback under #28.
- **App-module self-unjail** — `evo_jailbreak.c`, Lapy/etaHEN file-drop. Covers
  `/mnt/usb0` + `/data`. Implemented; hardware-confirmed as part of #26's launch.
