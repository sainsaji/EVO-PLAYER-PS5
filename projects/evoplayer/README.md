# projects/evoplayer

The EVO Player fork. **Empty on purpose.**

Do not start here. The order matters:

1. `projects/prosperoplayer/` must build **and run on hardware** first — that
   establishes the baseline. `./scripts/build-prosperoplayer.sh`
2. Confirm the baseline's *existing* playback actually works on your 12.70
   console. Record it in [../../docs/validation.md](../../docs/validation.md).
3. Only then copy the upstream tree here and start changing it.

```bash
cp -r projects/prosperoplayer/. projects/evoplayer/
rm -rf projects/evoplayer/.git
git add projects/evoplayer
```

Unlike `projects/prosperoplayer/` (git-ignored, always a pristine upstream
checkout), this directory **is** committed — it is the project's own source.

## First targets

From ProsperoPlayer's own README of known limitations, all of which are
codec/integration problems rather than UI ones:

- E-AC3 / Dolby Digital Plus silent
- FLAC / OGG support varies
- 4K playback file-dependent
- DTS not mentioned at all

The minimal FFmpeg profile already builds `eac3`, `dca`, `flac`, `opus`,
`vorbis` and `alac` (see `output/logs/ffmpeg-7.0.1-minimal-codecs.txt`), so if
these still fail the problem is in the player's decode/resample/output path,
not a missing codec. That is exactly the kind of question the `minimal` vs
`full` FFmpeg profiles are designed to settle.

Remember `AudioOut` is fixed at **48 kHz stereo S16** — anything decoded at
another rate or layout must go through `libswresample` first. That is a strong
candidate for the "silent E-AC3" symptom.

## Keep experiments out of here

Hardware decoding, the GPU renderer and libSceAvPlayer each have their own
project (`decoder_test`, `yuv_gpu_test`, `avplayer_test`) so a dead end in one
costs nothing elsewhere. Integrate only what is proven.
