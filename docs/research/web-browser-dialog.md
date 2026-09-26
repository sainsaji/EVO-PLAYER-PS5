# Web-UI providers: the system browser in the app module (#101)

Started as a go/no-go spike on whether `PPSA99039` can drive the PS5's own
WebKit (`libSceWebBrowserDialog`). The answer was yes, so it became a feature:
**Emby and Jellyfin are providers whose UI is their own web client**, opened
beside EVO's nav rail, with Play rerouted to EVO's player. Stage 1 (offline
research) is in
[the issue comment](https://github.com/sainsaji/EVO-PLAYER-PS5/issues/101#issuecomment-5838945530).

## How it works

| Piece | Where |
|---|---|
| Dialog lifecycle, loopback reverse proxy, injected hook | `projects/evoplayer/src/evo_webui.c` (+ `include/evo_webui.h`) |
| `EVO_PROVIDER_CAP_WEBUI` + `web_ui_url` in the provider vtable | `addons/include/evo_provider.h` |
| Emby: address in `emby.conf` (host, port, `https`) | `addons/src/provider_emby.c` |
| Jellyfin: address in `jellyfin.conf` | `addons/src/provider_jellyfin.c` |
| The provider chooser on the rail slot, the web branch | `core/src/screens/ProviderHostScreen.cpp` |
| Starting playback of a handed-over stream, reopening the page after | `core/src/Application.cpp` (`webui_playback_pump`) |
| PRX import stub (six names), linked into every player build | `tools/native-app/stubs/prx/libSceWebBrowserDialog.syms` |

1. **Boot, pre-unjail:** `sceSysmoduleLoadModule(0xAB)` and
   `sceWebBrowserDialogInitialize`. `sceSysmoduleLoadModule` stops working
   after `evo_jailbreak_self()` (#31, #34), so this cannot wait until the
   dialog is needed.
2. **Open:** the provider screen calls `evo_webui_open("http[s]://host:port",
   "/web/index.html")`. EVO starts a reverse proxy on `127.0.0.1:8686` (a thread
   per connection) and opens the dialog there in Mode 2 (Custom) at
   `x=108 y=0 1812x1080`: right of the 108 px nav rail, with no browser chrome.
3. **Hook:** the proxy injects `<script src="/evo/hook.js">` right after
   `<head>` in the site's `index.html`. The hook catches the site's own player
   as it sets a media source: through the `src` setter, `setAttribute`,
   `<source>`, `.m3u8` XHR/fetch, and as a catch-all the capture-phase
   `loadstart`/`play` events. It sends the stream URL, the item title (from the
   site's `ApiClient`) and a return URL to `/evo/play`.
4. **Handoff:** EVO rewrites the URL from the proxy's origin to the real
   server, closes the dialog itself (`sceWebBrowserDialogClose` on a running
   dialog works), and plays the stream through `startPlaybackSource`. The stream
   URL carries the server's `api_key`, so FFmpeg needs no further auth.
5. **Return:** when playback ends, EVO reopens the page the user was on. That
   is the last page that was not the site's own player route (Emby `videoosd`,
   Jellyfin `#/video`); reopening the player route leaves an empty player.
6. **Exit:** the hook adds a **Back to EVO** button to every page, which calls
   `/evo/close`. The embedded layout switches the browser's own controls off,
   and the PS button is the close path that has panicked the console.

The proxy sends HTTP/1.0 with `Connection: close` and `Accept-Encoding:
identity` upstream, so a response is a plain byte stream that can be relayed
or rewritten. It rewrites absolute `Location:` redirects back to itself, and
de-chunks `index.html` if needed. Upstream can be HTTPS (OpenSSL, SNI, no
certificate verification, the same as `evo_net.c`) and a hostname
(`getaddrinfo`). The browser itself always talks plain HTTP to loopback.

## Hardware results (FW 12.70, 2026-09-26)

| # | Question | Result |
|---|---|---|
| 1 | Loads and opens from the app module? | **Yes.** Module load, `Initialize` (pre-unjail) and `Open` all return `0` |
| 2 | Anything refused? | **No.** `sceUserServiceGetInitialUser` gives a user `Open` accepts |
| 3 | Composites over the AGC plane? | **Yes**, cleanly, including in Custom mode beside the rail |
| 4 | Page → EVO handoff? | **Yes, through the loopback listener**, by `fetch()` (cross-origin, CORS) and by navigation. The library's own `CallbackInitParam` is **not usable**: a guessed PS4 layout is accepted, but the dialog never closes on the callback URL and `GetResult` refuses every callback-result buffer (`0x80b8000a`) |
| 5 | Controller | The system browser's cursor/scroll model |
| 6 | Closing | `sceWebBrowserDialogClose()` on a RUNNING dialog returns `0` and FINISHED follows ~25 frames later |
| 7 | Layouts | Mode 2 opens with parts/control `0/0`, `0x7/0` and `0/0x1`; `0xFF/0xFF` is refused (`0x80b8000a`) |
| 8 | Real site | Emby's web UI through the proxy: Play in Emby plays in EVO |

User agent: `Mozilla/5.0 (PlayStation; PlayStation 5/12.70) AppleWebKit/605.1.15
(KHTML, like Gecko) Version/17.0 Safari/605.1.15`.

### Lessons that cost a console cycle

- **`strcasestr` is not in the native-app libc.** Calling it is a call through a
  null import: SIGSEGV at address 0, on the first proxied response. Use only
  libc calls EVO already makes elsewhere; `evo_net.c` carries its own
  `evo_strcasestr` for the same reason.
- **A dialog that has been terminated is never initialized again.** After the
  unjail that was never tried, so the subsystem stays initialized for the
  session and `evo_webui_shutdown()` terminates it at exit.

### Not yet verified on hardware

Jellyfin, the provider chooser's latest routing (IPTV's own setup page, the
per-row Square action), HTTPS upstreams, and the `loadstart`/`play`
catch-all. Some Emby files still played in Emby's own player before the
catch-all went in.

## Nuvio, and the page storage (2026-09-26, hardware-verified)

**Nuvio** (`addons/src/provider_nuvio.c`) is the third web-UI provider: the
open-source Nuvio TV web app (NuvioTVSmart), a Stremio-addon client, so
catalogs and debrid streams (TorBox, Real-Debrid via addons) arrive with no
debrid code in EVO. The hosted app (`web.nuvioapp.space`) was returning
Cloudflare 522, so the test setup self-hosts the unmodified build (`nuvio-test`
container, port 8098). A self-built copy has no Nuvio account backend or TMDB
key: accounts, QR sign-in and cloud sync don't work; addons and playback do.
Played on hardware: Nuvio → EVO Test Library → Play → stream → EVO's player.

The hook has a **`nuvio` profile** (`web_ui_hook` in the provider vtable,
passed as `/evo/hook.js?p=nuvio`):
- a stream is any `http(s)` source, but only on Nuvio's player element
  (`<video id="videoPlayer">` inside `#player`): the home screen's autoplaying
  trailers set a `src` too;
- `.m3u8`/`.mpd` fetches count only while `#player` is shown (hls.js / dash.js);
- `canPlayType` always answers `"probably"`: Nuvio will not try a source the
  browser says it can't play, and the PS5's WebKit refuses MKV/HEVC/DTS;
- **clicks on Nuvio's hold-able buttons become Enter down + up.** Play, stream
  cards, add-to-library and episode/season/more-like cards start a
  press-and-hold timer and act only on key-up; a click never sends one, so Play
  did nothing. The PS5 browser's X is a click. Reproduced in desktop Chromium
  (Playwright) too, so it's Nuvio, not the console;
- the title comes from the stream screen (`.stream-route-title`), noted at the
  moment a stream is picked, because Nuvio clears that screen as the player
  mounts.

**The page storage.** The browser dialog starts every opening with empty
storage: an addon saved in Nuvio was gone the next time it opened, and Emby
and Jellyfin sign-ins were being lost the same way. The hook restores EVO's
copy of the site's `localStorage` (`GET /evo/storage`) before the site's own
code runs, once per dialog session (a `sessionStorage` flag, so a reload
doesn't roll back newer changes), and posts it back (`POST /evo/storage`)
every 5 s when it changed, synchronously before a handoff or Back to EVO, and
by beacon on page-hide. EVO keeps one file per upstream site in
`/data/evoplayer/webui/<host>_<port>.json`: every web provider shares the
origin `127.0.0.1:8686`, and Emby's and Jellyfin's clients use the same keys.
Values over 512 KB (caches) are left out. The file can be edited over FTP
while the site is closed; EVO restores the edit the next time it opens.

**Diagnostics.** The hook reports to `evo.log` as `web: page: ...`: key
presses, route changes (Nuvio's router state), clicks, script errors,
media-element events and every handoff attempt. That was what located the Play
failure.

**Test library:** `tools/test-addon/serve.sh` builds a static Stremio addon
over a media folder (plus one public HLS stream) and serves it with nginx at
`http://<host>:8100/manifest.json` (CORS open, range requests for seeking).
Nuvio's TV app has no field for an addon URL; its phone page (`/?addonsRemote=1`)
saves into the same browser storage, and the dev trigger file can open that
page inside the dialog.

## Known limits

- Emby/Jellyfin are not told what EVO played: watched state and resume points
  do not update on the server yet.
- The dialog is modal and uses the system browser's cursor UX; EVO cannot draw
  over it.
- `Secure` cookies from an HTTPS upstream are not stored over the loopback
  HTTP origin. Emby and Jellyfin keep their session in local storage, so this
  has not mattered.

## Dev override

`/mnt/usb0/evo_web_probe`, read once at boot, opens a session without going
through a provider: `url=`, `proxy=http[s]://host:port`, `port=`, `layout=<n>`,
and `layouts=1` + `hold=<s>` to cycle the layouts. `tools/provider-server.sh`
serves a test page at `/web/probe.html` (`tools/web-probe/probe.html`) that
exercises fetch, navigation and `window.close()`.
