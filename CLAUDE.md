# EVO Player

A native C media player for jailbroken PS5 (12.70), built against the PS5
Payload SDK. Currently mid-migration from an immediate-mode SDF UI to RmlUi
(retained-mode C++ HTML/RCSS) — see "Active work" below.

Full docs live in `docs/`. This file is the index plus the rules that are
expensive to relearn by trial and error on real hardware. Read the linked doc
before doing deep work in its area instead of exploring the tree cold.

**Implementing a GitHub issue?** Start at [docs/roadmap.md](docs/planning/roadmap.md) —
the dependency-ordered plan; each issue body also carries its own
"References & sequencing" block (docs + files to read).

---

## Rules that must not be broken

- **Never call `make` directly.** The Makefile is missing FFmpeg's transitive
  dependency list. Use `scripts/build-evoplayer.sh` (host compile check) or
  `scripts/package-app.sh` (the real build). → [docs/tooling.md](docs/build/tooling.md)
- **One hardware path: the `.ffpfsc` app module.** `scripts/package-app.sh
  --ffpfsc` + `scripts/deploy-app.sh --ffpfsc`, launched from the Games row
  (ShadowMountPlus auto-launches on the `.ffpfsc` change). There is no other
  deploy path; `build-evoplayer.sh` is a compile check only.
- **Never deploy over a running EVO** (panic risk) and **never stack launches**
  — the app slot stays resident; stacking has kernel-panicked the console
  (~50 min lost). **Close with Settings → System & Diagnostics → QUIT EVO**,
  not the PS button: PS-close kills the process, so `Application::shutdown()`
  never runs and the kernel reclaims VideoOut + the 64 MB direct-memory pool
  under a possibly-still-submitting GPU — that panicked the console on
  2026-09-18. QUIT + the `agc_wait_gpu_idle()` drain are **hw-verify-pending**;
  the PS button remains the fallback. → [docs/tooling.md](docs/build/tooling.md)
- **Never sweep kernel `.text`** (`kernel_copyout` over a range). Panics the
  console every time; this is why the `kdump` project no longer exists.
- **Never call `sceVideoOutOpen` from a payload.** Returns a handle that
  passes a `< 0` check but is bogus, and panics the console once a compute
  queue is allocated afterward. → [docs/hardware-decode.md](docs/hardware/hardware-decode.md)
- **Do not kill `kstuff`.** Destabilizes the console into a panic. Kernel R/W
  works fine with it left running.
- **The console's `/fs` web route is read-only**, no `DELETE`. Don't attempt
  to script deletion of USB screenshots through it — `tools/shot.sh clean`
  explains the two routes that actually work.
- **App-module diagnostics: one file — `/mnt/usb0/evo.log`.** Every build
  writes it (boot trace + playback breadcrumbs + decoder notes + per-file
  stats, all timestamped). `klog` also gets the same lines live via
  `sceKernelDebugOutText` (`-DEVO_APP_MODULE`); popups only with `--breadcrumbs`.
  `tools/evo-remote.sh log` pulls `evo.log` over FTP → `output/logs/evo.log`.
  A `timeout`/`curl` timeout on an `evo-remote.sh` call is the normal,
  successful outcome. (`evo_status` — a live one-line state snapshot for the
  dev remote — is the only other file, `--usb-remote` builds only.)
- **Everything toolchain-related runs in the pinned Docker container.**
  Scripts under `scripts/` and `tools/` re-exec themselves through
  `docker compose` when run from Windows.
- **The app module is the whole story.** `.ffpfsc` (`scripts/package-app.sh`
  → `scripts/deploy-app.sh` → ShadowMountPlus, TITLE_ID `PPSA99039`) is the
  release path *and* the only thing you ever deploy — it has `sceVideodec2`
  decode, `sceAgc` GPU, audio, a real user session, the self-unjail for
  `/data`. For a UI/layout question use the **host renderer** (`uiview.sh` /
  `uiplay.sh`), not a console.
  → [docs/tooling.md](docs/build/tooling.md#ffpfsc--the-only-route)

---

## Quick commands

```bash
# DEPLOY - the ONLY hardware path (app module, PPSA99039)
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc     # + --usb-remote for the FTP dev remote
  ./scripts/deploy-app.sh --ffpfsc'     # deploy also clears the /mnt/usb0 logs
# ShadowMountPlus re-mounts + auto-launches on the .ffpfsc change; otherwise
# launch PPSA99039 from the Games row. PS-button-close a running EVO first.
# Bare-metal sceAgc is the only render path (docs/evo-pro/agc-bare-metal-ui.md).
# `--agc` is accepted but redundant; `--gl`/`--no-gl`/`--gl-smoke`/`--gl-hdr-probe`
# and the ps5-opengl submodule are gone and now fail with that explanation.
# Diagnostics = /mnt/usb0/evo.log (one file) + klog live; popups with --breadcrumbs.
# Unattended: tools/evo-remote.sh  (build/play/seek/status/boot over FTP).

# COMPILE CHECK ONLY - keeps the non-app-module path green (#31/#36/modularisation)
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh   # never deploys

# render the UI on the host, no console needed (use this for any layout question)
./tools/uiview.sh --all    # every RmlUi screen -> output/uiview/rml_*.png
# ...including the provider screens: the harness starts a loopback HTTP server
# and drives the REAL provider (evo_net, M3U parse, bundle fetch, data binding,
# D-pad focus) - rml_provider_{iptv,iptv_focus,iptv_channels,fallback}.

# serve provider UI bundles + a test playlist TO THE CONSOLE (#90).
# Runs on the HOST, never in the container: the PS5 cannot reach a container
# listener on Windows bridge networking (docs/hardware/networking.md).
./tools/provider-server.sh
./tools/uiplay.sh          # contact sheet of them all -> output/uiplay/index.html

# watch the console log
docker compose run --rm ps5-dev ./tools/klog.sh
```

Prefer the host UI renderer (`uiview.sh` / `uiplay.sh`, both now the RmlUi
harness `tools/uiview_playback_rml`) over a hardware round trip whenever the
question is about layout or rendering — it links the real RmlUi documents,
stylesheets and assets. Add a fixture to `tools/uiview_playback_rml.cpp` for a
screen or cursor state it doesn't cover. Go to hardware only when the question
is genuinely about console behavior (theme repaint, overlay-over-video, input
timing).

Full command reference, all scripts, screenshot measurement tools
(`shot.sh probe/scan/crop/diff`), env vars: [docs/tooling.md](docs/build/tooling.md).

---

## Repo layout

```
projects/evoplayer/
  main.cpp      entry point only (~96 lines): crash/SIGTERM handlers, then
                evo::Application::run(). NOT where the player lives.
  main.c.legacy the old 11.5k-line monolith. NOT COMPILED, not in any build —
                kept for reference while the carve-up finishes. Do not edit it
                expecting a behaviour change, and do not read it to learn
                current behaviour; grep core/ instead.
  core/         the player: Application.cpp (frame loop, shutdown), screens/,
                services/ (CoverArtService, settings, metadata), fsm/
  media/        subsystems carved out of the legacy main.c (own state/threads,
                narrow interface)
  pp/           playback: pace + presentation clock + seek (pp_playback),
                theme. The CPU converters, tile_copy, the V8/V3/1080 backend
                dispatch and pp_agc*/pp_videoout/pp_platform.h are all deleted —
                media/src/evo_agc_runtime.c owns present + VideoOut.
  ui/           shared primitives: nav/focus/input/feedback/layout + evo_keyboard
                (D-pad/buffer state; its immediate-mode renderer is gone,
                evo_draw/evo_widgets with it). Screen renderers (evo_screens.c/
                evo_chrome.c) deleted in #44 — every screen draws through ui_rml.
  ui_rml/       RmlUi integration: app.cpp, bridge.cpp, render.cpp — the UI —
                plus evo_rmlui_render_agc.cpp, the sceAgc render interface.
                Off-device the same sources build against the CPU rasteriser.
                src/rmlui_patch/ is one upstream RmlUi TU with a finer corner
                tessellation, swapped into librmlui.a at package time (#68)
                evo_rmlui_provider*.cpp is the provider seam's render half:
                its own Rml context, RmlUi data binding + native focus, and
                remote artwork into the evo:mem/ registry (#90)
  addons/       the network half: evo_net (HTTP/TLS), cJSON, and the provider
                seam — evo_provider_t, the registry + resolver chain, the UI
                bundle fetch/verify/cache, provider_iptv, provider_emby.
                A provider's LOOK is not here: it is fetched at runtime.
                → docs/addons/provider-architecture.md
  assets/rml/   .rml/.rcss documents for the RmlUi screens
scripts/        build/deploy — see docs/tooling.md
tools/          uiview, klog, shot, evo-remote, gen_icons, provider-server
                — see docs/tooling.md
assets/providers/
                provider UI bundles, SERVED not embedded. Checked in only
                because IPTV is the provider #90 proves the seam against; a
                third-party bundle lives on that provider's server (#90)
docs/           everything below
```

Full rationale for the layer boundaries, and how much of the legacy `main.c`
is still to be absorbed: [docs/architecture.md](docs/architecture/architecture.md).

---

## Active work: RmlUi migration (`feat/rmlui-native-integration`)

Replacing the legacy SDF renderer in `ui/` with RmlUi, screen by screen, with
a hard constraint of 100% parity with `main`'s stability and behavior. Zero
mock data — everything in the DOM binds to live C structs
(`EVOPlayerState`, `evo_file_entry_t`, `evo_settings_t`, ...) through
`evo_rmlui_bridge.cpp`.

- Spec and architecture diagram: [docs/rmlui-integration-guide.md](docs/ui/rmlui-integration-guide.md)
- C++ sources: `projects/evoplayer/ui_rml/src/{evo_rmlui_app,evo_rmlui_bridge,evo_rmlui_render}.cpp`
  (`evo_rmlui_app.cpp` is ~1900 lines — grep for the screen/function you need
  rather than reading it whole)
- Host preview tool for this path: `tools/uiview_playback_rml.cpp` / `.sh`
- Framebuffer format is `0xAABBGGRR` (BGRA in memory) — get this wrong and
  colors are silently swapped, not crashed, so it won't show up as an error.

---

## docs/ index

Grouped into `build/`, `architecture/`, `ui/`, `hardware/`, `planning/`,
`research/` and `addons/` — [docs/README.md](docs/README.md) is the navigable
version of this table.

| Doc | What's in it |
|---|---|
| [roadmap.md](docs/planning/roadmap.md) | **Issue implementation order + per-story doc/file references.** Start here for any GitHub issue. |
| [project-tracking.md](docs/planning/project-tracking.md) | The "EVO Player Roadmap" GitHub Project board — setup script, field↔label map, views, auto-add workflows |
| [architecture.md](docs/architecture/architecture.md) | Layer boundaries; what still remains in the un-compiled `main.c.legacy` |
| [tooling.md](docs/build/tooling.md) | Every script, launch safety, screenshot measurement, klog |
| [building.md](docs/build/building.md) | Full dev environment setup, SDK, FFmpeg, packaging |
| [rmlui-integration-guide.md](docs/ui/rmlui-integration-guide.md) | RmlUi migration spec (active work) |
| [rmlui-parity.md](docs/ui/rmlui-parity.md) | **#44 per-screen RmlUi-vs-`main` parity checklist** + #16 text-clamp status + marquee scope |
| [ui-handoff.md](docs/ui/ui-handoff.md) | Legacy UI layer, what's covered by `uiplay.sh` |
| [theming.md](docs/ui/theming.md) | Theme/color system |
| [memory-budget.md](docs/hardware/memory-budget.md) | **The title's memory, measured: direct 12 GB (at least 8 GB usable by EVO, probed 2026-09-26), flexible 448 MB.** 4K software decode runs flexible memory dry, so the malloc shim takes blocks of 1 MB and up from direct memory (type 11 - type 3 is uncached). Read this before saying EVO is memory-limited, and check `map_fail` in `evo.log` before blaming a codec |
| [hardware-decode.md](docs/hardware/hardware-decode.md) / [-review.md](docs/hardware/hardware-decode-review.md) | Hardware decoder investigation, panic vectors |
| [evo-pro/agc-bare-metal-ui.md](docs/evo-pro/agc-bare-metal-ui.md) | **The RmlUi UI on bare-metal `sceAgc`** (hw-verified 2026-09-12) — `.pipe` + amdllpc shader toolchain, the gfx1013 LLPC patch, the silent-failure bugs and the `agc health` lines that verify a build. `--agc` builds only; video present is still unported. |
| [evo-pro/](docs/evo-pro/README.md) | **EVO Pro program** — app-module repackage + hardware decode + GPU rendering. **Resume-here: [evo-pro/status.md](docs/evo-pro/status.md)**. **#31 native 4K decode DONE + closed** (GTA plays on `sceVideodec2` — `media/src/evo_vdec_native.c`). Test loop: `tools/evo-remote.sh` (scriptable `play`/`seek`/`boot` over FTP). Also: [native-decode-plan.md](docs/evo-pro/native-decode-plan.md) (master plan), [videodec2-abi.md](docs/evo-pro/videodec2-abi.md) (Route B ABI), [gpu-rendering-plan.md](docs/evo-pro/gpu-rendering-plan.md) + [agc-implementation.md](docs/evo-pro/agc-implementation.md) (**historical** — the hand-rolled sceAgc path that preceded today's runtime) + [sharpprospero-agc-reference.md](docs/evo-pro/sharpprospero-agc-reference.md) (AGC ABI), [phase-1b-app-module.md](docs/evo-pro/phase-1b-app-module.md) |
| [shader-compilation.md](docs/hardware/shader-compilation.md) | The `.pipe` → amdllpc → PAL-metadata shader toolchain (gfx1013), and how a compiled pipeline is fed to `sceAgc` |
| [upscaler.md](docs/hardware/upscaler.md) | **#103 video upscaler** (Settings → UPSCALING: Off / Sharp = FSR1 / AI = Anime4K CNN in shaders), PS5 Pro detection (`evo_hw`), the unrun PSML spike, and the hardware checklist. hw-verify-pending |
| [gpu-notes.md](docs/hardware/gpu-notes.md) | The GPU reverse-engineering history behind the bare-metal sceAgc runtime |
| [converter-perf.md](docs/research/converter-perf.md) | **History** — the CPU YUV→BGRA converters and `bench.sh`, both deleted when the GPU took over present. Kept for the BT.601 reference matrix |
| [networking.md](docs/hardware/networking.md) | Console services, jailbreak-lapsed symptoms |
| [media-tile.md](docs/ui/media-tile.md) | Media tile / metadata handling |
| [provider-architecture.md](docs/addons/provider-architecture.md) | **The provider seam (#90)** - the vtable, the runtime UI bundle format, the RCSS/binding rules a bundle must follow, and the playback identity |
| [packaging.md](docs/build/packaging.md) | PKG packaging (app-module `.ffpfsc` is in [tooling.md](docs/build/tooling.md#ffpfsc--the-only-route)) |
| [validation.md](docs/build/validation.md) | Validation checklist |
| [modularisation-plan.md](docs/architecture/modularisation-plan.md) | `main.c` carve-up — in progress; Track A is the decoder seam that unblocks native decode |
| [backlog.md](docs/planning/backlog.md) / [improvements-roadmap.md](docs/planning/improvements-roadmap.md) | Planning docs, not current state |
| [icon-swap-handoff.md](docs/ui/icon-swap-handoff.md) | RmlUi icon swap to Lucide/Kenney — candidates approved, not yet implemented |
| [prosperoplayer-baseline.md](docs/research/prosperoplayer-baseline.md) / [reng-analysis-integration.md](docs/research/reng-analysis-integration.md) / [native-media-research.md](docs/research/native-media-research.md) / [sdk-audit.md](docs/research/sdk-audit.md) / [baseline-defects.md](docs/research/baseline-defects.md) | Upstream baseline research |
| [proprietary.md](docs/proprietary.md) | Licensing notes |

---

## Working efficiently in this repo

- **Read narrow.** `evo_rmlui_app.cpp`, `uiview_playback_rml.cpp` and
  `core/Application.cpp` are all 1000+ lines. Grep for the symbol/screen first, then read
  just that range, instead of reading the whole file.
- **Don't Read a file right after Edit-ing it** — a successful Edit already
  confirmed the change; re-reading just to check burns tokens for nothing.
- **Skip agents for known-location lookups.** If the file is already known
  (e.g. "find X in evo_rmlui_app.cpp"), a direct Grep beats spawning an
  Explore/general-purpose agent.
- **Batch hardware verification.** One launch that captures several screens
  beats one launch per screen — and hardware round trips are the slow, risky
  part of the loop anyway (see the launch-safety rule above).
- **Prefer `uiview.sh`/`uiplay.sh` over hardware** for anything about layout
  or rendering — it's the same RmlUi code and assets, with no console risk and
  no 90s launch cooldown.
