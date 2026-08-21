# RmlUi icon swap — handoff

Status as of 2026-08-21: candidates researched, fetched, and **approved by the
user** after visual review. Nothing in the repo has been changed yet — this
doc is everything a fresh session needs to actually do the swap.

Review artifact (side-by-side current vs. candidate, built during the
approval pass): https://claude.ai/code/artifact/aef382de-1181-4754-a382-94eba466643a

---

## Scope (decided)

- **RmlUi icons only** — `projects/evoplayer/assets/icons/*.png`, loaded by
  `EvoRenderInterface::LoadTexture` in
  [evo_rmlui_render.cpp:182-284](../projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp).
  It's a generic `stbi_load()` against a search path list — any real PNG with
  the right filename drops in with no code change.
- **Not in scope**: the legacy SDF icon set (`evo_icons.h` /
  `tools/gen_icons.py`), used by screens not yet migrated to RmlUi. That's a
  different pipeline (procedural, not file-loaded) and a separate, bigger job.
- **Controller button glyphs are in scope too** (user confirmed swapping
  these despite the style-mismatch caveat below — read it before touching them).
- `icon_logo.png` (EVO brand mark) and `icon_emby.png` (Emby trademark) are
  **excluded** — keep both as they are.
- `btn_play.png` / `btn_pause.png` are unreferenced anywhere in the current
  codebase (no `.rml` or C source points at them) — left unscoped, no
  candidate was sourced.

## Sources (decided, licenses checked)

- **Concept icons → [Lucide](https://lucide.dev)**, ISC license (confirmed by
  fetching `LICENSE` from the repo directly). No attribution required.
  Repo: `github.com/lucide-icons/lucide`, icons live at
  `icons/<name>.svg`, raw fetch: `https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg`.
- **Controller glyphs → Kenney "Input Prompts"**, CC0 1.0 (public domain,
  attribution appreciated not required). Official pack:
  `kenney.nl/assets/input-prompts`. Fetched via the community mirror
  `github.com/tanuki-billie/kenney-input-prompts` (verified its `LICENSE.txt`
  is Kenney's original, unmodified CC0 text) — files live under
  `addons/kenney_input_prompts/PlayStation Series/Default/`.

## Where the fetched files are staged

Already downloaded and sitting in the working tree (gitignored via
`output/uiview/`, so `git status` won't show them):

```
output/uiview/icon_candidates/lucide/   14 SVGs, named after the EVO icon they'd replace
output/uiview/icon_candidates/kenney/   7 PNGs (64x64), Kenney's original filenames
output/uiview/icon_candidates/review.html   local copy of the approved comparison page
```

If this directory is gone in the new session (temp cleanup, different
machine, etc.), everything is re-fetchable — see the mapping table below for
exact source names, and the two URL patterns above.

## Mapping table

### Concept icons (14) — Lucide SVG → recolor → replace PNG

| Current file | Lucide source | Note |
|---|---|---|
| `icon_folder.png` | `folder` | direct match |
| `icon_favorites.png` | `star` | direct match |
| `icon_recent_files.png` | `clock` | Lucide has no "history" glyph; clock matches the original SDF version too |
| `icon_settings.png` | `settings` | separate file from the `evo_icons.h` gear fixed earlier this session — this is the RmlUi settings-row icon |
| `icon_developer_tools.png` | `square-terminal` | reads closer to the current terminal-window motif than plain "terminal" |
| `icon_about_support.png` | `info` | direct match |
| `icon_chevron.png` | `chevron-right` | direct match |
| `icon_resume.png` | `circle-play` | used as the generic row icon for playable video files, not literally "resume" |
| `icon_aspect.png` | `proportions` | semantic pick for the aspect-ratio setting; `scan`/`ratio` are the alternates if this reads wrong |
| `icon_subtitles.png` | `captions` | Lucide has no "subtitles" glyph |
| `icon_palette.png` | `palette` | direct match |
| `icon_trash.png` | `trash-2` | two-line-lid variant, slightly more detail than plain `trash` |
| `icon_home.png` | `house` | Lucide renamed `home` → `house` |
| `icon_browse_usb.png` | `usb` | direct match |

### Controller glyphs (6) — Kenney PNG → resize/recompose → replace PNG

| Current file | Kenney source |
|---|---|
| `btn_cross.png` | `playstation_button_color_cross.png` |
| `btn_circle.png` | `playstation_button_color_circle.png` |
| `btn_triangle.png` | `playstation_button_color_triangle.png` |
| `btn_square.png` | `playstation_button_color_square.png` |
| `btn_dpad.png` | `playstation_dpad.png` |
| `btn_lstick.png` | `playstation_stick_l.png` |

(`playstation_stick_r.png` was also fetched as a bonus — there's no current
`btn_rstick.png` to replace, so it's unused unless a right-stick hint gets
added later.)

---

## Two open implementation decisions (not yet made — resolve before coding)

These came up during research and weren't part of the approval ask, so pick
an answer before implementing rather than guessing:

**1. Bake cyan, or wire up runtime tinting?**
Every current RmlUi icon PNG is alpha-only art baked with a *fixed* cyan RGB
(`0,205,255` — confirmed by sampling `icon_folder.png`'s pixel data). That
means today's icons don't actually follow the four-theme dynamic theming
engine (Midnight/Carbon/Ember/Aurora) — they're cyan regardless of theme.
The vendored RmlUi build supports `image-color` (confirmed present in
`ElementImage.cpp`) but nothing in the codebase uses it; the legacy SDF
renderer already retints its icons per-theme via `evo_icon_tinted()`
(`evo_draw.c`), so this is a real, pre-existing gap on the RmlUi side, not
something the swap introduces.
  - **(a) Match current behavior**: rasterize each Lucide SVG to a 72×72
    alpha-only PNG baked with the current theme's cyan, same convention as
    today. Zero C++/RCSS changes, but keeps the theming gap.
  - **(b) Fix it while we're here**: rasterize as white/plain-alpha, then set
    `image-color: <theme accent>` per element in `evo_rmlui_app.cpp`
    (mirroring how backgrounds/borders/text already get themed there).
    Bigger change, but makes icons theme-adaptive like everything else on
    the RmlUi side.

**2. Controller glyphs: composite onto the current "badge" look, or accept Kenney's flatter style?**
Current `btn_*.png` assets bake in a dark filled circle backdrop *and* the
glyph in one image (confirmed by pixel sampling — e.g. `btn_cross.png` is a
navy disc `(20,40,80,230)` behind a white cross outline in accurate DualSense
blue). Kenney's PNGs are just the flat glyph, no backdrop — Sony's own
"official" cutout style. Options:
  - **(a) Composite**: keep EVO's disc-badge treatment, drop Kenney's glyph
    shape into it (image work, not just a file copy).
  - **(b) Restyle**: move the disc/badge to RCSS (`.footer-glyph` etc.) as a
    background-color + border-radius wrapper around a plain glyph `<img>`,
    letting Kenney's flat PNGs be used unmodified. Touches `browser.rcss`
    and every other screen's footer-hint markup, but is more maintainable
    than baking backdrops into bitmaps.
  - **(c) Skip for now**: given decision cost is higher than the concept
    icons and the current badges already look accurate and polished, do only
    the 14 concept icons this pass and revisit controller glyphs separately.

The review artifact linked above shows the raw Kenney PNGs next to the
current badges "as is" (no compositing attempted) — the style gap is visible
there, which is what prompted flagging this rather than just picking one.

---

## How to verify once implemented

Same loop used earlier this session for the gear-icon and browser-footer
fixes — no hardware needed:

```bash
docker compose run --rm ps5-dev bash ./tools/uiview_playback_rml.sh
```

Then convert the relevant `.bmp` outputs in `output/uiview/` to PNG at full
res for a close look (the half-res `.png` siblings the script writes are
fine for a quick check, but were too small to judge the gear-icon issue
earlier this session):

```python
from PIL import Image
Image.open('output/uiview/rml_browser.bmp').convert('RGB').save('output/uiview/rml_browser_full.png')
```

`docs/tooling.md` has the full `uiview.sh`/`shot.sh` reference if a hardware
screenshot ends up being warranted.
