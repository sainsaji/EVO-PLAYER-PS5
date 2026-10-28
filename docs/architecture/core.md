# The C++ core

`projects/evoplayer/core/` is the player proper: the frame loop, screen
navigation, the services that screens use, and the state machines that govern
both. Everything else (decode, GPU, UI drawing) is called from here.

```
core/
├── include/evo/
│   ├── Common.hpp          shared enums: ScreenId, DecoderPreference, Upscaler, ...
│   ├── Application.hpp     the singleton that owns everything
│   ├── interfaces/         IScreen, IStatefulFeature, one I<Service> per service
│   ├── fsm/                IStateMachine, StateMachine<State, Event>
│   ├── animation/          Tween, Easing, AnimationManager
│   ├── audio/              SpatialField, FieldMusic, IEC 61937 + passthrough
│   ├── screens/            one header per screen, plus ScreenManager, StatefulScreen
│   └── services/           service headers
└── src/                    the same layout; Application.cpp, Bridge.cpp
```

## Application

`Application::run()` (called from `main.cpp`) brings the machine up in a fixed
order, and the order matters:

1. hardware decoders probed (`evo_vdec_probe`, audio decoder probe),
2. the GPU runtime (`evo_agc_runtime_init`) at the panel's resolution,
3. the self-unjail (`evo_jailbreak_self`) so `/data` and `/mnt/usb*` appear,
4. the services, then every screen registered with `ScreenManager`,
5. the frame loop.

Each frame polls the network (`evo_net_poll`, so provider calls complete without
blocking), the dev remote, the pad (with auto-repeat), the late-jailbreak rebind
(`evo_jailbreak_poll`, so settings and history move to `/data` when it lands),
then updates and renders the active screen and presents.

Shutdown has two paths. **Settings → System & Diagnostics → QUIT EVO** (or
`evo-remote.sh quit`) calls `requestExit()`: the loop finishes its frame, media
stops, the GPU drains (`agc_wait_gpu_idle`, bounded at 500 ms) and EVO parks on
the *Safe to close* screen (`parked=1` in `evo_status`). The PS button kills the
process and skips all of that; a kill while the GPU is still submitting has
panicked the console, which is why every deploy script waits for `parked=1`. A
`SIGTERM`/`SIGINT`/`SIGHUP` handler sets `g_evo_term_requested` so a signalled
exit takes the orderly path too.

## Services

Services own state that outlives a screen. Each has an interface in
`interfaces/` so screens depend on the contract, not the class.

| Service | Owns |
|---|---|
| `SettingsService` | every user setting and `evo_player_settings.cfg` ([ui/settings.md](../ui/settings.md)) |
| `PlaybackController` | opening a source, the decoder choice, resume, seek/scrub, audio and quality switching, progress reports to providers |
| `FileSystemBrowser` | directory listing, categories, sorting, drive discovery (`/mnt/usb0`..`usb7`, `/mnt/ext0`/`ext1`, `/data`) |
| `FileTransferService`, `FtpClient`, `SmbClient` | copy/move/rename/delete, SMB (Samba/Windows) and FTP shares, and the console's own FTP server |
| `NetworkStorageService` | saved network shares (SMB and FTP via `evo_network_shares.json`) |
| `MediaMetadataService` | cheap `stat()` metadata for lists and container probing for media files |
| `CoverArtService` | posters, hero art and thumbnails, with a memory budget |
| `SoundEffectEngine` | the kit's navigation sounds |
| `SurroundTestService`, `SpeakerCalibrationService` | the Surround Studio tones and the DualSense-mic calibration ([ui/surround-studio.md](../ui/surround-studio.md)) |

## Screens

Every screen implements `IScreen`:

```cpp
getScreenId()  onEnter()  onExit()
handleInput(pressed, held, released)  update(deltaMs)  render(fb, w, h)
```

`ScreenManager` holds the registry, a navigation stack (`navigateTo`,
`navigateBack`), and the side navigation rail. Back is a stack pop, so a screen
reached from two places returns to the right one. Screens look each other up with
`getScreen(ScreenId)` to hand data across (the browser passing a resume prompt to
the dialog, for example).

Screens do not draw anything themselves. They fill a parameter struct and call
`evo_rmlui_update_<screen>()` / `evo_rmlui_render_<screen>()`, which
`ui_rml/src/evo_rmlui_bridge.cpp` routes to the kit first and RmlUi second
([ui/hui-kit.md](../ui/hui-kit.md)).

`ScreenId` (in `Common.hpp`; `evo-remote.sh go <number>` takes these):

| Id | Screen | Id | Screen |
|---|---|---|---|
| 0 | Home (`LaunchScreen`) | 19 | Changelog |
| 1 | File browser (`BrowserScreen`) | 20 | Exit confirm (`ModalDialogScreen`) |
| 2 | Player | 21 | Text reader |
| 3 | Resume prompt (`ModalDialogScreen`) | 22 | `EmbyScreen`, a dead placeholder |
| 10 | Settings hub | 23 | `ProviderHostScreen`: every provider (the old Emby browse slot) |
| 12 | Recent | 24–27, 33, 34 | Settings sections: video, subtitles, interface, system, audio, experimental |
| 13 | Favourites | 28 | Surround Studio |
| 14 | About & support | 30 | Image viewer |
| 15 | Developer tools | 31 | Audio track picker |
| 16 | Media info | 32 | Safe to close (the parked frame) |
| 18 | Subtitle picker | 35 | Chapter picker |

Ids 11 (profile select), 17 (playback finished) and 29 (theme select) are still in
the enum but have no screen; the theme is a Settings row.

Providers (Emby, Jellyfin, Plex, Stremio, Nuvio, IPTV, Xtream) all use
`ProviderHostScreen`, one class parametrised by provider id
([providers/architecture.md](../providers/architecture.md)). The six settings
section screens are one class, `SettingsSectionScreen`.

## State machines

Implicit state (loose booleans, unconstrained integers) caused races on rapid
input, so the long-lived pieces run on a small FSM framework in `evo/fsm/`:

- `IStateMachine`: type-erased: `update`, `getStateName`, `getStateId`,
  `canDispatch(event)`, `dispatchEvent(event)`.
- `StateMachine<TState, TEvent>`: header-only; guards, transition actions,
  per-state `onEnter/onExit/onUpdate`, observer callbacks, no heap allocation on
  the transition path, logging through `<cstdio>`.
- `IStatefulFeature`: the contract. `IScreen` extends it, so a screen that does
  not expose a machine does not compile.
- `StatefulScreen`: the base class that supplies the lifecycle machine
  (`Uninitialized → Entering → Active ⇄ Suspended → Exiting`). A screen with its
  own domain states exposes that machine from `getStateMachine()` instead.

Machines in the tree: `Application` (`ApplicationState`: initialising hardware,
services, screens, running, exiting), `PlaybackController` (`PlaybackState`:
stopped, opening, playing, paused, scrubbing, seeking, finished, error),
`ScreenManager` (`NavigationState`), `BrowserScreen`, `PlayerScreen`,
`LaunchScreen` and `ModalDialogScreen`.

## Network Storage & SMB Streaming

EVO supports browsing and streaming from remote network storage, including Windows / Samba shares (SMB) and FTP servers:

- **`NetworkStorageService`**: Persists configured shares in `/data/evoplayer/evo_network_shares.json`. Configured shares appear in `BrowserScreen` under **Network Storage**.
- **`SmbClient` (`libsmb2`)**: Connects to Windows and Samba shares (`smb://host/Share` or `user:pass@host/Share`) for directory enumeration and file metadata.
- **Direct AVIO Streaming (`evo_smb_io`)**: Playback streams directly over SMB via custom FFmpeg `AVIOContext` callbacks (`evo_smb_io_open`, `evo_smb_io_read`, `evo_smb_io_seek`, `evo_smb_io_close`). No local disk copy or download is required; seeking is handled over the SMB connection, scaling with the user's configured network buffer.
- **Add Server flow (#226)**: `BrowserScreen` runs address, Guest or Account (user, masked password), then `NetworkStorageService::testConnection` on a worker. The test returns the stage that failed (`Address`, `Login`, `Share`, `Folder`) with a plain message shown on the next prompt. It uses its own SMB/FTP client, so it never touches the browse state. An SMB address without a share ends in a share picker (the action menu with a kit switch per row via `action_menu_items[].toggle`: at most 6 shares on screen plus Done; Edit Server on a server root re-runs the test and reopens it with the saved ticks) fed by `NetworkTestResult::shares`; the ticked names go to `NetworkShare::chosenShares` (JSON `shares`, omitted when empty). Empty means show all, and all-ticked is stored as empty. `listEntries` applies `filterChosenShares` at the server root. A prompt is never opened from another prompt's callback: the callback sets a pending flag and `update()` opens it.
- **Status chips (#226)**: each server card shows Online (ms), Offline or Checking. One TCP probe at a time on a worker (`probeReachable`, 2.5 s), cached for the session, Offline re-checked after 30 s. It does not take the listing slot. Not yet in the RmlUi fallback.
- **Server Management**: Pressing **OPTIONS** on a network share opens the Server Options menu (**Open**, **Edit Server**, **Remove Server**). Selecting **Edit Server** re-opens the on-screen keyboard with existing details pre-filled. Shortcut **TRIANGLE** opens the remove confirmation dialog directly.

## Rules for code in the core

- **No `#include <iostream>`** anywhere. Its static initialiser dereferences null
  at load in the app-module CRT, with no log line. Use `<cstdio>`.
- **`stderr` is not usable.** Log with `evo_boot_log()` / `evo_bt()`, which land in
  `/mnt/usb0/evo.log` and klog ([console/diagnostics.md](../console/diagnostics.md)).
- **Only call libc functions EVO already uses.** The clean-room `libc.prx` has no
  `strcasestr`, `opendir` and so on; a missing import is null and crashes at
  address 0 ([app-module.md](app-module.md#traps)). Use `evo_readdir`, `evo_mkdir`
  and `evo_data_path` for the filesystem.
- **Probe lazily and cheaply.** Lists use `stat()` only. Container probing
  (`avformat_open_input`) runs only for audio and video once the cursor has
  rested, and never for text or images; FFmpeg's text demuxers read a whole `.txt`
  as subtitle packets and corrupt memory.
- **Never block the frame loop.** Network, FTP and decode-heavy work run on
  worker threads and complete through callbacks pumped once per frame.
- **Colours are `0xAABBGGRR`** in framebuffers; a raw `0xFF00D7FF` is yellow.
- A new screen extends `StatefulScreen`; a new setting follows
  [ui/settings.md](../ui/settings.md#adding-a-setting).
