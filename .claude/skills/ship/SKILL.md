---
name: ship
description: Build, deploy, launch, or screenshot EVO Player on the PS5, or render its UI on the host. Use whenever asked to run / launch / deploy / install / screenshot / hardware-test the app, verify a change on real hardware or in the real app, or do a console session. There is exactly ONE hardware path — the PPSA99039 app module (.ffpfsc) — plus the host UI renderer. Encodes the launch-safety and packaging rules.
---

# Shipping EVO Player

Everything re-execs through the pinned Docker container. **Never call `make`
directly** (missing FFmpeg's transitive deps) and **never stack launches** (the
app slot stays resident — stacking has kernel-panicked the console). Full
reference: `docs/tooling.md`. Console session details: `docs/evo-pro/status.md`.

## THE hardware rule — read this first

**Every deploy / launch / hardware test goes through the `.ffpfsc` app module
(`PPSA99039`). Full stop.** There is no ELF-payload path any more — the
`install-homebrew` / `launch.sh` / `deploy.sh` scripts were **deleted
2026-09-03** because reaching for them (even for a UI check) is a recurring
mistake that wastes console sessions and the user's time. `build-evoplayer.sh`
still exists but is a **host compile check only** — it cannot and must not
deploy anything.

- Layout / rendering / navigation question → **host renderer (§3)**, no console.
- Anything that has to run on the console → **app module (§2)**.

## 1. Route

| The change is about… | Route |
|---|---|
| Decode, GPU/`sceAgc`, audio, the sandbox, `/data` / `/mnt/usb0`, persistence, boot, input timing, theme repaint on hardware, overlay-over-video | **App module** (§2) |
| UI layout, RCSS, a screen's look, navigation, marquee/clamp | **Host renderer** (§3) — this answers it without a console |
| Compile-green check of the non-app-module path (#31 §4, #36, modularisation parity) | `./scripts/build-evoplayer.sh` — say "compile check only" |

## 2. App module — the only hardware path (`PPSA99039`)

```bash
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc'
```

- Flags: `--ffpfsc` (the image), `--usb-remote` (the scriptable FTP dev
  remote + verbose vdec log), `--breadcrumbs` (on-screen boot popups),
  `--rebuild-libc`. The self-unjail + `EVO_APP_MODULE` + boot trace are always
  on. (The `--agc-probe` / `--videodec2-probe` / `--avplayer-probe` /
  `--geo-text` / `--shader-scan` probe flags were removed 2026-09-09.)
- **GL is ON by default since 2026-09-10.** The boot cuts over to the
  persistent ps5-opengl GL/EGL context instead of `pp_agc_init` +
  `pp_videoout`, so `./scripts/build-ps5-opengl.sh` must have run once.
  `--no-gl` selects the legacy path — it is UNMAINTAINED and known-broken at
  boot since GL-3/GL-4 (no GL-era commit was ever verified on it), so use it
  only as an escape hatch.
- `deploy-app.sh --ffpfsc` DELETEs every `/mnt/usb0/evo_*` + `pp_4k_stage_*`
  file, so each launch starts with a fresh log set.
- **Before deploying, check the console is up:** `nc -w4 -vz $PS5_HOST 2121`.
  Timeout → stop; the console is off / rest mode / jailbreak lapsed.
- Deploy FTPs `PPSA99039.ffpfsc` to `/data/homebrew/`. The user's
  **ShadowMountPlus re-mounts and auto-launches** it on the `.ffpfsc` change —
  that is the only non-manual relaunch. Otherwise the user launches PPSA99039
  from the Games row. **You cannot launch or close it remotely.**
- **Never deploy over a running EVO** — panic risk. If EVO is already running,
  the user must **PS-button close** it first (the only way to free the slot).
- Diagnostics come back as **on-screen notification popups** (klog doesn't get
  them from the sandbox) — the user reads the TV and relays. For an unattended
  run use `tools/evo-remote.sh` (`build` / `play` / `seek` / `status` / `boot`
  over FTP; needs `--usb-remote` in the build; launch is still one manual press).
- `deploy-app.sh --undeploy` removes it.

## 3. Host renderer — no console, use this for any UI question

```bash
./tools/uiview.sh --all          # render every screen to output/uiview/*.png
./tools/uiplay.sh                # contact sheet -> output/uiplay/index.html
./tools/uiview_playback_rml.sh   # the RmlUi playback / menu / cursor states
./tools/prof_rmlui.sh            # RmlUi frame profiler (-DEVO_RML_PROFILE)
```

Same RmlUi code, real `.rml`/`.rcss`/assets, no console risk, no cooldown. Add
a fixture to `tools/uiview_playback_rml.cpp` for a screen/state it misses.
Go to hardware only when the question is genuinely about console behaviour.

## After any hardware deploy

- Report what the notifications / klog / `evo-remote.sh status` actually show —
  including failures and the exact error text.
- Remind the user to **PS-button-close** EVO before the next deploy.
- Never re-deploy to "retry" while an instance is still running.
