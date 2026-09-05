# Validation status

Two categories, kept strictly apart:

- **Verified in CI/container** — reproducible by anyone, gated by CI.
- **Verified on hardware** — run on a jailbroken PS5 on 12.70.

A green checkbox in the first table means the code *builds*; only the second
table means it *runs*. Rows marked "visually/audibly confirmed" were checked
against the panel and speakers, not merely reported as successful by the API —
that distinction matters, because every VideoOut call succeeded for a whole
session while the screen stayed black (see correction 5 below).

---

## Verified in the container

Last run 2026-08-09, image `evo-player/ps5-dev:llvm18-sdk-v0.42`.

| Check | Status | Evidence |
|---|---|---|
| Docker image builds | pass | `docker compose build`, 1.24 GB |
| Container starts | pass | `docker compose run --rm ps5-dev bash` |
| PS5 SDK installed | pass | `/opt/ps5-payload-sdk`, release zip |
| SDK version documented | pass | `EVO_SDK_VERSION` → v0.42 |
| 12.70 support verified | pass (by source audit) | `crt/kernel.c` `case 0x12700000:` — see [sdk-audit.md](sdk-audit.md#2-firmware-support-and-1270-specifically) |
| Clang works | pass | 18.1.3 |
| LLD works | pass | 18.1.3 |
| CMake works | pass | 3.31.6 |
| Ninja works | pass | 1.12.1 |
| Python works | pass | 3.12.3 |
| pyelftools works | pass | `ELFFile` import + `setup-sdk.sh` check |
| SDK `hello_world` builds | pass | `setup-sdk.sh` smoke test |
| EVO `hello_world` builds | pass | 113 KB PIE ELF |
| `system_info` builds | pass | 115 KB |
| VideoOut test builds | pass | 119 KB, links `-lSceVideoOut -lSceUserService` |
| AudioOut test builds | pass | 116 KB, links `-lSceAudioOut` |
| `gpu_test` builds | pass | 116 KB, links `-lSceGnmDriver` |
| `decoder_test` builds | pass | 116 KB |
| GPU environment understood | pass | [gpu-notes.md](gpu-notes.md) — stubs yes, headers/Gnmx/shader compiler no |
| FFmpeg version identified | pass | 7.0.1, from pacbrew `ffmpeg/PKGBUILD` |
| Custom FFmpeg builds | pass | minimal profile; all 14 target codecs present |
| ProsperoPlayer deps identified | pass | `build-prosperoplayer.sh --audit` |
| ProsperoPlayer builds | pass | 43,305,656-byte ELF, after the transitive-link fix |
| `compile_commands.json` generated | pass | CMake + Ninja, 8 targets |
| Source stays outside container | pass | bind mount, `.:/workspace` |
| Build artifacts persist | pass | `output/` on the bind mount; ccache + FFmpeg tree on named volumes |
| Proprietary files excluded | pass | `.gitignore`; nothing required to reach this point |
| README has full setup | pass | [../README.md](../README.md) |

### FFmpeg minimal profile codec inventory

73 components. All of the brief's targets present:

`aac` `ac3` `eac3` `dca` `mp3` `flac` `opus` `vorbis` `alac` ·
`h264` `hevc` `vp9` `mpeg2video` `av1`

Full list: `output/logs/ffmpeg-7.0.1-minimal-codecs.txt`.

### ProsperoPlayer baseline finding

Upstream builds unmodified **only after** adding transitive static-link
dependencies. pacbrew builds FFmpeg with `--enable-openssl --enable-libass
--enable-libfreetype --enable-libfribidi --enable-libharfbuzz`; static archives
carry no dependency metadata, so those must appear on the link line. Upstream's
`LIBS` omits them.

Symptoms, in the order they surface:

```
undefined symbol: BN_set_word, BN_num_bits, BN_rand, BN_CTX_new, BN_mod_exp
    -> referenced by rtmpdh.c in libavformat.a   -> add -lssl -lcrypto
undefined symbol: libiconv, libiconv_open, libiconv_close
    -> add -liconv
```

Fixed in the **environment**, not by editing upstream: `build-prosperoplayer.sh`
overrides `LIBS` on the make command line. Reproduce the failure with
`EVO_SKIP_LINK_FIX=1`.

---

## Hardware results — PS5 at 192.168.0.10, firmware 12.70

Run 2026-08-09 against a jailbroken console with `ps5-payload-elfldr` on 9021.

| # | Check | Status | Evidence |
|---|---|---|---|
| 1 | Container reaches the console | **pass** | `nc -vz` succeeded; ping 8.7–20.5 ms. Default Docker Desktop bridge, no host networking |
| 2 | `hello_world` loads and runs | **pass** | stdout returned over the loader socket: `argc=1`, `argv[0]=payload.elf`, `clang 18.1.3` |
| 3 | `hello_world` exits cleanly | **pass** | exit 0 |
| 4 | Firmware really is 12.70 | **pass** | raw `0x12700001`, group `0x12700000`, 16 CPUs, 16 KiB pages |
| 5 | VideoOut solid colours | **pass, visually confirmed** | full-screen red/green/blue/white, no unwritten regions |
| 6 | VideoOut colour bands | **pass, visually confirmed** | 9 tile-tall bands; **red directly above blue -> ABGR8888 channel order is correct** |
| 7 | AudioOut sine wave | **pass, audibly confirmed** | clean 440 Hz, both channels, no clicks or stutter |
| 8 | GNM submits allowed | **pass** | `sceGnmAreSubmitsAllowed() → 1`; GnmDriver mapped at `0x8002a0000` |
| 9 | Native decoder modules reachable | **pass** | `libSceAvPlayer` loaded, all 6 entry points resolved by NID — see [native-media-research.md](native-media-research.md) |
| 10 | ProsperoPlayer baseline plays media | **pass** | installed as homebrew (`ProsperoPlayer_baseline`); video and audio play from USB. Two defects found, both traced to upstream source — see [baseline-defects.md](baseline-defects.md) |

All output confirmed on the panel and through the speakers, not merely
reported as successful by the API.

### What the hardware run corrected

Six assumptions in the original scaffold were wrong. All are now fixed in
code, with the reasoning recorded at the top of each `main.c`:

1. **No user session in a payload.** `sceUserServiceGetInitialUser()` returns
   `0x80940004`; klog shows `SceLncService getAppLaunchedUser: LNC_ISOK::0x80940004`.
   Fix: pass user id `0xff` (system) to `sceVideoOutOpen` / `sceAudioOutOpen`
   and drop `-lSceUserService` entirely.

2. **The PS4-style VideoOut API is the wrong one.** PS5 uses
   `sceVideoOutSetBufferAttribute2` / `sceVideoOutRegisterBuffers2`, with a
   64-bit pixel format (`0x8000000022000000`, memory layout ABGR8888) and an
   array of buffer descriptors. Pitch is implicit. Cross-checked against
   `ps5-payload-dev/SDL`'s backend, which is known-good on this platform.

3. **No direct-memory budget.** `sceKernelGetDirectMemorySize()` returns 0 and
   klog shows the payload spawned with `dmem#0`. Use
   `sceKernelAllocateMainDirectMemory`, and note **64 MiB fails with EAGAIN
   (`0x80020023`) while 32 MiB succeeds** — SDL's 64 MiB value is too large
   for an elfldr payload.

4. **Sony modules export NIDs, not names.** `sceKernelDlsym` by name returns
   `0x80020003` for every symbol. Use `nid_encode()` + `kernel_dynlib_resolve()`.

5. **A payload under elfldr is headless.** `ps5-payload-elfldr` spawns payloads
   inside `SceSpZeroConf` (`websrv/src/ps5/elfldr.c:74`,
   `/system/vsh/app/NPXS40112/eboot.bin`) - a background network service with
   no display plane and no audio. VideoOut and AudioOut calls all *succeed*
   there and 960 flips were reported against a blank screen. Anything that
   draws or plays sound must be installed as homebrew and launched through
   websrv's `hbldr_launch`, which borrows the PS Now app slot
   (`hbldr.c:45`). `scripts/install-homebrew.sh` automates this.
   Note POSTing to websrv's `/elfldr` does *not* help - that path calls
   `elfldr_spawn` and lands back in `SceSpZeroConf`.

6. **The scanout surface is tiled, and linear is not available on retail.**
   Requesting tiling mode 1 gives:
   ```
   [VideoOut] Tiling Mode Error: Linear format is only valid with
   "Enhanced Display Buffer Attribute" enabled (at Debug Settings)
   sceVideoOutRegisterBuffers2 -> 0x80290007
   ```
   Tiles are 512x128 px. Writing a non-uniform image linearly scrambles it;
   correct output needs a swizzle (ps5-payload-dev/SDL generates the table in
   `SDL_ps5tilemap.inc` and applies it across 12 threads per frame).
   `videoout_test` sidesteps this by drawing bands exactly one tile tall, so
   every tile is a flat colour and is invariant under the permutation.
   Filling only `width*height` also left the padding unwritten - the black
   bottom-right wedge seen on the first run - so fills now cover the whole
   buffer region.

   **Architectural consequence:** GPU-side YUV->RGB is not optional polish.
   A CPU path would have to swizzle every pixel of every frame.

### Recording a run

Append to this file: date, firmware raw word, payload sha256, what you saw,
and any error codes. A photograph of the screen is worth more than a
description for the VideoOut tests.

---

## UI parity (#44)

Per-screen RmlUi-vs-`main` sign-off lives in
[rmlui-parity.md](rmlui-parity.md). Every screen is **OK / OK\*** on the host
render pairs (`tools/uiview_playback_rml.sh` vs `tools/uiview.sh --all`), with
#16 folded in. The following still need a hardware pass on a jailbroken PS5
(app module, `PPSA99039`) before the legacy screen code is deleted:

| Check | Why host can't confirm it | Status |
|---|---|---|
| Theme switch repaint on device | RmlUi applies theme colours per-element in `Update*State`; a missed element only shows when switching themes live | not run |
| Playback OSD over live decoded video | Host renders the OSD over a flat fill; real compositing is over a moving 4K frame (BGRA `0xAABBGGRR`) | not run |
| Dialog / Media Info / subtitle picker over video | Same — overlay-over-video path | not run |
| OSD title marquee smoothness | Host steps a fake clock; real cadence is the player's frame loop while paused | not run |
| D-pad focus / navigation order + timing | Host fixtures set focus directly; real nav is `evo_focus` / `evo_nav` driving the DOM | not run |

Record results here (date, `.ffpfsc` sha, screen, pass/fail, photo).

---

## Video decoder setting (#37)

Code-complete, hw-verify-pending (2026-09-05): the Auto/FFmpeg/Native settings
row, config migration and the Media Info decoder badge — see
[evo-pro/native-decode-plan.md](evo-pro/native-decode-plan.md) § Phase 5.
Host preview (`tools/uiview_playback_rml.sh`) confirms the row/badge render.

**First hardware pass (2026-09-05) caught a real behavior bug:** `NATIVE`
silently played an unsupported-codec 4K clip on FFmpeg — exactly `AUTO`'s
graceful-degrade, which defeats the point of picking `NATIVE` explicitly.
Fixed same day: `NATIVE` now refuses to play anything it can't decode itself
(closes the FFmpeg decoder that had already opened, shows "NATIVE DECODE
UNSUPPORTED" via `prospero_codec_error`, returns to the browser) instead of
falling back — both at open time and on a mid-stream native fatal (the #57
retry-on-FFmpeg path is skipped under `NATIVE`). Re-verify pending:

| Check | Status |
|---|---|
| `AUTO` with the probe armed opens native on an H.264 file | not run |
| `FFMPEG` forces software decode on the same clip | not run |
| `NATIVE` with the probe failed, or on a codec `evo_vdec_native.c` doesn't support (e.g. HEVC), shows "NATIVE DECODE UNSUPPORTED" and does **not** play | not run (fix landed after the first pass, which found the opposite: it played) |
| `NATIVE` mid-file native fatal ends the file (`SCREEN_PLAYBACK_FINISHED`) rather than reopening on FFmpeg | not run |
| Old (pre-#37) config file loads with `AUTO` and keeps every other setting | not run |
| Toggling mid-file toasts "Applies to next video"; current file unaffected, next file honours it | not run |
| Media Info's DECODER row matches the file actually playing, for both backends | not run |

A per-codec decode-backend column with ms/frame + dropped-frame counts is
Phase 6 / **#38**'s job (FFmpeg-vs-native A/B benchmark), not this table.
