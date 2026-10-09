# EVO Player documentation

Grouped by what you are trying to do. Start at
[planning/roadmap.md](planning/roadmap.md) if you are picking up a GitHub issue.

## [build/](build/) — getting it onto the console

| | |
|---|---|
| [building.md](build/building.md) | Dev environment, SDK, FFmpeg, the whole toolchain |
| [tooling.md](build/tooling.md) | Every script, launch safety, the close path, screenshots, klog |
| [packaging.md](build/packaging.md) | PKG packaging |
| [validation.md](build/validation.md) | Validation checklist |

## [architecture/](architecture/) — how the code is shaped

| | |
|---|---|
| [architecture.md](architecture/architecture.md) | Layer boundaries, what still lives in `main.c.legacy` |
| [modularisation-plan.md](architecture/modularisation-plan.md) | The carve-up, in progress |

## [ui/](ui/) — the interface

| | |
|---|---|
| [rmlui-integration-guide.md](ui/rmlui-integration-guide.md) | The RmlUi layer and how it binds to C state |
| [rmlui-parity.md](ui/rmlui-parity.md) | Per-screen parity checklist |
| [ui-handoff.md](ui/ui-handoff.md) | The legacy UI layer |
| [theming.md](ui/theming.md) | Theme format and tokens |
| [icon-swap-handoff.md](ui/icon-swap-handoff.md) | Icon set migration |
| [media-tile.md](ui/media-tile.md) | Media tile and metadata handling |
| [surround-studio.md](ui/surround-studio.md) | Surround Sound Studio (#106): room model, orb, DualSense-mic calibration |

## [hardware/](hardware/) — talking to the console

| | |
|---|---|
| [hardware-decode.md](hardware/hardware-decode.md) | Decoder investigation and panic vectors |
| [hardware-decode-review.md](hardware/hardware-decode-review.md) | Review of the above |
| [gpu-notes.md](hardware/gpu-notes.md) | GPU reverse-engineering history |
| [shader-compilation.md](hardware/shader-compilation.md) | `.pipe` → amdllpc → PAL metadata, for gfx1013 |
| [refresh-rate-modes.md](hardware/refresh-rate-modes.md) | VideoOut mode values (23.976/24/50/119.88 Hz), the experimental MATCH VIDEO FRAME RATE, and why 24p output is refused to a game-slot app on 12.70 |
| [upscaler.md](hardware/upscaler.md) | #103 video upscaler: Sharp (FSR1) / AI (Anime4K S/M/UL), PS5 Pro detection, `upcompare` |
| [motion-smoothing.md](hardware/motion-smoothing.md) | #105 frame interpolation (experimental): the four passes, the artefacts it had, the debug views |
| [psml-research.md](hardware/psml-research.md) | Sony's PSML / PSSR on the PS5 Pro: modules, Trinity mode, what EVO can and cannot reach |
| [networking.md](hardware/networking.md) | Console services, jailbreak-lapsed symptoms |

## [evo-pro/](evo-pro/README.md) — the app-module program

Hardware decode, GPU rendering and the app-module repackage.
**Resume here: [evo-pro/status.md](evo-pro/status.md).**

## [planning/](planning/) — what to do next

| | |
|---|---|
| [roadmap.md](planning/roadmap.md) | **Issue order and per-story references. Start here.** |
| [project-tracking.md](planning/project-tracking.md) | The GitHub Project board |
| [backlog.md](planning/backlog.md) | Ideas, not current state |
| [improvements-roadmap.md](planning/improvements-roadmap.md) | Older planning notes |

## [research/](research/) — findings, kept for the record

These describe work that is finished, superseded or abandoned. They are here so
the reasoning survives, not to be acted on.

| | |
|---|---|
| [prosperoplayer-baseline.md](research/prosperoplayer-baseline.md) | Upstream baseline |
| [baseline-defects.md](research/baseline-defects.md) | Defects found in it |
| [reng-analysis-integration.md](research/reng-analysis-integration.md) | Reverse-engineering notes |
| [native-media-research.md](research/native-media-research.md) | Native media APIs |
| [sdk-audit.md](research/sdk-audit.md) | SDK surface audit |
| [converter-perf.md](research/converter-perf.md) | The deleted CPU converters, and the BT.601 reference matrix |

## [addons/](addons/) — media server integration

| | |
|---|---|
| [provider-architecture.md](addons/provider-architecture.md) | **The provider seam (#90).** The `evo_provider_t` vtable, the runtime-fetched UI bundle format, the binding and RCSS rules a bundle must follow, and how a provider item reaches the player. Replaces the payload-era `addons-emby-nuvio.md` |
| [ui/hui-kit.md](ui/hui-kit.md) | **EVO's UI on ps5-homebrew-ui.** How the kit's draw lists reach sceAgc (`ui_sdf.pipe`), which screens run on it, the RmlUi fallback, `tools/hui_preview.sh`, and how to add a screen |

## Top level

| | |
|---|---|
| [proprietary.md](proprietary.md) | Licensing notes |
