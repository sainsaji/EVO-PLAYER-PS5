# EVO Player UI — state of play

Written at the end of the theming pass (v0.0.2) so the next session can start
without re-deriving anything. Read this first, then
[`docs/theming.md`](theming.md) for the theme format.

---

## 1. Where things stand

The UI overhaul was requested as: *"The ui currently looks like its made by a
child. I want a professional UI. Proper cards. I'd start with a theme setup.
Plug and play theme support."*

The theming half is **done and verified on hardware** (PS5 at `192.168.0.10`,
firmware 12.70). Nothing on screen is off-theme any more — background, cards,
typography, icons and controller prompts all read from one struct.

### What was built

| Piece | File |
|---|---|
| Theme tokens + built-ins + `.theme` loader | `projects/evoplayer/pp/src/evo_theme.c`, `pp/include/evo_theme.h` |
| SDF drawing primitives (card, round rect, circle, hairlines, background) | `pp/src/evo_ui.c`, `pp/include/evo_ui.h` |
| Icon + controller-prompt generator | `tools/gen_icons.py` → `projects/evoplayer/assets/evo_icons.h` |
| Sample drop-in themes | `themes/*.theme` |
| Docs | `docs/theming.md` |

Four built-in themes (`MIDNIGHT` default, `CARBON`, `EMBER`, `AURORA`) plus
anything dropped in `/mnt/usb0/evo_themes/`. Switch in **Settings → THEME**.
The choice persists **by name**, not index — a USB theme appearing or
disappearing between boots would otherwise silently select a different theme.

### Screens converted to themed cards

- Main menu — 6 rows, full width (`180 … 1740`), 96px cards on a 112px pitch
- Settings — 8 rows, 84px cards on a 94px pitch (denser, to clear the footer)
- Playback profile — 4 rows
- USB browser — 6 rows, 1220px wide (stops short of the preview panel at
  x=1440), themed preview panel
- Recent files, Favorites — same metrics as the browser

### Defects fixed along the way

- Settings navigation wrapped at `% 6` while the row table had grown to 7,
  so **REMOVE HOME TILE was unreachable**. Now derives from
  `EVO_SETTINGS_COUNT`.
- Subtitle text sat 11px **below the card bottom edge** (title `+25` / sub
  `+70` in an 82px card). Fixed against measured glyph ink bounds.
- Three separate hardcoded version literals, two still saying `1.0`. Now one
  `-DEVO_PLAYER_VERSION` injected by the Makefile from
  `projects/evoplayer/VERSION`.
- Icons and controller prompts stayed cyan under every theme. Icons are
  monochrome so they are now alpha-tinted (`rr_img_tint`); the controller
  prompts were two-tone (103 distinct RGBs) so they were regenerated as
  single-hue SDF glyphs.
- A 14-dot "drifting particle" overlay that read as stuck pixels — removed.
  I chased one of these as a rendering bug before finding the loop drawing it.
- `.theme` comment scanner formed a pointer before the start of its buffer.

---

## 2. How to work on this

Everything runs in the pinned container. **Do not call `make` directly** — the
build script supplies transitive link dependencies the project Makefile does
not list, and a bare `make` fails to link.

```bash
# build → install → launch → grab a screenshot, in one go
docker compose run --rm ps5-dev bash -lc '
  EXTRA_CFLAGS="-DEVO_AUTOSHOT=4" bash ./scripts/build-evoplayer.sh
  bash ./scripts/install-homebrew.sh --name EVOPlayer output/elf/EVOPlayer.elf
  curl -sS --max-time 12 --get \
    --data-urlencode "path=/data/homebrew/EVOPlayer/eboot.elf" \
    --data-urlencode "pipe=1" "http://$PS5_HOST:8080/hbldr" >/dev/null 2>&1
  sleep 3; bash tools/fetch_shot.sh'
```

Then open `output/screenshots/latest.png`.

### Development switches (`EXTRA_CFLAGS`, empty in shipping builds)

| Flag | Effect |
|---|---|
| `-DEVO_AUTOSHOT=N` | Auto-capture a BMP to `/mnt/usb0/` N seconds after launch |
| `-DEVO_START_SCREEN=n` | Boot straight into a screen: `1` browser, `10` settings, `11` profile |

`tools/fetch_shot.sh` pulls the newest `evo_shot_*.bmp` over HTTP and converts
it to `latest.png` at half size. **The raw BMPs are 6MB each and accumulate on
the stick** — worth clearing occasionally.

### Regenerating icons

```bash
python3 tools/gen_icons.py     # writes evo_icons.h + a contact sheet
```

Icons are described as vector shapes and rasterised from signed distance
fields, so edges carry analytic coverage. Append to `ICONS` as
`(macro_prefix, shape_fn, size)` and add a `case` in `rr_icon()`.

### Testing the theme parser on the host

```bash
docker compose run --rm ps5-dev bash -lc '
  clang -Wall -Wextra -fsanitize=address,undefined \
    -DEVO_THEME_DIR=\"evo_themes\" \
    -I/workspace/projects/evoplayer/pp/include \
    your_test.c /workspace/projects/evoplayer/pp/src/evo_theme.c -o t && ./t'
```

`EVO_THEME_DIR` is overridable precisely so this works. Last run: 7 themes
parsed, clean under ASan/UBSan, per-key inheritance and by-name lookup both
correct.

---

## 3. Things that will bite you

- **Colours are `0xAABBGGRR`.** A raw `0xFF00D7FF` is **yellow** on this
  framebuffer, not cyan. Build colours with `EVO_RGBA()` / `RR_BGRA()`.
- **Two font systems exist.** The main UI uses the `RR_FONT` atlas via
  `rr_text(fb, x, y, s, colour, face)` with faces `3` title / `2` menu /
  `1` sub / `0` small. There is also a legacy 5×7 `draw_char` renderer used
  elsewhere. Editing the wrong one wastes a cycle — I did exactly that.
- **Glyph ink bounds, measured** (rows within each glyph box, includes
  descenders): face 3 `11..52` of 57, face 2 `9..41` of 46, face 1 `7..23`
  of 29, face 0 `6..20` of 25. Position text against these, not by eye.
- **The atlas has no `/` or `_`.** `rr_idx()` returns −1 and advances 12px, so
  paths render with gaps: `test_files_aud_vid` shows as `test files aud vid`.
- **Never trust an unverified string substitution.** Several silently matched
  nothing during this project. Use the Edit tool or assert the match count.
- **Measure, don't eyeball.** Reading framebuffer pixels out of the BMP
  settled several questions that zooming in did not — including proving that
  "jagged icons" were baked into the artwork rather than a renderer bug.

---

## 4. Suggested next steps

Design references the user dropped in `samples/` (Samsung Tizen home, an Xbox
dashboard, an Android file browser) point at a **left navigation rail +
content area** layout rather than the current full-width row stack:

- `side_nav.jpg`, `Side_Nav2.jpg` — persistent icon rail down the left edge,
  content to the right
- `file_browser.jpg` — labelled nav rail (Device / Local Network / Favorite /
  History / Settings), file list centre, video preview bottom-left
- `Launch.jpg` — horizontal poster shelves with section headings

> These are third-party product screenshots, so they are **deliberately not
> committed** — see `.gitignore`. They are local reference only.

Concrete work that follows from that:

1. **Navigation rail.** `evo_theme` already carries a `rail_px` metric and the
   drawing primitives handle it; the screens are the work.
2. **Poster/grid layout** for the browser, instead of one file per row. The
   thumbnail decoder already exists (`prospero_browser_preview_*`).
3. **Focus animation.** Selection currently glides vertically; a subtle scale
   or elevation change on the focused card would read as more modern.
4. **Theme previews in the settings row** — swatches of the accent and surface
   colours next to the theme name, so cycling is not blind.
5. **A real `icon0.png`.** Currently a 1×1 placeholder.

### Known unfinished business unrelated to the UI

- Launching from the console home tile is broken; use the curl command above.
- FFmpeg `full` decoder profile has never been built.
- `libSceVdecCore` export names unknown — no hardware decode.
- The 29-file test set in `/mnt/usb0/test_files_aud_vid/` has not had a full
  codec sweep.
