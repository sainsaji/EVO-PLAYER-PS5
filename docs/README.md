# EVO Player documentation

EVO Player is a native media player for a jailbroken PS5 (FW 13.60), built against the
PS5 Payload SDK and shipped as an app module (`.ffpfsc`, title `PPSA99039`). The UI is
the ps5-homebrew-ui kit and RmlUi on bare-metal `sceAgc`; video decodes on
`sceVideodec2` with an FFmpeg fallback.

Grouped by what you are trying to do. If you are picking up a GitHub issue, start at
[planning/roadmap.md](planning/roadmap.md). The rules that must never be broken (the
ones expensive to relearn on real hardware) are in the root [CLAUDE.md](../CLAUDE.md).

## [architecture/](architecture/): how the code is shaped

| | |
|---|---|
| [architecture.md](architecture/architecture.md) | The layers, a frame's path, the decode strategy, the repository layout |
| [core.md](architecture/core.md) | The C++ core: Application, services, screens, state machines, rules for code in it |
| [app-module.md](architecture/app-module.md) | What `PPSA99039` is: `param.json`, the build tail, the sandbox and self-unjail, the traps |

## [build/](build/): getting it built, shipped and proven

| | |
|---|---|
| [building.md](build/building.md) | Windows, Docker, the pinned toolchain, FFmpeg, shaders, daily commands |
| [packaging.md](build/packaging.md) | `package-app.sh` flags, the `.ffpfsc` and zip, `deploy-app.sh` and its interlock |
| [release.md](build/release.md) | Cutting a release, the CI workflows, the in-app updater |
| [tooling.md](build/tooling.md) | Every script and tool, with a pointer to the page that explains it |
| [validation.md](build/validation.md) | Unit tests, host harnesses, hardware checks, the codec sweep |
| [agy.md](build/agy.md) | Delegating mechanical work to a worker agent |

## [console/](console/): working with the dev console

| | |
|---|---|
| [hardware-cycle.md](console/hardware-cycle.md) | Quit, close, deploy, launch without panicking it; `cycle`; the services it needs |
| [remote-control.md](console/remote-control.md) | `evo-remote.sh`: keys, `ui`, `go`, `set`, the status line |
| [diagnostics.md](console/diagnostics.md) | `evo.log`, klog, the live endpoints (default `:9780`), screenshots, debug switches, the files EVO keeps |
| [networking.md](console/networking.md) | Container to console, ports, running servers on the host |
| [remote-play.md](console/remote-play.md) | PS Remote Play from the PC for system dialogs |
| [console-wake.md](console/console-wake.md) | A plan (not started) to wake the console after a power cut |

## [hardware/](hardware/): talking to the console's silicon

| | |
|---|---|
| [video-decode.md](hardware/video-decode.md) | `sceVideodec2`: the resident decoders, sizing, feeding, fallback, HDR and Dolby Vision |
| [videodec2-abi.md](hardware/videodec2-abi.md) | The verified ABI and call sequence |
| [codec-support.md](hardware/codec-support.md) | What plays, on which decoder, and what was verified |
| [audio.md](hardware/audio.md) | The audio path, gain and delay, and bitstream passthrough |
| [ntfs.md](hardware/ntfs.md) | NTFS USB drives: EVO's own read-only reader on `/dev/daN`, `ntfs://` paths, test images |
| [agc-runtime.md](hardware/agc-runtime.md) | The `sceAgc` runtime: what runs on the GPU, bugs fixed, how to verify a build |
| [shader-compilation.md](hardware/shader-compilation.md) | `.pipe` to amdllpc to AGC registers, the `gfx1013` patch |
| [agc-reference.md](hardware/agc-reference.md) | Where the `libSceAgc` ABI and frame loop came from |
| [memory-budget.md](hardware/memory-budget.md) | Direct, flexible and heap pools, measured. Read before calling EVO memory-limited |
| [upscaler.md](hardware/upscaler.md) | Sharp (FSR 1) and AI (Anime4K) upscaling |
| [motion-smoothing.md](hardware/motion-smoothing.md) | Frame interpolation (experimental) |
| [refresh-rate-modes.md](hardware/refresh-rate-modes.md) | VideoOut modes and why 24p is refused on 12.70 |
| [hdr-sdr-switching.md](hardware/hdr-sdr-switching.md) | Per-video HDR/SDR and 24p switching on 13.60: every test, not solved yet |
| [psml-research.md](hardware/psml-research.md) | Sony's PSSR on the Pro and what EVO cannot reach |

## [ui/](ui/): the interface

| | |
|---|---|
| [hui-kit.md](ui/hui-kit.md) | EVO's screens on ps5-homebrew-ui: how they reach `sceAgc`, how to add one, text in every script |
| [rmlui.md](ui/rmlui.md) | What RmlUi still does, where it lives, and the rules that cost hardware cycles |
| [settings.md](ui/settings.md) | Every setting, the config file format, how to add a setting |
| [theming.md](ui/theming.md) | Themes and the `.theme` format |
| [browser.md](ui/browser.md) | The Browser section (#237): the out-of-process engine, its lifetime and the frame handoff |
| [surround-studio.md](ui/surround-studio.md) | The room soundstage and DualSense-mic calibration |

## [providers/](providers/): media servers and Live TV

| | |
|---|---|
| [architecture.md](providers/architecture.md) | The provider seam: vtable, UI bundles, playback sources, the stream picker, the shelves home |
| [media-servers.md](providers/media-servers.md) | Each provider: setup, protocol notes, quirks, what is not built |
| [web-ui.md](providers/web-ui.md) | The system browser as an optional "web version" |
| [youtube.md](providers/youtube.md) | YouTube in EVO's player: phone casts (60 s), full-length links (QuickJS + yt-dlp's solver), what breaks and how to fix it |

## [planning/](planning/): what to do next

| | |
|---|---|
| [roadmap.md](planning/roadmap.md) | Open issues by area |
| [project-tracking.md](planning/project-tracking.md) | The GitHub Project board |

## Top level

| | |
|---|---|
| [proprietary.md](proprietary.md) | What is never committed, and the third-party licences in the build |
| [images/](images/) | Screenshots. The root README uses fourteen console captures taken 2026-11-04 and four older ones (home, browse, providers, iptv). The new ones: player, stats, mediainfo, chapters, picker, network, surround, about, changelog, and five settings pages); the rest are older captures |
| [video/youtube-listing.md](video/youtube-listing.md) | Copy for the release video upload |
| [theme-mock/ui.html](theme-mock/ui.html) | A static design mock of the home screen |

## Keeping the docs true

- A doc that describes code should name the file, so a reader can check it. A doc that
  describes a result should say the date and the build it was measured on.
- Do not keep a journal in a doc. Status belongs in the issue, the commit and the
  [CHANGELOG](../CHANGELOG.md); a doc states how things are now. Superseded text is deleted
  (git keeps it).
- When code moves or is deleted, grep the docs for the old name. `python
  tools/rag/evo_rag.py index` refreshes the search index after you change any of this.
