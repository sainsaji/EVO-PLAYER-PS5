# EVO Player UI — state of play

Written so the next session can start without re-deriving anything. Read this
first, then [`theming.md`](theming.md) for the theme format and
[`tooling.md`](tooling.md) for how to build, capture and measure.

---

## 1. Where things stand

Two passes have landed.

**v0.0.2 — theming.** Every colour and metric moved into one struct
(`evo_theme`), drawing moved onto SDF primitives (`evo_ui`), and icons became
monochrome so they could be tinted. Verified on hardware.

**Current pass — architecture.** The theming pass left the player *looking*
consistent but not *being* consistent: each screen still drew its own header,
footer and cards at its own coordinates, and navigation was two long
`if (screen == …)` chains. That is now a real UI layer under
`projects/evoplayer/ui/`.

### The layer

| Module | Responsibility |
|---|---|
| `evo_metrics.h` | The layout grid. Every margin, row pitch and column edge, in one file. |
| `evo_draw` | Text and icons, reached through a vtable. Measurement, ellipsising, marquee, and ink-bounds vertical centring. |
| `evo_nav` | Screen ids, section metadata, and a navigation **stack**. |
| `evo_focus` | One selection model for every list, plus `evo_grid` for the launch screen's shelves. |
| `evo_input` | Raw pad bits → semantic actions, with auto-repeat. |
| `evo_feedback` | Sound, haptics and lightbar behind one semantic API. |
| `evo_chrome` | Page chrome: background, header, footer hints, side navigation rail. |
| `evo_widgets` | Row, tile, shelf label, property table, preview panel, scrollbar, progress, empty state. |
| `evo_screens` | The screens themselves, drawn from plain model structs. |

Nothing in `ui/` includes the decoder or the ~1 MB of asset headers, so the
whole layer compiles in about a second and is clean under `-Wall -Wextra`. It
can be exercised on the host under sanitizers.

`main.c` binds the renderers at startup:

```c
evo_draw_bind(&EVO_DRAW_VTABLE);   /* rr_text, rr_text_w, rr_icon, … */
evo_feedback_init(pad, evo_sfx_play);
```

The vtable exists because the font atlas and icon sheets are `static const`
arrays in headers only `main.c` includes; a second translation unit including
them would duplicate every byte in the ELF.

### Screens

- **Launch** — full-bleed home. Hero (resume the last file), a *Jump back in*
  shelf, and a *Library* shelf of six destinations. Two-dimensional cursor:
  each shelf remembers its own column, and vertical movement skips shelves
  that are empty.
- **Browse** — rail, file list, and an **inspector**: preview frame with a
  duration badge, then type, format, container, size, length, resolution and
  codecs. Empty fields are skipped rather than shown blank.
- **Recent, Favorites, Settings, Tools, About** — all one function
  (`evo_screen_list`) taking a model. They were ~1900 lines of five separate
  implementations that had already drifted apart in row height and margin.

### Defects fixed in this pass

- The `%N` wrap bug class is gone. Counts now live with the cursor in
  `evo_focus`, so a list cannot grow past its own navigation arithmetic —
  which is what made the eighth settings row unreachable.
- **Holding a direction did nothing.** Every list moved one item per physical
  press. `evo_input` adds repeat (380 ms, then 110 ms, tightening to 45 ms),
  and L1/R1 page through the browser.
- **`EVO_UI_H` was both the include guard and the framebuffer height** in
  `evo_ui.h`, so every file including it emitted `-Wmacro-redefined`.
- Sections were dead ends — from Favorites the only route to Settings was
  back out and in again. The rail (LEFT from any list) makes lateral moves
  possible, and Back is now a stack pop, so a screen reached from two places
  returns to the right one.
- The header rule faded out halfway across the page: at alpha 120 it lifted
  the dark left by 17 levels and the lighter right by 2. Found by probing
  pixels, not by looking.
- The inspector's ninth property row (AUDIO) landed past the footer.

### Controller feedback

`evo_feedback(EVO_FB_…)` drives two channels at once — the existing
synthesised blips, and the lightbar tinted to the theme accent with a brief
flash on confirm. Callers name the event, not the effect.

Both are user-controllable under **Tools**. `evo_feedback_tick()` must be
called every frame so the confirm flash decays.

### Haptics were removed — do not rebuild them without reading this

Vibration was built, tested on hardware, and **taken out because it does not
work on this platform.** PS5 firmware 12.70, launched via `hbldr` into the
PS Now app slot. The probe that established it:

| Call | Result | Felt? |
|---|---|---|
| `scePadSetLightBar` | `0x00000000` | **yes** — colour changes |
| `scePadSetVibration` | `0x00000000` | no |
| `scePadSetPS4BcVibrationMode(pad, 1)` then vibrate | `0x00000000` | no |
| `scePadSetVibrationMode(pad, 1)` then vibrate | `0x00000000` | no |
| `scePadSetVibrationStrength(pad, 255)` | `0x80920001` invalid arg | — |
| `scePadSetVibrationForce` | `0x803b0003` not a ScePad code | no |

The lightbar succeeding **on the same handle** rules out a bad handle, a
privilege problem and the transport. Every vibration path either reports
success and produces nothing, or rejects the call. The likeliest explanation
is that `SceShellCore` owns the vibration channel for the foreground
application and a payload in this slot cannot take it.

One caveat worth knowing, because it wasted a cycle: an earlier round of "no
rumble" genuinely **was** our bug — the first patterns were 16 ms at 30/255,
about one frame and below the motors' spin-up time. Retuning to 35 ms and up
changed nothing, which is what moved the diagnosis to the platform.

If you do try again: check the console's own **Settings → Accessories →
Controller (General) → Vibration Intensity** first, then re-add a probe like
the one above — five entry points, distinguishable pulse counts, return codes
logged. It costs ten seconds and answers the question outright.

Because it produced nothing, keeping it would have meant a settings row that
lies, motor state to tick every frame, and a channel in the API that no
caller could rely on. `evo_feedback` is sound and lightbar only.

---

## 2. How to work on this

See [`tooling.md`](tooling.md) for the full set. The short version:

```bash
# build, install, launch, capture
docker compose run --rm ps5-dev bash -lc '
  EXTRA_CFLAGS="-DEVO_AUTOSHOT=6 -DEVO_START_SCREEN=1" ./scripts/build-evoplayer.sh
  ./scripts/install-homebrew.sh --name EVOPlayer output/elf/EVOPlayer.elf
  ./tools/launch.sh --timeout 12
  ./tools/shot.sh grab'
```

**Exit the app on the console before launching again.** The app slot stays
resident, so a second launch adds an instance rather than replacing one, and
stacking them has kernel-panicked the console. `tools/launch.sh` guards
against it but cannot close the previous instance for you — see
[`tooling.md`](tooling.md).

**Do not call `make` directly** — the build script supplies transitive link
dependencies the project Makefile does not list.

`-DEVO_START_SCREEN=n` boots straight into a screen: `0` launch, `1` browser,
`10` settings, `13` favorites, `14` about, `15` tools.

Then **measure**:

```bash
./tools/shot.sh probe 152 222        # colour at a framebuffer coordinate
./tools/shot.sh scan row 260         # pixel runs along a line
./tools/shot.sh crop 1296 222 560 315
```

Coordinates are 1080p, the same numbers that appear in `evo_metrics.h`.

Run `./tools/klog.sh` in another terminal while you work; it records
everything to an append-only log and survives payload restarts.

---

## 3. Things that will bite you

- **Colours are `0xAABBGGRR`.** A raw `0xFF00D7FF` is **yellow** on this
  framebuffer, not cyan. Build colours with `EVO_RGBA()` / `RR_BGRA()`.
- **Never hardcode a colour in a screen.** If a token is missing, either
  derive it (`mix()` in `evo_chrome.c` / `evo_widgets.c` blends two existing
  tokens) or add it to `evo_theme` and to all four built-ins.
- **Position text with `evo_text_y_centred()` / `evo_text_y_stacked()`**, not
  by offset. The glyph boxes carry leading the ink does not fill; positioning
  by box height is what put subtitles 11 px below their card in the old UI.
- **Two font systems exist.** The UI uses the `RR_FONT` atlas via the
  `evo_draw` vtable. There is also a legacy 5×7 `draw_char` renderer used by
  the player overlay. Editing the wrong one wastes a cycle.
- **The atlas does carry `/`, `_`, `.`, `:`, `-` and `+`.** An earlier version
  of this document said it did not, and the browser transliterated paths into
  `>` because of it. All 69 glyphs have real widths — verified against the
  metrics tables. Breadcrumbs show real paths now.
- **Anything that opens a media file is expensive.** Probing codecs and
  extracting a thumbnail both cost enough to stall the frame, so the browser
  debounces both behind `EVO_PROBE_SETTLE_FRAMES` and caches by path. Cover
  art on the launch shelf is resolved at **one per frame** for the same
  reason — eight at once was seconds of hitch.
- **Measure, don't eyeball.** Reading pixels out of a capture settled several
  questions this pass that zooming in did not.

---

## 4. Suggested next steps

1. **A real home icon.** The rail's HOME entry reuses the folder glyph because
   the set has no house. Add one to `tools/gen_icons.py`.
2. **Migrate the modal screens** — profile picker, resume prompt, media info,
   playback finished. They still draw their own chrome. The profile picker is
   an `evo_screen_list` with four rows.
3. **Larger cover art.** The cache is 80×80, which is why recent tiles show a
   crisp inset thumbnail rather than filling the tile. A 400×225 cache would
   let them be full-bleed posters like the reference.
4. **Persist the feedback settings.** Haptics, sound and lightbar reset to
   defaults on relaunch; they should go through `prospero_settings_save()`.
5. **Retire `selected`.** `main.c` still carries the old main-menu integer;
   the launch grid ignores it, but it is dead weight in the loop signature.

### Known unfinished business unrelated to the UI

- Launching from the console home tile is broken; use the curl command above.
- FFmpeg `full` decoder profile has never been built.
- `libSceVdecCore` export names unknown — no hardware decode.
- The 29-file test set in `/mnt/usb0/test_files_aud_vid/` has not had a full
  codec sweep.
