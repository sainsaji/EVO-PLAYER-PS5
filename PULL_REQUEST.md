# HDR colour, native Stremio account, PGS on streams and TLS verification

This PR brings three releases built and used on an LG G5 OLED and a PS5 Pro
(FW 13.60), **rebased on `main` at a54dcf5**. Each one is its own commit, so
you can review them, cherry-pick them or drop them one at a time:

| Commit | Version | What it does |
|---|---|---|
| `0.13.1` | v0.13.1 | Exact HDR/SDR colour conversion, Dolby Vision per-shot peak mapping, online subtitles, language preferences |
| `0.14.0` | v0.14.0 | Native Stremio account (link-code sign-in, account addons, Continue Watching sync), ranked automatic stream choice, PGS on network streams, network and caching speed-ups |
| `0.14.1` | v0.14.1 | HTTPS certificate verification, Clang Static Analyzer and `-Wextra` fixes, dead code removed |

**Testing status, please read.** After the rebase, the branch builds with the
pinned toolchain (`scripts/build-evoplayer.sh`, `scripts/package-app.sh --ffpfsc`,
standard and `--pro-memory`). The host tests pass, 36/36 (4 of them new), and
the kit screens render with `tools/hui_preview.sh`. On hardware, 0.13.1 was tested on
a PS5 Pro: the HDR colour fix was confirmed visually, and online subtitles
work. 0.14.0 and 0.14.1 have **not** been verified on a console yet; the
hardware checklist is at the end of this file.

The version bumps (`VERSION`, `contentVersion`, the changelog entries) belong
to the fork. Drop or renumber them as you like.

**Aligned with `main` during the rebase:**
- Settings file: line 29 stays yours (phone & log port). Ours moved to
  30 HDR peak, 31 audio language, 32 subtitle language and 33 play best addon
  stream. The round-trip and legacy tests cover all four.
- The commit message of 0.14.0 still mentions a "Choose stream" row in the
  Triangle menu. It was dropped in the rebase, because your detail page
  already has CHOOSE STREAM (`sel.force_pick`). With PLAY BEST ADDON STREAM
  on, PLAY starts the top-ranked source, and CHOOSE STREAM opens the same
  ranked list. The rank also fills `width`, `height`, `video_range`,
  `video_codec` and `size_bytes`, so your picker's pills work for addon
  sources.
- Your page-chrome redesign already removed the repeated provider name and the
  footer label. Of our shelf changes, only the removal of the item count next
  to each shelf title is left.

---

## 0.13.1: picture and subtitles

### Picture
- **Fixed: HDR10/HLG/Dolby Vision base layers were about 12 % desaturated.**
  The BT.2020 limited-range conversion skipped the chroma gain (1023/896).
  `tools/gen_video_pipes.py` now emits every Y'CbCr conversion with exact
  range and bit-depth constants.
- **Fixed: 8-bit HD/4K SDR used BT.601.** BT.709 is now chosen above
  576 lines (new pipes `video_yuv_nv12_709`, `video_yuv_planar_709`).
- **New: HDR PEAK setting** (Off/800/1000/1500/2000/3000 nits). Dolby Vision
  is fitted shot by shot from the RPU's L1 metadata (profiles 5/7/8; 7/8 are
  parsed map-only), HDR10 from MaxCLL and the mastering display. New pipe
  `video_yuv_p010_pq_dyn`; helpers `evo_agc_runtime_set_hdr_peak()` and
  `evo_agc_runtime_set_hdr_static_peak()`.

### Subtitles and languages
- **New: online subtitles from Stremio addons** (`addons/src/evo_online_subs.c`).
  EVO asks every addon with a `subtitles` resource, and falls back to
  OpenSubtitles v3 like the Stremio app does. The subtitle picker lists them
  as ONLINE rows.
- **New: AUDIO LANGUAGE and SUBTITLE LANGUAGE settings**
  (`media/src/evo_langpref.c`). They pick the audio track and subtitle track
  at open.
- **Fixed:** a forced (signs-only) track flagged default won over the full
  dialogue track.
- **Fixed:** the AUTO SUBTITLES setting was saved but never read at open.
- **Improved:** the subtitle picker shows format, FORCED and the cue count.
- The UI kit gained a `ComplexText` seam (`set_fallback`, `set_complex`,
  `GlyphQuad.complex`). The kit's original repo is gone, so this was applied
  to the `Yahballon/ps5-homebrew-ui` fork; the diff is in
  `ps5-homebrew-ui-complextext.patch` (attached separately).

Settings file lines 30-32 were added, with round-trip and legacy tests.

---

## 0.14.0: Stremio, natively

### Stremio account (`addons/src/evo_stremio_account.c`, new)
This speaks the API that stremio-core uses (`api.strem.io`). The request
shapes were checked against stremio-core's sources, and the link API against
the live service.
- **Sign-in by link code:** `link.stremio.com/api/v2/create`, then `read`
  every 3 s. The code renews itself after 4.5 min. No password is typed on
  the pad. It reuses the Quick Connect slots (`qc_start` / `qc_poll` /
  `qc_cancel`).
- **The account's addons:** `addonCollectionGet`, using the manifests the
  account stores, so there is no fetch per addon. The app's local
  streaming-server addons (127.0.0.1) are skipped.
- **Library and Continue Watching:** `datastoreGet` on `libraryItem`. Continue
  Watching uses stremio-core's rule: `type != other`, `(!removed || temp)`,
  `timeOffset > 0`.
- **Progress:** `datastorePut` of the changed item, keeping every field EVO
  does not know. Pushed at most once a minute and immediately on stop;
  counted as watched past 90 %. A title not in the library yet is added as a
  `temp` item, as Stremio does.
- Saved to `stremio_account.json`, `stremio_addons.json` and
  `stremio_library.json`, so Home opens with no network.

### Provider (`provider_stremio.c`)
- The account's addons come first, then the ones added on the console, then
  the built-in Cinemeta. `account_apply()` rebuilds the list only when the
  account's URLs changed, and only from `list_root`, so addon indices inside
  on-screen ids stay valid.
- New root shelves `l:cw` (Continue Watching; episodes resume at
  `timeOffset`) and `l:lib`.
- `CAP_PROGRESS` / `report_progress`; sign-in and sign-out slots.
- `AD_MAX` raised from 12 to 24, `AD_URL` from 512 to 1792 (AIOStreams URLs
  carry their configuration), `AD_CHOICES` from 16 to 48.

### Automatic stream choice
- `rank_stream()` scores each source from what the addon prints, in this
  order: debrid cache state (Torrentio `[TB+]`/`[RD+]`, the AIOStreams/Comet
  bolt; uncached last), resolution, Remux/BluRay/WEB-DL, then Dolby Vision
  with an HDR10 base > HDR10+ > HDR10 > DV-only. Size is the bitrate proxy.
  Cam, 3D and uncached sources go to the bottom.
- When the list is full, the lowest-scored source is replaced; the list is
  sorted when the lookup ends.
- **New setting PLAY BEST ADDON STREAM** (line 33, default on): source 0
  plays at once, and the existing fallback tries the next ones. The detail
  page's CHOOSE STREAM opens the ranked list.

### Subtitles
- **Fixed: PGS never showed on debrid or media-server streams.** Pictures are
  decoded at demux time, up to minutes ahead of playback on a stream, and the
  ring held 4, so each was evicted before it was due. The ring now holds 192
  palette-indexed pictures within a 96 MB budget, expanded to RGBA only when
  drawn, and evicts the ones already shown first. Text cues go from 256 to
  1024.

### Network and speed
- `evo_net`: a new `evo_net_request_async_slow()` with a 25 s read timeout,
  used for stream and subtitle lookups (AIOStreams and debrid checks were cut
  off at 6 s). Queue raised from 32 to 128, page workers from 4 to 8.
- A disk cache for manifests (24 h), catalog page 0 (6 h) and meta (24 h), in
  `<data>/addon_cache/<fnv1a64>.json` with an `EVOCACHE <unix>` header. A
  stale copy is served when the network fails.

### Interface
- Shelves: no item counter next to each shelf title.

---

## 0.14.1: hardening and clean-up

### Security
- **HTTPS certificates were never verified** (`SSL_VERIFY_NONE` by default),
  so anyone on the path could read the Stremio authKey and the debrid keys
  inside addon URLs. EVO now verifies against the Mozilla roots in
  `assets/certs/cacert.pem` (curl.se extract, SHA-256 checked, MPL-2.0, listed
  in `docs/proprietary.md`), checks the hostname with `SSL_set1_host`, and
  requires TLS 1.2 or newer. The roots are loaded through the app's own stdio
  into the store.
  - Exempt: home-network hosts (private, loopback, link-local and CGNAT IPv4;
    names without a dot; `.local`, `.lan`, `.home.arpa`, `.internal`), and
    validity dates (`X509_V_FLAG_NO_CHECK_TIME`, because consoles that never
    sync their clock exist).
  - If the bundle cannot be read, EVO logs it and connects as before.
  - Tested on the host against a TLS-intercepting gateway: refused with the
    Mozilla roots, accepted with the gateway's CA.
  - `.gitignore` gets one exception for this public `.pem`.
- The saved Stremio key is validated when it is loaded, not only at sign-in.

### Fixes from the Clang Static Analyzer and cppcheck
- `evo_keyboard.c`: a modulo by zero on Left/Right for a row with no keys.
- `provider_iptv.c`: `tag_attr()` left `out` unwritten on a miss, and callers
  tested uninitialised buffers.
- Two `%llu` traces were printing a `uint64_t`.
- Most analyzer and cppcheck reports were false positives (cross-TU cJSON
  checks, invariants the analyzer cannot see) and were left alone.

### Clean-up
- `evo_bt()` outside the app module is now an empty inline
  `printf`-attributed call instead of `((void)0)`. Traces are format-checked
  in every build, and variables kept only for a trace no longer read as
  unused.
- Removed unused code: `iptv_ui_bundle_url`, `find_usb_playlist` and
  `kUsbPlaylistNames`, `audio_get_sample`, `audio_float_to_s16`,
  `prospero_subtitle_cycle_track` and `prospero_subtitle_collect_tracks`,
  `split_utf8`, a duplicate `kind_word`/`kCardW`/`kGap`, and dead locals.
- Removed `projects/evoplayer/evo_changelog.h`: an identical twin of
  `include/evo_changelog.h` that every include path resolves past.
  `docs/build/release.md` is updated.
- The PS5 build is warning-free with
  `-Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers`.

---

## Hardware checklist (not yet run for 0.14.x)
1. Stremio tile → OPTIONS → **Sign in with Stremio**: the code appears; enter
   it at link.stremio.com; the account's addons and the Continue Watching /
   Library shelves appear.
2. Open a title and press PLAY: the top-ranked source starts. CHOOSE STREAM
   shows the ranked list, cached 4K Remux / DV first.
3. Stop halfway through an episode: it is in Continue Watching on another
   Stremio device, at the same position.
4. A file with PGS subtitles over a debrid stream: the subtitles show.
5. HTTPS: Stremio, an addon, Plex/Jellyfin on the LAN and the updater all
   still work. `evo.log` shows `TLS verification on (118 root certificates)`.
6. The virtual keyboard: Left/Right on every row.
