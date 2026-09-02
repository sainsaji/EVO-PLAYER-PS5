# Roadmap — implementation order & per-story references

The GitHub milestones (`v0.8.0` → `v0.9.0` → `v1.0.0` → `v1.1.0`) with a
dependency-ordered plan. **Each open issue's body carries its own "References &
sequencing" block** pointing at the exact docs + files to read — this page is
the map across all of them.

To implement a story: open its issue, read the docs it names, then the files it
names, then go.

Priority labels track this order: **high** #27, #25 · **medium** #9, #16, #6,
#29/#30 · **low** the rest. `independent` = no cross-deps, work any time in
parallel. (#26 closed 2026-09-02 — app-module playback works; demanding 4K → #29.)

---

## Dependency graph

```
                 ┌─────────────────── independent, any order, no console ───────────────────┐
                 │  #17 CI/tests   #9 Emby URL   #8 codec metrics   #3 subtitles (meta)      │
                 │  #5 swscale MT  #16 UI text/overflow   #30 evo_vdec.h seam sign-off       │
                 └──────────────────────────────────────────────────────────────────────────┘

  #26 app-module playback ─── CLOSED (works; demanding 4K → #29)

  #6 rotate buffers → direct mem ──────────────┐        (touches pp_videoout; do before #27)
                                               │
  #4 10-bit fast path (CPU stopgap) ───────────┤        (real fix is #27; optional)
                                               ▼
  system-PRX import stubs ──┬──► #27 GPU Step 2: sceAgc convert+present ──► #28 Step 3
   (app module can't runtime-  │      (libSceAgc + libSceAgcDriver)            RmlUi on sceAgc
    load an undeclared .sprx;  ├──► #29 Route A: libSceAvPlayer   ┐
    proven on hw 09-02/09-03)  └──► #29 Route B: libSceVideodec2  ┘ native decode spike

  #25 RmlUi migration (umbrella) ── sign off before #28 replaces the renderer

  #29 native hw decode (umbrella, v1.1.0)
    └─ #30 Phase 3: finish the evo_vdec.h seam  ── SIGNED OFF 09-03 (parity sweep owed)
       └─ PRX stubs → Phase 2 spike → Phase 4 native backend → Phase 5 toggle → Phase 6
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
| **#30** | Finish + sign off the `evo_vdec.h` decoder seam (Phase 3 of #29) | `docs/evo-pro/native-decode-plan.md` §3, `docs/modularisation-plan.md` Track A, `docs/validation.md`; `media/include/evo_vdec.h`, `media/src/evo_vdec_ffmpeg.c`, `main.c` thumbnail decoders, `tools/bench.sh` |

### 1 · `#26` — app-module playback crash — **CLOSED 2026-09-02**

1080p + reasonable 4K play in `PPSA99039`; demanding 4K degrades gracefully
(toast, no crash). Fixes: `55685aa0` (posix_fadvise SIGSYS), `d84d05c`
(flexible-memory allocator), `bb80de1` (slice threading + fatal-decode abort).
Demanding-4K playback needs native decode → tracked under **#29**.

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

**Prerequisite (hardware 2026-09-02):** `sceKernelLoadStartModule("libSceAgc.sprx")`
is refused from the app module — a fake-signed module can only load PRXes it
declares NEEDED. **A `libSceAgc` / `libSceAgcDriver` import stub must be added to
the app-module link first** (`tools/native-app/` + `scripts/package-app.sh`);
NID list + encoder are in `evo_agc_probe.c` (verified vs `prospero-nid`),
export-table layout in SharpProspero `tools/SharpProspero.Prx/`. Only then does
the AGC gate (`EVO agc: … VIABLE`) get a real answer. The big one.

- Reads: **`docs/evo-pro/agc-implementation.md`** (§0 what's proven, §1 shaders, §3 `render_frame` annotated, §4 the port), `docs/evo-pro/gpu-rendering-plan.md`, `docs/evo-pro/sharpprospero-agc-reference.md`, `docs/evo-pro/videodec2-abi.md` §6 (AGC/decoder ordering)
- Files: `third_party/ProsperoLight/src/native_agc_present.cpp` (+ `assets/private/*.bin`), `pp/src/pp_videoout.c`, `pp/src/pp_playback.c`, `projects/evoplayer/src/evo_agc_probe.c` (runtime NID resolution pattern), `tools/build-shader.sh`, `pp/shaders/rgba_ps.s`

### 5 · `#28` — GPU Step 3: complete RmlUi on sceAgc

Blocked by #27 (it reuses all of #27's plumbing). Deletes the CPU rasteriser.

- Reads: `docs/evo-pro/agc-implementation.md` §5, `docs/evo-pro/gpu-rendering-plan.md`, SharpProspero `Graphics/Agc/` (`Renderer3D`, `CxRenderTarget`, `AgcRenderTargetSetup`, `AgcBufferDescriptor` — the register model to transcribe)
- Files: `ui_rml/src/evo_rmlui_render.cpp` + `.h` (the interface being replaced), `ui_rml/src/evo_rmlui_app.cpp`, `pp/shaders/`, `tools/build-shader.sh`, `tools/prof_rmlui.sh` (parity check)

### 6 · `#29` / `#30` — native hardware decode *(v1.1.0)*

`sceVideodec2` from the app module. Phase 1 gate PASSED on hardware
(2026-09-01); the decoder seam (`evo_vdec.h`) is ~80 % built. **Start with
`#30`** — Phase 3, finishing + signing off the seam: `independent`, no console,
ships regardless of whether native decode ever lands.

- Reads: **`docs/evo-pro/native-decode-plan.md`** (the full 9-phase plan, §3 architecture, kill criteria §8), `docs/evo-pro/videodec2-abi.md`, `docs/modularisation-plan.md` (Track A), `docs/evo-pro/status.md`
- Files: `media/include/evo_vdec.h`, `media/src/evo_vdec_ffmpeg.c`, `media/include/sce/sce_videodec2.h`, `main.c` (`start_video_playback` + the two thumbnail decoders), `third_party/ProsperoLight/src/moonlight_stream.cpp` (hardware-verified reference)
- Sequencing: `#30` now (parallel to the GPU track) → Phase 2 native spike (needs console, timeboxed) → Phase 4 `evo_vdec_native.c` → Phase 5 settings toggle + probe → Phase 6 validation. Depends on #26 (app-module playback milestone).

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
