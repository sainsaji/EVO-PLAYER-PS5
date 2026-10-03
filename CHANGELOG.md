# Changelog

Notable changes per release. The release workflow lifts the matching section
into the GitHub release notes, so keep the headings in the form `## 0.1.0`.

---

## 0.11.0

![](https://img.shields.io/badge/Release-v0.11.0-blueviolet?style=flat-square) ![](https://img.shields.io/badge/PS5%20Hardware-Verified-0070d1?style=flat-square&logo=playstation&logoColor=white) ![](https://img.shields.io/badge/Firmware-12.70-blue?style=flat-square)

**EVO plays what you have, from wherever it is.** 0.10.0 made EVO a real PS5 app; 0.11.0 fills it in. Live TV from an M3U playlist, your Emby or Jellyfin library in EVO's own screens, real-time AI upscaling for anything below 4K, Dolby Vision, a surround test room you can fly a sound around, and file management with FTP so a PC is no longer in the loop. 96 commits since 0.10.0.

Download **`PPSA99039.ffpfsc`**, deploy it with ShadowMountPlus, then launch EVO from the Games row.

### ![](https://img.shields.io/badge/PROVIDERS-e91e63?style=flat-square) Live TV & Media Servers

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Providers**, reached from the nav rail or the home screen, replacing the Emby entry that was switched off in 0.10.0. **Cross** opens one, **Square** sets its address, **Triangle** opens the web version in the PS5's own browser, and OPTIONS twice signs out.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Live TV from an M3U playlist.** Put `iptv.m3u` on a USB stick and EVO finds it with no typing, or enter a URL. Channels browse as a 2×4 grid of logos with L1/R1 paging, a detail pane, favourites and search.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **A channel guide.** XMLTV with NOW and NEXT on the channel, discovered automatically from USB or the web — each candidate is scored by how many of your channels it actually covers — or pointed at a feed of your own. Xtream accounts use their own EPG.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Xtream Codes accounts.** Set one up by hand, or load an M3U exported from the account and EVO reads the credentials out of it. Live, VOD and Series categories, with a sign-out that removes the account.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Emby, Jellyfin and Stremio addons as native EVO screens** — poster grids, Continue Watching, libraries, series, seasons and episodes, with resume and watched state written back to the server. Jellyfin Quick Connect is tried before asking for a password, and the server address is pre-filled from LAN discovery.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Pick which version plays.** A title with several versions lists them with resolution, codec, HDR or Dolby Vision, size, bitrate and audio format.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Web versions of Emby, Jellyfin and Nuvio** run in the console's browser beside the nav rail, hand the stream to EVO's player when you press Play, and keep their sign-ins between sessions.

### ![](https://img.shields.io/badge/PICTURE-ff8c00?style=flat-square) The Picture

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Real-time upscaling for anything below 4K.** Settings → Video & Display → `UPSCALING`: **Sharp** is FSR 1 sharpening, **AI** runs Anime4K. `AI NETWORK` picks Standard, Large or Maximum — Maximum is for a PS5 Pro. The player's OSD names what ran, and says why when it bypassed.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Dolby Vision Profile 5** reshapes on the GPU from the RPU, so skin is no longer purple and backgrounds no longer green.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **AV1**, including raw `.obu` files and 4K 10-bit, through libdav1d.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **4K HEVC Main10 HDR on the console's hardware decoder**, with decoder slots that grow on demand.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **120 Hz output** — Off, Auto, or Playback Only — for smoother 24 fps. The row dims itself when the display or HDMI sink cannot do it.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **`HDR OUTPUT` is yours to set**, Auto or Off, rather than always following the file.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **HDR to SDR tone mapping is BT.2390**, not Reinhard, with a correct BT.2020 → BT.709 conversion. 8-bit NV12 HDR and HLG streams get their own pipelines, and HLG highlights are no longer pink or cyan.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **The `VIDEO DECODER` setting is honoured** — it was silently resolving to Auto whatever you chose — and 4K software decode is on by default.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Exotic pixel formats display** instead of decoding into a dead buffer: 4:2:2, 4:4:4, 10-bit 4:4:4 and GBRP showed a black screen.

### ![](https://img.shields.io/badge/AUDIO-9c27b0?style=flat-square) Sound

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Surround Sound Studio.** Settings → Audio → `SURROUND SOUND TEST` opens a 2.5D room: a perspective floor with distance rings, per-speaker VU meters, and eight tests — 3D sound field, spatial orb free-roam, a 360° sweep, auto tests for 5.1 and 7.1, and the speaker layout. The orb flies on the stick, the touchpad or the D-pad, with L1/R1 for height.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Room calibration with the DualSense microphone.** It measures each speaker's level and delay, applies the trims to playback and saves them.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Stereo downmix.** Settings → Audio → `OUTPUT CHANNELS` folds 5.1 and 7.1 to 2.0 for headphones or a stereo TV.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Every audio codec FFmpeg supports**, Dolby TrueHD included, plus WMV, FLV and ASF containers.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Opus files no longer crash playback.**

### ![](https://img.shields.io/badge/SUBTITLES-00bcd4?style=flat-square) Subtitles

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Auto-sync.** The picker's `AUTO-SYNC` row aligns the subtitles to the audio, shows its progress, and says so plainly when the file is not confident enough to align.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Two tracks at once** — Cross sets the primary, Square the secondary — with the secondary's position (stacked or top of screen) and colour under Settings → Subtitles.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Nudge the delay while you watch**, by swiping the DualSense trackpad or with L2, without opening a menu. L1/R1 move the secondary.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **WebVTT** decodes.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Subtitle size really cycles.** LARGE was unreachable and two of the three stops rendered identically.

### ![](https://img.shields.io/badge/FILES-795548?style=flat-square) Your Files

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **File operations in the browser.** OPTIONS on anything opens copy, cut, paste, rename, delete and new folder. Transfers run off the UI thread with the current item, bytes, speed and time remaining, cancel at a chunk boundary, and resolve a name collision as overwrite, skip or auto-rename.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **FTP network storage.** Add a server from the browser's sidebar and browse, play and copy to and from it like any other source.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Folder filter chips.** Press Up from the top row to filter the folder you are actually looking at, instantly, with a chip only for the categories that have files there. They used to search the whole drive, not filter, not clear, and freeze the UI.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **The All Videos, Music and Photos views** recurse from the source root and list files rather than folders.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **No more `[Unknown] [Unknown]` codec pills** on text and log files.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The browser no longer hangs** when the jailbreak daemon is not running.

### ![](https://img.shields.io/badge/PLAYBACK-0070d1?style=flat-square) Playback

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **A preview frame on the seek bar.** The pipeline parks while you scrub, and the preview keeps up with a moving bar instead of trailing it.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Network streams pre-buffer** before the decoders start, and **rebuffer** when the read-ahead runs dry instead of stuttering silently. The read-ahead is sized in seconds of video, not bytes.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **A `BUFFERING...` indicator** for any source, until the first frame lands and again mid-file if the stream runs out.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Quality and audio picker on R2.** On a live stream it lists the renditions read from the HLS master as well as the audio tracks, and your choice sticks across reopens. `ASK WHICH LIVE STREAM` under Settings → Interface & Storage controls whether it asks when a channel opens.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Live TV's OSD knows it is live** — no seek bar, timer or chapter keys on a stream with nothing behind the live edge.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Live channels play their best rendition.** Stream selection took the first video stream, which by convention is the lowest bitrate.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Big MKVs open instead of hanging.** An 85-stream UHD remux never reached playback, because the demuxer probe was unbounded.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Resuming no longer runs subtitles two to three seconds early.**

### ![](https://img.shields.io/badge/INTERFACE-007acc?style=flat-square) Interface

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Settings is six pages**, not four: Video & Display, **Audio**, Subtitles, Interface & Storage, System & Diagnostics and **Experimental** — each row with its own icon.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **A Safe to close screen.** QUIT EVO, from the nav rail or Settings, releases playback, the decoders and the GPU and then tells you it is done, with the three steps to close it from the switcher.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **An `UPSCALER` row in Media Info** (Square during playback), and the console model under System & Diagnostics.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Toasts are visible at 4K.** Every overlay — toasts, the keyboard, the debug overlay — was projected at twice its size and landed off-screen, so on a 4K panel no toast was ever seen.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The virtual keyboard draws.** It opened and took input while never being rendered.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The letterbox bars are black** instead of keeping the last menu frame in the corner for the length of a film.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Readable text**: the version pill and the `PAUSED` badge were white on near-white in Carbon, and the OSD no longer vanishes while paused.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The native PS5 keyboard is the default again**, and HDR10 screenshots are no longer rainbow noise.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The splash screen dismisses** on the first presented frame.

### ![](https://img.shields.io/badge/EXPERIMENTAL-6e7681?style=flat-square) Experimental

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Motion smoothing** — 24 → 60 fps synthesised on the GPU, Off, Low or High, under Settings → Experimental. It costs about 0.55 ms of GPU time, and it bypasses itself for 50 fps or faster sources, 10-bit and HDR, and anything above 1080p. It is unfinished: expect soft edges and haloes around fast motion, and worse if AI upscaling is on at the same time — the row says so when it is.

### ![](https://img.shields.io/badge/INTERNAL-6e7681?style=flat-square) Under the Hood

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **The console's own DNS resolver**, replacing a hand-rolled UDP client with a hardcoded server list.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Logs redact API keys, tokens and provider credentials.**
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Deploys are verified by sha256** before the new image is promoted, and the dev remote can launch, quit and close EVO without touching a controller.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Large allocations come from direct memory first**, and the GPU's transient ring doubled to 128 MB.

---

## 0.10.0

![](https://img.shields.io/badge/Release-v0.10.0-blueviolet?style=flat-square) ![](https://img.shields.io/badge/PS5%20Hardware-Verified-0070d1?style=flat-square&logo=playstation&logoColor=white) ![](https://img.shields.io/badge/Firmware-12.70-blue?style=flat-square)

**EVO is a real PS5 app now.** It runs as a game-category app module, decodes video on the console's own hardware decoder at 4K, renders its entire interface on the GPU, and the UI has been rebuilt from scratch. 315 commits since 0.7.0.

Download **`PPSA99039.ffpfsc`** and deploy it with ShadowMountPlus, then launch EVO from the Games row. The ELF payload downloads from previous releases no longer apply — that route has been removed.

### ![](https://img.shields.io/badge/APP%20MODULE-e91e63?style=flat-square) A Real Application

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Game-category app module (`PPSA99039`).** EVO launches from the Games row like any other title, with a real user session, its own graphics and audio, instead of borrowing a background service with no display plane.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Self-unjail for `/data`.** The app opens its own sandbox at boot, so settings, resume points and Emby credentials persist to internal storage rather than depending on a USB stick.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Quit EVO.** A soft close in Settings → System & Diagnostics that stops playback, releases the decoders and drains the GPU before you close the app from the switcher.

### ![](https://img.shields.io/badge/DECODE-ff8c00?style=flat-square) Hardware Video Decode

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **`sceVideodec2` hardware decode at 4K.** H.264, HEVC and VP9 all decode on the console's own decoder, with resident per-codec decoders created once at boot.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **10-bit HDR.** HDR10 (PQ) and HLG tone mapping with a BT.2020 matrix, and a per-frame HDR VideoOut switch.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Native audio decode** through the console's audio decoder.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Every video container opens**, with an automatic FFmpeg fallback when a clip falls outside what the hardware decoder accepts.

### ![](https://img.shields.io/badge/GRAPHICS-0070d1?style=flat-square) The Interface on the GPU

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Bare-metal `sceAgc` rendering.** The whole UI is submitted to the GPU as real draw calls, replacing the CPU rasteriser.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Renders at the panel's own resolution** instead of a fixed 1080p surface.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Three scanout buffers**, each fenced against the flip that replaces it.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **Menus hold 60 fps** and only redraw when something actually changes, so an idle screen costs nothing.

### ![](https://img.shields.io/badge/INTERFACE-007acc?style=flat-square) Rebuilt From Scratch

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Every screen redrawn in RmlUi**, bound to live player state rather than mock data.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Colour themes** — Midnight, Carbon, Ember, Aurora and a USB-supplied theme — applied consistently across every document.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Storage browser rebuilt** with a thumbnail on every card, and Recent and Favourites folded into it.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Media Info, subtitle picker and dialogs** redesigned, with the navigation rail streamlined to five sections.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Screenshot capture** straight to USB, and a developer tools screen with a live performance overlay.

### ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) Fixes

- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The scrub head starts where the picture is.** Beginning a scrub reset the bar and clock to the previous seek point, or to the start of the file on the first scrub of a session.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Choosing a theme changes the colours.** Only the first theme in the list ever applied; every other one silently did nothing and was never saved.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Resuming no longer flashes 0:00** before jumping to the saved position.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Seeking keeps audio in step** with the picture, and stops presenting stale frames during the discard window.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Posters decode on the hardware decoder** and survive a software fallback.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Cover art is cached instead of re-read every frame** — a deleted file in Recent caused continuous filesystem access and log writes for the life of the process.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Closing no longer leaves the GPU mid-flight.** Teardown drains submitted work before releasing the scanout registration and the memory it points at.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Text no longer clips** in the OSD, changelog and dialogs, and Emby streams show a real title instead of a URL.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **The version shown in the app is the version you ran** — it is now defined once and read by the build, the tests and the release workflow.

### ![](https://img.shields.io/badge/REMOVED-6e7681?style=flat-square) Removed

- ![](https://img.shields.io/badge/REMOVED-6e7681?style=flat-square) **The ELF payload launch path.** The app module replaces it entirely; the old push scripts are gone and should not be recreated.
- ![](https://img.shields.io/badge/REMOVED-6e7681?style=flat-square) **Emby, for now.** The media-server integration is turned off while it is reworked. Nothing is deleted - it still builds, it is just unreachable from the UI - and it returns in a later release.
- ![](https://img.shields.io/badge/REMOVED-6e7681?style=flat-square) **The software picture pipeline** — the CPU YUV converters and tile copy — now that the GPU does the work.

---

## 0.7.0

![](https://img.shields.io/badge/Release-v0.7.0-blueviolet?style=flat-square) ![](https://img.shields.io/badge/PS5%20Hardware-Verified-0070d1?style=flat-square&logo=playstation&logoColor=white) ![](https://img.shields.io/badge/Firmware-12.70-blue?style=flat-square)

**Emby password authentication, native HTTPS/TLS streaming, master-detail hierarchical changelog viewer, direct memory buffer streaming, and native PS5 IME keyboard.**

Download **`EVOPlayer-0.7.0-InstallTile.elf`** to register the Media tile on the home screen, or **`EVOPlayer-0.7.0-player-only.elf`** for homebrew app slot launch.

### ![](https://img.shields.io/badge/ADDON-007acc?style=flat-square) Emby Password Authentication & Setup

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Account Password Support.** Dedicated interactive password configuration row in Emby Setup screen with virtual keyboard input and secure asterisk masking (`********`).
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Robust cJSON Authentication.** Server authentication payloads formatted with `cJSON` for compliant user session tokens against password-protected Emby and Jellyfin servers.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Config Persistence.** Safe loading and saving of passwords in `/data/evoplayer/emby.conf` and USB configurations with key-value and legacy positional compatibility.

### ![](https://img.shields.io/badge/UI%2FUX-007acc?style=flat-square) Hierarchical Changelog Viewer

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Master-Detail Version Architecture.** Replaced legacy flat cards with a clean dual-pane layout: a version selector on the left and a dedicated release notes inspector card on the right.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Categorized Visual Badges.** Clear `[NEW]`, `[FIXED]`, `[IMPROVED]`, and `[REMOVED]` indicator pills with accent color coding.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Instant Version Switching.** Fluid D-pad navigation across all releases with automatic detail panel updates.

### ![](https://img.shields.io/badge/CORE-ff8c00?style=flat-square) Core Subsystems & Network Engine

- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) ![](https://img.shields.io/badge/SECURITY-e91e63?style=flat-square) **Native HTTPS & TLS Support.** Integrated OpenSSL (`libssl.a`/`libcrypto.a`) with TLS 1.2/1.3 handshakes, SNI extension, and secure communication for remote HTTPS media servers.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Direct Memory Region Manager.** High-throughput streaming buffer allocations leveraging native PS5 direct memory (`dmem`).
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Native PS5 IME Keyboard Support.** Seamless integration with `libSceImeDialog.sprx` supporting multilingual input and USB keyboards, with automatic fallback to custom virtual keyboard.
- ![](https://img.shields.io/badge/NEW-007acc?style=flat-square) **Media Directory Search.** Interactive search modal in file browser for fast folder and item filtering.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Streaming Stability.** Fixed custom AVIO lifecycle conflict and stabilized network streaming playback.
- ![](https://img.shields.io/badge/FIXED-2ea44f?style=flat-square) **Subtitle Overlay Presentation Clock.** Integrated `pp_product_overlay_enter()`/`leave()` into subtitle picker lifecycle to eliminate 4K decode stalls and frame drop bursts upon dismissal.
- ![](https://img.shields.io/badge/IMPROVED-ff8c00?style=flat-square) **CPU SIMD Vectorized YUV Pipeline.** 8-wide AVX2 SIMD workgroups for high-throughput video presentation (7.4ms per 4K frame).

---

## 0.6.0

**Direct Emby streaming, on-screen virtual keyboard, and a dedicated Surround Sound Studio for 5.1 and 7.1 speaker calibration.**

Download **`EVOPlayer-0.6.0-InstallTile.elf`** to register the Media tile on the home screen, or **`EVOPlayer-0.6.0-player-only.elf`** for homebrew app slot launch.

### New — Emby Media Server Add-on

- **Native Emby LAN streaming & browsing.** Connect directly to your local Emby server, browse media libraries with dynamic cover art and backdrops, and direct-stream video files without transcode overhead.
- **On-screen Virtual Keyboard.** Full controller-driven keyboard with character, numeric, and symbol sets for convenient entering of server IP addresses, ports, usernames, and passwords.
- **Playstate reporting.** Real-time playback progress and resume position synchronization back to Emby server.
- **Custom Emby branding.** Dedicated section icon and navigation rail entry.

### New — Surround Sound Studio

- **360° Top-Down Soundstage Theater.** Bespoke visual calibration interface featuring a top-down view of listener sweet spot, concentric acoustic range rings, front display soundbar, and 8 positional speaker modules (`FL`, `FC`, `FR`, `LFE`, `SL`, `SR`, `BL`, `BR`).
- **PS5 8-Channel Hardware Audio Output (`S16_8CH`).** Verified hardware tone generator with smooth 50ms attack/decay envelope fades to prevent audio clicking.
- **Sequential 5.1 & 7.1 Auto-Calibration.** Automated channel sweep across each speaker with live frequency and status monitoring.
- **360° Perimeter Rotation Sweep.** Continuous circular pan around the room perimeter to verify surround transitions.
- **Dynamic 5.1 Speaker Hiding & 2D D-Pad Room Traversal.** Seamlessly hides inactive side speakers in 5.1 mode and adjusts D-pad traversal grid to navigate directly between active physical channels.

### New & Improved — Settings & Subtitles

- **Structured Settings Hierarchy.** Organized preferences into dedicated sub-sections: *Playback & Video*, *Subtitles*, *Interface & Controls*, *System & Hardware*, and *Developer Tools*.
- **Default Subtitle Sizing.** Configurable and persistent default subtitle size preference (`SMALL`, `MEDIUM`, `LARGE`) with improved base scaling for high-resolution displays.

---

## 0.5.0

**If a previous release did nothing when you ran it, this is the fix.** The
release shipped the bare player, which needs `hbldr` to be given a display
plane — sent straight to elfldr by a payload manager it started headless and
never drew anything. Download **`EVOPlayer-0.5.0-InstallTile.elf`** instead: it
registers the Media tile and carries the player inside it.

Also: a text reader, and a font that can finally spell a comma.

### Fixed — install

- **The release now ships the ELF that works from a payload manager.**
  `EVOPlayer.elf` is the bare player and needs `hbldr` for a display plane;
  elfldr alone runs it headless, so nothing appears. The old release notes
  documented this in a footnote under a step pointing at that very file.
  The tile launcher already carried its own copy of websrv's `hbldr` and works
  from elfldr alone — it just was never built or shipped. Verified on 12.70 on
  a console where **only** elfldr was reachable.
- **The bare player is renamed `-player-only.elf`** so it cannot be picked by
  mistake, and the release notes now say which file is which.
- **The tile launcher announced the wrong version** — a hardcoded `0.2.0` that
  had drifted two releases behind the player it embeds. It now takes the
  version from the same file CI checks against the release tag.
- **CI verified only one of the three payloads.** It now checks all of them,
  and asserts the launcher is larger than the player it embeds — a launcher
  that suddenly shrinks means the `.incbin` picked up nothing, which would
  ship a tile that opens to a black screen and pass every other check.

### New

- **A text reader.** Opening a `.txt`, `.log`, `.md`, `.nfo`, `.json`, `.csv`,
  `.ini`/`.cfg` or a subtitle file from the browser now reads it instead of
  doing nothing — those extensions were listed and selectable and CROSS on
  them was silently ignored, which read as the browser being broken.

  D-pad scrolls a line, the shoulders page (with one line of overlap, so a
  paragraph keeps a landmark), the triggers jump a tenth of the file, and
  TRIANGLE cycles three reading sizes. Changing size re-wraps and keeps the
  top of the view on the same words, because losing your place is the one
  thing a size control must not do. The scrollbar thumb is sized to how much
  of the document is on screen, which is what tells you whether "half way"
  means five more pages or five hundred.

  Text is decoded as UTF-8 and folded to what the font can draw: curly quotes
  become straight ones, em dashes hyphens, ellipses three periods, accented
  Latin letters their base letter. A file that is not valid UTF-8 still opens.
  Reads at most 2 MB and says so rather than appearing to end early; refuses
  binaries instead of showing a screen of dots.

- **The UI font has punctuation.** It had 69 glyphs — letters, digits and
  `/._:-+` — and drew everything else as a 12px gap. Survivable in a filename,
  unreadable in prose, and a text reader is nothing but prose. 26 glyphs
  (`,` `'` `"` `!` `?` `(` `)` `;` `[` `]` `{` `}` `<` `>` `=` `*` `#` `|` `\`
  `~` `^` `` ` `` `%` `&` `@` `$`) are now generated by `tools/gen_icons.py` at
  all four sizes.

  The original atlas is a pre-generated asset with no generator in the tree, so
  these live in a second atlas that the text routines fall back to. They sit on
  the letters' baseline because `tools/measure_font.py` reads the real baseline,
  cap height and stroke weight out of the atlas ink rather than guessing, and
  their advances are measured from the rasterised glyph rather than declared —
  the first pass declared them and `$4.50 (approx` came out with the bracket
  touching the zero.

### Changed

- **Glyph lookup is shared between the player and `tools/uiview.c`.** Both
  carried their own `rr_idx` scan and their own four-way face switch; with two
  atlases to choose between that would have been two more places to drift.
  Both now call `evo_font.h`.
- **The browser labels a file "DOCUMENT" if the reader will open it.** The two
  lists had drifted the moment the reader learned new extensions, so a `.json`
  was labelled "FILE" right up until you pressed CROSS on it and it opened.

### Documentation

- **The README is about the player again.** It had grown into a twenty-section
  setup manual — Windows prerequisites, Docker, LLVM version rationale, FFmpeg
  profiles, a validation checklist — with the player itself covered in the
  first forty lines. Someone who wants to *use* EVO Player needed none of it.
  707 lines to 175: what it is, how to install it, what it does, the controls.
  Nothing was deleted; the developer guide moved intact to
  [docs/building.md](docs/building.md).
- **[docs/architecture.md](docs/architecture.md) is new** — the layers inside
  the player, why `main.c` is carved in the order it is (with the measurement
  that argues against the obvious first cut), the draw vtable, the two font
  atlases, and what can be verified without a console.
- **Eight screenshots**, all real `uiview` renders rather than mock-ups.
- **`EVO_TEXT_CHARSET` is generated**, not hand-maintained. It listed 69
  characters and would have gone on reporting a comma as unsupported long
  after the comma was added; `tools/gen_icons.py` now emits the punctuation
  alphabet into a header small enough for the UI layer to include.
- The font warnings in [docs/ui-handoff.md](docs/ui-handoff.md) said prose
  could not be written and a prompt "cannot ask a question mark's worth of
  question". Both were true and are not any more.

---

## 0.4.0

Playback does less work per frame, and the code that does it lives in fewer
places.

The measurable part: the 4K converter stopped computing pixel addresses in
floating point, the tiling path that runs on every 1080p frame stopped
creating twelve threads to do it, and the player stopped blacking two million
pixels it was about to overwrite. The structural part: 87.7 MB of assets
nothing included are gone, icons draw from a generated table instead of three
hand-written switches, and the first module has left `main.c`.

Interface work too — CIRCLE asks before it stops playback, and cover art is
large enough to read as cover art.

### Performance

Every change here is verified on the host by `./tools/bench.sh`, which hashes
the output plane and refuses to print timings if the pixels moved. Host
timings are not the console's; they compare changes against each other.

- **The 4K swizzle stopped doing a `double` divide per pixel.** The fused
  converter called `pp_tiled_pixel_offset` four times per 2×2 block, and each
  call evaluated a double divide and multiply purely to compute an address —
  8.3M of them per frame at 2160p. It now walks whole tile spans, so the tile
  base is computed once per 512 pixels. `tile_copy.c` had already made this
  move for the linear path and carries the proof that the integer form is
  identical. 4K single-thread **38.2 ms → 20.9 ms**; at 4 workers 12.4 → 10.4;
  1080p at 4 workers 3.0 → 2.1. Plane hashes unchanged.
- **The tiling path that actually runs stopped spawning 12 threads a frame.**
  `pp_converter_parallel.c` names per-frame `pthread_create`/`join` as the
  Soft-UHD freeze root cause, and both converters were rewritten around
  persistent pools because of it. `pp_draw_pixels_as_tiles` was missed — and it
  is not dormant: `pp_videoout_present` calls it for every 1080p frame, so at
  60fps it asked the scheduler for 720 thread creations a second on the render
  thread. (The dormant one is the similarly named `PS5_DrawPixelsAsTiles` in
  `main.c`; they are different functions.) Now on a pool: **2.00 ms → 1.68 ms**
  on the host, and the console is where the stalls were actually observed.
- **The player no longer blacks 2M pixels it is about to overwrite.**
  `draw_player_screen` cleared the whole frame and then had every pixel of it
  replaced by the video on the next line. The clear could not simply be
  deleted — it is load-bearing when no frame is ready, and the render thread
  cannot ask "is one ready?" first, because the seek thread can retire the
  display between the question and the answer. So the guarantee moved down:
  `pp_playback_copy_display` now leaves every pixel of its target defined,
  decided under the lock that owns the display. Worth 0.28 ms of a 2.49 ms
  render thread — real, and smaller than it was billed as.
- **The full-frame display copy is one `memcpy`** rather than 1080 row-sized
  ones, which is what it had always been in the case that runs every frame.
- **Glyph lookup is a table**, not a linear scan of the 69-character
  `RR_CHARS` run once per character drawn — and the miss case, which is most
  punctuation in a filename, paid all 69 comparisons. There were two hand-
  copied implementations of that scan; there is now one function.

### Changed

- **The thumbnail worker is its own translation unit.** `main.c` was ~18k
  lines in one file, and the received wisdom was to pull decode/demux out
  first. Measuring the file says otherwise: half its 263 file-scope globals
  are referenced across spans of more than 5,000 lines, and the *most* smeared
  of them are the playback ones — `g_pp_pb` is touched across 17,899 lines,
  `player_paused` across 17,311. Cutting there first would publish sixty
  globals through a header and turn a tangle into a tangle with an API.

  The scrub-preview thumbnail worker measured as the real seam: 1,096 lines,
  39 symbols, and a total dependency on the rest of the program of two helpers
  and two screen constants. It owns its thread, mutex, condition variable and
  request queue, so the boundary already existed — it just wasn't a file. It
  is now `media/src/prospero_thumbnail.c` behind a six-function header.
  `main.c` is 16,815 lines and 199 file-scope statics, from 17,985 and 229.

  Nothing about the code changed: the move is checked mechanically against the
  original region, and differs only by the four substitutions its header
  documents.
- **Source-over pixel blending is in one header.** `main.c` carried two copies
  of it, the second commented "duplicated rather than shared … because that one
  is defined ~3000 lines further down" — a workaround for declaration order
  inside a single translation unit, which is exactly the problem a header does
  not have. Both are now `evo_blend_pixel()` in `ui/include/evo_blend.h`, with
  identical arithmetic to what they both already did.
- **Icon drawing goes through a generated table.** `tools/gen_icons.py` now
  emits `EVO_ICON_TABLE` / `EVO_CTRL_TABLE`, and both the player and
  `tools/uiview.c` index them. There were *three* hand-written `switch (idx)`
  copies naming the same macros — and one had already drifted: `rr_icon()` in
  main.c stopped at case 12, so it drew nothing for HOME or LOGO. It is
  reachable through `EVO_DRAW_VTABLE`, so that was a defect waiting for its
  first caller rather than a live one. Adding an icon is now one edit to the
  generator; it can no longer land in the mock and be forgotten in the player.
- **87.7 MB of unreferenced assets are gone** — `approved_raw_assets.h`,
  `approved_ui_assets.h`, `pp_ui_assets.h`, `pp2_ui_assets.h`,
  `pp_font_metrics.h` and three `.bmp`s. Nothing included any of them; the
  asset directory went from 117 MB to 30 MB. `pp_font_metrics.h` going with
  them settles the "two font systems exist" warning in the handoff doc — there
  was one, plus a header nothing read.

### New

- **CIRCLE during playback asks before it stops.** It was the one
  irreversible thing CIRCLE did anywhere in the app — tearing down the
  decoder, the audio port and your position in the file — on a single press
  of the button that means "back" everywhere else. It now opens a prompt over
  the paused frame: CIRCLE (the button that opened it) keeps watching, CROSS
  stops. The action order is deliberate: the destructive answer is not the one
  you reach by repeating what you just did.
- **Cover art is large enough to be cover art.** The cache moved from 80×80 to
  320×180 and the recent shelf draws it full bleed instead of as an inset
  thumbnail, so tiles are posters. 320×180 is 16:9 — the shape the source
  frames actually are — and it *minifies* into the tile's 270×162 drawing
  area rather than magnifying, which is what the old 3.4× upscale could never
  do. The cache is 16 slots rather than 48: only the eight recent tiles ever
  ask for one, and the trade is 3.7 MB against 1.2 MB.
- **The application mark is the EVO logo.** The top-left of the rail and of
  the launch header drew two concentric circles standing in for a logo. Both
  now draw `EVO_ICON_LOGO` — the ring-and-play mark from the home-screen icon,
  generated by `tools/gen_icons.py` as one monochrome glyph so it tints with
  the theme like every other icon.
- **Sound and lightbar survive a relaunch.** Appended to the settings file
  after the theme name, and written when the Tools rows are toggled.

### Fixed

- **Tools' DEBUG OVERLAY toggle did not save**, though Settings' DEVELOPER
  MODE row — which flips the same variable — always had. Which page you used
  decided whether it stuck.
- **Full-bleed tile artwork is cropped, not stretched.** Scaling a 16:9 cover
  into a 5:3 tile is a ~7% horizontal squash: invisible on a gradient,
  obvious on a face. `evo_widget_tile` now takes the centred source rectangle
  that fills the tile, the same way the hero backdrop already did.
- **LEFT no longer opens the navigation rail over the subtitle picker.**
  Neither it nor the new stop prompt was listed in `evo_screen_is_modal()`, so
  the rail could open as a third layer over a panel floating over video.
- **The 4K-surface dance around a modal overlay is written once.** Media Info
  and the stop prompt both have to leave a 4K plane and hold the presentation
  clock; that was ~25 lines transcribed by hand, and a second copy is exactly
  how a 4K session comes back from an overlay stuck in 1080. Both go through
  `pp_product_overlay_enter()` / `_leave()` now.

### Tooling

- `tools/uiplay.sh` covers the new prompt (`M` cycles to it), and the fixture
  artwork is 320×180 with a ring drawn in it — a gradient could not show
  whether a cover had been squashed, and 128×72 magnified into everything it
  was drawn on. The hero gets its own 960×540 fixture, matching
  `EVO_HERO_ART_W/_H`, so the mock stops rendering stair-stepping the console
  never produces.

---

## 0.3.0

The player starts from the console now, and tells you what changed.

### New

- **A home-screen tile.** EVO Player registers a Media-category title and
  appears in the console's own Media row, so it starts from the controller.
  Until now launching meant opening a browser on a second device to reach the
  websrv launcher. The tile is title `EVOP10001` on loopback port 9056,
  deliberately distinct from ProsperoPlayer's `PRSP10001`/9055 so both can be
  installed on the same console; nothing here touches upstream's registration.
  Build and install with `./scripts/build-media-tile.sh --install`.
- **The uninstaller ships with it, and is built first.** Sony usually hides
  Options → Delete for Media hosts, so removal is `--uninstall`, or the
  one-shot `EVOPlayer_UninstallTile.elf` when the launcher is not resident.
  It is built before the launcher, unconditionally, so the way back exists
  before it is needed.
- **A changelog in the app**, under About. Release notes per version with the
  emphasis inverted the way the other informational rows work — the change is
  what you came to read, so `NEW` / `FIXED` steps back.
- **A real application icon**, generated by `tools/gen_app_icon.py` from vector
  shapes using the same signed-distance rasteriser as the UI icons. The
  repository previously had no EVO icon at all, so packaging fell back to a
  1×1 placeholder and the Media launcher carried upstream's branded artwork.
- **`./scripts/update-console.sh`** — rebuilds and updates both install paths
  in one command.

### Fixed

- **The Media launcher's Makefile could never have worked.** It depended on
  `assets/ProsperoPlayer.elf` while the source embeds `assets/EVOPlayer.elf`,
  so `make` failed on a missing prerequisite. A missing player now explains
  itself instead of surfacing as an assembler error about a file it cannot
  open.
- **The tile and the development install no longer fight over one directory.**
  The launcher embeds the player and rewrites it on every launch; with both
  pointing at `/data/homebrew/EVOPlayer` the tile silently overwrote whatever
  `install-homebrew.sh` had just pushed, with nothing to say which binary had
  actually started. The tile's copy lives at `/data/evoplayer/app`, and
  uninstalling the tile no longer deletes the development install.
- **`uiview` reported version 0.0.2** for every render, because the version was
  hardcoded in `tools/uiview.sh` rather than read from `VERSION`. Its About
  fixture had also drifted from the real screen.
- **Installing the tile never returned.** `prospero-deploy` is
  `socat -t 9999999`, which waits for the far end to close, and elfldr does not
  close while the payload it spawned is alive. The launcher is resident by
  design — that is what serves the tile's deeplink — so the deploy ran until it
  was killed, long after the payload was up. Console deploys are bounded now
  (`--timeout`, default 90s), and `timeout`'s exit 124 is reported as what it
  actually means: still resident, detached.

### Known gaps

- The launcher is a payload, so it must be re-injected after every jailbreak;
  the tile stays registered but does nothing without it. Autoload is the
  answer, and is the exploit host's job rather than this repository's.
- The tile embeds the player whole, so the launcher payload is player-sized
  (~34 MB) and updating the tile means re-injecting all of it.
- `PP_VERSION` in the launcher is a separate constant from `VERSION`.

---

## 0.2.0

Subtitles, and the interface for choosing them.

### New

- **Subtitle track picker.** DOWN during playback lists every track in the
  file with the number of cues it declares, the active one marked, and the
  film still playing behind it. It replaces cycling, which reopened the file
  on every step - on a disc rip with thirty-four tracks that meant thirty-odd
  reopens to reach the one you wanted.
- Track names are built from the language code, not the container's title.
  The font atlas has no accents or parentheses, so a track really titled
  `Español (España)` would draw as a row of holes; fifty language codes map
  to ASCII names, and same-language tracks are numbered rather than shown as
  duplicate rows.

### Fixed

- **Subtitles appeared to be broken and were not.** A release group ships a
  vanity track tagged English, flagged default, holding two cues whose first
  lands thirty-seven minutes in. Selection picked it on metadata alone and
  then correctly displayed nothing for a whole episode. mkvmerge records
  `NUMBER_OF_FRAMES` per track, so the cue count is readable before a packet
  is demuxed, and it now outranks every other signal: a track with fewer than
  ten cues loses even when it is English and flagged default. Tracks that
  lose this way are still offered in the picker, dimmed and marked
  `SIGNS ONLY`, because the count can itself be wrong.
- **The marquee scrolled at whatever speed the render loop happened to be
  running.** It advanced a fixed number of pixels per frame, and the loop
  runs anywhere from 36fps with a preview decoding to 60fps on a settled
  list, so one filename scrolled at two visibly different rates. It measures
  milliseconds now and travels 180px/s regardless.
- **The marquee also stepped a glyph at a time**, because the offset was
  computed in pixels and then applied by dropping whole characters. The
  sub-character remainder is applied as a negative x, with the glyph
  overhanging each end clipped by keeping and restoring the strips either
  side. Checked on the host: no ink outside the box across 376 phases,
  largest step 3px against a 17px advance.
- **`EXTRA_CFLAGS` never reached the compiler** when building from Windows.
  The re-exec into the dev container forwarded `PS5_HOST` and `PS5_PORT` and
  dropped everything else, so a `-D` switch produced a successful build, a
  clean install, and a binary without it. Forwarded now, and the build greps
  its own compile line and fails if a requested flag did not land.

### Known gaps

- Cue counts come from mkvmerge's statistics tags. Containers written by
  other tools do not carry them; those tracks are ranked on metadata as
  before and show no count in the picker.
- Switching tracks reopens the file and seeks back, so it costs the same
  pause as a seek. Cues are collected as packets stream past, so a track
  selected mid-film has no cues from earlier in it.

---

## 0.1.0

A rebuild of the interface, and the tooling to work on it without a console.

### New

- **Launch screen.** A hero that resumes what you were last watching, a
  *Jump back in* shelf, and a *Library* shelf. Two-dimensional cursor: each
  shelf remembers its own column, and empty shelves are skipped rather than
  becoming dead stops.
- **File browser inspector.** Selecting a file shows a frame from it plus
  type, container, size, length, resolution and codecs, read from the
  container itself. Probing is debounced, so scrolling never stalls.
- **Side navigation rail.** Sections are reachable from each other instead of
  each being a dead end you had to back out of. Back is a stack now, so a
  screen opened from two places returns to the right one.
- **Hold to scroll.** Every list moved one item per physical press before.
  Shoulder buttons page; L2/R2 jump to the next initial in the browser.
- **Themed toast**, and a `danger` theme token for failure states. Existing
  `.theme` files inherit it.
- **Playback OSD follows the theme** — panel, seek bar, scrubber, chapter
  marks, captions and the music visualiser.
- **Theme swatches** on the settings row, so cycling themes is not blind.
- **L3 captures a screenshot during playback** (R3 keeps subtitle delay).

### Fixed

- The eighth settings row was visible but unreachable — navigation wrapped at
  a hardcoded count that had drifted from the real one. That whole class of
  bug is gone; counts now live with the cursor.
- **Audio failures were all blamed on E-AC3**, including files with no audio
  track at all. The message was hardcoded and overwrote the accurate one.
  E-AC3, AC-3, DTS, TrueHD, FLAC, Opus and ALAC are all present and linked.
- **Pixelated previews** — thumbnails were point-sampled twice, once down and
  once up. Minification now box-filters and the preview is presented 1:1.
- The 4K converter created and joined worker threads **every frame**, the
  pattern already documented here as causing a freeze. It uses a persistent
  pool: 4K conversion measured 11.57 ms → 9.15 ms per frame on the host.
- Commas and parentheses rendered as gaps: the font atlas has neither, and an
  unknown glyph leaves a hole rather than being skipped.
- Scrims painted as hard black slabs — `evo_ui_vgrad` replaces rather than
  blends, so a transparent colour was written straight into the framebuffer.
- The expanded rail was not quite opaque, letting the page title ghost
  through it.

### Removed

- **Haptics.** Built, tested on hardware, and taken out: every vibration
  entry point in `libScePad` either reports success and does nothing or
  rejects the call, while `scePadSetLightBar` succeeds on the same handle.
  Sound and lightbar remain. Evidence is in `docs/ui-handoff.md` so nobody
  re-derives it.

### Tooling

- `tools/uiplay.sh` — the UI as a navigable page in a browser, on any
  machine. Real renderer, real font atlas, real icons.
- `tools/uiview.sh` — render any screen to a PNG.
- `tools/shot.sh` — fetch captures and interrogate them numerically: probe a
  coordinate, scan pixel runs, crop, diff.
- `tools/klog.sh` — console log, timestamped, append-only, survives payload
  restarts.
- `tools/launch.sh` — refuses to stack app instances, which is what
  kernel-panicked a console during development.
- `tools/bench.sh` — host benchmark for the converter, with correctness
  hashing and ASan/TSan modes.

### Known gaps

- The player OSD is themed but still draws its own layout rather than the
  shared chrome.
- `gpu-notes.md` previously recommended an SDL2/mesa path for GPU YUV
  conversion. That does not work: the sysroot ships OSMesa (llvmpipe), a
  software rasteriser, with no `radeonsi`. The document now records the
  measurements.

---

## 0.0.2

Plug-and-play theming, SDF-drawn cards, generated vector icons, navigation
sounds. Four built-in themes plus `.theme` files from USB.

## 0.0.1

First release of the EVO Player fork: 7.1 surround output with stereo
fallback, flip-synchronised presentation, a faster tile swizzle, and
folders-first browsing.
