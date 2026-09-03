# Phase 1b — repackage EVO Player as a game-category app module

> **Status (2026-09-03): Milestone 1 COMPLETE — EVO Player runs *and plays* as
> `PPSA99039`.** Tasks 1–7 ✅; **task 8 fixed** — the file-open crash was
> `posix_fadvise()` faulting SIGSYS-class from the app sandbox
> (`evo_stream_io.c`, both call sites now `#ifndef EVO_APP_MODULE`, commit
> `55685aa0`). 1080p + reasonable-4K play through `009_FIRST_FRAME_ENTER`;
> demanding 4K (GTA trailer) hit the ~450 MB fake-signed flex budget → solved
> by native decode (#31, done). Self-unjail via PS5-Lapy-JB-Daemon covers
> `/mnt/usb0` + `/data`. Build/deploy: `./scripts/package-app.sh` →
> `./scripts/deploy-app.sh` → ShadowMount+. §8 below is the historical
> task-8 risk table + the resolution.
>
> **Predecessors:** Phase 0 (`sce_videodec2.h` / [videodec2-abi.md](videodec2-abi.md))
> and the Phase 1 go/no-go gate — **PASSED on hardware 2026-09-01** via the
> ProsperoLight `PROSPEROLIGHT_VDEC_SELF_TEST`. See
> [native-decode-plan.md](native-decode-plan.md) and the
> `native-decode-app-slot-plan` memory.

The gate proved `sceVideodec2Decode` succeeds from a **fake-signed
game-category app module** (its own `TITLE_ID`, `param.json`, sandbox, user
session), launched from a folder under `/data/homebrew/` by ShadowMountPlus —
**not** from an elfldr payload / hbldr borrowed slot. The errno-5200 wall was a
process-context limit only.

**Consequence:** to reach native decode, EVO Player must ship the same way
ProsperoLight does — a static-linked `eboot.bin` + a carried clean-room
`libc.prx` in `sce_module/` + a full `param.json`, deployed as a folder and
launched via ShadowMountPlus. This phase does that repackaging with the
**unchanged FFmpeg-software-decode player**, and proves the app-sandbox context
doesn't break VideoOut, pad, audio, USB browsing, or settings persistence.

---

## 0. The reference: `ps5-native-app-boilerplate`

ProsperoLight is a fork of **`blackbearreloaded/ps5-native-app-boilerplate`**
(`a15ab71`, GPL-3.0-or-later). Its media/streaming code is irrelevant here; its
**build tail** is the entire point. It replaces the SDK's CRT, link step, and
packaging with:

| Piece | File (in ProsperoLight `tooling/native/`) | Role |
|---|---|---|
| CRT | `app_crt.cpp` | custom `_start` → `_init_env` → `.preinit/.init_array` → `main` (asm-aliased) → `exit`; replaces SDK `crt1.o` |
| C++ alloc runtime | `app_cpp_runtime.cpp` | `operator new/delete`; **routes ≥64 KB to `mmap`**, smaller to libc `posix_memalign`; also `ar`-packaged as a fake `libpthread.a` to satisfy libc++'s recorded dependency |
| Linker script | `ps5-pie.ld` | segregated PT_LOAD (text / ro / relro / data), `.text` at 0, keeps `.dynsym`/`.rela` only for the converter to consume then discard |
| Symbol policy | `app-symbols.map` | `{ local: *; }` — every app symbol hidden, so the module writer sees only PS5 imports |
| Compiler wrapper | `prospero-clang18` | `-target x86_64-sie-ps5 -fno-stack-protector -fno-plt -femulated-tls`, SDK sysroot + libc++ v1 includes |
| Host converter | `native_app_builder.cpp` (+ `elf_object`, `self_container`, `sce_module_writer`) | rewrites the intermediate LLVM PIE → PS5 module `eboot.elf`, stamps module-SDK `0x02000009` / companion `0x08050001`, rewrites imports to SCE stubs, then `self --sign` → `eboot.bin` (FSELF magic `0x1D3D154F`) |
| Clean-room libc | `libc_builder.cpp` + `runtime/{api-surface,imports}.txt` | deterministically emits `runtime/libc.prx` (1.28 MB FSELF, 2,566 exports, forwards `malloc`/`free`/`posix_memalign` to `libSceLibcInternal`, stubs/zero-fills the rest) |

Build flow: compile each `src/*` → `prospero-lld -T ps5-pie.ld` against
`$SDK/target/lib/*.so` (`--as-needed`) plus hand-built `--shared` import stubs
for modules the SDK stub set misses → host converter → sign → assemble
`dist/<TITLE_ID>/{eboot.bin, sce_sys/param.json, sce_module/libc.prx, assets/}`
→ FTP to `/data/homebrew/<TITLE_ID>/` → ShadowMountPlus mount + launch from the
Games row.

**Decision: fork this tail into EVO** (`tools/native-app/`), reuse EVO's
existing Makefile for *compilation*, swap only the *link + package* step.

---

## 1. Licensing — settled, no blocker

EVO Player is **already GPL-3.0-or-later** ([LICENSE](../../LICENSE); fork of
ProsperoPlayer, [README.md](../../README.md)). Vendoring GPL-3.0-or-later tooling
and shipping the GPL `libc.prx` adds **no new obligation**.

Compatibility checks (all pass with GPL-3): FFmpeg LGPL-2.1+, pacbrew OpenSSL 3.x
Apache-2.0, FreeType FTL, HarfBuzz MIT, libass ISC.

How ProsperoLight stays clean (mirror this):

- Whole app GPL-3.0-or-later; SPDX header on every source/script; upstream
  copyright stated.
- `libc.prx` ships **with** its in-repo emitter source (`libc_builder.cpp` +
  two text manifests) — that is what makes redistributing the binary
  compliant. No Sony implementation in it.
- SDK / zlib / pacbrew / libc++ archives stay in ignored `.deps/` `vendor/`,
  downloaded + SHA-pinned, **not redistributed** by the repo.
- `stubs/*.c` are linker-only import descriptions, never packaged.

**Housekeeping when vendoring:**

1. Keep BlackBearReloaded SPDX + copyright headers intact.
2. `tools/native-app/README.md` — provenance: forked from
   `blackbearreloaded/ps5-native-app-boilerplate` @ `a15ab71`, GPL-3.0-or-later.
3. Keep `libc_builder.cpp` + `runtime/*.txt` in-repo.
4. `.gitattributes` — force LF on every vendored `.sh` / `.cpp` / `.hpp` /
   `.ld` / `.map` / `.txt` (repo has `core.autocrlf=true`; a CRLF checkout
   breaks the container scripts — see the
   `windows-writes-break-container-scripts` memory).

Vendor, don't submodule: a submodule re-introduces the CRLF/checkout fragility,
and EVO **forks** `runtime/api-surface.txt` (§4) so upstream's pinned digest no
longer applies anyway.

**Console-health "trouble" (separate from licensing):**

- Fake-signed self on an already-jailbroken 12.70 console = standard homebrew,
  exactly what ProsperoLight does. Keep the console off PSN with HEN active.
- `make undeploy` clears the FTP folder but **not** the Shell DB entry
  (`DEPLOYMENT.md`) — churning TITLE_IDs litters the home screen with dead
  tiles. Use **one stable TITLE_ID**, folder-redeploy only.
- Owed console cleanup from earlier attempts (`FAKE00042` dirs, `ndreg`,
  `register.elf`) — do it in the same session.

---

## 2. TITLE_ID and `param.json`

**TITLE_ID: `PPSA99039`.** `PPSA22xxx` is inside Sony's live retail allocation
range — a collision with a real installed game corrupts the Shell DB /
`/system_ex/app/` / savedata namespace. `PPSA99xxx` is the homebrew-safe range
(ProsperoLight = `PPSA99002`). Keeps the "…039" the user asked for.

`projects/evoplayer/sce_sys/param.json` — values from ProsperoLight's
hardware-proven file, adjusted for EVO:

| Field | Value | Note |
|---|---|---|
| `titleId` | `"PPSA99039"` | |
| `conceptId` | `"99039"` | numeric TITLE_ID portion |
| `contentId` | `"UP9000-PPSA99039_00-EVOPLAYER0000000"` | regex `[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}`; suffix is 16 chars |
| `applicationCategoryType` | `0` | game |
| `contentBadgeType` | `1` | game (validator requires `(0,1)` or `(65536,2)`) |
| `gameIntent.permittedIntents` | `[{ "intentType": "launchActivity" }]` | required for category 0 |
| `attribute` | `1644167168` (`0x62000000`) | required for the HDR-capable VideoOut profile (`PORTING.md`) |
| `contentVersion` | `"01.000.001"` | `NN.NNN.NNN` |
| `masterVersion` | `"01.00"` | |
| `downloadDataSize` | `256` (or larger) | **must be > 0** to make `/download0` writable (§5) |
| `requiredSystemSoftwareVersion` / `sdkVersion` | `"0x0000000000000000"` | |

**Loader/container constants stay fixed** to the cross-firmware-validated
profile: module-SDK `0x02000009`, companion-SDK `0x08050001`, FSELF magic
`0x1D3D154F`. These are what the 12.70 loader + ShadowMountPlus accepted.

Category does **not** grant filesystem/codec/network entitlements
(`CONFIGURATION.md`) — media category would buy nothing, and game category is
already proven for `sceVideodec2`.

---

## 3. Linker + CRT approach

### Compilation — unchanged, plus three flags

Keep EVO's [Makefile](../../projects/evoplayer/Makefile) rules and flags (its
`-std`, `-I`, `-D`, per-object build from Track A). Add for the app build only:

```
-femulated-tls -fno-plt -fno-stack-protector
```

to `CFLAGS` and `CXXFLAGS`, matching `prospero-clang18`. (Mixed emulated/native
TLS across the prebuilt FFmpeg archives is risk **R5** below.)

### Link — replace the SDK tail

New `scripts/package-app.sh` (re-execs in the container like the other scripts):

1. `make` → `$(C_OBJS) $(RMLUI_OBJS)`.
2. compile `app_crt.o`, `app_cpp_runtime.o` with `-fno-exceptions -fno-rtti`.
3. `prospero-ar rcs libpthread.a app_cpp_runtime.o`.
4. build `--shared` import stubs for every SCE module EVO imports that has **no**
   `$SDK/target/lib/libSce*.so`. EVO's import set from the Makefile:
   `libSceNotification, libSceSystemService, libSceUserService, libScePad,
   libSceAudioOut, libSceVideoOut, libSceKeyboard, libSceImeDialog` + implied
   `libkernel, libSceSysmodule, libSceLibcInternal, libc, libm, libSceNet`
   (Emby addon). Audit which lack an SDK stub; hand-stub those (ProsperoLight
   ships stubs for PngDec / Videodec2 / CommonDialog / Agc / AgcDriver / Mouse /
   VideoOut as the precedent).
5. `prospero-lld -T ps5-pie.ld --eh-frame-hdr --version-script app-symbols.map
   --exclude-libs=ALL -e _start` over:
   `app_crt.o` + EVO objects + **static** `libc++.a libc++abi.a libunwind.a`
   (EVO currently uses `-lc++ -lc++abi`) + the app archives (rmlui, SDL2,
   FFmpeg×5, ass, freetype, harfbuzz, fribidi, png16, ssl, crypto, samplerate,
   z, bz2, lzma, zstd) + the stubs + `--as-needed $SDK/target/lib/*.so`.
6. `native_app_builder link` → `eboot.elf`.
7. `self --sign` → `eboot.bin`.
8. assemble `dist/PPSA99039/{eboot.bin, sce_sys/param.json,
   sce_module/libc.prx, assets/}`.

Keep `-fexceptions -frtti` on `ui_rml/*.cpp` only (RmlUi needs it); everything
else stays `-std=c11` / `-std=c++17` as today (boilerplate uses c++20 — c++17
sources compile fine either way).

`app-symbols.map`'s `_Zn*/_Zd*` local rules must let EVO's `operator new`
(from `app_cpp_runtime.o`) win over libc++abi's — verify with `llvm-nm` on the
intermediate PIE.

### CRT

Drop the SDK CRT; use `app_crt.cpp`'s
`_start(process_parameters, loader_teardown)`. EVO's `main()` is asm-aliased —
**no EVO source change**. `envp` is passed as `nullptr`, so any `getenv()` in
EVO or FFmpeg returns null (risk **R7**). EVO's quit path calls `exit()` → crt
runs `.fini_array` + `atexit` + loader teardown.

---

## 4. Where EVO's size fights the clean-room libc — risk register

**How resolution actually works.** The carried `sce_module/libc.prx` is a
**shim, not a C library** — 2,566 exports, "much of it project-authored
compatibility stubs or zero-initialised storage" (`RUNTIME_SHIM.md`). Its real
jobs: satisfy the loader's hard requirement for a module named `libc` (with the
`Need_sceLibc` marker + export-library identity), and install a startup
heap-API table that forwards `malloc`/`free`/`posix_memalign` to the **system
`libSceLibcInternal`**. Heavy libc/libm/stdio symbols bind at link time to the
SDK `target/lib/*.so` stubs and resolve on-console to real system modules —
**iff** those symbols are tagged to `libSceLibcInternal`/`libm`/`libkernel`
rather than to `libc`.

ProsperoLight already exercises SDL2 + RmlUi + freetype + harfbuzz + mbedTLS +
opus + zlib through this shim. EVO **adds**: FFmpeg (avcodec/avformat/avutil/
swscale/swresample), libass, fribidi, **OpenSSL** (vs mbedTLS), libsamplerate,
bz2/lzma/zstd, libiconv, libpng16.

| # | Risk | Why EVO-specific | Milestone-1 action |
|---|---|---|---|
| **R1** | **Heap ceiling.** `app_cpp_runtime.cpp` already redirects `operator new ≥64 KB` to `mmap` because "RmlUi allocations exceed the application libc heap." FFmpeg's `av_malloc` → `posix_memalign`/`malloc` **directly**, no redirect. A 4K AVFrame ≈ 25 MB; pools + swscale contexts multiply it. Capped heap → allocation failure or crash. | FFmpeg is the largest un-redirected allocator in the process. EVO already routes converter/decode-output buffers through `evo_direct_mem` (direct memory); FFmpeg-internal malloc remains. | Instrument `malloc` high-water on device during 1080p and 4K play. Confirm the shim's heap table enables `sceLibcHeapExtendedAlloc` (grow via flexible memory). If capped: raise the heap size in the proc-param, or shim `av_malloc` onto `evo_direct_mem`. |
| **R2** | **api-surface coverage.** `runtime/api-surface.txt` (2,572 lines) was harvested from *ProsperoLight's* link. Any `libc`-tagged symbol FFmpeg/OpenSSL/libass reference that isn't in it → unresolved import at load. Suspects: `strtod`/`strtoll`, `qsort`/`bsearch`, `sscanf`/`v*printf` guts, `memmem`, `localtime_r`/`gmtime_r`/`mktime`, `getentropy`/`arc4random`, `secure_getenv`. | FFmpeg + OpenSSL touch several hundred libc entry points. | Re-harvest the surface from EVO's actual intermediate PIE (`llvm-nm -u`), diff against `api-surface.txt`, regenerate `libc.prx`. **Breaks the pinned SHA-256** — expected; EVO forks + re-pins the manifest. |
| **R3** | **libm.** FFmpeg aac/resample/DSP pull `pow/exp/log/sin/cos/atan2/hypot/ldexp/lrint/llrint/rint/fabs`. If any bind to the shim's `libc` rather than system `libm`, they are stubs returning 0 → silent audio/scaling corruption, not a crash. | ProsperoLight (Opus only) barely touches libm. **Host check 2026-09-02: `libm.a` defines all of them as real `T` symbols; `libc.a` defines none of them (no collision); `package-app.sh` links `libm.a` inside the `--start-group`. So they resolve to real impls — R3 is unlikely to be the crash.** | Confirm on device via the `av_log`→notify path + audio plane-hash A/B. Downgraded from "suspect" to "verify". |
| **R4** | **stdio backing.** FFmpeg format probing, `av_log` to stderr, libass `fopen` of font files hit stdio; shim stdio may be a stub. | | Confirm `evo_stream_io` covers all file reads; route `av_log` → `evo_notify`; verify libass font load path (fonts ship under `assets/`). |
| **R5** | **TLS model mismatch.** Wrapper forces `-femulated-tls`; EVO objects + pacbrew FFmpeg archives are native-TLS today. Cross-module `__thread` with mixed models can corrupt silently. FFmpeg uses TLS (error context, RNG). | | ProsperoLight links pacbrew archives under `-femulated-tls` successfully, so coexistence is partly proven. Rebuild EVO objects `-femulated-tls`; if FFmpeg misbehaves, rebuild it via `build-ffmpeg.sh --profile minimal --extra-cflags=-femulated-tls --install` (the §Q4 fallback). |
| **R6** | **Exceptions / unwind.** Static `libunwind.a` + `.eh_frame_hdr`. RmlUi throws; FFmpeg is `-fno-exceptions` C. An exception unwinding through FFmpeg C frames → `std::terminate`. | EVO's RmlUi surface is larger than ProsperoLight's. | Keep RmlUi calls off the FFmpeg thread (already true); smoke-test a deliberate RmlUi parse error. |
| **R7** | **`getenv` null.** crt passes `envp = nullptr`. FFmpeg reads `AV_LOG_FORCE_*`, `http_proxy`; EVO may read `HOME`. | | Audit; supply constants directly. Low severity. |

---

## 5. Filesystem / sandbox — the second gate

A registered title launched from the home screen goes through normal
`sceSblACMgr` sandbox setup — **not** hbldr's already-unsandboxed borrowed
slot, which is why EVO's `opendir("/mnt/usb0")` works today. `PLATFORM_NOTES.md`:
sandbox is `/app0` (RO) + `/download0` (writable **iff `downloadDataSize > 0`**).
`CONFIGURATION.md`: *"Use `/download0` for configuration, pairing state, caches,
logs"*. ProsperoLight never browses USB or `/data`, so **the reference gives us
nothing here.**

EVO touches sandboxed paths everywhere: [main.c:1636](../../projects/evoplayer/main.c#L1636)
(`/mnt/usb0` browse root), [main.c:12074](../../projects/evoplayer/main.c#L12074)
(`mkdir /data/evoplayer`), [main.c:6852](../../projects/evoplayer/main.c#L6852)
(settings), themes ([evo_theme.c:26](../../projects/evoplayer/pp/src/evo_theme.c#L26)),
`emby.conf`, favorites/recent DBs, RmlUi asset fallback paths.

### Step A — probe (first deploy, gates everything)

Tiny build: `opendir` + write-test `/`, `/app0`, `/download0`, `/data`,
`/data/evoplayer`, `/mnt/usb0`, `/mnt/ext0`; report via `evo_notify`. Some HEN
configs apply process-wide sandbox patches — if `/mnt/usb0` and `/data` are
already visible, keep every EVO path as-is and skip Step B.

#### Step A result — hardware, 2026-09-02 (`PPSA99039`, `projects/sandbox_probe`)

Second, layered run (`od`=`opendir` `st`=`stat` `o`=`open(O_RDONLY|O_DIRECTORY)`
`gd`=`getdents(fd)` bytes, `e`=`errno`):

```
userService: init=0x0  getInitialUser=0x0  uid=0x1ea2f4d9   <- REAL user session
/                od:-- e1   st:OK e0   o:OK e0   gd:132 '.'   w:--
/app0            od:-- e1   st:OK e0   o:OK e0   gd:-1        w:OK
/download0       od:-- e1   st:OK e0   o:OK e0   gd:24  '.'   w:OK
/data            od:-- e2   st:-- e2   o:-- e2   gd:-1        w:--
/data/evoplayer  od:-- e2   st:-- e2   o:-- e2   gd:-1        w:--
/mnt/usb0        od:-- e2   st:-- e2   o:-- e2   gd:-1        w:--
/mnt/ext0        od:-- e2   st:-- e2   o:-- e2   gd:-1        w:--
```

Four conclusions:

1. **Real user session.** `sceUserServiceGetInitialUser` → `0x0`, uid
   `0x1ea2f4d9` — not the elfldr `0x80940004`. Unblocks `sceVideoOutOpen`
   with a real user id, audio, save-data. (§9 checklist item ✅.)

2. **`opendir()` fails EPERM; raw `open`+`getdents` work — the fix is an
   `evo_readdir()` over the syscalls, not `opendir`.** On `/` and
   `/download0`: `stat` works, `open(O_RDONLY|O_DIRECTORY)` returns a valid fd,
   and **`getdents` on that fd returns real entries** (`/` → 132 bytes,
   `/download0` → 24 bytes = just `.`, an empty fresh savedata mount). Only the
   `opendir()` *wrapper* fails, **EPERM (1)** — almost certainly at its internal
   `_fstatfs` call (FreeBSD `opendir` = `open` → `_fstatfs` for the read-buffer
   size → malloc → `getdents`). Cause not fully pinned: the sandbox may restrict
   `fstatfs`/`statfs` on the sandboxed mounts, or it is a shim gap — but
   `open`/`stat`/`getdents`/`close` all pass straight through, which argues
   against a broad shim problem. **It doesn't matter which:** EVO's browser
   gets a ~15-line `evo_readdir()` over `open(O_RDONLY|O_DIRECTORY)` +
   `getdents` (both proven on device here). A task-4 `api-surface.txt`
   re-harvest *may* also make `opendir` itself work — bonus, not the plan.

3. **The sandbox is a strict namespace, not a permission wall.** `/data`,
   `/data/evoplayer`, `/mnt/usb0`, `/mnt/ext0` are **ENOENT (2)** from inside —
   they do not exist in the sandbox, not "access denied". So Step B.2 (unjail
   or `nullfs`-bind) is **mandatory** for USB browse; there is no ACL to relax.
   `/download0` read (via `getdents`) + write both work → Step B.1 unblocked.

4. `/app0` opens + stats + is writable but `getdents` returns -1 — the RO app
   image mount enumerates differently; irrelevant to milestone 1.

**Step A is complete.** No more probe rounds needed — task 4 proceeds with two
knowns: (a) expect `fstatfs` & friends in the re-harvested api-surface, and
(b) `sandbox-unjail` / bind is required, not optional.

### Step B — if sandboxed (expected)  ← confirmed by Step A

1. **Settings / DBs → `/download0/evoplayer/`.** `downloadDataSize > 0` in
   `param.json`. Add one prefix helper `evo_data_path()`; route the ~8 literal
   `/data/evoplayer/...` sites through it. Write-temp-then-`rename` (already the
   pattern). Contained change.

2. ✅ **USB media browse → unsandbox the running EVO process.** elfldr payload
   **`projects/sandbox_unjail/`** + wrapper **`tools/sandbox-unjail.sh`**
   (elfldr context *has* kernel R/W via `<ps5/kernel.h>`). It finds the running
   `eboot.bin` proc and applies the identical lift `ps5-payload-elfldr` gives
   its own payloads (`elfldr_raise_privileges`):

   | call | effect |
   |---|---|
   | `kernel_set_proc_rootdir(pid, kernel_get_root_vnode())` | `fd_rdir` → real `/` |
   | `kernel_set_proc_jaildir(pid, 0)` | `fd_jdir` → 0 (no jail root) |
   | `kernel_set_ucred_uid(pid, 0)` | `cr_uid` → 0 |
   | `kernel_set_ucred_caps(pid, {0xff×16})` | full `cr_sceCaps` |

   `namei` reads `fd_rdir`/`fd_jdir` at lookup time, so EVO's **next** `open()`
   sees the real FS — no remount, no path changes, the literal `/mnt/usb0`
   strings just resolve. Only touches procs whose `jaildir != 0` (skips an
   already-unjailed one). **The same primitive HEN applies to the hbldr PS-Now
   slot.**

   Dev loop (launch-safety rules still apply — never stack launches; a game
   also runs as `eboot.bin`, so close one first):
   1. `./scripts/package-app.sh` → `./scripts/deploy-app.sh`
   2. ShadowMountPlus mount + launch from the Games row → RmlUi menu
   3. `PS5_HOST=<ip> ./tools/sandbox-unjail.sh` → elfldr runs the payload,
      on-screen notification reports `pid=N unjailed`
   4. reopen EVO's media browser → `/mnt/usb0` now lists

   No "waiting for filesystem" boot gate was needed — EVO already boots to the
   menu in the sandbox (task 4); only the browser needs the unjail, and
   `load_usb_files()` re-scans on every browser entry.

   **Self-service unjail — no per-launch command (2026-09-02):**
   `projects/evoplayer/src/evo_jailbreak.c` (`EVO_APP_MODULE` only, called at
   `main()` entry). A registered module cannot promote itself — the loader
   re-applies the sandbox every launch and the module has no kernel access
   (third_party/SharpProspero/docs/app-promotion.md). But it can *ask* a
   **persistent jailbreak daemon** (PS5-Lapy-JB-Daemon, or etaHEN) via the
   file-drop protocol: write `{"PID":"<pid>"}` to
   `/download0/etahen_jailbreak`. The daemon polls
   `/mnt/sandbox/<TID>_<NNN>/download0/etahen_jailbreak` every 250 ms, reads
   the pid, applies caps + authid + uid + `sceAttr@0x83` + `fd_rdir`/`fd_jdir`
   = rootvnode, then `unlink()`s the file. `namei` re-reads per lookup, so one
   drop at boot is enough. `evo_jailbreak.c` waits ~1.2 s for the sandbox to
   open and `evo_bt`-reports the outcome.
   - **Daemon running (the user has PS5-Lapy-JB-Daemon): zero extra steps** —
     EVO self-unjails on launch; one promotion opens the real root, so **both**
     media sources resolve — `/mnt/usb0` (USB) and `/data` (INTERNAL STORAGE).
   - **If not:** falls back — the user runs `tools/sandbox-unjail.sh` once per
     launch as before. (Lapy's own note: a first attempt can lose a timing
     race; relaunching the app succeeds.)

   Fallback if the daemon route is unreliable: `nmount`/`nullfs`-bind
   `/mnt/usb0` and a writable `/data/evoplayer` into
   `/mnt/sandbox/PPSA99039_000/…` — more code (iovec construction), narrower
   blast radius.

---

## 6. FFmpeg source

**Primary: pacbrew's prebuilt archives** — what ProsperoLight proved links
cleanly under this toolchain, same source EVO already uses
([build-evoplayer.sh](../../scripts/build-evoplayer.sh) links pacbrew's
`libav*.a`).

**Fallback:** `scripts/build-ffmpeg.sh --profile minimal` rebuilt with
`--extra-cflags=-femulated-tls` and `--install`'d into the sysroot — drop-in if
milestone 1 hits R5. No decision needed now.

---

## 7. Repo integration

`✅` = landed & hardware-verified (2026-09-02). `◻` = later milestone-1 tasks.

```
✅ tools/native-app/              vendored from ps5-native-app-boilerplate a15ab71
   ✅ README.md  runtime/README.md      provenance + SPDX + per-file map
   ✅ .gitattributes                    LF-lock tools/native-app/**
   ✅ app_crt.cpp  app_cpp_runtime.cpp  prospero-clang18
   ✅ ps5-pie.ld (+ __eh_frame bounds)  app-symbols.map
   ✅ native_app_builder.cpp  elf_object.{cpp,hpp}  hash.hpp
   ✅ self_container.{cpp,hpp}  sce_module_writer.{cpp,hpp}
   ✅ libc_builder.cpp
   ✅ runtime/{api-surface,imports}.txt  runtime/libc.prx.sha256   pinned upstream
   ✅ stubs/libc_ext.c                   _setjmp/_longjmp, dladdr/__dl*, recvmmsg
   ✅ stubs/malloc_shim.c                mmap-backed allocator (R1)
   ✅ stubs/*_link_stub.c                7 SCE link stubs (unused so far)
✅ projects/sandbox_unjail/{main.c,Makefile}          elfldr payload: unjail EVO proc (task 7)
✅ tools/sandbox-unjail.sh                            build + push to elfldr, host wrapper (task 7)
✅ projects/evoplayer/sce_sys/{param.json,icon0.png}   PPSA99039, game category
✅ projects/evoplayer/Makefile                         `objects` / `print-objects`
✅ projects/evoplayer/include/evo_boot_trace.h         EVO_BOOT_TRACE breadcrumbs
✅ projects/sandbox_probe/{main.c,Makefile}            §5 Step A probe (+ payload A/B)
✅ projects/app_ctl/{main.c,Makefile}                  launch/kill payload for app-loop.sh
✅ projects/evoplayer/{src/evo_data_path.c,src/evo_readdir.c,
   include/evo_data_path.h,include/evo_readdir.h}       /download0 helper + getdents enum (task 6)
✅ scripts/setup-native-app-deps.sh    static zlib 1.3.2 bootstrap into .deps/
✅ scripts/package-app.sh  [--probe|--player(default)|--rebuild-libc]
✅ scripts/deploy-app.sh               FTP output/app/PPSA99039/ → /data/homebrew/
✅ tools/app-loop.sh                   build → deploy → relaunch → klog (unattended;
                                       launch step blocked, see below)
✅ Dockerfile / docker-compose.yml     bake static zlib 1.3.2 at EVO_NATIVE_ZLIB
✅ .gitignore                          .deps/  output/app/  runtime/libc.prx
```

Output lives under `output/app/` (EVO convention), not `dist/`.

**Docker:** the image bakes SHA-pinned static zlib 1.3.2 at `$EVO_NATIVE_ZLIB`
for the host converter; `scripts/setup-native-app-deps.sh` rebuilds the same
archive into `.deps/native-app/` at runtime when the image predates that layer
(no image rebuild needed to iterate). Only zlib is fetched — the SDK is already
in the image. Not dependent on the stale `pl:build` image.

**Automation (`tools/app-loop.sh`):** build → deploy → `app_ctl` payload →
30 s klog capture → parse `EVO boot:` lines, all unattended. Two gaps found on
hardware: (a) `sceKernelDebugOutText` from the app sandbox does **not** reach
klogsrv — only the system-notification popup works, so breadcrumbs must be read
off the TV; (b) `sceSystemServiceLaunchApp("PPSA99039")` from an hbldr payload
returns `0x80940005` (`isValid(titleId)` false) — the title needs a live
ShadowMountPlus registration. So each iteration still needs a manual
mount+launch and a glance at the screen.

---

## 8. Milestone 1 — task order

**Goal:** unchanged FFmpeg-software-decode player boots as `PPSA99039`, reaches
the RmlUi main menu, and VideoOut / pad / audio / USB browse / settings all work
in the app sandbox. No native decode.

1. ✅ Vendor `tools/native-app/`, LF-normalise, `.gitattributes`, extend Docker
   image, this doc. *(2026-09-02)*
2. ✅ **Sandbox probe** (§5 Step A). Hardware result recorded in §5. *(2026-09-02)*
3. *(folded into task 2)* `param.json` + `package-app.sh` skeleton.
4. ✅ **Full player boots as an app module** — `./scripts/package-app.sh`
   → `./scripts/deploy-app.sh` → ShadowMountPlus. **On hardware
   2026-09-02: boots to the RmlUi main menu, pad navigation works, VideoOut /
   RmlUi / FFmpeg / audio / networking all initialise in the app sandbox.**

   All 43 EVO objects compile with `prospero-clang` (the SDK clang already
   forces `-femulated-tls -fno-plt -fno-stack-protector`, so **task 5 folds in
   here** — native TLS was never the SDK default). `eboot.bin` ≈ 34 MB, signs
   `integrity: valid`, authority `0x3100000000000002`. **No `api-surface.txt`
   re-harvest** — the runtime shim stays on the pinned upstream digest.

   Five fixes, all in `tools/native-app/` (details in its README):

   | Fix | Why | Symptom it cured |
   |---|---|---|
   | `ps5-pie.ld` — `PROVIDE __eh_frame[_hdr]_{start,end}` | a custom `-T` script must define them; `libunwind.a` refs them | link error |
   | link static SDK `libc.a` in a `--start-group`, **last** | supplies `__emutls_get_address` **and** (via `no-locale.o`) real C-locale `strtod_l`/`mbrtowc_l`/`___runetype_l`/`__runes_for_locale`/`_DefaultRuneLocale`/… | `SIGSEGV rip=0` in `std::__1::DoIOSInit` — libc++ iostream static-init called a NULL xlocale import (the console's real `libSceLibcInternal` doesn't export the `*_l` family; an earlier "regenerate the stub with `return 0` bodies" attempt failed because stub bodies are discarded and the imports bound to 0) |
   | `stubs/libc_ext.c` (new, small) — `_setjmp`/`_longjmp` asm-alias, `dladdr` + `__dl*` no-ops, `recvmmsg`/`sendmmsg` ENOSYS | not in libc.a, or (dladdr) drags libc.a's `dladdr.o` → `__dlopen` cascade the converter can't resolve | converter error, later NULL calls |
   | `stubs/malloc_shim.c` (new) — interpose `malloc`/`free`/`calloc`/`realloc`/`posix_memalign`/… onto an **mmap-backed slab + large allocator** | the clean-room `libc.prx` installs a **bounded** heap mspace; it fills up and `malloc()` returns NULL with ~360 MB flexible memory still free | `pp_videoout_init` step 15: `malloc(8 MB)` NULL while `mmap(8 MB)` OK. This is plan **R1** — now fixed process-wide (FFmpeg's `av_malloc` included) |
   | `-DEVO_BOOT_TRACE` breadcrumbs (`projects/evoplayer/include/evo_boot_trace.h`) | app-sandbox has no visible stdout; `sceKernelDebugOutText` doesn't reach klogsrv either | each `main()` init milestone pops a system notification — the only channel that works. Set only by `package-app.sh`; inert otherwise. Remove once §9 is signed. |

5. ✅ *(folded into task 4)* — every object built `-femulated-tls`. Residual
   heap/TLS risk (R1/R5) addressed by `malloc_shim.c`; watch at runtime.
6. ✅ `evo_data_path()` → `/download0/evoplayer/` settings migration (§5 Step B.1)
   + `evo_readdir()` over `open`+`getdents` for the browser (§5 Step A #2).
   **Hardware 2026-09-02:** `EVO_DATA_DIR` (compile-time, `-DEVO_APP_MODULE=1`
   from `package-app.sh`) routes settings / recent / favorites / last-folder /
   emby.conf / themes to `/download0/evoplayer/`; settings write + **persist
   across a clean PS-button relaunch** (`settings loaded`); `evo_readdir` over
   `open(O_DIRECTORY)`+`getdents` enumerated `/download0/evoplayer/` correctly
   (`2 entries first=evo_last_folder.cfg`); `evo_opendir("/mnt/usb0")` returns
   NULL cleanly → browser shows "NOT FOUND", no crash/hang.
   New: `src/evo_data_path.{c},include/evo_data_path.h`,
   `src/evo_readdir.{c},include/evo_readdir.h`. Boot trace trimmed to 4 lines.

   > ⚠️ **Superseded by #46 (code 2026-09-03).** The "persists across relaunch"
   > result above did **not** hold once `evo_jailbreak_self()` moved to `main()`
   > boot — `/download0/evoplayer/` is a savedata-relative mount with no
   > `sceSaveDataMount2`/commit, so it is wiped every launch. `evo_data_dir()` /
   > `evo_data_path()` now resolve the root at **runtime**: `/data/evoplayer`
   > once the sandbox is open, `/download0/evoplayer` only as a pre-unjail
   > fallback. `evo_mkdir()` uses `sceKernelMkdir` (POSIX `mkdir` absent from
   > the shim surface). See `docs/evo-pro/status.md` #46 block.
7. ✅ `sandbox_unjail` elfldr payload (§5 Step B.2) — `/mnt/usb0` is ENOENT in
   the sandbox. `projects/sandbox_unjail/` + `tools/sandbox-unjail.sh`; applies
   `elfldr_raise_privileges`' rootdir/jaildir/uid/caps lift to the running
   process. No boot gate — EVO boots fine sandboxed, only the browser needs it.
   **Hardware 2026-09-02: works — after `./tools/sandbox-unjail.sh`, EVO's USB
   browser lists `/mnt/usb0`.** Must be re-run after every relaunch (the lift is
   per-process); a resident auto-unjail daemon is the "production later" item.
   Process lookup: `sysctl {1,14,8,0}` namelen 4 (`KERN_PROC_PROC`) — the
   namelen-3 `KERN_PROC_ALL` form in `projects/app_ctl` returns EINVAL on this
   console; `ki_pid`@72, `ki_tdname`@447.
8. **NEXT** — instrumented playback. **2026-09-02: picking a media file
   crashes the app module** (menu / browse / settings all stable). Unproven
   before this and expected to need work — R1 (FFmpeg `av_malloc` vs the capped
   libc heap), R3 (libm binding), the `exit()` `SIGSYS`, and possibly the
   mid-run cred swap from `sandbox_unjail` as a new vector.

   **Diagnostic instrumentation landed (commit `9c7b3fa`, console-free):**
   - `pp_stage_breadcrumb.c` — under `EVO_APP_MODULE`, every `pp_stage_bc*`
     checkpoint also fires a notification + klog line. Lights up the existing
     `001..012` trail, invisible before (sandbox `/mnt/usb0` = ENOENT).
   - `main.c` `EVO_P8()` — unconditional breadcrumbs `P8_00..P8_31` down
     `start_video_playback` (the existing checkpoints are all inside the 4K
     branch; a 1080p file hits none).
   - `av_log` → notify callback (`AV_LOG_ERROR`), so the *reason* an
     `avformat_open_input` / `avcodec_open2` fails reaches out of the sandbox.
   - `malloc_shim.c` `evo_alloc_stats()` — mmap high-water, reported at
     `P8_31` (checks R1 from the other side).

   Next hardware session: deploy `package-app.sh --agc-probe`, pick a 1080p
   file (`test.mp4` / `rabbit.mp4`), read the last `EVO bc:` / `EVO P8` /
   `P8_AVLOG` notification before the crash → that names the failing call.
   Then 4K. The same launch's `EVO agc:` line answers the GPU Step 2 gate.

---

## 9. On-hardware verification checklist ("milestone 1 done")

- [x] Title in the Games row, launches; no loader / fatal-signal / crash / panic
      marker on boot — 2026-09-02
- [x] `sceVideoOutOpen` returns a real handle from the app module; RmlUi main
      menu renders correctly (colours right, no `0xAABBGGRR` swap) — 2026-09-02
- [ ] Menu frame pacing matches `main` (uiplay baseline) — not yet measured
- [x] `sceUserServiceGetInitialUser` returns a real user (not `0x80940004`)
      — uid `0x1ea2f4d9`, Step A 2026-09-02
- [x] `libScePad` drives navigation — 2026-09-02
- [x] **directory enumeration works** — `evo_readdir` over
      `open(O_DIRECTORY)`+`getdents` listed `/download0/evoplayer/` (2 entries,
      names correct), 2026-09-02. `opendir` still fails EPERM (unused now).
- [ ] `libSceAudioOut` — play a file, audio present, A/V sync within `main`
      tolerance
- [ ] elfldr / klog / websrv / ftpsrv stay healthy after exit; clean PS-button
      close (a `SIGSYS` fired in `exit()` teardown on the early-exit path —
      revisit once the app has a real quit path)
- [x] USB browse lists `/mnt/usb0` media after `./tools/sandbox-unjail.sh`
      — 2026-09-02 (must re-run the unjail after every relaunch)
- [x] Settings read+write survives relaunch — `/download0/evoplayer/`,
      `settings loaded` after a clean PS-button close, 2026-09-02
- [ ] 1080p **and** 4K FFmpeg software decode play without allocation failure;
      decode plane hashes match `main`
- [ ] Clean close via PS button → home screen, no panic
- [ ] No stacked launches at any point; `tools/launch.sh` / ShadowMountPlus only

---

## 10. Open items

- `tools/sandbox-unjail` proc-jail internals for 12.70: confirm the `fd_rdir`/
  `fd_jdir` offsets + `sceSblACMgr` flag layout against the SDK's `crt/kernel.c`
  12.70 block and existing HEN source before writing the payload.
- Whether ShadowMountPlus already exposes a hook to run a payload at launch
  (would remove the manual step 4 even for dev).
- Exact heap-config field in the fake proc-param that raises the libc heap
  ceiling (R1), if the shim's `sceLibcHeapExtendedAlloc` path isn't enough.
- Which of EVO's SCE imports lack an `$SDK/target/lib` stub (§3.4) — enumerate
  before writing `tools/native-app/stubs/`.

## 11. After milestone 1

Milestone 2+: bring `evo_vdec_native.c` up behind the existing
[evo_vdec.h](../../projects/evoplayer/media/include/evo_vdec.h) seam (Track A
already carved `evo_vdec_ffmpeg.c`), using the verified
[videodec2-abi.md](videodec2-abi.md) sequence — resuming
[native-decode-plan.md](native-decode-plan.md) Phase 4.
