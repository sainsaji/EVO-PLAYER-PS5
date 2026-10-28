# Tooling

A map of `scripts/` and `tools/`, and which page explains each. Two rules save
time:

- **Never call `make` directly.** The Makefile does not list FFmpeg's transitive
  dependencies; static archives carry no dependency metadata, so a bare `make` fails
  at link. `scripts/package-app.sh` (real build) and `scripts/build-evoplayer.sh`
  (compile check) supply them.
- **Everything runs in the pinned container.** Scripts re-run themselves through
  `docker compose` when started from Windows. `./tools/dx.sh <cmd>` runs any of
  them in one long-lived container (about 0.2 s per call instead of seconds); run
  `./tools/dx.sh --stop` before `docker compose build`.

## Hardware cycle

Putting a build on the console and closing it again, with the rules that stop it
panicking, is its own page: [../console/hardware-cycle.md](../console/hardware-cycle.md).
Driving the UI and settings from the host: [../console/remote-control.md](../console/remote-control.md).
Logs, screenshots, the dashboard and the debug switches:
[../console/diagnostics.md](../console/diagnostics.md).

## `scripts/`: build and deploy

| Script | What it does |
|---|---|
| `package-app.sh` | **The real build.** Compile, link, convert, sign, assemble `output/app/PPSA99039/`; `--ffpfsc` packs the image ([packaging.md](packaging.md)) |
| `deploy-app.sh` | Upload the image or folder over FTP, sha256-verified, with the resident-EVO interlock ([packaging.md](packaging.md#deploy-appsh)) |
| `build-evoplayer.sh` | **Host compile check only.** Builds `output/elf/EVOPlayer.elf` so the non-app-module code path stays green; it cannot deploy and errors on `--run` |
| `build-ffmpeg.sh`, `build-dav1d.sh` | FFmpeg 7.1.1 (profiles `minimal` / `baseline` / `full`) and libdav1d ([building.md](building.md#ffmpeg)) |
| `setup-sdk.sh`, `install-sdk-image.sh`, `install-pacbrew-image.sh`, `setup-native-app-deps.sh`, `setup-pfs-tool.sh` | Image-build and bootstrap helpers: the SDK health check, the SDK and pacbrew sets, static zlib, MkPFS |
| `gen-compile-commands.sh`, `gen-sonar-commands.py` | `compile_commands.json` for clangd; the Sonar command list |
| `common.sh`, `shell.sh` | Sourced by the rest (strict mode, container re-exec, `PS5_HOST` validation); a container shell |
| `setup-github-project.sh` | Builds the GitHub Project board ([../planning/project-tracking.md](../planning/project-tracking.md)) |
| `build-prosperoplayer.sh`, `package-pkg.sh` | **Leftovers**: the upstream baseline build and the old ELF/homebrew packager. Not used by any workflow |

## `tools/`: the console

| Tool | What it does |
|---|---|
| `evo-remote.sh` | The dev remote: lifecycle, keys, `ui`, `go`, `set`, sweeps, comparisons ([remote-control.md](../console/remote-control.md)); `logs` honours `PS5_LOG_PORT` (default 9780) |
| `evo_ui.py`, `evo_lifecycle.py` | Its helpers: the UI state reader and ShadowMount lifecycle evidence |
| `ps5-controllers/` | The title-aware launch and close payloads. Never send them by hand |
| `klog.sh`, `shot.sh`, `grab_shot.sh`, `shot.py`, `fetch_shot.sh` | Kernel log capture, screenshots and pixel measurement ([diagnostics.md](../console/diagnostics.md)) |
| `evo-dash.py`, `evo-panel.py`, `dash/` | The health dashboard (`evo-dash` honours `PS5_LOG_PORT`, default 9780) and the Tkinter control panel |
| `wipe-evo.py` | The EVO-only console wipe behind `deploy-app.sh --fresh` |
| `chiaki.ps1` | Launcher for the PC Remote Play client ([../console/remote-play.md](../console/remote-play.md)) |
| `provider-server.sh`, `gen_provider_manifest.py` | Serve and regenerate provider UI bundles ([../providers/architecture.md](../providers/architecture.md)) |
| `test-addon/`, `mock_xtream_server.py`, `mydebrid/`, `laptop_ftp_server.py`, `testdata/` | Test servers and fixtures that run on the host: a Stremio addon, an Xtream server, a personal debrid, a read-only FTP share, a playlist and guide |
| `checks/` | Per-feature hardware checks run by `evo-remote.sh verify` ([../build/validation.md](validation.md#hardware-checks)) |
| `sweep_run.py`, `sweep_report.py`, `combined_sweep_report.py`, `make_sweep_clips.sh`, `push_sweep_clips.sh` | The codec sweep |
| `dbcompare_run.py`, `upcompare_run.py`, `upscale_ref.py` | Held-frame captures for Deep Blacks and the upscaler, and the numpy reference the upscaler is diffed against |
| `seekstutter_run.py`, `seekstutter_report.py`, `subsync_run.py`, `subsync_report.py` | Seek-stutter and subtitle-drift measurement from `evo.log` |
| `dualsense_dump.py` | Read a real DualSense's report descriptor over USB, for the wake-board spike |

## `tools/`: the host

| Tool | What it does |
|---|---|
| `hui_preview.sh` (+ `hui_preview/`) | Render every kit screen to PNG ([../ui/hui-kit.md](../ui/hui-kit.md)) |
| `uiview.sh`, `uiview_playback_rml.sh/.cpp`, `uiplay.sh`, `prof_rmlui.sh/.cpp` | The RmlUi preview, its contact sheet and its profiler |
| `*_host.sh` / `*_host.c` (`subsync`, `dualsub`, `dns`, `streamopts`, `hls`, `netconnect`, `iptv_epg`) | Host harnesses for single source files ([validation.md](validation.md#host-harnesses)) |
| `host-ffmpeg-probe.sh` | Probe a media file with a host build of the same FFmpeg and flags, **before** spending a console cycle on a file that crashes the app. Needs `MSYS_NO_PATHCONV=1` |
| `rag/` | `evo_search`: search code, docs, issues, memory notes and the `third_party/` repos and their commit messages. Reindex after changing docs (`python tools/rag/evo_rag.py index`) |
| `agy-run.sh`, `oc-run.sh`, `agy-prune.sh`, `agy-task.md` | Delegating mechanical work to a worker agent ([agy.md](agy.md)) |

## `tools/`: assets and generators

Everything generated is checked in, and each generator says how to re-run it.

| Tool | Produces |
|---|---|
| `build_agc_pipes.py`, `gen_video_pipes.py`, `gen_upscale_pipes.py`, `gen_interp_pipes.py`, `gen_hdr_ui_pipe.py`, `patches/llpc-add-gfx1013.py` | GPU shaders: `.pipe` sources and the compile step, in the amdllpc image ([../hardware/shader-compilation.md](../hardware/shader-compilation.md)) |
| `bundle_rml_assets.py` | The embedded UI asset bundle, run by `package-app.sh` (`ui_rml/src/evo_rmlui_bundle_data.cpp` is tracked and changes hugely each time) |
| `gen_charset_tables.py` | Code-page tables for subtitle decoding (`media/src/evo_charset_tables.inc`) |
| `gen_app_icon.py`, `gen_app_pics.py`, `gen_app_icon_variants.py`, `gen_power_icon.py`, `ttf_text.py`, `preview-art/` | The title art |
| `lucide_icon.sh`, `gen_settings_icons.sh`, `gen_surround_icons.py`, `preview_icons.py` | UI icons from Lucide SVGs into `assets/icons/` |
| `package_zip.py` | The folder-title archive |
| `gen_icons.py`, `measure_font.py`, `dump_offsets.py`, `check_crash.py` | **Leftovers**: the first two target the old bitmap icon and font atlas (`evo_icons.h`, `renderer_reset_assets.h`), which no longer exist; the last two are one-off crash-address scripts with a captured register dump hard-coded. To symbolise a `CRASH anchor=` line by hand, the load offset is `anchor` minus the address of `evo_crash_handler` in `output/app/.build/llvm-pie.elf` |
