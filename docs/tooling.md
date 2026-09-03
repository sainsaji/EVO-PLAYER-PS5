# Tooling

Everything in `scripts/` and `tools/`, and when to reach for it.

Two rules that save time:

- **Never call `make` directly.** The project Makefile does not list FFmpeg's
  transitive dependencies, and static archives carry no dependency metadata, so
  a bare `make` fails at link. `scripts/build-evoplayer.sh` supplies them.
- **Everything runs in the pinned container.** The scripts re-exec themselves
  through `docker compose` when run from Windows, so you can call them either
  way, but the toolchain only exists inside.

---

## The short version

```bash
# the ONLY hardware path — build + deploy the app module
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc'
# ShadowMountPlus re-mounts + auto-launches PPSA99039 on the .ffpfsc change;
# otherwise launch it from the Games row. PS-button-close a running EVO first.

# watch the console log (separate terminal, keeps a record)
docker compose run --rm ps5-dev ./tools/klog.sh

# unattended: build/deploy + drive playback over FTP, pull the boot log
docker compose run --rm ps5-dev bash -lc './tools/evo-remote.sh build --usb-remote'
docker compose run --rm ps5-dev ./tools/evo-remote.sh status

# a UI/layout question? render on the host, no console:
./tools/uiview.sh --all      # -> output/uiview/rml_*.png
```

**There is no ELF-payload deploy loop any more.** `install-homebrew.sh`,
`tools/launch.sh` and `scripts/deploy.sh` were deleted 2026-09-03 — reaching
for them (even for a UI check) kept costing console sessions.
`scripts/build-evoplayer.sh` still exists but is a **host compile check only**;
it hard-errors on `--run`.

> **PS-button-close EVO on the console before every redeploy.** Deploying over
> a running instance risks a panic; stacking launches has kernel-panicked the
> console.

---

## Launching, and why it needs care

Deploy = `deploy-app.sh --ffpfsc` FTPs `PPSA99039.ffpfsc` to
`/data/homebrew/`. The user's **ShadowMountPlus re-mounts and auto-launches**
it on the file change — the only non-manual relaunch. Otherwise the user
launches PPSA99039 from the **Games row**. There is **no remote launch or kill**
for a fake-signed title.

**The app slot stays resident.** Launching again does not replace the running
instance — it adds one, and each opens videoout, an audio port, the pad and
decoder threads. Stacking them kernel-panicked the console on 2026-08-09
(~50 min lost). **Only the PS button → close the application** frees the slot,
and you must do that **before** the next deploy — deploying over a live EVO can
panic.

Two habits:

- **Batch verification.** One launch that captures several screens beats one
  launch per screen.
- **Iterate in the container.** Building and syntax-checking cost nothing.
  Go to hardware when there is something real to see.

`curl` / `timeout` reporting a timeout (exit 28 / 124) on an `evo-remote.sh`
call is the normal, successful outcome — the far end holds the socket.

---

## Packaging: the app module

### `.ffpfsc` — the only route

`scripts/package-app.sh` → `scripts/deploy-app.sh` → mount + launch from the
**Games row** with ShadowMountPlus. Builds a fake-signed **game-category app
module** (`PPSA99039`): full `param.json`, a carried clean-room `libc.prx`,
the whole EVO player.

This is the **only context with real system access** — `sceVideodec2`
hardware decode, `sceAgc` GPU rendering, a proper user session, the full
sandbox. A borrowed-process payload (below) reaches none of that; the errno
5200 decode wall and the elfldr "no graphics path" result were both just
process-context limits. From here on, anything touching decode, GPU, or
audio/output fidelity is tested here.

```bash
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc     # eboot.bin + param + libc, PFS-packed
  ./scripts/deploy-app.sh --ffpfsc'     # PS5_HOST from .env, like everything else
# then, on the console: ShadowMountPlus -> mount PPSA99039 -> launch from Games
```

| Script | What it does |
|---|---|
| `package-app.sh` | Compiles EVO with the native-app link tail, converts + FSELF-signs `eboot.bin`, assembles `output/app/PPSA99039/`. `--ffpfsc` also PFS-packs it to `PPSA99039.ffpfsc` (MkPFS — same format ProsperoLight ships). `--agc-probe` adds the boot-time `sceAgc` reachability recon. `--probe` builds the sandbox probe instead of the player. |
| `deploy-app.sh` | FTP-uploads the folder (or, with `--ffpfsc`, the single image) to `/data/homebrew/`. `--undeploy` removes it. Does **not** launch — ShadowMountPlus + the launch-safety rule are on you. |
| `setup-pfs-tool.sh` | Fetches MkPFS into `.deps/` (pinned, isolated venv). Called by `--ffpfsc`; needs network on first run. |
| `setup-native-app-deps.sh` | Bootstraps the static zlib the host converter needs. Called by `package-app.sh`. |

`param.json` lives at `projects/evoplayer/sce_sys/param.json` and is validated
on every build. The `eboot.bin` / `param.json` / `libc.prx` are byte-identical
between the folder and the `.ffpfsc` — the image is just a different container.

Iteration still needs a manual mount + launch per cycle (no remote
`SceSystemServiceLaunchApp` for an unregistered title). Diagnostics come back
as **system-notification popups + klog** (`-DEVO_APP_MODULE` routes the
`pp_stage_bc` / `evo_bt` / `EVO_P8` breadcrumbs there — `/mnt/usb0` is ENOENT
inside the sandbox, so file-based breadcrumbs are invisible). USB media browse
needs `tools/sandbox-unjail.sh`, re-run per launch.

### The removed ELF-payload route

Historically EVO also ran as an elfldr/`/hbldr` ELF payload
(`build-evoplayer.sh` → `install-homebrew.sh` → `tools/launch.sh`). That
context has **no graphics path** (`libSceGnmDriver` init crashes,
`sceKernelLoadStartModule("libSceAgc.sprx")` hangs) and hit the errno-5200
decode wall — it could never do the headline features. Deploying it as a
stand-in for the app module kept costing console sessions, so the push scripts
were **deleted 2026-09-03**. `scripts/build-evoplayer.sh` remains as a host
compile check (keeps the non-app-module `#else` paths green for #31/#36/the
modularisation plan) and cannot deploy.

For anything that used to be a "quick UI check on the real framebuffer", use
the host renderer (`tools/uiview.sh` / `uiplay.sh`) — same RmlUi code and
assets, no console. Kernel-R/W / dynlib recon probes, if ever needed again,
are in git history.

Historical detail on why the payload context was a dead end:
`docs/hardware-decode.md`, `docs/evo-pro/phase-1b-app-module.md`.

---

## `scripts/` — build and deploy

| Script | What it does |
|---|---|
| `common.sh` | Sourced by everything else. Strict mode, an `ERR` trap that names the failing line, logging helpers, container detection and re-exec, SDK loading, `PS5_HOST` validation, ELF validation. Not run directly. |
| `setup-sdk.sh` | Installs the PS5 payload SDK into the image. |
| `install-sdk-image.sh`, `install-pacbrew-image.sh` | Pull the pinned SDK / pacbrew package sets. |
| `build.sh` | Builds the small sample projects under `projects/`. |
| `build-ffmpeg.sh` | Builds FFmpeg for the console. Slow, cached in a Linux volume. |
| `build-prosperoplayer.sh` | Builds the upstream baseline fork. |
| `build-evoplayer.sh` | **Host compile check only.** Builds `output/elf/EVOPlayer.elf` to keep the non-app-module code path green (#31/#36/modularisation). Hard-errors on `--run`; cannot deploy. `--stage N` picks a 4K product stage. |
| `package-app.sh` / `deploy-app.sh` | **The one deploy path.** Build + deploy EVO as the `PPSA99039` app module — see [above](#ffpfsc--the-only-route). |
| `package-pkg.sh` | Produces a distributable PKG. |
| `gen-compile-commands.sh` | Regenerates `compile_commands.json` for clangd. |
| `shell.sh` | Drops you into a container shell. |

### Build switches

Passed through `EXTRA_CFLAGS`, empty in shipping builds:

| Flag | Effect |
|---|---|
| `-DEVO_AUTOSHOT=N` | Capture the framebuffer to `/mnt/usb0/` N seconds after launch |
| `-DEVO_START_SCREEN=n` | Boot straight into a screen — `0` launch, `1` browser, `10` settings, `11` profile |
| `-DEVO_PAD_DEBUG=1` | Print the raw pad mask on every press |

### Seeing payload `printf`

Payload stdout is **not** in klog — klog is the kernel log. It comes back as
the body of the `hbldr` request when `pipe=1` is set, so capture that response
instead of discarding it:

```bash
curl -sS --max-time 20 --get \
  --data-urlencode "path=/data/homebrew/EVOPlayer/eboot.elf" \
  --data-urlencode "pipe=1" "http://$PS5_HOST:8080/hbldr"
```

`curl` will report a timeout when the window expires — that is expected, the
payload is still running, and everything printed up to then is on stdout.

`build-evoplayer.sh` deletes the ELF before every build on purpose. Upstream's
make rule tracks `main.c` and the `pp/` sources but not `CFLAGS` and not
headers, so changing a `-D` flag left the previous binary in place and reported
"up to date" — which cost a debugging cycle when a flag change was installed,
launched, and reasoned about against a binary that had never been rebuilt.

---

## `tools/shot.sh` — screenshots, and measuring them

Captures are written by the player itself: a build with `-DEVO_AUTOSHOT=N`
writes one N seconds after launch, and **L3 or R3 in the menus** writes one on
demand. They land on the USB stick as `evo_shot_NNN.bmp`.

```bash
./tools/shot.sh grab            # newest capture -> output/screenshots/latest.png
./tools/shot.sh grab --full     # full resolution instead of half
./tools/shot.sh list            # what is on the stick, and how much space
./tools/shot.sh clean           # what is there, and how to remove it
```

Each BMP is an uncompressed 1920×1080 framebuffer — about **6 MB**, and the
player never cleans them up, so the stick fills over a long session.

`clean` cannot delete them for you: the console's web server serves `/fs`
read-only and implements no `DELETE` (its own `apiClient.js` only ever GETs).
So `clean` lists what is there and points at the two routes that do work — the
terminal built into `http://$PS5_HOST:8080/`, or pulling the stick. Local
copies under `output/screenshots` are ordinary files.

### Measuring, not squinting

This is the part worth knowing about. The capture is raw framebuffer, so it can
be interrogated numerically, and several UI questions were only settled that
way. "Are these icon edges aliased, or is the blitter wrong?" looks identical
on screen either way; the coverage ramp in the pixels answers it outright.

All of these default to the last capture you grabbed, so you `grab` once and
then ask as many questions as you need.

```bash
# colour at one framebuffer coordinate (1080p space - the same numbers
# that appear in ui/include/evo_metrics.h)
./tools/shot.sh probe 180 214

# summary of a region: mean, distinct colour count, top colours by area
./tools/shot.sh probe 152 222 1704 92

# pixel runs along a line, repeats collapsed. An antialiased edge shows a
# ramp; an aliased one steps from background to full intensity in one pixel
./tools/shot.sh scan row 260
./tools/shot.sh scan col 400 200 320

# cut one card or icon out at full resolution
./tools/shot.sh crop 1228 222 560 315

# did that change actually do anything?
./tools/shot.sh diff evo_shot_007.bmp evo_shot_008.bmp
```

`tools/shot.py` holds the BMP/PNG code and can be called directly if you want
to script something; `tools/shot.sh` is the front end and handles fetching.

`tools/fetch_shot.sh` still works — it is a shim onto `shot.sh grab`.

---

## `tools/klog.sh` — the console log, kept

The console's kernel log is the only channel that shows a payload dying before
it can draw anything. It needs **`ps5-payload-klogsrv` running on the console**;
the log then streams on port 3232.

The documented approach used to be `nc $PS5_HOST 3232`, which has two problems:
nothing is recorded, so the log is only useful if you happen to be watching at
the moment something goes wrong (which, for a crash on launch, you are not);
and klogsrv drops the connection whenever the payload restarts, so a bare `nc`
exits exactly when an investigation is starting.

```bash
./tools/klog.sh                 # follow, printing and recording
./tools/klog.sh --quiet         # record only
./tools/klog.sh --grep evo      # follow, filtered
./tools/klog.sh --once          # drain what is buffered and exit
./tools/klog.sh --tail 200      # last 200 recorded lines, offline
./tools/klog.sh --sessions      # what has been recorded
```

Every line is timestamped host-side (klog carries no clock of its own, so
without this there is no way to line a message up against a build or a
screenshot) and written to two places:

- `output/logs/klog/klog-<UTC>.log` — one file per capture session
- `output/logs/klog/klog-all.log` — **append-only, never rotated or
  truncated.** Nothing is ever lost, across any number of sessions.

A dropped connection reconnects instead of ending the capture, and the
reconnect is logged inline so a payload restart is visible in the record. After
a successful connection closes the retry is immediate — a restart is the case
where the *next* few lines are the ones that matter.

---

## `tools/gen_icons.py` — icon and controller-glyph generation

```bash
python3 tools/gen_icons.py     # writes assets/evo_icons.h + a contact sheet
```

Icons are described as vector shapes and rasterised from signed distance
fields, so edges carry analytic coverage at any size.

**To add an icon:** append to `ICONS` as `(macro_prefix, shape_fn, size)`, add
its name to `ICON_TABLE`, and add a named constant in `ui/include/evo_draw.h`
(such as `EVO_IC_LOGO`, `EVO_IC_HOME`, or `EVO_IC_EMBY`).
There is no `switch` to update any more — the generator emits
`EVO_ICON_TABLE`, and both the player and `tools/uiview.c` index it. That
replaced three hand-written switches, one of which had already drifted and
drew nothing for two icons.

**It also generates the font punctuation.** The UI atlas has 69 glyphs and no
comma, apostrophe, parenthesis or question mark, and it cannot be regenerated
— there is no generator for it in the tree. So `PUNCT` here describes the
missing marks in cap-height units, and the script emits a second atlas
(`assets/evo_font_punct.h`), the alphabet on its own for the UI layer
(`ui/include/evo_font_charset.h`), and a contact sheet at
`output/screenshots/punct_preview.png`. Look at the sheet after changing a
glyph; several of them read wrong on the first attempt and only the picture
showed it.

Advances are measured from the rasterised ink, not declared, so a shape change
cannot leave the spacing behind.

---

## `tools/measure_font.py` — the font's real metrics

```bash
python3 tools/measure_font.py
```

Reads baseline, cap height, x-height, descender and stroke weight straight out
of the atlas ink, per face, by walking glyphs whose shape is known (`H`, `x`,
`p`, `.`, `:`, `-`).

Generated punctuation has to sit on the same baseline as the letters, and the
atlas carries no such metadata — guessing puts a comma floating mid-line. The
numbers this prints are pasted into `gen_icons.py` as `FACE_METRICS`. Re-run it
rather than trusting them if the atlas is ever replaced.

They are monochrome by design: the UI tints them with the theme accent at draw
time. The controller prompts were originally two-tone bitmaps (103 distinct
RGB values), which is why they stayed cyan under every theme until they were
regenerated as single-hue glyphs.

---

## `tools/uiview.sh` — the UI, rendered on the host

**Look at the UI without a console.** This is not a mock-up: it links the real
drawing code — `evo_ui`'s SDF primitives, `evo_chrome`, `evo_widgets`,
`evo_screens` — against the real font atlas and the real generated icons, and
paints a 1920×1080 buffer exactly as the player does. What comes out is what
the console draws, modulo the tile swizzle, which changes where pixels live in
memory and not what they look like.

```bash
./tools/uiview.sh --all                  # every screen -> output/uiview/
./tools/uiview.sh browse --sel 3
./tools/uiview.sh browse --rail          # rail focused (the expanded overlay)
./tools/uiview.sh launch --row 2         # cursor on the library shelf
./tools/uiview.sh favorites --empty      # the empty state
./tools/uiview.sh settings --theme EMBER
```

It renders BMP and converts through `tools/shot.py`, the same path console
captures take, so a host render and a console capture are directly comparable
with `shot.py diff`.

Themes come from the four built-ins: `EVO_THEME_DIR` is pointed at a path that
does not exist on the host, so a render never depends on what happens to be on
someone's USB stick.

It earns its keep immediately — its first run surfaced two real defects that
had shipped to hardware unnoticed:

- `evo_ui_vgrad` **replaces** rather than blends. Using it for a scrim wrote
  transparency straight into the framebuffer, and since scanout ignores alpha
  the hero's fade rendered as a hard black slab. Fixed by adding
  `evo_ui_vgrad_over()`.
- The expanded rail was ~240/255 opaque, because card surfaces carry alpha
  (MIDNIGHT's `surface` is 235) and that alpha propagated into the panel fill.
  The page title behind ghosted through it.

Both are the kind of thing you notice instantly in a still and never quite
pin down at ten feet.

### Walking through it: `tools/uiplay.sh`

`uiview` gives stills. `uiplay` gives something you can drive, in a browser,
on any machine:

```bash
./tools/uiplay.sh                # then open output/uiplay/index.html
./tools/uiplay.sh --theme EMBER
```

Arrow keys move, Enter selects, Esc goes back, Left opens the navigation rail
— the same model the console uses. `E` toggles the empty states, `M` steps
through the modal screens, `F` goes fullscreen and `H` hides the page chrome,
because the frame is the point and a 1920×1080 still deserves the whole
monitor.

`M` exists because the resume prompt, the playback-finished prompt and media
info are opened *by the player*, so there is no route into them from the
menus and the walkthrough needs its own way in.

**Every screen the UI layer draws is covered.** The one exception is the
player OSD, which overlays live video and has not been migrated — see
[ui-handoff.md](ui-handoff.md).

There is no emscripten, no mingw and no host SDL2 in the container, so a
windowed binary is not buildable from here. It does not need to be: this repo
owns both the renderer and the navigation model, so every reachable cursor
position can be rendered ahead of time — 107 frames at full 1920×1080, about
15 MB — and stepped through with a JavaScript copy of `evo_grid` /
`evo_focus` / `evo_nav`. If that copy and the C ever disagree, the C is right
and the page is the bug.

Frames are full resolution rather than half on purpose: these get viewed on a
desktop monitor that is often wider than 1920, and the hairlines, 1 px borders
and small type — the things actually worth inspecting — turn to mush when a
half-size frame is scaled up.

It is **not** live data, animation or the player's timing. It shows layout and
navigation, which is exactly what cannot be checked without a console.

It found the hero's action chip sizing its width for a 48px controller glyph
placed at a 62px text offset, so the ✕ and the label overlapped whenever the
hero was not selected — visible only in the unselected state, which no
hardware screenshot had happened to capture.

---

## `tools/bench.sh` — the converter, measured on the host

The YUV→BGRA+swizzle path needs no console: it takes a plain `pp_frame` in and
writes a plain buffer out. Since there is no hardware GL driver available (see
[`gpu-notes.md`](gpu-notes.md)), this is the main performance lever there is.

```bash
./tools/bench.sh          # timings, worker scaling, budget check
./tools/bench.sh 100      # more iterations, steadier numbers
./tools/bench.sh --asan   # overruns and UB
./tools/bench.sh --tsan   # data races
```

It hashes the output plane and **refuses to report timings** if worker counts
disagree — a faster converter that produces different pixels is not faster.

Findings and reference hashes: [`converter-perf.md`](converter-perf.md).

> ThreadSanitizer needs Docker's seccomp profile relaxed, or it aborts with
> `personality(ADDR_NO_RANDOMIZE)` failing. `docker compose run` cannot pass
> that, so use:
> ```bash
> MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined \
>   -v "/d/Projects/EVO Player:/workspace" -w /workspace \
>   evo-player/ps5-dev:llvm18-sdk-v0.42 bash -lc './tools/bench.sh --tsan'
> ```

---

## Testing UI code on the host

The theme parser and anything else that does not touch the framebuffer can be
compiled and run natively under sanitizers, which is much faster than a
console round trip:

```bash
docker compose run --rm ps5-dev bash -lc '
  clang -Wall -Wextra -fsanitize=address,undefined \
    -DEVO_THEME_DIR=\"evo_themes\" \
    -I/workspace/projects/evoplayer/pp/include \
    your_test.c /workspace/projects/evoplayer/pp/src/evo_theme.c -o t && ./t'
```

`EVO_THEME_DIR` is overridable precisely so this works.

---

## Environment

| Variable | Meaning |
|---|---|
| `PS5_HOST` | Console IP. Never committed — put it in `.env` at the repo root. |
| `PS5_PORT` | ELF loader port, default 9021. |
| `PS5_WEB_PORT` | `ps5-payload-websrv` port, default 8080. Used for file transfer. |
| `KLOG_PORT` | klogsrv port, default 3232. |
| `EXTRA_CFLAGS` | Development build switches, see above. |

Which services need to be running on the console:

| Task | Needs |
|---|---|
| `deploy-app.sh`, `tools/evo-remote.sh` | `ps5-payload-ftpsrv` (2121) + ShadowMountPlus to mount + launch |
| `tools/shot.sh`, `evo-remote.sh` boot-log pull | `ps5-payload-websrv` (8080) |
| `tools/sandbox-unjail.sh` (rarely) | `ps5-payload-elfldr` (9021) |
| `klog.sh` | `ps5-payload-klogsrv` (3232) |

All of them need the jailbreak re-run after every console reboot. A port that
pings but refuses connections almost always means the exploit has lapsed
rather than anything being wrong with the tooling — see
[`networking.md`](networking.md).
