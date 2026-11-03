# EVO Player

A native media player for jailbroken PS5 (FW 13.60), built against the PS5
Payload SDK and shipped as an app module (`.ffpfsc`, title `PPSA99039`). The UI
is RmlUi on bare-metal `sceAgc`; decode is `sceVideodec2` with an FFmpeg
fallback.

This file holds the rules that are expensive to relearn on real hardware. The
full docs are in `docs/` ([docs/README.md](docs/README.md)). Read the linked
doc before working deep in an area.

**Replies:** Reply in plain, simple words: the result first, then the next step. No tables or extra detail unless I ask.

**Test on the console yourself.** After any fix, run it on the PS5 yourself: `evo-remote.sh quit` → `close` → deploy → `launch`, then drive it with `screen` / `key` and read where focus went from `key`'s output or `ui`. Use `key l3` + `tools/shot.sh grab` only when you need to see pixels (layout, colour, rendering). Never ask me to test what you can test. → [hardware-cycle.md](docs/console/hardware-cycle.md)

**Use agy first.** Before editing code for any issue, ask: is this mostly
mechanical (a settings row, a screen from a worked example, boilerplate, host
tests, reading big logs or trees)? If yes, write the agy task and run it; do not
write it yourself. Only hardware decisions, console work, and review stay with
Claude. State "agy: yes/no, because ..." in your first reply on an issue.
→ [agy.md](docs/build/agy.md)

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
  (`hui_preview.sh` for the kit screens, `uiview.sh` for RmlUi), not the console.
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
  → [video-decode.md](docs/hardware/video-decode.md)
- **Do not kill `kstuff`.** It destabilises the console into a panic.
- **Remote Play (chiaki-ng) works** (paired 2026-10-07, offline-activated, no
  PSN). It reaches the system dialogs that `evo-remote.sh key` cannot (native
  IME, web browser). Don't start a stream or any console action the owner has
  not asked for, and close EVO (`quit`, then `close`) before sending payloads.
  Steps and pitfalls: [remote-play.md](docs/console/remote-play.md). For tests
  that type text without it, set Settings → Interface & Storage →
  `KEYBOARD INPUT` = `VIRTUAL KEYBOARD`.
- **Bound every console-facing call with a timeout.** A resident payload never
  closes the elfldr socket, so an unbounded call hangs.
- **No commits until verified.** Keep iteration uncommitted; land one clean
  commit after build, tests and the hardware check pass. "Push to PS" means
  build + deploy only, no status poll loop.
- **The console's `/fs` web route is read-only.** Delete over FTP
  (`tools/shot.sh clean`).
- **Diagnostics: one file, `/mnt/usb0/evo.log`.** It holds the boot trace,
  breadcrumbs, decoder notes and per-file stats. klog gets the same lines live.
  `evo-remote.sh log` pulls it. `evo_status` is the live one-line state
  (`--usb-remote` builds only). A `timeout`/`curl` timeout on an
  `evo-remote.sh` call is a normal, successful outcome.
- **App-module code traps.** Never `#include <iostream>` (static init
  null-derefs at load, no log). Only call libc functions EVO already uses: a
  missing one (e.g. `strcasestr`) is a null import and SIGSEGVs at address 0.
- **Write container scripts with the Write tool.** Python `write_text` on
  Windows adds CRLF and breaks them inside the container.
- **Raise `contentVersion` in `param.json` every release** (0.11.0 →
  `01.011.000`), or ProsperoStore never offers the update.
- **Framebuffer format is `0xAABBGGRR`** (BGRA in memory). Get it wrong and the
  colours swap silently; nothing crashes.

---

## Quick commands

```bash
# build + deploy (always --usb-remote for testing: enables evo-remote)
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc   # sha256-verified

# fast path: reuse one container (~0.7 s per call instead of several s). Prefix any
# container command with ./tools/dx.sh; ./tools/dx.sh --stop before `docker compose build`
./tools/dx.sh ./tools/evo-remote.sh status

# drive the console (PS5_HOST=192.168.0.x)
./tools/evo-remote.sh launch | quit | close | status | log
./tools/evo-remote.sh screen <id> | key <button>... | play <path>   # key l3 = screenshot
./tools/evo-remote.sh ui [--line]       # screen + focused element + modal/toast as text (#115)
./tools/shot.sh grab                                                 # -> output/screenshots/latest.png
./tools/evo-remote.sh cycle --secs 60   # whole loop, classified result in output/cycles/

# host UI renderer - any layout question, no console
./tools/hui_preview.sh                  # every kit screen -> output/uiview/hui_*.png
./tools/uiview.sh --all                 # every RmlUi screen -> output/uiview/rml_*.png

# compile check only, never deploys
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh
```

All scripts, the dev dashboard (`evo-dash.py`), `provider-server.sh` (runs on the
HOST), screenshot measurement and env vars: [tooling.md](docs/build/tooling.md).

---

## Repo layout

```
projects/evoplayer/
  main.cpp       entry point only: crash/SIGTERM handlers -> Application::run()
  main.c.legacy  old monolith, NOT COMPILED: don't edit or read it.
  core/          the player: Application.cpp (frame loop, soft close), screens/,
                 services/ (PlaybackController, settings, metadata)
  media/         decode, audio, evo_agc_runtime.c (GPU present + VideoOut)
  pp/            pace, presentation clock, seek
  ui/            nav/focus/input primitives + evo_keyboard state
  ui_kit/        EVO's own screens on the ps5-homebrew-ui kit (docs/ui/hui-kit.md)
  ui_rml/        the RmlUi UI: evo_rmlui_app/bridge/render.cpp, the sceAgc
                 render interface, evo_rmlui_provider*.cpp (provider screens)
  addons/        evo_net (HTTP/TLS), the provider seam, provider_iptv/xtream/
                 emby/jellyfin/nuvio -> docs/providers/architecture.md
  assets/rml/    .rml/.rcss, embedded into the binary at package time
  src/           evo_usb_remote.c (the evo_cmd / evo_status dev remote)
scripts/         build + deploy
tools/           evo-remote, evo_lifecycle.py, ps5-controllers/, uiview, shot, klog
assets/providers/ provider UI bundles, served, not embedded
```

Layer boundaries: [architecture.md](docs/architecture/architecture.md).

---

## Key docs

- [hardware-cycle.md](docs/console/hardware-cycle.md): quit, close, deploy, launch, and why
- [tooling.md](docs/build/tooling.md): every script and tool, and where each is explained
- [video-decode.md](docs/hardware/video-decode.md): resume here for decode work
- [memory-budget.md](docs/hardware/memory-budget.md): direct 12 GB, flexible
  448 MB. Read it before calling EVO memory-limited, and check `map_fail` in
  `evo.log` before blaming a codec.
- [agc-runtime.md](docs/hardware/agc-runtime.md) +
  [shader-compilation.md](docs/hardware/shader-compilation.md): the sceAgc UI and the shader toolchain
- [ui/hui-kit.md](docs/ui/hui-kit.md): EVO's own screens on ps5-homebrew-ui (draw lists → sceAgc),
  the RmlUi fallback, and `tools/hui_preview.sh` (host render of every screen)
- [providers/architecture.md](docs/providers/architecture.md): the provider seam (#90)
- [upscaler.md](docs/hardware/upscaler.md): FSR1 / Anime4K (#103)
- [proprietary.md](docs/proprietary.md): licences, including the vendored GPL controllers
- Everything else: [docs/README.md](docs/README.md)

---

## Delegating to agy (saves Claude tokens)

Bulk reading and small code drafts go to the `agy` worker via
`tools/agy-run.sh <name>`. Claude reviews the report and `git diff`, re-runs the
checks, does every console action and commits; agy never touches the console or
git. Never trust its report unverified. Full procedure: [agy.md](docs/build/agy.md).

## Working efficiently

- **Search before you grep.** The `evo_search` tool (MCP server `evo-rag`,
  [tools/rag/](tools/rag/README.md)) searches the code, docs, every GitHub issue
  and the memory notes by meaning or by name. Ask it first ("why can't we call
  sceVideoOutOpen", "where is seek handled") and then Read the `path:lines` it
  returns. Fall back to Grep for an exact string you already know.
- **Check the sibling repos before designing a fix** (decode, audio, A/V sync, GPU,
  provider bugs). `third_party/` holds ~40 PS5 homebrew repos. Ask `evo_search` with
  `source: commits` ("did anyone already fix X") or `source: thirdparty` ("how does
  another app call Y"); narrow with `path:`. Port by hand: check the licence
  ([proprietary.md](docs/proprietary.md)), never paste, verify on the console. Hits on
  EVO's own file names inside Jelly5 etc. are noise. Exact strings: Grep. EVO's own
  history: `git log` / `git blame`.
- **Read narrow.** `evo_rmlui_app.cpp` (4k lines), `uiview_playback_rml.cpp`
  and `core/src/Application.cpp` are large. Grep first, then read only that range.
- **Don't re-read a file right after editing it.**
- **Trim command output.** Pipe builds and logs through `tail` / `grep` for the
  failure lines; never print a full Docker build log.
- **Batch console work:** run several `key` presses in one `docker compose run
  ... bash -c`. Each container start costs seconds.
- **On Windows, write multi-line patch scripts to a file and run
  `python3 -X utf8 file.py`.** Heredocs with quotes or em dashes break in Git
  Bash. A PreToolUse hook (`.claude/hooks/block-heredoc-escapes.py`) blocks a
  Bash heredoc that has backslash escapes or feeds python: use the Write tool.
- **`evo-remote.sh launch` waits for ShadowMount to go idle** (log quiet 15 s)
  first. Launching mid-scan after a deploy gives "Can't start game or app";
  only an X press on the pad clears it.
