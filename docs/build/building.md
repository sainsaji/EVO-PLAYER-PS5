# Building EVO Player

Developer setup: a Windows host, Docker, the PS5 Payload SDK toolchain, FFmpeg,
and the commands that produce the `.ffpfsc`. Everything toolchain-related runs in
one pinned Docker image, so `docker compose build` gives the same toolchain
tomorrow as today.

```
Windows ─► Git Bash / VS Code ─► Docker container ─► clang 18 + PS5 Payload SDK v0.42
                                                  ─► app module (PPSA99039.ffpfsc) ─► PS5 13.60
```

Contents: [prerequisites](#prerequisites) · [first build](#first-build) ·
[pinned toolchain](#the-pinned-toolchain) · [FFmpeg](#ffmpeg) ·
[shaders](#shaders) · [daily commands](#daily-commands) ·
[editor setup](#editor-setup) · [troubleshooting](#troubleshooting)

---

## Prerequisites

| Need | Notes |
|---|---|
| Windows 10 21H2+ or 11 with WSL2 | `wsl --install`, then reboot |
| Docker Desktop (WSL2 backend) | give it at least 4 CPUs and 8 GB RAM; FFmpeg builds with `-j$(nproc)` |
| Git for Windows | its Git Bash runs the `./scripts/*.sh` and `./tools/*.sh` wrappers |
| ~25 GB free disk | the image is about 1.3 GB, plus the SDK, FFmpeg and build trees |
| A jailbroken PS5 on 13.60 | only to *run* the result, not to build it |

## First build

```bash
git clone --recursive <fork-url> "EVO Player"      # --recursive fetches third_party/ps5-homebrew-ui
cd "EVO Player"
cp .env.example .env                               # then edit PS5_HOST (git-ignored)
docker compose build                               # the pinned image, once
docker compose run --rm ps5-dev bash               # a shell inside it
```

- **`PS5_HOST` lives in `.env`.** `docker compose` reads `.env` only; a
  `.env.local` on its own leaves the container pointing at the old address. Find
  the console's address under Settings → Network → Connection Status.
- Use `run`, not `up`: the container is a dev shell, not a service.
- Health check inside the container: `./scripts/setup-sdk.sh` (also builds the
  SDK's own sample).
- **FFmpeg is not in the image at the version EVO needs.** The pacbrew sysroot
  ships 7.0.1 and EVO needs 7.1.1 (`evo_dovi.c` refuses to build against anything
  else), so on a fresh volume run `./scripts/build-ffmpeg.sh --profile minimal
  --install` once. The result lives in the `evoplayer_ps5_sdk` volume.

Then build and package:

```bash
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
```

Flags and outputs are in [packaging.md](packaging.md); deploying and driving the
console is in [../console/hardware-cycle.md](../console/hardware-cycle.md).

## The pinned toolchain

| Component | Version | Why |
|---|---|---|
| Ubuntu | 24.04 LTS | ships clang-18 and lld-18 in its own archive |
| LLVM / Clang / LLD | 18 | the only configuration the SDK's CI builds; libc++ is built from 18.1.8 sources, so a newer host compiler risks C++ ABI skew |
| PS5 Payload SDK | v0.42 | includes 12.70 offsets; installed from the release zip (`BUILD_SDK_FROM_SOURCE=1` builds from git) |
| pacbrew-repo | v0.39 | the prebuilt library sysroot: SDL2, OpenSSL, libass, freetype, harfbuzz, fribidi, RmlUi |
| CMake / Ninja | 3.31.6 / 1.12.1 | checksummed upstream tarballs; not CMake 4.x, which breaks several ports |
| Python | 3.12.3 | distro |
| static zlib | 1.3.2 | for the host-side module converter |
| **FFmpeg** | **7.1.1** | built by `build-ffmpeg.sh`, not from the image |

The pins are in the `Dockerfile` header and mirrored in `docker-compose.yml` and
`.github/workflows/build.yml`; change all three together.

Three named volumes keep heavy trees off the Windows bind mount:
`evoplayer_ps5_sdk` (the SDK sysroot, plus anything you install into it such as
FFmpeg), `evoplayer_ffmpeg_build` and `evoplayer_ccache`.

## FFmpeg

```bash
./scripts/build-ffmpeg.sh --profile minimal --install   # production (the default profile)
./scripts/build-ffmpeg.sh --profile baseline            # pacbrew's exact configure line
./scripts/build-ffmpeg.sh --profile full                # every decoder, for compatibility research
./scripts/build-dav1d.sh --install                      # only if you rebuild dav1d
```

`minimal` starts from `--disable-everything` and enables what the player uses:
H.264, HEVC, VP8/9, MPEG-2/4, AV1 (via **libdav1d only**: FFmpeg's own `av1` decoder
is a hwaccel-only wrapper that faults without hardware), the audio set in
[../hardware/codec-support.md](../hardware/codec-support.md), the subtitle
decoders, the container demuxers, and network (`http,https,tcp,tls,crypto`,
demuxers `hls` and `dash`) for the providers. Each build writes its codec
inventory to `output/logs/ffmpeg-<ver>-<profile>-codecs.txt`.

Component names are version-specific; check `configure --help` rather than
guessing (`pgssub` is the decoder, `image2` is a demuxer with no `--enable`).
Installing a custom build **replaces** pacbrew's in the sysroot; `docker volume rm
evoplayer_ps5_sdk` restores the pristine one.

The static FFmpeg archives carry no dependency metadata, so the link lines name
`-lssl -lcrypto -liconv -lxml2` and friends explicitly. This is why you never call
`make` directly: only the scripts know the full list.

## Shaders

GPU shaders are `.pipe` sources compiled by **amdllpc** in a *second* image, and
`package-app.sh` does **not** rebuild them. After editing any `.pipe`:

```bash
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml build ps5-dev   # once, ~1 h
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml \
  run --rm ps5-dev python3 tools/build_agc_pipes.py
```

Details and the traps: [../hardware/shader-compilation.md](../hardware/shader-compilation.md).

## Daily commands

```bash
# the real build (all flags: packaging.md)
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
# compile check only: never deploys, never touches the console
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh
# host unit tests (build/validation.md)
docker compose run --rm ps5-dev ./tests/run_tests.sh
# render every UI screen on the PC
./tools/hui_preview.sh
# a long-lived container for fast repeated calls (~0.2 s instead of seconds)
./tools/dx.sh ./tools/evo-remote.sh status        # ./tools/dx.sh --stop before `docker compose build`
```

On Windows the scripts re-run themselves through `docker compose` when started
from the host. From Git Bash, call `docker compose run --rm -e PS5_HOST=... ps5-dev
./tools/...` directly, and set `MSYS_NO_PATHCONV=1` when you pass a container path.
Write multi-line container scripts with an editor, not a Python `write_text`:
Windows adds CRLF and breaks them inside the container (`.gitattributes` forces LF
on the tracked ones).

## Editor setup

Open the folder and choose **Reopen in Container** (Dev Containers extension). VS
Code then sees the cross toolchain.

- **clangd** is the C/C++ language server, driven by `compile_commands.json`.
  Regenerate it after adding a source file with `./scripts/gen-compile-commands.sh`,
  then restart clangd. cpptools IntelliSense is disabled on purpose.
- CMake Tools is configured *not* to auto-configure: a bare `cmake` would build for
  the host and poison the compile database.
- **Ctrl+Shift+B** and the tasks in `.vscode/tasks.json` wrap the same scripts.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `docker compose up` seems to hang | wrong command; it starts a shell with nothing attached. Ctrl+C, `docker compose down`, use `run` |
| `PS5_PAYLOAD_SDK is undefined` | outside the container, or not a login shell. Use `./scripts/shell.sh` |
| `volume ... was not created by Docker Compose` | harmless label warning. `docker compose down` and `docker volume rm evoplayer_ffmpeg_build` to silence it |
| `Permission denied` writing `/build/ffmpeg` | the named volume predates the image; `docker volume rm evoplayer_ffmpeg_build` |
| `chmod: Operation not permitted` on scripts | expected on Windows bind mounts; run with `bash ./scripts/foo.sh` |
| `detected dubious ownership in repository` | the image already sets `safe.directory '*'`; rebuild the image |
| undefined `BN_*` or `libiconv` at link | you ran `make` directly. Use the scripts |
| FFmpeg `Unknown option "--enable-xyz"` | component names change per version; check `configure --help` |
| builds are slow | the Windows bind mount, not the network; heavy trees already live on volumes; give Docker more CPU and RAM |

Reset everything:

```bash
docker compose down
docker volume rm evoplayer_ps5_sdk evoplayer_ffmpeg_build evoplayer_ccache
docker compose build --no-cache
```

The container reaches the console over Docker's default bridge network; see
[../console/networking.md](../console/networking.md) for why not host networking
and for connectivity checks.
