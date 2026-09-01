# main.c modularisation plan

> **Status:** in progress. `src/evo_toast.c`, `src/evo_recent.c`,
> `src/evo_favorites.c` and four `media/src/*.c` modules are already carved
> out; `main.c` is **~18,800 lines / ~225 top-level functions**. The Makefile
> now compiles per-object (`%.o: %.c`), so each extraction genuinely buys
> rebuild time — the note in [architecture.md](architecture.md) that it does
> not is out of date.
>
> **This revision re-scopes the plan** around one goal: get a clean decoder
> seam out of `main.c` so the native hardware-decode work
> ([evo-pro/native-decode-plan.md](evo-pro/native-decode-plan.md)
> Phase 3) has somewhere to land. That is **Track A** below and it is the
> priority. **Track B** is the rest of the carve and can proceed in any order
> around it.

---

## Rules (unchanged, and they are the point)

1. **No behaviour change.** Each extraction is a pure move + `extern` wiring.
   Verify before starting the next:
   - `./scripts/build-evoplayer.sh` clean, no new warnings;
   - `./tools/bench.sh` plane hashes unchanged (converter path untouched);
   - `./tools/uiview.sh --all` renders identical (diff the PNGs);
   - where practical, same ELF `sha256` from the same sources.
2. **Leaf modules first.** Never extract two interdependent sections in one
   step. A module that only *reads* shared state through a small accessor set
   is a leaf; one that needs 40 raw `extern`s is not — redesign the seam.
3. **Carve where the globals already cluster, not where instinct says.**
   `main.c`'s file-scope globals are smeared across enormous spans —
   `g_pp_pb` is touched across ~17,900 lines, `player_paused` across ~17,300.
   Publishing those as raw `extern`s turns a tangle into a tangle with a
   header. The fix is a **façade of accessor functions** (§4), not an
   `evo_state.h` extern dump. The superseded version of this plan proposed the
   dump; do not.
4. **One header = one complete interface.** Private functions stay `static`.
   Include guard `EVO_<MODULE>_H`. Add each `.c` to the right `_SRCS` list in
   `projects/evoplayer/Makefile` and re-run `scripts/gen-compile-commands.sh`.

---

## Region map (current line numbers, from the marker scan)

`main.c` still carries `/* PROSPERO_*_START/END */` markers — these are the
seams. Decode-critical regions are **bold**.

| Lines | Region marker | Target module | Track |
|---|---|---|---|
| 264–273 | `SETTINGS_EARLY_GLOBALS` | `evo_settings.c` | B |
| 1687–1932 | `AUDIO_RESAMPLER_STATE` | `media/evo_audio_resample.c` | **A** (audio decode dep) |
| ~2000–2360 | audio output thread, sfx thread, `PacketQueue` | `media/evo_audio_out.c` + `media/evo_packet_queue.c` | **A** |
| 2370–3410 | `SUBTITLE_*`, `EMBEDDED_SUBTITLE_MODULE` | `media/evo_subtitle.c` | **A** (demux feeds it) |
| 3432–3434 | `TOAST_STATE` | already `src/evo_toast.c` (finish) | B |
| **5055–5180** | `stop_video_playback` | **`media/evo_playback.c`** | **A** |
| **5460–5862** | `TRUE_AV_SEEK`, `demux_thread_func` | **`media/evo_demux.c`** | **A** |
| **5866–5913** | `video_decode_thread_func` | **`media/evo_playback.c`** | **A** |
| **5915–6119** | audio sample helpers, `mix_audio_frame_to_queue` | **`media/evo_audio_out.c`** | **A** |
| 6119–6431 | `AUDIO_TRACK_SWITCH` | `media/evo_audio_out.c` | **A** |
| **6506–7366** | `start_video_playback` (open, stream select, thread spawn) | **`media/evo_playback.c`** + `media/evo_demux.c` | **A** |
| **7398–7660** | `decode_next_video_frame` (decode + A/V pacing — **split**, §5) | **`evo_vdec_ffmpeg.c`** + `media/evo_playback.c` | **A** |
| 8002–8594 | `PLAYER_OSD_MODULE` | `evo_osd.c` | B (reads playback façade) |
| 8601–9993 | `SRT_MODULE`, `SUBTITLE_CONTROLS` | `media/evo_subtitle.c` | A/B |
| 11685–12210 | `DYNAMIC_MEDIA_LABELS` | `evo_media_meta.c` | B |
| 12406–13204 | `PERSISTENT_SETTINGS` | `evo_settings.c` | B (touched by native-decode Phase 5) |
| 16624–16990 | toast renderer, real seek, netflix scrub, scrub hold | respective modules | B |
| 16993–17109 | `USB_FAVORITES` | `src/evo_favorites.c` (finish) | B |
| 17113–17570 | `PLAYBACK_COMPLETE_MODULE` | `evo_playback_complete.c` | B |
| 17669–end | `main()`, init, main loop, input dispatch, screen routing | **stays in `main.c`** | — |

---

## Track A — the decoder seam (priority; blocks native decode)

### A.0 Target structure

```
projects/evoplayer/media/
  include/
    evo_packet_queue.h     AVPacket FIFO with cap + clear         (leaf)
    evo_audio_out.h         audio decode→resample→AudioOut path    (leaf-ish)
    evo_subtitle.h          embedded + SRT subtitle engine
    evo_demux.h             one thread: av_read_frame → queues
    evo_vdec.h              DECODER INTERFACE (see native-decode plan §3)
    evo_playback.h          session façade: open/close/seek/clock/pace
  src/
    evo_packet_queue.c
    evo_audio_out.c
    evo_audio_resample.c    (already planned; pull in here)
    evo_subtitle.c
    evo_demux.c
    evo_vdec_ffmpeg.c       avcodec_* + pp_map_avframe, PURE (no clocks)
    evo_playback.c          the play loop, A/V sync, thread lifecycle
```

`evo_vdec.h` is defined verbatim by the native-decode plan §3. Track A's job
is to make `evo_vdec_ffmpeg.c` exist and be the *only* place `avcodec_*` video
calls live, so `evo_vdec_native.c` can slot in beside it later.

### A.1 Ordered steps

Each row is one commit, build-clean before the next.

| # | Extract | From (lines) | Depends on | Risk | Notes |
|---|---|---|---|---|---|
| A1 | `evo_packet_queue` | 2040–2120 | nothing | none | `PacketQueue`, push/pop/count/clear. Pure leaf — the two queue instances move to `evo_demux` in A5. |
| A2 | `evo_audio_resample` | 1687–1932 + resampler fns | FFmpeg only | low | Already scoped in the old plan; do it here. |
| A3 | `evo_audio_out` | ~2000–2360, 5915–6431 | A1, A2 | medium | audio_output_thread, `mix_audio_frame_to_queue`, audio queue ring, track switch. Owns `audio_clock_seconds` / `audio_pts_seconds` — expose via `evo_audio_clock_seconds()`. |
| A4 | `evo_subtitle` | 2370–3410, 8601–9993 | A1 | medium | Demux hands it packets; keep `prospero_embedded_subtitle_decode_packet()` as the entry point. Large but self-contained behind its markers. |
| A5 | `evo_demux` | 5460–5860, packet-queue instances | A1, A4 | medium | `demux_thread_func`, seek-request handling, the two `PacketQueue` instances, `video_stream_index` / `audio_stream_index`. Depends only on "is paused?" and "seek pending?" from the façade. |
| A6 | `evo_vdec.h` + `evo_vdec_ffmpeg.c` | **split** 7398–7530 + `pp_map_avframe` (5304–5350) | A1 | **high** | The core move. Pull the pure decode out of `decode_next_video_frame` — see §5. `play_ctx`, `play_frame`, `avcodec_send/receive`, `pp_map_avframe`, `pp_map_yuv420p10_to_8` all move here. |
| A7 | `evo_playback` | 5055–5180, 5866–5913, 6506–7366, pacing half of 7398–7660 | A3, A5, A6 | **high** | `start_video_playback`, `stop_video_playback`, `video_decode_thread_func`, the A/V-sync pacing, thread lifecycle. Defines and owns the façade (§4). |
| A8 | façade cleanup | across `ui/`, `evo_osd.c`, screen draw, main loop | A7 | medium | Replace scattered `extern int player_paused;` etc. with `evo_pb_*()` calls. This is what makes A7's header small. |

### A.2 What `main.c` keeps from playback

`main.c` still owns the *policy*: which file to open (browser selection),
when to call `evo_playback_open()` / `_close()` / `_seek()`, and the
`pp_playback` (`g_pp_pb`) + `pp_videoout` objects — those belong to the app,
not the decode session (VideoOut ownership is a hard rule,
[hardware-decode.md](hardware-decode.md)). `evo_playback.c` gets a pointer to
`g_pp_pb`, it does not own it.

---

## 4. The playback façade (why this replaces the extern dump)

`evo_playback.h` exposes **functions, not globals**. Everything outside the
playback core that currently reads a playback global goes through one of
these:

```c
/* state queries — replace scattered `extern` reads */
int      evo_pb_is_active(void);        /* was: video_decode_ready / screen==SCREEN_PLAYER */
int      evo_pb_is_paused(void);        /* was: player_paused */
int      evo_pb_is_eof(void);           /* was: video_decode_done */
double   evo_pb_position_s(void);       /* was: video_clock_seconds - first_video_pts_seconds */
double   evo_pb_duration_s(void);
double   evo_pb_audio_clock_s(void);    /* was: audio_clock_seconds */
void     evo_pb_queue_depth(int *vpkts, int *apkts, int *ablocks);  /* debug overlay */
int      evo_pb_active_backend(void);   /* EVO_VDEC_BACKEND_* — for the OSD status line */

/* commands — the only ways main.c drives playback */
int      evo_pb_open(const char *path, double resume_s);
void     evo_pb_close(void);
void     evo_pb_set_paused(int paused);
void     evo_pb_seek(double target_s);
```

Counting the current call sites: the smeared globals are read from **~30
places** in UI/OSD/screen/debug code and written from **~6** (all inside the
would-be `evo_playback.c`). The façade turns 30 fragile `extern`s into ~10
stable function calls, and the writes become private. That is the whole
argument for doing playback via a façade instead of a header of `extern`s.

`evo_audio_out.h` gets the same treatment for `audio_clock_seconds` etc.
(`evo_audio_clock_seconds()`), so `evo_playback.c` and `evo_vdec_ffmpeg.c`
don't cross-include each other.

---

## 5. Splitting `decode_next_video_frame()`

Today (`main.c:7398`) this one function does **four** jobs:

1. `avcodec_receive_frame` / pull-and-`send_packet` loop over the video queue;
2. compute `video_clock_seconds` from the frame PTS;
3. A/V-sync pacing — sleep while video is ahead of `audio_clock_seconds`,
   drain non-keyframes when badly behind, host-clock fallback when audio
   hasn't primed;
4. `convert_frame_to_rgb(play_frame)` → `pp_playback_push_frame`.

The native backend must be able to do **1** without knowing anything about
**2–4**. Split as:

```c
/* evo_vdec_ffmpeg.c — pure, no clocks, no sleeps, no pp_playback */
int evo_vdec_send(evo_vdec*, const uint8_t *au, int len, int64_t pts_us);
int evo_vdec_receive(evo_vdec*, pp_frame *out);   /* fills pp_frame, sets pts_us */

/* evo_playback.c — video_decode_thread_func body */
static void pump_one_video_frame(void) {
    if (evo_vdec_receive(v, &pf) != 1) { feed_from_queue_or_return(); return; }
    update_video_clock(pf.pts_us);          /* job 2 */
    pace_against_audio_clock();             /* job 3 */
    pp_playback_push_frame(&g_pp_pb, &pf);  /* job 4 */
}
```

`feed_from_queue_or_return` is the existing `packet_queue_pop` +
`video_video_pending_pkt` logic, moved as-is. The pacing block (jobs 2–3)
moves verbatim into `pace_against_audio_clock()` — no logic change, just a
new function boundary. This is the highest-risk single step (A6+A7); do it
with the codec sweep from [validation.md](validation.md) run before and after.

---

## Track B — the rest of the carve (parallel, lower priority)

Same rules, no native-decode dependency. Rough order by risk:

| # | Module | Lines | Risk |
|---|---|---|---|
| B1 | finish `evo_toast` / `evo_favorites` (renderers still in `main.c`, 16624, 16993) | ~250 | none |
| B2 | `evo_media_meta` (chapters, compat report, dynamic labels 11685–12210) | ~800 | low |
| B3 | `evo_settings` + `evo_settings_screens` (12406–13204, settings draw) | ~2,300 | low — **coordinate with native-decode Phase 5** which adds a row here |
| B4 | `evo_osd` (8002–8594) — reads the façade from §4 | ~600 | medium |
| B5 | `evo_playback_complete` (17113–17570) | ~460 | low |
| B6 | `evo_cover` (cover-art cache, thumbnail, browser preview) | ~650 | medium |
| B7 | `evo_browser` (file browser, USB nav — the deepest UI/state user) | ~2,400 | high — do last |
| B8 | `evo_launch_screen`, `evo_screen_draw` (grid, artwork, per-screen draw) | ~3,200 | low–medium |

---

## Coordination with the native-decode plan

| This plan | Native-decode plan | Ordering |
|---|---|---|
| Track A (A1–A8) | Phase 3 (decoder abstraction refactor) | **A *is* Phase 3's prerequisite** — do A1–A7 first, then Phase 3 is just "add `evo_vdec_native.c` beside `evo_vdec_ffmpeg.c`" |
| B3 (`evo_settings`) | Phase 5 (settings toggle) | Land B3 before Phase 5, or Phase 5 edits `main.c` and B3 has to rebase |
| — | Phase 0 (ABI harvest), Phase 1 (app-slot bring-up) | Independent — can run in parallel with Track A |

**Recommended sequence:** A1→A5 (leaf + demux + audio, ~1 week of careful
moves), then A6→A8 (the decoder seam, the risky part), then hand off to
native-decode Phase 3/4. Track B fills gaps whenever Track A is blocked on a
console verification.

---

## Success criteria

- [ ] `avcodec_*` **video** decode calls appear in exactly one file
      (`evo_vdec_ffmpeg.c`); `grep -n avcodec_receive_frame main.c` is empty.
- [ ] `main.c` no longer declares `play_ctx`, `play_fmt`, `player_paused`,
      `video_clock_seconds`, the packet queues, or the decode threads.
- [ ] Nothing outside `media/` includes an FFmpeg header except `main.c`'s
      remaining demux-open glue (ideally zero).
- [ ] `main.c` under ~9,000 lines after Track A, under ~1,500 after Track B.
- [ ] Every checkpoint: build clean, `bench.sh` hashes stable, `uiview --all`
      pixel-identical, codec sweep ([validation.md](validation.md)) unchanged.
- [ ] `scripts/gen-compile-commands.sh` re-run; clangd resolves every symbol.
