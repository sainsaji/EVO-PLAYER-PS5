# Nuvio native provider

NuvioTV's content model reimplemented in C behind `evo_provider_t` and played
by EVO's hardware-decoded player. That model is Stremio-protocol addons for
catalogs, metadata and streams, debrid for resolving torrents, and watch
progress synced to a Nuvio account.

This is the roadmap's **Stremio / Nuvio v3** story, together with its
**Torbox** and **Real-Debrid** resolver stories.

**Status:** all three phases are implemented and host-verified (21 tests,
including end-to-end runs against fake servers, clean under ASan and UBSan).
None of it has run on a console yet. Branch `feat/nuvio-native-provider`.
Setup and the first-run checklist are in [nuvio-setup.md](nuvio-setup.md).

---

## 1. Why native, and not the web-UI provider

`provider_nuvio.c` opens NuvioTV's web build in the system browser and
`evo_webui.c` catches its `<video>` element. That works, but it lives inside
the PS5 browser: cursor-style navigation, an old WebKit, and a dialog teardown
around every playback. The native provider keeps the service logic in EVO and
gets D-pad focus from RmlUi. The web provider is still there, shown as
**Nuvio (Web)**.

## 2. What was ported, and from where

NuvioTV is the specification, not code to transliterate:

| NuvioTV | Here | Notes |
|---|---|---|
| `AddonRepositoryImpl.canonicalizeUrl`, `AddonMapper.parseResources` | `nuvio_stremio.c` | base URL keeps a configured addon's `?query`; a bare-string resource inherits the manifest's types |
| `CatalogRepositoryImpl.buildCatalogUrl` | `nuvio_catalog_url` | all three URL shapes, `skip` paging, `hasMore = supportsSkip && rawCount > 0` |
| `MetaRepositoryImpl`, `StreamRepositoryImpl` | `nuvio_resource_url`, `provider_nuvio_native.c` | type and id are percent-encoded path segments; first addon serving `meta` wins |
| `core/debrid/DebridFileSelection.kt` | `nuvio_debrid.c` | filename, then `s01e02`/`1x02`, then `fileIdx`, then largest; a hint that matches nothing is a failure |
| `TorboxDirectDebridResolver`, `RealDebridDirectDebridResolver` | `provider_debrid.c` | cached-only, same request sequences; RD deletes a torrent it could not use |
| `WatchProgress`, `WatchProgressRepositoryImpl.progressKey` | `nuvio_progress.c` | `<id>` / `<id>_s<S>e<E>`, milliseconds, 2 %/90 % thresholds |
| `AuthManager`, `ServerDiscovery`, `AddonSyncService`, `WatchProgressSyncService` | `nuvio_account.c` + provider | see section 6 |

Not ported: JS scraper plugins, local torrent streaming, Trakt/Simkl/MDBList,
TMDB enrichment, trailers, skip-intro, profiles UI, collections (section 9).

## 3. The tree

The seam's model is one list per level, so NuvioTV's home rows become folders.
Ids are opaque to EVO and parsed only by the provider:

| Id | Level |
|---|---|
| `""` | root: Continue Watching, then one folder per catalog (`showInHome` first) |
| `cw` | Continue Watching |
| `c:<addon>:<type>:<catalog>` | a catalog, paged by `skip` |
| `m:<type>:<meta>` | a title: its seasons, or straight to streams for a film |
| `s:<season>:<type>:<meta>` | a season's episodes, in episode order |
| `v:<type>:<meta>\|<video>` | an episode's streams |
| `x:<gen>:<index>` | a stream, the only playable rows |

A stream URL does not fit an item id, so `x:` indexes the provider's table for
the last streams level. The generation makes a stale id fail cleanly instead
of playing the wrong stream. Meta ids too long for an item id are dropped,
never truncated.

The streams level doubles as a stream picker. Streams are sorted playable-first,
then by resolution; within a tier each addon keeps its own order (Torrentio
sorts by seeders). Emoji are stripped from addon text because the UI font
cannot draw them.

A catalog that requires an argument other than `search` (a genre, say) is
hidden, since EVO has no UI to pick one. A search-only catalog is reached
through Search (Square).

## 4. Changes outside the provider

Each one is generic. A different provider could have needed any of them.

| Change | Where | Why |
|---|---|---|
| 4 worker threads (was 1) | `evo_net.c` | a stream fan-out to eight addons ran serially, up to about 48 s |
| completed requests kept in an unbounded list | `evo_net.c` | the fixed queue dropped finished requests when full, and their callbacks never fired |
| `evo_net_request_async_timeout` | `evo_net.[ch]` | scraping addons idle past the 6 s default before their first byte; streams get 20 s, search 15 s |
| caller's `Content-Type` replaces the JSON default | `evo_net.c` | debrid APIs take form and multipart bodies |
| `\uXXXX` escapes decoded to UTF-8 | `cJSON.c` | they came through as literal `uXXXX` |
| resolver chain skips catalog/web providers | `evo_provider_mgr.c` | a connected Emby accepted a Nuvio magnet as its own id and reported it playable |
| optional `play_title`, `resume_sec`, `source_prompt` | `evo_provider.h` | OSD title for stream rows, resume from progress, a keyboard prompt that is not "playlist URL". No API bump: NULL means the old behaviour |
| resolvers go straight to their key prompt from the chooser | `ProviderHostScreen.cpp` | opening a catalog-less provider landed on an empty host |
| OPTIONS / chooser Square open the keyboard for a provider with a `source_prompt` | `ProviderHostScreen.cpp` | the setup page is IPTV's, the fallback skin has no button on it, and opening it calls `set_source("")` |
| SDK v0.42 → v0.43 | `Dockerfile`, compose, scripts | v0.43 is the first SDK with the 13.60 kernel offset table |

## 5. Configuration

Everything lives in `/data/evoplayer/providers/nuvio-native/` and can be
written over FTP:

| File | What |
|---|---|
| `addons.txt` | manifest URLs, one per line, in order. Absent means Cinemeta only |
| `account.conf` | optional Nuvio account (section 6) |
| `bundle.txt` | optional URL of the Nuvio skin (section 7) |
| `progress.json` | watch progress, written by EVO |
| `m_<hash>.json` | cached manifests, so the root lists without network |

OPTIONS on the provider screen, or Square on it in the chooser, opens the
keyboard with the provider's `source_prompt`. What is typed **adds** one addon
URL. `reset` puts the list back to Cinemeta. Empty input is
a no-op on purpose: the host calls `set_source("")` every time it opens the
setup page, so treating empty as "reset" would wipe the addon list.

Torbox and Real-Debrid keys go in through their own entries in the chooser
(`torbox.conf`, `realdebrid.conf` in the data root). A key is never pre-filled
into the keyboard.

## 6. Account sync (phase 3)

NuvioTV's official server URL and publishable key are injected at build time
and are **not** in its source. This client never carries them. It supports
what NuvioTV itself supports for other servers:

- `server=https://…`: NuvioTV's self-hosted discovery,
  `GET <server>/.well-known/nuvio` → `backend_url` and `publishable_key`
  (`version: 1`, `service: nuvio`);
- or `backend_url=` and `publishable_key=` given directly.

Sign-in is email and password (`/auth/v1/token?grant_type=password`), read
once from `account.conf`. After the first success the password is removed from
the file and a refresh token replaces it. A request that comes back 401
refreshes once and retries (NuvioTV's `withJwtRefreshRetry`).

On the first root listing each session:

1. `rpc/get_sync_owner`, so a linked device reads its owner's rows;
2. `addons?user_id=eq.<owner>&profile_id=eq.<n>&order=sort_order.asc`. The
   account's enabled addons replace `addons.txt`;
3. `rpc/sync_pull_watch_progress`, merged newest-wins and not marked for push;
4. `rpc/sync_push_watch_progress` with the local changes, in NuvioTV's shape,
   including `p_origin_client_id` (`evo-ps5-…`, generated once).

Progress is also pushed when playback stops. TV QR login is not implemented:
it needs a QR encoder and an image path into the provider UI, and email plus
password covers the same need.

## 7. UI bundle

Without `bundle.txt` the provider renders in EVO's embedded fallback skin. The
Nuvio skin (`assets/providers/nuvio-native/`, data model `nuvio`) uses NuvioTV's
palette and three row kinds:

- a poster card for folders with art;
- a text tile for folders without it (catalogs);
- a full-width stream row for playables.

Episodes show the show's poster, with the 16:9 still as the backdrop. Serve it
with `tools/provider-server.sh` and put the printed URL (ending
`/nuvio-native/manifest.json`) in `bundle.txt`.

**Not yet rendered anywhere.** It follows the RCSS rules in
`provider-architecture.md` 6.3 and mirrors the IPTV bundle's patterns, but
`tools/uiview.sh` has no Nuvio fixture yet and was not run.

## 8. Known gaps

| Gap | Effect |
|---|---|
| no per-stream request headers in `PlaybackSource` | streams with `behaviorHints.proxyHeaders` are listed last, marked "may not play" |
| no external subtitle URLs | addon `subtitles[]` ignored; embedded subtitles work |
| one list per level (API v1) | no horizontal home rails |
| no TV QR login | account needs email and password once |
| uncached torrents | fail at once with the debrid service's answer; nothing downloads and waits |
| player limits | per `codec-support.md`: no TrueHD, no bitstream, 4K only on the hardware decoder |

## 9. Out of scope

- **JS scraper plugins.** They would be arbitrary third-party code on the
  console, which the provider seam exists to prevent, and they need a
  reimplemented host API.
- **Local torrent streaming.** Debrid covers it.
- **Trakt, Simkl, MDBList, TMDB, trailers, skip-intro, profiles, collections.**
  Independent follow-ups.

## 10. Tests

`tests/run_tests.sh` (host, `-DNO_OPENSSL`):

- unit tests for URL shapes, manifest/resource matching, catalog, meta and
  stream parsing, magnets, sorting, the JSON writer, the progress store and
  file selection;
- `evo_net`: parallelism, which fails with one worker, and the per-request
  timeout;
- end to end, through a fake HTTP server driving the **real** providers and
  registry:
  - browse to streams;
  - magnet → chain → Torbox, with a connected Emby in the way;
  - magnet → Real-Debrid;
  - series, progress, resume and search;
  - account sign-in, addon pull, and progress pull and push.

The assertions check URLs, headers and request bodies, not just outcomes.
