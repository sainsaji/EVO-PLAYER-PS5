# Native provider screens: Emby, Jellyfin, Stremio addons

**Status: Phase A and B working on hardware (2026-10-01).** Replaces the system
browser (#101) as the everyday way into Emby, Jellyfin and Nuvio. The browser
stays as an optional "web version" (Triangle in the provider chooser).

Verified on the PS5 (FW 12.70, `--usb-remote --virtual-keyboard` build):

- Emby 4.9.5 and Jellyfin 12.1: sign-in on EVO's keyboard (Emby pre-fills its
  public user), home (Continue Watching, Next Up, libraries), TV show →
  seasons → episodes, playback on the hardware decoder.
- Watched state and resume: the server records the stop position; back in EVO
  the list reloads and shows "Resume from …", and Resume starts there.
- Stremio addons: manifest → catalogs → poster grid (paged) → series → seasons
  → episodes → stream picker (every source labelled) → playback.

Learned on the way:

- Emby 4.9 answers `/Sessions/Playing` without a `PlaySessionId` with 400
  "Value cannot be null (key)"; the client sends one per playback.
- Both servers ignore resume points for items shorter than their minimum
  resume duration (5 min by default), so the test episodes are 7 minutes.
- Jellyfin 12 rejects `X-Emby-Token` (401).
- `overflow: hidden` on a rounded box hides its solid-colour children on the
  sceAgc path (`clip_masks=0`); progress bars copy launch.rcss's tile bar.

- Jellyfin Quick Connect: sign-in opens on a six-digit code; approving it in
  another Jellyfin app signs EVO in within one 3 s poll (Square falls back to
  the password, a server with Quick Connect off falls back by itself).
  Endpoints: `POST /QuickConnect/Initiate`, `GET /QuickConnect/Connect?Secret=`,
  `POST /Users/AuthenticateWithQuickConnect`. Emby has no equivalent.

- A deployed, reverse-proxied server (AIOStreams' Jellyfin at
  `https://<host>/jellyfin`, 2026-10-01): sign-in with a UUID + password,
  22 libraries, TMDB posters, 4K HEVC10 HDR playback from a TorBox redirect
  in real time, resume point recorded on the server. It took three fixes:
  - the address's path was dropped; it is now kept (`path=` in the conf) and
    every request, image and stream is built under it, the web version too;
  - every body-less POST over https failed (`SSL_write` of 0 bytes returns 0),
    which broke Quick Connect's start;
  - its session token is a 378-char JWT; a 128-byte token field cut it and
    every request after sign-in came back 401. The field is 1024 now.
- A rejected session (401/403, e.g. revoked) drops the saved session and the
  screen goes straight back to sign-in instead of "Could not load the
  catalog".
- Screenshots of HDR10 playback came out as psychedelic noise: the capture
  read the 10-bit PQ / BT.2020 scanout (R10G10B10A2) as 8-bit BGRA. Fixed in
  `evo_agc_runtime_read_scanout`: PQ -> nits, BT.2020 -> BT.709, highlight
  roll-off, sRGB; verified on the same 4K HDR10 film (picture and OSD).

Open: removing an addon from EVO (only "clear all" today), a phone page for
pasting addon URLs, password masking on EVO's keyboard.

## Why

On the console the web-UI providers were a navigation nightmare, measured on
hardware 2026-10-01:

- the PS5 browser owns the controller: cursor navigation, no D-pad focus, and
  EVO cannot draw over it or close it from the remote;
- the only exits are the page's "Back to EVO" button or the PS button (a panic
  vector), so a page that fails to load is a trap;
- setup is a typed server address (103 D-pad presses for one IP);
- nothing is reported back to the server (watched state, resume points).

Already fixed around the browser (preflight, page watchdog, discovery, keyboard
shortcuts). This plan removes the browser from the main path.

## Shape

Everything rides the existing provider seam (#90). No new screen class: the
provider host (`evo_rmlui_provider.cpp`) already does data-bound rows, folders,
paging, artwork, a selected-item panel, activation, the stream picker and
`report_progress` through `PlaybackController`.

The UI is an embedded RmlUi document per provider family, bound to the host's
data model, like `iptv.rml` and `xtream.rml`. The rule from #90 still holds:
EVO never draws a bespoke C++ screen for a provider.

## Phase A: Emby + Jellyfin (one client)

1. **Shared client.** `addon_emby.c` becomes an instance-based media-server
   client: one instance per provider, each with its own config file, path
   prefix (`/emby` for Emby, none for Jellyfin) and auth header (Emby:
   `X-Emby-Token`; Jellyfin 12 rejects that with 401 and needs
   `Authorization: MediaBrowser …, Token="…"`). `api_key=` works for stream
   URLs on both. The old `emby_*` functions stay as wrappers (host tests).
2. **Catalog** (opaque, prefixed ids):
   - root: Continue Watching, Next Up, then each library;
   - movie library: posters (recursive, sorted by name, paged by
     `TotalRecordCount`);
   - TV library: series → seasons → episodes;
   - any other library: plain folder browse;
   - every playable row carries runtime, resume position and played state.
3. **Sign-in.** Address from LAN discovery (done). Then EVO's keyboard for the
   username (pre-filled with the server's first public user) and the password.
   Token and user id persist in the provider's conf.
4. **Resume.** The selection carries the server's resume position into
   `startPlaybackSource`. Start/progress/stop already flow through
   `report_progress`.
5. **Screen.** `rml/mediaserver.rml`: a 4×2 poster grid with a detail panel
   (poster, title, year · runtime, overview, resume bar, Play/Resume). The host
   gains generic fields (overview, meta line, progress, played) and an optional
   `ui_embedded` vtable name, so Emby and Jellyfin share one document.
6. **Web version** stays reachable from the chooser (Triangle).

## Phase B: Stremio addons (replaces Nuvio as the main path)

1. New provider `addons` ("Addons"): a Stremio addon client. Config is the list
   of addon manifest URLs in `/data/evoplayer/addons.json`.
2. Catalog: root lists every manifest catalog; inside a catalog, metas
   (posters, paged with `skip`); series → seasons → episodes from `meta`.
3. Resolve: ask every addon that serves `stream` for the type; `url` streams
   become choices, best first; the existing stream picker lets the user choose.
4. Adding an addon: EVO's keyboard (manifest URL), plus a phone page served by
   EVO for pasting, as a follow-up.
5. The Nuvio web provider stays as an optional extra.

## Testing (all on the console, no browser involved)

- Screens are EVO's own: `evo-remote.sh key` + `key l3` screenshots.
- Builds use `--usb-remote --virtual-keyboard`.
- Test servers on the dev PC: `emby-test` :8096 (user `bin`, no password),
  `jellyfin-test` :8097 (`evo`/`evo`), test addon :8100. A small TV show is
  added to both media servers for series/season/episode coverage.
- Watched state and resume are checked against the server's API from the PC
  after a playback stops.
