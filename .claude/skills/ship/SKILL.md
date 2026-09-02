---
name: ship
description: Build, deploy, launch, or screenshot EVO Player on the PS5, or render its UI on the host. Use whenever asked to run / launch / deploy / install / screenshot / hardware-test the app, verify a change on real hardware or in the real app, or do a console session. Picks the app-module route for decode/GPU/audio/sandbox work and the payload or host route for UI/layout work. Encodes the launch-safety and packaging rules.
---

# Shipping EVO Player

Everything re-execs through the pinned Docker container. **Never call `make`
directly** (missing FFmpeg's transitive deps) and **never stack launches** (the
app slot stays resident — stacking has kernel-panicked the console). Full
reference: `docs/tooling.md`. Console session details: `docs/evo-pro/status.md`.

## 1. Pick the route

| The change is about… | Route |
|---|---|
| Decode, GPU/`sceAgc`, audio fidelity, the app sandbox, `/mnt/usb0` or `/data` access, native-decode | **App module** (§2) — the only context with real system access |
| UI layout, navigation, RmlUi rendering, theming, a screen's look | **Host preview** (§4) first; **payload** (§3) only to confirm on the real framebuffer |
| Anything else / a quick smoke test | Payload (§3) |

## 2. App module — the release path (`PPSA99039`)

```bash
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --agc-probe --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc'
```

- `--agc-probe` compiles the boot-time `sceAgc` recon + the `P8_*` playback
  breadcrumbs + the self-unjail. Drop it for a plain build.
- **Before deploying, check the console is up:** `nc -w4 -vz $PS5_HOST 2121`.
  If it times out, stop — the console is off / rest mode / jailbreak lapsed.
- Deploy triggers the user's ShadowMountPlus auto-mount. **Launch is manual** —
  the user launches EVO from the Games row. You cannot do this remotely.
- Diagnostics come back as **on-screen notification popups** (klog doesn't get
  them from the sandbox). The user has to read the TV and relay.
- `deploy-app.sh --undeploy` removes it.

## 3. Payload — UI iteration (`/hbldr`)

```bash
docker compose run --rm ps5-dev bash -lc '
  EXTRA_CFLAGS="-DEVO_AUTOSHOT=6" ./scripts/build-evoplayer.sh
  ./scripts/install-homebrew.sh --name EVOPlayer output/elf/EVOPlayer.elf
  ./tools/launch.sh --name EVOPlayer --timeout 12
  ./tools/shot.sh grab'
```

- `launch.sh` refuses to stack on a recent launch (90 s cooldown). Only the
  **PS button → close** on the console frees the slot — tell the user if a
  launch is blocked.
- `curl` exit 28 (timeout) on the launch is the **normal, successful** outcome.
- `-DEVO_AUTOSHOT=N` captures the framebuffer N s after launch; `shot.sh grab`
  fetches it to `output/screenshots/latest.png`. Read that image.
- Build switches (`EXTRA_CFLAGS`): `-DEVO_AUTOSHOT=N`, `-DEVO_START_SCREEN=n`,
  `-DEVO_PAD_DEBUG=1`, `-DEVO_DIAG_FPS=1`.
- Watch the console log in a second shell:
  `docker compose run --rm ps5-dev ./tools/klog.sh`

## 4. Host — no console

```bash
./tools/uiview.sh --all          # render every screen to output/uiview/*.png
./tools/uiplay.sh                # interactive: open output/uiplay/index.html
./tools/uiview_playback_rml.sh   # the RmlUi playback/menu screens
./tools/prof_rmlui.sh            # RmlUi frame profiler (-DEVO_RML_PROFILE)
./tools/bench.sh                 # CPU converter timings
```

Prefer these for any layout / navigation / rendering question — same drawing
code, real assets, no console risk, no cooldown. Go to hardware only when the
question is genuinely about console behavior.

## 5. GPU shaders

```bash
./tools/build-shader.sh pp/shaders/<name>.s     # GCN .s -> raw .text blob
```

## After any hardware launch

- Report what the screenshot / notifications / klog actually show — including
  failures and the exact error text.
- Remind the user to **PS-button-close** before the next build if a launch happened.
- Never re-run a launch to "retry" without the previous instance being closed.
