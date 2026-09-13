# EVO Player Core Architecture & Legacy Migration Guide

## 1. Overview & Context

During the evolution of EVO Player into **EVO Pro** (the native game-category app module `PPSA99039` supporting bare-metal `sceAgc` GPU rendering and hardware video decode), the codebase underwent a major architectural migration:
* **The Monolith (`main.c.legacy`)**: An ~11,500-line procedural C file containing the entire application logic—pad polling, main frame loop, screen routing, RmlUi drawing, libavformat demuxing/decoding, timers, persistent state, and hardware context setup.
* **The Modern Core (`projects/evoplayer/core/`)**: An object-oriented C++17 modular architecture decoupling UI screens, background services, hardware bridges, and media controllers into distinct, maintainable classes.

While modernizing, `main.c.legacy` remains preserved in the repository as the **authoritative reference implementation**. It embodies years of hard-won PlayStation 5 homebrew compatibility fixes, timing tolerances, edge-case handling, and hardware quirks that are not documented anywhere else.

---

## 2. The Modern `core/` Architecture

```
projects/evoplayer/core/
├── include/evo/
│   ├── Common.hpp                     # Global enums (ScreenId, ViewMode, PlaybackProfile)
│   ├── Application.hpp                # Application singleton & lifecycle orchestration
│   ├── interfaces/
│   │   ├── IScreen.hpp                # Screen lifecycle interface
│   │   ├── IPlaybackController.hpp    # Playback orchestration contract
│   │   ├── IFileSystemBrowser.hpp     # Directory scanning & categorization
│   │   ├── ICoverArtService.hpp       # Thumbnail & hero art generation
│   │   ├── IMediaMetadataService.hpp  # Stream metadata introspection
│   │   └── ISettingsService.hpp       # Persistent configuration
│   ├── screens/                       # Concrete UI screen presenters
│   └── services/                      # Concrete background service implementations
└── src/
    ├── Application.cpp                # Hardware init, pad polling, frame loop, teardown
    ├── Bridge.cpp                     # C-linkage bridges for legacy C modules
    ├── screens/                       # Screen implementations (Browser, Player, Settings, etc.)
    └── services/                      # Service implementations
```

### Key Architectural Patterns

1. **Application Lifecycle Orchestration (`Application.cpp`)**:
   * Initializes hardware (`evo_agc_runtime` — bare-metal AGC, the only render path).
   * Instantiates background services and registers all screens with `ScreenManager`.
   * Drives the master frame loop: network polling, remote USB commands, pad polling, auto-repeat, screen updates, and frame buffer presentation.
   * Handles late jailbreak rebinding (`evo_jailbreak_poll()`) so settings and history persist across launches.

2. **Screen Navigation State Machine (`ScreenManager.cpp`)**:
   * Manages the screen navigation stack (`m_history`) allowing hierarchical back navigation (`Circle` $\to$ `navigateBack()`).
   * Manages the side navigation rail (sections 0–6) and keeps RmlUi in sync via `evo_sync_rmlui_nav()`.
   * Routes pad input to the active screen.
   * Provides typed screen lookup (`getScreen(ScreenId)`) to allow inter-screen data transfer (e.g. browser opening a document or setting a resume prompt).

3. **Decoupled Screen Presenters (`IScreen`)**:
   * Every screen implements `onEnter()`, `onExit()`, `handleInput()`, `update(deltaMs)`, and `render(framebuffer, w, h)`.
   * Screens are stateless presenters where possible: business logic is delegated to persistent services (`PlaybackController`, `FileSystemBrowser`, `CoverArtService`).

---

## 3. How to Use `main.c.legacy` as a Golden Source of Truth

When implementing or debugging any screen or subsystem in `core/`, `main.c.legacy` is your primary blueprint. The table below outlines how legacy structures map to the modern C++ components:

### Screen & Navigation Mapping

| Screen Name | Legacy Screen ID | Modern `ScreenId` | Modern Class | Legacy Functions |
|---|---|---|---|---|
| **Main Menu (Home)** | `SCREEN_MAIN_MENU` (0) | `ScreenId::MainMenu` (0) | `LaunchScreen` | `draw_launch_screen()`, `evo_launch_nav()` |
| **USB Storage Browser** | `SCREEN_USB_BROWSER` (1) | `ScreenId::UsbBrowser` (1) | `BrowserScreen` | `draw_usb_browser()`, `enter_selected_usb()` |
| **Video/Audio Player** | `SCREEN_PLAYER` (2) | `ScreenId::Player` (2) | `PlayerScreen` | `draw_player_screen()`, `prospero_scrub_move()` |
| **Resume Prompt** | `SCREEN_RESUME_PROMPT` (3/4) | `ScreenId::ResumePrompt` (3) | `ModalDialogScreen` | `draw_resume_prompt()`, `load_resume_position()` |
| **Image Viewer** | `3` (legacy route) | `ScreenId::ImageViewer` (30) | `ImageViewerScreen` | `load_stb_image_file()`, `evo_rmlui_render_image()` |
| **Settings Hub** | `SCREEN_SETTINGS` (10) | `ScreenId::Settings` (10) | `SettingsScreen` | `draw_settings_screen()` |
| **Playback Settings** | `SCREEN_SETTINGS_PLAYBACK` (24) | `ScreenId::SettingsPlayback` (24) | `SettingsPlaybackScreen` | `prospero_settings_activate_selected()` |
| **Subtitles Settings** | `SCREEN_SETTINGS_SUBTITLES` (25) | `ScreenId::SettingsSubtitles` (25) | `SettingsSubtitlesScreen` | `draw_settings_screen()` |
| **Interface Settings** | `SCREEN_SETTINGS_INTERFACE` (26) | `ScreenId::SettingsInterface` (26) | `SettingsInterfaceScreen` | `draw_settings_screen()` |
| **System Settings** | `SCREEN_SETTINGS_SYSTEM` (27) | `ScreenId::SettingsSystem` (27) | `SettingsSystemScreen` | `draw_settings_screen()` |
| **Recent Files** | `SCREEN_RECENT_FILES` (12) | `ScreenId::RecentFiles` (12) | `RecentFilesScreen` | `draw_recent_files_screen()` |
| **Favorites** | `SCREEN_FAVORITES` (13) | `ScreenId::Favorites` (13) | `FavoritesScreen` | `draw_favorites_screen()` |
| **About & Support** | `SCREEN_ABOUT_SUPPORT` (14) | `ScreenId::AboutSupport` (14) | `AboutSupportScreen` | `draw_about_support_screen()` |
| **Changelog** | `SCREEN_CHANGELOG` (19) | `ScreenId::Changelog` (19) | `ChangelogScreen` | `draw_changelog_screen()`, `EVO_CHANGELOG_RELEASES` |
| **Developer Tools** | `SCREEN_DEVELOPER_TOOLS` (15) | `ScreenId::DeveloperTools` (15) | `DeveloperToolsScreen` | `draw_developer_tools_screen()` |
| **Media Information** | `SCREEN_MEDIA_INFO` (16) | `ScreenId::MediaInfo` (16) | `MediaInfoScreen` | `draw_mediainfo_screen()` |
| **Subtitle Picker** | `SCREEN_SUBTITLE_PICKER` (18) | `ScreenId::SubtitlePicker` (18) | `SubtitlePickerScreen` | `draw_subtitle_picker()`, `evo_subs_open()` |
| **Document Reader** | `SCREEN_TEXT_READER` (21) | `ScreenId::TextReader` (21) | `TextReaderScreen` | `draw_text_reader_screen()`, `evo_reader_open()` |
| **Surround Sound Test** | `SCREEN_SURROUND_TEST` (28) | `ScreenId::SurroundTest` (28) | `SurroundTestScreen` | `draw_surround_test_screen()` |
| **Exit Confirmation** | `SCREEN_EXIT_CONFIRM` (20) | `ScreenId::ExitConfirm` (20) | `ModalDialogScreen` | `draw_exit_confirm()` |

---

## 4. Case Studies & Critical Lessons from the Migration

### Case Study 1: The Non-Media File Hover Crash
* **Symptom**: Simply moving the cursor over a `.txt`, `.log`, `.nfo`, or `.md` file in the storage browser caused the entire application to crash.
* **The Root Cause**: In `BrowserScreen.cpp`, when the cursor settled on a file for $\ge 150\text{ ms}$, the code unconditionally called `avformat_open_input(&fmt, fullPath.c_str(), ...)` and `avformat_find_stream_info(fmt, ...)`. When invoked on plain text files, FFmpeg's text and subtitle demuxers (`microdvd`, `srt`, `tedcaptions`, `tty`) attempted to read and parse the entire file as subtitle packets, causing excessive allocations and memory corruption.
* **Legacy Ground Truth**: In `main.c.legacy:6996`, media probing (`evo_probe_media`) was strictly gated to video and audio files.
* **Resolution**: Probe format contexts only when `entry->category == FileCategory::Video || entry->category == FileCategory::Audio`. For documents, images, and other formats, call `extractBasicMetadata()`, which safely runs `stat()` for file size.

### Case Study 2: Browser Selection Disappearing on Scroll
* **Symptom**: When scrolling down in a folder with more than 8 files, the cursor highlight disappeared. Pressing Up made it visible again.
* **The Root Cause**: In `browser.rcss`, `#browser-list` has:
  ```css
  #browser-list { top: 222px; height: 742px; overflow: hidden; }
  .brow { height: 84px; margin-bottom: 10px; } /* 94px pitch */
  ```
  Exactly 8 rows fit in $742\text{ px}$ ($8 \times 94\text{px} - 10\text{px} = 742\text{px}$). The 9th row begins at $752\text{ px}$ and is clipped by `overflow: hidden`. However, `BrowserScreen::navigate()` hardcoded `visibleRows = 9;`. When the selection reached item 8 (the 9th item), `m_scrollOffset` did not advance, placing the selection on row index 8 outside the visible container.
* **Legacy Ground Truth**: Legacy layout metrics (`evo_layout.c`) calculated row capacity directly from available content height.
* **Resolution**: Set `visibleRows = 8` in both `navigate()` and `render()` (`params.row_count`). Off-screen rows receive `display: none;`, keeping the focus rectangle anchored to row 7 when scrolling down.

### Case Study 3: The Text Reader & Image Viewer Wiring
* **Symptom**: Selecting a document opened a blank page or crashed; selecting an image showed `UNSUPPORTED`.
* **The Root Cause**:
  * In `activateSelection()`, document activation navigated to `ScreenId::TextReader` without calling `openFile(fullPath)`. `TextReaderScreen` rendered with uninitialized doc pointers and dummy font metrics.
  * Image files were completely unhandled in the browser.
* **Legacy Ground Truth**:
  * Legacy text reader used `evo_textreader.h` (`evo_text_load`, `evo_text_wrap`, `evo_text_scroll`), calibrated advance widths (`reader_measure`: 9, 12, 16, 21), and document status notices (`evo_reader_notice`).
  * Legacy image viewing decoded full-bleed images using `stbi_load` / BMP loader and passed RGBA buffers to `evo_rmlui_update_image`.
* **Resolution**:
  * Implemented `ImageViewerScreen` using `stbi_load` and connected it to `evo_rmlui_render_image`.
  * Rewrote `TextReaderScreen` using `evo_textreader.h`, accurate metrics, status notices (`COULD NOT OPEN`, `FILE EMPTY`), and line numbering badges (`LINE X OF Y`).

### Case Study 4: Resume Playback State Preservation
* **Symptom**: Videos always started from the beginning (0:00).
* **The Root Cause**: `BrowserScreen` did not query `loadResumePosition()`, and `ModalDialogScreen` had no parameters to store the path or target timestamp.
* **Legacy Ground Truth**: `main.c.legacy:2255` checked `load_resume_position()`. If $> 0.0\text{s}$, it captured `pending_resume_pos` and transitioned to `SCREEN_RESUME_PROMPT` (displaying "STOPPED AT XX:XX OF YY:YY").
* **Resolution**: Checked `playback->loadResumePosition()` in `BrowserScreen`. If $> 5.0\text{s}$, set parameters on `ModalDialogScreen` and prompt the user. On "RESUME", playback begins from the saved timestamp; on "START OVER", playback begins from 0.0.

---

## 5. Golden Rules for Future Development

1. **NO `<iostream>` Ever**:
   Never include `<iostream>` in any C++ source file compiled for PS5. The standard stream initialization (`std::ios_base::Init`) triggers static constructors before the PS5 user runtime is stabilized, causing instant SIGSEGV upon boot. Always use `<cstdio>`, `<cstring>`, and `<algorithm>`.
2. **Always Check RCSS Viewport Heights**:
   Never assume row capacities. Inspect the corresponding `.rcss` file (`#browser-list`, `#list-scroll-area`) to compute the exact number of rows that fit before defining `visibleRows`.
3. **Cross-Check `main.c.legacy` for Button Contracts**:
   Button bindings must match user muscle memory established in legacy EVO Player:
   * `Circle`: Cancel / back navigation (`ScreenManager::navigateBack()`).
   * `Triangle`: Contextual action (e.g. Favorite in browser, subtitle size in player, silence tone in surround test).
   * `Square`: Information / secondary screen (MediaInfo in player, DevTools in About, compatibility export).
   * `Options`: Stats for nerds toggle.
4. **Decouple Probing from Browsing**:
   File metadata extraction in lists must be lightweight (`stat()` only). Deep container parsing (`avformat_open_input`) must only run when the cursor has settled for $\ge 150\text{ ms}$ on verified audio/video formats.
5. **Mandatory State Machine Implementation**:
   All new screens, media services, and subsystem orchestrators must implement state machines via `IStatefulFeature`. Never rely on loose boolean flags or unconstrained state integers.

---

## 6. State Machine Architecture & Mandatory Contract for Future Features

To eliminate implicit state bugs, race conditions during rapid user input, and unmanaged lifecycle states, EVO Player enforces a **formal Finite State Machine (FSM) architecture** across all major subsystems.

### 6.1 Core Framework Components (`evo/fsm/`)

The framework is designed specifically for PlayStation 5 native execution (C++17, zero `<iostream>`, zero heap allocation on the transition path):

* **[`IStateMachine`](file:///D:/Projects/EVO%20Player/projects/evoplayer/core/include/evo/fsm/IStateMachine.hpp)**:
  Abstract, type-erased interface providing polymorphic access to any state machine:
  - `update(double deltaMs)`: Tick time-dependent state logic.
  - `getStateName()`: Human-readable name of the current active state for diagnostics and logging.
  - `getStateId()`: Integer representation of the current state.
  - `canDispatch(int eventId)`: Query if an event transition is valid from the current state.
  - `dispatchEvent(int eventId)`: Trigger an event transition.

* **[`StateMachine<TState, TEvent>`](file:///D:/Projects/EVO%20Player/projects/evoplayer/core/include/evo/fsm/StateMachine.hpp)**:
  Strongly-typed, header-only template implementation supporting:
  - Guard conditions (`std::function<bool()>`) evaluated before transitions.
  - Transition actions (`std::function<void()>`) executed atomically during state change.
  - Per-state lifecycle handlers: `onEnter()`, `onExit()`, and `onUpdate(double deltaMs)`.
  - State change observer callbacks.
  - No `<iostream>` dependencies: uses `<cstdio>` for snprintf-safe logging.

* **[`IStatefulFeature`](file:///D:/Projects/EVO%20Player/projects/evoplayer/core/include/evo/interfaces/IStatefulFeature.hpp)**:
  The architectural contract interface:
  ```cpp
  class IStatefulFeature {
  public:
      virtual ~IStatefulFeature() = default;
      virtual IStateMachine* getStateMachine() = 0;
      virtual const IStateMachine* getStateMachine() const = 0;
  };
  ```

### 6.2 Architectural Enforcement

The requirement that future features implement state machines is enforced at **compile time**:

1. **`IScreen` Extends `IStatefulFeature`**:
   `IScreen` derives directly from `IStatefulFeature`. If a developer creates a new screen class without implementing `getStateMachine()`, the compiler will reject it as an abstract type instantiation error.

2. **`StatefulScreen` Base Class**:
   [`StatefulScreen`](file:///D:/Projects/EVO%20Player/projects/evoplayer/core/include/evo/screens/StatefulScreen.hpp) provides a turn-key implementation of `IStatefulFeature` for screens. It manages a `StateMachine<ScreenLifecycleState, ScreenLifecycleEvent>`:
   - States: `Uninitialized` $\to$ `Entering` $\to$ `Active` $\rightleftharpoons$ `Suspended` $\to$ `Exiting`.
   - Automatically handles lifecycle hook transitions when `onEnter()`, `onExit()`, and `update()` are called.
   - Any screen with custom domain states (e.g. `PlayerScreen`, `BrowserScreen`, `LaunchScreen`, `ModalDialogScreen`) can either override `getStateMachine()` to expose its domain FSM or nest multiple state machines.

3. **Subsystem State Machines**:
   - **`Application`**: Driven by `m_appFsm` (`ApplicationState`: `Uninitialized` $\to$ `InitializingHardware` $\to$ `InitializingServices` $\to$ `InitializingScreens` $\to$ `Running` $\to$ `Exiting` $\to$ `Terminated`).
   - **`PlaybackController`**: Driven by `m_playbackFsm` (`PlaybackState`: `Stopped`, `Opening`, `Playing`, `Paused`, `Scrubbing`, `Seeking`, `Finished`, `Error`).
   - **`ScreenManager`**: Driven by `m_navFsm` (`ScreenManagerState`: `ScreenActive`, `RailFocused`, `ModalActive`).
   - **`BrowserScreen`**: Driven by `m_browserFsm` (`BrowserScreenState`: `Browsing`, `ProbingMedia`, `Searching`).
   - **`PlayerScreen`**: Driven by `m_playerFsm` (`PlayerScreenState`: `NormalPlayback`, `OsdVisible`, `Scrubbing`, `StatsOverlay`).
   - **`ModalDialogScreen`**: Driven by `m_modalFsm` (`ModalDialogState`: `Inactive`, `Prompting`, `Confirmed`, `Cancelled`).

### 6.3 Implementing a New Feature / Screen

When implementing a new screen or feature in EVO Player:

```cpp
// 1. Inherit from StatefulScreen (or implement IStatefulFeature directly)
#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class MyNewFeatureScreen : public StatefulScreen {
public:
    MyNewFeatureScreen() : StatefulScreen("MyNewFeatureScreen") {}

    ScreenId getScreenId() const override { return ScreenId::Custom; }

    void onEnter() override {
        StatefulScreen::onEnter(); // Advances lifecycle state to Active
        // Initialize screen data
    }

    void onExit() override {
        StatefulScreen::onExit(); // Advances lifecycle state to Suspended
        // Clean up resources
    }

    void update(double deltaMs) override {
        StatefulScreen::update(deltaMs); // Ticks lifecycle FSM
    }

    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override {
        // Handle input only when active
        if (!isScreenActive()) return false;
        // ...
        return true;
    }

    void render(uint32_t* framebuffer, int width, int height) override {
        // Render UI
    }
};

} // namespace evo
```

