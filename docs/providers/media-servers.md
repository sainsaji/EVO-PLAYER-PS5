# The providers

What each media source does, how it is set up, and what was learned the hard way. The
seam they all sit behind is [architecture.md](architecture.md); the system-browser
fallback is [web-ui.md](web-ui.md).

All of them are browsed in **EVO's own screens** through one `ProviderHostScreen`: the
shelves home, "See all" levels, details and cast, a stream picker, resume, and progress
reports back to the server. The system browser is only an optional "web version" (Triangle
or OPTIONS in the provider chooser).

| Provider | Source | Sign-in | Config (data root) | Catalog and playback |
|---|---|---|---|---|
| **Emby** | an Emby server | username and password on EVO's keyboard (the server's first public user is pre-filled) | `emby.conf` | libraries, Continue Watching, Next Up, TV drill-down, resume, watched and favourites, transcode fallback, skip intro |
| **Jellyfin** | a Jellyfin server | Quick Connect (a six-digit code approved in another Jellyfin app) or password | `jellyfin.conf` | same as Emby |
| **Plex** | a Plex account | PIN: enter the code shown on screen at `plex.tv/link` on a phone or PC | `plex.conf` (the token) | libraries, shows, seasons, episodes; LAN server discovery with connection scoring; direct play; timeline reports (playing, paused, stopped, resume) |
| **Stremio** | Stremio addons | none | `addons.json` (manifest URLs) | catalogs, series, seasons, episodes; every stream source labelled in the picker |
| **Nuvio** | the addons installed in a Nuvio web app (self-hosted, hosted, or EVO's bundled `local:nuvio`) | none | `nuvio.conf`, and the page storage in `webui/<host>_<port>.json` | the Stremio engine over Nuvio's addon list |
| **IPTV** | an M3U/M3U8 playlist, optional XMLTV | none | `iptv.conf` (`playlist=`, `xmltv=`, `bundle=`); USB fallback `/mnt/usb0/.evo_iptv.conf` | channel groups, a guide with now and next, logos, favourites, a stream picker |
| **Xtream** | an Xtream Codes account | host, port, user, password, https | `xtream.conf`; USB fallback `/mnt/usb0/.evo_xtream.conf` | Live, Movies and Series by category, with the EPG streamed through a worker thread |

Server addresses come from LAN discovery where the protocol allows, otherwise they are
typed on EVO's keyboard. Every prompt that asks for an address, playlist URL or addon link
also accepts text from a phone: the keyboard title shows `phone: <console ip>:9780/input`
(open it on a phone on the same network, paste, Send). It needs the `X-EVO-Input` header,
works only while a prompt is open, never writes the text to the log, and is not offered on
sign-in screens, so passwords are still typed on the console. An address may omit the
scheme (a home-network address is `http`, anything else `https`).

Tokens and passwords are stored in plain text in the data root (a known gap).

## Emby and Jellyfin

One shared client (`addon_emby.c`, instance-based: one per provider with its own config,
path prefix and auth header) behind `provider_emby.c` and `provider_jellyfin.c`.

- **Auth differs.** Emby lives under `/emby/` and sends `X-Emby-Token`. Jellyfin has no
  prefix and **rejects `X-Emby-Token` with 401**; it needs `Authorization: MediaBrowser
  ..., Token="..."`. `api_key=` works on stream URLs for both. Sign-in is `POST
  /Users/AuthenticateByName` (or Jellyfin Quick Connect: `POST /QuickConnect/Initiate`,
  `GET /QuickConnect/Connect?Secret=`, `POST /Users/AuthenticateWithQuickConnect`, polled
  every 3 s; Square falls back to the password). Emby has no Quick Connect.
- **Catalog.** Root is Continue Watching, Next Up, then each library; a movie library is a
  recursive poster list sorted by name and paged by `TotalRecordCount`; a TV library
  drills series → seasons → episodes. Details and cast use `/Items?Ids={id}&UserId={uid}&Fields=People,…`
  (the older per-user item URL answers 404 on some servers). Watched and favourite use
  `/UserPlayedItems` and `/UserFavoriteItems` (POST and DELETE).
- **Progress.** The three `/Sessions/Playing*` posts with a `PlaySessionId` per playback
  (**Emby 4.9 answers a report without one with 400** "Value cannot be null (key)"), a real
  `IsPaused`, `PlayMethod` DirectPlay or Transcode, and a per-console `DeviceId`.
- **Resume.** The selection carries the server's resume position into playback; the list
  shows "Resume from ..." after a stop. Both servers **ignore resume points on items under
  their minimum resume duration** (5 minutes by default), so test episodes are 7 minutes.
- **Deployed servers.** A reverse-proxied server under a path (`https://host/jellyfin`)
  works: the path is kept (`path=` in the conf) and every request, image and stream is
  built under it. Session tokens can be long JWTs (378 characters), so the token field is
  1024 bytes. A rejected session (401/403, a revoked token) drops the saved session and
  returns to sign-in instead of "Could not load the catalog". Body-less POSTs over https
  once failed (`SSL_write` of 0 bytes returns 0), which broke Quick Connect's start.

## Plex

`provider_plex.c`: PIN pairing against `plex.tv`, LAN server discovery with connection
scoring, library browsing, direct-stream playback, and timeline reporting that keeps
watched state in step with the account.

## Stremio addons and Nuvio

`provider_stremio.c` is the addon client; `provider_nuvio.c` runs a second instance of it
over the addons saved in a Nuvio web app.

- A Stremio addon is four JSON requests: `/manifest.json`, `/catalog/{type}/{id}.json`,
  `/meta/{type}/{id}.json` (a series lists its videos) and `/stream/{type}/{id}.json`. Only
  streams with a direct http(s) `url` are played. An **`infoHash`** stream is a torrent and
  needs a debrid resolver, which EVO does not have, so it is skipped; Play with nothing
  playable says why (no stream addon, or torrent links only). Debrid services (TorBox,
  Real-Debrid) arrive through Nuvio's addons, with no debrid code in EVO.
- **Cinemeta is built in** (`builtin_sync()`): it rides at the end of every instance's list
  while it has user addons, is never saved or offered for removal, lends its catalogs only
  when no added addon has any (Torrentio has none), and always serves meta and search. It
  is skipped when the user added Cinemeta themselves. A catalog item keyed on its own ids
  (`tmdb:`) with an `imdb_id` is mapped to the IMDb id, which is what stream addons look up.
- **Adding and removing addons** is OPTIONS ("Add & manage addons") inside the card:
  "Add addon" takes a manifest URL, a base URL or a `stremio://` link (EVO keyboard or
  phone page), and there is a "Remove <name>" row per saved addon. For Nuvio these edit the
  same page-storage keys the web version uses (`installedAddonUrls` and
  `installedAddonEnabledStates`, per profile), so both show the same list; the list is
  re-read each time the root opens. With nothing saved, the first add saves Cinemeta plus
  the new addon. Addresses with settings (`providers=yts,eztv|qualityfilter=...`) load fine.
- Every Nuvio request goes through FFmpeg's HTTP client (`evo_net_request_async_avio`).
- Catalog item ids are opaque to EVO and prefixed by kind: `c:` a catalog, `s:` a series,
  `ss:` a season, `m:` a title, `e:` an episode, `x:` an addon that failed to load.
- The addon list is saved with the `cJSON` printer; it once printed every array as `null`,
  so every added addon vanished at the next start (fixed 2026-10-21, with a round-trip
  test).

## IPTV and Xtream

- **IPTV.** `#EXTINF` carries `tvg-id`, `tvg-logo` and `group-title` as unordered quoted
  attributes, and the display name is everything after the **last** comma (splitting on
  the first truncates any channel whose name has one). Groups become folders, so a
  5000-line playlist browses as thirty. There is deliberately no default playlist. XMLTV
  gives now and next; the file is scanned for `<programme>` rather than parsed to a DOM,
  and an EPG over `evo_net`'s body cap does not arrive and leaves now and next empty. A
  malformed playlist entry should not fail the whole playlist (#180, open).
- **Xtream.** `player_api.php`: Live, VOD and Series with seasons and episodes, posters,
  and HLS vs MPEG-TS selection. Credentials are masked in every log line. The guide streams
  through a worker thread (one 64 MB download once stalled the UI).
- Both use the **stream picker** for live channels ([architecture.md](architecture.md#the-stream-picker)).

## Testing a provider

Everything is checked on the console through EVO's own screens: `evo-remote.sh provider
<id>`, `key`, `ui`, `key l3` ([../console/remote-control.md](../console/remote-control.md)).
Build with `--usb-remote --virtual-keyboard` so `type` works. Test servers run on the dev PC
(not in the container, see [../console/networking.md](../console/networking.md)): Emby on
`:8096`, Jellyfin on `:8097` (`evo`/`evo`), `tools/test-addon/serve.sh` for a Stremio addon
on `:8100`, `tools/mock_xtream_server.py`, `tools/provider-server.sh` for a playlist and
guide. Check watched state and resume against the server's own API after a playback stops.
The host preview has fixtures for each home: `shelves` (Jellyfin), `shelves_stremio`,
`shelves_plex`, `shelves_iptv`, `shelves_xtream` ([../ui/hui-kit.md](../ui/hui-kit.md)).

## Not built

TorBox and Real-Debrid as direct resolvers (`CAP_RESOLVE` providers with no catalog; the
resolver chain exists for them), a full XMLTV grid, Trakt sync (#176), DLNA/UPnP (SMB landed in v0.13.0)
sources (#213), per-stream HTTP headers for IPTV (#92). See [../planning/roadmap.md](../planning/roadmap.md).
