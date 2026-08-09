# Baseline defects — ProsperoPlayer on 12.70

> ## Both of these are FIXED in EVO Player.
>
> This document describes the **unmodified upstream baseline**, kept as the
> record of what was wrong and why. It is history, not a backlog — read it for
> the root-cause analysis, not for work to pick up.
>
> | Defect | Status in EVO Player |
> |---|---|
> | 1. No surround output | **Fixed.** `sceAudioOutOpen` with `S16_8CH` when the source is multichannel, `AV_CHANNEL_LAYOUT_7POINT1` on the resampler, stereo fallback retained. `main.c`, search `want_surround`. |
> | 2. Screen tearing | **Fixed.** `EVO_FLIP_SYNC` in `pp/src/pp_videoout.c` retires a buffer only once `sceVideoOutGetFlipStatus` reports its `flipArg`, replacing upstream's 32 ms timer. |
>
> **The codec set is wider than any message in the app suggests.** The build
> enables and links `aac ac3 eac3 dca mp3 mp2 flac opus vorbis alac pcm_*`
> for audio and `h264 hevc vp9 vp8 mpeg2video mpeg4 av1 mjpeg` for video, plus
> `truehd`/`mlp`. Verified with `nm` against both `libavcodec.a` and the
> shipped ELF. If playback reports a codec as unsupported, check the message
> against that list before believing it.
>
> Mistaking this file for open work has already cost one session's planning
> time. If you are looking for what is actually outstanding, see
> [`ui-handoff.md`](ui-handoff.md) and [`gpu-notes.md`](gpu-notes.md).

Established 2026-08-09 by running the unmodified upstream build
(`PS5MediaPlayerPRO.elf`, 43 MB) on a real console against a 29-file
multichannel test set, then tracing each symptom to a line in upstream's
source.

**The baseline works.** Video and audio play. These are the two defects
observed, both with confirmed root causes. Neither is a build or environment
problem — they are missing implementation, which made them EVO Player's first
two jobs.

---

## 1. No surround output — everything is downmixed to stereo

**Observed:** all 29 test files play, none produce surround. Not silence —
correct stereo.

**Root cause.** Two places, both hardcoded:

`main.c:1850`
```c
AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;

swr_alloc_set_opts2(&prospero_audio_swr,
                    &output_layout,          /* <-- always 2.0 */
                    AV_SAMPLE_FMT_S16,
                    prospero_audio_output_rate, ...);
```

`main.c:3398`
```c
#define PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1
sceAudioOutOpen(..., 48000, PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
```

So libswresample downmixes every layout to 2.0, and the AudioOut port is only
ever opened with two channels. Surround was never implemented.

This also reframes the "E-AC3 silent" report in upstream's README: the decoder
is fine, and E-AC3 content is audible here. Whatever that report was about, it
is not a missing codec — our minimal FFmpeg build contains `eac3`, and the
content plays.

**Fix.** AudioOut supports 8 channels:

```c
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH     2
#define SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_8CH   5   /* verify before use */
```

1. Open the port with `S16_8CH` when the source has more than two channels,
   falling back to stereo otherwise. Keep the fallback — a 2.0 source should
   not be upmixed into a 7.1 port.
2. Set the swresample output layout to `AV_CHANNEL_LAYOUT_7POINT1` to match.
3. **Confirm the channel order.** FFmpeg's 7.1 order is
   `FL FR FC LFE BL BR SL SR`. The PS5's expected interleave order is *not
   verified* and may differ — getting it wrong puts the centre channel in a
   surround speaker, which is worse than stereo. Test with
   `Atmos test tones 1000Hz V4.mp4`, which is designed to identify individual
   speakers.
4. Grain size is per-channel frames, not bytes — check that
   `sceAudioOutOutput` is fed `grain * 8 * sizeof(int16_t)`.

Objects-based Atmos (`Dolby Atmos 24.1.13 with 7.1.2 beds and objects.mp4`) is
a separate problem and needs `sceAudioOut2`'s 3D audio context, not a plain
8-channel port. Treat that as a later milestone.

---

## 2. Screen tearing during motion

**Observed:** visible tearing as the camera moves; static scenes are clean.

**Root cause.** `pp/src/pp_videoout.c` creates a flip event queue and registers
for flip events:

```c
rc = sceKernelCreateEqueue(&vo->equeue, "pp_videoout_flip");     /* :152 */
(void)sceVideoOutAddFlipEvent(vo->equeue, vo->handle, NULL);     /* :154 */
```

...and then **never waits on it**. `sceKernelWaitEqueue` does not appear
anywhere in the project. Neither does `sceVideoOutGetFlipStatus` nor
`sceVideoOutWaitVblank`. Flips are submitted (`:236`, `:270`) and the next
frame is written immediately.

So the CPU starts writing a buffer that may still be scanning out. That is
exactly a tear, and exactly why static scenes look fine — when consecutive
frames are identical, writing into the live buffer is invisible.

**Fix.** Wait for flip completion before reusing a buffer:

```c
struct kevent ev;
int out = 0;
sceKernelWaitEqueue(vo->equeue, &ev, 1, &out, NULL);
```

This is what `ps5-payload-dev/SDL`'s backend does, and the queue upstream
already builds is the right mechanism — it is simply unused.

Consider triple buffering as well. With two buffers the renderer must stall on
every flip; a third lets frame N+1 be prepared while N is on screen, which
matters once GPU conversion is in the pipeline.

> Note for `projects/videoout_test`: it uses `sceVideoOutWaitVblank`, which
> paces to the refresh but does **not** guarantee the previous flip retired.
> It is adequate for a static test pattern and should not be copied into the
> player.

---

## Test set

`/mnt/usb0/test_files_aud_vid/` — 29 files, **all multichannel**, no stereo
source at all. Covers AC-3, E-AC-3, TrueHD, Atmos (bed + objects), DTS, DTS-HD
MA, DTS-X, AAC 5.1/7.1, FLAC 5.0/7.1, LPCM 7.1.

Useful ones:

| File | Why |
|---|---|
| `Atmos test tones 1000Hz V4.mp4` | identifies individual speakers — use this to verify channel order |
| `Dolby Digital AC-3 5.1.mp4` | simplest multichannel baseline |
| `Dolby Digital Plus E-AC-3 5.1.mp4` | the format upstream flagged |
| `AAC 5.1.mp4` | upstream's "recommended" codec, multichannel |
| `FLAC 5.0.flac` / `FLAC 7.1.flac` | lossless, odd layouts |
| `LPCM 7.1.wav` | no decoder involved — isolates the output path from decoding |
| `DTS-HD MA 5.1.mkv` | DTS plus MKV demux |

`LPCM 7.1.wav` is the highest-value first test for the surround work: if that
cannot reach eight speakers, the problem is entirely in the output path and no
decoder is involved.
