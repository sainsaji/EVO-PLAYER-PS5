# Video decode

How EVO turns a compressed stream into frames: the `sceVideodec2` hardware decoder
where it can, FFmpeg where it must. What is supported is in
[codec-support.md](codec-support.md); the ABI is in [videodec2-abi.md](videodec2-abi.md).

## Why it works at all

Hardware decode works only from the app module (`PPSA99039`): a registered title
with its own `param.json`, sandbox and user session. The same calls from an elfldr
payload or an hbldr slot got as far as `CreateDecoder` and then failed at the
decode ioctl with errno 5200, whatever the credentials. It was a process-context
limit, not a hardware, driver or signing one ([../architecture/app-module.md](../architecture/app-module.md)).
The three payload-era dead ends (kernel `.text` sweeps, `sceVideoOutOpen`, fuzzing
the decoder fd) stay on the never-retry list in `CLAUDE.md`.

## The seam

All video decode goes through `media/include/evo_vdec.h`:

```
evo_vdec_open(params, &chosen) → send(au) → receive(&pp_frame) → flush → close
```

Two backends implement it: `evo_vdec_ffmpeg.c` (pure: no clocks, no sleeps, no
globals) and `evo_vdec_native.c`. `evo_vdec_pref_resolve()` turns the Settings →
Video Decoder row into a request (`AUTO`: native for a codec whose decoder came up
this session, else FFmpeg; `NATIVE`: ask regardless; `FFMPEG`: software), and
`evo_vdec_open()` reports what it really built. A native open may decline a
stream and hand back FFmpeg. Pacing, A/V sync and presentation are the play
loop's job (`media/src/evo_playback.c`, `pp/`), never the decoder's. The cover
extractor goes through the same seam so a poster of a 4K HEVC file uses the
hardware decoder: FFmpeg's software HEVC faults, rather than fails, when it cannot
allocate a picture buffer.

## The resident decoders

`evo_vdec_native.c` keeps one decoder per mode, created at boot **before** the
self-unjail (`evo_vdec_native_probe()`), and reuses it for every file by
resetting it:

| Slot | Codec / profile | Output |
|---|---|---|
| AVC | H.264, up to High | NV12 |
| HEVC | HEVC Main | NV12 |
| VP9 | VP9 Profile 0 | NV12 |
| HEVC10 | HEVC Main10 | P010 (`PP_FRAME_NV12_10`) |
| VP92 | VP9 Profile 2 | P010 |

- **Sized to the stream, not the worst case.** Every slot comes up at 1920×1088 at
  boot (`EVO_VDEC_NATIVE_BOOT_4K=0`). Opening a larger stream tears the idle slot
  down and brings it up at the stream's size (up to 3840×2176, HEVC level 5.1, 12256 KB
  frames); closing puts it back. If the resize fails the previous size is restored
  and the caller gets its FFmpeg fallback. What this saves is **flexible** memory
  (the resident set once held about 180 MB of it permanently), the tighter pool.
- The claim that `libSceVideodec2` stops working after the credential swap, which is
  why decoders were once all created pre-unjail at maximum size, was measured and
  disproven (uid 1 → 0, a complete late bring-up succeeds). The GPU still has to be
  created first.
- Per-codec modes live in the `g_codec[]` table: codec type, profile, `max_level`
  for 1080p and 4K (AVC level ×10; HEVC `general_level_idc` ×30, so 123 is 4.1 and
  153 is 5.1; **a 4K surface at level 4.1 is an invalid configuration** and is
  refused at `QueryDecoderMemoryInfo` with `0x811d0200`), the bitstream filter,
  and the pipeline depth (4 for the 8-bit codecs, 1 for the 10-bit ones).
  A wrong `codec_type` or `max_level` faults instead of returning, so every
  bring-up stage is logged *before* it is entered; when Sony's code dies the last
  klog line is the only evidence.
- Rollback switches exist at build time: `package-app.sh --no-native-secondary`
  (AVC only), `--no-native-secondary-4k`, `--no-native-10bit`
  ([../build/packaging.md](../build/packaging.md)).

## Feeding it

- **Access units** are Annex-B. MP4/MKV streams go through `h264_mp4toannexb` or
  `hevc_mp4toannexb`; streams already in Annex-B pass through.
- **VP9** has no Annex-B: a compound superframe is rejected whole, so every packet
  goes through `vp9_superframe_split`, and hidden alt-ref frames are submitted for
  reference with their output suppressed.
- **Frame order.** `OutputInfo` carries no PTS, so the backend assumes display-order
  output with min-PTS pairing and a small PTS-sorted reorder window. If B-frame
  content misbehaves, build with `-DEVO_VDEC_NATIVE_DECODE_ORDER=1` and a larger
  `-DEVO_VDEC_REORDER_DEPTH`.
- **Open-GOP seeks.** After a random-access seek into an x265 open-GOP stream,
  `drop_undecodable_leading()` drops the RASL pictures; without it the decoder
  returns `out.error=1` on every access unit. After a flush, up to 32 decode errors
  are tolerated before the first picture, well below the playback layer's own
  16-failure streak that ends a stream which cannot decode at all.
- Input ring: four 8 MiB access-unit slots per decoder (about fifty times the
  largest 4K IDR ever observed). Output ring: eight slots of one frame.
- A decode call that never returns would wedge the app slot; the watchdog test hook
  is `/mnt/usb0/evo_vdec_hang` ([../console/diagnostics.md](../console/diagnostics.md#switches)).

## Fallback and 4K software decode

When the hardware declines (a profile, bit depth or size it cannot take, a slot that
failed to come up, a mid-stream fatal) the play loop reopens on FFmpeg. **Software
decode above 1080p is allowed by default** since #95: the malloc shim serves large
blocks from direct memory, and 4K 10-bit AV1 plays in real time. What is left is
speed; a codec the CPU cannot decode in real time at 4K plays slowly.
`/mnt/usb0/evo_no_sw_4k` switches it back off and shows an "UNSUPPORTED" toast.
Memory facts: [memory-budget.md](memory-budget.md).

FFmpeg itself is 7.1.1 (`minimal` profile). AV1 is libdav1d only; FFmpeg's own `av1`
decoder is a hardware-only wrapper and faults without hardware. Threading is 4
threads, frame threading for HEVC and slice threading for H.264.

## Colour, HDR and Dolby Vision

- Frames reach the GPU as NV12 or P010 and the AGC video pipelines do YUV→RGB, so
  there is no CPU converter. Pipelines exist per transfer function: SDR NV12 and
  planar, HDR10 (PQ) and HLG for NV12 and P010, plus PQ-output variants used while
  the scanout is HDR10 (`video_yuv_*.pipe`, generated by `tools/gen_video_pipes.py`).
- Settings → Video & Display → HDR OUTPUT: `AUTO` switches the display to HDR10 for
  HDR video (the TV goes black briefly at start and end); `OFF` tone-maps to SDR with
  a BT.2390 curve (BT.2020 to BT.709).
- **Dolby Vision Profile 5** is reshaped on the GPU from the RPU (`evo_dovi.c`), which
  removes the magenta and green cast. The RPU parser is FFmpeg-internal, so
  `evo_dovi.c` mirrors FFmpeg 7.1.1's `DOVIContext` and the build **fails on any other
  libavcodec** (`LIBAVCODEC_VERSION` 61.19).
- The curve is chosen from the stream's `color_trc`, not the decoder profile: an HLG
  file once looked right until it was checked against a PQ one with the same pattern.
- **Y'CbCr to R'G'B' is exact for range and bit depth** (`yuv_fn` in
  `tools/gen_video_pipes.py`): luma 16-235 / 64-940 and chroma 16-240 / 64-960
  around 128 / 512, with the chroma gain 255/224 or 1023/896. The BT.2020 paths
  (HDR10, HLG, both scanouts) used to skip the chroma gain, which left every HDR
  picture about 12 % less saturated than it was mastered.
- **8-bit SDR above 576 lines uses BT.709** (`video_yuv_nv12_709`,
  `video_yuv_planar_709`); BT.601 is kept for SD. An untagged HD stream is read as
  BT.709, the rule every player follows.

### HDR peak mapping

Settings → Video & Display → `HDR PEAK` (default Off). With a value set and a
HDR10 scanout, a picture brighter than that peak is fitted to it with a BT.2390
roll-off on max(R,G,B), one gain for all three channels so hue survives:

- **Dolby Vision, per shot.** Profiles 7 and 8 play their HDR10 base layer as
  before, but the RPU is now parsed too (map-only, `evo_dovi_parser_set_map_only`)
  for its L1 max; Profile 5 carries the same L1 peak in its reshape constants.
  The peak rides in fields the reshape maths does not use (`lohi[0..2][3]` of
  `evo_dovi_params`, see `evo_dovi.h`). Profile 7/8 draws through
  `video_yuv_p010_pq_dyn`, Profile 5 through `video_yuv_p010_dovi_pq_out`.
- **HDR10, static.** No RPU: the film's MaxCLL, else its mastering peak (the
  smaller when both are there), read at open
  (`evo_agc_runtime_set_hdr_static_peak`).
- A shot at or below the panel peak is passed through untouched, so the setting
  only ever changes highlights the TV would have had to compress itself.

The point is the same as a Dolby Vision display: the panel gets a picture it can
show, decided per shot, rather than one static guess for the whole film. Use it
with the TV's own dynamic tone mapping off (HGIG on LG). The playback panel shows
`DOLBY VISION > 1500 NITS PER SHOT` when it is mapping.

## Where it is measured

`evo.log` carries, per file, `sweep v=1 ...` with the backend, decode ms per frame
(average and p95), late drops and a colour signature
([../build/validation.md](../build/validation.md#the-codec-sweep)). On the dev PS5
Pro, native decode is 0.3 to 0.8 ms per frame at every resolution, with zero late
drops; 4K60 H.264 and HEVC use 3 to 7% of the frame budget.
