# Codec support

State of **v0.10.0** (released). Changes made after the release are not
included here.

Verified = played on a real PS5 and confirmed decoding, not inferred from the
build configuration.

---

## Video — hardware (sceVideodec2)

| Codec | Profile | Max | Verified |
|---|---|---|---|
| H.264 / AVC | High (100) | 4K, level 5.1–5.2 | yes |
| HEVC | Main (1) | 4K, level 5.1 | yes |
| VP9 | Profile 0 | 4K, level 5.1 | yes |
| HEVC | Main10 (2) | 1080p only | yes |
| VP9 | Profile 2 (10-bit) | 1080p only | no |

Five resident decoders, one per row, created once at start-up.

## Video — software (FFmpeg)

| Codec | Max | Verified |
|---|---|---|
| AV1 (libdav1d) | 1080p | yes |
| MPEG-2 | 1080p | yes |
| MPEG-4 Part 2 | 1080p | no |
| VP8 | 1080p | no |
| H.264 / HEVC / VP9 | 1080p | yes |

Software decode above 1080p is refused; it does not fit the title's memory
budget.

## Video — not supported

| Codec |
|---|
| AV1 above 1080p |
| HEVC 10-bit above 1080p |
| VC-1 |
| WMV |
| Theora |
| ProRes / DNxHD |

---

## Audio

| Codec | Channels tested | Verified |
|---|---|---|
| AAC / AAC-LATM | 2, 5.1, 7.1 | yes |
| AC-3 | 5.1 | yes |
| E-AC-3 | 5.1, 7.1, Atmos bed | yes |
| DTS (core) | 5.1 | yes |
| DTS-HD High Resolution | 7.1 | yes |
| DTS-HD Master Audio | 5.1 | yes |
| DTS-X | 7.1.4 bed | yes |
| FLAC | 5.0, 7.1 | yes |
| ALAC | — | no |
| Opus | 2 | yes |
| Vorbis | 2 | yes |
| MP3 / MP2 | 2 | yes |
| PCM (s16le/s16be/s24le/f32le) | — | no |

## Audio — not supported

| Codec | Note |
|---|---|
| Dolby TrueHD | no decoder in the build; file does not play |
| Dolby Atmos in TrueHD | same |
| WMA | no decoder |
| WavPack | no decoder |
| Monkey's Audio (APE) | no decoder |
| TTA | no decoder |

## Audio output

| Property | Value |
|---|---|
| Port | 8 channel (7.1) when source > 2ch, else stereo |
| Format | S16, 48 kHz |
| Bitstream passthrough | none — all sources are decoded to PCM |
| Object audio | bed only; Atmos/DTS-X objects are not rendered |

---

## Subtitles

| Format | Verified |
|---|---|
| SRT / SubRip | yes |
| ASS / SSA | yes |
| MOV text (mp4) | no |
| PGS (Blu-ray bitmap) | no |
| DVB subtitles | no |
| DVD subtitles | no |

Non-ASCII characters render as `?` (issue #35).

---

## Containers

| Container | Verified |
|---|---|
| MP4 / MOV | yes |
| Matroska (MKV) | yes |
| AVI | no |
| MPEG-TS | no |
| MPEG-PS | no |
| Ogg | no |
| WAV | yes |
| FLAC | yes |
| MP3 / AAC / AC-3 / E-AC-3 / DTS (raw) | partial |
| Image (image2) | yes |

---

## Practical limits

| Case | Result |
|---|---|
| 4K30 H.264 | real time |
| 4K60 H.264 | ~14 fps, breaks up |
| 4K HEVC 8-bit | real time |
| 4K HEVC 10-bit | not played — hardware declines, software refuses above 1080p |
| 4K AV1 | not played — needs ~135 MB against ~125 MB available |
| 1080p AV1 | real time |

---

## Sources

| Claim | Where it comes from |
|---|---|
| Hardware decoder table | `g_codec[]`, `media/src/evo_vdec_native.c` |
| FFmpeg decoder lists | `--enable-decoder=` in `scripts/build-ffmpeg.sh` |
| 1080p software cap | `PlaybackController.cpp`, measured allocator telemetry |
| Audio verification | hardware sweep, 2026-09-23, `test_files_aud_vid` |
