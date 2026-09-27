# Video upscaler (#103)

**Status (2026-09-26): hardware-verified on a PS5 Pro, merged to main.**
- **Hardware:** Off, Sharp and AI Standard / Large / Maximum all render
  correctly. AI Large costs ~1.75 ms per frame (1080p → 4K). AI Maximum's GPU
  cost is not measured yet.
- **Default:** UPSCALING still defaults to **Off**. Issue #103 planned Sharp
  once verified; that switch hasn't been made.
- **Published:** lossless same-frame comparisons and the write-up are at
  https://github.com/sainsaji/ps5-upscalar-research.

A 720p or 1080p file on a 4K panel used to get one bilinear fetch inside the
YUV→RGB shader. **Settings → Playback & Video → UPSCALING** now offers:

| Mode | What runs | Network |
|---|---|---|
| **Off** | the pre-#103 single pass, byte for byte | — |
| **Sharp** | FSR1 EASU (edge-adaptive upsample) → RCAS (sharpen) | — |
| **AI** | Anime4K `Upscale_CNN_x2` in fragment shaders + depth-to-space | **S** (4 convs), **M** (7 convs + 1x1) or **UL** (7 layers x 3 textures + RGB 1x1) - see AI NETWORK |

**AI NETWORK** (same screen) picks the network:

| Option | Network | Measured on the dev PS5 Pro (1080p → 4K, whole frame) |
|---|---|---|
| Auto | Large on a detected PS5 Pro, Standard otherwise | — |
| Standard | Anime4K S: 4 convs x 4 channels | ≤ ~1.1 ms |
| Large | Anime4K M: 7 convs x 4 channels + 1x1 | ~1.75 ms |
| Maximum (Pro) | Anime4K UL: 7 layers x 12 channels + an RGB 1x1 over layers 2-6 | not yet measured |

The override exists because the Pro probe cannot identify every Pro (it fails
on the dev console, which *is* a Pro). Every choice works on any PS5; each
steps down one network (Maximum → Large → Standard) on its own if its
pipelines or scratch memory are missing, or if it goes over the GPU budget.

UL is 36 passes: 21 conv passes (3 per layer, each reading the whole previous
layer), 15 accumulate passes (the final 1x1 conv, per layer 2-6 and per output
colour), and a depth-to-space that adds an RGB residual instead of S/M's luma
one. Its pipe table rows are generated (`shaders/agc/upscale_wide_pipes.inc`).

The settings are lines 14 (UPSCALING) and 15 (AI NETWORK) of
`evo_player_settings.cfg`, each read only when the file has that many lines, so
older files keep Off / Auto.

## Where it lives

| Piece | File |
|---|---|
| Pass generator (FSR1 port, Anime4K hook translation) | `tools/gen_upscale_pipes.py` |
| Generated pipelines (21) | `projects/evoplayer/shaders/agc/upscale_*.pipe` + `_pipe.h` |
| Anime4K sources, unchanged, MIT | `third_party/anime4k/` |
| Upscale stage | `agc_upscale_*` in `media/src/evo_agc_runtime.c`, called from `evo_agc_blit_yuv` |
| API | `evo_agc_upscale_set_mode / _label / _take_downgrade` in `evo_agc_runtime.h` |
| PS5 Pro detection | `media/src/evo_hw.c` / `evo_hw.h` |
| Host maths reference | `tools/upscale_ref.py` |

Regenerate and rebuild the pipelines inside the amdllpc image:

```bash
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml run --rm ps5-dev \
    bash -lc 'python3 tools/gen_upscale_pipes.py && python3 tools/build_agc_pipes.py'
```

## How a frame flows

With the upscaler engaged, the YUV→RGB pass renders at **source** size into
scratch surface `L0`, instead of into the scanout. Then the chain runs:

```
Sharp   L0 --EASU--> E (visible image size, RGBA8) --RCAS--> scanout
AI  S   L0 --conv0--> F0 --conv1--> F1 --conv2--> F0 --conv3--> F1
        L0 + F1 --depth-to-space--> scanout
AI  M   for k in 0..6:  F[k%2] = conv_k(prev)
                        A[k%2] = A[(k-1)%2] + W_k * crelu(F[k%2])   (+ bias at k=0)
        L0 + A --depth-to-space--> scanout
```

- **The footprint is the Off quad's.** The final pass writes only the visible
  image rectangle, with Fit/Fill/Stretch folded into that rectangle and into
  the source UV sub-rect it shows. Letterbox bars and the player-mode "skip the
  clear" logic behave exactly as before.
- **M's last layer is split.** Anime4K M ends in a 1x1 conv over all seven
  feature maps. That conv is linear, so it runs as one accumulate pass per
  layer, and only two feature maps and two accumulators are ever alive.
  `upscale_ref.py --selftest` checks the split against the single conv.
- **Depth-to-space** adds the network's 2x luma residual to a bilinear upsample
  of `L0`. The residual itself is bilinearly resampled off the virtual 2x grid,
  so the output can be any size. mpv instead upsamples its 2x result with its
  own scaler.
- **Barriers.** Each pass is followed by `evo_agc_flush_color_target()`, which
  is RELEASE_MEM event 45 with GCR `0xC`: CB flush plus GLV/GL1 invalidate.
  That is the same barrier the RmlUi blur uses between its H and V passes.
  Ping-ponged feature maps depend on the GLV/GL1 half.

### Scratch surfaces

These are **dedicated** direct-memory blocks of 36 MB slots, allocated on the
first frame that actually needs them and released at shutdown after the GPU
drain: block 0 (180 MB, 5 slots) for every mode, block 1 (288 MB, 8 slots) only
for Maximum. If block 1 cannot be allocated, Maximum runs as Large.

| Slot | Sharp | AI S | AI M |
|---|---|---|---|
| 0 | L0 (RGBA8, source) | L0 | L0 |
| 1 | E (RGBA8, visible image) | F0 (RGBA16F) | F0 |
| 2 | — | F1 | F1 |
| 3, 4 | — | — | A0, A1 (RGBA16F) |

Maximum (UL) uses slot 0 for L0, slots 1-6 for two sets of three feature maps
and slots 7-12 (block 1) for two sets of three accumulators.

- **Why not the RmlUi layer pool (as the issue proposed).** RmlUi CPU-clears a
  layer on acquire. It could do that to a surface the GPU is still upscaling
  the previous frame from.
- **Surface layout (fixed after the first hardware run).** Every colour
  target `setup_color_target()` builds is **64KB_R_X tiled**
  (`CB_COLOR0_ATTRIB3` = `0x4dc6c000`, COLOR_SW_MODE 27). The first build sampled
  the scratch surfaces with a linear T#, and the picture came out as smeared
  horizontal blocks. They are now sampled with
  `evo_agc_build_tsharp_render_target()` (SW_MODE 27, exact target size), and
  slots are 36 MB because a tiled 4K RGBA8 surface is 30x17 64 KB blocks =
  33.4 MB. The RmlUi backdrop blur samples its layers with a *linear* T# too:
  the same latent bug.
- **The RGBA16F target.** It is `setup_color_target()`'s RGBA8 block with
  `CB_COLOR0_INFO` patched to FORMAT=`COLOR_16_16_16_16`, NUMBER_TYPE=FLOAT,
  ROUND_MODE=1 and no BLEND_CLAMP. It is sampled with IMG_FORMAT 71
  (`16_16_16_16_FLOAT`). **Neither has been on hardware.** If AI mode draws
  garbage while Sharp is right, look here first.

## Scope guards and fallbacks

Each of these is logged once per change of source or plan, never per frame:

| Condition | Result | Log |
|---|---|---|
| 10-bit / HDR / HLG source | bypass (v1 is SDR 8-bit) | `agc upscale: bypass ... reason=HDR source` |
| image on panel ≤ 1.05x the source (4K source, 1080p panel) | bypass | `reason=source >= output` |
| AI with < 1.2x scale (Anime4K's own threshold) or a feature map > 36 MB (tiled) | Sharp | `mode=Sharp` |
| a pipeline failed to compile | next mode down | `agc pipe ... unavailable` at boot |
| scratch allocation refused | bypass for the session | `scratch alloc ... FAILED` |
| GPU over budget | step AI Maximum → Large → Standard → Sharp → Off for the session, toast once | `agc upscale: over budget` |

- **The GPU budget.** `frame_end` already blocks on each frame's fence right
  after submit. For upscaled frames, that submit→retire time is logged every
  120 frames as `agc upscale us=<avg> n=120 mode=...`. It is whole-frame GPU
  time, polled every 100 us on upscaled frames (1 ms on others), not a
  per-pass timestamp.
- **The cap.** Two consecutive windows over 12 ms (about 4 s at 60 fps) step
  the mode down one level. Choosing a mode again in Settings clears the cap.
- **Per-pass timing.** Precise per-pass timings need a RELEASE_MEM timestamp
  write. The `sceAgcCbReleaseMem` data-select encoding for that is unverified,
  and guessing wrong could wedge the GPU, so it was left out.

The active upscaler, including the bypass reason, shows in:
- Media Info → Engine card → **UPSCALER**
- the stats overlay's RENDER line

## PS5 Pro detection

`evo_hw_probe()` runs in `Application::initialize()`, before the unjail. It
asks two libkernel queries through `sceKernelDlsym`:
- `sceKernelHasTrinityMode` (`yu17wG8L5FI`)
- `sceKernelIsAuthenticTrinity` (`X0HkB92+NRE`)

It logs:

```
hw: ps5 pro=<0|1|?> trinity_mode=<0|1|?> authentic=<0|1|?>
```

**On the dev PS5 Pro (FW 12.70) this always reports `?`, and there is no
known way from an app module to do better.** What was tried (2026-09-26/27,
details in [psml-research.md](psml-research.md#experiment-log)):

- **`sceKernelDlsym` resolves nothing from the app module.** Even
  `sceKernelUsleep` returns `0x80020003`, so every dlsym miss is meaningless.
- **Direct import is fatal.** Importing `sceKernelIsTrinityMode` (the query
  Sony's PSSR library uses) through a `libkernel.sprx` link stub makes the
  loader reject EVO at launch, before any log is written. Don't retry it.
- **Pro-mode `param.json` flags change nothing observable.** Declaring Pro
  mode (`attribute3 |= 0x400000` plus a `psml` block, as Pro-enhanced games
  have) still launches, but EVO has no way to observe whether it took effect.
  The build flag was removed with the experiment.

So "unknown" is the normal state:
- **Auto** treats unknown as a base PS5 (Standard network).
- The UI says **NOT DETECTED** / "PS5 model unknown".
- **AI NETWORK** is the way to get Large or Maximum on a Pro.

The model shows in Settings → System & Diagnostics → **CONSOLE**, the Media
Info renderer line and the stats overlay.

## PSML (Sony's upscaler)

Researched in [psml-research.md](psml-research.md). EVO does **not** use PSML;
its AI mode is Anime4K in EVO's own shaders. In short:
- **`scePsmlBcSisr*` (single-image SR).** Only in `/system/priv/lib` (not
  importable), built on the system shell's AGC.
- **`scePsmlMfsr*` (PSSR).** Importable, but temporal: it needs game motion
  vectors.
- **Both are gated on Trinity mode**, which EVO can neither detect nor, as
  far as it can observe, enable (above).

## Hardware checklist

1. **Build and deploy.** `package-app.sh --ffpfsc --usb-remote`, then
   `deploy-app.sh --ffpfsc`.
2. **Boot log.** `evo.log` must show:
   - an `agc pipe upscale_* ...` line for all 58 upscale pipes, none
     `unavailable`
   - the `hw:` lines (see [PS5 Pro detection](#ps5-pro-detection))
3. **Play and compare.** `evo-remote.sh play Demos/bbb_1080p_h264.mp4` on a
   4K panel, then a 720p clip. With the video playing, run
   `./tools/evo-remote.sh upcompare`. EVO pauses and redraws the **same** frame
   with the upscaler Off, Sharp, AI Standard, Large and Maximum, with no OSD.
   The five scanouts land in
   `output/upcompare/{off,sharp,ai,ai_large,ai_max}.bmp`. From Git Bash, pass
   console paths as `//mnt/usb0/...`, or MSYS rewrites them. On the host,
   `python tools/upcompare_run.py --montage [--crop x,y,w,h]` writes a
   side-by-side 1:1 crop (`compare.png`) and difference stats against Off.
4. **Check the log.** For each mode, look for:
   - `agc upscale: mode=... net=... rect=...`
   - `agc upscale: 180 MB scratch`
   - `agc upscale us=`
   - in `agc health`: `timeouts=0`, `ring_fail=0`, `tex_fail=0`
   - `map_fail=0`
5. **Diff against the host reference.** Convert the paused frame to RGB at
   source size, run `python tools/upscale_ref.py frame.png --size <image WxH>`,
   and `shot.py diff` each console shot against the matching PNG.
6. **Off regression.** Off must be pixel-identical to a pre-#103 build
   (`shot.py diff` shows zero delta).
7. **Bypass check.** Play a 4K source and an HDR source, and confirm
   `bypass ... reason=` in the log.
8. **On a PS5 Pro.** Repeat with `hw: ps5 pro=1` and `net=anime4k-M` expected.
9. **Quit.** Use QUIT EVO.
