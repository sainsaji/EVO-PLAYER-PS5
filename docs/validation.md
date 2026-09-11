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

A per-codec decode-backend column with ms/frame + dropped-frame counts is not
this table's job — it is the [codec sweep](#codec-sweep--decode-speed-drops-and-colour-8)
below (#8), whose numbers are also what #38's FFmpeg-vs-native A/B compares.

---

## Codec sweep — decode speed, drops and colour (#8)

The sweep this project has needed since `backlog.md` §1: play every clip in the
29-file test set and record **how fast it decoded, how much was dropped, and
what colour it actually ended up on screen** — not a checkmark.

A pass/fail sweep has now called a broken build green twice, and both are the
reason this table has the columns it does:

- The app blamed E-AC3 for failures in files with **no audio track at all**
  ([baseline-defects.md](baseline-defects.md)). A binary result cannot
  distinguish a missing decoder from a slow one, so `verdict` does.
- Every 10-bit file played, held A/V sync, and was **the wrong colour** — the
  PQ inverse-EOTF was applied off the decoder's *profile* when the shader
  needed `color_trc` (#41). "It played" was true and useless, so the table
  carries a colour signature.

### Running it

```bash
# one deployed --usb-remote build, launched once from the Games row
docker compose run --rm ps5-dev bash -lc '
  ./tools/evo-remote.sh build            # package --usb-remote + deploy
  # ... PS-button-close, launch PPSA99039 from the Games row ...
  ./tools/evo-remote.sh sweep'           # plays every clip, harvests, renders
```

`sweep` plays each clip for a 30 s window (`--secs`), then moves on; the next
`play` closes the previous file, which is what writes its row, and a trailing
`stop` flushes the last one. Output lands in `output/logs/sweep.md`, ready to
paste below. `--dir` points it at another directory, `--max` cuts a run short.

To re-render a table from a log you already have — no console needed:

```bash
python3 tools/sweep_report.py output/logs/evo.log -o output/logs/sweep.md
```

### Where the numbers come from

| Column | Measured by | Notes |
|---|---|---|
| Decode ms/frame | `evo_vdec_send` + `evo_vdec_receive` wall time, in the seam (`media/src/evo_vdec_ffmpeg.c`) | The seam is the only place both backends are visible, so FFmpeg and native numbers are directly comparable — this is also #38's A/B. Pipelined work is charged to the frame it produced, not the call that collected it. |
| Budget | `1000 / fps` | One frame period. Decode p95 over budget is a throughput failure whatever the cause. |
| GPU ms | Texture upload + YUV→RGB on the quad + OSD composite, µs clock, player screen only | GL-4 deleted the CPU converters, so this is the successor to the issue's "conversion time" column — the YUV→RGB now happens in the fragment shader inside this number. **Split at `eglSwapBuffers`**: the swap half is the vblank wait, pins to the refresh period whatever the frame cost, and would read ~16 ms for every clip, so it is logged as `swap_ms_avg` and kept out of the table. Timing them together measures nothing — the first run did exactly that and reported 13.66 ms for an 8 fps 800x600 clip. |
| Dropped | `pp_playback` `frames_late_dropped` / `frames_discarded_seek` | Late drops are the pipeline missing its clock; seek discards are expected and not a fault. |
| Colour | 16-pixel deterministic probe of the composited framebuffer, `evo_gl_probe_rgb()` | Taken after the video quad and **before** the OSD composite, 90 video frames in. Same sample positions every run, so the signature is comparable across runs and backends. |
| Verdict | `media/src/evo_sweep.c` | `no_decoder` (nothing could open it) / `no_frames` / `decode_error` / `slow_decode` (p95 over budget) / `slow_pipeline` (>5% late) / `realtime`. |

Each played file writes one `sweep v=1 …` line to `/mnt/usb0/evo.log`;
`tools/sweep_report.py` is the only thing that needs to understand it.

### Results

Record with each run: date, build id from the `evo_status` line, and which of
`--native-10bit` / `--no-native-secondary` / `--no-native-secondary-4k` produced
it - those change which backend is under test, so a table without them is not
comparable to the next one.

#### 2026-09-11 - baseline. Build `92eb09c4-dirty_0911-1419`, no `--native-10bit`

21 clips from `/mnt/usb0`, 30 s window, single runner. **10-bit excluded**
(`--skip hevc10 --skip 10bit --skip high10`) while the crash below is open.

An earlier pass the same evening is discarded, not recorded: two runners ran
concurrently (stopping a backgrounded `docker compose run` kills the host pipe,
not the container), so clips were yanked mid-measurement. Check `docker ps` for
leftovers before a run.

| Clip | Codec | Res | Backend | Decode ms/frame (avg / p95) | Budget | GPU ms (avg / p95) | Frames | Dropped (late / seek) | Colour | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| bbb_1080p_h264.mp4 | h264 | 1920x1080 | native | 1.15 / 0.56 | 33.3 | 8.06 / 6.49 | 282 | 0 / 0 | `8eb41b1d` #33746b | ✅ real-time |
| Clarksons.Farm.S01E01.720p.AMZN.WEBRip.x264-GalaxyTV.mkv | h264 | 1280x720 | native | 0.32 / 0.31 | 40.0 | 7.14 / 7.05 | 756 | 0 / 0 | `a71afde5` #3d2f1f | ✅ real-time |
| EVO_TEST_av1_1080p.mp4 | av1 | 1920x1080 | ffmpeg | — | 41.7 | 10.72 / 11.79 | 0 | 0 / 0 | not probed | 🔴 no frames |
| EVO_TEST_av1_4k.mp4 | av1 | 3840x2160 | ffmpeg | — | 41.7 | 10.61 / 11.60 | 0 | 0 / 0 | not probed | 🔴 no frames |
| EVO_TEST_colour_av1_1080p.mp4 | av1 | 1920x1080 | ffmpeg | — | 41.7 | 10.70 / 11.68 | 0 | 0 / 0 | not probed | 🔴 no frames |
| EVO_TEST_colour_h264_1080p.mp4 | h264 | 1920x1080 | native | 0.35 / 0.32 | 41.7 | 8.02 / 6.58 | 244 | 0 / 0 | `8511e6c3` #668464 | ✅ real-time |
| EVO_TEST_colour_h264_fullrange_1080p.mp4 | h264 | 1920x1080 | native | 0.35 / 0.31 | 41.7 | 8.02 / 6.56 | 242 | 0 / 0 | `8d61cd05` #678965 | ✅ real-time |
| EVO_TEST_colour_hevc_1080p.mp4 | hevc | 1920x1080 | native | 0.38 / 0.35 | 41.7 | 7.68 / 6.47 | 284 | 0 / 0 | `6e0c59f0` #668464 | ✅ real-time |
| EVO_TEST_colour_vp9_1080p.webm | vp9 | 1920x1080 | native | 0.34 / 0.30 | 41.7 | 8.37 / 6.44 | 195 | 0 / 0 | `6e0c59f0` #668464 | ✅ real-time |
| EVO_TEST_h264_4k60.mp4 | h264 | 3840x2160 | native | 0.57 / 1.20 | 16.7 | 10.00 / 8.93 | 474 | 0 / 0 | `97f1743c` #807f8f | ✅ real-time |
| EVO_TEST_hevc8_1080p.mp4 | hevc | 1920x1080 | native | 0.48 / 0.40 | 41.7 | 7.10 / 6.47 | 471 | 0 / 0 | `58616608` #76957c | ✅ real-time |
| EVO_TEST_hevc8_4k.mp4 | hevc | 3840x2160 | native | 0.75 / 0.61 | 41.7 | 8.62 / 7.83 | 337 | 0 / 0 | `0bca21ab` #7f7f8f | ✅ real-time |
| EVO_TEST_hevc8_4k60.mp4 | hevc | 3840x2160 | native | 0.56 / 0.59 | 16.7 | 9.74 / 7.80 | 471 | 0 / 0 | `8ad27f38` #7f7f8f | ✅ real-time |
| EVO_TEST_mpeg2_1080p.mp4 | mpeg2video | 1920x1080 | ffmpeg | 1.71 / 2.25 | 40.0 | 8.49 / 6.51 | 202 | 0 / 0 | `248ffb08` #769778 | ✅ real-time |
| EVO_TEST_vp9p0_1080p.webm | vp9 | 1920x1080 | native | 0.40 / 0.33 | 41.7 | 7.21 / 6.49 | 435 | 0 / 0 | `61f134e1` #75937b | ✅ real-time |
| EVO_TEST_vp9p0_4k.webm | vp9 | 3840x2160 | ffmpeg | 24.42 / 37.95 | 41.7 | 8.54 / 7.80 | 360 | 0 / 0 | `788402a8` #7a818e | ✅ real-time |
| GTAVI_An_Extended_Look.mp4 | h264 | 3840x2160 | native | 0.82 / 0.82 | 33.3 | 8.05 / 7.93 | 964 | 0 / 0 | `2df8121f` #010001 | ✅ real-time |
| TearsOfSteel.mkv | vp8 | 1920x800 | ffmpeg | 0.35 / 0.55 | 41.7 | 7.05 / 8.22 | 776 | 0 / 0 | `bb1c3ef7` #0d0f10 | ✅ real-time |
| Test Jellyfin 1080p HEVC 8bit 20M.mp4 | hevc | 1920x1080 | native | 0.41 / 0.45 | 16.7 | 6.95 / 6.68 | 1795 | 0 / 0 | `58a7183f` #da5794 | ✅ real-time |
| sample-30s.webm | vp9 | 1920x1080 | ffmpeg | 126.48 / 241.96 | 33.3 | 6.79 / 6.40 | 9 | 0 / 0 | `f0fb786b` #3c714e | 🔴 decode too slow (7.3x budget) |

20 clips · 16 realtime · 1 slow decode · 3 no frames · 13 native / 7 software

**Colour cross-check** — same source pattern, 4 codecs. Reference is the per-channel median #668464; a path that disagrees by more than 6/255 has a colour bug.

| Clip | Codec | Mean RGB | Δ from median | |
|---|---|---|---|---|
| EVO_TEST_colour_h264_1080p.mp4 | h264 | #668464 | 0 | ✅ |
| EVO_TEST_colour_h264_fullrange_1080p.mp4 | h264 | #678965 | 5 | ✅ |
| EVO_TEST_colour_hevc_1080p.mp4 | hevc | #668464 | 0 | ✅ |
| EVO_TEST_colour_vp9_1080p.webm | vp9 | #668464 | 0 | ✅ |

Worst disagreement: **5/255** — PASS.

### What this run establishes

**Native decode has enormous headroom.** 0.32-0.82 ms/frame at every resolution
and frame rate, with **zero late drops on every clip that decoded**. The two
4K60 clips are the strongest statement: a 60 fps frame period is 16.7 ms, and
native H.264 spent 0.57 ms (p95 1.20) and native HEVC 0.56 ms (p95 0.59) - about
3-7% of budget at 4K60. Whatever limits playback on this console, it is not
`sceVideodec2` throughput, and the CPU-converter era's performance worries do
not apply to the GL path.

**AV1 opens and decodes nothing.** All three AV1 clips report
`be=ffmpeg open=ok dec_n=0 verdict=no_frames`: `avcodec_find_decoder()` succeeds,
so the codec is advertised, and then not one frame comes out. This is a third
state the sweep had to be able to express - it is *not* `no_decoder` (nothing to
open) and *not* `slow_decode` (too slow); the decoder is present and inert.
`validation.md`'s FFmpeg inventory lists `av1` among the built components, which
is how it passes the find-decoder check. Needs its own issue: either the build
carries the AV1 parser without a working decoder, or the decoder is present but
failing silently.

**Native VP9 fails on some files and silently falls back.** Two of four VP9
clips produced `be=native dec_n=0 no_frames` and were retried on FFmpeg (the #57
fatal-fallback path), while the other two decoded natively at 0.34-0.40
ms/frame. The fallback is where it hurts:

| VP9 clip | native | FFmpeg fallback |
|---|---|---|
| `EVO_TEST_vp9p0_1080p.webm` | 0.40 ms | not needed |
| `EVO_TEST_colour_vp9_1080p.webm` | 0.34 ms | not needed |
| `EVO_TEST_vp9p0_4k.webm` | 0 frames | 24.42 ms (p95 37.95, just inside a 41.7 ms budget) |
| `sample-30s.webm` | 0 frames | **126.48 ms (p95 241.96) - 7.3x budget** |

`sample-30s.webm` is the clip that looks "stuck" on the panel. It is not hung:
it is decoding 1080p VP9 in software at roughly 4 fps. A pass/fail sweep would
have recorded it as playing.

**Colour is correct across codecs.** The `colour_*` clips are one source pattern
encoded four ways, so they are decidable against each other on a single run with
no baseline: H.264, HEVC and VP9 all land on **exactly** `#668464`, and the
full-range H.264 variant is 5/255 away. That last number is the interesting one
- both the shader and the old CPU reference assume limited range (`uRange` is an
unwired GL-4 leftover, see the #62 section above), so a full-range source being
only 5/255 off is consistent with it being treated as limited. Small, but it is
the known gap showing up as a number rather than as a shrug.

#### Open: the 10-bit crash

Excluded from the run above. On the previous pass the app **exited** (process
gone - `evo_status` stopped updating; not a hang) immediately after a 1080p HEVC
Main10 clip reached the GL path:

```
EVO vdec native: HEVC10 prof=2 10-bit 1920x1080 unsupported -> FFmpeg
009_FIRST_FRAME_ENTER fmt=4 1920x1080
010_FIRST_FRAME_DECODED gl video path
[process gone]
```

Not yet attributable: that pass also had the two-runner problem, and an
audio-only file had been opened 0.7 s earlier, so a music-mode teardown is in
the frame too. The separate, clean-window finding is that
`EVO_TEST_hevc10_pq_1080p.mp4` managed 4 frames at 150 ms/frame and ended in
`decode_error` with no collision involved - so the FFmpeg 10-bit path is also
unwell independently of the crash.

Corpus for chasing it, all now on `/mnt/usb0`:
`EVO_TEST_hevc10_pq_1080p` (PQ), `EVO_TEST_hevc10_hlg_1080p` (**HLG**),
`EVO_TEST_hevc10_pq_4k`, `EVO_TEST_vp9p2_10bit_1080p` (Profile 2),
`EVO_TEST_h264_high10_1080p`, and the Jellyfin 10-bit file. The HLG clip matters
most: with only PQ in the corpus, a shader applying the PQ curve unconditionally
looks correct, which is exactly how #41 shipped.

#### Next run

- The 10-bit set, on its own, to settle the crash - and a second pass with
  `--native-10bit` so those rows carry both backends.
- AV1: confirm whether the FFmpeg build has a real AV1 decoder at all.
- `--colour-ref` against this run's `evo.log` once there is a second run, which
  turns the Colour column from a signature into match/differs.

---

## GL video path colour parity (#62, delivered by GL-4 / #80)

`#62` was originally "plane-hash A/B: sceAgc vs the CPU converter". GL-4 deleted
the CPU converter, so the question became: does the GLSL fragment shader that
now does YUV→RGB on the video quad produce the same picture the CPU converter
did?

The reference is `pp/src/pp_converter.c`'s `yuv_to_bgra()` as of `b8c42b7` — the
last commit before Stage 3 removed it (`git show b8c42b7:projects/evoplayer/pp/src/pp_converter.c`).
BT.601 limited range, fixed point: `298/409/516/-100/-208`, `(… + 128) >> 8`,
`y - 16`, `uv - 128`. The shader under test is `YUV_MATRIX_GLSL` in
`ui_rml/src/evo_gl_context_device.cpp`.

**Reproduce (host, no console):**

```bash
python3 tools/gl_yuv_parity.py --verbose
```

It evaluates both arithmetics over all 2^24 `(Y,U,V)` triples — a stronger
statement than any single reference frame, which only visits the few thousand
triples that happen to be in it — and fails if any channel differs by more than
1/255.

**Result, 2026-09-10:**

| per-channel Δ | samples | share |
|---|---|---|
| 0 (bit-exact) | 16,674,957 | 99.390% |
| 1 | 102,259 | 0.610% |
| ≥2 | 0 | 0% |

`PASS`. The residual ±1 is rounding, and is not removable: the CPU path rounds a
fixed-point integer (`+128 >> 8`), the shader rounds float→unorm8 in the ROP.

**What the check caught.** The shader as first written (Stage 2a) used `0.0625`
and `0.5` for the black/chroma offsets — 16/256 and 128/256. The CPU converter
works in 0–255, so the offsets are 16/**255** and 128/**255**. That drift cost up
to **2/255 per channel on 3.83% of triples**, worst on saturated reds and greens.
Corrected to `0.0627451` / `0.5019608`, which is what the table above measures.
Colour on the panel looked right either way — this is exactly the class of error
a numeric check exists to find.

**Not covered here** (needs hardware, and is the remaining half of the row):

| Check | Status |
|---|---|
| Panel scanout order — the shader's `.bgr` swizzle matches the ps5-opengl default framebuffer | **verified** (GL-4 hw pass 2026-09-10: correct colour on the video quad and the RG8 OSD composite, no R↔B swap) |
| Chroma siting / bilinear upsample vs the CPU converter's nearest-neighbour | not run — the shader samples chroma with `GL_LINEAR`, so smooth gradients differ slightly by design |
| Full-range (JPEG) sources | not run — both paths assume limited range; `uRange` is a GL-4 leftover hook, not wired to `evo_settings` |
| P010 / 10-bit | not applicable — `#41`'s tail |
