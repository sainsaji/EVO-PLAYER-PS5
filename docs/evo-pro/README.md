# EVO Pro

The program to turn EVO Player from an hbldr homebrew payload into a
**registered PS5 app module** with **hardware video decode** and **GPU
rendering** — i.e. the `PS5MediaPlayerPRO` build target, productised.

All of it rests on one hardware result (2026-09-01): from a **fake-signed
game-category app module** — its own `TITLE_ID`, `param.json`, sandbox, user
session, launched from `/data/homebrew/` by ShadowMountPlus — `sceVideodec2`
decodes cleanly and `sceAgc` runs a full shader pipeline. Neither works from
the elfldr-payload / hbldr borrowed-slot context EVO ships in today.

## Start here

**[status.md](status.md)** — current state, the exact commands to run when a
console is available, and what each result means. Point an AI at it to resume.

## Documents

| Doc | What it is | Status |
|---|---|---|
| [status.md](status.md) | **Resume-here** — next actions, decision tree, what's done/blocked | 🧭 living |
| [videodec2-abi.md](videodec2-abi.md) | **Phase 0 (Route B)** — verified `libSceVideodec2` structs + exact call sequence; header `projects/evoplayer/media/include/sce/sce_videodec2.h` | ✅ done, hardware-verified |
| [avplayer-abi.md](avplayer-abi.md) | **Phase 0 (Route A)** — `libSceAvPlayer` ABI from SharpProspero (`SceAvPlayerInitData` 120B, `...FrameInfoEx` pitch+crop, callback port, memory-typing diff vs the old `WC_GARLIC` try); header `sce/sce_avplayer.h`; spike `projects/avplayer_test/` | ✅ transcribed + spike compile-clean; not yet hardware-run |
| [native-decode-plan.md](native-decode-plan.md) | The master plan — 9 phases from ABI harvest to a shipped Auto/FFmpeg/Native decoder toggle, with kill criteria | **Phase 4 ✅ DONE on hardware (#31 closed)** — GTA 4K H.264 plays on `sceVideodec2`; Phase 5 (settings toggle) next |
| [phase-1b-app-module.md](phase-1b-app-module.md) | **Phase 1b** — repackage EVO as app module `PPSA99039` (fork the `ps5-native-app-boilerplate` build tail, clean-room `libc.prx`, ShadowMountPlus). **Milestone 1:** the unchanged FFmpeg-software player running in the app sandbox | ✅ **DONE** — boots to menu; task 8 (playback crash) was `posix_fadvise` from the sandbox, fixed `55685aa0`; 1080p + reasonable-4K play, demanding 4K needs native decode (#31, done) |
| [gpu-rendering-plan.md](gpu-rendering-plan.md) | The hand-rolled `sceAgc` convert/present/UI plan (#27/#28). | 📜 **historical** — superseded by the OpenGL render overhaul; `pp_agc*` deleted (GL-6) |
| [agc-implementation.md](agc-implementation.md) | Line-by-line reverse-engineering of the ProsperoLight `sceAgc` path — DCB layout, CX registers, shader-blob format. | 📖 reference (the code it describes is deleted) |
| [sharpprospero-agc-reference.md](sharpprospero-agc-reference.md) | Study of `SvenGDK/SharpProspero`'s `sceAgc` GPU path — full `libSceAgc` ABI, DCB layout, render-target register model, swizzle library. | 📖 reference |
| [opengl-render-overhaul.md](opengl-render-overhaul.md) | **The one OpenGL funnel for every pixel** — `ps5-opengl` (Mesa + PS5 Gallium + patched PSSL) is the only GPU talker; RmlUi-on-GL + a GL video pass. Full pixel-path inventory + `GL-1`…`GL-6` phasing (#77–#82). | ✅ **GL-1…GL-6 DONE (2026-09-10)** |

Prerequisite (not EVO-Pro-specific, lives in [../modularisation-plan.md](../modularisation-plan.md)):
**Track A** — the decoder seam (`evo_vdec.h`, `evo_vdec_ffmpeg.c`). Mostly
landed; it is what lets `evo_vdec_native.c` slot in beside the FFmpeg backend.

## Phase map

```
Track A (modularisation) ── evo_vdec.h seam ─────────────┐
                                                         │
Phase 0  videodec2 ABI harvest ......................... ✅
Phase 1  app-slot decode gate (ProsperoLight self-test)  ✅ 2026-09-01
Phase 1b repackage EVO as app module PPSA99039           ✅ 2026-09-02
         └─ milestone 1: unchanged player boots + plays in sandbox ✅
Phase 2  native decode spike (sceVideodec2 in-app)       ✅  (Route A dead)
Phase 3  decoder abstraction refactor  ── needs Track A ─┘ ✅ (#30 signed off)
Phase 4  evo_vdec_native.c  (continuous stream, seek, HEVC) ✅ #31 CLOSED
         └─ GTA 4K H.264 plays real-time on sceVideodec2 (2026-09-03)
         └─ seek not clean on the V8 4K path → #32 (high)
Phase 5  settings toggle  Auto / FFmpeg / Native  + runtime probe  ◀── next
Phase 6  host preview, validation, docs

GPU rendering track — REPLACED by the OpenGL render overhaul (#77–#82):
  Step 1  dirty-flag the RmlUi surface       ✅ shipped + hw-verified (kept)
  Step 2  hand-rolled sceAgc present (#27)    ✅ hw-verified, then deleted by GL-6
  Step 3  hand-rolled sceAgc UI geo (#28)     ✅ solids hw-verified, then deleted
  GL-1…GL-6  one ps5-opengl funnel            ✅ DONE 2026-09-10
```

## What to expect out of EVO at each stage

| After | EVO becomes | User-visible change | State |
|---|---|---|---|
| Phase 1b m1 | A home-screen title, FFmpeg decode, in the app sandbox | Launch from the Games row; settings in `/download0`; USB works after self-unjail. Same picture/sound/menus. | ✅ done |
| GPU Step 1 | UI rasterised only on change | RmlUi menus stop dropping frames when idle | ✅ done |
| Phase 4 | `sceVideodec2` as a second decode backend | Demanding 4K (GTA trailer) plays smoothly; FFmpeg auto-fallback | ✅ done |
| GL-1…GL-6 | Every pixel through one `ps5-opengl` GL context | 4K/1080p video, menus, OSD, subtitles, keyboard, HUD all on the GPU; the CPU converters / bitmap fonts / hand-rolled `sceAgc` path are gone | ✅ done 2026-09-10 |
| Phase 5 | "Video decoder" + "Renderer" settings rows | Pick per preference; never breaks playback | ◻ next |

## Key constraints (carried across all docs)

- FFmpeg software decode stays the **default and the fallback**. Native decode
  is runtime-probed, never a build-time dependency.
- Never stack payload launches; never `sceVideoOutOpen` from a payload; never
  sweep kernel `.text`; don't kill `kstuff` (see root `CLAUDE.md`).
- Loader/container constants are fixed to the cross-firmware-validated profile:
  module-SDK `0x02000009`, companion `0x08050001`, FSELF magic `0x1D3D154F`.
- Local reference clone (git-ignored): `third_party/ProsperoLight/` — the build
  tail forked into `tools/native-app/` and the AGC render reference. See its
  `README.EVO.md`.
