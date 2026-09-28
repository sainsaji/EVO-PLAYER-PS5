# Surround Sound Studio (#106)

Settings → Playback → SURROUND SOUND TEST. A 2.5D room soundstage: speakers
placed where they physically stand, a listener at the sweet spot, and a
spatial "orb" sound source that can be moved around them.

## Where things live

| Piece | File |
|---|---|
| Room model: speaker positions (metres), DBAP gain matrix, telemetry, time-of-flight | `core/include/evo/audio/SpatialField.hpp` (header-only, no audio/UI) |
| Screen: panes, input, orb motion, flight modes, sweeps | `core/src/screens/SurroundTestScreen.cpp` |
| Test-tone port (8ch S16) + one-shot PCM playback | `core/src/services/SurroundTestService.cpp` |
| Mic calibration run + analysis | `core/src/services/SpeakerCalibrationService.cpp` |
| Calibration profile: persist + apply to playback | `media/src/evo_speaker_cal.c` (hooked in `audio_queue_push`) |
| Document | `assets/rml/surround.{rml,rcss}`, filled by `EvoRmlApp::UpdateSurroundState` |
| Listener / speaker icons | `tools/gen_surround_icons.py` → `assets/icons/icon_{speaker_body,speaker_cone,listener,armchair}.png` (Lucide, ISC) |
| Host fixtures | `tools/uiview_playback_rml.cpp` → `rml_surround`, `rml_surround_orb`, `rml_surround_calibration` |

Channel indices everywhere are the AudioOut interleave, hardware-verified in
`evo_audio_resample.c`: **FL FR FC LFE BL BR SL SR**. In 5.1, channels 4/5 are
the surrounds (labelled SL/SR, drawn side-rear) and 6/7 don't exist.

## Controls

| Pane | Controls |
|---|---|
| Actions | D-pad up/down, Right → speaker cursor, Cross runs, Square 5.1/7.1, Triangle silence |
| Speakers | D-pad moves by room position (cone search); Left past FL/SL/SBL returns to Actions |
| Orb (3D SOUND FIELD / FREE ROAM) | Left stick / touchpad drag / D-pad (tap 18 dp, held 6 dp per frame) move; L1/R1 height; Triangle MANUAL → ORBIT → FLYBY; Square reset; Cross tone on the loudest speaker |
| Calibration | Cross START (NEXT when done), Triangle REPEAT, Square APPLY, Circle BACK |

3D SOUND FIELD plays continuous music (`core/include/evo/audio/FieldMusic.hpp`:
a music-box arpeggio over a pad, I–vi–IV–V, with a sub-bass line on the LFE).
It is panned sample-smoothly across every speaker by the DBAP gain matrix
(`SurroundTestService::setFieldGains`, ramped per 512-sample block), and gets
louder as the source nears the seat. Cross plays or pauses it. FREE ROAM only
plays a test tone, on Cross.

**Frame cost:** this screen redraws every frame. Its update goes through a
last-value cache (`srd_set` / `srd_text`), positions are whole dp, the growing
rings scale with `transform`, and the numeric readouts refresh at 10 Hz.
Without that cache every element's geometry was rebuilt every frame: 180
`CompileGeometry` calls per frame against 7.7 now (`tools/prof_rmlui.sh`,
SURROUND scenario). The uncached version ran at about 10 fps on the console. Moving the source by hand drops an
autopilot flight mode back to MANUAL.

## Auto calibration (DualSense mic)

The microphone is `libSceAudioIn` (`sceAudioInOpen(user, GENERAL, 0, 256,
16000, S16 mono)`). It has no SDK stub, so it is a declared PRX import:
`tools/native-app/stubs/prx/libSceAudioIn.syms`, added to `PRX_STUB_WANT` in
`package-app.sh`. The payload ELF (compile check) gets -1 fallbacks.

The run:

1. **Mic check:** 0.8 s of room noise. All-zero PCM means the controller is
   muted or off. A floor above -25 dBFS fails the run.
2. **Measure:** one gap-free 8-channel stream. Each speaker gets a log sweep
   in its own 1 s slot (mains 300 Hz–6 kHz / 150 ms, LFE 35–160 Hz / 400 ms),
   and FRONT LEFT plays again at the end.
3. **Analyse:** a matched filter per slot. The first peak at ≥ 50 % of the
   window's maximum is the direct-path arrival (parabolic sub-sample). Its
   height gives the level. Detection needs 12 dB over the same filter run on
   room noise.

Because every sweep sits at an exact sample offset in one output stream and is
found in one capture stream, the relative timing needs no clock timestamps.
The repeated FRONT LEFT measures the output-vs-mic clock drift, which is
removed linearly.

The unknown output + Bluetooth-mic latency is common to every speaker, so:

- **PATH** is relative: the extra distance to the nearest speaker. Absolute
  distance is not knowable this way.
- **DELAY** aligns every arrival to the latest one (clamped at 30 ms).
- **LEVEL** is the trim that balances each speaker to the mains' mean (±12 dB).
  The subwoofer trim is approximate, because the controller mic hears little
  deep bass.

APPLY writes `speaker_calibration.cfg` to the data root. `evo_speaker_cal_process`
then applies per-channel gain and delay to every 8-channel playback block. It
is loaded at boot and again on the `/data` rebind.

## Status

Host-rendered and compile-checked. **hw-verify-pending:**
- Does the app module still load with the new `libSceAudioIn` positional import?
  If it doesn't, the symptom is CE-108255-1 at launch with no log, and dropping
  `libSceAudioIn` from `PRX_STUB_WANT` recovers it.
- Mic capture and chirp detection on real speakers (see the `calibration:`
  lines in `evo.log`).
- The calibration trims audibly applied on playback.

## Gotcha found on the way

RmlUi's `rgba()` parses alpha as **0–255**, not 0–1
(`PropertyParserColour.cpp`). So `rgba(0,205,255,0.3)` is fully transparent.
Use `#rrggbbaa`. Several other `.rcss` files still carry float alphas.
