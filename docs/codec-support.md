# Codec support

State of **v0.10.0** (released). Changes made after the release are not
included here.

**Verified** = played on a real PS5 and confirmed decoding, not inferred from
the build configuration.

---

## Video

| Codec | Decoder | Max | Verified |
|---|---|---|---|
| H.264 / AVC High | hardware | 4K30 real time; 4K60 ~14 fps | yes |
| HEVC Main (8-bit) | hardware | 4K, real time | yes |
| HEVC Main10 (10-bit) | hardware | 1080p | yes |
| VP9 Profile 0 | hardware | 4K | yes |
| VP9 Profile 2 (10-bit) | hardware | 1080p | no |
| AV1 | software (libdav1d) | 1080p, real time | yes |
| MPEG-2 | software | 1080p | yes |
| MPEG-4 Part 2 | software | 1080p | no |
| VP8 | software | 1080p | no |
| HEVC 10-bit above 1080p | none | — | — |
| AV1 above 1080p | none | — | — |
| VC-1 | none | — | — |
| WMV | none | — | — |
| Theora | none | — | — |
| ProRes / DNxHD | none | — | — |

Five resident hardware decoders, one per hardware row, created once at
start-up.

Software decode above 1080p is refused by default — but the reason originally
given for it (a 4K frame needing ~135 MB against ~125 MB available) was based
on a flexible-memory ceiling that was never measured and turned out to be
wrong. The title has **448 MB** of flexible memory, 144–192 MB free at decoder
open, and 4K 10-bit AV1 decodes faster than real time once the guard is lifted
(`/mnt/usb0/evo_sw_4k`). See [hardware/memory-budget.md](hardware/memory-budget.md).

---

## Audio

| Codec | Decoder | Channels tested | Verified |
|---|---|---|---|
| AAC / AAC-LATM | software | 2, 5.1, 7.1 | yes |
| AC-3 | software | 5.1 | yes |
| E-AC-3 | software | 5.1, 7.1, Atmos bed | yes |
| DTS (core) | software | 5.1 | yes |
| DTS-HD High Resolution | software | 7.1 | yes |
| DTS-HD Master Audio | software | 5.1 | yes |
| DTS-X | software | 7.1.4 bed | yes |
| FLAC | software | 5.0, 7.1 | yes |
| ALAC | software | — | no |
| Opus | software | 2 | yes |
| Vorbis | software | 2 | yes |
| MP3 / MP2 | software | 2 | yes |
| PCM s16le/s16be/s24le/f32le | software | — | no |
| Dolby TrueHD | none | — | file does not play |
| Dolby Atmos in TrueHD | none | — | file does not play |
| WMA | none | — | — |
| WavPack | none | — | — |
| Monkey's Audio (APE) | none | — | — |
| TTA | none | — | — |

Output is an 8-channel (7.1) port whenever the source has more than two
channels, otherwise stereo; S16 at 48 kHz. Everything is decoded to PCM —
there is no bitstream passthrough to a receiver, and object audio is not
rendered, so Atmos and DTS-X play their bed only.

---

## Subtitles

| Format | Decoder | Verified |
|---|---|---|
| SRT / SubRip | software | yes |
| ASS / SSA | software | yes |
| MOV text (mp4) | software | no |
| PGS (Blu-ray bitmap) | software | no |
| DVB subtitles | software | no |
| DVD subtitles | software | no |

Non-ASCII characters render as `?` (issue #35).

---

Containers: MP4/MOV, Matroska, AVI, MPEG-TS, MPEG-PS, Ogg, WAV, FLAC, and raw
MP3/AAC/AC-3/E-AC-3/DTS streams. MP4, MKV, WAV and FLAC are verified; the rest
are enabled but untested.

Sources: hardware decoder rows from `g_codec[]` in
`media/src/evo_vdec_native.c`; software rows from `--enable-decoder=` in
`scripts/build-ffmpeg.sh`; the 1080p cap from allocator telemetry in
`PlaybackController.cpp`; audio verification from a hardware sweep on
2026-09-23 against `test_files_aud_vid`.
