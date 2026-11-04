# Diagnostics

Where to look when something goes wrong, and the switches that change EVO's
behaviour for a test.

## The log: `/mnt/usb0/evo.log`

One file holds everything: the boot trace, breadcrumbs, decoder notes, the
per-file `sweep` stats, updater, provider and network lines, and a `CRASH` line
with the fault address when the process dies. Each line is timestamped, append-only
and written straight through, so the last line before a crash survives; keep
anything emitted per frame rare. Lines logged before `/mnt/usb0` is reachable are
buffered and flushed right after the self-unjail. Credentials are redacted before a
line enters the log.

```bash
./tools/evo-remote.sh log                 # pull it to output/logs/evo.log
./tools/evo-remote.sh watch 60            # stream status + new lines
curl -N http://<ps5>:9780/raw             # live, no FTP (default port 9780)
curl "http://<ps5>:9780/raw?grep=hui&level=warn"
```

- Every `deploy-app.sh --ffpfsc` deletes `evo.log`, `evo_status` and
  `evo_report.txt`, so each launch starts clean.
- **A static log proves nothing about whether EVO exited.** The idle heartbeat is
  about six minutes apart.
- Check the build id on the first lines against the build you meant to test.
- **klog** carries the boot lines (`evo_bt`) live through `sceKernelDebugOutText`,
  and is the only channel that shows a launch dying before the sandbox opens.
  Runtime lines are in `evo.log` and on `:9780` only.
  `./tools/klog.sh` follows and records (`--quiet`, `--grep evo`, `--once`,
  `--tail 200`, `--sessions`) to `output/logs/klog/klog-<UTC>.log` and an
  append-only `klog-all.log`; it reconnects when the payload restarts and logs the
  reconnect inline. Needs `ps5-payload-klogsrv` on 3232. Payload `stdout` is not in
  klog and the app module has none at all.
- On-screen notification popups of each line are off since #51; `package-app.sh
  --breadcrumbs` brings them back.
- Identical lines in a row are written once, then
  `log: last line repeated N more times over M ms` when a different line comes or
  the repeats stop for 2 s (at most a minute later).

### Writing a log line

Every line is `[seconds] LEVEL tag: text`. The live viewer colours and filters on
the level and treats the first `word:` as the tag (`tag:net` in its filter box).

| Use | For |
|---|---|
| `EVO_LOGI/W/E("tag", fmt, ...)` (`evo_boot_log.h`) | anything at runtime. `E` = a failure the user sees (server unreachable, login refused, file unreadable), `W` = one EVO works around (fallback, skip, retry), `I` = the rest. No klog write, no `EVO boot:` prefix |
| `EVO_LOGW_EVERY(ms, "tag", fmt, ...)` (also `I`/`E`) | a failure that can repeat fast (reconnect loop, per-packet error): at most one line per `ms` from that call site, with the count it dropped |
| `PROV_LOG/WARN/ERR(fmt, ...)` (`evo_provider_log.h`) | provider and network C code; start the text with its own tag (`"iptv: ..."`, `"net: ..."`). Prints to stderr on the host |
| `evo_bt(...)` (`evo_boot_trace.h`) | boot only (before the frame loop, the jailbreak, probes): it also writes klog |

Say what failed and why: the operation, the host or path, and the HTTP status,
FTP reply, `errno` or SSL error. Never put a password, token or query string in
the text; `evo_log_redact` masks `key=value` secrets, not a key inside a URL path.
Nothing per frame. `evo_log_mono_ms()` gives the time for a "took N ms" line. The
`EVO_LOG*` macros compile to nothing in host builds (tests, uiview), but the
format is still checked.

### Lines worth knowing

| Prefix | Content |
|---|---|
| `mem budget [<when>]` | the kernel's view: direct total and largest free run, flexible configured and free, EVO's own pool. Logged at boot and at each software decoder open |
| `alloc [<when>]` | the malloc shim: `live`/`peak`, **`map_fail`** (backing maps refused: nonzero is a real out-of-memory, so check it before blaming a codec), `flex_avail`, `direct=`/`direct_live=`/`direct_peak=` for heap blocks served from direct memory. Logged at `decode-open`, every 2 s of playback (`play`, the line that survives a crash) and at `stop` |
| `agc health` / `agc pipe` / `agc color_target` | the GPU runtime ([../hardware/agc-runtime.md](../hardware/agc-runtime.md#verifying-a-build)) |
| `dns:` | one line per real lookup: the host, the addresses, the console-DNS timing |
| `sweep v=1 ...` | one line per played file, when its decoder closes |
| `updater:` | the in-app updater |
| `net:` | every failed HTTP request (method, path, the reason in the words the user sees, time taken), plus DNS, connect and TLS failures with their cause. Successful page requests are INFO |
| `ftp:` / `netstore:` / `browser:` | FTP commands that failed (with the reply), SMB errors, share config load/save, and each folder listing or connection test with its time |
| `jellyfin:` `emby:` `xtream:` `iptv:` `stremio:` `nuvio:` `plex:` `bundle:` | provider sign-in, catalog and stream failures |
| `ffmpeg:` | FFmpeg's own warnings and errors, at its own level |
| `hui:` | the kit screens starting, or switched off |
| `CRASH signal=... addr=...` | written by the crash handler with raw I/O (async-signal-safe) |

## Live endpoints (default `:9780`)

`src/evo_log_server.c`, app module only, at most a few clients at once; a slow
client never holds up the logger.

The port defaults to 9780 and is user-settable in Settings → Interface & Storage →
`PHONE & LOG PORT` (digits 1024–65535, Triangle resets to 9780). EVO test-binds the
chosen port; if busy at startup, it falls back to the next nine ports (n+1..n+9) and
keeps retrying the saved port in the background. The real bound port feeds the live
log server, the phone keyboard URL and the QR code. If all 10 ports are busy, the
server is disabled and the keyboard shows `PHONE LINK OFF` with a "Set port" button
(R3). Dev tools (`tools/evo-remote.sh logs`, `tools/evo-dash.py`) default to 9780
and honour `PS5_LOG_PORT` from the environment.

| Path | Returns |
|---|---|
| `/raw` | plain text, kept open |
| `/stream` | Server-Sent Events, one per line |
| `/log` (and `/` when no phone keyboard is open) | the log viewer for a phone or PC, in the active theme's colours and the kit's fonts. **Now**: live cards from `/stats` (playing, picture, loaded ahead, network against the video's bitrate, memory) and plain-words alerts. **What happened**: the session as a story, one card per video with a timeline of pauses and errors, quiet rows for everything else, and the run before this one (**Last run**). **Log**: every line, search, levels, tags, routine lines hidden. `diag:` lines (the diagnostic engine, #234) show as findings in both. Page: `assets/web/log.html`, embedded with the bundle; preview it on a PC with `tools/logview_preview.py --log <evo.log> --demo` |
| `/info` | `{"version","build","model","up_s","last_run","theme"}` for the viewer; `last_run` is `crashed`, `closed`, `ended` or `none`, `theme` the active colours as `#RRGGBBAA` |
| `/session.txt` | the end of `evo.log` from disk (as much as a report carries, redacted again), or with `?run=last` the saved run before this one. The viewer builds its story from it and then follows `/stream` |
| `/web/<file>` | the viewer's files: `log.html` and the kit's fonts as Latin-only WOFF2 |
| `/snapshot` | the last ~128 KiB, then closes |
| `/report` | the phone page for a bug report (#229): Download / Share / Copy, and the last warnings and errors. Settings → System & Diagnostics → Report a Problem shows its QR |
| `/report.txt` | that report as a download: version, build, model, `/stats`, the settings file and the last 1.5 MB of `evo.log` from disk (with `evo.log.1` if there is room), every line redacted again. The network-shares file (it has passwords) is never in it |
| `/report.txt?run=last` | the report of the run before this one, from `evo.prev.log` (`/report?run=last` is its page). At start, before the new session header, EVO copies the last session of `evo.log` (up to 1.5 MB) to `evo.prev.log` next to it and reads how it ended: `CRASH signal=` = crashed, `soft close: parked` = closed from EVO, neither = ended without closing (PS button, kill). `src/evo_prev_run.c` |
| `/stats` | one JSON snapshot of memory pools, read-ahead, network reader and playback (`src/evo_stats.c`) |
| `/input`, `/input/state`, `POST /input` | the phone page for typing into an open EVO keyboard (needs the `X-EVO-Input` header; refused unless a phone keyboard is open) |

Query options on `/raw`, `/stream`, `/log`: `tail=0` (only new lines), `grep=<text>`
(case-sensitive), `level=warn|error`. Unknown paths redirect to `/input`.

### The dashboard

`python tools/evo-dash.py` (host, plain Python) polls `/stats` once a second and tails
`/raw`, then shows in plain words whether playback is healthy: the flexible pool
(the one that runs out), seconds of video loaded ahead, download speed against what
the video needs, and a "what happened" list (buffering, connection lost or back,
out-of-memory). Open `http://localhost:8790`; every sample is also written to
`output/dash/<date>.jsonl`. `--host` or `PS5_HOST` picks the console, and `PS5_LOG_PORT`
overrides the log port (default 9780).

## Screenshots

`key l3` (or R3) in a menu, or `evo-remote.sh key l3`, writes
`/mnt/usb0/evo_shot_NNN.bmp`, a raw framebuffer at the panel's resolution (3840×2160
on a 4K output; numbers wrap at 32). Fetch and look:

```bash
./tools/grab_shot.sh <name>     # presses l3, fetches the shot it wrote (USB0 or /data/evoplayer) -> output/screenshots/<name>.png
./tools/shot.sh grab            # newest capture via the web server (needs websrv on 8080)
./tools/shot.sh list            # what is on the stick
```

HDR10 scanouts are read back with the PQ curve applied (BT.2020 to BT.709, highlight
roll-off, sRGB), so a capture of HDR video looks like the picture, not noise.

The capture is raw pixels, so measure instead of squinting:

```bash
./tools/shot.sh probe 180 214               # colour at a coordinate
./tools/shot.sh probe 152 222 1704 92       # mean, distinct colours, top colours of a region
./tools/shot.sh scan row 260                # pixel runs along a line (an aliased edge steps, a smooth one ramps)
./tools/shot.sh crop 1228 222 560 315       # cut one region out at full resolution
./tools/shot.sh diff a.bmp b.bmp            # did that change do anything
```

The console's `/fs` route is read-only. Delete BMPs over FTP, or from the web
terminal; the stick fills over a long session.

## Switches

Files on the USB stick that change EVO's behaviour. They are read at launch (or
per use, where noted) and need no rebuild.

| File | Effect |
|---|---|
| `/mnt/usb0/evo_no_hui` | keep every screen on RmlUi, kit off |
| `/mnt/usb0/evo_no_sw_4k` | refuse software decode above 1080p (the default allows it since #95) |
| `/mnt/usb0/evo_dm_probe` | at boot, allocate, write and verify direct memory in 256 MB steps up to a ceiling in MB (default 8192), log a `dm probe:` line per step, release it. Delete the file to skip |
| `/mnt/usb0/evo_agc_no_nt` | put copies back on `memcpy` plus a cache flush instead of non-temporal stores |
| `/mnt/usb0/evo_dns_single` | turn the multi-record DNS lookup off |
| `/mnt/usb0/evo_web_probe` | the system-browser probe ([../providers/web-ui.md](../providers/web-ui.md)) |
| `/mnt/usb0/evo_interp_stage`, `evo_interp_debug` | a digit stops the motion-smoothing chain early, or replaces its output with a debug view ([../hardware/motion-smoothing.md](../hardware/motion-smoothing.md)) |
| `/mnt/usb0/evo_vdec_hang` | test hook: the 100th decode blocks forever, to exercise the watchdog |

Written by EVO: `evo_report.txt` (Settings → System & Diagnostics → Report a
Problem; on `/data/evoplayer` with no stick), and the `evo_status` / `evo_ui.json` / `evo_set.txt` of the
dev remote.

On the data root (`/data/evoplayer`), not the USB stick: `thumb_quarantine` lists
files that crashed the process during poster extraction and are skipped from then
on (`evo_crash_note.h`). If a file mysteriously has no poster, delete the entry (or
the file) to retry.

## Files EVO keeps

`/data/evoplayer/` once the sandbox is open (`/download0/evoplayer/` before that,
which does not survive a relaunch):

| File | Content |
|---|---|
| `evo_player_settings.cfg` | every setting ([../ui/settings.md](../ui/settings.md)) |
| `evo_recent.txt`, `evo_favorites.txt`, `ps5_media_resume.txt`, `evo_last_folder.cfg` | history |
| `evo_network_shares.json` | saved SMB (Samba/Windows) and FTP network shares |
| `<provider>.conf` (`emby`, `jellyfin`, `plex`, `iptv`, `xtream`, ...), `addons.json` | provider setup and sessions. Tokens are stored in plain text |
| `webui/<host>_<port>.json` | a web UI's saved page storage (Nuvio's addon list) |
| `providers/<id>/` | cached provider UI bundles |
| `themes/*.theme` | custom themes ([../ui/theming.md](../ui/theming.md)) |
| `fonts/` | optional CJK Noto fonts and the shipped extras |
| `evo_home_art.tsv`, `evo_plex_home_art.tsv`, `iptv_guides.map` | home artwork and guide caches |
| `evo_stats_hud.cfg` | the diagnostics panel's on/off state |

`tools/wipe-evo.py` removes all of it for a fresh install.
