# GL-1 spike (#77) — prove `ps5-opengl` renders on FW 12.70

> **Status (2026-09-09): GO — verified on the FW-12.70 console.** The GL smoke
> rendered (Mesa 26.2, GL 3.3 Core, GLSL 330, PS5 AGC backend), pixel-exact
> readback, no driver fail-stop. Receipt in §7. GL-2 (#78) is unblocked.
> Story GL-1 of the [OpenGL render overhaul](opengl-render-overhaul.md).

`ps5-opengl` (blackbearreloaded — Mesa 26.2 + a bespoke PS5 Gallium driver +
a patched OpenGNM PSBC) was validated on a **FW-6.02 research console**. EVO
runs on **12.70**. This spike vendors it, makes its fail-stop paths safe for
the app module, resolves the allocator collision, confirms the init slot, and
adds a self-contained GL smoke test to EVO's own `.ffpfsc` so the go/no-go can
be answered with one console launch.

**If the smoke does not render on 12.70, the overhaul stops here** (GL-2…GL-6
do not start) and this doc records where it failed.

---

## 1. Vendor decision — git submodule

`third_party/ps5-opengl` is a **git submodule** (`.gitmodules`), pinned to
`9eb75fc` (`sdk-0.1.0-perf20260909-g55-hfr-sdl2-focused`). Bumped G47
(`23a594c`) → G55 for GL-4 (#80). GL-1 smoke re-verified on hardware (§7a);
the re-measure showed the `glTexSubImage2D` staging copy is **RGBA8-only** —
R8/RG8 uploads are free, so GL-4's NV12 path needs no zero-copy. See
[gl4-video-path-plan.md](gl4-video-path-plan.md).

- Submodule, not a vendored copy: the tree is Mesa-sized and has its own fast
  release cadence; a gitlink keeps EVO's history clean and the pin explicit.
- Submodule, not the prebuilt `v0.1.0-perf20260907-sampled` archive: we want
  the source so the `_Exit` patch and any 12.70 driver fixes are in-tree. The
  prebuilt archive stays the **fallback** — `build-ps5-opengl.sh --frozen-archive`.
- `git submodule update --init third_party/ps5-opengl` after checkout;
  `scripts/build-ps5-opengl.sh` does it automatically.
- **Bumping the pin** = re-run the GL-1 smoke on hardware and refresh the
  `_Exit` patch (below), then record a new receipt here.

---

## 2. `_Exit` → recoverable

`ps5-opengl` hard-exits on four unrecoverable GPU paths. `_exit()` from the app
module strands the PS5 app slot (only a PS-button close frees it), loses resume
state, and skips teardown — forbidden by `CLAUDE.md`.

| File | Site (line @ G55) |
|---|---|
| `src/gallium/ps5/ps5_screen.c:3139` | `ps5_release_resource_memory` — unmap/release failed |
| `src/gallium/ps5/ps5_screen.c:7908` | `ps5_context_queue_present` — queued present failed |
| `src/gallium/ps5/ps5_screen.c:7926` | `ps5_draw_batch_flush_locked` — deferred batch cleanup failed |
| `src/platform/ps5_agc_native_runtime.c:774` | `runtime_require_retirement` — submission retirement failed |

**`patches/ps5-opengl/0001-recoverable-fail.patch`** replaces each
`_Exit(EXIT_FAILURE)` with `ps5gl_fatal("<site tag>")`, declared
`__attribute__((weak, noreturn))` in a new `src/platform/ps5gl_fatal.h` whose
weak body is the original `_Exit` (so a standalone `ps5-opengl` build is
unchanged). `scripts/build-ps5-opengl.sh` applies it idempotently after
`submodule update`.

**EVO's strong override** — `projects/evoplayer/pp/src/pp_gl_fatal.c`
(only linked with `--gl-smoke`):

- logs `GL-1 SMOKE: driver fail-stop caught (<tag>)` to `/mnt/usb0/evo.log`,
  sets `g_pp_gl_dead`;
- if `pp_gl_smoke_run()` armed the `setjmp` fence → `longjmp` back, reported as
  `result=FAIL stage=<n> reason=driver-fail-stop`;
- otherwise parks (heartbeat + `sleep`), still never `_exit()`.

**Backstop:** `scripts/package-app.sh --gl-smoke` links `-Wl,--wrap=_Exit`, so
any `_Exit` the patch missed (e.g. deep in a vendored dep) also routes through
`pp_gl_fatal.c`.

GL-2+ inherit this hook. A real GPU device-loss / fault-recovery contract is a
**follow-up**, not part of #77.

---

## 3. Allocator coexistence — `malloc_shim` wins

`ps5-opengl` mandates (`docs/consumer-build.md`):

```
--wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size
```

plus its app-owned **128 MiB process-lifetime heap** (`native-app/app_heap.c`).

EVO already wraps **exactly that set** in
`tools/native-app/stubs/malloc_shim.c` (compiled first in link order for the
player build), routing every allocation to `sceKernelMapNamedFlexibleMemory`
(the full title budget — GBs — not the small anonymous-`mmap` pool). The
clean-room `libc.prx` bounded heap fills up otherwise (hardware 2026-09-02).

**Decision: do NOT link `ps5-opengl`'s `native-app/app_heap.c`.** EVO's
`malloc_shim` supersedes the fixed 128 MiB heap and the wrap set is identical,
so the "partial wrapping is unsafe" hazard does not arise — every allocator
entry point is already interposed by one owner.

`scripts/package-app.sh --gl-smoke` therefore adds only the ps5-opengl runtime
+ Mesa + PSBC archives, never `app_heap.o` or a second `--wrap=malloc` set.

**To verify on the receipt run:** `ps5-opengl` may assume its heap is
contiguous / process-lifetime for some GPU mappings. The smoke logs the GL
strings and any allocation failure; watch `evo.log` for `get_buffer`-style
errors or an EGL init failure that looks allocator-shaped.

---

## 4. Pre-unjail init slot — confirmed

`libSceAgc*` / `libSceVideoOut` go **API-dead** (`0x811D0111`; sysmodule 207 →
`ESDKVERSION`) after `evo_jailbreak_self()`. GL init must run **before** it.

Slot: `projects/evoplayer/main.c`, immediately before the existing
`pp_agc_init(1920, 1080, 0)` call (`main.c` ~L12100), inside
`#if defined(EVO_GL_SMOKE)`. It runs **unconditionally at boot** in a
`--gl-smoke` eboot — the USB stick is read-only over FTP (STOR to `/mnt/usb0`
returns 550) and `/data` isn't readable pre-unjail, so a runtime hook file was
dropped; the build flag is the switch. A `--gl-smoke` eboot is a dedicated
diagnostic build and is never shipped.

The smoke runs **instead of** `pp_agc_init` and the normal boot — `ps5-opengl`
calls `sceAgcInit` and owns the `sceVideoOut` flip queue, and a second
`sceVideoOut` open panics the console (see the "PS5 kernel panic vectors"
memory / `docs/hardware-decode.md`). After the receipt line is flushed, `main()`
parks in a `sleep(30)` loop — read `evo.log`, reboot.

For GL-3 this means **GL cannot be lazily initialised on first UI draw** — it
initialises in this pre-unjail slot, next to `pp_agc_init`.

---

## 5. Build toolchain gap

The pinned dev image (`Dockerfile` → `evo-player/ps5-dev:llvm18-sdk-v0.42`) is
LLVM 18 + payload SDK v0.42. `ps5-opengl`'s Mesa/PSBC build
(`third_party/ps5-opengl/docs/building.md`) wants — as reported by
`scripts/build-ps5-opengl.sh --check` on the base image:

| Need | Base image | Gap |
|---|---|---|
| Payload SDK v0.42 | ✅ (`dependencies.json` pin matches) | — |
| `clang-18` / `ld.lld-18` (native-app glue) | ✅ | — |
| Meson ≥ 1.10.1 | ✅ | — |
| Ninja (1.12.1) | ✅ | — |
| Python ≥ 3.12, PyYAML | ✅ | — |
| bison / flex | ✅ | — |
| SDK-wrapper Clang/LLD **21.1.8** (`x86_64-sie-ps5`, Mesa/PSBC) | ❌ LLVM 18 | overlay adds clang-21; wrapper wiring TBD on first build |
| Python **Mako**, **packaging** | ❌ | overlay: `python3-mako` + `python3-packaging` |
| `glslangValidator`, SPIR-V Tools | ❌ | overlay: `glslang-tools` + `spirv-tools` |

So the real gap is small: clang-21 + 4 packages.

**Opt-in overlay:** `Dockerfile.ps5-opengl` + `docker-compose.ps5-opengl.yml`,
**not** wired into the default image (keeps the base pinned and small):

```sh
docker compose -f docker-compose.yml -f docker-compose.ps5-opengl.yml build ps5-dev
docker compose -f docker-compose.yml -f docker-compose.ps5-opengl.yml \
  run --rm ps5-dev ./scripts/build-ps5-opengl.sh
```

`scripts/build-ps5-opengl.sh --check` runs the preflight and prints exactly
what is missing.

**Build result (2026-09-09):** the overlay + `build-ps5-opengl.sh` produced a
complete `ps5-opengl-core33` SDK from source and it passed
`verify-installed-sdk.sh` (344 Core exports, Make/pkg-config/CMake link). The
`prospero-clang` wrapper already resolves to clang-21 once clang-21 is on the
image — no base rebuild needed. Notes:

- `Dockerfile.ps5-opengl` needs `USER root` for the apt/pip layer, then drops
  back to `dev`.
- The base image's `meson` is **1.3.2** (pip); Mesa 26.2 needs ≥ 1.4.0, so the
  overlay pins `meson==1.10.1`. (The `--check` version test was also fixed — it
  compared backwards.)
- `make sdk` builds ps5-opengl's own patched `src/` (the `_Exit` patch);
  `source-fetch` only hash-checks the *external* pinned deps, so the patch does
  not trip it.

---

## 6. What landed on this branch

| File | Purpose |
|---|---|
| `.gitmodules`, `.gitignore` | `third_party/ps5-opengl` submodule; ignore only its `build/` |
| `patches/ps5-opengl/0001-recoverable-fail.patch` | `_Exit` ×4 → `ps5gl_fatal` + the weak `ps5gl_fatal.h` |
| `scripts/build-ps5-opengl.sh` | SDK build driver (`--check`, `--frozen-archive`) |
| `Dockerfile.ps5-opengl`, `docker-compose.ps5-opengl.yml` | opt-in toolchain overlay |
| `projects/evoplayer/pp/src/pp_gl_smoke.c` + `pp/include/pp_gl_smoke.h` | EGL/GL 3.3 clear + triangle + `glReadPixels` probe |
| `projects/evoplayer/pp/src/pp_gl_fatal.c` | strong `ps5gl_fatal` + `__wrap__Exit` + no-op `_ZTH*_mesa_glapi_tls_*` |
| `projects/evoplayer/main.c` | pre-unjail `#if EVO_GL_SMOKE` boot hook (runs the probe, then parks) |
| `projects/evoplayer/Makefile` | `GL_SMOKE=1` adds the two objects + public cflags |
| `scripts/package-app.sh` | `--gl-smoke`: defs, SDK `.mk` cflags/libs, archive group, `-u`, `--wrap=_Exit`, sceAgc-import harvest |

`build-evoplayer.sh` and a plain `package-app.sh --ffpfsc` are unchanged
(verified: host compile check green, GL objects excluded).

**`--gl-smoke` link (2026-09-09): builds a signed `.ffpfsc` end-to-end.**
Getting there needed three fixes beyond the plan, all in `package-app.sh`:

1. Parse `PS5_OPENGL_LDLIBS` by `make`-evaluating the `.mk`, not text-scraping
   (the SPDX line `GPL-3.0-or-later` scraped as `-later`).
2. `libPS5OpenGLCore33.a` is a GNU ld `GROUP(...)` script; `-L<sdk>/lib
   -lPS5OpenGLCore33` inside the existing `--start-group` pulls the ~17 real
   archives.
3. ps5-opengl's Gallium driver links **36** `sce*` symbols directly (harvested
   from the archives with `llvm-nm`); the two not already in EVO's PRX stubs
   (`sceAgcCbReleaseMem`, `sceAgcDcbSetNumInstances`) are auto-added to the
   `libSceAgc` stub. **Risk for the receipt run:** these are 2 new positional
   sceAgc imports — if either NID is absent on 12.70 the module won't load
   (CE-108255-1). Both are standard command-buffer API and EVO already imports
   16 sceAgc NIDs fine.

Mesa's emulated-TLS glapi context emits a weak `_ZTH23_mesa_glapi_tls_Context`
init helper the app-module converter rejects as an unstubbed undef; a no-op
strong definition in `pp_gl_fatal.c` satisfies it (emutls does the real init).

Linked PIE 135 MB; `eboot.bin` 59 MB, `self --inspect` **integrity: valid**;
`PPSA99039.ffpfsc` 29.6 MB. Symbols confirmed present in the PIE:
`pp_gl_smoke_run`, `ps5gl_fatal`, `eglGetDisplay`, `ps5_egl_*`,
`ps5_agc_gate2_run`.

---

## 7. Hardware receipt — ✅ GO (2026-09-09)

Console `192.168.0.13`, FW 12.70. `--gl-smoke` eboot deployed via
`deploy-app.sh --ffpfsc`, launched from the Games row. Receipt captured on
**klog** (`tools/klog.sh` — klogsrv had to be started on the console; the USB
stick was reconnected mid-session so `/mnt/usb0/evo.log` did not flush, klog is
the record):

```
EVO boot: BUILD 578d5273-dirty_0909-1449
EVO boot: main() entry
EVO boot: GL-1 smoke
GL-1 SMOKE: result=PASS stage=7 dead=0 px=189E8C want=189E8C
           vendor="PS5 homebrew" renderer="PS5 AGC"
           gl="3.3 (Core Profile) Mesa 26.2.0" glsl="3.30"
EVO boot: GL-1 smoke done rc=0
```

- **PASS through stage 7** (final present loop). `dead=0` — no
  `ps5gl_fatal` / `_Exit` path taken.
- `px=189E8C == want` — `glReadPixels` of the cleared framebuffer returned the
  exact clear colour: GL genuinely rendered, not a no-op.
- **Mesa 26.2 / GL 3.3 Core / GLSL 330 on the PS5 AGC backend, on 12.70.** The
  runtime PSSL compile (the trivial fragment shader) worked — this is the wall
  `#69`/`#70` were stuck against, cleared.
- No allocator failure (`malloc_shim` handled it — `app_heap.c` not linked, §3).
- TV showed black by the time it was observed: the probe clears + draws + swaps
  3 frames then `eglTerminate`s (blanks the VO) and parks. The pixel readback
  before teardown is the render proof.

**VERDICT: GO.** ps5-opengl renders on FW 12.70. GL-2 (#78) is unblocked; the
overhaul proceeds.

### 7a. G55 re-verification — ✅ (GL-4 #80 Stage 1, 2026-09-10)

Console `192.168.0.13`, FW 12.70, `--gl-smoke` `.ffpfsc` (BUILD
`6f4ce974-dirty_0909-2012`), klog:

```
GL bench: 1080p RGBA8      1920x1080 RGBA8      mean=65.53ms p95=65.73ms  min=65.00ms
GL bench: 1080p RGBA8 PBO  1920x1080 RGBA8 PBO  mean=65.74ms p95=66.23ms  min=65.40ms
GL bench: 1080p luma R8    1920x1080 R8         mean=0.10ms  p95=0.11ms   min=0.10ms
GL bench: 1080p chroma RG8  960x540  RG8        mean=0.03ms  p95=0.03ms   min=0.03ms
GL bench: 4K RGBA8         3840x2160 RGBA8      mean=252.79ms p95=254.24ms
GL bench: 4K luma R8       3840x2160 R8         mean=0.90ms  p95=0.96ms
GL bench: 4K chroma RG8    1920x1080 RG8        mean=0.46ms  p95=0.50ms
GL-1 SMOKE: result=PASS stage=7 dead=0 px=189E8C want=189E8C
           renderer="PS5 AGC" gl="3.3 (Core Profile) Mesa 26.2.0" glsl="3.30"
EVO boot: jailbreak: ... sandbox=OPEN (/data errno=0)
```

- **GL-1 PASS on G55** — no regression from the bump. `sandbox=OPEN` this run
  (the G47 receipt's `errno=2` park-loop unjail gap didn't recur).
- **The staging copy is RGBA8-only.** R8 / RG8 uploads are effectively free
  → NV12 two-plane upload = 0.13 ms/frame (1080p) / 1.36 ms/frame (4K). GL-4
  needs **no** zero-copy import. See
  [gl4-video-path-plan.md](gl4-video-path-plan.md).
- PBO (`GL_MAP_UNSYNCHRONIZED_BIT`) makes no difference — the copy is inside
  `glTexSubImage2D`, past the map.

Follow-up nits (not blockers):
- klogsrv on this console drops the TCP connection every ~5 s; `klog.sh`
  reconnects and still caught the trace.
- The post-probe `evo_jailbreak_self()` in the park path logged
  `sandbox=closed (/data errno=2)` — it didn't fully open, so `evo.log` never
  flushed. The normal boot loops `evo_jailbreak_ensure()`; the smoke park loop
  doesn't. Harmless for a diagnostic build; klog is the channel that matters.

- **GO** → GL-2 (#78) unblocks; the overhaul proceeds.
- **NO-GO** → record the failing stage + `evo.log` excerpt above; the overhaul
  is abandoned per `opengl-render-overhaul.md` and the `render-overhaul` issues
  (#78–#82) are closed.
