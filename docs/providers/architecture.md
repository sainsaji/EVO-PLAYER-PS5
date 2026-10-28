# Provider Architecture

How EVO Player connects to a media source that is not the USB stick, and why a
provider brings its own UI instead of EVO drawing one for it.

Implemented by #90. What each provider does and how it is set up is in
[media-servers.md](media-servers.md); this page is the seam they share. Per-provider
stories follow under the `addons` label and build on this seam rather than extending it.

---

## 1. The rule this exists to enforce

**The 0.6/0.7 Emby integration was wrong because EVO drew a custom UI for it.** A
nine-row D-pad list that looked like EVO and nothing like Emby, re-authored by hand for
every new source, could not express what those services look like: a channel guide is a
grid, a library is a poster wall.

So a provider is two halves with a hard line between them:

| Layer | Owner | Delivery |
|---|---|---|
| Auth, catalog, search, link resolution, progress reporting | in-binary C behind `evo_provider_t` | compiled into the app module |
| Screens, layout, navigation | a data-bound document over the provider's data model | an **embedded** document EVO ships for its own providers (`mediaserver`, `iptv`, `xtream`, the fallback skin), or a **bundle fetched over HTTP at run time**, cached under `/data/evoplayer/providers/<id>/` |

The C half publishes a **data model**; the markup binds to it by name and never
references an EVO element id. There is no per-provider C++ screen: every provider shares
one `ProviderHostScreen`. EVO's own providers draw on one shared **shelves** layout
(below), rasterised by the kit ([../ui/hui-kit.md](../ui/hui-kit.md)); a bundle served by
a third party is drawn by RmlUi itself, and arbitrary third-party *code* never reaches the
console.

**EVO supplies playback; a provider supplies content and, if it wants, its own look.**

---

## 2. Files

| Path | What it is |
|---|---|
| `addons/include/evo_provider.h` | the vtable, the data types, `EVO_PROVIDER_API_VERSION` |
| `addons/src/evo_provider_mgr.c` | the static provider table, enable flags, the resolver chain |
| `addons/include/evo_provider_bundle.h` | bundle format, limits, path safety, the art registry API |
| `addons/src/evo_provider_bundle.c` | manifest parse, fetch, sha256, cache |
| `addons/src/provider_iptv.c` | the IPTV provider — M3U/M3U8, group folders, XMLTV now/next |
| `addons/src/provider_emby.c` | Emby on the vtable; `addon_emby.c` keeps its own API |
| `addons/src/provider_{jellyfin,plex,stremio,nuvio,xtream}.c`, `xtream_epg.c` | the other providers ([media-servers.md](media-servers.md)) |
| `ui_rml/src/evo_rmlui_provider.cpp` | the per-provider Rml context, data model, focus, events |
| `ui_kit/src/evo_hui_provider.cpp` | the shelves layout, drawn by the kit |
| `ui_rml/src/evo_rmlui_provider_art.cpp` | remote artwork → `evo:mem/` textures, LRU |
| `core/src/screens/ProviderHostScreen.cpp` | the one screen class every provider shares |
| `assets/rml/{mediaserver,iptv,xtream,provider_fallback}.{rml,rcss}` | the embedded documents; `provider_fallback` is also the binding spec |
| `assets/providers/iptv/` | the IPTV bundle — **served, not embedded** |
| `tools/provider-server.sh` | serves bundles + a test playlist to the console |
| `tools/gen_provider_manifest.py` | regenerates a bundle's `manifest.json` |

---

## 3. Runtime constraints (app module, not payload)

EVO is the `PPSA99039` app module. There is no payload heap and no 3.5 GB.

| Capability | Status on the app module |
|---|---|
| BSD sockets, DNS | native; `socket()`, `connect()`, `getaddrinfo()` all work |
| TLS | static OpenSSL from the pacbrew sysroot. `-lssl -lcrypto` are in the shipping binary and have been since 0.6 |
| FFmpeg network | **on since #90** — `--enable-network --enable-openssl`, protocols `http,https,tcp,tls,crypto`, demuxers `hls` and `dash` |
| JSON | `cJSON`, compiled unconditionally |
| Memory | three pools, none of them 3.5 GB; see [../hardware/memory-budget.md](../hardware/memory-budget.md). A bundle's whole cache is capped at 8 MiB and its decoded artwork at 24 MiB, both smaller than one 4K frame |
| Writable storage | `/data/evoplayer` once `evo_jailbreak_self()` lands; `/download0/evoplayer` before that and it does **not** survive a relaunch (#46) |

Three consequences that are easy to get wrong:

- **`dash` needs libxml2**, which needs libiconv. Both are in the pacbrew
  prefix, and both are now in the link lists (`Makefile`,
  `build-evoplayer.sh`, `package-app.sh`). Forgetting one turns a provider
  change into an FFmpeg-looking link failure.
- **Nothing may block the 60 Hz loop.** Every provider call returns
  immediately and completes through a callback delivered by `evo_net_poll()`,
  which `Application.cpp` already pumps once per frame.
- **The demux thread must never block on DNS.** Provider URLs reach FFmpeg only
  after `resolve()` has produced an `http(s)` URL, and the open goes through
  `evo_stream_io_open()`, which sets a 5 s timeout. A hung network open would
  wedge the app slot the way a hung decode call does (#39).

---

## 4. The provider vtable

`evo_provider_t` (`addons/include/evo_provider.h`). Read the header — it is the
normative version. In summary:

```c
init / shutdown / is_configured      lifecycle; init() must not touch the network
auth                                 CAP_AUTH
list_catalog(parent_id, page, cb)    CAP_CATALOG
search(query, page, cb)              CAP_SEARCH
resolve(item_id, cb)                 CAP_RESOLVE -> evo_stream_choice_t[]
report_progress(id, pos, dur, state) CAP_PROGRESS
ui_bundle_url()                      CAP_UI
```

A capability bit with a NULL slot behind it is rejected at startup, so a
mismatch is a log line and a disabled provider rather than a null call inside a
screen the user just opened.

**`resolve` is what makes Torbox and Real-Debrid fit.** They contribute no
catalog at all — only a link resolver. `evo_provider_resolve_chain()` resolves
an item with its own provider and, if the best choice comes back with
`needs_resolver` set, hands that link to each enabled `CAP_RESOLVE` provider in
turn until one returns something playable. Neither provider knows the other
exists.

Item ids are **opaque**. Whatever a provider needs to identify an item again;
EVO never parses, splits or constructs one. That is the difference from
`BrowserEntry::fullPath`, which is a filesystem path the browser manipulates
freely — a remote item has no path.

---

## 5. The UI bundle

### 5.1 `manifest.json`

```json
{
  "id":          "iptv",
  "name":        "IPTV",
  "version":     "20260924.083000",
  "api_version": 1,
  "entry":       "main.rml",
  "data_model":  "iptv",
  "assets": [
    { "path": "main.rml",  "bytes": 3700, "sha256": "<64 hex>" },
    { "path": "main.rcss", "bytes": 5461, "sha256": "<64 hex>" }
  ]
}
```

- `api_version` is the **minimum** seam the bundle needs. A bundle written
  against a newer one is refused outright rather than half-rendered — its markup
  binds to fields this build does not publish.
- `data_model` must match the name the bundle's `data-model` attribute uses. A
  mismatch renders the document bound to nothing, which looks like a working
  screen with no content and produces no warning.
- `version` is what EVO compares against its cache. An edit that does not change
  it will not be picked up. `tools/gen_provider_manifest.py` stamps a UTC
  timestamp on every run for exactly this reason.
- Hashes are an **integrity** check — a truncated download, a mangled cache —
  not an authenticity one. The manifest travels the same connection as the
  files, so anything able to rewrite one rewrites both. Use https for a bundle
  you care about.

Regenerate with:

```bash
python3 tools/gen_provider_manifest.py assets/providers/iptv
```

### 5.2 Limits

All of these are enforced, not assumed (`evo_provider_bundle.h`):

| Limit | Value |
|---|---|
| files per bundle | 64 |
| bytes per file | 2 MiB |
| bytes per bundle | 8 MiB |
| `manifest.json` | 64 KiB |
| path length | 128 |
| documents | 4 |
| DOM nodes | 4000 |
| fonts | 4 |
| decoded artwork | 24 MiB (LRU) |

Byte counts are checked against the manifest **and** against what actually
arrives — a manifest declaring 4 KB while the server sends 40 MB is the
interesting case, and only the second check catches it.

### 5.3 Trust boundary

- **RmlUi scripting stays off.** A bundle contains `.rml`, `.rcss`, fonts and
  images. No code, ever. `gen_provider_manifest.py` refuses any other
  extension.
- **Every path goes through `evo_bundle_path()`.** It rejects a leading `/`, a
  drive letter, any `..`, any backslash, any `scheme:` prefix, an empty
  component, a component that is only dots, and anything over the length cap.
  This is the only sanctioned way to build a path inside a provider's cache
  directory, including for names EVO generated itself — RmlUi's file interface
  reaches disk with a plain `fopen()` and does no checking of its own.
- **Artwork is http(s) only.** An item's `art_url` is data from a remote
  service; a `file://` in it must not become a local read.
- **Any failure at all drops to the embedded fallback skin with the reason on
  screen.** Never a blank screen, never a hang. On a console whose log is not
  reachable while a screen is up, an unexplained failure is unreportable.

---

## 6. Writing a bundle

The normative reference is `assets/rml/provider_fallback.rml` — it is the one
document in the repo guaranteed to stay in step with `EvoProviderModel`.

### 6.1 The data model

Bound per provider. Top level:

| Name | Type | Notes |
|---|---|---|
| `provider_name` | string | |
| `breadcrumb` | string | `"IPTV / Sports"` |
| `status` | string | `""` when idle; otherwise a message to show |
| `has_error` | bool | picks the colour for `status` |
| `loading` | bool | |
| `empty` | bool | loaded, and there is genuinely nothing |
| `is_folder_level` | bool | this level is folders, not playable items |
| `count` | int | rows at this level |
| `rows` | array | |

Per row: `id`, `title`, `subtitle`, `overview`, `art`, `now`, `next`,
`duration`, `initial`, `is_folder`, `is_live`, `index`.

`art` is `""` until the poster has been downloaded and decoded, then the
`evo:mem/...` key to use as an `<img src>`. Paint a background colour behind a
poster: artwork fills in over several frames and a row with no background
flashes empty on every navigation. `initial` is the title's first character,
upper-cased, published because RmlUi has no substring transform.

### 6.2 Events

Three, and only three:

```
activate()    start the row that raised it — see the warning below
load_more()   next page, when `has_more`
go_back()     pop a level
```

> **`activate()` takes no arguments, and the row must be carried on a
> `data-attr-rowid="row.id"` attribute.**
>
> `DataControllerEvent::Initialize` parses its expression once and resolves the
> variable addresses at that moment, so inside a `data-for` the alias is baked
> to the first iteration: `activate(row.id)` hands back row[0]'s id no matter
> which row was clicked. Measured, not assumed — every card reported the first
> group's id. Views (`data-attr`, `data-if`, `{{ }}`) re-resolve per clone and
> are unaffected, which is why an attribute carries it and an argument cannot.

### 6.3 RCSS and RML: what actually works

These are the things that cost time to discover. Every one of them was measured
against this RmlUi build, not inferred from HTML.

| Do this | Not this | Why |
|---|---|---|
| `{{ expression }}` for text | `data-value="expr"` | `data-value` is the two-way binding for a **form control's `value` attribute**. On a div it renders nothing, with no warning. |
| `tab-index: auto` in RCSS | `tabindex="auto"` attribute | RmlUi has **no `tabindex` attribute**. Without the RCSS property a row is not focusable at all, so Tab and the D-pad both skip it and the screen reads as hung. |
| `nav: auto` in RCSS | — | RmlUi's spatial navigation reads `nav-up`/`-right`/`-down`/`-left`. With the default `none` the key press arrives and nothing moves. |
| `display: block` stated explicitly | relying on a div being block | **RmlUi's `display` defaults to `inline`**, for a div as much as a span — there is no HTML default stylesheet. Leaving it out laid a header out on one line and dropped the root element's padding entirely. |
| `data-attr-rowid="row.id"` | `activate(row.id)` | see 6.2 |

Supported and worth knowing:

- **Layout is RmlUi's, never the render backend's.** `display: flex`,
  `inline-flex`, `table`, `inline-block`, `position: absolute/relative/fixed`,
  `overflow`, `border-radius`, `box-sizing` all work, and work identically in
  the host preview and on the console.
- **`:focus` and `:focus-visible`** are set by RmlUi when navigation moves
  focus. The focus ring is the bundle's to draw; EVO pushes no `is_focused`
  flag and does not know which row is selected.
- **Single-quoted string literals** work in data expressions:
  `data-if="status != ''"`.
- **Transform functions** are `to_lower`, `to_upper` and `format`. There is no
  `substr`.
- **dp units.** Every EVO stylesheet is authored against a 1920×1080 canvas in
  `dp`; the context's density-independent ratio scales it to the panel. Use
  `dp`, not `px`.
- **Leave 152dp of gutter on the left.** EVO's navigation rail is a document in
  EVO's *main* context, composited over the provider context after it has
  drawn (`ProviderHostScreen::render`). It is not in the bundle's layout and
  nothing reserves room for it, so a bundle that starts at x=0 has its first
  column drawn underneath the rail. `assets/providers/iptv/main.rcss` puts the
  whole page in a `#tv-pane` at `left: 152dp`.

### 6.3.1 EVO's shared assets

EVO's embedded asset bundle (#60) is reachable from a provider document under
the path `/assets/...`:

```
/assets/icons/btn_cross.png      the PlayStation glyphs
/assets/icons/icon_folder.png    and the rest of the icon set
/assets/wallpaper/settings_bg.png
/assets/fonts/LatoLatin-Regular.ttf
```

The mechanism is the leading slash. RmlUi's `SystemInterface::JoinPath` treats
a path starting with `/` as absolute and **strips that one character** instead
of resolving it against the document, which hands `EvoRmlFileInterface::Open`
the string `assets/icons/btn_cross.png` — one of the prefixes
`evo_rmlui_bundle_find()` normalises away, so the lookup hits the embedded
bundle. The same string works in the host preview and on the console, and it
never touches the filesystem. `/app0/assets/...` does **not** work: the leading
slash is gone by the time the bundle is searched.

Two rules come with it:

- **It is optional, and a bundle must survive without it.** A missing texture
  renders as nothing, not as an error, so every one of these belongs in a box
  that still reads correctly when the image does not arrive — a glyph badge
  with a border, a preview plate with a background.
- **It is EVO's asset set, not a stable API for third parties.** A bundle on a
  provider's own server should ship its own images in its manifest. The IPTV
  bundle uses the shared ones because it is EVO's own first-party bundle and
  the alternative was checking a copy of EVO's glyphs into this repo twice.

### 6.4 The one real backend difference

| Feature | host preview (CPU rasteriser) | console (sceAgc) |
|---|---|---|
| geometry, textures, scissor, transform, clip masks | yes | yes |
| `filter:` / `backdrop-filter:` | **no** — logs `Could not compile filter` | blur only |

Note the direction: a bundle using `backdrop-filter: blur()` looks **broken in
`uiview.sh` and correct on hardware**. That is the opposite of the usual risk
and the reason this table exists.

Everything else is layout and geometry, which RmlUi resolves before either
backend sees it — so the host preview is a faithful preview.

### 6.5 Theme

Provider contexts do **not** receive EVO's theme push. `evo_rmlui_set_theme`
stamps colours onto every element as inline properties, which would silently
overwrite exactly what a bundle exists to supply. A bundle's colours are its
own, and it must set every colour it wants.

The consequence, for a bundle that wants to look like the rest of EVO: it has
to write EVO's palette out as literals and it will not follow a theme change.
The IPTV bundle does exactly that — its stylesheet opens with the MIDNIGHT
tokens spelled out, and the header of `main.rml` says why a first-party
provider chooses to match. That choice is the bundle's; nothing about the seam
pushes it either way, and what the seam proves is still on screen, because the
IPTV bundle draws a channel grid that drills into groups where EVO draws a
nine-row list.

---

## 7. The host preview

This is what keeps provider-UI iteration off hardware.

```bash
./tools/uiview.sh --all      # includes the provider screens
```

`tools/uiview_playback_rml.sh` starts a `python3 -m http.server` on the loopback
interface, stages a data root in a temp directory via
`EVO_DATA_DIR_OVERRIDE`, writes an `iptv.conf` pointing at it, and the fixture
in `tools/uiview_playback_rml.cpp` then drives the real provider: the real
`evo_net` client, the real M3U parser, the real bundle fetch and hash check, the
real data model, and the provider's own markup and RCSS.

Four shots:

| Shot | What it proves |
|---|---|
| `rml_provider_iptv` | the provider's own bundle rendering its own grid from a real playlist |
| `rml_provider_iptv_focus` | D-pad movement through it, with the bundle's own focus ring |
| `rml_provider_iptv_channels` | activation into a group — the full click → folder push → second catalog call |
| `rml_provider_fallback` | a failed bundle falling back to the embedded skin **with the reason visible** |

Two host substitutions, both because the dev image lacks the libraries, both
marked in the source where they are made:

- `-DNO_OPENSSL=1` — no host OpenSSL, so https is out. The fixture's local
  server speaks http.
- `-DEVO_PROVIDER_ART_NO_DECODE=1` — no host FFmpeg, so posters render as the
  no-artwork branch. The download, disk cache, key registry, LRU and path
  checks all still run.

### Serving a bundle to the console

```bash
./tools/provider-server.sh            # runs on the HOST, not in the container
```

It regenerates each manifest, stages `assets/providers/*`, generates a test
playlist and an XMLTV file with live now/next windows, prints the LAN URL and
the `iptv.conf` to paste, and serves it.

It must not run in the dev container: per
[networking.md](../console/networking.md) the PS5 cannot reach a
listener inside the container on Windows bridge networking, so the server would
be invisible to the console even though `curl` from inside the container works.
The script refuses to start if it detects `/.dockerenv`.

---

## 8. Playback

A provider item reaches the player through `PlaybackSource`, not a path string:

```c
struct PlaybackSource {
    std::string url;       // what avformat opens; may carry a token
    std::string title;     // what the OSD shows
    std::string provider;  // "" for a local file
    std::string item_id;   // opaque; "" for a local file
    bool is_live;
};
```

A bare path was enough while every source was a file on the USB stick: the
filename was the identity, the title and the thing FFmpeg opened, all at once. A
provider item is none of those.

- **The OSD shows `title`.** Deriving one from the URL is the root cause of #9 —
  `cleanMediaTitle` takes the last path component, which is right for
  `/mnt/usb0/Movie.2019.mkv` and wrong for
  `http://host/emby/Videos/abc/stream?api_key=…`.
- **The URL is never persisted.** Recent, Favorites and the resume file store
  `evo://<provider>/<item_id>` — tokenless, and still exactly what `resolve()`
  takes. A signed URL that has expired is both a leak and useless.
- **`current_media_path` stays empty for a provider source.** It is read by the
  subtitle sidecar scan, the demuxer and the favourites toggle, all of which
  expect a filesystem path.
- **Live streams are marked, not discovered.** `is_live` comes from the
  provider's catalog or from the resolved choice. The OSD, the resume store and
  the seek path all assume a seekable file; a seek that fails is not a usable
  way to find out.
- **`report_progress` is pumped from `saveResumePosition()`**, rate-limited to
  roughly every ten seconds, with `PLAY_START` on open and `PLAY_STOP` on stop.

Both local and network opens go through `evo_stream_io_open()` (`media/src/
evo_stream_io.c`), which is where `reconnect`, `reconnect_streamed` and
`timeout` live. It had no callers at all before #90 — every open went straight
to `avformat_open_input`, so none of those options had ever been applied.

### The stream picker

A channel resolves to a list of `evo_stream_choice_t`: the URL the playlist gave,
and, for a `.m3u8` or `.ts` address, the same address with the other extension
(a guess, derived in `provider_iptv.c`). Left to itself EVO plays the first and,
if it does not open, tries the next.

With **Settings → Interface & Storage → ASK WHICH LIVE STREAM** on (the default),
`ProviderHostScreen` does not decide. After the channel resolves it reads the
HLS master playlist behind the listed URL (`evo_hls_variants_fetch`, an async GET
through `evo_net`, bounded to 8 s) and opens a list:

| Row | Where it comes from | Badge |
|---|---|---|
| the URL as listed | the provider's first choice | `LISTED` |
| each quality variant, best first, up to six | `#EXT-X-STREAM-INF`: resolution, bitrate, codecs | `HLS` |
| the `.ts` / `.m3u8` swap | the provider's other choices | `GUESS` |

EVO plays only the row chosen. If it does not open, the toast says so and the
list comes back with the cursor on it; nothing else is tried behind the user's
back. A channel with one stream and nothing behind it (no HLS master, no
alternative) has nothing to choose, so it plays directly. With the setting off
none of this runs and a channel behaves as it always did, automatic fallback
included.

The choice is read when the channel is opened, not when the reply lands. The
list is the generic list document, so it needs no markup of its own; the rows
are filled by `ProviderHostScreen::renderStreamPicker`. The parser and the
relative-URL join in `evo_hls_variants.c` are pure and are tested on the host by
`tools/hls_host.sh`.

The list opens **at once** with the rows it already knows (the listed URL and any
guess), and says "reading the available qualities…" in its subtitle; the variants
are inserted when the read lands, and the cursor moves with the rows. If the read
fails (an HTTP 403 on a signed channel, a timeout) the subtitle says "qualities
unavailable" and the list keeps the listed URL and the guess. `evo_net` does not
send per-stream headers (#92), so a server that refuses an unknown User-Agent
refuses this read too.

### Quality inside the player

The picker above depends on one fetch succeeding, so the player has a second,
independent way to choose: **R2 → QUALITY & AUDIO**. When the open stream has more
than one video variant (an HLS master exposes each as its own video stream) the
list starts with an AUTO row and one row per variant, best first, with resolution,
codec, bitrate and frame rate, then the audio tracks. The variants come from the
stream itself once it is open, so nothing has to be fetched. Choosing one
re-opens the stream at the current position (`switchVideoVariant`, as
`switchAudioTrack` does for audio) with that video stream pinned; the audio comes
from the same variant's program, so the two stay one encode. AUTO drops the pin.
The pin belongs to the URL it was made on and outlives an audio switch, but not a
different channel. The control hint beside R2 shows the playing quality
(`720p`, or `720p AUTO`).

A reopen keeps the provider's title, identity and live flag. It used to rebuild a
bare-URL source, so an audio switch (and `replay()`) on a provider stream dropped
the title back to the URL - the #9 defect again.

---

## 9. Home layout and protocol notes

Per-service protocol details (Emby and Jellyfin endpoints, Stremio, IPTV, Xtream, Plex) are in
[media-servers.md](media-servers.md).

### Home layout: shelves for every provider

Every provider with a catalog opens on the same shelves screen
(`ui_kit/src/evo_hui_provider.cpp`, data and focus in
`ui_rml/src/evo_rmlui_provider.cpp`). A provider only changes the rows:
each folder in its root becomes a shelf of up to 24 cards, with a "See all"
card when there is more, and the rest of the folders gather in a closing "More"
shelf. Poster cards and channel cards (logo on a plate, LIVE pill, now line)
share one card size. A root that is not a list of folders (a flat playlist)
falls back to the plain level. Mapping:

| Provider | Shelf | Card |
|---|---|---|
| Emby / Jellyfin | Continue Watching, Next Up, Recently Added, Favourites, one per library | poster |
| Stremio addons / Nuvio | one per addon catalog (`c:` ids); two with one name get " - Movies" / " - Series" | poster |
| Plex | one per library, found by opening each server first | poster |
| IPTV | one per channel group, plus Favourites and Languages | channel |
| Xtream | Live, Movies, Series | category |

"See all" and folder levels use the same card, 8 across and 2 rows a page
(`EVO_PROVIDER_COLS`, `EVO_PROVIDER_PAGE_SIZE`), with the focused card's title
(or the channel's now/next) at the top right. The focused card's `backdrop_url`
and `logo_url` are fetched after the cursor has rested and drawn behind the
shelves. Triangle opens the item menu when the provider has `item_action` or
`panel`; Stremio and Nuvio offer Details & cast built from the title's meta
reply (`panel` hook, kind "details").

### Movie and show detail page (#221)

Cross on an item the provider marks `evo_provider_item_t::detail` (1 a movie, 2 a TV
show) opens a page instead of playing: poster, PLAY and RESTART on the left, then
Overview / Cast / Details tabs (Episodes for a show). Only providers with a `panel`
hook use it, so IPTV still plays straight away. What the page reads:

- **Item fields** (already in the list reply): `overview`, `rating`, `cert`, `facts`
  (year, runtime, genres), `art_url`, `resume_pos_sec`.
- **Cast:** the `panel` hook, kind "details". Person rows carry `group = "cast"`, `title` =
  name, `detail` = role (empty when unknown), `art_url` = headshot (empty on Xtream and
  on addons that send names only; the page draws a letter in a circle).
- **Details tab** (replaced a Versions tab that re-ran the stream lookup and came back
  empty when no stream addon answered): the non-cast rows of the same "details" reply
  (Rating, Genres, Director, Studio, Tagline), shown as label / value. No extra request.
- **Play / Restart:** the selection a card press queues, with `skip_prompt` set, so
  ProviderHostScreen does not ask Continue / Restart / Cancel again. Restart is only
  offered when the Resume playback setting is on and a position is saved.
- **Show:** one EPISODES button (and the Episodes tab's row) opens the show's folder
  exactly as the card press did.

---

## 10. Known gaps

- Credentials in `/data/evoplayer` are plaintext.
- A bundle's `backdrop-filter` blur looks wrong in the host preview and right on the
  console (section 6.4).
- Live TV search and recordings (#179), per-stream headers (#92) and a malformed playlist
  entry failing the whole playlist (#180) are open; see [../planning/roadmap.md](../planning/roadmap.md).

## 11. See also

- [media-servers.md](media-servers.md): each provider
- [web-ui.md](web-ui.md): the system-browser fallback
- [../ui/rmlui.md](../ui/rmlui.md): the RmlUi layer and its rules
- [../ui/hui-kit.md](../ui/hui-kit.md): the kit that draws the shelves
- [../console/networking.md](../console/networking.md): why the bundle server runs on the host
- [../build/building.md](../build/building.md): FFmpeg profiles
