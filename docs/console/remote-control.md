# The dev remote: `evo-remote.sh`

A build made with `package-app.sh --usb-remote` can be driven over FTP with no
controller and no rebuild between tests. EVO polls `/mnt/usb0/evo_cmd` for one-line
commands and writes `/mnt/usb0/evo_status` once a second and `/mnt/usb0/evo_ui.json`
whenever the UI changes. `tools/evo-remote.sh` is the host side. The code is
`src/evo_usb_remote.c`; release builds compile all of it out.

```bash
./tools/dx.sh ./tools/evo-remote.sh status       # ~0.2 s per call on the long-lived container
docker compose run --rm ps5-dev ./tools/evo-remote.sh key down cross
```

Batch several `key` presses into one container start; each start costs seconds. Every
call needs `PS5_HOST` in `.env` and the console's FTP server on 2121. The lifecycle
commands (`launch`, `quit`, `close`, `cycle`, `verify`, `kill`) are described in
[hardware-cycle.md](hardware-cycle.md).

## Commands

| Command | Does |
|---|---|
| `build [flags]` | package with `--usb-remote` and deploy the `.ffpfsc` |
| `launch`, `quit`, `close [--force]`, `kill`, `cycle`, `verify` | lifecycle, guarded ([hardware-cycle.md](hardware-cycle.md)) |
| `play <path>` | open a file from the start |
| `seek <s>` / `seek +<s>` / `seek -<s>` | absolute or relative seek |
| `stop` | end playback, back to the browser (also flushes the sweep line) |
| `key <button>...` | synthetic pad presses in order, then read back where focus landed (below) |
| `type [--submit] <text>` | fill the virtual keyboard in one shot; `--submit` presses DONE |
| `kbdone`, `kbclose` | press DONE on the virtual keyboard; cancel any open keyboard, the native IME included |
| `ui [--line]` | the UI state as text (below) |
| `go <name>` | jump to a screen by name (below) |
| `provider <id>` | open a provider's screen: `iptv xtream emby jellyfin plex stremio nuvio` |
| `screen <id>` | jump to a `ScreenId` number ([../architecture/core.md](../architecture/core.md#screens)) |
| `source <n>` | browser source: 0 USB, 1 Internal, 2 Favourites, 3 Recent |
| `image <path>`, `text <path>` | open a file in the image viewer or text reader |
| `set <setting> [value]`, `set --list`, `focus <row>` | read or write a setting, list them, open a row (below) |
| `dbcompare`, `upcompare [--crop x,y,w,h]` | capture one held frame under each Deep Blacks / upscaler mode |
| `sweep [--dir p] [--secs n] [--max n] [--skip s]...`, `report` | the codec sweep ([../build/validation.md](../build/validation.md#the-codec-sweep)) |
| `status` | print `evo_status` once |
| `log` | pull `/mnt/usb0/evo.log` to `output/logs/` |
| `logs [now] [problems|<text>]` | stream live logs from EVO's HTTP log server (honours `PS5_LOG_PORT`, default 9780) |
| `watch [s]` | stream `evo_status` and new `evo.log` lines |
| `clear` | delete `evo.log` and `evo_status` |

## `key`, and reading focus as text

Buttons: `up down left right cross circle square triangle l1 r1 l2 r2 l3 r3 options
touchpad touchpad_left touchpad_right`. `l3` (or `shot`) takes a screenshot.

```bash
./tools/evo-remote.sh key down down cross
# down     [focus] #rec-tile-0 "Best of the Best Recent File" (item 1/6, launch) on MainMenu(0)  (seq 3)
```

`key` sends each press, waits for `seq` in `evo_ui.json` to move (up to
`EVO_KEY_WAIT` seconds, default 2.5) and prints where focus came to rest.
`(unchanged)` means the press moved nothing: the end of a list, or a key the screen
ignores. `key --no-wait <b>` sends one press and does not read back.

`ui` prints the whole JSON plus that line; `ui --line` just the line:

| Field | Content |
|---|---|
| `screen` | the `ScreenId` number and name |
| `docs` | every visible RmlUi document |
| `focused` | `id`, `tag`, `class`, `text`, `doc`, `index`/`total`, `rect` (UI pixels, 3840×2160 on a 4K output) and `focusable`, the other items of the same list; icon-only items read as `[icon_home]` |
| `also_highlighted` | other highlighted elements, such as a remembered selection in a pane without the cursor |
| `modal` | `{kind: keyboard\|dialog, text}` or null; the keyboard wins over a dialog, a dialog over the rail |
| `toast` | its text or null |
| `native_player` | player screen only: `osd_visible`, `paused`, `active_overlay` (`scrub`, `stats`) |

Focus is found (`ui_rml/src/evo_rmlui_devstate.cpp`) from RmlUi's own focus
(provider screens), or from an element carrying a `focused`, `*-focused` or
`*-cursor` class (every native screen). **A new screen that highlights with inline
properties only is invisible to this**: give the highlighted element a `*-focused`
class even with no RCSS rule. Use `ui` and `key` to navigate and test; keep `key l3`
plus `tools/shot.sh grab` for pixels.

## `go`, `set`, `focus`

```bash
./tools/evo-remote.sh go settings-playback
./tools/evo-remote.sh set audio-delay +100      # writes, applies live, prints it back
./tools/evo-remote.sh set audio-delay           # prints the current value
./tools/evo-remote.sh set --list                # every name with its valid values
./tools/evo-remote.sh focus audio-delay         # opens the section, cursor on the row
```

- `go` names: `home browse internal favorites recent providers settings
  settings-playback settings-subtitles settings-interface settings-system
  settings-audio settings-experimental about changelog themes surround devtools`, a
  provider id, or a `ScreenId` number. It works from any screen, with no key walk.
- `set` names are the row title in lower case with dashes (`audio-delay`,
  `volume-boost`, `network-buffer`). The answer file is `/mnt/usb0/evo_set.txt`; a
  failure prints `ERR ...` and exits 1.
- **Typing text.** `type` needs Settings → Interface & Storage → `KEYBOARD INPUT` =
  `VIRTUAL KEYBOARD`: the native PS5 IME is a system dialog and nothing EVO injects
  reaches it. If the Square key opened it (a browser search does), `kbclose` cancels
  it. For system dialogs, use Remote Play ([remote-play.md](remote-play.md)).

## The status line

`evo-remote.sh status`:

```
build=<id> t=<s> scr=<n> be=<0|1> pos=<s> dur=<s> fps=<n> fatal=<0|1> eof=<0|1> active=<0|1> parked=<0|1>
```

`t=` advancing means EVO is running; `be` is the decoder backend in use (0 FFmpeg,
1 native); `fatal=1` is a decode failure; `parked=1` is written once as the soft
close parks, after which `t=` is frozen. The build id comes from `evo_build_id.h`;
a `+dirty` suffix means an uncommitted tree, and a run whose id does not match the
build you meant to test proves nothing.

## Desktop front ends

`python tools/evo-panel.py` is a Tkinter panel over the same scripts (package with
any flags, deploy, remote, log pull, preview, klog, screenshots) with streamed
output. `python tools/evo-dash.py` is the live health dashboard
([diagnostics.md](diagnostics.md#the-dashboard)).
