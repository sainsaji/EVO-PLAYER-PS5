# Changelog

Notable changes per release. The release workflow lifts the matching section
into the GitHub release notes, so keep the headings in the form `## 0.1.0`.

---

## 0.2.0

Subtitles, and the interface for choosing them.

### New

- **Subtitle track picker.** DOWN during playback lists every track in the
  file with the number of cues it declares, the active one marked, and the
  film still playing behind it. It replaces cycling, which reopened the file
  on every step - on a disc rip with thirty-four tracks that meant thirty-odd
  reopens to reach the one you wanted.
- Track names are built from the language code, not the container's title.
  The font atlas has no accents or parentheses, so a track really titled
  `Español (España)` would draw as a row of holes; fifty language codes map
  to ASCII names, and same-language tracks are numbered rather than shown as
  duplicate rows.

### Fixed

- **Subtitles appeared to be broken and were not.** A release group ships a
  vanity track tagged English, flagged default, holding two cues whose first
  lands thirty-seven minutes in. Selection picked it on metadata alone and
  then correctly displayed nothing for a whole episode. mkvmerge records
  `NUMBER_OF_FRAMES` per track, so the cue count is readable before a packet
  is demuxed, and it now outranks every other signal: a track with fewer than
  ten cues loses even when it is English and flagged default. Tracks that
  lose this way are still offered in the picker, dimmed and marked
  `SIGNS ONLY`, because the count can itself be wrong.
- **The marquee scrolled at whatever speed the render loop happened to be
  running.** It advanced a fixed number of pixels per frame, and the loop
  runs anywhere from 36fps with a preview decoding to 60fps on a settled
  list, so one filename scrolled at two visibly different rates. It measures
  milliseconds now and travels 180px/s regardless.
- **The marquee also stepped a glyph at a time**, because the offset was
  computed in pixels and then applied by dropping whole characters. The
  sub-character remainder is applied as a negative x, with the glyph
  overhanging each end clipped by keeping and restoring the strips either
  side. Checked on the host: no ink outside the box across 376 phases,
  largest step 3px against a 17px advance.
- **`EXTRA_CFLAGS` never reached the compiler** when building from Windows.
  The re-exec into the dev container forwarded `PS5_HOST` and `PS5_PORT` and
  dropped everything else, so a `-D` switch produced a successful build, a
  clean install, and a binary without it. Forwarded now, and the build greps
  its own compile line and fails if a requested flag did not land.

### Known gaps

- Cue counts come from mkvmerge's statistics tags. Containers written by
  other tools do not carry them; those tracks are ranked on metadata as
  before and show no count in the picker.
- Switching tracks reopens the file and seeks back, so it costs the same
  pause as a seek. Cues are collected as packets stream past, so a track
  selected mid-film has no cues from earlier in it.

---

## 0.1.0

A rebuild of the interface, and the tooling to work on it without a console.

### New

- **Launch screen.** A hero that resumes what you were last watching, a
  *Jump back in* shelf, and a *Library* shelf. Two-dimensional cursor: each
  shelf remembers its own column, and empty shelves are skipped rather than
  becoming dead stops.
- **File browser inspector.** Selecting a file shows a frame from it plus
  type, container, size, length, resolution and codecs, read from the
  container itself. Probing is debounced, so scrolling never stalls.
- **Side navigation rail.** Sections are reachable from each other instead of
  each being a dead end you had to back out of. Back is a stack now, so a
  screen opened from two places returns to the right one.
- **Hold to scroll.** Every list moved one item per physical press before.
  Shoulder buttons page; L2/R2 jump to the next initial in the browser.
- **Themed toast**, and a `danger` theme token for failure states. Existing
  `.theme` files inherit it.
- **Playback OSD follows the theme** — panel, seek bar, scrubber, chapter
  marks, captions and the music visualiser.
- **Theme swatches** on the settings row, so cycling themes is not blind.
- **L3 captures a screenshot during playback** (R3 keeps subtitle delay).

### Fixed

- The eighth settings row was visible but unreachable — navigation wrapped at
  a hardcoded count that had drifted from the real one. That whole class of
  bug is gone; counts now live with the cursor.
- **Audio failures were all blamed on E-AC3**, including files with no audio
  track at all. The message was hardcoded and overwrote the accurate one.
  E-AC3, AC-3, DTS, TrueHD, FLAC, Opus and ALAC are all present and linked.
- **Pixelated previews** — thumbnails were point-sampled twice, once down and
  once up. Minification now box-filters and the preview is presented 1:1.
- The 4K converter created and joined worker threads **every frame**, the
  pattern already documented here as causing a freeze. It uses a persistent
  pool: 4K conversion measured 11.57 ms → 9.15 ms per frame on the host.
- Commas and parentheses rendered as gaps: the font atlas has neither, and an
  unknown glyph leaves a hole rather than being skipped.
- Scrims painted as hard black slabs — `evo_ui_vgrad` replaces rather than
  blends, so a transparent colour was written straight into the framebuffer.
- The expanded rail was not quite opaque, letting the page title ghost
  through it.

### Removed

- **Haptics.** Built, tested on hardware, and taken out: every vibration
  entry point in `libScePad` either reports success and does nothing or
  rejects the call, while `scePadSetLightBar` succeeds on the same handle.
  Sound and lightbar remain. Evidence is in `docs/ui-handoff.md` so nobody
  re-derives it.

### Tooling

- `tools/uiplay.sh` — the UI as a navigable page in a browser, on any
  machine. Real renderer, real font atlas, real icons.
- `tools/uiview.sh` — render any screen to a PNG.
- `tools/shot.sh` — fetch captures and interrogate them numerically: probe a
  coordinate, scan pixel runs, crop, diff.
- `tools/klog.sh` — console log, timestamped, append-only, survives payload
  restarts.
- `tools/launch.sh` — refuses to stack app instances, which is what
  kernel-panicked a console during development.
- `tools/bench.sh` — host benchmark for the converter, with correctness
  hashing and ASan/TSan modes.

### Known gaps

- The player OSD is themed but still draws its own layout rather than the
  shared chrome.
- `gpu-notes.md` previously recommended an SDL2/mesa path for GPU YUV
  conversion. That does not work: the sysroot ships OSMesa (llvmpipe), a
  software rasteriser, with no `radeonsi`. The document now records the
  measurements.

---

## 0.0.2

Plug-and-play theming, SDF-drawn cards, generated vector icons, navigation
sounds. Four built-in themes plus `.theme` files from USB.

## 0.0.1

First release of the EVO Player fork: 7.1 surround output with stereo
fallback, flip-synchronised presentation, a faster tile swizzle, and
folders-first browsing.
