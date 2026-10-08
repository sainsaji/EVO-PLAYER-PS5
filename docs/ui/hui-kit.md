# EVO's UI on ps5-homebrew-ui

EVO's own screens are drawn with the **ps5-homebrew-ui** kit
(`third_party/ps5-homebrew-ui`, GPL-3.0-or-later like EVO). RmlUi is still
linked, but only for **provider screens** (their UI is fetched from the network
at run time, see [provider-architecture.md](../addons/provider-architecture.md))
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

Home, list (Recent / Favorites / audio tracks), file browser (with file
operations and transfer progress), settings, player OSD (+ stats HUD, music
view), subtitle picker, media info, about, changelog, safe-to-close, exit
dialog and toasts. The navigation rail is drawn over every menu screen.
Still RmlUi: Surround Studio, text reader, image viewer, virtual keyboard (being
ported), provider screens.

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
```

Add a fixture in `tools/hui_preview/hui_preview.cpp` for a new screen or state.
Layout questions are answered there; only run the console for behaviour.
Hardware loop: [tooling.md](../build/tooling.md#hardware-cycle).

## Adding a screen

1. A class in `ui_kit/` with `set(params)`, `enter()`, `update(dt)`,
   `draw(list, ctx) const`, `focus(FocusInfo*) const` (copy the strings in
   `set()`; the caller's pointers die).
2. `evo_hui_update_<x>` / `evo_hui_render_<x>` in `evo_hui_app.cpp` and `evo_hui.h`.
3. Call them first in the matching `evo_rmlui_update_/render_<x>` in
   `evo_rmlui_bridge.cpp` (see `evo_rmlui_render_launch`).
4. Add the source to `UI_KIT_SRCS` in the Makefile and `tools/hui_preview.sh`.
