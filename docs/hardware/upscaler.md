# Video upscaler (#103)

**Status (2026-09-26): code-complete and host-verified, hw-verify-pending.**
Every pipeline compiles with amdllpc, the host compile check is clean, and
`tools/upscale_ref.py --selftest` passes. Nothing here has run on a console
yet, which is why the setting still defaults to **Off**. Change the default to
Sharp once the hardware checklist below passes.

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
resolves two libkernel queries with `sceKernelDlsym`:

- `sceKernelHasTrinityMode` (NID `yu17wG8L5FI`)
- `sceKernelIsAuthenticTrinity` (NID `X0HkB92+NRE`)

It tries handle `0x2001`, then `0x2`, then asks the loader for
`libkernel_sys.sprx` / `libkernel.sprx`, trying each symbol by name and then by
NID. Neither symbol is in the SDK's libkernel stubs, which is why this is not
an import: a missing import is a null import and crashes the app module at
load.

The result is logged once:

```
hw: ps5 pro=<0|1|?> trinity_mode=<0|1|?> authentic=<0|1|?>
```

An unresolved symbol reports `?` and the console is treated as a base PS5.

The model shows in three places:
- Settings → System & Diagnostics → **CONSOLE**
- the Media Info renderer line
- the stats overlay

It also picks the AI network (M on a Pro, S otherwise).

## PSML SISR on PS5 Pro — spike not run

The go/no-go needs two things this session did not have:
- **A PS5 Pro**, to list `/system/common/lib` over FTP (read-only) for the PRX
  exporting `scePsml*`.
- **A decrypted Pro-enhanced game dump** that calls SISR, to recover the
  argument structs offline. This is the same method as the shader ripper.

**Never scan console or kernel memory for it.**

The rules the spike must follow:
- **No-go if the module is only in `/system/priv/lib`.** A fake-signed app
  module cannot import from there, and the attempt bricks the load
  (`tools/native-app/stubs/prx/README.md`).
- **If it is a go:** add `libScePsml.syms` containing only the SISR functions
  EVO actually imports, or `package-app.sh`'s dead-import guard fails the
  build. Add an `AI (PSML)` label, and fall back to the shader CNN on any
  error.

Until then, the Pro runs AI mode as Anime4K M in shaders, and the build has
no PSML imports.

## Hardware checklist

1. **Build and deploy.** `package-app.sh --ffpfsc --usb-remote`, then
   `deploy-app.sh --ffpfsc`.
2. **Boot log.** `evo.log` must show:
   - an `agc pipe upscale_* ...` line for all 21 pipes, none `unavailable`
   - `hw: ps5 pro=0 ...` on a base PS5
3. **Play and compare.** `evo-remote.sh play Demos/bbb_1080p_h264.mp4` on a
   4K panel, then a 720p clip. With the video playing, run
   `./tools/evo-remote.sh upcompare`. EVO pauses and redraws the **same** frame
   with the upscaler Off, Sharp, AI Standard and AI Large, with no OSD. The four
   scanouts land in `output/upcompare/{off,sharp,ai,ai_large}.bmp`. On the host,
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
