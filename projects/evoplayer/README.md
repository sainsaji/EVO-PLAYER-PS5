# projects/evoplayer

The EVO Player source: a native media player for a jailbroken PS5, built as the
`PPSA99039` app module. It began as a fork of ProsperoPlayer (see `NOTICE`); almost
nothing of the original remains.

| Directory | What is in it |
|---|---|
| `main.cpp` | entry point only: crash and SIGTERM handlers, then `Application::run()` |
| `core/` | the player: `Application`, `ScreenManager`, screens, services, state machines |
| `media/` | demux, decoders (`sceVideodec2` and FFmpeg), audio out, subtitles, the `sceAgc` runtime |
| `pp/` | the presentation clock, audio clock, `pp_playback`, theme loader |
| `ui/` | nav, focus, input, layout and keyboard-state primitives |
| `ui_kit/` | EVO's screens drawn with the ps5-homebrew-ui kit |
| `ui_rml/` | the RmlUi bridge, provider documents and the `sceAgc` render interface |
| `addons/` | `evo_net`, the provider seam and the providers, the in-app updater |
| `src/`, `include/` | boot log, jailbreak, data paths, recent and favourites, log server, dev remote |
| `assets/`, `shaders/`, `sce_sys/` | UI documents and fonts, `.pipe` shaders, `param.json` and title art |

Do not run `make` here; the project Makefile lacks FFmpeg's transitive link
dependencies. Build with `scripts/package-app.sh` from the repository root.

Documentation lives in [`../../docs/`](../../docs/README.md): start with
[architecture](../../docs/architecture/architecture.md) and
[building](../../docs/build/building.md).
