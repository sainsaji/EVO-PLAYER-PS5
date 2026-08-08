# PS5 Payload SDK audit

Audit performed **2026-08-09** against
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) at tag **v0.42**.

Everything below was read out of the SDK's own sources rather than taken from
its README, because the README is out of date in at least one respect (see
[LLVM](#4-llvm-clang-and-lld-requirements)).

---

## 1. Current release

| Item | Value |
|---|---|
| Latest release | **v0.42**, published 2026-08-02 |
| Release asset | `ps5-payload-sdk.zip` |
| Previous | v0.41 (2026-06-28), v0.40 (2026-05-24), v0.39 (2026-05-12) |
| Pinned in this repo | **v0.42** |

Releases are marked *pre-release* by the upstream workflow
(`softprops/action-gh-release` with `prerelease: true`); that is how the
project ships everything, not a signal of instability.

## 2. Firmware support, and 12.70 specifically

The C runtime picks a kernel offset table from the running firmware version.
`crt/kernel.c` reads the version from `libSceLibcInternal.sprx` (not a syscall —
changed in v0.41 because some payloads spoof the kernel's reported version) and
switches on `kernel_get_fw_version() & 0xffff0000`.

**Firmware 12.70 is explicitly supported.** From `crt/kernel.c`:

```c
case 0x12000000:
case 0x12020000:
case 0x12200000:
case 0x12400000:
case 0x12600000:
case 0x12700000:                      /* <-- 12.70, our target */
    KERNEL_ADDRESS_TEXT_BASE        = KERNEL_ADDRESS_DATA_BASE - 0x0D50000;
    KERNEL_ADDRESS_ALLPROC          = KERNEL_ADDRESS_DATA_BASE + 0x2885E00;
    KERNEL_ADDRESS_SECURITY_FLAGS   = KERNEL_ADDRESS_DATA_BASE + 0x0D83064;
    KERNEL_ADDRESS_ROOTVNODE        = KERNEL_ADDRESS_DATA_BASE + 0x30D7510;
    KERNEL_ADDRESS_BUS_DATA_DEVICES = KERNEL_ADDRESS_DATA_BASE + 0x20757E8;
    KERNEL_OFFSET_VMSPACE_P_ROOT    = 0x1d0;
    KERNEL_OFFSET_VMSPACE_VM_PMAP   = 0x2e8;
    break;
```

The encoding is `0xMMmm0000` with the major/minor read as hex-coded decimal, so
12.70 → `0x12700000`. The full supported span at v0.42 runs from 1.00 through
13.40.

Confirm your console at run time with `projects/system_info`, which prints the
raw word and flags it if it is not 12.70.

> **PS4 vs PS5.** This SDK targets **Prospero (PS5) only**. The compiler triple
> is `x86_64-sie-ps5` (`sce_stubs/Makefile`) and the toolchain wrappers are all
> named `prospero-*`. There is no PS4 path to accidentally take. The one place
> a PS4-looking value appears is `samples/install_app`, which passes
> `PS4_SDK_VERSION ?= 0x07590001` as the fake-SELF app/fw version field — that
> is a metadata constant in the SELF header, not a build target.

## 3. Build-time host requirements

| Tool | Required? | Why | Pinned here |
|---|---|---|---|
| `clang` | **yes** | compiles the CRT, libc, stubs and your payloads | 18.1.3 |
| `ld.lld` | **yes** | `sce_stubs/Makefile` links stub `.so`s with `-m elf_x86_64 -shared` | 18.1.3 |
| `llvm-config` | **yes** | `Makefile.inc` uses it to find `LLVM_BINDIR` | 18 |
| `make` | **yes** | the SDK and all samples are Make-based | 4.3 (distro) |
| `wget` | **yes** | `libcxx.sh` fetches llvm-project; stub Makefile fetches `aerolib.csv` | distro |
| `python3` | **yes in practice** | `Makefile.inc` sets `PYTHON ?= python3`; needed by `genstub.py` and `make_fself.py` | 3.12.3 |
| `pyelftools` | for stub generation | `genstub.py` does `from elftools.elf.elffile import ELFFile` | distro |
| `socat` | **yes for deployment** | `prospero-deploy` is a `socat` one-liner — deployment fails without it | distro |
| `cmake` | optional | `samples/hello_cmake`, and required by `libcxx.sh` | 3.31.6 |
| `ninja` | optional | not used by the SDK itself; used here for `compile_commands.json` | 1.12.1 |
| `meson` | optional | `samples/hello_meson`, `toolchain/prospero.ini` | distro |
| `pkg-config` | optional | wrapped as `prospero-pkg-config` for ports | distro |

The upstream README lists CMake/Meson/pkg-config/Python as "optional". That is
true of *building the SDK*, but misleading for real work: without `socat` you
cannot deploy, and without `cmake` you cannot run `libcxx.sh`, so C++ payloads
will not link.

## 4. LLVM, Clang and LLD requirements

Three sources, and they do not fully agree — which is why this matters.

1. **README** (Debian/Ubuntu): `clang-18`, `lld-18`.
2. **CI** (`.github/workflows/ubuntu-latest.yml`), which is what actually
   produces the releases:
   ```
   sudo apt install clang-18 lld-18 mingw-w64 cmake meson
   ```
3. **`Makefile.inc`**, which probes in this order and takes the first hit:
   ```make
   LLVM_CONFIG_CANDIDATES := llvm-config-22 llvm-config-21 \
                             llvm-config-20 llvm-config-19 llvm-config-18 \
                             llvm-config-17 llvm-config-16 llvm-config-15 \
                             llvm-config
   ```

So the SDK **tolerates LLVM 15–22**, but only **18** is CI-verified end to end.

The tiebreaker is `libcxx.sh`, which hardcodes:

```bash
LLVM_VER="18.1.8"
```

and cross-builds libc++/libc++abi/libunwind from those sources into the PS5
sysroot. ProsperoPlayer links `-lc++ -lc++abi`, so the C++ ABI in the sysroot is
18.1.x. **Pinning the host compiler to 18 keeps the compiler and the runtime on
the same major version.**

**Decision: LLVM 18.** Documented alternative: 19–22 work per
`Makefile.inc` (v0.41's notes say "host: add support for llvm-22"), but Ubuntu
24.04 has no clang-20+, so you would need the apt.llvm.org repository, and you
would be pairing a newer clang with 18.1.8 libc++ sources — untested upstream.
Build with `--build-arg LLVM_VERSION=20` if you want to try.

## 5. SCE stubs

`sce_stubs/` contains **32 entries** at v0.42. Stubs are generated from Sony
`.sprx` modules by `genstub.py`, which walks `PT_DYNAMIC` with pyelftools and
maps NIDs to names via `aerolib.csv` (fetched from
`zecoxao/sce_symbols`). Each `.c` is a list of bare `asm(".global ...")`
declarations, linked into a `.so` with a matching `-soname`.

### Available (relevant to EVO Player)

| Stub | Provides | Used by |
|---|---|---|
| `libSceVideoOut.c` | ~170 symbols: `sceVideoOutOpen`, `SetBufferAttribute`, `RegisterBuffers`, `SubmitFlip`, `WaitVblank`, `GetResolutionStatus`, … | `videoout_test` |
| `libSceAudioOut.c` | `sceAudioOutInit/Open/Output/SetVolume/Close`, plus the whole `sceAudioOut2*` context API | `audioout_test` |
| `libSceGnmDriver.c` | `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceGnmAreSubmitsAllowed`, `sceGnmSubmitDone`, … | `gpu_test` |
| `libSceGnmDriverForNeoMode.c` | PS4-Pro-mode variant — **not** what a PS5 payload wants | — |
| `libkernel.c` | `sceKernelAllocateDirectMemory`, `MapDirectMemory`, `sceKernelDlsym`, `LoadStartModule`, … | all |
| `libSceUserService.c` | `Initialize`, `GetInitialUser`, `GetLoginUserIdList` — **no `Terminate`** | videoout/audioout |
| `libSceSysmodule.c` | `LoadModule`, `LoadModuleInternal`, `IsLoaded`, … | decoder research |
| `libSceSystemService.c`, `libSceNotification.c`, `libScePad.c`, `libSceKeyboard.c`, `libSceImeDialog.c` | — | ProsperoPlayer |
| `libSceNet.c`, `libSceNetCtl.c`, `libSceHttp.c`, `libSceHttp2.c`, `libSceSsl.c` | networking | — |
| `libSceAppInstUtil.c` | app registration | `install_app` / packaging |

### Absent — and this shapes the roadmap

There are **no stubs** for any native media decode module:

```
libSceAvPlayer      libSceAvPlayer.native   libSceAvPlayerStreaming
libSceVdecCore      libSceVdecShevc         libSceVdecSvp9
libSceVdecwrap      libSceVideoDecoderArbitration
```

You therefore **cannot** `-lSceVdecCore`. Two routes exist; see
[native-media-research.md](native-media-research.md) and
[proprietary.md](proprietary.md).

### Headers

`include/ps5/` ships only `kernel.h`, `klog.h`, `mdbg.h`, `nid.h`, `payload.h`.
**There are no VideoOut/AudioOut/GNM headers at all.** Every SCE prototype used
by this repo is hand-declared in
[`projects/common/include/evo_ps5.h`](../projects/common/include/evo_ps5.h),
and each one was checked against the stub symbol lists so it links.

`ps5/kernel.h` is valuable beyond firmware detection — it exposes
`kernel_dynlib_handle()`, `kernel_dynlib_dlsym()` and
`kernel_dynlib_mapbase_addr()`, which resolve symbols in already-mapped modules
with no stub required. That is the basis of `decoder_test`.

## 6. Dynamic and SPRX loading

Supported, via the CRT's own runtime loader:

| Source | Capability | Sample |
|---|---|---|
| `crt/rtld_dlfcn.c` | `dlopen`/`dlsym`/`dlclose` | `samples/hello_dlfcn` |
| `crt/rtld_sprx.c` | loading Sony `.sprx` system modules | `samples/hello_sprx` |
| `crt/rtld_so.c` | loading ELF shared objects | `samples/hello_so` |
| `crt/rtld_payload.c` | payload-in-payload | — |

`sceKernelDlsym` and `sceKernelLoadStartModule` are also exported by the
libkernel stub.

## 7. Deployment

`host/bin/prospero-deploy`:

```bash
socat -t 9999999 - TCP:$HOST:$PORT < payload.elf
```

- Default port **9021**, matching `ps5-payload-elfldr`. Every sample Makefile
  defaults `PS5_HOST ?= ps5` and `PS5_PORT ?= 9021`.
- `-i` appends stdin for payloads that read it.
- Supported loaders: `ps5-payload-elfldr`, `ps5-payload-websrv`,
  `bdj-ipv6-hen`, `elfloader` (ps5-jar-loader), `remote_lua_loader`.
- Debugging uses `gdb-multiarch` against `ps5-payload-gdbsrv` on TCP **2159**.

## 8. Toolchain environment

`toolchain/prospero.sh` exports `CC`, `CXX`, `LD`, `AR`, `NM`, `RANLIB`,
`STRIP`, `OBJCOPY`, `CMAKE`, `MESON`, `PKG_CONFIG`, `PS5_DEPLOY`,
`PS5_CROSS_FIX_ROOT`, `PS5_SYSROOT` (`$SDK/target`) and `PS5_HBROOT`
(`/user/homebrew`). `prospero.mk` is the Make equivalent, `prospero.cmake` the
CMake toolchain file (`CMAKE_SYSTEM_NAME FreeBSD`, `PROSPERO TRUE`).

`prospero-cmake` / `prospero-meson` are thin wrappers that inject the toolchain
file — **always use `$CMAKE`, never bare `cmake`**, or you will silently
configure for the host.

## 9. Prebuilt libraries (pacbrew-repo)

[ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
cross-compiles ~60 libraries as Arch-style PKGBUILDs. Its CI tars the whole
`/opt/ps5-payload-sdk` tree into `ps5-payload-dev.tar.gz` per release.

- Pinned here: **v0.39** (2026-08-02).
- Contains **FFmpeg 7.0.1**, SDL2, mesa, libass, freetype, openssl, and the
  codec libraries (flac, opus, vorbis, lame, faad2, libvpx, libmpeg2, …).
- This image installs **only** `target/user/homebrew` from that tarball, so the
  pinned SDK v0.42 is not overwritten by pacbrew's own older SDK copy. See
  `scripts/install-pacbrew-image.sh`.

## Sources

- <https://github.com/ps5-payload-dev/sdk> (v0.42: `Makefile.inc`, `ci.sh`, `libcxx.sh`, `crt/kernel.c`, `sce_stubs/`, `host/`, `.github/workflows/ubuntu-latest.yml`)
- <https://github.com/ps5-payload-dev/pacbrew-repo> (v0.39: `ffmpeg/PKGBUILD`, `ci-libs.sh`)
- <https://github.com/ps5-payload-dev/websrv>
