# Architecture

How EVO Player is put together, and where each layer lives. For the C++ core
(screens, services, state machines) read [core.md](core.md). For what the app
module is and the traps of running in it, read [app-module.md](app-module.md).

---

## What it runs as

EVO is a **fake-signed game-category app module**, title `PPSA99039`. It has its
own title id, `param.json`, sandbox and user session, and ShadowMountPlus mounts
it from `/data/homebrew/PPSA99039.ffpfsc`. That context is what gives it
`sceVideodec2` hardware decode and `sceAgc` GPU access; a payload loaded through
elfldr or hbldr gets neither. There is one hardware path and no ELF payload.

## The layers

`projects/evoplayer/` is built from these layers. Each one only calls downward.

```
main.cpp            entry point only: crash + SIGTERM handlers -> Application::run()
  core/             the player (C++17): Application, ScreenManager, screens/, services/,
  │                 interfaces/, fsm/, audio/ (IEC 61937 + passthrough)
  │
  ├── ui_kit/       EVO's own screens, drawn with the ps5-homebrew-ui kit (ui/hui-kit.md)
  ├── ui_rml/       RmlUi: the bridge every screen call goes through, provider
  │                 documents, the fallback, and the sceAgc render interface
  ├── ui/           shared UI primitives in C: nav, focus, input, layout, feedback,
  │                 the virtual keyboard state
  │
  ├── media/        playback engine and the GPU runtime: demux, FFmpeg + sceVideodec2
  │                 decoders, audio out, subtitles, upscaler/interp passes,
  │                 evo_agc_runtime.c (sceAgc + sceVideoOut)
  ├── pp/           playback pacing: presentation clock, audio clock, pp_playback;
  │                 theme loader
  ├── addons/       evo_net (HTTP/TLS), the provider seam and providers, updater
  └── src/, include/  boot log, jailbreak, data paths, recent/favourites, stats and log
                    server, the dev remote (evo_usb_remote.c), web-UI proxy
```

Other trees:

| Path | What it is |
|---|---|
| `assets/rml/`, `assets/hui/`, `assets/fonts/`, `assets/icons/` | UI documents, kit fonts and sounds, fonts, icons. Embedded into the binary at package time by `tools/bundle_rml_assets.py` |
| `shaders/agc/` | `.pipe` shader sources and the generated `*_pipe.h` headers ([hardware/shader-compilation.md](../hardware/shader-compilation.md)) |
| `sce_sys/` | `param.json` and the title art |
| `../../assets/providers/` | provider UI bundles that are **served, not embedded** ([providers/architecture.md](../providers/architecture.md)) |
| `../../tools/native-app/` | the link tail, FSELF signing and the clean-room `libc.prx` ([app-module.md](app-module.md)) |
| `../../tests/` | host unit tests ([build/validation.md](../build/validation.md)) |

Layer rules:

- **Screens only draw.** Focus, input and state stay in `core/`. A screen gets a
  plain parameter struct and draws it; that is what lets the host preview render
  every screen with no console.
- **`media/` owns the GPU.** `evo_agc_runtime.c` is the sole owner of `sceAgc`
  and `sceVideoOut` for the whole session. A second `sceVideoOut` open panics the
  console.
- **FFmpeg is behind a seam.** Video decode goes through `media/include/evo_vdec.h`
  (`open / send / receive / flush / close`). `evo_vdec_ffmpeg.c` is pure: no
  clocks, no sleeps. `evo_vdec_native.c` is the `sceVideodec2` backend. Pacing and
  A/V sync live in `evo_playback.c` and `pp/`.

## A frame

```
file / network ─► demux (FFmpeg) ─► packets ─► decoder ─► NV12 / P010 frame ─┐
                                        ├── sceVideodec2 (H.264, HEVC, VP9)   │
                                        └── FFmpeg + dav1d (everything else)  │
audio packets ─► FFmpeg decode ─► resample ─► AudioOut (PCM 2ch or 8ch,       │
                                  or IEC 61937 passthrough)                   │
                                                                              ▼
screens (kit / RmlUi) ─ draw lists ─► evo_agc_runtime.c ─ one sceAgc command buffer
                                      per frame: video pipeline (YUV→RGB, upscale,
                                      interpolation), then UI and OSD ─► sceVideoOut flip
```

- The render size is the panel's own (`sceVideoOutGetResolutionStatus`), not a
  fixed 1080p.
- YUV→RGB, tone mapping, Dolby Vision reshaping, the upscaler and motion
  smoothing all run in AGC pixel shaders; there are no CPU converters.
- The pad, USB and `/data` are only reachable after the self-unjail
  ([app-module.md](app-module.md#sandbox-and-the-self-unjail)). The GPU runtime and
  the resident decoders come up before it, because `libSceAgc*` and `libSceVideoOut`
  go API-dead after the credential swap.

## Decoding strategy

Three layers, deliberately not coupled:

1. **Hardware**: `sceVideodec2` for H.264, HEVC (Main and Main10) and VP9
   (Profile 0 and 2). Resident decoders are created once at boot and reset per
   file. See [hardware/video-decode.md](../hardware/video-decode.md).
2. **FFmpeg 7.1.1 software**: the fallback for anything the hardware declines, and
   the only path for MPEG-2, MPEG-4, VP8 and AV1 (libdav1d).
3. **Policy**: Settings → Video Decoder is `AUTO` (hardware when the codec has a
   resident decoder, otherwise FFmpeg), `NATIVE` (asks for hardware on any codec
   and still falls back to FFmpeg when the hardware declines the stream) or
   `FFMPEG` (always software).

FFmpeg software decode stays the default and the fallback. Hardware decode is
probed at run time, never a build-time dependency. The full codec table is in
[hardware/codec-support.md](../hardware/codec-support.md).

## Repository layout

```
EVO Player/
├── Dockerfile  Dockerfile.amdllpc          toolchain images (the shader compiler is the second)
├── docker-compose.yml  docker-compose.amdllpc.yml
│                   (Dockerfile.ps5-opengl is an unused leftover of the removed OpenGL path)
├── scripts/        build, package and deploy (build/tooling.md)
├── tools/          dev remote, host preview, host test harnesses, generators, RAG search
├── tests/          host unit tests and fixtures
├── projects/
│   ├── evoplayer/  the player (this document)
│   ├── common/     shared SCE declarations and make fragments
│   └── sandbox_probe/  the sandbox probe used to map the app sandbox
├── assets/providers/   served provider UI bundles
├── themes/         sample .theme files
├── third_party/    git-ignored reference clones and submodules (FFmpeg sources, the UI kit, ...)
├── output/         build products, logs, screenshots, cycle evidence (git-ignored)
├── proprietary/    never committed (proprietary.md)
└── docs/           you are here
```

## Where to start reading

| If you want to… | Read |
|---|---|
| build and deploy | [build/building.md](../build/building.md), [build/tooling.md](../build/tooling.md) |
| drive the console | [console/hardware-cycle.md](../console/hardware-cycle.md) |
| change a screen | [ui/hui-kit.md](../ui/hui-kit.md), [core.md](core.md) |
| add a setting | [ui/settings.md](../ui/settings.md) |
| work on a media server or Live TV | [providers/architecture.md](../providers/architecture.md) |
| work on decode, audio or GPU | [hardware/video-decode.md](../hardware/video-decode.md), [hardware/agc-runtime.md](../hardware/agc-runtime.md) |
| check what is planned | [planning/roadmap.md](../planning/roadmap.md) |
