# EVO Player

A native media player for jailbroken PS5 (FW 12.70), built against the PS5
Payload SDK and shipped as an app module (`.ffpfsc`, title `PPSA99039`). The UI
is RmlUi on bare-metal `sceAgc`; decode is `sceVideodec2` with an FFmpeg
fallback.

This file holds the rules that are expensive to relearn on real hardware. The
full docs are in `docs/` ([docs/README.md](docs/README.md)). Read the linked
doc before working deep in an area.

**Replies:** Reply in plain, simple words: the result first, then the next step. No tables or extra detail unless I ask.

**Test on the console yourself.** After any fix, run it on the PS5 yourself: `evo-remote.sh quit` → `close` → deploy → `launch`, then drive it with `screen` / `key` and read where focus went from `key`'s output or `ui`. Use `key l3` + `tools/shot.sh grab` only when you need to see pixels (layout, colour, rendering). Never ask me to test what you can test. → [tooling.md#hardware-cycle](docs/build/tooling.md#hardware-cycle)

**Implementing a GitHub issue?** Start at [roadmap.md](docs/planning/roadmap.md).
Each issue body also has a "References & sequencing" block.

---

## Rules that must not be broken

- **Never call `make` directly.** The Makefile is missing FFmpeg's transitive
  dependencies. Use `scripts/package-app.sh` for the real build, or
  `scripts/build-evoplayer.sh` for a compile check only.
- **Everything toolchain-related runs in the pinned Docker container.**
  Scripts under `scripts/` and `tools/` re-run themselves through
  `docker compose` when started from Windows. From Git Bash, call
  `docker compose run --rm -e PS5_HOST=... ps5-dev ./tools/...` directly, and
  set `MSYS_NO_PATHCONV=1` when you pass a container path.
- **One hardware path: the `.ffpfsc` app module.** Build with `package-app.sh
  --ffpfsc`, deploy with `deploy-app.sh --ffpfsc`, then start it with
  `evo-remote.sh launch`. ShadowMount+ v1.7 does NOT auto-launch an updated
  image. Never push an EVO ELF. For layout questions use the host renderer
  (`uiview.sh`), not the console.
- **Never deploy over a running EVO, and never stack launches.** Both have
  kernel-panicked the console. Close it with `evo-remote.sh quit` (soft close:
  stop media, drain the GPU, park), then `evo-remote.sh close`. `close` is a
  kill, the same as the PS button, so the script only sends it once
  `evo_status` says `parked=1`. Never send the controllers in
  `tools/ps5-controllers/` by hand. A kill while the GPU is still submitting
  panicked the console on 2026-09-18.
- **Lifecycle truth is ShadowMount's `/data/shadowmount/debug.log`**
  (`[GAME] started` / `runtime layers released`), not an exit code. Run one
  cycle at a time, stop at the first anomaly, and never auto-retry.
- **Never sweep kernel `.text`** (`kernel_copyout` over a range). It panics the
  console every time.
- **Never call `sceVideoOutOpen` from a payload.** It returns a bogus handle
  and panics once a compute queue is allocated.
  → [hardware-decode.md](docs/hardware/hardware-decode.md)
- **Do not kill `kstuff`.** It destabilises the console into a panic.
- **Remote Play (chiaki-ng) works on this console** (paired and streamed
  2026-10-07; an earlier note said a jailbroken PS5 cannot pair, which was
  wrong). It needs no PSN: the account was offline-activated with `offact` and
  paired with **LinkDev** (a websrv app) and chiaki-ng v1.10.0 on the PC
  (`tools/chiaki.ps1`). The steps, the result and the pitfalls are in
  [remote-play.md](docs/build/remote-play.md). It gives the real screen and real
  pad input, so it reaches the system dialogs that `evo-remote.sh key` cannot
  (the native IME, the web browser). Still: don't start a stream or any console
  action the owner has not asked for, and close EVO (`quit`, then `close`)
  before sending payloads. The pairing survives power cuts only if the account
  stays activated; the console's address changes (`ps5-remoteplay status`).
  `evo-remote.sh key` itself still only injects into EVO's own pad state; for
  tests that type text without Remote Play, set Settings → Interface & Storage
  → `KEYBOARD INPUT` = `VIRTUAL KEYBOARD`.
- **The console's `/fs` web route is read-only.** Delete over FTP
  (`tools/shot.sh clean`).
- **Diagnostics: one file, `/mnt/usb0/evo.log`.** It holds the boot trace,
  breadcrumbs, decoder notes and per-file stats. klog gets the same lines live.
  `evo-remote.sh log` pulls it. `evo_status` is the live one-line state
  (`--usb-remote` builds only). A `timeout`/`curl` timeout on an
  `evo-remote.sh` call is a normal, successful outcome.
- **Framebuffer format is `0xAABBGGRR`** (BGRA in memory). Get it wrong and the
  colours swap silently; nothing crashes.

---

## Quick commands

```bash
# build + deploy (always --usb-remote for testing: enables evo-remote)
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc   # sha256-verified

# drive the console (PS5_HOST=192.168.0.x)
./tools/evo-remote.sh launch | quit | close | status | log
./tools/evo-remote.sh screen <id> | key <button>... | play <path>   # key l3 = screenshot
./tools/evo-remote.sh ui [--line]       # screen + focused element + modal/toast as text (#115)
./tools/shot.sh grab                                                 # -> output/screenshots/latest.png
./tools/evo-remote.sh cycle --secs 60   # whole loop, classified result in output/cycles/

# host UI renderer - any layout question, no console
./tools/uiview.sh --all                 # every RmlUi screen -> output/uiview/rml_*.png
EVO_UIVIEW_IPTV_SWEEP=<m3u> ...         # walk every IPTV group with the D-pad (see the .cpp)

# live health dashboard (memory, buffer, network) - on the HOST -> http://localhost:8790
python tools/evo-dash.py

# serve provider bundles / a playlist TO the console - on the HOST, not the container
./tools/provider-server.sh

# compile check only, never deploys
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh
```

All scripts, screenshot measurement (`shot.sh probe/scan/crop/diff`) and env
vars: [tooling.md](docs/build/tooling.md).

---

## Repo layout

```
projects/evoplayer/
  main.cpp       entry point only: crash/SIGTERM handlers -> Application::run()
  main.c.legacy  the old monolith. NOT COMPILED. Don't edit it or read it to
                 learn current behaviour; grep core/ instead.
  core/          the player: Application.cpp (frame loop, soft close), screens/,
                 services/ (PlaybackController, settings, metadata)
  media/         decode, audio, evo_agc_runtime.c (GPU present + VideoOut)
  pp/            pace, presentation clock, seek
  ui/            nav/focus/input primitives + evo_keyboard state
  ui_rml/        the RmlUi UI: evo_rmlui_app/bridge/render.cpp, the sceAgc
                 render interface, evo_rmlui_provider*.cpp (provider screens)
  addons/        evo_net (HTTP/TLS), the provider seam, provider_iptv/xtream/
                 emby/jellyfin/nuvio -> docs/addons/provider-architecture.md
  assets/rml/    .rml/.rcss, embedded into the binary at package time
  src/           evo_usb_remote.c (the evo_cmd / evo_status dev remote)
scripts/         build + deploy
tools/           evo-remote, evo_lifecycle.py, ps5-controllers/, uiview, shot, klog
assets/providers/ provider UI bundles, served, not embedded
```

Layer boundaries: [architecture.md](docs/architecture/architecture.md).

---

## Key docs

- [tooling.md](docs/build/tooling.md): every script, the hardware cycle, launch safety
- [evo-pro/status.md](docs/evo-pro/status.md): resume here for decode/GPU work
- [memory-budget.md](docs/hardware/memory-budget.md): direct 12 GB, flexible
  448 MB. Read it before calling EVO memory-limited, and check `map_fail` in
  `evo.log` before blaming a codec.
- [agc-bare-metal-ui.md](docs/evo-pro/agc-bare-metal-ui.md) +
  [shader-compilation.md](docs/hardware/shader-compilation.md): the sceAgc UI and the shader toolchain
- [ui/hui-kit.md](docs/ui/hui-kit.md): EVO's own screens on ps5-homebrew-ui (draw lists → sceAgc),
  the RmlUi fallback, and `tools/hui_preview.sh` (host render of every screen)
- [provider-architecture.md](docs/addons/provider-architecture.md): the provider seam (#90)
- [upscaler.md](docs/hardware/upscaler.md): FSR1 / Anime4K (#103)
- [proprietary.md](docs/proprietary.md): licences, including the vendored GPL controllers
- Everything else: [docs/README.md](docs/README.md)

---

## Working efficiently

- **Search before you grep.** The `evo_search` tool (MCP server `evo-rag`,
  [tools/rag/](tools/rag/README.md)) searches the code, docs, every GitHub issue
  and the memory notes by meaning or by name. Ask it first ("why can't we call
  sceVideoOutOpen", "where is seek handled") and then Read the `path:lines` it
  returns. Fall back to Grep for an exact string you already know.
- **Read narrow.** `evo_rmlui_app.cpp` (4k lines), `uiview_playback_rml.cpp`
  and `core/Application.cpp` are large. Grep first, then read only that range.
- **Don't re-read a file right after editing it.**
- **Batch console work:** run several `key` presses in one `docker compose run
  ... bash -c`. Each container start costs seconds.
- **On Windows, write multi-line patch scripts to a file and run
  `python3 -X utf8 file.py`.** Heredocs with quotes or em dashes break in Git
  Bash. A PreToolUse hook (`.claude/hooks/block-heredoc-escapes.py`) blocks a
  Bash heredoc that has backslash escapes or feeds python: use the Write tool.
- **`evo-remote.sh launch` waits for ShadowMount to go idle** (log quiet 15 s)
  first. Launching mid-scan after a deploy gives "Can't start game or app";
  only an X press on the pad clears it.
