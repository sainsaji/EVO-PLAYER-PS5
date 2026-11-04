# Settings

Reference for every user setting, how they are stored, and how to add one. The code is
`core/src/screens/SettingsScreen.cpp` (rows and sections), `core/src/services/SettingsService.cpp`
(values and the file) and `core/include/evo/Common.hpp` (the enums).

## The rows

Six sections, in sidebar order. A row is a **toggle** (Cross flips it), a **value**
(Cross expands it in place to show every choice, so nothing is cycled blind; Back out of
an open row keeps the cursor), or an **action**. Every row has a live preview card showing
its real effect (#158). A row can be disabled by another setting, and then says why.

| Section | Row | Choices |
|---|---|---|
| **Video & Display** | Default aspect ratio | Fit, Fill, Stretch |
| | Resume playback | on, off |
| | Skip intros & credits | Off, Ask, Auto. Off never offers a skip. Ask shows a Skip button during an intro or credits and Triangle presses it. Auto skips by itself once and says so. |
| | Video decoder | Auto, Hardware (`sceVideodec2`), Software (FFmpeg) ([../hardware/video-decode.md](../hardware/video-decode.md)) |
| | Upscaling | Off, Sharp (FSR 1), AI (Anime4K) ([../hardware/upscaler.md](../hardware/upscaler.md)) |
| | AI network | Auto, Standard, Large, Maximum (Pro). Only used when Upscaling is AI |
| | 120 Hz output | Off, Always, Playback only. Disabled when the display or HDMI sink lacks 120 Hz. The TV goes black briefly at each switch |
| | HDR output | Auto (HDR10 for HDR video), Off (tone-map to SDR) |
| | HDR peak | Off (the TV tone-maps), 800, 1000, 1500, 2000, 3000 nits. On a HDR10 output, fits Dolby Vision shot by shot from its L1 metadata and HDR10 from MaxCLL to that peak ([../hardware/video-decode.md](../hardware/video-decode.md#hdr-peak-mapping)) |
| | Network buffer | 30 s, 1 min, 2 min, 5 min. Sized when a network file opens, so it applies from the next file; high-bitrate files get less than you pick |
| **Audio** | Volume boost | Off, +50, +100, +200, +300 % ([../hardware/audio.md](../hardware/audio.md)) |
| | Audio delay | 0, in 50 ms steps from -500 to +500 ms |
| | Output channels | Auto, Stereo (downmix 5.1/7.1) |
| | Audio passthrough | off, on (Dolby and DTS bitstream to a receiver) |
| | Audio language | Auto, Italiano, English, Español, Français, Deutsch, Português, Japanese: the track a file opens with (Auto: the first decodable one) |
| | Surround sound test | action: opens Surround Studio ([surround-studio.md](surround-studio.md)) |
| | Navigation sounds | on, off |
| **Subtitles** | Auto subtitles | on, off (read when a file opens) |
| | Subtitle language | Auto (English), Italiano, English, Español, Français, Deutsch, Português, Japanese: the embedded track chosen, then online subtitles from Stremio addons ([../providers/media-servers.md](../providers/media-servers.md#online-subtitles)) |
| | Default font style | Standard, Rounded, Bold, Condensed |
| | Secondary subtitle position | Stacked above, Top of screen |
| | Secondary subtitle colour | Yellow, Cyan, White |
| **Interface & Storage** | Theme | MIDNIGHT, CARBON, EMBER, AURORA, and any `.theme` file ([theming.md](theming.md)) |
| | Controller lightbar | on, off (DualSense light follows the accent) |
| | Folders first | on, off |
| | Keyboard input | Native PS5 IME, Virtual keyboard |
| | Phone & log port | digits 1024–65535 (default 9780; Triangle resets). EVO test-binds it, falls back to n+1..n+9 if busy, retries saved in background. Real port feeds log server and phone keyboard. |
| | Ask which live stream | on, off ([../providers/architecture.md](../providers/architecture.md#the-stream-picker)) |
| **System & Diagnostics** | Report a problem | card: QR to the phone report page (`/report`); Cross saves `evo_report.txt` to USB0, or `/data/evoplayer` with no stick |
| | Report the last run | the run before this one (`evo.prev.log`): its detail says whether it crashed, closed, or ended without closing. QR to `/report?run=last`; Cross saves `evo_report_last_run.txt`. Home shows a toast once after a crash |
| | Debug overlay | on, off (live performance numbers) |
| | Developer tools | action: diagnostics and performance screen |
| | Console | shows the detected model (PS5 Pro, PS5, or not detected) |
| | Memory | shows Standard or Pro memory mode ([../hardware/memory-budget.md](../hardware/memory-budget.md)) |
| | Software update | action: check, download, verify, install ([../build/release.md](../build/release.md#in-app-update)) |
| | Quit EVO | action: releases everything; close from the switcher |
| **Experimental** (unfinished, default off) | Motion smoothing | Off, Low, High ([../hardware/motion-smoothing.md](../hardware/motion-smoothing.md)) |
| | Match video frame rate | off, on ([../hardware/refresh-rate-modes.md](../hardware/refresh-rate-modes.md)) |
| | Deep blacks | Off, Low, High (pulls raised shadows to true black on OLED) |

Defaults: the first listed choice, except Default font style (Rounded, the second) and Skip intros & credits (Ask, the second); Phone & log port defaults to 9780; toggles that default on are Resume playback, Auto subtitles, Navigation sounds, Controller lightbar, Folders first and Ask which live stream. Everything in Experimental, Upscaling, Audio passthrough and the Debug overlay defaults off. A fresh install uses the MIDNIGHT theme.

The row titles above are what the dev remote addresses: `evo-remote.sh set audio-delay
+100` uses the title lower-cased with dashes, `set --list` prints every name with its valid
values, and `focus <row>` opens the row's section ([../console/remote-control.md](../console/remote-control.md)).

## The file

`evo_player_settings.cfg` in the data root, **one value per line, by position**, written
and read with a single `fscanf`:

```
 1 legacy profile (0)        8 navigation sounds       15 AI network           22 motion smoothing
 2 resume playback           9 lightbar                16 120 Hz output        23 deep blacks
 3 default aspect           10 subtitle font           17 HDR output           24 audio passthrough
 4 auto subtitles           11 keyboard type           18 secondary position   25 match video rate
 5 debug overlay            12 video decoder           19 secondary colour     26 volume boost
 6 folders first            13 keyboard marker (2)     20 ask stream           27 network buffer
 7 theme (by name)          14 upscaling               21 output channels      28 audio delay (ms)
                                                                               29 phone & log port
                                                                               30 settings schema
                                                                               31 HDR peak
                                                                               32 audio language
                                                                               33 subtitle language
                                                                               34 play best addon stream
                                                                               35 skip intros & credits
```

A line is applied only if the file has that many (`readCount >= N`), so an older file
loads, keeps every earlier value and defaults the new one. Values are range-checked on
load. The theme is stored by **name**, not index, so a theme present on one boot and absent
on the next cannot shift the choice. Line 13 is a migration marker: files from builds whose
keyboard default was secretly the virtual keyboard are moved to the native IME once.

## Adding a setting

1. If it is an enum, add it to `Common.hpp`.
2. Add the getter and setter to `ISettingsService`, and the member (with its default),
   getter, setter and display name to `SettingsService`.
3. **Append** a line to `saveSettings()` and the matching `fscanf` field and
   `readCount >= N` clause to `loadSettings()`. Never insert or reorder: the file is
   positional.
4. In `SettingsScreen.cpp`: add a `SettingKey`, a `case` in `makeDef()` (title, one-line
   detail, icon, choices), the dispatch in `toggleSetting()` or `applyOption()`, the key in
   its section's array, and a `kPreviews` entry (the plain-words benefit and the preview
   kind). Titles and details are written in capitals, like the others.
5. If it needs a kit preview, draw it in `ui_kit/src/evo_hui_settings_preview.cpp` and add
   a fixture in `tools/hui_preview/settings_preview_fixtures.inc`.
6. **Extend the round-trip test** in `tests/test_settings.c`: the new value survives
   save and load, and a file lacking the newest line still loads with the default
   ([../build/validation.md](../build/validation.md#unit-tests)).
7. Add a hardware check in `tools/checks/` that sets the value with `evo-remote.sh set`,
   exercises the feature and reads the evidence back.
