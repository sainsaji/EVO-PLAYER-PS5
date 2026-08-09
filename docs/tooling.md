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
# build, install, launch, capture a screenshot
docker compose run --rm ps5-dev bash -lc '
  EXTRA_CFLAGS="-DEVO_AUTOSHOT=6" ./scripts/build-evoplayer.sh
  ./scripts/install-homebrew.sh --name EVOPlayer output/elf/EVOPlayer.elf
  ./tools/launch.sh --timeout 12
  ./tools/shot.sh grab'

# watch the console log while you do it (separate terminal, keeps a record)
docker compose run --rm ps5-dev ./tools/klog.sh
```

Then open `output/screenshots/latest.png`.

> **Exit the app on the console before launching again.** This is the one rule
> that matters — see the next section.

---

## Launching, and why it needs care

**The app slot stays resident after a launch.** `/hbldr` never closes its log
pipe because the process does not exit, and launching again does **not**
replace the running instance — it adds another one. Every instance opens
videoout, an audio port, the pad and decoder threads.

Stacking them kernel-panicked the console on 2026-08-09, during a loop that
fired about ten launches without exiting anything in between. It cost roughly
fifty minutes of recovery.

So launch through `tools/launch.sh`, which refuses to pile another instance on
top of a recent one:

```bash
./tools/launch.sh                # stream stdout for 15s
./tools/launch.sh --timeout 30
./tools/launch.sh --force        # you know the previous instance is gone
```

The cooldown is 90 s by default (`EVO_LAUNCH_COOLDOWN`). It is a speed bump,
not a real interlock: the console exposes no remote "kill app" that we have
found, so **only you can actually close the previous instance** — PS button →
close the application.

Two habits that help more than the guard does:

- **Batch verification.** One launch that captures several screens beats one
  launch per screen.
- **Iterate in the container.** Building and syntax-checking cost nothing.
  Go to hardware when there is something real to see.

`curl` reporting a timeout (exit 28) is the normal, successful outcome — the
pipe never EOFs.

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
| `build-evoplayer.sh` | **The one you want.** Builds EVO Player. `--run` installs and launches, `--stage N` picks a 4K product stage. |
| `install-homebrew.sh` | Installs an ELF as a homebrew app and optionally launches it. |
| `package-pkg.sh` | Produces a distributable PKG. |
| `deploy.sh` | Sends an ELF straight to `ps5-payload-elfldr` on port 9021. |
| `tools/launch.sh` | Launches the installed homebrew and streams its stdout, refusing to stack instances. Prefer this over a raw `curl` to `/hbldr`. |
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
fields, so edges carry analytic coverage at any size. Append to `ICONS` as
`(macro_prefix, shape_fn, size)` and add a `case` in `rr_icon()` — or better,
a named constant in `ui/include/evo_draw.h`, which is where icon indices live
now.

They are monochrome by design: the UI tints them with the theme accent at draw
time. The controller prompts were originally two-tone bitmaps (103 distinct
RGB values), which is why they stayed cyan under every theme until they were
regenerated as single-hue glyphs.

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
| `deploy.sh` | `ps5-payload-elfldr` (9021) |
| `shot.sh`, `install-homebrew.sh` | `ps5-payload-websrv` (8080) |
| `klog.sh` | `ps5-payload-klogsrv` (3232) |

All of them need the jailbreak re-run after every console reboot. A port that
pings but refuses connections almost always means the exploit has lapsed
rather than anything being wrong with the tooling — see
[`networking.md`](networking.md).
