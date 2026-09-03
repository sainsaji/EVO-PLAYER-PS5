# EVO Player

A native C media player for jailbroken PS5 (12.70), built against the PS5
Payload SDK. Currently mid-migration from an immediate-mode SDF UI to RmlUi
(retained-mode C++ HTML/RCSS) — see "Active work" below.

Full docs live in `docs/`. This file is the index plus the rules that are
expensive to relearn by trial and error on real hardware. Read the linked doc
before doing deep work in its area instead of exploring the tree cold.

**Implementing a GitHub issue?** Start at [docs/roadmap.md](docs/roadmap.md) —
the dependency-ordered plan; each issue body also carries its own
"References & sequencing" block (docs + files to read).

---

## Rules that must not be broken

- **Never call `make` directly.** The Makefile is missing FFmpeg's transitive
  dependency list. Use `scripts/build-evoplayer.sh` (host compile check) or
  `scripts/package-app.sh` (the real build). → [docs/tooling.md](docs/tooling.md)
- **One hardware path: the `.ffpfsc` app module.** `scripts/package-app.sh
  --ffpfsc` + `scripts/deploy-app.sh --ffpfsc`, launched from the Games row
  (ShadowMountPlus auto-launches on the `.ffpfsc` change). The ELF-payload
  push scripts (`install-homebrew.sh`, `tools/launch.sh`, `scripts/deploy.sh`)
  were **deleted 2026-09-03** — do not recreate them or a deploy path around
  them, not even for a UI check. `build-evoplayer.sh` is a compile check only.
- **Never deploy over a running EVO** (panic risk) and **never stack launches**
  — the app slot stays resident; stacking has kernel-panicked the console
  (~50 min lost). Only the PS button (close app on console) frees the slot.
- **Never sweep kernel `.text`** (`kernel_copyout` over a range). Panics the
  console every time; this is why the `kdump` project no longer exists.
- **Never call `sceVideoOutOpen` from a payload.** Returns a handle that
  passes a `< 0` check but is bogus, and panics the console once a compute
  queue is allocated afterward. → [docs/hardware-decode.md](docs/hardware-decode.md)
- **Do not kill `kstuff`.** Destabilizes the console into a panic. Kernel R/W
  works fine with it left running.
- **The console's `/fs` web route is read-only**, no `DELETE`. Don't attempt
  to script deletion of USB screenshots through it — `tools/shot.sh clean`
  explains the two routes that actually work.
- **App-module diagnostics are on-screen notification popups.** klog only gets
  `sceKernelDebugOutText` lines (`-DEVO_APP_MODULE`); the sandbox has no visible
  stdout. `tools/evo-remote.sh` pulls `/mnt/usb0/evo_*` logs over FTP when the
  build carries `--usb-remote`. A `timeout`/`curl` timeout on an `evo-remote.sh`
  call is the normal, successful outcome.
- **Everything toolchain-related runs in the pinned Docker container.**
  Scripts under `scripts/` and `tools/` re-exec themselves through
  `docker compose` when run from Windows.
- **The app module is the whole story.** `.ffpfsc` (`scripts/package-app.sh`
  → `scripts/deploy-app.sh` → ShadowMountPlus, TITLE_ID `PPSA99039`) is the
  release path *and* the only thing you ever deploy — it has `sceVideodec2`
  decode, `sceAgc` GPU, audio, a real user session, the self-unjail for
  `/data`. The old ELF-payload route (borrowed `/hbldr` process, no graphics,
  errno-5200 decode wall) is gone; its scripts were deleted. For a UI/layout
  question use the **host renderer** (`uiview.sh` / `uiplay.sh`), not a
  console. → [docs/tooling.md](docs/tooling.md#packaging-two-routes)

---

## Quick commands

```bash
# DEPLOY - the ONLY hardware path (app module, PPSA99039)
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc     # add --agc-probe only for #27 GPU Step 2
  ./scripts/deploy-app.sh --ffpfsc'
# ShadowMountPlus re-mounts + auto-launches on the .ffpfsc change; otherwise
# launch PPSA99039 from the Games row. PS-button-close a running EVO first.
# Diagnostics = on-screen notification popups (+ klog for -DEVO_APP_MODULE).
# Unattended: tools/evo-remote.sh  (build/play/seek/status/boot over FTP).

# COMPILE CHECK ONLY - keeps the non-app-module path green (#31/#36/modularisation)
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh   # never deploys

# render the UI on the host, no console needed (use this for any layout question)
./tools/uiview.sh --all    # every RmlUi screen -> output/uiview/rml_*.png
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
(`shot.sh probe/scan/crop/diff`), env vars: [docs/tooling.md](docs/tooling.md).

---

## Repo layout

```
projects/evoplayer/
  main.c        the player: FFmpeg, threads, input, screens, state
  media/        subsystems carved out of main.c (own state/threads, narrow interface)
  pp/           playback backend: VideoOut, converters, clocks, theme
  ui/           shared immediate-mode primitives still used by both paths:
                draw/nav/focus/input/feedback, evo_keyboard (IME modal),
                evo_widgets (toasts), evo_layout (grid/capacity geometry).
                The screen renderers (evo_screens.c/evo_chrome.c) were deleted
                in #44 — every screen now draws through ui_rml.
  ui_rml/       RmlUi integration: app.cpp, bridge.cpp, render.cpp — the UI
  assets/rml/   .rml/.rcss documents for the RmlUi screens
scripts/        build/deploy — see docs/tooling.md
tools/          uiview, klog, shot, bench, gen_icons — see docs/tooling.md
docs/           everything below
```

Full rationale for the layer boundaries, and why `main.c` is still large:
[docs/architecture.md](docs/architecture.md).

---

## Active work: RmlUi migration (`feat/rmlui-native-integration`)

Replacing the legacy SDF renderer in `ui/` with RmlUi, screen by screen, with
a hard constraint of 100% parity with `main`'s stability and behavior. Zero
mock data — everything in the DOM binds to live C structs
(`EVOPlayerState`, `evo_file_entry_t`, `evo_settings_t`, ...) through
`evo_rmlui_bridge.cpp`.

- Spec and architecture diagram: [docs/rmlui-integration-guide.md](docs/rmlui-integration-guide.md)
- C++ sources: `projects/evoplayer/ui_rml/src/{evo_rmlui_app,evo_rmlui_bridge,evo_rmlui_render}.cpp`
  (`evo_rmlui_app.cpp` is ~1900 lines — grep for the screen/function you need
  rather than reading it whole)
- Host preview tool for this path: `tools/uiview_playback_rml.cpp` / `.sh`
- Framebuffer format is `0xAABBGGRR` (BGRA in memory) — get this wrong and
  colors are silently swapped, not crashed, so it won't show up as an error.

---

## docs/ index

| Doc | What's in it |
|---|---|
| [roadmap.md](docs/roadmap.md) | **Issue implementation order + per-story doc/file references.** Start here for any GitHub issue. |
| [project-tracking.md](docs/project-tracking.md) | The "EVO Player Roadmap" GitHub Project board — setup script, field↔label map, views, auto-add workflows |
| [architecture.md](docs/architecture.md) | Layer boundaries, why `main.c` is still large |
| [tooling.md](docs/tooling.md) | Every script, launch safety, screenshot measurement, klog |
| [building.md](docs/building.md) | Full dev environment setup, SDK, FFmpeg, packaging |
| [rmlui-integration-guide.md](docs/rmlui-integration-guide.md) | RmlUi migration spec (active work) |
| [rmlui-parity.md](docs/rmlui-parity.md) | **#44 per-screen RmlUi-vs-`main` parity checklist** + #16 text-clamp status + marquee scope |
| [ui-handoff.md](docs/ui-handoff.md) | Legacy UI layer, what's covered by `uiplay.sh` |
| [theming.md](docs/theming.md) | Theme/color system |
| [hardware-decode.md](docs/hardware-decode.md) / [-review.md](docs/hardware-decode-review.md) | Hardware decoder investigation, panic vectors |
| [evo-pro/](docs/evo-pro/README.md) | **EVO Pro program** — app-module repackage + hardware decode + GPU rendering. **Resume-here: [evo-pro/status.md](docs/evo-pro/status.md)** (top block = current front: **#27 GPU Step 2**, render_frame port). **#31 native 4K decode DONE + closed** (GTA plays on `sceVideodec2` — `media/src/evo_vdec_native.c`). **#27 AGC gate PASSED + `pp_agc_init` shader setup hw-verified** (`pp/src/pp_agc.c`). Test loop: `tools/evo-remote.sh` (scriptable `play`/`seek`/`boot` over FTP — no popup screenshots). Also: [native-decode-plan.md](docs/evo-pro/native-decode-plan.md) (master plan), [videodec2-abi.md](docs/evo-pro/videodec2-abi.md) (Route B ABI), [gpu-rendering-plan.md](docs/evo-pro/gpu-rendering-plan.md) + [agc-implementation.md](docs/evo-pro/agc-implementation.md) (Step 2/3 how-to) + [sharpprospero-agc-reference.md](docs/evo-pro/sharpprospero-agc-reference.md) (AGC ABI), [phase-1b-app-module.md](docs/evo-pro/phase-1b-app-module.md), [avplayer-abi.md](docs/evo-pro/avplayer-abi.md) (Route A — dead) |
| [gpu-notes.md](docs/gpu-notes.md) | Why there's no hardware GL driver |
| [converter-perf.md](docs/converter-perf.md) | YUV→BGRA+swizzle perf, `bench.sh` findings |
| [networking.md](docs/networking.md) | Console services, jailbreak-lapsed symptoms |
| [media-tile.md](docs/media-tile.md) | Media tile / metadata handling |
| [addons-emby-nuvio.md](docs/addons-emby-nuvio.md) | Emby/Nuvio addon integration |
| [packaging.md](docs/packaging.md) | PKG packaging (app-module `.ffpfsc` is in [tooling.md](docs/tooling.md#packaging-two-routes)) |
| [validation.md](docs/validation.md) | Validation checklist |
| [modularisation-plan.md](docs/modularisation-plan.md) | `main.c` carve-up — in progress; Track A is the decoder seam that unblocks native decode |
| [backlog.md](docs/backlog.md) / [improvements-roadmap.md](docs/improvements-roadmap.md) | Planning docs, not current state |
| [icon-swap-handoff.md](docs/icon-swap-handoff.md) | RmlUi icon swap to Lucide/Kenney — candidates approved, not yet implemented |
| [prosperoplayer-baseline.md](docs/prosperoplayer-baseline.md) / [reng-analysis-integration.md](docs/reng-analysis-integration.md) / [native-media-research.md](docs/native-media-research.md) / [sdk-audit.md](docs/sdk-audit.md) / [baseline-defects.md](docs/baseline-defects.md) | Upstream baseline research |
| [proprietary.md](docs/proprietary.md) | Licensing notes |

---

## Working efficiently in this repo

- **Read narrow.** `evo_rmlui_app.cpp`, `uiview_playback_rml.cpp` and
  `main.c` are all 1000+ lines. Grep for the symbol/screen first, then read
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
