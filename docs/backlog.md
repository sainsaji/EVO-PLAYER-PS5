# Backlog

Ranked. Top of the list is what to pick up next.

This consolidates what was scattered across `ui-handoff.md` §4, the CHANGELOG's
*Known gaps*, and `native-media-research.md`. Those documents stay authoritative
for their own detail — this one decides **order**.

Ranking is by payoff per console round trip. Hardware time is the scarce
resource: every launch needs a jailbreak session, and instances must be closed
between runs (stacking them has kernel-panicked a console).

Two things this file is **not**: [`baseline-defects.md`](baseline-defects.md) is
history, not work — both defects it describes are fixed. And the dead ends in
§11 are closed on evidence; do not reopen them without reading why.

| # | Item | Size | Risk | Payoff |
|---|---|---|---|---|
| 1 | [Codec sweep of the 29-file test set](#1-codec-sweep-of-the-29-file-test-set) | M | none | Know what actually plays, before users tell you |
| 2 | ~~Persist feedback settings~~ | — | — | **Done, unreleased.** See §2 |
| 3 | ~~Larger cover-art cache~~ | — | — | **Done, unreleased.** See §3 |
| 4 | ~~Migrate the four modal screens~~ | — | — | **Done** before this list was written; see §4 |
| 5 | ~~A real home icon~~ | — | — | **Done** before this list was written; see §5 |
| 6 | [Universal subtitle cue counts](#6-universal-subtitle-cue-counts) | M | low | Makes 0.2.0's ranking work on every container |
| 7 | [Decide what sidecar subtitles are](#7-decide-what-sidecar-subtitles-are) | S to investigate | low | Unknown behaviour on a common file type |
| 8 | [Console-native launch (Media tile)](#8-console-native-launch-media-tile) | **built, awaiting first install** | **high** | Removes the second device from first-run |
| 9 | [FFmpeg `full` decoder profile](#9-ffmpeg-full-decoder-profile) | S–M | low | Widens format coverage; needs #1 first |
| ~~10~~ | ~~[Hardware decode spike](#10-hardware-decode-spike-libsceavplayer)~~ | — | — | **CLOSED 2026-08-14 — the definitive no.** The driver refuses the job with ioctl errno 5200, and the goal it existed for (4K without CPU conversion) was met by the GPU compute pipeline instead |
| 11 | [Retire `selected`](#11-chore-retire-selected) | S | none | Dead weight in the loop signature |
| 12 | [Overlay helpers for the subtitle picker](#12-give-the-subtitle-picker-the-overlay-helpers) | S | low | Latent 0.1.3-class freeze; the fix already exists |
| 13 | [Retire `prospero_cover_blit()`](#13-chore-retire-prospero_cover_blit) | S | none | Dead code describing an 80×80 world |

---

## 1. Codec sweep of the 29-file test set

**Recorded in:** `ui-handoff.md` §"Known unfinished business" — *"The 29-file test
set in `/mnt/usb0/test_files_aud_vid/` has not had a full codec sweep."*

Surround output and flip sync both landed, and nothing has systematically
re-run the set since. The console already holds it: AC-3, E-AC-3, TrueHD,
Atmos (bed and objects), DTS, DTS-HD MA, DTS-X, AAC 5.1/7.1, FLAC 5.0/7.1,
LPCM 7.1.

This is first because it is the only item that turns opinion into data, and
because the project has already been burned by believing a message over the
build: [`baseline-defects.md`](baseline-defects.md) records that the app blamed
E-AC3 for failures in files with no audio track at all, while the build
actually links `aac ac3 eac3 dca mp3 mp2 flac opus vorbis alac pcm_*` and
`h264 hevc vp9 vp8 mpeg2video mpeg4 av1 mjpeg` plus `truehd`/`mlp`.

Batch it into as few launches as possible. Highest-value single file is
`LPCM 7.1.wav` — no decoder involved, so a failure there is purely the output
path. Use `Atmos test tones 1000Hz V4.mp4` to confirm channel order is still
`FL FR FC LFE BL BR SL SR` and nothing has swapped centre into a surround.

**Done when** there is a pass/fail table per file in
[`validation.md`](validation.md), and any failure has a cause, not just a
symptom.

## 2. Persist the feedback settings — DONE (unreleased)

Sound and lightbar are appended to the settings file after the theme name and
written whenever the Tools rows are toggled. `%[^\n]` stops at the newline and
the `%d`s that follow skip it, so a file written by any older build still
parses and keeps the defaults. Tools' DEBUG OVERLAY row had the same defect
for a field that *was* already in the file, and now saves too.

**Still to check on hardware:** toggle sound off, exit, relaunch, and confirm
it is still off — and that a settings file written before this change loads.

*Original entry, for the reasoning:*

**Verified in source.** `prospero_settings_save()` (`projects/evoplayer/main.c:13666`)
writes seven fields:

```c
fprintf(file, "%d\n%d\n%d\n%d\n%d\n%d\n%s\n",
        current_profile, prospero_resume_playback_enabled,
        prospero_default_view_mode, prospero_auto_subtitles_enabled,
        show_debug_overlay, evo_sort_folders_first, evo_theme_name(...));
```

Sound and lightbar are not among them, so both reset to defaults on every
relaunch. The Tools rows present themselves as settings and do not behave as
settings — that is a defect, not a missing feature.

Match the existing append-at-the-end pattern (the EVO fields were added that
way and `load()` tolerates short files), so an older settings file still loads.

**Done when** toggling sound off, exiting, and relaunching leaves it off.

## 3. Larger cover-art cache — DONE (unreleased)

320×180 rather than the suggested 400×225: it is 16:9, which is the shape the
source frames are, and it still *minifies* into the tile's 270×162 drawing
area instead of magnifying — art larger than that buys nothing a tile can
show. The cache is 16 slots rather than 48, because only the eight recent
tiles ever ask for a cover; that is 3.7 MB against the old 1.2 MB.

The shelf draws it full bleed (`tile.art_inset = 0`) and cover-*crops* to the
tile rather than stretching, which the hero backdrop already did — a 16:9
cover squashed into a 5:3 tile is a ~7% horizontal squash, invisible on a
gradient and obvious on a face. Verified on the host: the fixture's ring
measures 147px across and 147px tall in the rendered tile.

The one-per-frame budget is unchanged, and the pacing worry turned out to be
misplaced: the expensive part is opening the file and decoding a frame, not
scaling the result. That is now written down in `ui-handoff.md` §3 so the next
person does not re-derive it.

**Still to check on hardware:** entry to the launch screen with eight recent
files, watching for hitch. The argument above is reasoning, not a measurement.

## 4. Migrate the four modal screens — DONE

Already true when this list was written, and `ui-handoff.md` has said so
since: resume prompt and playback-finished go through `evo_screen_dialog()`,
media info through `evo_screen_info()`, the profile picker is an
`evo_list_model`. None draws its own chrome. Ranked here in error.

*Original entry:*

**Recorded in:** `ui-handoff.md` §4.2. The profile picker, resume prompt, media
info and playback-finished screens still draw their own chrome, which is the
same condition that produced ~1900 lines of five drifted list implementations
before `evo_screen_list` replaced them.

Media info is the one to do first: it is where the stale presentation clock
froze playback (`cd8f09e`), so it has already cost a debugging cycle. The
profile picker is reportedly a four-row `evo_screen_list` and should be nearly
mechanical.

**Done when** all four go through `evo_chrome`/`evo_widgets` and none of them
carries its own coordinates.

## 5. A real home icon — DONE

**Done.** `tools/gen_icons.py` has `icon_home()`, it is emitted as
`EVO_ICON_HOME`, and the rail's HOME entry uses `EVO_IC_HOME`. Ranked here in
error, like §4. The set has since gained `icon_logo()` too — the application
mark, replacing the two concentric circles the rail and the launch header
drew in place of a logo.

*Original entry:*

**Recorded in:** `ui-handoff.md` §4.1. The rail's HOME entry reuses the folder
glyph because the generated set has no house. Add one to `tools/gen_icons.py`.

Small, but it is the last obviously-wrong glyph in a UI that is otherwise
screenshot-ready — and screenshots are what the project is being judged on
right now.

## 6. Universal subtitle cue counts

**Recorded in:** CHANGELOG 0.2.0 *Known gaps*.

0.2.0 ranks subtitle tracks by how many cues they declare, which is what stops
a two-cue vanity track from winning on metadata. But the count comes from
mkvmerge's `NUMBER_OF_FRAMES` statistics tags, so:

- containers written by other tools carry no count, and those tracks fall back
  to exactly the metadata ranking that was wrong in the first place
- a track selected mid-film has no cues from earlier in it, because cues are
  collected as packets stream past

Both point at the same fix: count cues from the container rather than trusting
a tag. That means a demux pass, which is expensive — so it wants the same
treatment the browser gives probing (debounce, cache by path, never on the
render thread).

**Done when** a non-mkvmerge container ranks tracks as well as an mkvmerge one,
and switching tracks mid-film shows cues from before the switch point.

## 7. Decide what sidecar subtitles are

**Verified in source.** `prospero_subtitle_sidecar_path()`
(`projects/evoplayer/main.c:10288`) resolves sidecar subtitle files next to the
media. But there is **no libass anywhere in the player** — no `ass_library`, no
`ass_renderer`, no `libass` reference in any `.c` or `.h` under
`projects/evoplayer/`. (`-lass` is on the link line only because FFmpeg's
static archives need it.)

So `.ass`/`.ssa` styling, positioning and karaoke are being flattened through
the player's own text renderer. That may be a perfectly reasonable choice — but
nobody has decided it, and it is a common file type.

This is an **investigation, not a task**: check what extensions the sidecar path
accepts, load a styled `.ass`, and look at what actually reaches the screen.
Then either accept plain text and say so in the README, or scope libass as its
own item. Note the atlas has 69 glyphs and no accents, which constrains how far
styled subtitles can go regardless.

## 8. Console-native launch (Media tile)

**Full analysis:** [`media-tile.md`](media-tile.md).

Today the player needs a browser on a second device to start. The Media-tile
launcher already in `projects/evoplayer/prospero_media_standalone/` puts a tile
on the home screen under **Media**; it compiles clean against SDK v0.42 with the
current player embedded (34.3 MB launcher, verified).

It sits this far down despite the payoff because it is the only UI-adjacent item
that can damage the console rather than the app: it remounts `/system_ex`
read-write and registers a title in `app.db`, and upstream's own README warns
that earlier installers "can corrupt app.db or leave lock / disc-like entries".

**The prerequisites are now done** (`media-tile.md` §5): EVO Player registers
its own `EVOP10001` on port 9056 with its own runtime dir and a generated icon,
coexisting with Prospero's tile rather than replacing it. `scripts/build-media-tile.sh`
builds both payloads and verifies the identity reached the binary.

What is left is the part that cannot be done off-console: install once, confirm
the tile launches, then immediately prove `--uninstall` removes it completely
before relying on it.

## 9. FFmpeg `full` decoder profile

**Recorded in:** `ui-handoff.md` §"Known unfinished business" — the `full`
profile has never been built.

Deliberately behind #1: widening codec coverage before knowing what the current
codec set actually does with real files is optimising blind. Once the sweep has
a failure list, it will say whether `full` is the answer to any of it, and what
it costs in ELF size (the player is already 33.9 MB).

## 10. ~~Hardware decode spike (`libSceAvPlayer`)~~ — **CLOSED, 2026-08-14**

> **This item asked for "4K without CPU conversion — or a definitive no". It
> got both, from different directions, and neither leaves anything to do here.**
>
> **The definitive no.** Ten phases of research took the software side as far
> as it goes: modules load, symbols resolve by NID, decoders are created for
> H.264 and HEVC, memory maps, the compute queue allocates, and
> `sceVideodec2Decode` builds a well-formed command buffer with the right
> resolution and real physical addresses. The driver then refuses the job with
> **ioctl errno 5200**. Every cheap route past it has been tried and closed —
> the alternate ioctl command is unreachable (34 live readings), the driver
> handshake succeeds so nothing was skipped, and `Reset` cannot recover the
> decoder because it waits forever on a completion the refused job will never
> produce. See [hardware-decode.md](hardware-decode.md), which now opens with
> the closure and the list of things that must never be retried.
>
> **And the goal was met anyway.** The reason this item was rated "high" value
> was the 9.15 ms/frame CPU colour convert quoted below. That is gone: the
> **GPU compute YUV→RGB pipeline** does 4K in ~7.4 ms — 2.81× — in
> `pp/src/pp_compute_pipeline.c`. Hardware decode was one way to get there and
> it is no longer the only one, so the risk/reward that justified the spike no
> longer exists.
>
> **Two rules survive this closure and outlive the item**, because they cost
> real console time to learn:
> - Never sweep kernel `.text` from a payload — it panics the console.
> - Never call `sceVideoOutOpen` from a payload — it panics the console, and it
>   returns a bogus handle that passes a `< 0` check.

**Full research:** [`native-media-research.md`](native-media-research.md), and
the `research/hardware-decode` branch for the phase-by-phase findings.

<details>
<summary>The original item, as written before it was closed</summary>

Verified on 12.70 with no proprietary files: `libSceAvPlayer.sprx` loads and all
six probed entry points resolve — `sceAvPlayerInit`, `AddSource`,
`GetVideoData`, `GetAudioData`, `IsActive`, `Close`. Everything today is FFmpeg
software decode plus a CPU colour convert measured at 9.15 ms/frame for 4K.

Last, because it is the only item that can consume a lot of hardware time and
return nothing. Keep it a **timeboxed spike in `projects/avplayer_test`**, not a
change to the player. Two unknowns decide it quickly:

- the PS5 layout of `sceAvPlayerInit`'s argument struct (PS4 takes an
  allocator/callback block; PS5 unconfirmed)
- whether it works without an app sandbox — `videoout_test` established a
  payload has **no user session** (`sceUserServiceGetInitialUser` →
  `0x80940004`), which may be exactly what stops it

The related `libSceVdecCore` thread — modules load, but no PS4-era
`sceVideoDecoder*` name resolves — is a naming problem, not an availability
problem. It needs the real export names via NID reversal or `aerolib.csv`, and
is research, not a deliverable.

If it ever lands, `native-media-research.md` is emphatic on the shape: decoder
behind an interface, FFmpeg software path always available, hardware selected at
**run time** by probing. Never a build-time dependency.

</details>

## 11. Chore: retire `selected`

**Recorded in:** `ui-handoff.md` §4.5. `main.c` still carries the old main-menu
integer. The launch grid ignores it; it is dead weight in the loop signature.

## 12. Give the subtitle picker the overlay helpers

**Verified in source.** The picker sets `screen = SCREEN_SUBTITLE_PICKER` and
draws itself over `draw_player_screen()`. That stalls the decode thread —
every worker loop gates on `screen != SCREEN_PLAYER` — while the presentation
clock's wall-time base keeps running, so on the way back frames are judged
late by however long the picker was open. It also leaves a 4K surface without
remembering to put it back.

That is the 0.1.3 Media Info freeze, and the fix already exists:
`pp_product_overlay_enter()` / `pp_product_overlay_leave()`, which Media Info
and the stop-playback prompt both call. Two lines.

It has shipped since 0.2.0 with no report, so treat it as latent rather than
broken — which is exactly why it is down here and not at the top.

**Done when** opening the picker on a 4K file and dismissing it returns to 4K
playback with no dropped-frame burst.

## 13. Chore: retire `prospero_cover_blit()`

Dead — nothing calls it, and nothing has since the launch shelf stopped
drawing inset thumbnails. It describes an 80×80 world that no longer exists
and paints a hardcoded cyan frame, which is the one thing
[`theming.md`](theming.md) forbids outright. About 45 lines.

---

## Closed — do not reopen without reading why

| Idea | Why it is closed |
|---|---|
| GPU YUV conversion via SDL2 + mesa | The sysroot ships OSMesa (llvmpipe), a **software** rasteriser, with no `radeonsi`. Measurements in [`gpu-notes.md`](gpu-notes.md). The README roadmap still lists this — it is stale. |
| Controller haptics | Built, tested on hardware, removed. Every vibration entry point either returns success and does nothing or rejects the call, while `scePadSetLightBar` succeeds on the same handle. Full probe table in [`ui-handoff.md`](ui-handoff.md). |
| `package-pkg.sh --format app` for the player | `make_fself.py` requires a static `ET_EXEC`; every payload here is PIE. Structural, not a missing flag — see [`packaging.md`](packaging.md) and [`media-tile.md`](media-tile.md) §1. |
| Signed fPKG | Requires Sony's proprietary `prospero-pub-cmd`. [`packaging.md`](packaging.md) §3. |
| The two baseline defects | Surround and flip sync are **fixed**. [`baseline-defects.md`](baseline-defects.md) is kept as root-cause history; mistaking it for a backlog has already cost one session's planning time. |
