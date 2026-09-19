# Roadmap — implementation order & per-story references

The GitHub milestones (`v0.8.0` → `v0.9.0` → `v1.0.0` → `v1.1.0`) with a
dependency-ordered plan. **Each open issue's body carries its own "References &
sequencing" block** pointing at the exact docs + files to read — this page is
the map across all of them.

To implement a story: open its issue, read the docs it names, then the files it
names, then go.

For a **visual** view of the same issues — kanban, priority table, milestone
timeline, blocked list — see the "EVO Player Roadmap" GitHub Project;
[project-tracking.md](project-tracking.md) has the one-command setup.

Priority labels track this order: **critical** #32 · **high** #36, #72, #76, #77 · **medium** #35,
#37/#38/#39/#41/#59/#60, #33, #34, #9, #42, #47, #49, #50, #53, #62, #63, #64, #65, #68, #73, #74, #75 · **low** #48, #51,
#52, the rest. `independent` = no cross-deps, work any time in parallel.

**#27 CLOSED 2026-09-04** — sceAgc GPU present path delivered + hardware-verified
(PR #61). 982 µs/frame. `pp_agc*` was deleted by GL-6 (#82); the present path
is now `media/src/evo_agc_runtime.c`, bare-metal `sceAgc`. Historical leftovers:
**#41** HEVC + VP9 native decode remains open; #62/#37/#28 folded into GL-4/GL-5.

**Closed 2026-09-03 (evening):** **#6** (video buffers → direct mem — landed as
the swscale rotate-ring slab move + `--agc-probe` gate; PR #54, hw-verified) ·
**#56** (GTA 4K regression — was #27's unproven sceAgc path armed by default;
gated) · **#57** (4K seek "too demanding" — #31 routed Tears of Steel to native
decode, which rejects it post-`sceVideodec2Reset`; PR #58 adds the FFmpeg
fallback). **#55** (4K V3-fallback buffer overflow) — **fixed in PR #61** as part
of #27: `pp_product_reconfigure_vo` gates on `pp_playback_set_output` completing,
`pb->display_cap` tracks the alloc, the V3 branch reallocs to the real size.
Exercised (no corruption/hang) via #27's VO-retile path; verify + close.

**Closed 2026-09-03 (earlier):** **#46** (persistence — hardware-verified,
`788b1a0`), **#44** (RmlUi parity signed off, legacy screen renderer deleted) +
**#16** (text clamps, absorbed into #44). The #16 fixes had been shadowed on
hardware by a stale `/data/evoplayer/app/assets/` until `edc3a08` made
`deploy-app.sh --ffpfsc` re-sync the asset tree. #35 is now unblocked (part B's
dependency #44 is done); **#28 unblocked** (#27 closed 2026-09-04).

**Grouping labels** (umbrellas retired 2026-09-03): `native-decode` = #30–#41 (#30 ✅ closed; #41 = codec-independent H.264/HEVC/VP9 backend, research base `third_party/ps5-hardware-video-decoding-research/`) ·
`rmlui` = #45, #28, #49, #60, #68 (#44 + #16 ✅ closed) · `subtitles` = #35, #42, #43 ·
`modularisation` = #49, #53 (`main.c` carve-up — [modularisation-plan.md](modularisation-plan.md)) ·
`render-overhaul` = #77–#82 (collapsed every present route / font system /
converter into one funnel; the OpenGL backing was later removed in favour of
bare-metal `sceAgc` — [evo-pro/agc-bare-metal-ui.md](evo-pro/agc-bare-metal-ui.md)).
(#26 closed 2026-09-02 — app-module playback works. #31 closed 2026-09-03 — native 4K
H.264 plays. #44 PR1+PR2 landed 2026-09-03 — legacy screen renderer deleted,
only a hardware pass left.)

**Render overhaul — COMPLETE (2026-09-10):** the sceAgc rendering used to be
spread across 5 present routes, 3 font systems and CPU+GPU converters. Chain
**#77 → #78 → #79 → #80 → #81 → #82** (`GL-1`…`GL-6`) collapsed it into a
single funnel, initially backed by a `ps5-opengl` GL/EGL context. **All six
done.** That OpenGL backing has since been **removed**: `media/src/evo_agc_runtime.c`
drives `sceAgc` and `sceVideoOut` directly, at the panel's own resolution, and
the submodule plus the `--gl*` build flags are gone. The rows below record the
GL chain as it happened; see [evo-pro/agc-bare-metal-ui.md](evo-pro/agc-bare-metal-ui.md)
for what runs now. GL-6 (#82) deleted `pp_agc*` / `pp_videoout` / `pp_platform.h`
/ the `#28` geo sink / the shader blobs and closed **#67, #69, #70, #5**.
**Rescoped/open:** #68 (mechanism → GLSL/RCSS, absorbs #70). #4 is gone —
P010 sampling shipped in #81 and its HDR-output-metadata + PQ/HLG tail was
folded into #41 §4, then #4 was deleted (2026-09-11). Delivered along the way:
#62 (GL video parity, #80), #35/#34/#63 (#81), #76 (#80), #49 (folded into #79).

**New stories (2026-09-03):** #47 native audio decode + Dolby/DTS bitstream
passthrough (v1.1.0, alongside `native-decode`) · #48 re-probe controller
vibration from the app module + haptics feedback channel (v0.9.0) · #49
consolidate the RmlUi UI seam — drop the model→params double hop, move the
screen builders out of `main.c` (v0.9.0, `rmlui`, do before #28) · **#50 +
#51 are sub-issues of #46** (spun out of its implementation): #50 grow the host
test suite over the runtime data-root logic + persistence parsers (v0.9.0,
`independent`, CI wiring stays with #17) · #51 quiet the boot breadcrumbs —
keep `klog` + `evo_boot.log`, drop the notification popups (v0.9.0,
`app-module`).

**2026-09-03 (later):** #52 stand up the GitHub Project board (tooling in
`f7a340e`; `docs/project-tracking.md`) · #53 `main.c` Track-B carve-up —
`main.c` logic is at 0% coverage because it can't be host-linked; #53 is the
prerequisite for meaningful test coverage and **blocks #50**'s `main.c` part.
New label `modularisation` = #49 + #53 · **#59** surface active video decoder
backend in player UI (HW vs SW badge, start/fallback toasts, OSD / Media Info, v1.1.0, `native-decode`,
`priority: medium`) · **#60** make `.ffpfsc` self-contained by embedding RmlUi
assets in binary (eliminate `/data` FTP sync, v0.9.0, `rmlui`, `priority: medium`) ·
**#63** add real-time CPU, GPU, and RAM usage graphs to the player (Diagnostic HUD,
v0.9.0, `independent`, `priority: medium`) · **#64** fix Launch screen recent thumbnails
blanking, artwork card overflow & missing corner pixels (evo_cover_budget cache check, tile overflow:hidden, concentric radius alignment, v0.9.0, bug, `priority: medium`) ·
**#65** support D-pad navigation on playback exit confirmation dialog (v0.9.0, `ui`, `priority: medium`) ·
**#68** enhance UI geometry and styling on GPU (curves, shadows, analytical AA, v1.0.0, `rmlui`/`ui`/`gpu`, `priority: medium`) ·
**#72** fix internal storage browsing failure under Lapy JB (POSIX permissions, `O_DIRECTORY` fallback, `DT_UNKNOWN` resolution, v0.9.0, `app-module`/`ui`, `priority: high`) ·
**#73** auto-resume playback after seek/scrub confirm & display OSD on start then hide (v0.9.0, `playback`/`ui`, `priority: medium`) ·
**#74** revamp About screen with dedicated RmlUi layout (replace faux-menu cards with info dashboard, native vs FFmpeg codec matrix & distinct action buttons, v0.9.0, `rmlui`/`ui`, `priority: medium`) ·
**#75** modernise toast notifications with dedicated RmlUi overlay (replace legacy CPU software rasterizer, v0.9.0, `rmlui`/`ui`, `priority: medium`) ·
**#76** fix 4K native playback aspect ratio toggle corrupting screen colors & failing to scale on GPU (v0.9.0, `gpu`/`playback`/`ui`, `priority: high`).

---

## Dependency graph

These edges are also wired as **native GitHub relationships** (2026-09-03) so
the issue UI shows blockers / sub-tasks directly:

- **Sub-issues:**
  - #68 → #69, #70 (closed — superseded by the OpenGL overhaul; MSAA/SDF are GLSL now)
  - #46 → #50, #51 (data-root test coverage & breadcrumb quieting)
  - #44 → #16 (RmlUi screen parity & text clamps, both closed)
  - #29 → #30, #31 (native decode phases, both closed)
  - #25 → #44, #45 (RmlUi umbrella, closed)
  - #3 → #42, #43 (subtitle meta tracker, closed)
- **Blocking → blocked:**
  - #77 → #78 → #79 → #80 → #81 → #82 (`render-overhaul` GL-1…GL-6; #77 is go/no-go on `ps5-opengl` @ FW 12.70)
  - #79 folds in #49 · **#80 CLOSED (hw-verified `c49b317`): closed #76, resolved #62, retired --no-gl · #81 delivers #34/#35/#63 + P010** · #68 blocked-by #79
  - #60 → #36 (self-contained `.ffpfsc` blocks release pipeline packaging)
  - #37 → #38, #41, #47, #59 (video decoder toggle blocks benchmark, HEVC, audio passthrough, and UI badge)
  - #59 → #63 (decoder indicator blocks Diagnostic HUD graphs)
  - #31 → #39, #40 (native decoder backend blocks hang recovery & memory tidy)
  - #46 → #72 (persistence & data root blocks Lapy JB internal storage fix)
  - #35 → #43 (caption overlay blocks styled `.ass`)
  - #53 → #50 (`main.c` carve-up blocks test suite)
  - #6 + #27 both closed → #28 unblocked → #67 (text-over-GPU underway).

All relationships are wired natively via GitHub's Issue Relationships
(`blocked-by`, `blocking`, and `sub-issues` / `parent`), visible directly in the
GitHub issue sidebar and Project #5 board.

```
                 ┌─────────────────── independent, any order, no console ───────────────────┐
                 │  #17 CI/tests   #9 Emby URL   #8 codec metrics   #42 subtitle cue counts   │
                 │  #5 swscale MT  #16 UI text/overflow   #34 IME kb   #35 subtitles A/C/D   │
                 └──────────────────────────────────────────────────────────────────────────┘

  #26 app-module playback ─── CLOSED (works; demanding 4K → native decode ✅ #31)

  #6 rotate buffers → direct mem ── ✅ CLOSED (PR #54)

  positional PRX import stubs ─┬─► #27 GPU Step 2: sceAgc convert+present ─ ✅ CLOSED
   (package-app.sh step 6b,      │   2026-09-04, PR #61 — GTA 4K on GPU, correct colour,
    unconditional DT_NEEDED)      │   982µs/frame. Leftovers: #62 A/B, #37 row, #41 (HEVC+VP9 ✅ verified)
                                  │      │
                                  │      └─► #28 Step 3: RmlUi on sceAgc (unblocked)
                                  │   (same PRX-stub wall also blocks #34 native IME kb)
                                 └─► Route B libSceVideodec2 ✅ #31 CLOSED — GTA 4K
                                     plays real-time on native decode (2026-09-03)

  RmlUi migration  (label: rmlui — #25 umbrella retired; screens built)
    #44 per-screen parity sign-off + delete legacy screen code  ✅ CLOSED 09-03
    #16 text clamping / widget overflow  ✅ CLOSED 09-03 (with #44)
    #45 icon swap (Lucide + Kenney, approved)
    #35 part B: on-video caption overlay off legacy rr_text → RmlUi  (unblocked)
    #28 RenderInterface → sceAgc + delete the CPU rasteriser (GPU Step 3, after #27)
    #49 seam cleanup — drop model→params hop, screen builders out of main.c
    #60 self-contained .ffpfsc: embed RmlUi assets in binary (drop /data FTP sync)

  native hw decode  (label: native-decode, v1.1.0 — #29 umbrella retired)
    #30 evo_vdec.h seam ✅ CLOSED (FFmpeg-parity regression check moved to #38)
    #31 evo_vdec_native.c ✅ CLOSED — GTA 4K H.264 plays
    #37 Video decoder toggle (Auto/FFmpeg/Native) + probe + config  ◀── next
    #38 validation sweep + FFmpeg-vs-native A/B benchmark + docs
    #39 decode-thread watchdog (hung call must not wedge the app slot)
    #40 route direct memory via evo_direct_mem + multi-hour soak
    #41 codec-independent NativeVideoBackend (H.264 + HEVC + VP9 ✅ verified — see issue for the phased plan on what's left)
    #59 Surface video decoder backend in player UI (HW vs SW badge + OSD)
    #32 4K native playback: seeking plays audio but video shows black (critical)
```

---

## Order

### 0 · Independent — start any time, in parallel, no hardware

Tagged `independent`. No cross-dependencies; each touches an isolated subsystem.

| # | Story | Reads |
|---|---|---|
| **#17** | CI: unit tests, coverage, Sonar | `docs/tooling.md`, `docs/validation.md`, `docs/converter-perf.md`; `.github/workflows/`, `tools/bench.sh`, `tools/prof_rmlui.sh` |
| **#52** | Stand up the "EVO Player Roadmap" GitHub Project board — tooling landed (`f7a340e`); needs `gh auth refresh -s project`, one script run, and the views + built-in workflows set up in the UI | `scripts/setup-github-project.sh`, `.github/workflows/add-to-project.yml`, `docs/project-tracking.md` |
| **#53** | `main.c` carve-up, **Track B** — extract `evo_settings` / `evo_osd` / `evo_media_meta` / `evo_browser` / per-screen draw etc. as leaf modules (pure moves, no behaviour change). `main.c` is ~12.9k lines and links the whole runtime, so its logic is at **0% coverage**; this is the lever on that. Blocks #50's `main.c` portion. `modularisation` label with #49 | `docs/modularisation-plan.md` (§ Track B, § Rules), `projects/evoplayer/main.c` (`/* PROSPERO_*_START/END */` markers), `projects/evoplayer/Makefile` (`_SRCS`), `tools/{bench,uiview}.sh` (parity checks) |
| **#33** | Clean up the unattended hw-test harness — **partly done**: `tools/evo-remote.sh` (scriptable `play`/`seek`/`watch` over FTP) + `src/evo_usb_remote.c` (`-DEVO_USB_REMOTE`) replaced the compile-time autoplay; `note()` USB log gated to `-DEVO_VDEC_LOG`; payload build now invalidates the app cflags stamp. Remaining: fully hands-off launch (shsrv), `app_ctl` launch fix | `tools/evo-remote.sh`, `src/evo_usb_remote.c`, `scripts/package-app.sh` (`--usb-remote`) |
| **#9** | Emby shows raw stream URL, not the title | `docs/addons-emby-nuvio.md`; `projects/evoplayer/addons/src/addon_emby.c`, `ui_rml` list rendering (`evo_rmlui_bridge.cpp` / `evo_rmlui_app.cpp` list path) |
| **#8** | Codec-sweep decode latency / drop / colour metrics — **instrumentation + runner landed, hw-run pending**. Note the references below are post-GL-4: `tools/bench.sh` and the CPU converters are gone, so the "convert ms" column is now the GL present cost, and the decode timers live in the `evo_vdec` seam (both backends, = #38's A/B) | `docs/validation.md` § *Codec sweep*, `docs/tooling.md`; `media/src/evo_sweep.c`, `media/src/evo_vdec_ffmpeg.c` (seam timers), `tools/sweep_run.py`, `tools/sweep_report.py`, `tools/evo-remote.sh sweep` |
| **#42** | Universal subtitle cue counts — demux-probe count for non-mkvmerge containers so decoy tracks lose the ranking (`prospero_subtitle_declared_cues` / `_score_stream`) | `docs/backlog.md` §6; `media/src/evo_subtitle.c`, `media/src/prospero_thumbnail.c` (probe pattern), `main.c` picker path |
| **#5** | Multi-thread the swscale fallback | `docs/converter-perf.md`; `media/src/evo_playback.c` (swscale fallback), `pp/src/pp_converter_parallel.c` (persistent-pool pattern) |
| **#34** | App module crashes when the native PS5 IME keyboard is opened (directory search). Same `sceKernelLoadStartModule`/undeclared-PRX wall as #27/#31 — `libSceImeDialog`/`libSceCommonDialog` aren't linked or PRX-stubbed for `PPSA99039`. Stopgap: force the virtual keyboard in the app module | `projects/evoplayer/ui/src/evo_keyboard.c`, `ui/include/evo_ime_dialog.h`, `main.c` (~L12504 search, ~L7581 kb toggle), `scripts/package-app.sh` (steps 5 + 6b), `tools/native-app/stubs/prx/README.md` |
| ~~#46~~ | ✅ **CLOSED 2026-09-03, hardware-verified.** Runtime data-root resolution (`/data/evoplayer` once `evo_jailbreak_is_open()`, `/download0` fallback) + `evo_mkdir`=`sceKernelMkdir` + `evo_persistence_rebind()` + one-shot store migration. `788b1a0`. Follow-ups: #50 (tests), #51 (breadcrumbs). |
| **#50** | *(sub-issue of #46)* Grow the host test suite over the #46 runtime data-root logic (`evo_data_path` join / rebind / `evo_mkdir`) and the persistence parsers (`evo_recent`, `evo_favorites`), plus `evo_theme` parse + `evo_theme_reset`, `evo_layout` geometry. Host-only; CI wiring stays with #17 | `tests/run_tests.sh` (SRCS), `tests/test_runner.c`, `src/evo_data_path.c`, `src/evo_recent.c`, `src/evo_favorites.c`, `pp/src/evo_theme.c`, `ui/src/evo_layout.c` |
| ~~#51~~ | ✅ **CLOSED 2026-09-05, hardware-verified.** *(sub-issue of #46)* Split the on-screen notification popup from the durable channels: `evo_bt_()`'s guard moved from `EVO_BOOT_TRACE` to `EVO_APP_MODULE` so `sceKernelDebugOutText` (klog) is unconditional; `sceKernelSendNotificationRequest` moved behind opt-in `EVO_BOOT_TRACE_POPUP` (`package-app.sh --breadcrumbs`). Default `APP_DEFS` no longer sets `-DEVO_BOOT_TRACE=1`; `main.c`'s readdir self-test re-gated on `EVO_BOOT_TRACE_POPUP` since the bare `EVO_BOOT_TRACE` macro no longer exists. **Scope grew during hw testing**: two more unconditional popup sources turned up and got the same treatment — `pp_stage_bc()` (`pp_stage_breadcrumb.c`, fires on every `P8_AVLOG`/`SEEK_AVFRAME`/`P8_VDEC_FATAL` checkpoint, i.e. routinely *during playback*) and `evo_vdec_native.c`'s `note()` (decode heartbeat/errors — the reported recurring "EVO vdec native:" popups mid-playback). Both now gated on the same `EVO_BOOT_TRACE_POPUP`, klog/file trails left unconditional. Hardware-verified: default build boots + plays with popups gone, confirmed by the user. | `include/evo_boot_trace.h`, `src/evo_boot_log.c`, `include/evo_boot_log.h`, `pp/src/pp_stage_breadcrumb.c`, `media/src/evo_vdec_native.c`, `main.c`, `scripts/package-app.sh`, `docs/tooling.md`, `docs/evo-pro/status.md` |
| **#35** | Subtitles only render English — `prospero_subtitle_clean_line` folds every non-ASCII char to `?` because the legacy bitmap atlas is ASCII-only. Parts A (stop mangling) + C (SRT charset detect via iconv) + D (sidecar formats/naming) are independent; part B (RmlUi caption overlay + Noto fallback fonts + HarfBuzz/BiDi) needs #44. Related: #42 (cue counts), #43 (`.ass` styling) | `media/src/evo_subtitle.c` (~L1147–1356, ~L1453), `main.c` (`prospero_subtitle_draw` ~L5041, `rr_text` ~L4860), `ui/include/evo_font.h`, `ui_rml/src/evo_rmlui_app.cpp` (~L150), `assets/rml/*.rcss`, `assets/fonts/`, `scripts/package-app.sh` |
| **#36** | Switch the release pipeline (`release.yml`) from ELF payloads to `EVOPlayer-<tag>.ffpfsc`. **§1/§2/§6 landed 2026-09-05** (pipeline mechanics untested-on-a-real-tag but locally dry-run verified): `release.yml` now builds + PFS-packs via `package-app.sh --ffpfsc` and publishes `EVOPlayer-<tag>-PPSA99039.ffpfsc` + `SHA256SUMS.txt` (all ELF/tile/homebrew-zip build steps removed — Option A from §5, since CLAUDE.md already asserts `.ffpfsc` as the sole release/deploy path); `build.yml` gained a `package-app` job (`--ffpfsc` on every PR, archived as an artifact); both verify steps check the signed container (`ps5-native-tool self --inspect`, not `file`/`readelf` — `eboot.bin` is FSELF-wrapped and reads as opaque `data`; the pre-sign ELF at `output/app/.build/eboot.elf` is what's ELF-type-checked), `param.json` `titleId`, `libc.prx` presence, and a non-trivial `assets/` size (guards #60's embedded bundle landing empty too). Ported from the sibling `ShaderRip-PS5` repo's `release.yml`/`build.yml` (its `build.yml` has real passing CI runs proving the same Docker-in-Actions pattern; its `release.yml` has never actually been tag-triggered, so treat that lineage as design-reviewed, not battle-tested). **Still open:** §3 (VERSION → `contentVersion`/`masterVersion` mapping — deliberately not invented; current `01.000.001` predates this pipeline), §4 full docs flip (`docs/packaging.md`, README, `docs/validation.md`), §8 reproducibility audit (MkPFS/FSELF determinism unverified). Needs a real tag push to confirm the release job end-to-end (local dry-run only covered the build+verify steps, not the GH release upload). | `.github/workflows/{release,build}.yml` (done), `scripts/{package-app,deploy-app,setup-pfs-tool}.sh` (untouched, reused as-is), `projects/evoplayer/{VERSION,sce_sys/param.json}`, `CHANGELOG.md`, `docs/{packaging,tooling,validation}.md` (tooling.md done, others open) |
| **#63** | Real-time CPU, GPU, and RAM usage graphs in the player Diagnostic HUD (Stats for Nerds) | `projects/evoplayer/assets/rml/playback.rml`, `projects/evoplayer/assets/rml/playback.rcss`, `media/include/evo_perf_monitor.h`, `media/include/evo_direct_mem.h`, `ui_rml/` |
| **#64** | Fix Launch screen recent thumbnails blanking, artwork card overflow, and missing corner pixels | `projects/evoplayer/main.c` (~L7996, ~L8149), `projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp`, `projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp`, `projects/evoplayer/assets/rml/launch.rcss` |
| **#65** | Playback exit confirmation dialog: support D-pad navigation between actions | `projects/evoplayer/main.c` (~L10254, ~L12698), `projects/evoplayer/assets/rml/dialog.rml`, `projects/evoplayer/assets/rml/dialog.rcss` |
| **#72** | Fix internal storage browsing under Lapy JB while USB drive is accessible (`/data` permissions, `O_DIRECTORY` fallback, `DT_UNKNOWN` resolution) | `projects/evoplayer/main.c`, `projects/evoplayer/src/evo_readdir.c`, `projects/evoplayer/src/evo_jailbreak.c`, `projects/sandbox_unjail/main.c` |
| ~~#73~~ | ✅ **CLOSED 2026-09-05, hardware-verified.** Auto-resume playback after seek confirm and display OSD initially on start then auto-hide. Scope grew on hardware: the original "confirm with Cross" model itself was the complaint on native-4K (`k4_live`) content, so seeking now **auto-commits** ~450ms after the scrub target stops moving (`prospero_scrub_autocommit_tick()`, called once per frame) instead of requiring an explicit Cross press at all — Cross still works as an immediate-confirm shortcut, and Circle-cancel is unaffected (still restores the pre-scrub position/state, verified on hardware). `prospero_scrub_confirm()` always resumes (`restore_paused = 0`) regardless of the pre-scrub pause state; `prospero_scrub_overlay_pump()`'s `SCRUB_OVL_LEAVING` settle branches on `prospero_scrub_ovl_wait_seek` to force-resume only a confirmed seek; `prospero_chapter_jump()` gets the same treatment; `start_video_playback()` stamps `controls_last_used_ms` so the OSD shows on open instead of starting bare. **Follow-up split out, not part of this fix:** the native-4K/GPU present path's OSD compositor (`pp_agc_osd`, #28 Phase 2) is a separate opt-in feature (needs `/mnt/usb0/evo_agc_osd`) that stays off by default, so the "OSD on start" half doesn't show for `k4_live` content yet — deliberately deferred to a future #28 story rather than folded in here. | `projects/evoplayer/main.c` (`start_video_playback`, `prospero_scrub_overlay_pump`, `prospero_chapter_jump`, `prospero_scrub_confirm`, `prospero_scrub_move`, new `prospero_scrub_autocommit_tick`), `projects/evoplayer/media/src/evo_demux.c` (seek settlement, verified correct, untouched) |
| ~~#74~~ | ✅ **CLOSED 2026-09-05, hardware-verified.** Revamp About screen with dedicated RmlUi layout — replaced faux-menu list with information dashboard, 2-column comparative Codec Capabilities Matrix (Native Hardware Decode via `sceVideodec2` + `sceAgc` Direct GPU vs FFmpeg 6.x Multithreaded Software Fallback), 3-column technical specification grid, non-interactive card affordances, and distinct action bar with primary `[View Changelog]` button. Hardware-verified on console: renders cleanly on PS5, layout, font rendering, and D-pad navigation verified. | `projects/evoplayer/assets/rml/about.{rml,rcss}`, `projects/evoplayer/ui_rml/`, `projects/evoplayer/main.c` |
| ~~#75~~ | ✅ **CLOSED 2026-09-05, hardware-verified.** Modernise toast notifications with dedicated RmlUi overlay — replace legacy CPU rasterizer. `toast.rml`/`toast.rcss` render in their **own `Rml::Context`** (`m_toast_context`), separate from the menu/OSD context - a toast composites on top of *whatever* screen is active without disturbing its Show/Hide state or, for cached menu screens (`RenderCachedScreen`), forcing a full re-rasterise every animation frame the way sharing the main context would (the marquee-vs-cache problem this sidesteps entirely). Same caller-owns-the-animation convention as the CPU version: `evo_toast.c`'s existing hold/fade/slide timing pushes `alpha`/`slide` every frame via `evo_rmlui_update_toast()`, rendered via `evo_rmlui_render_toast()` at the exact old `draw_prospero_toast(fb)` call site in `main.c` - **zero main.c changes needed**. 4 kinds (info/tech/error/ok) with matching icons; `EvoThemeColors` has no `danger` channel so error uses a fixed red matching the 4 legacy themes' near-identical danger colours rather than threading a new field through `SetTheme` for this alone. Legacy `evo_widget_toast()` + its `evo_toast`/`evo_toast_kind` types **removed** (not just bypassed) from `evo_widgets.{c,h}`, along with the now-unused `EVO_TOAST_*` metrics. **Position note:** the issue's proposed `top:48px` collided with the launch screen's header (clock/version) - caught via a new host-preview fixture (`tools/uiview_playback_rml.cpp`'s `render_toast_screens`, `rml_toast_over_launch.png`) before ever touching hardware; moved to `top:172px`, clear of every screen's header. Host preview + full app-module build both compile clean (0 warnings). Hardware-verified: toasts render correctly over live playback video and over menu screens alike, colours/icons correct. | `projects/evoplayer/assets/rml/toast.{rml,rcss}`, `projects/evoplayer/ui_rml/{include,src}/evo_rmlui_{app,bridge}.{h,cpp}`, `projects/evoplayer/src/evo_toast.c`, `projects/evoplayer/ui/{include,src}/evo_widgets.{h,c}`, `projects/evoplayer/ui/include/evo_metrics.h`, `tools/uiview_playback_rml.cpp` |
| **#76** | Fix 4K native playback aspect ratio toggle corrupting screen colors and missing GPU scaling | `projects/evoplayer/pp/src/pp_agc.c`, `projects/evoplayer/main.c` (~L1605), `projects/evoplayer/pp/src/pp_videoout.c` (~L356) |
| **#77 ✅** | `render-overhaul` GL-1 — vendor `ps5-opengl`, prove it renders on FW 12.70. **GO (2026-09-09):** `--gl-smoke` eboot rendered on the console — `result=PASS`, Mesa 26.2 / GL 3.3 Core / PS5 AGC, pixel-exact readback, no fail-stop (`docs/evo-pro/gl1-spike.md#7`). Submodule + `_Exit` patch + from-source SDK + `package-app.sh --gl-smoke`. Ready to merge + close; **#78 unblocked.** | `docs/evo-pro/gl1-spike.md`, `docs/evo-pro/opengl-render-overhaul.md`, `third_party/ps5-opengl/`, `scripts/build-ps5-opengl.sh`, `projects/evoplayer/pp/src/pp_gl_smoke.c` |
| **#78** | `render-overhaul` GL-2 — RmlUi `RenderInterface_GL3`, host-proven | RmlUi `Backends/RmlUi_Renderer_GL3.cpp`, `projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp` + `include/evo_rmlui_render.h`, `evo_rmlui_fileinterface.cpp`, `tools/uiview_playback_rml.cpp`, `third_party/ps5-opengl/examples/core33-imgui/` |
| **#79** | `render-overhaul` GL-3 — GL owns the menu framebuffer; delete the dual RmlUi path (folds in #49) | `projects/evoplayer/main.c` (frame loop ~L12247–13332, dispatch ~L13167), `ui_rml/src/evo_rmlui_app.cpp` (`RenderCachedScreen`, `AgcGeoPresent`), `evo_rmlui_render_agc.cpp`, `pp/src/pp_agc.c` (`pp_agc_present_ui`) |
| **#80** | ✅ **CLOSED 2026-09-10** (`c49b317`, hw-verified). `render-overhaul` GL-4 — one GL present path: NV12 R8/RG8 + GLSL YUV→RGB + RG8 OSD composite + one flip; deleted the CPU converters / `tile_copy` / backend enum / 5-way dispatch / `#32` overlay machine; held-frame seek; retired `--no-gl`. Closed #76, resolved #62 (`tools/gl_yuv_parity.py`). **P010 → #81.** Plan: evo-pro/gl4-video-path-plan.md *(not written)*. | `projects/evoplayer/pp/src/` (`pp_converter_*.c`, `pp_compute_pipeline.c`, `tile_copy.c`, `pp_videoout.c`, `pp_playback.c`, `pp_agc.c` `agc_render_frame`), `main.c` (present dispatch, `prospero_apply_view_mode` ~L1605), `media/src/evo_vdec_native.c`, `ui_rml/src/evo_gl_context_device.cpp`, `pp/src/pp_gl_smoke.c`, `patches/ps5-opengl/` |
| **#81** | `render-overhaul` GL-5 — subtitles / keyboard / image viewer / HUD / **P010+tone-map** → GL; delete the bitmap fonts + `pp_map_yuv420p10_to_8` (delivers #34, #35, #63; P010 moved from #80) | `projects/evoplayer/main.c` (`prospero_subtitle_draw` ~L5161, `draw_char`/`draw_text` ~L2661, `draw_image_screen` ~L3059, `draw_fps_overlay` ~L11887), `ui/src/{evo_keyboard,evo_draw,evo_widgets}.c`, `media/src/evo_subtitle.c` |
| **#82** | ✅ **DONE 2026-09-10.** `render-overhaul` GL-6 — deleted `pp_agc.c`/`pp_agc_osd.c`/`pp_videoout.c`/`pp_platform.h`/`evo_rmlui_render_agc.cpp` + the `#28` geo sink + the vendored shader blobs (`pp/blobs/`, `pp/shaders/`, `agc_blobs.S`, `agc_ui_blobs.S`, `tools/build-shader.sh`); `evo_vdec_native` drops `pp_agc`; PRX stubs populated from ps5-opengl imports; rewrote `gpu-notes.md` / `gpu-rendering-plan.md` / `agc-implementation.md` / `architecture.md` / `status.md` / `CLAUDE.md`; closed #67/#69/#70/#5. **Kept** (user decision): both RmlUi render interfaces + the `EvoRenderBridge` seam. `main.c` ~11,000 lines. | `projects/evoplayer/pp/src/pp_agc*.c`, `pp_videoout.c`, `ui_rml/src/evo_rmlui_{app,bridge,render}.*`, `media/src/evo_vdec_native.c`, `scripts/package-app.sh`, `docs/gpu-notes.md`, `docs/evo-pro/{gpu-rendering-plan,agc-implementation,status,opengl-render-overhaul}.md`, `docs/architecture.md`, `CLAUDE.md` |
| **#31** | Phase 4 — `evo_vdec_native.c`, `sceVideodec2` backend behind `evo_vdec.h` (Route B **proven on hw 09-03**) | `docs/evo-pro/status.md` (cold-start plan), `docs/evo-pro/native-decode-plan.md` Phase 4, `docs/evo-pro/videodec2-abi.md`; `projects/evoplayer/src/evo_videodec2_probe.c` (port this), `media/include/{evo_vdec.h,sce/sce_videodec2.h}`, `media/src/evo_vdec_ffmpeg.c`, `tools/native-app/stubs/prx/`, `scripts/package-app.sh` |

### 1 · `#26` — app-module playback crash — **CLOSED 2026-09-02**

1080p + reasonable 4K play in `PPSA99039`; demanding 4K degrades gracefully
(toast, no crash). Fixes: `55685aa0` (posix_fadvise SIGSYS), `d84d05c`
(flexible-memory allocator), `bb80de1` (slice threading + fatal-decode abort).
Demanding-4K playback needs native decode → **done (#31)**; the rest of that
work is the **`native-decode`** label (§6).

### 2 · `#6` — rotate buffers → direct memory *(before #27)* — **CODE DONE, hw-verify pending**

Small, memory hygiene, and it touches `pp_videoout.c` which #27 rewrites — land
it first to avoid a merge tangle.

- Reads: `docs/improvements-roadmap.md` §P2, `docs/converter-perf.md` Finding 7
- Files: `media/src/evo_direct_mem.c`, `pp/src/pp_videoout.c`, `pp/src/pp_playback.c`, `media/src/evo_playback.c` (`VIDEO_ROTATE_BUFFERS`), `main.c`
- **Branch `feat/6-video-buffers-direct-mem` (PR #54) — hardware-verified 2026-09-03:**
  rotate ring 8→3 + `evo_direct_mem` slab (grow-only). `P8_31_RETURN_OK` logs
  `dmem=`. 1080p ✅, 4K TearsOfSteel ✅.
  - Routing `pp_playback` display / `pp_videoout` `cpu_bufs` through the slab
    + a 192 MiB pool were **backed out** — hung GTA 4K (a latent 4K V3-fallback
    overflow corrupts the *shared* slab). Overflow filed as **#55**.
  - The GTA 4K hang was actually **`90c890b` (#27's unproven sceAgc NV12 present
    path), armed by default in `main.c`**. This PR also gates `pp_agc_init`
    behind `--agc-probe` so the default build uses the CPU V8 converter
    (#31-proven) and the branch is deployable again. Commented on #27.
  - Seek on native 4K → "too demanding" (`0x811d0303` after flush) is
    pre-existing, unrelated — native-decode seek robustness (#32 area).

### 3 · `#4` — **deleted 2026-09-11, folded into `#41` §4**

Nothing to start here. The CPU stopgap it described was made moot by GL-4 (#80)
deleting the converter, P010 sampling + an SDR tone-map shipped in GL-5 (#81),
and the residue — a real PQ/HLG tone-map, HDR output signalling, and the
two-plane 10-bit GL path that would put HEVC Main10 on the hardware decoder —
lives in `#41` section 4 together with the decoder work it depends on.

### 4 · `#27` — GPU Step 2: sceAgc video convert + present — **✅ CLOSED 2026-09-04**

Delivered + hardware-verified (PR #61). GTA 4K plays via sceAgc — decode → NV12 →
GPU YUV→RGB → GPU flip, correct colour, 982 µs/frame (3% of the 30 fps budget),
CPU swizzle off the 4K path. `render_frame` runs on a watchdog'd worker thread; a
wedge/fault re-registers the VO tiled and the CPU path resumes. `--agc-probe`-gated,
default build unchanged. #55 fixed alongside. Leftovers: **#62** (plane-hash A/B),
**#37** (Renderer settings row), **#41** (HEVC + VP9 native decode), **#28** (GPU OSD over 4K).
See `docs/evo-pro/status.md` + `agc-implementation.md` for the full write-up.

### 5 · `#28` — GPU Step 3: complete RmlUi on sceAgc — **unblocked**

Reuses all of #27's plumbing (shader setup, DCB submit, VideoOut, panic
behaviour, all now proven). Deletes the CPU coverage rasteriser.

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
| **#41** | Codec-independent `NativeVideoBackend` — H.264 + HEVC + VP9 all **hardware-verified** on `sceVideodec2` (2026-09-10/11); one interface, common presentation, auto FFmpeg fallback, zero-copy. Left: turn HEVC/VP9 on by default, 4K, 10-bit, codec badge — see the issue's phased plan. Research base: `third_party/ps5-hardware-video-decoding-research/` (FW 6.02+12.70; ignore EVO's in-tree decode docs) | open |
| **#59** | Surface video decoder backend in player UI (Hardware vs Software decode indicator) | open, medium |
| **#32** | 4K native playback: seeking plays audio but video shows black (blank screen on GPU present) | open, critical |

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
| ~~#44~~ | ✅ **CLOSED 2026-09-03.** Per-screen RmlUi parity signed off on hardware; legacy `evo_screens.c`/`evo_chrome.c` deleted (PR2). |
| ~~#16~~ | ✅ **CLOSED 2026-09-03** with #44. RCSS clamps across OSD / dialog / inspector / media-info / settings / changelog / hero. Were shadowed on hardware by a stale `/data/evoplayer/app/assets/` until `edc3a08` (`deploy-app.sh --ffpfsc` now re-syncs assets). |
| **#45** | Icon swap — Lucide concept icons + Kenney controller glyphs (approved, `docs/icon-swap-handoff.md`) | open, low |
| **#35** part B | On-video **caption** rendering off legacy `rr_text` → RmlUi | open |
| **#28** | Bind `Rml::RenderInterface` → `sceAgc` + delete the CPU coverage rasteriser (GPU Step 3, after #27) | open, low |
| ~~#60~~ | ✅ **CLOSED 2026-09-05, hardware-verified.** Make `.ffpfsc` self-contained: embedded RmlUi assets in binary via `evo_rmlui_bundle_data.cpp` (`tools/bundle_rml_assets.py`) + `EvoRmlFileInterface`, `#44`'s out-of-band FTP asset sync deleted from `deploy-app.sh`. Hardware-verified on PPSA99039 with `/data/evoplayer/app/assets/` completely absent. |
| **#74** | Revamp About screen with dedicated RmlUi layout & Codec Capabilities Matrix | code complete (`8a2af88`), pending hw verify |
| **#68** | Enhance UI geometry and styling on GPU: fluid curves, shadows, and analytical anti-aliasing | open, medium |

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
