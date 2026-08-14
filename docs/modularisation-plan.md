# main.c Modularisation Plan

> **Status:** Planning  
> **Target:** Split `main.c` (18,437 lines, ~660 function definitions) into focused modules  
> **Constraint:** No behaviour changes. Each extraction must produce a build-clean result before the next one starts.

---

## Why now

`main.c` is the only file the Makefile tracks. That means:
- Any header change recompiles 530 KB of C
- No module can be tested in isolation
- The linker sees one translation unit — no dead-code elimination across logical sections
- Merge conflicts are near-guaranteed as the file grows

---

## Discovered Sections (from marker scan)

The file already has named regions via `/* PROSPERO_*_START */` / `/* PROSPERO_*_END */` markers and `/* ==== */` dividers. These are the natural extraction seams:

| Lines | Marker / Region | Proposed Module |
|---|---|---|
| 1–100 | Includes, audio extern decls | `main.c` (keep) |
| 100–260 | Screen constants, `PP_BACKEND` globals, `PlaybackProfile` | `evo_state.h` / `evo_state.c` |
| 258–267 | `PROSPERO_SETTINGS_EARLY_GLOBALS` | → settings module |
| 600–724 | Recent files DB | `evo_recent.h` / `evo_recent.c` |
| 617–730 | Favorites DB | `evo_favorites.h` / `evo_favorites.c` |
| 724–1430 | Cover art cache, thumbnail extraction, browser preview | `evo_cover.h` / `evo_cover.c` |
| 1428–1815 | Chapter management, compat report, media metadata | `evo_media_meta.h` / `evo_media_meta.c` |
| 1815–2173 | Global playback state, video/audio buffers | `evo_state.h` / `evo_state.c` |
| 1928–2173 | `PROSPERO_AUDIO_RESAMPLER` | `evo_audio_resample.h` / `evo_audio_resample.c` |
| 2400–3640 | Subtitle state + embedded subtitle engine | `evo_subtitle.h` / `evo_subtitle.c` |
| 3641–3777 | Toast state | `evo_toast.h` / `evo_toast.c` |
| 3937–6254 | File browser, USB navigation, seek | `evo_browser.h` / `evo_browser.c` |
| 6511–6823 | Audio track switching | → `evo_audio_resample.c` |
| 6827–8365 | Error messages, UI drawing helpers | `evo_ui_helpers.h` / `evo_ui_helpers.c` |
| 8366–8958 | Player OSD (overlay HUD) | `evo_osd.h` / `evo_osd.c` |
| 8965–10357 | SRT module + subtitle controls | → `evo_subtitle.c` |
| 10362–12231 | Settings screens (draw + logic) | `evo_settings_screens.h` / `evo_settings_screens.c` |
| 12231–12931 | Dynamic media labels + media info helpers | → `evo_media_meta.c` |
| 12952–13710 | Persistent settings (save/load) | `evo_settings.h` / `evo_settings.c` |
| 13909–16266 | Launch screen (grid, artwork, nav) | `evo_launch_screen.h` / `evo_launch_screen.c` |
| 14776–16266 | Screen draw functions (recent, favorites, settings, about, emby…) | `evo_screen_draw.h` / `evo_screen_draw.c` |
| 16277–16806 | Toast renderer, scrub bar, favorites controls | → respective modules |
| 16810–17267 | Playback complete module | `evo_playback_complete.h` / `evo_playback_complete.c` |
| 17271–18437 | Main loop, input dispatch | `main.c` (keep) |

---

## Target Module Map

```
projects/evoplayer/
├── main.c                        <- keep: init, main loop, input dispatch only
│
├── include/
│   ├── evo_state.h               <- all global playback/screen state declarations
│   ├── evo_recent.h
│   ├── evo_favorites.h
│   ├── evo_cover.h
│   ├── evo_media_meta.h
│   ├── evo_audio_resample.h
│   ├── evo_subtitle.h
│   ├── evo_toast.h
│   ├── evo_browser.h
│   ├── evo_osd.h
│   ├── evo_settings.h
│   ├── evo_settings_screens.h
│   ├── evo_launch_screen.h
│   ├── evo_screen_draw.h
│   └── evo_playback_complete.h
│
└── src/
    ├── evo_state.c
    ├── evo_recent.c
    ├── evo_favorites.c
    ├── evo_cover.c
    ├── evo_media_meta.c
    ├── evo_audio_resample.c
    ├── evo_subtitle.c
    ├── evo_toast.c
    ├── evo_browser.c
    ├── evo_osd.c
    ├── evo_settings.c
    ├── evo_settings_screens.c
    ├── evo_launch_screen.c
    ├── evo_screen_draw.c
    └── evo_playback_complete.c
```

---

## Extraction Order (safe sequence)

Each step: extract -> verify build clean -> commit -> next.

> **Rule:** Never extract two interdependent sections simultaneously. Always extract leaf modules first (no internal callers), then modules that call them.

### Phase 1 — Pure leaf modules (no player state deps)

| Step | Module | Lines extracted | Risk |
|:---:|---|:---:|:---:|
| 1 | `evo_toast` | ~140 | None |
| 2 | `evo_recent` | ~130 | None |
| 3 | `evo_favorites` | ~180 | None |
| 4 | `evo_audio_resample` | ~245 | Low |
| 5 | `evo_media_meta` | ~300 | Low |

### Phase 2 — State and cover (light player state deps)

| Step | Module | Lines extracted | Risk |
|:---:|---|:---:|:---:|
| 6 | `evo_state` | ~400 | Medium — many files will include this |
| 7 | `evo_cover` | ~650 | Medium — uses `evo_state` |

### Phase 3 — Playback modules

| Step | Module | Lines extracted | Risk |
|:---:|---|:---:|:---:|
| 8 | `evo_subtitle` | ~1,100 | Medium |
| 9 | `evo_osd` | ~600 | Medium |
| 10 | `evo_playback_complete` | ~460 | Low |
| 11 | `evo_browser` | ~2,400 | High — deepest player state usage |

### Phase 4 — UI screens

| Step | Module | Lines extracted | Risk |
|:---:|---|:---:|:---:|
| 12 | `evo_settings` | ~760 | Low |
| 13 | `evo_settings_screens` | ~1,500 | Low |
| 14 | `evo_launch_screen` | ~1,200 | Medium |
| 15 | `evo_screen_draw` | ~2,000 | Low |

---

## Key Challenges

### 1. Shared global state
Most modules read/write the same globals (`screen`, `file_selected`, `player_paused`, `g_pp_pb`, etc.). The cleanest approach:

- Declare all shared globals in `evo_state.h` as `extern`
- Define them in `evo_state.c`
- Every other module `#include "evo_state.h"`

### 2. Forward declarations / ordering
`main.c` currently relies on C's top-to-bottom single-TU resolution. Extracting to separate `.c` files means each module's header must be a complete, self-contained interface. Private (non-exported) functions stay `static` in their `.c`.

### 3. Makefile
`MAIN_SRCS` currently lists only `main.c`. Each new `.c` file needs adding to `MAIN_SRCS` (or a new `SRC_DIR` glob). The `gen-compile-commands.sh` script must be re-run after the Makefile changes.

### 4. Include guard hygiene
Every new header needs a unique include guard. Convention: `EVO_<MODULE>_H`.

---

## Success Criteria

- [ ] `main.c` is under **500 lines** (init + main loop + input dispatch only)
- [ ] Every module builds clean individually under sanitizers
- [ ] No behaviour change — same ELF output hash on same source inputs
- [ ] `gen-compile-commands.sh` updated — clangd shows no unknown symbols
- [ ] Each module has a brief `/* Module: ... */` header comment

---

## What stays in `main.c`

After full modularisation, `main.c` contains only:

1. `#include` directives for all modules
2. `int main()` / `_start` entry point
3. Top-level input dispatch (the `while(running)` loop)
4. Screen routing (the `draw_*_screen()` dispatch chain)

Estimated final `main.c` size: **~350 lines**.
