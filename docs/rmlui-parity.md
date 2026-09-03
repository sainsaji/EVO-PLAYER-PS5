# RmlUi per-screen parity checklist (#44)

Tracks each migrated screen against the pre-migration `main` (immediate-mode
SDF) baseline, folds in #16 (clipped text / overlapping labels), and records
the deliberate deviations. This is the gate for **#28** (GPU Step 3 needs a
stable RmlUi target) and for deleting the legacy screen code (`ui/src/evo_screens.c`,
`ui/src/evo_chrome.c`, the shadowed `draw_*_screen` tails in `main.c`).

## How this was checked

- **RmlUi renders:** `tools/uiview_playback_rml.sh` — the host harness now
  covers every screen (launch, list, browser, changelog, reader, surround, the
  three dialogs, media info, subtitle picker, settings ×9, themes ×3, playback
  OSD idle/paused/scrubbing/stats) plus a **`*_stress` fixture per #16 screen**
  with deliberately over-long strings. Output: `output/uiview/rml_*.png`.
- **Legacy baseline:** `tools/uiview.sh --all` → `output/uiview/*.png`.
- **Sign-off basis:** side-by-side of the host render pairs. Items that can
  only be confirmed on a console (theme switch repaint on device, OSD/dialog
  compositing over live decoded video, marquee smoothness at real frame
  cadence, D-pad nav timing) are marked **HW** and listed in
  [validation.md](validation.md) under "UI parity (#44)".

Status legend: **OK** = at parity on the host renders · **OK\*** = at parity,
with a deliberate deviation noted · **HW** = needs a hardware pass.

## Screens

| Screen | `evo_rmlui_render_*` | Status | Notes |
|---|---|---|---|
| Launch / home | `launch` | OK\* / HW | Hero + JUMP BACK IN shelf + LIBRARY shelf all present and positioned as `main`. **Deviation:** the collapsed nav rail on the left is an RmlUi-era addition (rmlui-integration-guide §6), absent from `main`. #16: `#hero-title` / `#hero-detail` now `nowrap` + ellipsis so a long title cannot overrun `#hero-foot`. |
| USB browser + inspector | `browser` | OK / HW | Two-pane layout, 12-row virtualised list, breadcrumb, live inspector — matches `main`. #16: `.ins-val` bounded (320px) + right-aligned + ellipsis; `.ins-key` `flex-shrink:0`; `#ins-name` clamped. Long filenames ellipsise in both the row and the inspector. |
| Recent / Favorites / Emby setup / Emby browse | `list` | OK / HW | One shared `list` document, geometry follows `settings.rcss` / `evo_metrics.h`. Row title + detail already `nowrap`+ellipsis. Empty states present. |
| Settings (main, Playback, Subtitles, Interface, System, Dev tools, Profile pick, Theme pick, About) | `settings` | OK / HW | One shared `settings` document. #16: `.row-text-box` `max-width:940px`, `.row-title`/`.row-detail` `nowrap`+ellipsis, `.row-right` `flex-shrink:0` + `max-width:600px`, `.row-badge` ellipsis — a long value badge (profile names) can no longer collide with the row title. |
| Surround sound studio | `surround` | OK / HW | Spatial room diagram with 5.1/7.1 speaker nodes + calibration monitor panel — matches `evo_screen_surround_test`. No dynamic-string overflow risk. |
| Changelog (master + detail) | `changelog` | OK / HW | Master release list + detail pane, NEW/FIXED/IMPROVED item badges. Master `.clrel-tag` already clamped. #16: `#cldetail-tag` now `nowrap`+ellipsis + bounded width, matching the master column; the old `inset_w - tag_w - 300` hardcoded offset is gone. |
| Text reader | `reader` | OK / HW | Single scrolling pane over a fixed line pool, scrollbar, footnote. Lines are pre-wrapped by `evo_textreader.c` (unchanged). |
| Media Info | `mediainfo` | OK\* / HW | **Deviation (intentional, rmlui-integration-guide §7.3):** redesigned from `main`'s flat two-column table into a four-card "AV diagnostics deck" (Container/Video/Audio/Subtitles+Engine). Same information, new layout. #16: `#mediainfo-title` / `#mediainfo-path` `nowrap`+ellipsis + `#header-text-block` `max-width:980px`; `.spec-val` bounded (380px) + right-aligned + ellipsis, `.spec-key` `flex-shrink:0`. Long codec strings ("HEVC (H.265 Main 10, Level 5.1)", "TrueHD 7.1 + Atmos") truncate instead of colliding with the key. |
| Subtitle track picker | `subtitles` | OK / HW | Track list over live video, size cycle, preview line. Long track labels fit. |
| Resume prompt / Playback finished / Exit confirm | `dialog` | OK\* / HW | One shared `dialog` document. **Deviation:** `main`'s resume modal showed a poster thumbnail on the left; the RmlUi dialog is text-only (eyebrow / title / detail / progress / actions). #16: `#dialog-actions-row` is now `flex-wrap: wrap` inside a fixed 824px width, and `#dialog-card` has `overflow:hidden` — a third action with a long label wraps to a second row instead of overflowing the card. Multi-line long titles wrap within the card (as `main` did). |
| Playback OSD | `playback_osd` | OK / HW | Top scrim (title + meta + badge rack), centre scrub/pause capsule, bottom transport bar, stats-for-nerds HUD. #16 + **marquee (#44):** `#title-block` is now a fixed 1180px `overflow:hidden` clip window; a long title is clipped clear of the badge rack, and `MarqueeTick` (`evo_rmlui_app.cpp`) ping-pong-scrolls `#media-title` inside it — dwell at each end, 85 px/s, matching the legacy `evo_text_marquee`. `#media-meta` is `nowrap`+ellipsis. The legacy `strlen(title) > 55` / `> 76` pre-truncation in `main.c` was removed (it hid the tail the marquee reveals). |
| IME keyboard modal | (`evo_keyboard.c`, shared) | OK / HW | Not an RmlUi screen — the virtual keyboard modal (`evo_screen_keyboard`) is drawn by the kept `ui/src/evo_keyboard.c` on both paths. See #34. |
| Toast | (`evo_widgets.c`, shared) | OK | `evo_widget_toast` via `src/evo_toast.c`, drawn every frame on both paths. `evo_widgets.c` stays. |
| Image viewer | (`main.c` `draw_image_screen`) | OK | No RmlUi equivalent; stays immediate-mode in `main.c`. Out of #44 scope. |

## Marquee scope decision (#44)

Long focused text scrolls **only on the playback OSD title**. The OSD document
is not in the Step-1 surface cache, so the per-frame re-render the marquee
relies on is already happening. Marqueeing a cached menu row (RECENT,
FAVORITES, browser list) would force a full surface re-raster every frame while
the cursor sits on that row — ~60fps → ~11fps on that screen for as long as the
cursor stays there. The menu/list rows therefore keep the static
`text-overflow: ellipsis` they already carry. If #28 replaces the CPU
rasteriser with the sceAgc path (cheap re-render), revisit and extend the
marquee to the menu rows.

## #16 — items resolved

All fixes are RCSS in `assets/rml/*.rcss` plus the OSD marquee and the removal
of the `main.c` OSD title pre-truncation. The hardcoded right-edge offsets #16
named (`panel_w - 580 / - 360`, `inset_w - tag_w - 300`, dialog `w = 1120`)
were legacy immediate-mode and no longer exist in the RmlUi assets — this pass
clamped the remaining unbounded dynamic strings:

- Player OSD title/meta vs badge rack — fixed (clip window + marquee).
- Changelog detail tagline vs stats badge — fixed (`#cldetail-tag` clamp).
- Modal multi-action overflow — fixed (`flex-wrap` + card `overflow:hidden`).
- USB browser inspector columns — fixed (`.ins-val` / `.ins-key`).
- Media Info header path + spec rows — fixed (`.spec-val` / `.spec-key`, header clamp).
- Settings row title vs value badge — fixed (`.row-text-box` / `.row-right` / `.row-badge`).
- Launch hero title — fixed (`#hero-title` clamp).

## Still owed (PR 2 / hardware)

- A hardware pass of the **HW**-marked rows: theme switch repaint on device,
  OSD + dialog + media-info + subtitle-picker compositing over live decoded
  video, marquee smoothness at real cadence, D-pad focus/nav order and timing.
  Log results in [validation.md](validation.md).
- Once every row above is OK / OK\* and the hardware pass is clean: delete the
  legacy screen code (see the #44 plan / roadmap — `ui/src/evo_screens.c`,
  `ui/src/evo_chrome.c`, the `draw_*_screen` legacy tails, `Makefile` `UI_SRCS`,
  `tools/uiview.c` → RmlUi harness).
