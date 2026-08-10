# EVO Player UI — state of play

Written so the next session can start without re-deriving anything. Read this
first, then [`theming.md`](theming.md) for the theme format and
[`tooling.md`](tooling.md) for how to build, capture and measure.

---

## 1. Where things stand

Three passes have landed.

**v0.0.2 — theming.** Every colour and metric moved into one struct
(`evo_theme`), drawing moved onto SDF primitives (`evo_ui`), and icons became
monochrome so they could be tinted. Verified on hardware.

**v0.1.0 — architecture.** The theming pass left the player *looking*
consistent but not *being* consistent: each screen still drew its own header,
footer and cards at its own coordinates, and navigation was two long
`if (screen == …)` chains. That is now a real UI layer under
`projects/evoplayer/ui/`.

**v0.3.0 — the console reaches it.** The player registers a home-screen Media
tile and starts from the controller, so the UI is finally reachable without a
browser on a second device — see [`media-tile.md`](media-tile.md). The UI-side
additions are the changelog screen and a generated application icon.

**Unreleased — posters, a logo, and a prompt before you lose your place.**
Four changes, all verified on the host renderer rather than on hardware:
cover art large enough to fill a tile, the application mark replaced with the
real logo, the feedback settings actually persisted, and CIRCLE during
playback asking before it stops. Details are in `CHANGELOG.md`; what matters
for the next session is in §1 and §4 below.

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
- **Changelog** (screen 19) — release notes, reached with CROSS on About's
  last row and registered as a child of `EVO_SECTION_ABOUT`, so the rail stays
  lit on About the way the profile picker keeps SETTINGS lit. Another
  `evo_screen_list` model; the data is a table in
  `projects/evoplayer/evo_changelog.h`.

  Two things about that table. It is **product content, so it lives beside
  `main.c`, not in `ui/`** — nothing under `ui/` should know what shipped in
  0.2.0. And every string in it is written to the font atlas's alphabet, which
  is not optional; see §3.

  `tools/uiview.c` renders it from the *real* table rather than a fixture, so a
  host render cannot disagree with the console. Fixtures that drift are how
  the About render came to show version 0.0.2 for three releases.
- **Stop playback** (screen 20) — CIRCLE during playback opens this rather
  than tearing the file down. An `evo_screen_dialog()` model drawn over
  `draw_player_screen()`, the way the subtitle picker is.

  Two things are load bearing. **CIRCLE dismisses it and CROSS confirms**,
  which inverts the usual reading of the two buttons on purpose: CIRCLE is
  what opened the prompt, so a second press must not be what destroys the
  session. And it goes through `pp_product_overlay_enter()` /
  `_leave()` — leaving `screen` off `SCREEN_PLAYER` stalls the decode thread
  but *not* the presentation clock, so without holding the clock every frame
  on the way back looks late and gets dropped. That is the 0.1.3 Media Info
  freeze exactly; those two helpers exist so there is one copy of the fix and
  not two.

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
`10` settings, `13` favorites, `14` about, `15` tools, `19` changelog.

**The player is installed twice, and one command updates both:**

```bash
./scripts/update-console.sh          # rebuild, update tile AND homebrew
```

`/data/homebrew/EVOPlayer` is the websrv install; `/data/evoplayer/app` is the
tile's own copy, which the launcher **embeds** and rewrites from that embedded
copy on every launch. So `install-homebrew.sh` alone does not update the tile —
you rebuild, press the tile, and get the previous build with nothing on screen
to say why. That is why the script defaults to both.

Console deploys are time-bounded. `prospero-deploy` is `socat -t 9999999` and
elfldr does not close while the payload it spawned is alive, so deploying the
resident launcher never returns; an unbounded install ran until it was killed.
Exit 124 from `timeout` means *still resident, detached*, which is success.

For the UI specifically, prefer the host: `tools/uiview.sh <screen>` renders
any screen to a PNG with the real drawing code, real atlas and real icons.
Application artwork is generated too — `tools/gen_app_icon.py` writes the
512×512 `icon0.png` the Media tile uses, from vector shapes.

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
- **The atlas is 69 glyphs, and that is the whole alphabet you have.**
  `assets/renderer_reset_assets.h` defines it exactly:

  ```
  RR_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /._:-+"
  ```

  Both cases, digits, space, and six punctuation marks. **No comma, no
  parenthesis, no apostrophe, no question mark** — and an unknown glyph leaves
  a *hole* rather than being skipped, so prose written without checking comes
  out as a row of gaps. `EVO_TEXT_CHARSET` and `evo_text_unsupported()` in
  `evo_draw.h` are the check; use them rather than eyeballing.

  Earlier revisions of this document, and the header comment in
  `evo_changelog.h`, say 43 glyphs and no lower case. That was the *legacy*
  `PP_CHARS` set, and it is not what the UI draws through — the browser and
  the launch shelves have been rendering lower-case filenames all along. The
  changelog entries are upper case as a house style, not because they have
  to be. **The one thing the shorter set was right about is the punctuation:**
  no `?`, so a confirmation prompt cannot ask a question mark's worth of
  question. The stop-playback prompt says `STOP PLAYBACK` and lets the two
  buttons be the answer.

  An earlier version of this document claimed the atlas lacked `/ _ . : - +`,
  and the browser transliterated real paths into `>` because of it. It does
  carry them; every glyph has a real width in the metrics tables. Breadcrumbs
  show real paths now.
- **Anything that opens a media file is expensive.** Probing codecs and
  extracting a thumbnail both cost enough to stall the frame, so the browser
  debounces both behind `EVO_PROBE_SETTLE_FRAMES` and caches by path. Cover
  art on the launch shelf is resolved at **one per frame** for the same
  reason — eight at once was seconds of hitch. Note what that budget is
  protecting: the cost is opening the file and decoding a frame, not scaling
  the result, which is why raising the cache from 80×80 to 320×180 did not
  change the pacing. Raising the *count* would.
- **A modal drawn over playback must hold the presentation clock.** Leaving
  `screen` off `SCREEN_PLAYER` stalls the decode thread but not the clock, so
  on the way back every arriving frame is judged late by however long the
  overlay was open, and all of them are dropped — a frozen picture with audio
  still playing. `pp_product_overlay_enter()` / `_leave()` do this and the 4K
  surface hand-off together; call them rather than writing it out again. The
  subtitle picker still does not, which is a known gap (§4).
- **Measure, don't eyeball.** Reading pixels out of a capture settled several
  questions this pass that zooming in did not.

---

## 4. Suggested next steps

[`backlog.md`](backlog.md) is the ranked list across the whole project and
decides order. What follows is the UI-local detail behind its entries — keep
them in step.

1. **Put the whole pass on hardware.** Everything in the unreleased section
   was verified with `tools/uiplay.sh` and a clean `build-evoplayer.sh`, and
   nothing in it has been on a console. Three things can only be answered
   there:
   - whether the larger cover cache still fills the shelf without a hitch —
     the argument that it should (§3) is reasoning, not a measurement;
   - whether the stop prompt's 4K round trip really comes back at 4K.
     `pp_product_overlay_enter()` / `_leave()` are Media Info's own code, so
     it should behave as Media Info does, but Media Info's behaviour here has
     itself only ever been checked by hand;
   - whether the settings file written by this build is still read by one
     written before it, and the reverse.
2. **Give the subtitle picker the overlay helpers.** It is drawn over
   playback exactly as the stop prompt is, and it holds neither the clock nor
   the 4K surface — see §3. It has shipped since 0.2.0 without a report, so
   this is a latent risk rather than a known break, and the fix is two calls.
3. **Retire `selected`.** `main.c` still carries the old main-menu integer and
   still passes it to `draw_menu_linear()`. The launch grid ignores it, but it
   is dead weight in the loop signature.
4. **Retire `prospero_cover_blit()`.** Dead since the shelf stopped drawing
   inset thumbnails — nothing calls it — and it now describes an 80×80 world
   that no longer exists, with a hardcoded cyan frame the theming rules
   forbid. Left in place only because this pass had no reason to touch it.

### Done since this document last said otherwise

Verified in the source rather than assumed, because two of these sat here as
open work after they had already shipped:

- **The home icon exists.** `tools/gen_icons.py` has `icon_home()`, it is
  emitted as `EVO_ICON_HOME`, and the rail's HOME entry uses `EVO_IC_HOME`.
- **All four modal screens are migrated.** Resume prompt and playback finished
  go through `evo_screen_dialog()`, media info through `evo_screen_info()`,
  and the profile picker is an `evo_list_model`. None of them draws its own
  chrome any more.
- **Cover art is large, and the feedback settings persist.** Both were items
  1 and 2 here; they are in the unreleased section of `CHANGELOG.md` now.

### Known unfinished business unrelated to the UI

- FFmpeg `full` decoder profile has never been built.
- `libSceVdecCore` export names unknown — no hardware decode.
- The 29-file test set in `/mnt/usb0/test_files_aud_vid/` has not had a full
  codec sweep. This is the top item in `backlog.md`.
