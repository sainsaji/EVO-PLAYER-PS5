# YouTube in EVO's player

EVO plays YouTube in its own player two ways. Both live in the cast receiver
(`src/evo_cast_receiver.c`, port 8765) and `core/src/services/CastService.cpp`.

Every way in ends in `CastService`'s launch handler with a video id, and
resolves the same way: the full-length route first, then the 60 s route.

| Started by | Notes |
|---|---|
| YouTube app on a phone (DIAL launch, then Lounge for later videos) | Needs Settings → CAST TO EVO PLAYER on; off sends the cast to the system browser instead. The phone's start time is kept |
| Settings → PLAY A YOUTUBE LINK | Keyboard prompt; the phone keyboard link works for pasting |
| The phone page (`<ps5>:9780/input`), "Play a YouTube link" box | `POST /youtube` with `X-EVO-Input`, same guard as the keyboard |
| `curl -d "<link>" http://<ps5>:8765/cast/url` | For scripts |

| Route | Client | What plays | Limit |
|---|---|---|---|
| Full length (first) | WEB_EMBEDDED_PLAYER | HLS up to 1080p60 (formats 91-94, 300, 301) | None found; see "When it breaks" |
| 60 s (fallback) | ANDROID, then ANDROID_VR | DASH up to 2160p (local MPD from the sidx) | Stops at about **60 s**: no PO token, so googlevideo answers 403 past that |

## Full-length links (`addons/src/evo_yt_link.c`)

The web_embedded HLS manifest plays in full without a PO token, but its
segments answer 403 until the `/n/<challenge>/` part of the manifest path is
replaced by the value YouTube's player script computes. EVO computes it the way
`yt-dlp --js-runtimes quickjs` does:

1. `iframe_api` → player version → `/s/player/<id>/player_ias.vflset/en_US/base.js`
   → `signatureTimestamp`.
2. `/embed/<video>?html5=1` (Referer: a non-YouTube site) → its `ytcfg`. The
   request copies `INNERTUBE_CONTEXT` (it carries
   `thirdParty.embeddedPlayerContext`), `INNERTUBE_CONTEXT_CLIENT_VERSION`,
   `VISITOR_DATA` and `WEB_PLAYER_CONTEXT_CONFIGS…encryptedHostFlags`. A
   hand-built context gets "This video is unavailable". No cookies needed.
3. `youtubei/v1/player` → `hlsManifestUrl`.
4. yt-dlp's solver (`third_party/yt-dlp-ejs`, embedded by
   `tools/gen_ytjs_blob.py`) runs in QuickJS-NG (`addons/src/quickjs-amalgam.c`)
   on its own 4 MiB thread and returns the new `n`. Its "preprocessed player"
   output is cached in `/data/evoplayer/ytjs_player.cache`, keyed by player
   version.

Cost on the console (PS5 Pro): first link after a player change about 6.5 s of
JavaScript (3 MB script), later links under 1 s in all.

Log lines start with `yt-link:`; a failure names the step (`embed config`,
`not playable: <reason>`, `solver FAILED: <error>`).

## When it breaks

- **`solver FAILED`** after YouTube ships a new player: update the solver. Copy
  `lib.min.js` and `core.min.js` from a newer `yt-dlp-ejs` release into
  `third_party/yt-dlp-ejs/`, run `python tools/gen_ytjs_blob.py`, rebuild. Test
  on the PC first: `yt-dlp --js-runtimes quickjs:<qjs>` with the same release.
- **`not playable: ... unavailable`**: compare EVO's request with
  `yt-dlp --print-traffic --extractor-args "youtube:player_client=web_embedded"`.
- **No `hlsManifestUrl`, or the segments 403 with a correct `n`**: YouTube asks
  for a PO token on web_embedded HLS too. The route is closed; links fall back
  to the 60 s phone-cast route.
- yt-dlp warns that YouTube may move web_embedded to SABR-only streaming
  (yt-dlp issue 12482); that would also close it.

## Seeking on the 60 s route

The fallback route's MPD `SegmentList` carries `timescale` and the average segment
`duration` from the sidx, which FFmpeg's `dash_seek` needs (it ignores a
`SegmentTimeline` inside a `SegmentList`). Without them a seek decoded from the
start of the file.
