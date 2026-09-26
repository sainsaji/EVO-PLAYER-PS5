# Audio passthrough (bitstream to the receiver) on PS5

Research for #47, 2026-09-26. Nothing here has been tried on hardware yet.

## Where EVO is today

- Every audio codec is decoded in software by FFmpeg and output as PCM
  (`docs/codec-support.md`). `PlaybackController.cpp` opens one
  `sceAudioOut` **MAIN** port as the system user (`0xFF`): **S16 8-channel at
  48 kHz** for a multichannel source, S16 stereo otherwise.
  `SoundEffectEngine.cpp` opens a second MAIN port for UI sounds, and the
  surround test opens its own.
- The FFmpeg build has the decoders (`dca` covers DTS core, HR and MA;
  `truehd`/`mlp`, `eac3`), the raw `dts`/`dtshd`/`truehd`/`eac3` demuxers and
  the parsers. It is built with `--disable-muxers`, so the `spdif` (IEC 61937)
  muxer is **not** in it.
- `docs/codec-support.md` lists **Dolby TrueHD, and Atmos in TrueHD, as not
  playing at all**, even though the decoder is linked. That is a separate bug,
  and fixing it is worth more than passthrough for most users (see "What
  passthrough actually buys").

## What the platform does

- The PS5 has two independent audio paths: one for games and apps, one for its
  own Blu-ray player. **Atmos, DTS:X and TrueHD bitstream output exist only on
  the Blu-ray path**: Sony's own streaming apps don't get Atmos either.
- For games and apps, *Settings → Sound → Audio Output → Audio Format
  (Priority)* is Linear PCM, Bitstream (Dolby) or Bitstream (DTS). "Bitstream"
  there means the **system re-encodes the mixed PCM** (Dolby Digital / DTS
  5.1). It is not a passthrough of the app's stream.

## The two technical routes

### Route A: Sony's passthrough port (`Pt`)

The SDK's `libSceAudioOut` stub already exports (so these link with no new PRX
stub):

| Family | Symbols | Probably |
|---|---|---|
| Passthrough port | `sceAudioOutPtOpen`, `sceAudioOutPtClose`, `sceAudioOutPtGetLastOutputTime`, and the `sceAudioOutExPt*` trio | Open a port that carries an encoded stream instead of PCM: what the Blu-ray player uses |
| Ex output control | `sceAudioOutExOpen`, `sceAudioOutExConfigureOutputMode`, `sceAudioOutExGetMonitorInfo`, `sceAudioOutExGetOutputInfo`, `sceAudioOutExSystemInfoIsSupportedAudioOutExMode` | Configure the HDMI audio mode, query what the sink supports |
| System | `sceAudioOutSysGetHdmiMonitorInfo`, `sceAudioOutSysHdmiMonitorInfoIsSupportedAudioOutMode`, `sceAudioOutSysConfigureOutputMode`, `sceAudioOutSysOpen` | The same, for the system's own settings UI |

The same names exist on PS4. **No public source documents their parameters**:
shadPS4 declares every one of them as a parameterless stub, and SharpProspero
and the audio research in `third_party/` don't bind them.

What it would take:
1. **The signatures.** Disassemble a *decrypted* `libSceAudioOut.sprx`, or
   better a caller: the PS4 or PS5 Blu-ray / media player module, where the
   arguments are set up. That needs decrypted system modules from a firmware
   dump, worked on offline the same way the sceAgc shaders were ripped from
   decrypted game dumps (the ShaderRip-PS5 repo). **Never** by scanning
   console memory; that panics the console.
2. **Access.** The Blu-ray-only restriction suggests the `Pt` port may be
   refused to anything but the system's player (by process capability, the way
   `/system/priv/lib` is closed to the app module). Only a hardware call
   answers that. A refusal is a return code, not a panic, but a wrong argument
   layout could crash EVO.

If both work out this is the proper answer: AC-3, E-AC-3, DTS, and possibly
TrueHD / DTS-HD MA / Atmos, straight to the receiver, with the capability query
deciding what to offer.

### Route B: IEC 61937 inside PCM on the normal port

The classic HTPC technique: wrap each compressed frame in an IEC 61937 burst
(preamble `Pa=0xF872 Pb=0x4E1F`, data type, length, byte-swapped payload,
zero-padded to the frame period) and send it as ordinary 16-bit PCM. If the
path to HDMI is bit-exact, the receiver recognises the burst and decodes it.

| Format | Carrier needed | Fits `sceAudioOut`? |
|---|---|---|
| AC-3, DTS core | 2 ch, 48 kHz | Yes (S16 stereo, 48 kHz) |
| E-AC-3 | 2 ch, 192 kHz (4× rate) | Maybe: the port accepts 192 kHz |
| TrueHD, DTS-HD MA (HBR) | 8 ch, 192 kHz | Maybe: S16 8-ch and 192 kHz are both accepted |

What decides it, all unknown until a hardware run:
- **Bit-exactness.** Unity volume (`0x8000`), no resampling, no mastering or
  limiter stage, and **nothing else mixed in**: EVO's own UI-sound port,
  system notification sounds and chat would all corrupt the bursts.
- **The receiver.** HDMI will flag the stream as plain PCM, and the PS5 may
  send it as 8-channel LPCM even from a stereo port. Many receivers
  auto-detect DTS or AC-3 bursts in 2-channel PCM (the DTS-CD case); few do
  inside 8-channel LPCM.
- **Safety.** If the receiver doesn't lock on, the burst plays as **full-scale
  noise**. Test with the receiver's volume low.

Building it is cheap: enable `--enable-muxer=spdif` in
`scripts/build-ffmpeg.sh`, or write a ~150-line packer for AC-3/DTS, plus a
passthrough mode in the audio thread that skips decode and `swr`.

## What passthrough actually buys

EVO already outputs **8-channel LPCM**, which carries TrueHD's and DTS-HD MA's
lossless 7.1 beds losslessly. Passthrough adds:
- **Atmos and DTS:X objects.** Only the receiver can render these, and it is
  the one thing PCM can never carry.
- Receiver-side processing and its "Dolby Atmos" / "DTS:X" indicator.
- Less CPU next to 4K decode (no audio decode, no `swr`).

For 5.1/7.1 content without objects, decoded LPCM and bitstream sound the same.

## Recommendation

1. **Fix TrueHD playback first.** It's broken today, cheap to investigate on
   the host (`host-ffmpeg-repro` with the test file
   `Dolby Atmos TrueHD, E-AC-3 7.1.4.mkv`), and it gets TrueHD files the
   lossless 7.1 bed over LPCM.
2. **Run a Route B probe**, one hardware run: an AC-3 5.1 file as IEC 61937 in
   S16 stereo at 48 kHz, with EVO's UI-sound port closed and volume at unity.
   The receiver says "Dolby Digital", or plays noise. If it locks on, try DTS,
   then E-AC-3 at 192 kHz, then HBR.
3. **Route A in parallel**, only if decrypted system modules are available:
   recover the `sceAudioOutPtOpen` and `...IsSupportedAudioOutExMode`
   signatures offline, then one probe run for "does it open from the app
   module". This is the only path with a chance at Atmos objects.
4. User-facing, whichever route works: a *Settings → Audio output: Auto /
   PCM / Bitstream* toggle, per-format fallback to PCM, and a Media Info line
   saying which one is active (as #47 describes).

## Sources

- [#47 Native audio decode + bitstream passthrough](https://github.com/sainsaji/EVO-PLAYER-PS5/issues/47)
- [shadPS4 `audioout.cpp`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/audio/audioout.cpp) (the `Pt`/`Ex`/`Sys` functions are parameterless stubs)
- [Enabling Dolby bitstream pass-through on PlayStation (Dolby)](https://games.dolby.com/news/enabling-dolby-bitstream-pass-through-on-playstation/)
- [PS5 audio help: Linear PCM or Bitstream (ResetEra)](https://www.resetera.com/threads/ps5-audio-help-linear-pcm-or-bitstream-dolby-dts.349918/)
- [PS5 audio formats (feintech)](https://feintech.eu/en/blogs/know-how/ps5-audioformate)
- `libSceAudioOut.so` export list from the pinned SDK; `third_party/ps5-audio-decoding-research/docs/AUDIO-OUTPUT.md`
