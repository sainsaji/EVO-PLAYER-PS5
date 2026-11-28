# Roadmap

What is open, grouped by area, as of **2026-11-08** (v0.13.6 prepared).
GitHub is the live source: `gh issue list --state open`, or the "EVO Player Roadmap"
project board ([project-tracking.md](project-tracking.md)). This page is the map across
them. Each issue body ends with a "References & sequencing" block naming the docs and files
to read first.

To implement a story: open its issue, read the docs it names, then the files it names, then
go. Check [../architecture/architecture.md](../architecture/architecture.md) for where the
code lives and [../build/validation.md](../build/validation.md) for how to prove it.

The v0.8.0 to v1.1.0 milestones are all closed. v0.11.0 still lists four issues (#87, #48,
#47, #43) that were never moved; the milestone no longer plans anything. Priority labels
are `priority: medium` and `priority: low`; `hw-verify-pending` marks code that is in but
not yet seen on a console.

## Landed in v0.13.6

Playback stutter fixes (zero I/O on render thread, background remote worker, no periodic USB
fsync during playback, on-demand drive detection), FLAC cover art freeze fix and embedded album art
on the Now Playing card (#232), experimental ProsperoWeb browser composited via GPU broker on Mesa
with sharp 2x 4K rendering (#237), Settings → Experimental → WEB BROWSER toggle with dynamic rail
syncing, and ShadowMount launch stability improvements. See [../../CHANGELOG.md](../../CHANGELOG.md).

## Landed in v0.13.5

SMB shares with a step-by-step Add Server flow and Online / Offline status (#213, #226),
read-only NTFS drives (#217), cover art on FTP and SMB (#230), a user-settable phone and
log port (#227), the movie and show detail page (#221), the resume prompt (#218), skip
intro and credits, native Stremio sign-in with a ranked automatic stream, true HDR colour
and HDR PEAK, TLS certificate checks, and the phone and PC log viewer with Report a
problem (#216, #229). See [../../CHANGELOG.md](../../CHANGELOG.md).

## Landed in v0.13.0

In-app updater (#120), volume boost (#166), audio delay (#170), the playback diagnostics
panel (#167), the network buffer setting (#199), a preview card for every setting (#158),
stream and audio pickers with pills and a details card and a busy card during a switch,
Pro memory mode (#143), faster console testing (`set`, `focus`, `verify`, per-feature
checks, #201), a pad scrub that always commits, backing out of playback keeping the
selection on the played item (#214). See [../../CHANGELOG.md](../../CHANGELOG.md).

## Verification and diagnostics

| # | Story |
|---|---|
| #209 | Hardware verification pass: sync, video, audio, IPTV, providers, text (`priority: medium`, `hw-verify-pending`) |
| #122 | Hardware-test the Nuvio/Stremio port fixes: HEVC cleaning, seek decoder renew, parallel reader, stream headers |
| #198 | Startup failures are visible: boot phase and reason written where users can read them (`medium`) |
| #216 | Network, provider and FTP failures leave no trace in `evo.log` |
| #113 | On-console test runner that reads PASS/FAIL back (`medium`). Partly met by `evo-remote.sh verify` and `tools/checks/` |
| #125 | Playback backend: host tests and a split `decode_one`, so a fix stops breaking something else |

## Playback

| # | Story |
|---|---|
| #188 | Deinterlacing for 1080i IPTV and broadcast files (`medium`) |
| #132 | Brightness flicker during playback, worse with subtitles on (bug) |
| #160 | The seek preview frame does not show on the player screen (provider streams; unreliable elsewhere) |
| #151 | Player OSD: speed, ends-at time, audio sync, next/previous, repeat, buffer bar, sleep timer, frame step |
| #164 | Lower quality automatically on sustained buffering, then climb back |
| #177 | Keep audio playing while browsing other screens |
| #200 | DVD and Blu-ray ISO playback with working menus |
| #215 | Local playback start and FTP browsing block the UI thread |

## Audio

| # | Story |
|---|---|
| #47 | Native audio decode and bitstream passthrough (`medium`). Passthrough shipped in 0.12.0 and AAC/MP3 decode on `libSceAudiodec`; the issue remains for the rest ([../hardware/audio.md](../hardware/audio.md)) |
| #163 | AAC passthrough and a split audio reader broke A/V sync and were rolled back; redo in measured steps |
| #178 | A network stall must not flush the audio queue; find out how much audio EVO drops (`medium`) |
| #210 | Audio processing: loudness, night mode, downmix, calibration, room EQ, bass, virtual surround |

## Picture and GPU

| # | Story |
|---|---|
| #119 | Deep Blacks shader (`medium`). In as Settings → Experimental → Deep Blacks with a held-frame comparison (`evo-remote.sh dbcompare`); the issue is still open |
| #123 | A live-action AI upscaler network: ship FSRCNNX (licence checked) (`medium`) |
| #87 | Analytical anti-aliasing and shadows for UI geometry in the AGC shaders (`medium`) |
| #244 | Per-video HDR/SDR and 24p output switching: research, not solved ([../hardware/hdr-sdr-switching.md](../hardware/hdr-sdr-switching.md)) |
| #211 | Decoder, memory, upscaler and test-suite follow-ups |

## Subtitles and text

| # | Story |
|---|---|
| #43 | Render styled `.ass`/`.ssa` subtitles: position and colour (investigate) |
| #112 | Faster subtitle auto-sync: reuse the played audio, stop early, cache results (`medium`) |
| #128 | Fonts for every script. Latin extended, Greek, Cyrillic, Hebrew, Arabic, Thai and Indic are in; CJK loads from `/data/evoplayer/fonts/`; emoji is not |

## Providers and storage

| # | Story |
|---|---|
| #92 | IPTV parity: per-stream headers, EPG auto-discovery, favourites and channel numbers, playlist refresh (`medium`) |
| #100 | Provider UI templates: standard D-pad-native screens a provider fills with data (`medium`) |
| #180 | A malformed playlist entry must not fail the whole playlist (bug, `medium`) |
| #179 | Live TV search and recordings |
| #176 | Trakt sync for Nuvio and Stremio (watched state and progress) |
| #213 | More network sources: SMB (landed in v0.13.5) and DLNA/UPnP |

## Interface and console

| # | Story |
|---|---|
| #212 | Interface polish: icons, virtual keyboard, setting previews, HDR brightness |
| #126 | Install the app in the Media tab |
| #48 | Re-probe controller vibration from the app module; a haptics channel (`hw-verify-pending`). The earlier "dead" finding predates the app module, so re-probe before citing it |
| #124 | Spike: wake the console after a power cut with a pretend Bluetooth controller ([../console/console-wake.md](../console/console-wake.md)) |

## Standing constraints

- One hardware path, the app module; FFmpeg stays the default and the fallback; hardware
  features are probed at run time.
- Every change that adds a setting extends the save/load round-trip test.
- No commits until the build, tests and a console check pass; one clean commit per change.
- Do not reopen the dead ends without reading why: payload-slot hardware decode, kernel
  `.text` sweeps, `sceVideoOutOpen` from a payload, killing `kstuff`
  ([../../CLAUDE.md](../../CLAUDE.md)).
