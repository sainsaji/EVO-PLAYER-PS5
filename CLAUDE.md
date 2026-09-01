# EVO Player

A native C media player for jailbroken PS5 (12.70), built against the PS5
Payload SDK. Currently mid-migration from an immediate-mode SDF UI to RmlUi
(retained-mode C++ HTML/RCSS) — see "Active work" below.

Full docs live in `docs/`. This file is the index plus the rules that are
expensive to relearn by trial and error on real hardware. Read the linked doc
before doing deep work in its area instead of exploring the tree cold.

---

## Rules that must not be broken

- **Never call `make` directly.** The Makefile is missing FFmpeg's transitive
  dependency list. Use `scripts/build-evoplayer.sh`. → [docs/tooling.md](docs/tooling.md)
- **Never stack payload launches.** The app slot stays resident; launching
  again *adds* an instance instead of replacing one. Ten stacked launches
  kernel-panicked the console once and cost ~50 minutes. Always launch through
  `tools/launch.sh`, which refuses to pile on top of a recent launch. Only the
  PS button (close app on console) actually frees the slot.
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
- **Payload `printf` is not in klog.** klog is the kernel log only. Payload
  stdout comes back as the HTTP response body from `/hbldr` when `pipe=1` —
  see the curl recipe in [docs/tooling.md](docs/tooling.md#seeing-payload-printf).
  A `curl` timeout (exit 28) on that request is the normal, successful outcome.
- **Everything toolchain-related runs in the pinned Docker container.**
  Scripts under `scripts/` and `tools/` re-exec themselves through
  `docker compose` when run from Windows.

---

## Quick commands

```bash
# build, install, launch, capture a screenshot
docker compose run --rm ps5-dev bash -lc '
  EXTRA_CFLAGS="-DEVO_AUTOSHOT=6" ./scripts/build-evoplayer.sh
  ./scripts/install-homebrew.sh --name EVOPlayer output/elf/EVOPlayer.elf
  ./tools/launch.sh --timeout 12
  ./tools/shot.sh grab'

# watch the console log while you do it
docker compose run --rm ps5-dev ./tools/klog.sh

# render the UI on the host, no console needed (fast iteration loop)
./tools/uiview.sh --all
./tools/uiplay.sh          # then open output/uiplay/index.html, arrow keys to drive it
```

Prefer the host UI renderer (`uiview.sh` / `uiplay.sh`) over a hardware round
trip whenever the question is about layout, navigation or rendering — it links
the real drawing code against the real assets. Go to hardware only when the
question is genuinely about console behavior.

Full command reference, all scripts, screenshot measurement tools
(`shot.sh probe/scan/crop/diff`), env vars: [docs/tooling.md](docs/tooling.md).

---

## Repo layout

```
projects/evoplayer/
  main.c        the player: FFmpeg, threads, input, screens, state
  media/        subsystems carved out of main.c (own state/threads, narrow interface)
  pp/           playback backend: VideoOut, converters, clocks, theme
  ui/           legacy immediate-mode screens/widgets/chrome (no FFmpeg, no VideoOut)
  ui_rml/       RmlUi integration: app.cpp, bridge.cpp, render.cpp (the active work)
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
| [architecture.md](docs/architecture.md) | Layer boundaries, why `main.c` is still large |
| [tooling.md](docs/tooling.md) | Every script, launch safety, screenshot measurement, klog |
| [building.md](docs/building.md) | Full dev environment setup, SDK, FFmpeg, packaging |
| [rmlui-integration-guide.md](docs/rmlui-integration-guide.md) | RmlUi migration spec (active work) |
| [ui-handoff.md](docs/ui-handoff.md) | Legacy UI layer, what's covered by `uiplay.sh` |
| [theming.md](docs/theming.md) | Theme/color system |
| [hardware-decode.md](docs/hardware-decode.md) / [-review.md](docs/hardware-decode-review.md) | Hardware decoder investigation, panic vectors |
| [native-decode-integration-plan.md](docs/native-decode-integration-plan.md) | Proposed: native decode via fake-SELF app slot, SharpProspero as ABI ref, FFmpeg/Native settings toggle |
| [gpu-notes.md](docs/gpu-notes.md) | Why there's no hardware GL driver |
| [converter-perf.md](docs/converter-perf.md) | YUV→BGRA+swizzle perf, `bench.sh` findings |
| [networking.md](docs/networking.md) | Console services, jailbreak-lapsed symptoms |
| [media-tile.md](docs/media-tile.md) | Media tile / metadata handling |
| [addons-emby-nuvio.md](docs/addons-emby-nuvio.md) | Emby/Nuvio addon integration |
| [packaging.md](docs/packaging.md) | PKG packaging |
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
  or navigation — it's the same drawing code, with no console risk and no
  90s launch cooldown.
