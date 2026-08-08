# Validation status

Two categories, kept strictly apart:

- **Verified in CI/container** — reproducible by anyone, gated by CI.
- **Requires hardware** — needs a jailbroken PS5 on 12.70. **Not yet run.**

The author of this scaffold has no console attached, so *every* on-hardware row
below is untested. Do not read a green checkbox in the first table as evidence
that anything runs on a PS5.

---

## Verified in the container

Last run 2026-08-09, image `evo-player/ps5-dev:llvm18-sdk-v0.42`.

| Check | Status | Evidence |
|---|---|---|
| Docker image builds | pass | `docker compose build`, 1.24 GB |
| Container starts | pass | `docker compose run --rm ps5-dev bash` |
| PS5 SDK installed | pass | `/opt/ps5-payload-sdk`, release zip |
| SDK version documented | pass | `EVO_SDK_VERSION` → v0.42 |
| 12.70 support verified | pass (by source audit) | `crt/kernel.c` `case 0x12700000:` — see [sdk-audit.md](sdk-audit.md#2-firmware-support-and-1270-specifically) |
| Clang works | pass | 18.1.3 |
| LLD works | pass | 18.1.3 |
| CMake works | pass | 3.31.6 |
| Ninja works | pass | 1.12.1 |
| Python works | pass | 3.12.3 |
| pyelftools works | pass | `ELFFile` import + `setup-sdk.sh` check |
| SDK `hello_world` builds | pass | `setup-sdk.sh` smoke test |
| EVO `hello_world` builds | pass | 113 KB PIE ELF |
| `system_info` builds | pass | 115 KB |
| VideoOut test builds | pass | 119 KB, links `-lSceVideoOut -lSceUserService` |
| AudioOut test builds | pass | 116 KB, links `-lSceAudioOut` |
| `gpu_test` builds | pass | 116 KB, links `-lSceGnmDriver` |
| `decoder_test` builds | pass | 116 KB |
| GPU environment understood | pass | [gpu-notes.md](gpu-notes.md) — stubs yes, headers/Gnmx/shader compiler no |
| FFmpeg version identified | pass | 7.0.1, from pacbrew `ffmpeg/PKGBUILD` |
| Custom FFmpeg builds | pass | minimal profile; all 14 target codecs present |
| ProsperoPlayer deps identified | pass | `build-prosperoplayer.sh --audit` |
| ProsperoPlayer builds | pass | 43,305,656-byte ELF, after the transitive-link fix |
| `compile_commands.json` generated | pass | CMake + Ninja, 8 targets |
| Source stays outside container | pass | bind mount, `.:/workspace` |
| Build artifacts persist | pass | `output/` on the bind mount; ccache + FFmpeg tree on named volumes |
| Proprietary files excluded | pass | `.gitignore`; nothing required to reach this point |
| README has full setup | pass | [../README.md](../README.md) |

### FFmpeg minimal profile codec inventory

73 components. All of the brief's targets present:

`aac` `ac3` `eac3` `dca` `mp3` `flac` `opus` `vorbis` `alac` ·
`h264` `hevc` `vp9` `mpeg2video` `av1`

Full list: `output/logs/ffmpeg-7.0.1-minimal-codecs.txt`.

### ProsperoPlayer baseline finding

Upstream builds unmodified **only after** adding transitive static-link
dependencies. pacbrew builds FFmpeg with `--enable-openssl --enable-libass
--enable-libfreetype --enable-libfribidi --enable-libharfbuzz`; static archives
carry no dependency metadata, so those must appear on the link line. Upstream's
`LIBS` omits them.

Symptoms, in the order they surface:

```
undefined symbol: BN_set_word, BN_num_bits, BN_rand, BN_CTX_new, BN_mod_exp
    -> referenced by rtmpdh.c in libavformat.a   -> add -lssl -lcrypto
undefined symbol: libiconv, libiconv_open, libiconv_close
    -> add -liconv
```

Fixed in the **environment**, not by editing upstream: `build-prosperoplayer.sh`
overrides `LIBS` on the make command line. Reproduce the failure with
`EVO_SKIP_LINK_FIX=1`.

---

## Requires hardware — NOT YET RUN

Every row is untested. Run them in order; each depends on the previous one.

| # | Check | Status | How |
|---|---|---|---|
| 1 | Container reaches the console | untested | `nc -vz $PS5_HOST 9021` |
| 2 | `hello_world` loads and runs | untested | `./scripts/deploy.sh output/elf/hello_world.elf` — expect a "hello from PS5" notification |
| 3 | `hello_world` exits cleanly | untested | no crash, no error dialog |
| 4 | Firmware really is 12.70 | untested | `system_info` → expect raw `0x12700000` |
| 5 | VideoOut solid colour | untested | `videoout_test` — expect red/green/blue/white, 2 s each |
| 6 | VideoOut RGB pattern | untested | `videoout_test pattern` — bars, gradient, grey ramp |
| 7 | AudioOut sine wave | untested | `audioout_test` — 440 Hz for 3 s, no clicks |
| 8 | GNM submits allowed | untested | `gpu_test` → `sceGnmAreSubmitsAllowed()` |
| 9 | Native decoder modules mapped | untested | `decoder_test`, then record in [native-media-research.md](native-media-research.md) |
| 10 | ProsperoPlayer baseline plays media | untested | deploy `PS5MediaPlayerPRO.elf`, confirm **existing** functionality before any changes |

### Known uncertainties in the untested code

Flagged honestly rather than presented as working:

- **`videoout_test`** — the pixel format, tiling mode and `SCE_KERNEL_WC_GARLIC`
  direct-memory type come from the documented VideoOut ABI, not from a
  hardware run. Every symbol used exists in the stub, so it links; whether the
  first flip actually presents is what run #5 establishes. If it fails, the
  likeliest culprits are the memory type (try `SCE_KERNEL_WB_ONION`), the
  2 MiB alignment, or needing `sceVideoOutSetFlipRate` before registration.
- **`audioout_test`** — `sceAudioOutInit` returning "already initialised"
  (`0x800f0002`) is treated as benign; that specific code is an assumption.
- **`gpu_test` / `decoder_test`** — these are *probes*. Reporting "not mapped"
  is a valid, useful result, not a failure.

### Recording a run

Append to this file: date, firmware raw word, payload sha256, what you saw,
and any error codes. A photograph of the screen is worth more than a
description for the VideoOut tests.
