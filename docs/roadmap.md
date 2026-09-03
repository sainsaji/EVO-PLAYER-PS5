# Roadmap — implementation order & per-story references

The GitHub milestones (`v0.8.0` → `v0.9.0` → `v1.0.0` → `v1.1.0`) with a
dependency-ordered plan. **Each open issue's body carries its own "References &
sequencing" block** pointing at the exact docs + files to read — this page is
the map across all of them.

To implement a story: open its issue, read the docs it names, then the files it
names, then go.

Priority labels track this order: **critical** #27 (render_frame ported + wired
09-03, awaiting a first hardware run) · **high** #46, #44, #32, #36 · **medium** #9,
#16, #6, #37/#38/#39/#41, #33, #34, #35, #42, #47, #49 · **low** #48, the rest.
`independent` = no cross-deps, work any time in parallel.

**Grouping labels** (umbrellas retired 2026-09-03): `native-decode` = #30–#41 (#30 ✅ closed) ·
`rmlui` = #44, #45, #16, #28, #49 · `subtitles` = #35, #42, #43. (#26 closed
2026-09-02 — app-module playback works. #31 closed 2026-09-03 — native 4K
H.264 plays. #44 PR1+PR2 landed 2026-09-03 — legacy screen renderer deleted,
only a hardware pass left.)

**New stories (2026-09-03):** #47 native audio decode + Dolby/DTS bitstream
passthrough (v1.1.0, alongside `native-decode`) · #48 re-probe controller
vibration from the app module + haptics feedback channel (v0.9.0) · #49
consolidate the RmlUi UI seam — drop the model→params double hop, move the
screen builders out of `main.c` (v0.9.0, `rmlui`, do before #28).

---

## Dependency graph

```
                 ┌─────────────────── independent, any order, no console ───────────────────┐
                 │  #17 CI/tests   #9 Emby URL   #8 codec metrics   #42 subtitle cue counts   │
                 │  #5 swscale MT  #16 UI text/overflow   #34 IME kb   #35 subtitles A/C/D   │
                 └──────────────────────────────────────────────────────────────────────────┘

  #26 app-module playback ─── CLOSED (works; demanding 4K → native decode ✅ #31)

  #6 rotate buffers → direct mem ──────────────┐        (touches pp_videoout; do before #27)
                                               │
  #4 10-bit fast path (CPU stopgap) ───────────┤        (real fix is #27; optional)
                                               ▼
  positional PRX import stubs ─┬─► #27 GPU Step 2: sceAgc convert+present ──► #28 Step 3
   (package-app.sh step 6b,      │   gate ✅ + pp_agc_init ✅ hw; render_frame ported
    unconditional DT_NEEDED)      │   + pp_playback V8 wired 09-03 — awaiting a hw run
                                 │   (same wall also blocks #34 native IME keyboard)
                                 └─► Route B libSceVideodec2 ✅ #31 CLOSED — GTA 4K
                                     plays real-time on native decode (2026-09-03)

  RmlUi migration  (label: rmlui — #25 umbrella retired; screens built)
    #44 per-screen parity sign-off + delete dead legacy screen code  ◀── blocks #28
    #16 text clamping / widget overflow  (folded into #44's pass)
    #45 icon swap (Lucide + Kenney, approved)
    #35 part B: on-video caption overlay off legacy rr_text → RmlUi
    #28 RenderInterface → sceAgc + delete the CPU rasteriser (GPU Step 3, after #27)

  native hw decode  (label: native-decode, v1.1.0 — #29 umbrella retired)
    #30 evo_vdec.h seam ✅ CLOSED (FFmpeg-parity regression check moved to #38)
    #31 evo_vdec_native.c ✅ CLOSED — GTA 4K H.264 plays
    #37 Video decoder toggle (Auto/FFmpeg/Native) + probe + config  ◀── next
    #38 validation sweep + FFmpeg-vs-native A/B benchmark + docs
    #39 decode-thread watchdog (hung call must not wedge the app slot)
    #40 route direct memory via evo_direct_mem + multi-hour soak
    #41 HEVC hardware decode (2nd resident decoder)
    #32 scrub shows no player UI on the V8 4K path (not a freeze — app
        responsive; k4_live never composites the OSD + v8_hold skips the flip) (high)
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
| **#42** | Universal subtitle cue counts — demux-probe count for non-mkvmerge containers so decoy tracks lose the ranking (`prospero_subtitle_declared_cues` / `_score_stream`) | `docs/backlog.md` §6; `media/src/evo_subtitle.c`, `media/src/prospero_thumbnail.c` (probe pattern), `main.c` picker path |
| **#5** | Multi-thread the swscale fallback | `docs/converter-perf.md`; `media/src/evo_playback.c` (swscale fallback), `pp/src/pp_converter_parallel.c` (persistent-pool pattern) |
| **#34** | App module crashes when the native PS5 IME keyboard is opened (directory search). Same `sceKernelLoadStartModule`/undeclared-PRX wall as #27/#31 — `libSceImeDialog`/`libSceCommonDialog` aren't linked or PRX-stubbed for `PPSA99039`. Stopgap: force the virtual keyboard in the app module | `projects/evoplayer/ui/src/evo_keyboard.c`, `ui/include/evo_ime_dialog.h`, `main.c` (~L12504 search, ~L7581 kb toggle), `scripts/package-app.sh` (steps 5 + 6b), `tools/native-app/stubs/prx/README.md` |
| **#46** | App module persists **nothing** (settings/recent/favorites/resume) — the ELF payload did. `/download0/evoplayer/` isn't durable for a fake-signed ShadowMount title. **CODE DONE 2026-09-03 (`refactor/main-c-media-modules`), HW-VERIFY PENDING** — `evo_data_dir()`/`evo_data_path()` resolve the root at runtime (`/data/evoplayer` once `evo_jailbreak_is_open()`, `/download0` fallback only); all compile-time sites + `RESUME_FILE` routed through it; `evo_mkdir()` = `sceKernelMkdir` on the app module; `evo_persistence_rebind()` for the late-unjail race; one-shot migration of the old store. | `src/evo_data_path.{c,h}`, `src/evo_jailbreak.{c,h}`, `main.c` (`evo_ensure_data_dir`, `evo_migrate_legacy_store`, `evo_persistence_rebind`, `RESUME_FILE`), `addons/src/addon_emby.c`, `pp/src/evo_theme.c` (`evo_theme_reset`), `docs/evo-pro/phase-1b-app-module.md` §5 |
| **#35** | Subtitles only render English — `prospero_subtitle_clean_line` folds every non-ASCII char to `?` because the legacy bitmap atlas is ASCII-only. Parts A (stop mangling) + C (SRT charset detect via iconv) + D (sidecar formats/naming) are independent; part B (RmlUi caption overlay + Noto fallback fonts + HarfBuzz/BiDi) needs #44. Related: #42 (cue counts), #43 (`.ass` styling) | `media/src/evo_subtitle.c` (~L1147–1356, ~L1453), `main.c` (`prospero_subtitle_draw` ~L5041, `rr_text` ~L4860), `ui/include/evo_font.h`, `ui_rml/src/evo_rmlui_app.cpp` (~L150), `assets/rml/*.rcss`, `assets/fonts/`, `scripts/package-app.sh` |
| **#36** | Switch the release pipeline (`release.yml`) from ELF payloads to `EVOPlayer-<tag>.ffpfsc` — the ELF/hbldr context has no hw decode / GPU / user session (#27/#31), so a tagged ELF release ships a player that can't do the headline features. Rework `release.yml` build + verify + notes (ShadowMount+ install), CI `package-app --ffpfsc` link-check, version consistency (VERSION ↔ tag ↔ `param.json`), doc flip. Decision owed: fate of the ELF artifacts (recommend: keep `player-only.elf` labelled "limited" for 1–2 releases, then drop) | `.github/workflows/{release,build}.yml`, `scripts/{package-app,deploy-app,setup-pfs-tool,build-media-tile,package-pkg}.sh`, `projects/evoplayer/{VERSION,CHANGELOG.md,sce_sys/param.json}`, `docs/{packaging,tooling,validation}.md` |
| **#16** | Text clamping / overflow / title collisions | `docs/rmlui-integration-guide.md`, `docs/theming.md`; `assets/rml/*.rcss`, `ui_rml/src/evo_rmlui_render.cpp` (text path), `tools/uiview.sh` to check every screen |
| **#31** | Phase 4 — `evo_vdec_native.c`, `sceVideodec2` backend behind `evo_vdec.h` (Route B **proven on hw 09-03**) | `docs/evo-pro/status.md` (cold-start plan), `docs/evo-pro/native-decode-plan.md` Phase 4, `docs/evo-pro/videodec2-abi.md`; `projects/evoplayer/src/evo_videodec2_probe.c` (port this), `media/include/{evo_vdec.h,sce/sce_videodec2.h}`, `media/src/evo_vdec_ffmpeg.c`, `tools/native-app/stubs/prx/`, `scripts/package-app.sh` |

### 1 · `#26` — app-module playback crash — **CLOSED 2026-09-02**

1080p + reasonable 4K play in `PPSA99039`; demanding 4K degrades gracefully
(toast, no crash). Fixes: `55685aa0` (posix_fadvise SIGSYS), `d84d05c`
(flexible-memory allocator), `bb80de1` (slice threading + fatal-decode abort).
Demanding-4K playback needs native decode → **done (#31)**; the rest of that
work is the **`native-decode`** label (§6).

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

### 6 · native hardware decode — label **`native-decode`** *(v1.1.0)*

`sceVideodec2` from the app module. The #29 umbrella was retired 2026-09-03 —
the work is discrete stories under the `native-decode` label. **#31 landed on
hardware**: GTA VI 4K H.264 plays real-time on `sceVideodec2` in EVO
(`media/src/evo_vdec_native.c`, resident decoder created pre-unjail, colours
correct, no judder, display-order frames). Route A (`sceAvPlayer`) is dead.

| Story | What | State |
|---|---|---|
| **#30** | `evo_vdec.h` decoder seam (Phase 3) | ✅ closed — FFmpeg-parity check moved to #38 |
| **#31** | `evo_vdec_native.c` — `sceVideodec2` backend (Phase 4) | ✅ closed |
| **#37** | `Video decoder: Auto / FFmpeg / Native` toggle + `evo_vdec_probe()` + config migration (Phase 5) | **◀ next** |
| **#38** | Validation sweep (backend column) + FFmpeg-vs-native A/B benchmark + docs rewrite (Phase 6) | open |
| **#39** | Watchdog the decode thread — a hung `sceVideodec2` call must not wedge the app slot | open |
| **#40** | Route the resident decoder's direct memory through `evo_direct_mem` + multi-hour soak | open |
| **#41** | HEVC hardware decode — a 2nd resident `sceVideodec2` decoder (H.264-only today) | open |
| **#32** | Scrub shows no player UI on the V8 4K path (not a freeze — app responsive) | open, high |

- Reads: **`docs/evo-pro/status.md`**, `docs/evo-pro/native-decode-plan.md`
  (§3 architecture, Phases 4–6, kill criteria §8), `docs/evo-pro/videodec2-abi.md`
- Files: `media/include/evo_vdec.h`, `media/src/{evo_vdec_native,evo_vdec_ffmpeg}.c`,
  `projects/evoplayer/src/evo_videodec2_probe.c` (the proven sequence),
  `media/include/sce/sce_videodec2.h`, `main.c` (probe runs before
  `evo_jailbreak_self()`), `tools/native-app/stubs/prx/`
- **Two hard-won requirements** (see status.md): (1)
  `libSceVideodec2`+`libSceAgc`+`libSceAgcDriver` as positional PRX import
  stubs; (2) decode init must run **before** the self-unjail, or
  `sceSysmoduleLoadModule(207)` → ESDKVERSION.
- Coordinate **#37**'s fscanf config-append with **#27**'s `Renderer` row
  (same settings screen; land decoder-append first).

### RmlUi migration — label **`rmlui`** *(#25 umbrella retired 2026-09-03)*

Every screen already renders through RmlUi (`evo_rmlui_render_*`). Remaining
work as discrete stories:

| Story | What | State |
|---|---|---|
| **#44** | Per-screen parity sign-off vs `main` + delete the dead legacy screen code — **blocks #28**. **PR 1 done** (#16 folded in, OSD title marquee, host harness → every screen, [rmlui-parity.md](rmlui-parity.md)). **PR 2 done** (`evo_screens.c`/`evo_chrome.c` deleted → `evo_layout.c`, every `draw_*_screen` legacy tail removed, `uiview`/`uiplay` → RmlUi harness; payload build + tests green). Only a hardware pass of the HW-marked rows remains | open, high |
| **#16** | Text clamping / widget overflow / title collisions — **done in #44 PR 1** (RCSS clamps on OSD / dialog / browser inspector / media info / settings / changelog / launch hero; OSD marquee) | open |
| **#45** | Icon swap — Lucide concept icons + Kenney controller glyphs (approved, `docs/icon-swap-handoff.md`) | open, low |
| **#35** part B | On-video **caption** rendering off legacy `rr_text` → RmlUi | open |
| **#28** | Bind `Rml::RenderInterface` → `sceAgc` + delete the CPU coverage rasteriser (GPU Step 3, after #27) | open, low |

- Reads: `docs/rmlui-integration-guide.md` (§7 per-screen parity specs),
  `docs/ui-handoff.md`, `docs/theming.md`, `docs/icon-swap-handoff.md`
- Files: `projects/evoplayer/{ui_rml,assets/rml,ui/src}/`, `main.c`, `Makefile` (`UI_SRCS`)

---

## Done (for context)

- **RmlUi Step 1** — surface cache, re-raster only on change. Shipped, hardware-
  verified (idle menus 11 → ~60 fps). Commit `b3d00ac`. Stays as the
  AGC-unavailable / host fallback under #28.
- **App-module self-unjail** — `evo_jailbreak.c`, Lapy/etaHEN file-drop. Covers
  `/mnt/usb0` + `/data`. Implemented; hardware-confirmed as part of #26's launch.
