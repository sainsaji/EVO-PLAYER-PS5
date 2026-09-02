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
| [native-decode-plan.md](native-decode-plan.md) | The master plan — 9 phases from ABI harvest to a shipped Auto/FFmpeg/Native decoder toggle, with kill criteria | Phase 1 gate ✅ PASSED |
| [phase-1b-app-module.md](phase-1b-app-module.md) | **Phase 1b** — repackage EVO as app module `PPSA99039` (fork the `ps5-native-app-boilerplate` build tail, clean-room `libc.prx`, ShadowMountPlus). **Milestone 1:** the unchanged FFmpeg-software player running in the app sandbox | 🟢 **tasks 1–7 done on hardware 2026-09-02** — boots to menu, pad, settings→/download0, USB browse (via sandbox-unjail); task 8 (playback) blocked, crashes on file select |
| [gpu-rendering-plan.md](gpu-rendering-plan.md) | Move YUV convert + composite + UI off the CPU onto `sceAgc` — the fix for the ~11 fps RmlUi frame. **Step 1 (dirty-flag the RmlUi surface) DONE + hardware-verified** (idle menus 11→~60 fps). Step 2/3 (AGC) sequenced after Phase 1b m1. | 🟢 Step 1 shipped; Step 2 pending AGC gate |
| [agc-implementation.md](agc-implementation.md) | **Step 2/3 how-to** — `native_agc_present.cpp` read line by line, the ProsperoLight shader blobs disassembled (`llvm-mc-18` assembles GCN, so hand-written shaders are possible), `render_frame` DCB annotated, and the concrete `pp/src/pp_agc.c` port + wiring plan | 📝 host analysis 2026-09-02 |
| [sharpprospero-agc-reference.md](sharpprospero-agc-reference.md) | Study of `SvenGDK/SharpProspero`'s `sceAgc` GPU path (cloned to `third_party/SharpProspero/`, git-ignored) — full `libSceAgc` ABI, DCB layout, render-target register model, clean-room swizzle library; complements the ProsperoLight C++ reference | 📖 reference |

Prerequisite (not EVO-Pro-specific, lives in [../modularisation-plan.md](../modularisation-plan.md)):
**Track A** — the decoder seam (`evo_vdec.h`, `evo_vdec_ffmpeg.c`). Mostly
landed; it is what lets `evo_vdec_native.c` slot in beside the FFmpeg backend.

## Phase map

```
Track A (modularisation) ── evo_vdec.h seam ─────────────┐
                                                         │
Phase 0  videodec2 ABI harvest ......................... ✅
Phase 1  app-slot decode gate (ProsperoLight self-test)  ✅ 2026-09-01
Phase 1b repackage EVO as app module PPSA99039           ◀── YOU ARE HERE
         └─ milestone 1: unchanged player boots in sandbox
            ✅ boots to RmlUi menu, pad nav works   (2026-09-02, tasks 1–5)
            ◻  settings→/download0, evo_readdir, USB unjail, playback (6–8)
Phase 2  native decode spike (sceAvPlayer / sceVideodec2 in-app)
Phase 3  decoder abstraction refactor  ── needs Track A ─┘
Phase 4  evo_vdec_native.c  (continuous stream, seek, HEVC)
Phase 5  settings toggle  Auto / FFmpeg / Native  + runtime probe
Phase 6  host preview, validation, docs

GPU rendering track (parallel, after Phase 1b m1):
  Step 1  dirty-flag the RmlUi surface        (CPU only, ships any time)
  Step 2  AGC present + composite + flip       (needs the app module)
  Step 3  full RmlUi GPU geometry backend      (optional)
```

## What to expect out of EVO at each stage

| After | EVO becomes | User-visible change |
|---|---|---|
| Phase 1b m1 | A home-screen title, FFmpeg decode unchanged, in the app sandbox | Launch from the Games row; no stacked-launch risk; settings in `/download0`; USB works after `sandbox-unjail`. Same picture/sound/menus. |
| GPU Step 1 | UI rasterised only on change | RmlUi menus stop dropping frames when idle |
| GPU Step 2 | Convert + composite + flip on the GPU | 60 fps realistic even at 4K; CPU freed for decode |
| Phase 4 | `sceVideodec2` as a second decode backend | 4K60 / HEVC Main10 / high-bitrate clips play smoothly; FFmpeg auto-fallback |
| Phase 5 | "Video decoder" setting Auto / FFmpeg / Native | Pick per preference; never breaks playback |

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
