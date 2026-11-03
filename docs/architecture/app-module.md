# The app module (`PPSA99039`)

EVO ships as a fake-signed, game-category **app module**: its own title id,
`param.json`, sandbox and user session, mounted by ShadowMountPlus from a PFS
image. That is the only context where `sceVideodec2` hardware decode and `sceAgc`
GPU rendering work. A payload loaded through elfldr or hbldr gets a borrowed
process and fails at the driver (errno 5200 on decode, no graphics path), so
there is no ELF route and no second install path.

Build and deploy commands are in [build/tooling.md](../build/tooling.md) and
[build/packaging.md](../build/packaging.md). This page is what the module *is*,
and the traps that come with running in it.

## Title and `param.json`

`projects/evoplayer/sce_sys/param.json`; `package-app.sh` validates it on every
build.

| Field | Value | Why |
|---|---|---|
| `titleId` | `PPSA99039` | `PPSA99xxx` is the homebrew-safe range. `PPSA22xxx` is inside Sony's retail allocation and a collision corrupts the Shell DB |
| `contentId` | `UP9000-PPSA99039_00-EVOPLAYER0000000` | the suffix must be 16 characters |
| `applicationCategoryType` / `contentBadgeType` | `0` / `1` | game category; the validator requires this pair |
| `gameIntent.permittedIntents` | `launchActivity` | required for category 0 |
| `attribute` | `1644167168` (`0x62000000`) | required for the HDR-capable VideoOut profile |
| `attribute3` | `524352` (`0x80040`) | standard memory. `--pro-memory` sets bit 22 in the *packaged* copy only |
| `kernel.flexibleMemorySize` | `1073741824` | 1 GiB, the most the console accepts. Default is 448 MB ([hardware/memory-budget.md](../hardware/memory-budget.md)) |
| `downloadDataSize` | `256` | must be above 0 or `/download0` is not writable |
| `contentVersion` | `NN.NNN.NNN` | **raise it every release** (0.13.0 is `01.013.000`), or ProsperoStore never offers the update; the release workflow fails if it did not go up |

Loader constants stay on the cross-firmware profile: module SDK `0x02000009`,
companion `0x08050001`, FSELF magic `0x1D3D154F`. Use one stable title id.
Churning ids litters the home screen with dead tiles, and `--undeploy` does not
clear the Shell DB entry.

## Build tail

Compilation uses the SDK's `prospero-clang` with EVO's normal per-object Makefile
and `-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections`. TLS is
native; the vendored `prospero-clang18` wrapper would force `-femulated-tls` and
`package-app.sh` deliberately does not use it. The link, module conversion and signing come from `tools/native-app/`, vendored
from `blackbearreloaded/ps5-native-app-boilerplate` (GPL-3.0-or-later; EVO is
too, so vendoring adds no obligation):

| Piece | Role |
|---|---|
| `app_crt.cpp` | custom `_start`; `main` is asm-aliased; passes `envp = NULL`, so **`getenv()` returns NULL** |
| `app_cpp_runtime.cpp` | `operator new/delete`; allocations of 64 KB and up go to `mmap` |
| `ps5-pie.ld`, `app-symbols.map` | segregated `PT_LOAD` layout; every app symbol hidden so the module sees only PS5 imports |
| `native_app_builder.cpp` (+ writers) | rewrites the LLVM PIE into a PS5 module, rewrites imports to SCE stubs, signs to `eboot.bin` |
| `libc_builder.cpp` + `runtime/*.txt` | deterministically emits the clean-room `libc.prx` shim, which ships with its emitter source (that is what makes redistributing it compliant) |
| `stubs/malloc_shim.c` | interposes `malloc`/`free`/`posix_memalign` onto an mmap-backed allocator; the shim's own heap is bounded and filled up on hardware |
| `stubs/libc_ext.c` | `_setjmp`/`_longjmp`, `dladdr` and `recvmmsg` stubs |
| `stubs/evo_dns.c` | `getaddrinfo` and friends on the console's own resolver (`libSceNet`); FW 12.70 exports them only as NULL stubs. Host test: `tools/dns_host.sh` |

The link also patches `librmlui.a` on a build-local copy (a finer corner
tessellation, `ui_rml/src/rmlui_patch/`), and `package-app.sh` refuses any `.syms`
symbol that no object imports.

### System PRX imports

The SDK ships no link stubs for some system modules. Each `*.syms` file in
`tools/native-app/stubs/prx/` becomes a stub `.so`, and the converter turns an
undefined symbol into a real PS5 import, so the loader auto-loads the module at
start. Today: `libSceVideodec2`, `libSceAgc`, `libSceAgcDriver`,
`libSceCommonDialog`, `libSceAudiodec`, `libSceAudioIn`, `libSceAvPlayer`,
`libSceWebBrowserDialog`. To use a function from one, `extern` it and add its name
to the `.syms` file; do not `sceKernelLoadStartModule` it.

- A name that no object imports, or that the firmware module does not export,
  makes the loader reject the **whole** module at launch (CE-108255-1) with no log
  line. `package-app.sh` stops the build on a dead entry for that reason.
- Modules under `/system/priv/lib` cannot be imported at all (proven with
  `libSceVcodec`). That rules out `libSceAudiodecCpu*` and `libScePsmlBcSisr`.

## Sandbox and the self-unjail

A registered title gets a strict namespace, not a permission wall. Measured with
`projects/sandbox_probe` on 2026-09-02:

- `/app0` (read-only) and `/download0` (writable when `downloadDataSize > 0`)
  exist. `/data`, `/mnt/usb0` and `/mnt/ext0` are **ENOENT**: they are not
  hidden, they are not in the namespace.
- The user session is real (`sceUserServiceGetInitialUser` succeeds), unlike a
  payload's `0x80940004`.
- `opendir()` fails with EPERM on every path (the clean-room libc cannot do its
  internal `fstatfs`), but `open(O_DIRECTORY)` + `getdents()` work.

So EVO asks a resident jailbreak daemon (PS5-Lapy-JB-Daemon, or etaHEN) to lift
its own process. `evo_jailbreak.c` writes `{"PID":"<pid>"}` to
`/download0/etahen_jailbreak`; the daemon polls that file inside the sandbox
mount every 250 ms, applies caps, uid and root/jail vnodes to that pid, and
deletes the file as its "done" signal. The lookup re-reads the root per `open`, so
one request is enough and every later `/mnt/usb0` or `/data` path just resolves.
A first request can lose a timing race; `evo_jailbreak_poll()` keeps re-dropping
it from the frame loop. Without a daemon there is no other route: USB and internal
storage stay empty and EVO logs the warning after about 300 frames.

What follows from it:

- **`evo_data_path()`** picks the data root at run time: `/data/evoplayer` once the
  sandbox is open, `/download0/evoplayer` before that. The fallback does **not**
  survive a relaunch, so settings written in that window are re-bound once the
  unjail lands (`evo_data_path_rebind()`).
- **`evo_readdir` / `evo_mkdir`** replace `opendir` and `mkdir` (`getdents` and
  `sceKernelMkdir`).
- **The GPU runtime and the resident decoders come up before the unjail.** The
  `libSceAgc*` and `libSceVideoOut` calls go dead after the credential swap, so
  the device cannot be created lazily on first draw. A later measurement showed
  `libSceVideodec2` still works after the swap (uid 1 → 0), which is why a decoder
  slot can be resized at run time, but the GPU ordering still holds.

## Traps

| Trap | What happens | Rule |
|---|---|---|
| `#include <iostream>` | its priority-100 static initialiser builds the global locale and dereferences null in the CRT's `_init`; SIGSEGV at load, no log, and layout-sensitive so it can survive for weeks | never include it; use `<cstdio>` |
| a libc function the shim lacks (`strcasestr`, `opendir`, ...) | the import binds to NULL; SIGSEGV at address 0 when first called | only call functions EVO already uses, or add a shim |
| `posix_fadvise` | SIGSYS-class fault from the app sandbox | already guarded by `#ifndef EVO_APP_MODULE` in `evo_stream_io.c`; keep it |
| `stderr` / `printf` | there is no stdout; `fprintf(stderr, ...)` in FFmpeg's `av_log` crashed every file that made it log | log through `EVO_LOGI/W/E` (or `evo_bt()` at boot); FFmpeg's log callback is redirected |
| `sceVideoOutOpen` | returns a bogus handle; panics the console once a compute queue exists | never call it. `evo_agc_runtime.c` owns VideoOut |
| a mid-stream credential swap | libSceAudiodec dies | probe the audio decoders before `evo_jailbreak_self()` |
| `getenv()` | always NULL | pass constants directly |
| stale `.ffpfsc` | two debugging rounds were spent reading logs from an image that never deployed | check the build id in `evo.log` before trusting a run |

## Memory

Three pools, none of them what the ELF-payload era had: direct (about 12 GB, the
only large one), flexible (448 MB by default, 1 GiB with the `param.json` key) and
EVO's own heap through the malloc shim. A refused allocation (`map_fail` in the
`alloc` log lines) is what breaks FFmpeg, not the codec. Read
[hardware/memory-budget.md](../hardware/memory-budget.md) before calling EVO
memory-limited.
