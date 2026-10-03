# Motion smoothing / frame interpolation (#105)

**Hardware-verified on the dev PS5 Pro, 2026-10-02** (Tears of Steel, 1080p
24 fps, FFmpeg backend): High costs **~0.55 ms of whole-frame GPU time against
a 12 ms budget**, survives seeks, and never downgrades. Measured per stage:
pyramid 0.21 ms, + motion estimation 0.42 ms, + median and warp 0.55-0.60 ms.

**Status: experimental. Off by default, and it lives under Settings →
EXPERIMENTAL, not with the finished video settings.** It synthesises
intermediate frames so 24 fps film moves at the panel's refresh rate. It will
never be artefact-free: the frames it shows were never photographed, and where
the motion search is wrong the picture is wrong with it.

| Mode | Search | Synthesis |
|---|---|---|
| **Off** | — | the pre-#105 path, byte for byte |
| **Low** | coarse-to-fine down to **whole pixels** | warp capped at 20 px, half strength |
| **High** | coarse-to-fine down to **quarter-pel** | warp capped at 56 px, full strength |

It turns itself off for sources it cannot help or cannot afford: **≥ 50 fps**
(already smooth), **10-bit / HDR** (the pipeline is 8-bit RGBA), **above
1080p**, and when the GPU budget says so. The OSD badge and
`evo_agc_motion_smoothing_label()` name the reason.

## The passes

Three of the four run **once per source frame pair** — 24 times a second, not
60 — which is what pays for a real search. Only the warp runs per presented
vsync.

| Pass | Target | Runs | What it does |
|---|---|---|---|
| `interp_pyr` | ¼ res, RGBA16F | per pair | Prefiltered luma for **both** frames in one target: R = luma A, G = luma B, B/A = local activity. Four bilinear taps give four 2×2 box averages, so the coarse search matches filtered luma instead of aliased point samples. |
| `interp_me` | 1 texel per **16×16** source block, RGBA16F | per pair | Logarithmic search on the pyramid (steps 8/4/2/1 of a quarter-res texel = **±60 source px**), then full-resolution refinement at 2/1/0.5/0.25 px. Outputs `(mv.x, mv.y, residual, confidence)`. |
| `interp_median` | same grid | per pair | Confidence-weighted **vector** median over 3×3, plus a 5×5 mean residual. |
| `interp_warp` | scanout, or L0 when an upscaler follows | **per vsync** | Edge-aware vector upsample, then synthesis. |

The phase comes from the presentation clock
(`pp_playback_get_interp_phase`): `t = (now − PTS_A) / (PTS_B − PTS_A)`, which
on a 60 Hz panel with 24 fps material cycles 0.0, 0.4, 0.8, 0.2, 0.6 without
drifting against audio. Video runs **one source frame behind** audio as a
result — interpolating between two frames means holding the newer one until the
pair is complete. That is inherent to the technique, not a bug to fix here.

When there is no phase to interpolate at — paused, scrubbing, between files —
the phase is **1.0**, which is "show frame B exactly". Phase 0.0 would hand
back frame A and the picture would jump back a frame on every pause.

## What the artefacts were, first time round

The first implementation (`9337ed4`) looked wrong on hardware for reasons worth
keeping written down, because most of them are the classic ones:

- **Tie-break.** `best_sad` started at `1e6`, the first candidate tested was
  `(−16,−16)`, and the test was `sad < best_sad`. SAD ties across every
  candidate inside a sky, a wall or a letterbox bar, so the first candidate
  won and whole regions picked up a bogus 16 px vector — at full confidence,
  because confidence was `1 − mean_sad*4` and the residual there is ~0. **Fix:
  a magnitude penalty in the cost, and confidence measured against local
  activity and against what standing still already explains.**
- **4 px vector granularity** (the fine step was 4, with no sub-pixel stage):
  every moving edge doubled. **Fix: refinement to quarter-pel.**
- **±20 px search range.** A 24 fps camera pan moves further than that in one
  frame, so the search simply failed and the result was a crossfade ghost.
  **Fix: ±60 px via the pyramid.**
- **32×32 blocks.** The vector field could not follow an object's edge.
  **Fix: 16×16, plus an edge-aware upsample in the warp instead of plain
  bilinear — a bilinear ramp between an object's vector and the background's
  vector IS the halo.**
- **Per-pixel scene-cut test** (`mean_sad > 0.22` → nearest frame) left a
  patchwork of two different frames across one picture. **Fix: the decision is
  driven by the 5×5 mean residual, so a region switches together.**
- **Low mode halved the motion vector**, which is worse than no vector at all —
  it guarantees every block lands in the wrong place. **Fix: Low shortens the
  search and the warp, it does not scale the answer.**
- **Point-filtered ME inputs**, so there was no prefilter and no sub-pixel
  sampling to refine into.
- **Slot collision.** The vectors lived in scratch slots 1/2, which is where
  Anime4K Standard/Large put their feature maps — so with an AI upscaler on,
  the field was overwritten between presents of the same pair. **Fix:
  everything interpolation owns moved into the extended scratch block (slots
  7–11), which only Anime4K UL reaches, and UL already steps down to Large
  while smoothing is on.**

Mid-confidence is handled by scaling the **warp distance** rather than
cross-fading a warped image with an unwarped one. Half weight then means one
image displaced half way, not two images on top of each other.

## What hardware found that the host could not

Four defects, none of which a compile or a host render would have shown:

- **A shader that needed scratch killed the console.** `interp_median`'s
  `vec4 v[9]` indexed by a loop counter made amdllpc spill to private memory
  (`scratch_en: true, scratch_memory_size: 160`). The AGC runtime programs no
  scratch ring, so the GPU wrote to an unmapped address: the process died with
  **no EVO log line at all**, and the only evidence was
  `GPU_FAULT_PAGE_FAULT_ASYNC` in ShadowMount's `debug.log`. Every tap is now a
  named variable, and **`tools/build_agc_pipes.py` refuses any shader that
  reports scratch**, so this cannot reach the console again.
- **Smoothing stopped at the first seek and never came back.** The
  "discontinuity, restart from frame A" branch left `pts_b` at its pre-seek
  value, and its own test is `pts - pts_b > 200 ms` - which stays true forever
  once the stream has jumped. B was never refilled, every later frame took the
  same branch, and the player quietly fell back to passing frame A through for
  the rest of the session. The branch now clears `pts_b`.
- **Frame A and Frame B were the same buffer.** The resident slot numbers were
  only ever set by `evo_agc_motion_smoothing_reset()`, which nothing called, so
  both stayed at the zeroed struct's 0 - scratch slot 0, which is also the
  upscaler's input. Set at init now, with a self-heal in the plan.
- **A false "GPU over budget - turned off" toast** on the first frame of every
  session: `downgrade_notice` defaults to 0, and 0 is `MOTION_SMOOTH_OFF`. It
  is initialised to -1 ("nothing to report").

**Do not judge motion quality from `evo-remote.sh key l3` screenshots.**
`evo_agc_runtime_read_scanout` reads a live 33 MB scanout with the CPU while
the GPU keeps flipping, so a capture taken during fast motion mixes several
frames into full-width horizontal bands that look exactly like a torn warp.
The debug views below prove it: a *weight map* shows the same bands, and a
weight map cannot tear for any reason but the capture.

## Tuning it

The confidence thresholds cannot be judged from a correct-looking picture.
Write a digit to **`/mnt/usb0/evo_interp_debug`** and the warp draws what it is
actually being told instead of the video:

| Value | View |
|---|---|
| `1` | motion vectors (R = x, G = y, ±32 px full scale) and the area residual in B |
| `2` | confidence, after the median |
| `3` | the final warp weight, after the occlusion and gain guards |

Read once per boot, so deploy, then launch. `/mnt/usb0/evo_interp_stage`
(1 = pyramid only, 2 = + motion estimation, 3 = + median) stops the chain early
and forces the warp to pass frame A through, which is how the scratch fault
above was bisected to one pass without a rebuild per step. Both files survive a
deploy, so delete them when finished.

The weight view is the one to tune against. The first thresholds left most of a
dark, detailed frame black (plain cross-fade, no real interpolation); loosening
them too far made neighbouring blocks land on opposite sides of the gate, a
16 px chequer of warped and unwarped squares. The settled answer: the median
hands on the **mean** confidence of its 3x3 rather than the winner's, so the
vector stays sharp at a motion boundary while the amount of trust varies
smoothly, and High gets a looser occlusion gate than Low.

## Where it lives

| Piece | File |
|---|---|
| Pass generator | `tools/gen_interp_pipes.py` |
| Generated pipelines | `projects/evoplayer/shaders/agc/interp_{pyr,me,median,warp}.pipe` + `_pipe.h` |
| Stage | `agc_interp_*` / `agc_motion_smoothing_plan` in `media/src/evo_agc_runtime.c` |
| API | `evo_agc_motion_smoothing_set_mode / _set_phase / _label / _take_downgrade` |
| Phase | `pp_playback_get_interp_phase` in `pp/src/pp_playback.c` |
| Setting | `MotionSmoothing` in `core/include/evo/Common.hpp`, line 22 of `evo_player_settings.cfg` |

Regenerate and rebuild the pipelines inside the amdllpc image:

```bash
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml run --rm ps5-dev \
    bash -lc 'python3 tools/gen_interp_pipes.py && python3 tools/build_agc_pipes.py'
```

`build_agc_pipes.py` rewrites `shaders/agc/evo_agc_pipes.h` from **every**
`.pipe` it is given, so always run it with no arguments.

## Known limits

- Video is one source frame behind audio while it is on (see above).
- Occlusion is handled by falling back, not by resolving it: where one frame
  can see something the other cannot, the warp shortens and the area softens.
- The GPU budget (12 ms window) steps High → Low → Off for the session and
  toasts once. Low really is cheaper — it skips the two finest search steps.
- Memory: the extended scratch block is 288 MB, allocated on first use.
- **Stacking it on AI upscaling makes both worse.** The search runs on the
  picture it is handed, so it estimates motion from — and then interpolates —
  whatever Anime4K invented, and the two sets of artefacts compound around
  fast motion. Both are allowed on together; the `MOTION SMOOTHING` row says
  so when `UPSCALING` is `AI` (`SettingsScreen.cpp`, the `MotionSmoothing`
  case). Sharp (FSR 1) does not have the same problem.
