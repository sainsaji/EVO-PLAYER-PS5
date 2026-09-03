# Roadmap — implementation order & per-story references

The GitHub milestones (`v0.8.0` → `v0.9.0` → `v1.0.0` → `v1.1.0`) with a
dependency-ordered plan. **Each open issue's body carries its own "References &
sequencing" block** pointing at the exact docs + files to read — this page is
the map across all of them.

To implement a story: open its issue, read the docs it names, then the files it
names, then go.

Priority labels track this order: **critical** #27 (render_frame ported + wired
09-03, awaiting a first hardware run) · **high** #25, #32 · **medium** #9, #16,
#6, #29/#30, #33, #34 · **low** the rest. `independent` = no cross-deps, work
any time in parallel. (#26 closed 2026-09-02 — app-module playback works;
demanding 4K → #29. #31 closed 2026-09-03 — native 4K decode plays.)

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
  positional PRX import stubs ─┬─► #27 GPU Step 2: sceAgc convert+present ──► #28 Step 3
   (package-app.sh step 6b,      │   gate ✅ + pp_agc_init ✅ hw; render_frame ported
    unconditional DT_NEEDED)      │   + pp_playback V8 wired 09-03 — awaiting a hw run
                                 │   (same wall also blocks #34 native IME keyboard)
                                 └─► #29 Route B: libSceVideodec2 ✅ #31 CLOSED — GTA 4K
                                     plays real-time on native decode (2026-09-03)

  #25 RmlUi migration (umbrella) ── sign off before #28 replaces the renderer

  #29 native hw decode (umbrella, v1.1.0)
    └─ #30 Phase 3: finish the evo_vdec.h seam  ── SIGNED OFF 09-03 (parity sweep owed)
       └─ #31 Phase 4 ✅ CLOSED → #32 seek freeze (high) → Phase 5 toggle → Phase 6
```

---

## Order

### 0 · Independent — start any time, in parallel, no hardware

Tagged `independent`. No cross-dependencies; each touches an isolated subsystem.

| # | Story | Reads |
|---|---|---|
| **#17** | CI: unit tests, coverage, Sonar | `docs/tooling.md`, `docs/validation.md`, `docs/converter-perf.md`; `.github/workflows/`, `tools/bench.sh`, `tools/prof_rmlui.sh` |
| **#33** | Clean up the unattended hw-test harness — **partly done**: `tools/evo-remote.sh` (scriptable `play`/`seek`/`watch` over FTP) + `src/evo_usb_remote.c` (`-DEVO_USB_REMOTE`) replaced the compile-time autoplay; `note()` USB log gated to `-DEVO_VDEC_LOG`; payload build now invalidates the app cflags stamp. Remaining: fully hands-off launch (shsrv), `app_ctl` launch fix | `tools/evo-remote.sh`, `src/evo_usb_remote.c`, `scripts/package-app.sh` (`--usb-remote`) |
| **#9** | Emby shows raw stream URL, not the title | `docs/addons-emby-nuvio.md`; `projects/evoplayer/addons/src/addon_emby.c`, `ui_rml` list rendering (`evo_rmlui_bridge.cpp` / `evo_rmlui_app.cpp` list path) |
| **#8** | Codec-sweep decode latency / drop metrics | `docs/converter-perf.md`, `docs/hardware-decode.md`, `docs/validation.md`; `pp/include/pp_pipeline_metrics.h`, `main.c` perf counters + `EVO_DIAG_FPS`, `projects/*_test/` |
| **#3** | Subtitle subsystem + precise overlay timing (meta) | `docs/validation.md`; `media/src/evo_subtitle.c`, `assets/rml/subtitles.rml`, `main.c` subtitle picker/overlay path |
| **#5** | Multi-thread the swscale fallback | `docs/converter-perf.md`; `media/src/evo_playback.c` (swscale fallback), `pp/src/pp_converter_parallel.c` (persistent-pool pattern) |
| **#34** | App module crashes when the native PS5 IME keyboard is opened (directory search). Same `sceKernelLoadStartModule`/undeclared-PRX wall as #27/#31 — `libSceImeDialog`/`libSceCommonDialog` aren't linked or PRX-stubbed for `PPSA99039`. Stopgap: force the virtual keyboard in the app module | `projects/evoplayer/ui/src/evo_keyboard.c`, `ui/include/evo_ime_dialog.h`, `main.c` (~L12504 search, ~L7581 kb toggle), `scripts/package-app.sh` (steps 5 + 6b), `tools/native-app/stubs/prx/README.md` |
| **#16** | Text clamping / overflow / title collisions | `docs/rmlui-integration-guide.md`, `docs/theming.md`; `assets/rml/*.rcss`, `ui_rml/src/evo_rmlui_render.cpp` (text path), `tools/uiview.sh` to check every screen |
| **#30** | Finish + sign off the `evo_vdec.h` decoder seam (Phase 3 of #29) — **signed off 09-03**, parity sweep owed | `docs/evo-pro/native-decode-plan.md` §3, `docs/modularisation-plan.md` Track A, `docs/validation.md`; `media/include/evo_vdec.h`, `media/src/evo_vdec_ffmpeg.c`, `main.c` thumbnail decoders, `tools/bench.sh` |
| **#31** | Phase 4 — `evo_vdec_native.c`, `sceVideodec2` backend behind `evo_vdec.h` (Route B **proven on hw 09-03**) | `docs/evo-pro/status.md` (cold-start plan), `docs/evo-pro/native-decode-plan.md` Phase 4, `docs/evo-pro/videodec2-abi.md`; `projects/evoplayer/src/evo_videodec2_probe.c` (port this), `media/include/{evo_vdec.h,sce/sce_videodec2.h}`, `media/src/evo_vdec_ffmpeg.c`, `tools/native-app/stubs/prx/`, `scripts/package-app.sh` |

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

### 4 · `#27` — GPU Step 2: sceAgc video convert + present — **IN PROGRESS**

**Prereqs cleared.** AGC gate PASSED (2026-09-03): `sceAgcInit` works from the
app module via the positional PRX import stubs (from #31 — no `LoadStartModule`,
no runtime NID). **`pp_agc_init` (shader setup: blobs → CreateShader ×2 →
LinkShaders) is ported and hardware-verified** (`pp/src/pp_agc.c` +
`agc_blobs.S` + `pp/blobs/`). **Next: port `render_frame` + wire the present
path** — full cold-start checklist in issue #27's body and
`docs/evo-pro/status.md` ("#27 shader setup VALIDATED" section). The `.syms`
for render_frame are pre-staged.

- Reads: **`docs/evo-pro/status.md`** (resume-here + render_frame checklist), **`docs/evo-pro/agc-implementation.md`** §2/§3 (blob layout, `render_frame` annotated), `gpu-rendering-plan.md`, `sharpprospero-agc-reference.md`, `videodec2-abi.md` §6
- Files: `third_party/ProsperoLight/src/native_agc_present.cpp` (`render_frame` ~571, `initialize_presenter` ~990), `pp/src/pp_agc.c` (scaffold done), `pp/src/agc_blobs.S`, `pp/blobs/`, `pp/src/pp_videoout.c`, `pp/src/pp_playback.c` (`use_v8` branch), `media/src/evo_vdec_native.c` (`ro_harvest` — NV12 vs I420), `tools/native-app/stubs/prx/libSceAgc*.syms`, `tools/evo-remote.sh` (test loop)

### 5 · `#28` — GPU Step 3: complete RmlUi on sceAgc

Blocked by #27 (it reuses all of #27's plumbing). Deletes the CPU rasteriser.

- Reads: `docs/evo-pro/agc-implementation.md` §5, `docs/evo-pro/gpu-rendering-plan.md`, SharpProspero `Graphics/Agc/` (`Renderer3D`, `CxRenderTarget`, `AgcRenderTargetSetup`, `AgcBufferDescriptor` — the register model to transcribe)
- Files: `ui_rml/src/evo_rmlui_render.cpp` + `.h` (the interface being replaced), `ui_rml/src/evo_rmlui_app.cpp`, `pp/shaders/`, `tools/build-shader.sh`, `tools/prof_rmlui.sh` (parity check)

### 6 · `#29` / `#30` — native hardware decode *(v1.1.0)*

`sceVideodec2` from the app module. **Phase 2 spike PASSED on hardware
2026-09-03** — `EVO vdec2: HARDWARE DECODE OK`, a 1920×1088 NV12 H.264 frame
decoded inside the full EVO Player. `#30` (the `evo_vdec.h` seam) is signed off. **#31 (Phase 4) DONE on hardware**
— GTA VI 4K H.264 plays real-time on `sceVideodec2` in EVO (`media/src/evo_vdec_native.c`,
resident decoder created pre-unjail, NV12→I420 de-interleave, colours correct,
no judder). Frame order is **display-order** (B-frame content smooth) — the
reorder window + min-PTS pairing hold. **Open: seek → frozen picture** on the
V8 4K path (filed high-priority). Next: **Phase 5** (Auto/FFmpeg/Native
settings row) + the seek fix.

- Reads: **`docs/evo-pro/status.md`** (the win + Phase 4 plan), `docs/evo-pro/native-decode-plan.md` (§3 architecture, Phase 2/4, kill criteria §8), `docs/evo-pro/videodec2-abi.md`
- Files: `media/include/evo_vdec.h`, `projects/evoplayer/src/evo_videodec2_probe.c` (the proven sequence to port), `media/include/sce/sce_videodec2.h`, `media/src/evo_vdec_ffmpeg.c` (the sibling impl), `main.c` (probe runs before `evo_jailbreak_self()`), `tools/native-app/stubs/prx/` (the PRX import stubs)
- **Two hard-won requirements** (see status.md): (1) `libSceVideodec2`+`libSceAgc`+`libSceAgcDriver` as positional PRX import stubs; (2) decode init must run **before** the self-unjail, or `sceSysmoduleLoadModule(207)` → ESDKVERSION.
- Sequencing: Phase 4 `evo_vdec_native.c` → Phase 5 settings toggle + `evo_vdec_probe()` → Phase 6 validation. Route A (`sceAvPlayer`) is dead.

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
