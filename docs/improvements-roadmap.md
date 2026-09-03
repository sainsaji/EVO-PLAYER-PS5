# EVO Player — Improvements Roadmap

> **Document Version:** 1.0.0  
> **Last Updated:** 2026-08-14  
> **Scope:** All actionable improvements across playback, UI, audio, input, memory, and codec coverage.  
> **Sources:** [`backlog.md`](backlog.md), [`reng-analysis-integration.md`](reng-analysis-integration.md), [`baseline-defects.md`](baseline-defects.md), [`ui-handoff.md`](ui-handoff.md)

---

## Priority Matrix

| Priority | Item | Size | Risk | Domain | Status |
|:---:|---|:---:|:---:|---|---|
| 🔴 **P1** | [Subtitle Picker Overlay Fix](#p1--subtitle-picker-overlay-fix) | XS | None | Playback / UI | **Resolved 2026-08-14** |
| 🔴 **P1** | [Codec Sweep — 29-file Test Set](#p1--codec-sweep--29-file-test-set) | M | None | QA / Validation | Ready |
| 🔴 **P1** | [10-bit Video Falls Off the Fast Path](#p1--10-bit-video-falls-off-the-fast-path-entirely) | M | Low | Playback / Performance | **New 2026-08-14** |
| 🔴 **P1** | [`https://` Is Parsed But Not Implemented](#p1--https-is-parsed-but-not-implemented) | M | Low | Addons / Network | **New 2026-08-14** |
| 🟠 **P2** | [The `sws_scale` Fallback Is Single-Threaded](#p2--the-sws_scale-fallback-is-single-threaded) | XS | Low | Playback | **New 2026-08-14** |
| 🟠 **P2** | [Rotate Buffers Bypass the Slab Allocator](#p2--the-rotate-buffers-bypass-the-slab-allocator) | S | Low | Memory | **New 2026-08-14** |
| 🟠 **P2** | [Make the Codec Sweep Measure Time](#p2--make-the-codec-sweep-measure-time-not-just-passfail) | XS | None | QA / Validation | **New 2026-08-14** |
| 📝 **Fix** | [Correction — "GPU Compute" Is CPU SIMD](#correction--the-gpu-compute-pipeline-is-cpu-simd) | XS | None | Docs / Naming | **Resolved 2026-08-14** |
| 🟠 **P2** | [Universal Subtitle Cue Counts](#p2--universal-subtitle-cue-counts) | M | Low | Subtitles | Planned |
| 🟠 **P2** | [DualSense Touchpad Timeline Scrubbing](#p2--dualsense-touchpad-timeline-scrubbing) | M | Low | Input | Planned |
| 🟠 **P2** | [Dynamic Audio Re-routing Detection](#p2--dynamic-audio-re-routing-detection) | S | Low | Audio | Planned |
| 🟠 **P2** | [Native PS Notifications & PS Button Banners](#p2--native-ps-notifications--ps-button-banners) | S | Low | System | Planned |
| 🟡 **P3** | [Sidecar Subtitle Format Investigation](#p3--sidecar-subtitle-format-investigation) | S | Low | Subtitles | Investigate |
| 🟡 **P3** | [FFmpeg `full` Decoder Profile](#p3--ffmpeg-full-decoder-profile) | M | Low | Codec | After #Codec Sweep |
| 🟡 **P3** | [AJM DSP — Dialogue Booster & DRC](#p3--ajm-dsp--dialogue-booster--drc) | M | Medium | Audio | Planned |
| ⚪ **Chore** | [Retire `selected`](#chore--retire-selected) | XS | None | Code Health | Ready |
| ⚪ **Chore** | [Retire `prospero_cover_blit()`](#chore--retire-prospero_cover_blit) | XS | None | Code Health | Ready |

---

## P1 — Subtitle Picker Overlay Fix

**Source:** [`backlog.md` §12](backlog.md#12-give-the-subtitle-picker-the-overlay-helpers)  
**Size:** XS — 2 lines of code  
**Risk:** None  

### Problem
Opening the subtitle picker during 4K playback (`screen = SCREEN_SUBTITLE_PICKER`) stalls the decode thread — the worker loop gates on `screen != SCREEN_PLAYER` — while the wall-clock presentation timer keeps running. On dismissal, every buffered frame is judged late by however long the picker was open, causing a burst of dropped frames. The 4K output surface state is also not restored.

### Fix
`pp_product_overlay_enter()` / `pp_product_overlay_leave()` already exist. Media Info and the stop-playback prompt both call them correctly. The subtitle picker simply doesn't.

```c
// On picker open:
pp_product_overlay_enter();

// On picker close:
pp_product_overlay_leave();
```

### Done When
Opening the subtitle picker on a 4K file and dismissing it returns to 4K playback with no dropped-frame burst and correct surface state. *(Resolved 2026-08-14 — pp_product_overlay_enter() / leave() wired into evo_subs_open(), evo_subs_activate(), and CIRCLE cancel).*

---

## P1 — Codec Sweep — 29-file Test Set

**Source:** [`backlog.md` §1](backlog.md#1-codec-sweep-of-the-29-file-test-set)  
**Size:** M (console time, not code)  
**Risk:** None  

### Problem
The 29-file test set at `/mnt/usb0/test_files_aud_vid/` has not had a complete codec sweep since surround and flip sync landed. Results are opinion, not data — the project has already been burned by this (`baseline-defects.md` records the app blaming E-AC3 for failures in files with no audio track at all).

### Test Coverage
| Format | File | Expected |
|---|---|---|
| LPCM 7.1 | `LPCM 7.1.wav` | Pass (no decoder path, purely output path) |
| Atmos bed+objects | `Atmos test tones 1000Hz V4.mp4` | Confirm channel order FL FR FC LFE BL BR SL SR |
| AC-3 / E-AC-3 | Various | Pass |
| TrueHD | Various | Pass |
| DTS / DTS-HD MA / DTS-X | Various | Pass |
| AAC 5.1 / 7.1 | Various | Pass |
| FLAC 5.0 / 7.1 | Various | Pass |
| HEVC Main10 | High-bitrate | Measure decode time |
| AV1 | Various | Pass / measure CPU load |

### Done When
A pass/fail table per file exists in [`validation.md`](validation.md), and every failure has a cause, not just a symptom.

---

## P2 — Universal Subtitle Cue Counts

**Source:** [`backlog.md` §6](backlog.md#6-universal-subtitle-cue-counts)  
**Size:** M  
**Risk:** Low  

### Problem
Subtitle track ranking uses `NUMBER_OF_FRAMES` statistics tags from mkvmerge. Containers written by other tools carry no count, so tracks fall back to broken metadata ranking. Additionally, a track selected mid-film has no cues from before the switch point because cues are only collected as packets stream past.

### Fix
Count cues directly from the container via a demux pass, debounced and cached by path, run off the render thread. Same treatment as the browser's thumbnail probe.

### Done When
A non-mkvmerge container ranks tracks as well as an mkvmerge one, and switching tracks mid-film shows accurate cue counts from before the switch point.

---

## P2 — DualSense Touchpad Timeline Scrubbing

**Source:** [`reng-analysis-integration.md`](reng-analysis-integration.md) §Phase 2  
**Size:** M  
**Risk:** Low  

### Problem
The DualSense touchpad is unused. `ScePadData.touchData` already delivers X/Y coordinates in the existing `scePadRead()` loop — no additional API calls required.

### Design
| Gesture | Action |
|---|---|
| **Horizontal swipe** | Timeline seek with acceleration curve |
| **Vertical swipe** | Volume adjustment |
| **Two-finger tap** | Play / Pause toggle |
| **Two-finger swipe left/right** | Previous / Next chapter marker |

### Implementation Target
- `evo_input.c` — map `touchData[0].x` delta to a seek offset in seconds
- Deadzone: ignore deltas < 20px to prevent accidental seeks from finger resting
- Acceleration curve: small delta → slow seek (±5s), large delta → fast seek (±30s)

### Done When
Horizontal swipe scrubs the timeline smoothly during playback without triggering button inputs.

---

## P2 — Dynamic Audio Re-routing Detection

**Source:** [`reng-analysis-integration.md`](reng-analysis-integration.md) §Phase 2  
**Size:** S  
**Risk:** Low  

### Problem
If a user plugs headphones into the DualSense during playback, or switches HDMI output from a 7.1 AVR to a 2.0 TV, the audio channel layout does not update. Playback continues with the wrong downmix matrix.

### Fix
Poll `sceAudioOutGetPortState()` on the existing audio thread for `RerouteCounter`. When it increments, rebuild the downmix matrix for the new output configuration without stopping playback.

### Done When
Plugging headphones into the DualSense 3.5mm jack during 7.1 playback automatically downmixes to stereo with no stutter.

---

## P2 — Native PS Notifications & PS Button Banners

**Source:** [`reng-analysis-integration.md`](reng-analysis-integration.md) §Phase 2  
**Size:** S  
**Risk:** Low  

### Problem
Background operations (Emby library sync, USB directory indexing) have no visible status. Users have no way to tell if a background operation is running or stuck.

### Implementation
- `libSceNotification` — fire-and-forget toast for completed operations (e.g. `"Emby: Library Synced (247 items)"`)
- `ShowPsButtonBanner()` — persistent background task banner beside the PS button (e.g. `"Indexing USB Drive..."`)

### Target Integration Points
| Event | Notification |
|---|---|
| Emby library sync complete | Toast: `"Emby: Library updated"` |
| USB indexing in progress | PS Button Banner: `"Scanning USB..."` |
| Subtitle download done | Toast: `"Subtitle loaded"` |
| Playback resume found | Toast: `"Resume position found"` |

---

## P3 — Sidecar Subtitle Format Investigation

**Source:** [`backlog.md` §7](backlog.md#7-decide-what-sidecar-subtitles-are)  
**Size:** S (investigation)  
**Risk:** Low  

### Problem
`prospero_subtitle_sidecar_path()` resolves `.ass`/`.ssa` sidecar files, but **there is no libass in the player**. Styled `.ass` subtitles are being silently flattened through the custom text renderer with a 69-glyph atlas and no accent support.

### Investigation Steps
1. Load a styled `.ass` file with positioning, colour, and bold/italic styling
2. Observe what actually renders on screen
3. Decide: accept plain text and document it in the README, or scope libass as a separate item

### Done When
Either a documented decision is in the README ("EVO Player renders `.ass` as plain text") or a libass integration is scoped and on the backlog.

---

## P3 — FFmpeg `full` Decoder Profile

**Source:** [`backlog.md` §9](backlog.md#9-ffmpeg-full-decoder-profile)  
**Size:** M  
**Risk:** Low  

### Problem
The `full` FFmpeg decoder profile has never been built. The current profile links `h264 hevc vp9 vp8 mpeg2video mpeg4 av1 mjpeg` plus `truehd`/`mlp` — formats not on this list silently fail.

### Prerequisite
**Must follow the codec sweep (#1).** Widening codec coverage before knowing what fails with real files is optimising blind. The sweep will identify which missing codecs are actually encountered.

### Likely Additions
- `mpeg1video`, `prores`, `dnxhd`, `theora`, `wmv*`, `vp6`, `rv*`
- Note: ELF is currently 33.9 MB — `full` profile will increase this

---

## P3 — AJM DSP — Dialogue Booster & DRC

**Source:** [`reng-analysis-integration.md`](reng-analysis-integration.md) §Phase 3  
**Size:** M  
**Risk:** Medium (`libSceAjm` job memory alignment requirements)  

### Problem
Action movies have a large dynamic range — whispered dialogue at -30dB and explosions at 0dB. No DRC (dynamic range compression) or dialogue enhancement exists in EVO Player today.

### Implementation
- `libSceAjm` hardware-assisted audio DSP pipeline
- **Night Mode DRC:** Compress dynamic range to -12dB headroom
- **Dialogue Enhancer:** +3dB centre channel boost for speech clarity  
- **Fallback:** FFmpeg software `af_loudnorm` filter (100% viable alternative if `libSceAjm` is restricted)

### Settings Integration
Add to Settings → Audio:
- `NIGHT MODE` toggle (DRC on/off)
- `DIALOGUE BOOST` toggle

---

## Chores

### Chore — Retire `selected`

**Source:** [`backlog.md` §11](backlog.md#11-chore-retire-selected)  
**Size:** XS  

The old main-menu `selected` integer in `main.c` is dead weight. The launch grid ignores it entirely. Remove it and clean up any references in the loop signature.

---

### Chore — Retire `prospero_cover_blit()`

**Source:** [`backlog.md` §13](backlog.md#13-chore-retire-prospero_cover_blit)  
**Size:** XS (~45 lines)  

Nothing calls this function. It describes an 80×80 world that no longer exists and hardcodes a cyan frame, which [`theming.md`](theming.md) forbids outright. Delete it.

---

## P1 — 10-bit Video Falls Off the Fast Path Entirely

**Found:** 2026-08-14, reading `convert_frame_to_rgb()` and `pp_map_avframe()`.
**Size:** M **Risk:** Low **Domain:** Playback / Performance

### Problem

`pp_map_avframe()` ([`main.c:5245`](../projects/evoplayer/main.c)) accepts
exactly **three** pixel formats:

```c
AV_PIX_FMT_YUV420P   AV_PIX_FMT_YUVJ420P   AV_PIX_FMT_NV12
```

Anything else fails the map and `convert_frame_to_rgb()` falls through to
`sws_scale()`. The code says so itself:

```c
/* 10-bit / odd formats → sws (much slower). One-shot toast. */
```

**That fallback is the entire 4K HDR case.** HEVC Main10 decodes to
`yuv420p10le` (or `p010le`), so every 10-bit file — which is essentially all
4K HDR REMUX — misses the AVX2 workgroup pipeline and lands on a
**single-threaded** `sws_scale`. The 8 MB sequential read-ahead and the slab
allocator were both built for exactly those files, and then the frame path
drops them onto the slow route.

So the player has a fast 8-bit path and a slow 10-bit one, and the headline use
case is the slow one. The measured 7.43 ms at 4K applies to content that is not
the content people bring to a 4K player.

### Fix

Add a 10-bit input variant to the existing kernel in
[`pp_compute_pipeline.c`](../projects/evoplayer/pp/src/pp_compute_pipeline.c).
The workgroup pool, band splitting and threading are all reusable unchanged —
what changes is the load and a `>> 2` on each component (`yuv420p10le` is
16-bit little-endian samples with 10 significant bits), plus a `p010le` variant
where chroma is interleaved like NV12 but 16-bit.

Then extend `pp_map_avframe()` to accept both, which is where the routing
decision actually lives.

### Done When

- A 4K HEVC Main10 file reports the AVX2 backend, not the sws fallback.
- The one-shot "much slower" toast stops firing on 10-bit content.
- `bench.sh` carries a 10-bit row next to the 8-bit ones, so the win is a
  number rather than a claim.

---

## P2 — The `sws_scale` Fallback Is Single-Threaded

**Found:** 2026-08-14. **Size:** XS **Risk:** Low **Domain:** Playback

Whatever remains on the fallback after the item above — 4:2:2, 4:4:4, and
10-bit until it lands — runs through a `sws_getContext()` built with default
options, which means **one thread** for the whole frame.

swscale can slice-thread, but the thread count has to be set before the context
is initialised, so `sws_getContext()` cannot express it. The shape is:

```c
play_sws = sws_alloc_context();
/* set srcW/srcH/srcFormat/dstW/dstH/dstFormat/flags via av_opt_set_int */
av_opt_set_int(play_sws, "threads", n, 0);
sws_init_context(play_sws, NULL, NULL);
```

It is a handful of lines and it multiplies the fallback by roughly the core
count. Worth doing **even though** P1 should retire most of its traffic —
because "most" is not "all", and a 4:4:4 file should not fall to one thread.

---

## P2 — The Rotate Buffers Bypass the Slab Allocator  ✅ DONE (#6, 2026-09-03)

**Found:** 2026-08-14. **Size:** S **Risk:** Low **Domain:** Memory

**Resolution (#6):** the CPU-side video buffers that churn the heap on every
open/seek now come from the `evo_direct_mem` slab, grow-only (allocated once,
reused across seeks / re-opens at the same-or-smaller resolution):

- `convert_frame_via_sws` rotate ring — `VIDEO_ROTATE_BUFFERS` trimmed **8 → 3**
  (it is only the exotic-pixfmt fallback path; the product path presents through
  `pp_playback`'s display model), slab-backed.
- `pp_playback` `display` / `display_back` / `nv12_fb` — slab-backed, grow-only
  (`display_cap`).
- `P8_31_RETURN_OK` now logs `dmem=used/total`.

**Not done, deliberately** (hardware-learned 2026-09-03):
- `pp_videoout` `cpu_bufs` stays on `malloc`. At 4K it is 3×33 MB and the V8
  GPU present path never reads it; routing it in just pressures the slab.
- The pool stays at **64 MiB**. A 192 MiB WB_ONION reservation competes with the
  GPU / sceAgc / VideoOut direct-memory budget and **wedged the first 4K V8
  present on hardware** (`dmem=31M/192M hw=1`, then a hang right after
  `006B_VO_RECONFIG_APPLIED`). The 4K display / staging buffers spill to
  `malloc()` via `evo_direct_mem_alloc`'s graceful fallback — exactly as before.

Original analysis below.

---

`VIDEO_ROTATE_BUFFERS` is **8**, and each one is allocated with a plain
`malloc(video_frame_w * video_frame_h * 4)`. At 4K that is
`3840 × 2160 × 4 = 33.2 MB` each — **265 MB from the heap**, in the one code
path that also runs the slow converter.

Meanwhile [`evo_direct_mem.c`](../projects/evoplayer/media/src/evo_direct_mem.c)
exists precisely to avoid this, is initialised at startup with a 64 MB region,
and measured 1.50× faster allocation with zero fragmentation. The rotate
buffers do not use it, and at 4K they could not fit in it as currently sized.

Two things to decide together, which is why this is one item and not two:
whether the pool should be sized for 4K rotate buffers, and whether **eight**
buffers is the right number at 4K — the count looks inherited from a
lower-resolution era, and each one costs 33 MB.

---

## P1 — `https://` Is Parsed But Not Implemented

**Found:** 2026-08-14, reading `evo_net.c` against the module inventory.
**Size:** M **Risk:** Low **Domain:** Addons / Network

### Problem

[`evo_net.c`](../projects/evoplayer/addons/src/evo_net.c) accepts an
`https://` URL, strips the scheme and sets the port to 443 — and then opens a
**plain BSD socket** and calls `send()`/`recv()` on it. There is no TLS
anywhere in the tree: no mbedTLS, no OpenSSL, no wolfSSL, and no `-lssl`,
`-lcrypto` or `SceSsl` in the link line.

```c
} else if (strncmp(p, "https://", 8) == 0) {
    p += 8;
    default_port = 443;     /* ...and then plaintext send() to port 443 */
}
```

So the URL parses, the connection opens, and the server — which is speaking
TLS — never replies to a plaintext `GET`. **Any Emby or Jellyfin server behind
HTTPS simply does not work**, which is most remote servers and a good share of
LAN ones. The failure surfaces as a timeout or an empty response, not as
"HTTPS is unsupported", so it looks like a broken server rather than a missing
feature.

Accepting the scheme is what makes this a bug rather than a limitation. A
client that rejected `https://` outright would at least be honest.

### Fix

Two routes, and the native one is available:

1. **`libSceHttp.sprx`** — confirmed present on 12.70 via `reng`. It is the
   console's own HTTP(S) client and handles TLS, redirects and connection
   reuse. It needs the same load-and-resolve-by-NID treatment every other Sony
   module in this project has had, and that path is well understood here.
2. **Bundle a small TLS library** (mbedTLS) and keep the existing socket code.
   No console research, larger ELF, and the player is already 33.9 MB.

Start by reading `libSceHttp`'s exports — the technique is proven and costs no
console time.

### Done When

- An `https://` Emby/Jellyfin server authenticates, lists libraries and streams.
- A TLS failure reports *itself* — bad certificate, handshake failure — rather
  than timing out.
- If neither route lands, `https://` is **rejected at entry** with a message
  saying so, instead of being silently attempted in plaintext.

---

## P2 — Make the Codec Sweep Measure Time, Not Just Pass/Fail

**Amends:** [P1 — Codec Sweep](#p1--codec-sweep--29-file-test-set)
**Size:** XS (a column, not a project) **Risk:** None

Now that hardware decode is closed, **decode is permanently on the CPU**, and
that changes what the sweep needs to record. A pass/fail table answers "does it
play"; it does not answer "does it play *at rate*", which is the question that
now has no other way of being answered.

Add per-file **decode ms/frame and dropped-frame count** to the
[`validation.md`](validation.md) table. The GPU compute pipeline took colour
conversion down to ~7.4 ms at 4K, so conversion is no longer the suspect when a
high-bitrate file stutters — decode is, and this is the measurement that says
so. It also tells the [FFmpeg `full` profile](#p3--ffmpeg-full-decoder-profile)
item whether a codec is missing or merely too slow, which are different
problems with different fixes.

---

## Correction — The "GPU Compute" Pipeline Is CPU SIMD

**Found:** 2026-08-14, reading
[`pp_compute_pipeline.c`](../projects/evoplayer/pp/src/pp_compute_pipeline.c).

**The performance is real. The attribution is not.** That file contains no
`sceGnm*` call, no shader, no dispatch and no compute queue. It is a
multi-threaded AVX2 converter — `compute_kernel_avx2_8px_fast()`,
`compute_workgroup_process_band()`, a persistent `compute_pool_worker` pool.
What makes it *look* like a GPU path is that `get_backend_name()` returns the
string:

```c
#if defined(EVO_TARGET_PS5)
    return "GPU Compute (RDNA2 Direct / GNM)";
```

On the PS5 build it reports RDNA2 and GNM while executing AVX2 on the CPU.
That label has propagated into
[`reng-analysis-integration.md`](reng-analysis-integration.md), where "GPU
Compute YUV→RGB Pipeline" is recorded as 100% complete.

**Why this is worth correcting rather than shrugging at.** The measured numbers
(0.75 ms at 1080p, 7.43 ms at 4K, 2.81×) stand — they were measured, and the
work behind them is real and good. But three things follow from the label being
wrong:

1. **The GPU is still entirely unused.** That is an untapped avenue, not a
   completed one, and the current docs say the opposite.
2. Anyone optimising later will reason from "the conversion is already on the
   GPU, so the CPU is free" — and both halves of that are false.
3. It sits directly next to [`gpu-notes.md`](gpu-notes.md), which correctly
   documents that there is no hardware GL/Vulkan and that raw GNM would be a
   large reverse-engineering project. The two documents currently contradict
   each other.

**Fix (Applied 2026-08-14):** renamed backend strings to "CPU SIMD (AVX2 8-Wide Workgroups)" / "CPU SIMD (AVX2 8-Wide Vectorized)" across `pp_compute_pipeline.c`, `pp_compute_pipeline.h`, and changelogs. Measured performance (0.75ms at 1080p, 7.43ms at 4K) is preserved.

---

## Closed — Do Not Reopen

| Item | Why Closed |
|---|---|
| GPU YUV via SDL2 + Mesa | Sysroot ships OSMesa (llvmpipe, software rasteriser). No `radeonsi`. See [`gpu-notes.md`](gpu-notes.md). The README roadmap still lists this — it is stale. |
| Controller haptics | Built, tested on hardware, removed. Every vibration entry point either returns success and does nothing or rejects the call. Full probe table in [`ui-handoff.md`](ui-handoff.md). |
| `package-pkg.sh --format app` | `make_fself.py` requires static `ET_EXEC`; all payloads here are PIE. Structural, not a missing flag. See [`packaging.md`](packaging.md). |
| Signed fPKG | Requires Sony's proprietary `prospero-pub-cmd`. See [`packaging.md`](packaging.md) §3. |
| Hardware video decode (`libSceVideodec2` / `libSceAvPlayer`) | **CLOSED 2026-08-14 — a definitive no, after ten phases.** The old reason given here (init-struct layout, no user session) was wrong on both counts: the struct is fully recovered, the app slot *does* have a user session, decoders are created for H.264 and HEVC, and `sceVideodec2Decode` builds a correct command buffer. **The driver refuses the job with ioctl errno 5200**, and every cheap route past it is now closed — alternate ioctl command unreachable (34 live readings), driver handshake succeeds so nothing was skipped, and `Reset` can never recover the decoder. The goal it existed for was met by the GPU compute pipeline instead. See [`hardware-decode.md`](hardware-decode.md), which opens with the closure and the two calls that **panic the console** and must never be retried. |
| Hardware JPEG / PNG decode for cover art | Looked for on 2026-08-14 and not found: no `libSceJpeg*`, `libScePng*` or image-codec module appears in the `reng` corpus for 12.70, while `libSceHttp`, `libSceIme`, `libSceNotification` and `libSceFont` all do. Cover art stays on `stb_image`. *Caveat: that corpus is dominated by firmware file paths and the vsh prefetch list, so this is strong absence-of-evidence, not proof.* |
