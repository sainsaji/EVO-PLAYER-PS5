# EVO Player

A reproducible Docker development environment for **PS5 homebrew targeting
firmware 12.70**, and the home of EVO Player — a media player forked from
[ProsperoPlayer](https://github.com/KINGDKAK/ProsperoPlayer).

```
Windows  ->  VS Code  ->  Docker container  ->  Linux toolchain (clang-18)
                                             ->  PS5 Payload SDK v0.42
                                             ->  ELF  ->  PS5 12.70
```

Everything is pinned. `docker compose build` produces the same toolchain
tomorrow as it does today.

> **Status.** Verified end to end on a real console (firmware `0x12700001`):
> `hello_world`, `system_info`, `videoout_test`, `audioout_test`, `gpu_test`
> and `decoder_test` all run. FFmpeg 7.0.1 builds, and unmodified
> ProsperoPlayer builds. Notably, **`libSceAvPlayer` turned out to be directly
> callable from a payload** — see
> [docs/native-media-research.md](docs/native-media-research.md).
> Full record: [docs/validation.md](docs/validation.md).

---

## Contents

1. [Windows prerequisites](#1-windows-prerequisites)
2. [Docker Desktop](#2-docker-desktop)
3. [Repository setup](#3-repository-setup)
4. [Starting the container](#4-starting-the-container)
5. [The SDK](#5-the-sdk)
6. [LLVM version, and why](#6-llvm-version-and-why)
7. [Building](#7-building)
8. [Deploying to the console](#8-deploying-to-the-console)
9. [PS5 12.70 requirements](#9-ps5-1270-requirements)
10. [FFmpeg](#10-ffmpeg)
11. [ProsperoPlayer baseline](#11-prosperoplayer-baseline)
12. [Packaging](#12-packaging)
13. [GPU development](#13-gpu-development)
14. [Video decoder research](#14-video-decoder-research)
15. [VS Code](#15-vs-code)
16. [Troubleshooting](#16-troubleshooting)
17. [Networking](#17-networking-docker--ps5)
18. [Repository layout](#18-repository-layout)
19. [Architecture](#19-architecture)
20. [Validation checklist](#20-final-validation-checklist)

---

## 1. Windows prerequisites

| Requirement | Notes |
|---|---|
| Windows 10 21H2+ / Windows 11 | WSL2 backend needed |
| WSL2 enabled | `wsl --install` in an elevated PowerShell, then reboot |
| Docker Desktop | see below |
| Git for Windows | supplies Git Bash, used to run `./scripts/*.sh` |
| VS Code | optional but recommended |
| ~25 GB free disk | image ≈1.3 GB, plus SDK, FFmpeg sources and build trees |
| A jailbroken PS5 on 12.70 | only needed to *run* payloads, not to build them |

```powershell
wsl --install
wsl --set-default-version 2
```

## 2. Docker Desktop

Install from <https://www.docker.com/products/docker-desktop/>.

Then in **Settings**:

- **General** → enable *Use the WSL 2 based engine*
- **Resources** → give it at least **4 CPUs** and **8 GB RAM** (FFmpeg builds
  with `-j$(nproc)`)

Verify:

```powershell
docker --version
docker compose version
docker run --rm hello-world
```

## 3. Repository setup

```powershell
git clone <your-fork-url> "EVO Player"
cd "EVO Player"
```

Set your console's address in a **git-ignored** `.env` at the repository root.
It is never committed:

```powershell
"PS5_HOST=192.168.1.50" | Out-File -Encoding ascii .env
```

Find it on the console: **Settings → Network → Connection Status**.

## 4. Starting the container

```powershell
docker compose build
docker compose run --rm ps5-dev bash
```

From Git Bash you can instead use the shortcut:

```bash
./scripts/shell.sh
```

Confirm the environment inside the container:

```bash
echo $PS5_PAYLOAD_SDK     # /opt/ps5-payload-sdk
clang --version           # Ubuntu clang version 18.1.3
ld.lld --version          # Ubuntu LLD 18.1.3
cmake --version           # cmake version 3.31.6
ninja --version           # 1.12.1
```

Or run the full health check, which also builds the SDK's own sample:

```bash
./scripts/setup-sdk.sh
```

## 5. The SDK

[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) **v0.42**,
installed to `/opt/ps5-payload-sdk` from the official release zip (which
already includes libc++, because upstream CI runs `libcxx.sh` before packaging).

To build it from source instead:

```powershell
docker compose build --build-arg BUILD_SDK_FROM_SOURCE=1
```

or, in a running container: `./scripts/setup-sdk.sh --from-source`.

The image also installs the prebuilt PS5 library sysroot from
[pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) **v0.39** —
FFmpeg 7.0.1, SDL2, mesa, libass and friends. Only `target/user/homebrew` is
taken from it, so the pinned SDK is never overwritten. Skip it with
`--build-arg INSTALL_PACBREW=0` (ProsperoPlayer will then not link).

Full audit: **[docs/sdk-audit.md](docs/sdk-audit.md)**.

## 6. LLVM version, and why

**LLVM/Clang/LLD 18.** Not "latest", and the reasoning matters:

1. The SDK's own CI installs `clang-18 lld-18` — that is the only configuration
   upstream actually builds and releases.
2. `libcxx.sh` hardcodes `LLVM_VER="18.1.8"` and compiles libc++/libc++abi/
   libunwind from those sources into the PS5 sysroot. ProsperoPlayer links
   `-lc++ -lc++abi`, so keeping the host compiler on 18 avoids C++ ABI skew.
3. `Makefile.inc` probes `llvm-config-22 … llvm-config-15` and takes the first
   hit, so the SDK *tolerates* 15–22 — but only 18 is CI-verified.

**Alternative:** LLVM 19–22 work per `Makefile.inc` (v0.41 added "support for
llvm-22"). Ubuntu 24.04 has no clang-20+, so you would need apt.llvm.org, and
you would be pairing a newer clang with 18.1.8 libc++ sources. Try it with
`--build-arg LLVM_VERSION=20`.

Everything pinned:

| Component | Version | Why |
|---|---|---|
| Ubuntu | 24.04 LTS | carries clang-18/lld-18 in-archive; SDK targets Debian-style hosts |
| LLVM/Clang/LLD | 18.1.3 | matches SDK CI and the 18.1.8 libc++ sources |
| CMake | 3.31.6 | upstream binary; **not** 4.x, which breaks `cmake_minimum_required(<3.5)` in several ports |
| Ninja | 1.12.1 | upstream binary, checksum-verified |
| Python | 3.12.3 | distro; `genstub.py` / `make_fself.py` |
| PS5 Payload SDK | v0.42 | latest release; includes 12.70 offsets |
| pacbrew-repo | v0.39 | supplies FFmpeg 7.0.1 + SDL2 |
| FFmpeg | 7.0.1 | the version ProsperoPlayer already links against |

CMake and Ninja come from checksummed upstream tarballs rather than apt,
because apt point-releases rotate out of the archive and would break
reproducibility.

## 7. Building

```bash
./scripts/build.sh                 # everything
./scripts/build.sh hello_world     # one project
./scripts/build.sh --list
./scripts/build.sh --clean
./scripts/build.sh --cmake         # also regenerate compile_commands.json
```

Payloads land in `output/elf/`. Each is validated as a real PS5 ELF — if a
project is accidentally built with the host compiler, the build fails rather
than producing a Linux binary that could never run.

Per-project, matching the SDK sample conventions:

```bash
make -C projects/hello_world
make -C projects/hello_world install-elf
PS5_HOST=192.168.1.50 make -C projects/hello_world test
```

### The project suite

| Project | Milestone | What it proves |
|---|---|---|
| `hello_world` | 1 | Docker → clang → SDK → ELF → console |
| `system_info` | 2 | libkernel; **prints the firmware version** |
| `videoout_test` | 3 | VideoOut init, framebuffer, registration, flip; solid colour and RGB pattern |
| `audioout_test` | 4 | AudioOut 48 kHz stereo S16; 440 Hz sine |
| `gpu_test` | 5 | GPU capability probe (see [GPU](#13-gpu-development)) |
| `decoder_test` | research | native decoder symbol reconnaissance |
| `avplayer_test` | research | reserved for libSceAvPlayer — placeholder |
| `yuv_gpu_test` | future | GPU YUV→RGB renderer — placeholder |

## 8. Deploying to the console

```bash
./scripts/deploy.sh output/elf/hello_world.elf
PS5_HOST=192.168.1.50 ./scripts/deploy.sh output/elf/hello_world.elf
```

`deploy.sh` refuses to run without `PS5_HOST`, validates that the file is a PS5
ELF, probes `PS5_HOST:9021` before transferring, and logs every deployment to
`output/logs/`. Port **9021** is the `ps5-payload-elfldr` default, and matches
the SDK's own `prospero-deploy`.

Payload output goes to on-screen notifications (every sample calls
`evo_notify()`), or to klog via `ps5-payload-klogsrv` on port 3232.

## 9. PS5 12.70 requirements

- Console on firmware **12.70**
- Jailbroken, **re-run after every reboot**
- `ps5-payload-elfldr` running and listening on 9021
- Console and PC on the same LAN

12.70 support is explicit in the SDK's `crt/kernel.c`:

```c
case 0x12700000:                      /* 12.70 */
    KERNEL_ADDRESS_TEXT_BASE  = KERNEL_ADDRESS_DATA_BASE - 0x0D50000;
    KERNEL_ADDRESS_ALLPROC    = KERNEL_ADDRESS_DATA_BASE + 0x2885E00;
    ...
```

Confirm your console really is on 12.70 by running `system_info`, which prints
the raw version word and warns if it is anything else.

> This is a **PS5-only** environment. The toolchain triple is
> `x86_64-sie-ps5` and every wrapper is `prospero-*`; there is no PS4 path to
> take by accident.

## 10. FFmpeg

**ProsperoPlayer uses FFmpeg 7.0.1** — not stated in its Makefile, which links
prebuilt archives, but determined from pacbrew's `ffmpeg/PKGBUILD`
(`pkgver=7.0.1`). That is the baseline to reproduce before changing anything.

```bash
./scripts/build-ffmpeg.sh --profile baseline   # pacbrew's exact config
./scripts/build-ffmpeg.sh --profile minimal    # production (default)
./scripts/build-ffmpeg.sh --profile full       # compatibility research
./scripts/build-ffmpeg.sh --profile minimal --install
```

| Profile | Purpose |
|---|---|
| `baseline` | Byte-for-byte pacbrew's configure line. Proves the toolchain reproduces the environment the existing player already builds against. |
| `minimal` | `--disable-everything`, then only what the player needs. No CLI tools, no encoders, no muxers, no network protocols. This is what ships. |
| `full` | Every decoder/demuxer/parser. Not for shipping — this answers "is the codec missing, or is our integration broken?" Diff its inventory against `minimal`. |

The minimal build produces 73 components and includes every codec the project
targets:

```
audio  aac  ac3  eac3  dca(DTS)  mp3  flac  opus  vorbis  alac
video  h264  hevc  vp9  vp8  mpeg2video  mpeg4  av1
subs   subrip  ass  srt  movtext  pgssub  dvdsub  dvbsub
mux    matroska(MKV/WebM)  mov(MP4/MOV)  mpegts  avi  ogg  wav
```

Each build writes a codec inventory to
`output/logs/ffmpeg-<ver>-<profile>-codecs.txt`.

> Component names are version specific. `pgssub` is the decoder (not
> `hdmv_pgs_subtitle`), and `image2` is a demuxer with no matching
> `--enable-image2` option. Always check `configure --help` rather than
> guessing — both of those were found the hard way.

FFmpeg object trees live on a Linux volume, not the Windows bind mount, and
ccache is enabled. Installing a custom build **replaces** pacbrew's in the
sysroot; `docker volume rm evoplayer_ps5_sdk` restores the pristine one.

## 11. ProsperoPlayer baseline

```bash
./scripts/build-prosperoplayer.sh --audit    # report dependencies only
./scripts/build-prosperoplayer.sh            # clone + build unmodified
```

Clones into `projects/prosperoplayer/` (git-ignored) and builds it **without
touching the source**, so there is a faithful baseline to fork from.

**Result: it builds** — a 43 MB ELF — but only after one environment fix.

pacbrew builds FFmpeg with `--enable-openssl --enable-libass
--enable-libfreetype --enable-libfribidi --enable-libharfbuzz`. Static archives
carry no dependency metadata, so those libraries must appear on the link line;
upstream's `LIBS` omits them:

```
undefined symbol: BN_set_word, BN_num_bits, BN_rand, ...   -> -lssl -lcrypto
undefined symbol: libiconv, libiconv_open, libiconv_close  -> -liconv
```

Fixed in the **environment** by overriding `LIBS` on the make command line, not
by editing upstream. Reproduce the raw failure with `EVO_SKIP_LINK_FIX=1`.

EVO Player's own fork belongs in `projects/evoplayer/`, started only after the
baseline is confirmed working **on hardware**.

## 12. Packaging

```bash
./scripts/package-pkg.sh --format homebrew output/elf/hello_world.elf
./scripts/package-pkg.sh --format app output/elf/evoplayer.elf \
    --title-id FAKE00001 --title "EVO Player"
```

**PS4 PKG tools do not work on PS5** — PS4 apps use binary `param.sfo`, PS5 uses
`param.json`. Three routes exist; the two that need no proprietary tooling are
implemented. True signed fPKG requires Sony's `prospero-pub-cmd` and is
explicitly refused rather than faked.

Packaging is **not** a dependency of the ELF workflow. Details:
**[docs/packaging.md](docs/packaging.md)**.

## 13. GPU development

Audited, and the result is more useful than a guess would have been:

- **Present:** `libSceGnmDriver` stubs — `sceGnmSubmitCommandBuffers`,
  `sceGnmAreSubmitsAllowed`, and friends.
- **Absent:** GNM headers, Sony's Gnmx helper library, any shader compiler.

So raw GNM means hand-assembling PM4 packets. The productive route is
**mesa + SDL2**, both already in the sysroot — start with
`SDL_PIXELFORMAT_NV12` textures, which do YUV→RGB on the GPU, and only write a
custom shader if that proves insufficient (likely for 10-bit P010).

`gpu_test` probes what actually resolves at run time. Details:
**[docs/gpu-notes.md](docs/gpu-notes.md)**.

## 14. Video decoder research

The SDK ships **no stubs** for `libSceVdecCore`, `libSceAvPlayer`, or any
related module, so `-lSceVdecCore` is not possible. Two routes:

1. **Runtime resolution** — `kernel_dynlib_handle()` + `kernel_dynlib_dlsym()`
   from `<ps5/kernel.h>` resolve symbols in already-mapped modules with no
   proprietary files at all. `decoder_test` does exactly this.
2. **Generated stubs** from a decrypted `.sprx` you supply locally —
   `make -C sce_stubs stubs` runs `genstub.py` over it. See
   [docs/proprietary.md](docs/proprietary.md).

Details: **[docs/native-media-research.md](docs/native-media-research.md)**.

## 15. VS Code

Open the folder and choose **Reopen in Container** (needs the *Dev Containers*
extension). VS Code then runs inside the container, so IntelliSense, the
terminal and the debugger all see the cross toolchain.

- **clangd** is the C language server, driven by `compile_commands.json`.
  cpptools' IntelliSense is deliberately disabled so two servers do not
  disagree; cpptools is kept for its debugger.
- Regenerate the compile database after adding a source file:
  ```bash
  ./scripts/gen-compile-commands.sh
  ```
  then *Ctrl+Shift+P → clangd: Restart language server*.
- **Ctrl+Shift+B** builds everything. More tasks in `.vscode/tasks.json`.
- Debugging uses `gdb-multiarch` against `ps5-payload-gdbsrv` on TCP 2159 —
  see `.vscode/launch.json`. There is no lldb-server on the console.

> CMake Tools is configured **not** to auto-configure: bare `cmake` would build
> for the host and poison the compile database. Always go through
> `$CMAKE` (`prospero-cmake`) or the script.

## 16. Troubleshooting

**`docker compose up` seems to do nothing** — it is the wrong command here, but
harmless. `up` is for long-running services; this container is a *dev shell*.
It starts `bash -l` with nothing attached, so it just sits there until you
Ctrl+C. Use one of these instead:

```powershell
docker compose run --rm ps5-dev bash     # interactive shell
docker compose run --rm ps5-dev ./scripts/build.sh
```

If you already ran `up`, clean up the stopped container with
`docker compose down`.

**`volume "evoplayer_..." already exists but was not created by Docker
Compose`** — the volume was created by a bare `docker run -v ...` and lacks
Compose's ownership labels. Harmless, but to silence it, delete the volume and
let Compose recreate it:

```powershell
docker compose down
docker volume rm evoplayer_ffmpeg_build
docker compose run --rm ps5-dev bash -lc "true"
```

Safe for `evoplayer_ffmpeg_build` and `evoplayer_ccache` — both are pure build
caches, and FFmpeg sources live on the bind mount in `third_party/ffmpeg/`.
Deleting `evoplayer_ps5_sdk` also resets any custom FFmpeg or generated SCE
stubs you installed into the sysroot back to the pinned image defaults.

**`PS5_PAYLOAD_SDK is undefined`** — you are outside the container, or not in a
login shell. Use `./scripts/shell.sh`, or `source
/opt/ps5-payload-sdk/toolchain/prospero.sh`.

**`cannot reach <host>:9021`** — the jailbreak has almost certainly lapsed. It
must be re-run after every reboot, with `ps5-payload-elfldr` started
afterwards. See [networking](#17-networking-docker--ps5).

**`detected dubious ownership in repository`** — already handled in the image
(`safe.directory '*'`), because Windows bind mounts always mismatch uid. If you
see it, rebuild the image.

**`Permission denied` writing to `/build/ffmpeg`** — the named volume was
created before the image pre-created that directory. Fix:
`docker volume rm evoplayer_ffmpeg_build` and re-run.

**`chmod: Operation not permitted` on `scripts/*.sh`** — expected on Windows
bind mounts; the executable bit is not settable. Invoke with `bash
./scripts/foo.sh`.

**ELF built but "not a PS5 payload"** — it was compiled with the host clang.
Use `$CC` from `prospero.sh`, or include `prospero.mk` from your Makefile.

**FFmpeg `Unknown option "--enable-xyz"`** — component names are version
specific. Check `third_party/ffmpeg/ffmpeg-7.0.1/configure --help`.

**Builds are slow** — that is the Windows bind mount, not the network. Heavy
trees already live on Linux volumes; give Docker Desktop more CPU/RAM.

**Reset everything:**

```powershell
docker compose down
docker volume rm evoplayer_ps5_sdk evoplayer_ffmpeg_build evoplayer_ccache
docker compose build --no-cache
```

## 17. Networking (Docker ↔ PS5)

The default **bridge** network is used deliberately. `network_mode: host` does
not mean on Windows what it means on Linux — Docker Desktop's "host" is the
WSL2 VM, not your PC, so host networking gains nothing here. Bridge already
NATs outbound traffic to your LAN, and the container is always the client
(elfldr is the server). **No privileged mode, no host networking.**

Test from inside the container:

```bash
nc -vz $PS5_HOST 9021
```

Full guide, including router AP-isolation and firewall cases:
**[docs/networking.md](docs/networking.md)**.

## 18. Repository layout

```
EVO Player/
├── Dockerfile                  pinned toolchain, every dependency justified
├── docker-compose.yml          services, volumes, networking
├── CMakeLists.txt              cross build -> compile_commands.json
├── .clangd  .devcontainer/  .vscode/
├── scripts/
│   ├── common.sh               strict mode + ERR trap naming the failed step
│   ├── shell.sh  build.sh  setup-sdk.sh  deploy.sh
│   ├── build-ffmpeg.sh  build-prosperoplayer.sh  package-pkg.sh
│   ├── gen-compile-commands.sh
│   └── install-*-image.sh      used during docker build
├── projects/
│   ├── common/                 evo_ps5.h (hand-written SCE decls) + evo.mk
│   ├── hello_world/  system_info/  videoout_test/  audioout_test/
│   ├── gpu_test/  decoder_test/  avplayer_test/  yuv_gpu_test/
│   ├── prosperoplayer/         upstream, unmodified (git-ignored)
│   └── evoplayer/              the fork
├── third_party/ffmpeg/         sources (git-ignored)
├── output/{elf,pkg,logs}/      artifacts
├── proprietary/                never committed
└── docs/
    ├── sdk-audit.md            the full SDK audit
    ├── networking.md  packaging.md  gpu-notes.md
    ├── native-media-research.md  proprietary.md  prosperoplayer-baseline.md
    └── validation.md           what is proven vs. what is not
```

Source lives on Windows via a bind mount; heavy build trees and ccache live on
Linux named volumes.

## 19. Architecture

Three layers, deliberately **not** coupled:

```
Layer 1   PS5 Payload SDK   ->  native applications        WORKING
Layer 2   custom FFmpeg     ->  demux + software decode    WORKING
Layer 3   libSceVdec* / GNM ->  hardware acceleration      RESEARCH
```

Layers 1 and 2 must keep working with layer 3 entirely absent. Target design:

```
                    EVO Player
                        │
                  Media frontend
                        │
                    libavformat
                        │
          ┌─────────────┴─────────────┐
        Video                       Audio
          │                           │
   ┌──────┴──────┐            ┌───────┴───────┐
 PS5 HW      FFmpeg        PS5/Sony        FFmpeg
 decoder     fallback      decoder         fallback
   └──────┬──────┘            └───────┬───────┘
      GPU renderer                AudioOut
          │
      VideoOut
```

The decoder interface should be an abstraction with the FFmpeg software path
always available, and a hardware path selected only when run-time probing
succeeds. **Never make hardware decode a build-time dependency.**

## 20. Final validation checklist

Container-verified (reproducible, gated by CI):

- [x] Docker image builds
- [x] Container starts
- [x] PS5 SDK installed
- [x] PS5 SDK version documented (v0.42)
- [x] 12.70 support verified (`crt/kernel.c` `case 0x12700000:`)
- [x] Clang works (18.1.3)
- [x] LLD works (18.1.3)
- [x] CMake works (3.31.6)
- [x] Ninja works (1.12.1)
- [x] Python works (3.12.3)
- [x] pyelftools works
- [x] Hello World builds
- [x] VideoOut test builds
- [x] AudioOut test builds
- [x] GPU environment is understood
- [x] FFmpeg source/version identified (7.0.1)
- [x] Existing ProsperoPlayer FFmpeg dependencies identified
- [x] Existing ProsperoPlayer builds
- [x] Source remains outside container
- [x] Build artifacts are persisted
- [x] Proprietary files are excluded from Git
- [x] README contains complete setup instructions

Hardware-verified on firmware 12.70:

- [x] Hello World ELF loads and runs on PS5
- [x] Deployment works from container
- [x] Firmware confirmed `0x12700001`
- [x] VideoOut presents (1920x1080, 960 frames)
- [x] AudioOut plays (48 kHz stereo S16)
- [x] GPU submits allowed (`sceGnmAreSubmitsAllowed() -> 1`)
- [x] `libSceAvPlayer` entry points resolved by NID
- [ ] ProsperoPlayer baseline playback confirmed on console

Track results in [docs/validation.md](docs/validation.md).

---

## Roadmap

1. ✅ Environment: Docker → clang-18 → SDK → ELF
2. ✅ Run `hello_world` on a 12.70 console
3. ✅ VideoOut / AudioOut on hardware
4. ✅ FFmpeg 7.0.1, minimal + full profiles
5. ✅ ProsperoPlayer baseline builds
6. ⬜ Confirm baseline playback on hardware
7. ⬜ Fork to `projects/evoplayer/`, fix E-AC3/DTS/FLAC/Opus
8. ⬜ GPU YUV renderer (SDL2 + mesa)
9. ⬜ 4K SDR, HEVC 10-bit
10. 🔬 Hardware decoder research — `libSceAvPlayer` reachable, `libSceVdecCore` export names still unknown
11. ⬜ HDR, if technically possible

## Licence and disclaimer

EVO Player is independent homebrew, **not affiliated with Sony or
PlayStation**. Use at your own risk, with media you own.

Third-party components keep their own licences: the PS5 Payload SDK is GPLv3+,
FFmpeg is LGPL/GPL, ProsperoPlayer carries its own LICENSE. No Sony code,
binaries or keys are included in or required by this repository.

## Credits

- [ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom) — SDK,
  elfldr, websrv, pacbrew-repo
- [KINGDKAK](https://github.com/KINGDKAK) — ProsperoPlayer
- [zecoxao/sce_symbols](https://github.com/zecoxao/sce_symbols) — NID database
