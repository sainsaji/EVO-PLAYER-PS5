# EVO Player — Improvements Roadmap

> **Document Version:** 1.0.0  
> **Last Updated:** 2026-08-14  
> **Scope:** All actionable improvements across playback, UI, audio, input, memory, and codec coverage.  
> **Sources:** [`backlog.md`](backlog.md), [`reng-analysis-integration.md`](reng-analysis-integration.md), [`baseline-defects.md`](baseline-defects.md), [`ui-handoff.md`](ui-handoff.md)

---

## Priority Matrix

| Priority | Item | Size | Risk | Domain | Status |
|:---:|---|:---:|:---:|---|---|
| 🔴 **P1** | [Subtitle Picker Overlay Fix](#p1--subtitle-picker-overlay-fix) | XS | None | Playback / UI | Ready |
| 🔴 **P1** | [Codec Sweep — 29-file Test Set](#p1--codec-sweep--29-file-test-set) | M | None | QA / Validation | Ready |
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
Opening the subtitle picker on a 4K file and dismissing it returns to 4K playback with no dropped-frame burst and correct surface state.

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

## Closed — Do Not Reopen

| Item | Why Closed |
|---|---|
| GPU YUV via SDL2 + Mesa | Sysroot ships OSMesa (llvmpipe, software rasteriser). No `radeonsi`. See [`gpu-notes.md`](gpu-notes.md). The README roadmap still lists this — it is stale. |
| Controller haptics | Built, tested on hardware, removed. Every vibration entry point either returns success and does nothing or rejects the call. Full probe table in [`ui-handoff.md`](ui-handoff.md). |
| `package-pkg.sh --format app` | `make_fself.py` requires static `ET_EXEC`; all payloads here are PIE. Structural, not a missing flag. See [`packaging.md`](packaging.md). |
| Signed fPKG | Requires Sony's proprietary `prospero-pub-cmd`. See [`packaging.md`](packaging.md) §3. |
| Hardware video decode (`libSceAvPlayer`) | Modules load and all 6 entry points resolve on 12.70. Blocked on PS5 `SceAvPlayerInitData` layout (unconfirmed vs PS4) and sandbox restriction (no user session in payload slot). Timeboxed spike in `projects/avplayer_test/` only. See [`hardware-decode.md`](hardware-decode.md). |
