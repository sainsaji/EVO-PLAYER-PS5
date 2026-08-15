# EVO Player - Native RmlUi Integration Specification & Porting Guide

**Target Architecture:** PlayStation 5 (FreeBSD / libkernel / SceVideoOut / ScePad / libSceAvPlayer)  
**Branch:** `feat/rmlui-native-integration`  
**Engine:** RmlUi (Retained-Mode C++ HTML/RCSS Engine)  

---

## 1. Executive Summary & Core Principles

This document defines the strict engineering architecture for replacing the legacy immediate-mode SDF renderer with **RmlUi** (a lightweight, retained-mode C++ HTML/RCSS engine) while maintaining **100% parity with the production stability, functionality, and aesthetic of the `main` branch**.

### Core Architecture Tenets
1. **Zero Mock Data**: The UI is purely a presentation layer. All data displayed in the DOM is dynamically bound to live C engine structures (`EVOPlayerState`, `evo_file_entry_t`, `current_path`, `evo_probe_info_t`, `evo_settings_t`, `RecentFileEntry[]`, `favorite_files[]`).
2. **Deterministic Navigation**: The USB browser must start at `/mnt/usb0` root and strictly mirror the native directory stack and real filesystem scanner (`evo_file.c` / `scan_directory()`).
3. **DOM List Virtualization**: Directory listings with hundreds or thousands of files must only render a visible viewport window of 12–14 DOM rows to maintain 60 FPS performance and zero CPU memory churn.
4. **Exact PS5 Color Math**: Framebuffer format is strictly `0xAABBGGRR` (BGRA in memory: Byte 0=Red, Byte 1=Green, Byte 2=Blue, Byte 3=Alpha).
5. **High-Performance Rasterizer**: Axis-aligned quad fast-paths with SIMD span-fills bypass barycentric division for panels, cards, and text glyphs.

---

## 2. System Architecture & Layer Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       EVO Player Native C Core                          │
│   (Playback Engine, FFmpeg Demuxer, SceAvPlayer, File Scanner, Audio)   │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │ Direct State Structs
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     C++ Data Binding & Bridge Layer                     │
│  - evo_rmlui_bridge.cpp: Exposes clean C API to main.c                 │
│  - Data Models: RmlUi DataBinding (Rml::DataModelHandle)                │
│    Binds EVOPlayerState, FileList, Settings, Recent, Favorites directly │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                  ┌──────────────────┴──────────────────┐
                  ▼                                     ▼
┌───────────────────────────────────┐ ┌───────────────────────────────────┐
│     Gamepad Spatial Navigator     │ │       RmlUi Retained Engine       │
│ - DualSense D-pad / Sticks / Cross │ │ - DOM Tree & Event Dispatcher     │
│ - 2D Spatial Focus Traversal      │ │ - RCSS Stylesheet Engine          │
│ - Virtualized List View (12 rows) │ │ - FreeType2 Font Atlas Engine     │
└─────────────────┬─────────────────┘ └─────────────────┬─────────────────┘
                  │                                     │
                  └──────────────────┬──────────────────┘
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                  PS5 High-Speed RenderInterface                         │
│  - Optimized Axis-Aligned Quad Fast-Path (SSE / AVX2 / SIMD)           │
│  - Exact Native Framebuffer Format (0xAABBGGRR / BGRA in memory)        │
│  - Zero-Copy Texture Buffer Cache for Video Thumbnails & Icons          │
│  - Frame Pacing synced to SceVideoOut (60.000 FPS / 16.6ms)             │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Data Models & Bridge Interface

### 3.1 C Linkage API (`evo_rmlui_bridge.h`)
The C core in `main.c` interfaces with RmlUi through a small, clean set of lifecycle and state synchronization functions:

```c
#ifndef EVO_RMLUI_BRIDGE_H
#define EVO_RMLUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Engine Lifecycle
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
void evo_rmlui_set_screen(int screen_id);
int  evo_rmlui_get_screen(void);

// Input & Event Pumping
void evo_rmlui_process_input(uint32_t buttons_pressed, uint32_t buttons_held);

// Frame Cycle
void evo_rmlui_update(float delta_time);
void evo_rmlui_render(uint32_t* framebuffer);

// State Synchronization (Direct C Struct Pointers)
void evo_rmlui_sync_browser(const char* path, const void* files_array, int file_count, int selected_idx, int top_idx);
void evo_rmlui_sync_inspector(const void* probe_info, const char* filename, uint64_t size_bytes, bool is_fav);
void evo_rmlui_sync_launch(const void* recent_array, int recent_count, const void* fav_array, int fav_count);
void evo_rmlui_sync_settings(const void* settings_struct);
void evo_rmlui_sync_mediainfo(const void* full_probe_info, const void* memory_stats);
void evo_rmlui_sync_modal(const char* title, const char* msg, uint64_t timestamp_ms, uint64_t duration_ms);

#ifdef __cplusplus
}
#endif
#endif
```

---

## 4. Virtualized File Browser Engine

### 4.1 The Viewport Window Pattern
To handle folders with 5,000+ files without DOM lag:
- The DOM contains exactly **12 file-row elements** (`#file-row-0` through `#file-row-11`).
- The C bridge calculates the visible slice: `[top_index, top_index + 12)`.
- When navigating:
  - Moving down within the screen changes the active focus row.
  - Moving down past row 11 increments `top_index` and updates the text/badges of the 12 DOM elements in-place.
- Result: **0ms allocation overhead**, instant scrolling, rock-solid 60 FPS.

---

## 5. Software Rasterizer & Exact Color Math

### 5.1 Color Mapping Formula
PS5 framebuffer is linear `0xAABBGGRR` (BGRA in memory):

```cpp
static inline uint32_t blend_bgra(uint32_t dst, uint32_t src, uint32_t a) {
    if (a == 0) return dst;
    if (a >= 255) return src | 0xFF000000;

    uint32_t inv = 255 - a;
    uint32_t dr = dst & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = (dst >> 16) & 0xFF;

    uint32_t sr = src & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = (src >> 16) & 0xFF;

    uint32_t r = (sr * a + dr * inv + 127) / 255;
    uint32_t g = (sg * a + dg * inv + 127) / 255;
    uint32_t b = (sb * a + db * inv + 127) / 255;

    return 0xFF000000 | (b << 16) | (g << 8) | r;
}
```

### 5.2 Axis-Aligned Quad Fast-Path
95% of geometry in RmlUi consists of rectangular panels, borders, and text glyphs.
- `try_draw_fast_quad()` detects quads with matching X/Y bounds and executes a direct scanline loop.
- UV texture coordinates for FreeType font glyphs are clamped strictly within `[min_u, max_u]` and `[min_v, max_v]` to prevent atlas neighbor bleeding.

---

## 6. Gamepad Spatial Navigation Engine

### 6.1 DualSense Mapping
- **D-Pad Up / Down**: Moves vertical index in file lists, setting rows, or shelf items.
- **D-Pad Left / Right**: Moves between master file pane and right inspector / settings categories.
- **Cross (X)**: Activates focused element / opens folder / starts playback.
- **Circle (O)**: Back to parent directory (`..`) or back to Launch Home.
- **Triangle (Δ)**: Toggles Favorite on current file / opens Media Info deck.
- **Square (□)**: Opens search & filter virtual keyboard.
- **L1 / R1**: Switches top navigation tabs (`Home`, `Browser`, `Recent`, `Favorites`, `Settings`).
- **Options**: Context menu / Quick audio & subtitle selector.

---

## 7. Screen Layouts & Parity Specifications

### 7.1 Launch / Home Screen (`launch.rml`)
- **Top Navigation Bar**: Brand logo, screen tabs, hardware capability pill (`PS5 12.70 • 4K HDR10`), profile avatar.
- **Hero Billboard**: Shows real most recently played video from `recent_files[0]`, formatted duration, playback progress bar, and action buttons (`RESUME`, `BROWSE USB`, `MEDIA INFO`).
- **Jump Back In Shelf**: Horizontal cards populated with real items from `recent_files[1..4]`.
- **Studio Hub Cards**: USB 3.0 Media Drive, Emby Cloud Media Hub, Surround 7.1 Studio, System Settings.
- **Bottom Controller Bar**: Clear button hints matching PS5 DualSense icons.

### 7.2 USB Storage Browser (`browser.rml`)
- **Header**: Active directory path (e.g. `/mnt/usb0/Movies`), file count badge, USB connection status.
- **Left Pane (Master File List)**: 12 virtualized rows with icon/badge (`DIR`, `4K`, `HD`, `AUDIO`), file name, size / duration, and favorite indicator.
- **Right Pane (Grand Inspector)**: Live FFmpeg probe telemetry for currently highlighted file:
  - Container (`.mkv`, `.mp4`, `.m4a`)
  - Video resolution & bitstream codec (`HEVC Main 10`, `H.264 High`)
  - Primary audio track & channel layout (`AAC 5.1`, `Dolby Atmos`)
  - Subtitle language tracks
  - Decoder pipeline indicator (`Direct Memory Slab (64MB Cache)`)
  - Action buttons (`PLAY / RESUME`, `+ FAVORITE`, `DETAILS`)

### 7.3 Stream & AV Diagnostics Telemetry Deck (`mediainfo.rml`)
- 3 distinct telemetry columns:
  1. **Video Stream & Bitstream**: Resolution, aspect ratio, codec, color space (`BT.2020`), dynamic range (`HDR10+ / Dolby Vision`), FPS, bitrate.
  2. **Audio Master & 7.1 Surround**: Codec, channels, sample rate, bit depth, output port (`SceAudioOut 7.1 Direct`).
  3. **Stream Telemetry & Memory Slab**: Direct memory slab cache, frame drops (0.00%), VSync cadence pacing (60.0 FPS stable).

### 7.4 Player Settings & Engine Configuration (`settings.rml`)
- Left category rail: `Playback & Video`, `Audio & 7.1 PCM`, `Subtitles & Fonts`, `System & Diagnostics`.
- Right option list with interactive toggles and option selectors.

### 7.5 Resume Playback Modal (`modal.rml`)
- Centered dark glassmorphic dialog with media title, last saved timestamp, and actions (`RESUME AT [TIMESTAMP]`, `START FROM BEGINNING`, `CANCEL`).

---

## 8. Directory & File Organization

```
projects/evoplayer/
├── assets/
│   ├── fonts/
│   │   ├── Roboto-Regular.ttf
│   │   ├── Roboto-Medium.ttf
│   │   └── Roboto-Bold.ttf
│   ├── icons/
│   │   ├── badge_1080p.png
│   │   ├── badge_4k.png
│   │   ├── badge_hevc.png
│   │   ├── badge_flac.png
│   │   ├── icon_play.png
│   │   └── ...
│   └── rml/
│       ├── common.rcss
│       ├── launch.rml / launch.rcss
│       ├── browser.rml / browser.rcss
│       ├── mediainfo.rml / mediainfo.rcss
│       ├── settings.rml / settings.rcss
│       ├── emby.rml / emby.rcss
│       ├── playback.rml / playback.rcss
│       ├── modal.rml / modal.rcss
│       └── keyboard.rml / keyboard.rcss
├── ui_rml/
│   ├── include/
│   │   ├── evo_rmlui_app.h
│   │   ├── evo_rmlui_bridge.h
│   │   ├── evo_rmlui_render.h
│   │   └── evo_rmlui_system.h
│   └── src/
│       ├── evo_rmlui_app.cpp
│       ├── evo_rmlui_bridge.cpp
│       ├── evo_rmlui_render.cpp
│       └── evo_rmlui_system.cpp
└── main.c
```

---

## 9. Verification & Quality Assurance Pipeline

1. **Host Mockup Render**: Run `./tools/uiview_rml.sh` to generate local PNGs in `output/uiview_rml/` for immediate layout, typography, and contrast verification.
2. **PS5 Hardware Build & Deploy**: Run `./scripts/push-ps5.sh --shot` to sync the bundle to `/data/homebrew/EVOPlayer/` and `/data/evoplayer/app/` on the PS5.
3. **PS5 Screenshot Capture**: Automatically retrieve the newest frame capture from `/mnt/usb0` via FTP (`./tools/shot.sh grab`).
4. **Side-by-Side Validation**: Compare PS5 hardware screenshots against `output/uiview/` from `main` to verify 100% visual and functional alignment before any merge consideration.
