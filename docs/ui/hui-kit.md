# EVO's UI on ps5-homebrew-ui

EVO's own screens are drawn with the **ps5-homebrew-ui** kit
(`third_party/ps5-homebrew-ui`, a git submodule, GPL-3.0-or-later like EVO; its changes
are separate commits from EVO's). RmlUi is still linked ([rmlui.md](rmlui.md)), but only
for the data binding and focus of **provider screens**, for provider UI bundles that are
fetched from the network at run time ([../providers/architecture.md](../providers/architecture.md)),
and as the fallback for any screen the kit cannot draw.

## How it fits

```
core/screens/*.cpp  ──►  evo_rmlui_update_<screen>(params)      (unchanged callers)
                         evo_rmlui_render_<screen>(...)
                              │
        ui_rml/src/evo_rmlui_bridge.cpp   tries the kit first
                              │  evo_hui_render_<screen>() == 1  → kit drew it
                              ▼
        ui_kit/src/evo_hui_app.cpp        owns fonts, the AGC batch, screen objects
                              │ hui::gfx::DrawList
                              ▼
        ui_kit/src/hui_agc_batch.cpp      DrawList → sceAgc, shader ui_sdf.pipe
```

- The kit's renderer is OpenGL-only (`gfx/gl_batch.cpp`). EVO has no OpenGL, so
  `hui_agc_batch.cpp` draws the same `DrawList` through the AGC runtime with the
  kit's own SDF shader ported to LLPC (`shaders/agc/ui_sdf.pipe`). Each
  `hui::gfx::Instance` is written six times (96-byte vertices) and drawn with
  identity indices: the pipe takes the quad corner from `gl_VertexIndex % 6`.
- HDR10 output: `ui_sdf_pq_out.pipe` is generated from `ui_sdf.pipe` by
  `tools/gen_hdr_ui_pipe.py` (same as `ui_screen_2d_pq_out.pipe`) and is swapped
  in by `agc_hdr_remap` while the scanout is HDR10.
- The kit compiles as C++20 under `ui_kit/obj/hui/` (its GL, platform and music
  files are left out). Fonts are the kit's baked SDF fonts in `assets/hui/fonts`
  (embedded by `tools/bundle_rml_assets.py`).
- **Fallback:** `evo_hui_render_*` returns 0 and the RmlUi screen draws if the
  kit failed to start, if `/mnt/usb0/evo_no_hui` exists, or (OSD only) if a
  caption holds a character the kit's fonts lack. Subtitles in other scripts
  still show, through RmlUi's Noto fonts.

## Screens on the kit

Everything EVO draws itself: Home, the list screens (Recent, Favourites, the audio,
stream and chapter pickers, with pills and a details card, and a busy card while a
switch runs on a worker), the file browser (file operations and transfer progress),
Settings (with a live preview card for every setting, #158), the player OSD (seek
bar, stats HUD, music view, seek preview), subtitle picker, media info, about,
changelog, safe-to-close, dialogs, toasts, the text reader, the image viewer,
Surround Studio, and every provider screen (shelves, details and cast, skeleton cards
while loading). The navigation rail is drawn over every menu screen. The virtual
keyboard draws its keys with the kit `Keyboard` component, in the native layout with
Shift and Caps Lock state and a clear button; EVO keeps focus and text (#157, #138).
Still RmlUi: downloaded provider bundles and the fallback skin.

Screens only *draw*: focus and input stay in `core/`. Each screen animates
towards the state in its params (springs, staggered entrances) and reports its
focused item for `evo-remote.sh ui` (`evo_hui_dev_focus_json`).

## Sounds

`SoundEffectEngine` plays the kit's recorded "glass" cues (`assets/hui/sfx`,
mono 48 kHz WAV) for move / confirm / back / toggle, falling back to the old
synthesised tones if a sample is missing. The existing **Navigation sounds**
setting switches them.

## Working on it

```
./tools/hui_preview.sh        # every screen -> output/uiview/hui_*.png (no console)
./tools/hui_preview.sh 1280 720           # optional size (default 1920 1080)
```

Add a fixture in `tools/hui_preview/hui_preview.cpp` for a new screen or state. The provider screens have fixtures for each home: `shelves` (Jellyfin), `shelves_stremio`, `shelves_plex`, `shelves_iptv`, `shelves_xtream` (poster, channel and category shelves side by side) and `livetv`/`xtream`/`provider_folder` for the See all levels; see [../providers/architecture.md](../providers/architecture.md#home-layout-shelves-for-every-provider).
The movie / show detail page (#221) has `detail_overview`, `detail_cast`, `detail_details`, `detail_show` and `detail_loading`.
It is a mode of `ProviderGridScreen` (`params.detail.open`), not a screen of its own: Cross on a movie or show opens it
(`EvoRmlProviderHost::OpenDetail`, for items the provider marks `evo_provider_item_t::detail`), the host keeps the zone and tab,
and Play / Restart queue the same selection a card press does, with `skip_prompt` so the Continue / Restart / Cancel prompt is not asked twice.
Layout questions are answered there; only run the console for behaviour.
Hardware loop: [../console/hardware-cycle.md](../console/hardware-cycle.md).

## Adding a screen

1. A class in `ui_kit/` with `set(params)`, `enter()`, `update(dt)`,
   `draw(list, ctx) const`, `focus(FocusInfo*) const` (copy the strings in
   `set()`; the caller's pointers die).
2. `evo_hui_update_<x>` / `evo_hui_render_<x>` in `evo_hui_app.cpp` and `evo_hui.h`.
3. Call them first in the matching `evo_rmlui_update_/render_<x>` in
   `evo_rmlui_bridge.cpp` (see `evo_rmlui_render_launch`).
4. Add the source to `UI_KIT_SRCS` in the Makefile and `tools/hui_preview.sh`.

## Text in every script (#128)

Kit text goes: baked ASCII atlases, then `noto-fallback.huifont` (Latin ext, Greek, Cyrillic, Hebrew, Arabic with joining and bidi order in `src/gfx/font.cpp`, symbols; baked by `tools/font-baker/bake_fallback.cpp`), then `ComplexText` (`projects/evoplayer/ui_kit/src/evo_hui_complex.cpp`: HarfBuzz shapes, FreeType draws SDF glyphs into a growing 2048 atlas; Thai and Indic faces are embedded, Chinese/Japanese/Korean load from `/data/evoplayer/fonts/NotoSans{SC,JP,KR}-Regular.otf` when present). `tools/hui_preview.sh` has no HarfBuzz, so Thai/Indic/CJK only show on the console. Subtitle sidecars are converted to UTF-8 by `media/src/evo_charset.c`; track pickers show native language names (`evo_langname.c`).
