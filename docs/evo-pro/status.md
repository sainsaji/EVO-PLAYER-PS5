# EVO Pro — status & next actions

> **Purpose:** point an AI (or yourself) here when console access is available.
> It says exactly what to run, what each result means, and where to go next.
> Last updated **2026-09-02** (evening). Branch: **`refactor/main-c-media-modules`**.

## 2026-09-02 evening — hardware session results

- **Self-unjail CONFIRMED on 12.70** — PS5-Lapy-JB-Daemon file-drop works
  (`daemon_saw=yes sandbox=OPEN`). One promotion opens `/mnt/usb0` **and**
  `/data`. Lapy must be launched via HBL before EVO each session.
- **Task 8 — 1080p MKV PLAYS.** Root cause was `posix_fadvise()` in
  `evo_stream_io.c` faulting (SIGSYS-class) from the `PPSA99039` sandbox —
  crashed on file-open between `P8_01c` and `P8_01d`. Fixed: both call sites
  `#ifndef EVO_APP_MODULE` (commit `55685aa0`). Full `P8_*` chain now completes
  through `009_FIRST_FRAME_ENTER`. `mmap live=98M peak=103M` → R1 heap is a
  non-issue at 1080p.
- **4K (GTA VI H.264) — decoded garbage then crashed.** FFmpeg spammed
  `get_buffer() failed` / `thread_get_buffer() failed` → missing reference
  pictures → corrupt frames → crash. This is the **frame pool running out of
  memory**: `malloc_shim.c` was backing every allocation with plain anonymous
  `mmap()`, which on PS5 is serviced from a small (~few-hundred-MB) system pool
  — fine at 1080p (~100 MB) but a 4K frame pool (400 MB+) exhausts it.
  **Fix (staged, not yet hardware-tested):** `malloc_shim.c` now maps through
  `sceKernelMapNamedFlexibleMemory` (full title budget, GBs) with plain `mmap`
  as fallback. Added `evo_alloc_map_info()` (map-fail count, last-fail size,
  `sceKernelAvailableFlexibleMemorySize`) — surfaced in `P8_31_RETURN_OK` and
  appended to the first buffer/alloc `P8_AVLOG` line. `evo_av_log_cb` narrowed
  back to `AV_LOG_ERROR` and de-duplicates repeated lines (the screenshots were
  40+ identical popups).
- **Build-hygiene bug that cost 3 sessions** — `make objects` reused stale
  *payload* `.o` files (the evoplayer Makefile tracks sources, not `CFLAGS`),
  so deployed eboots had **zero** app-module code. Plus a leftover
  `/data/homebrew/PPSA99039/` folder shadowed the `.ffpfsc`
  ("Duplicate PPSA99039 ignored"). Both fixed: `package-app.sh` writes
  `app-cflags.stamp` and force-cleans on mismatch; `deploy-app.sh --ffpfsc`
  deletes the folder first; `main()` fires `EVO boot: BUILD <sha>_<MMDD-HHMM>`
  first so a stale mount is caught in one glance.
- **`--agc-probe` now links.** `evo_agc_probe.c` was rewritten to drop the
  payload-only kernel-R/W symbols: it computes Sony NIDs inline via OpenSSL
  `SHA1()` (libcrypto is already linked) and resolves through
  `sceKernelLoadStartModule` + `sceKernelDlsym` (tries NID string then plain
  name). NID encoder verified byte-for-byte against the SDK's `prospero-nid`
  for 10 symbols. **The AGC gate answer is one launch away.**

---

## Checklist

**Next console session** (~5 min of your time):
- [ ] Console online — `nc -vz $PS5_HOST 2121` responds
- [ ] Add `/data/homebrew/lapy_jb/lapy_jb_daemon.elf` to `/data/autoload.txt` (once)
- [ ] `deploy-app.sh --ffpfsc` → auto-mounts
- [ ] Launch EVO from the Games row
- [ ] Relay the notifications:
  - [ ] `EVO boot: jailbreak: promoted …` → self-unjail works (USB + internal)
  - [ ] `EVO agc: … VIABLE` / `NOT AVAILABLE` → **Step 2 gate**
  - [ ] pick a 1080p file → last `P8_*` / `P8_AVLOG` before the crash → **task 8**
  - [ ] then a 4K file

**Then, in order** (GitHub issues):
- [ ] **Task 8** (#26) — fix the call the breadcrumb named; verify A/V sync + 4K vs `main`
- [ ] **Step 2** (#27) — `pp/src/pp_agc.{h,c}` + `agc_blobs.S` (port `native_agc_present.cpp`); wire `pp_videoout.c` / `pp_playback.c`; settings toggle Auto/CPU/GPU; device-test a test-pattern NV12; plane-hash A/B vs CPU
- [ ] **Step 2.5** (#27) — `rgba_ps` + reused header → `sceAgcCreateShader` accepts? → GPU OSD composite
- [ ] **Step 3** (#28) — solid/UI-VS/scissored shaders (`build-shader.sh`); `ui_rml/src/evo_rmlui_render_agc.cpp` (RenderInterface→AGC); `evo_rmlui_app.cpp` picks it when `pp_agc_available()`; fold the UI pass into `pp_agc` `render_frame`; **delete `evo_rmlui_render.cpp`'s CPU rasteriser** after plane-hash parity

**Optional / housekeeping:**
- [ ] GLSL→SPIR-V→RDNA2 toolchain in the container (Dockerfile) — nicer shader path ([agc-implementation.md](agc-implementation.md) §7)
- [ ] Rebuild `projects/sandbox_unjail/` as a resident daemon (for non-Lapy setups)
- [ ] `projects/app_ctl` — test `list` / `kill` on hardware

---

## 30-second picture

EVO Player's RmlUi UI was ~11 fps and 4K playback is CPU-bound. The fix is a
3-step GPU ladder ([gpu-rendering-plan.md](gpu-rendering-plan.md)):

| Step | | State |
|---|---|---|
| **1** | Cache the RmlUi menu surface, re-raster only on change (CPU) | ✅ **shipped + hardware-verified** — idle menus 11 → ~55–60 fps. Stays as the AGC-unavailable / host fallback. |
| **2** | GPU YUV→RGB convert + present for the **video path** (`sceAgc`) | 🔒 designed, blocked on a gate + on task 8 |
| **3** | **Complete UI rendering on the GPU** — bind `Rml::RenderInterface` to AGC draw calls; delete the CPU rasteriser | 🔒 committed scope ("full attempt"), sequenced after Step 2 |

**The destination is one GPU path** — UI triangles + video convert + composite
+ flip all on `sceAgc`, `evo_rmlui_render.cpp`'s ~2000-line CPU coverage
rasteriser gone, held-scroll smooth, 4K UI native, UI-over-video correct by
construction. Step 2 first because it is the minimum pipeline that de-risks
everything Step 3 needs (shader toolchain, `CreateShader`/`LinkShaders`, DCB
submit, VideoOut integration, panic discipline) using ProsperoLight's
**hardware-proven** fullscreen-quad path. Step 3 reuses all that plumbing and
swaps in arbitrary triangle geometry + a hand-written shader set — the
primitive is proven structurally by SharpProspero's `Renderer3D.DrawMesh` but
not yet on device, which is exactly why Step 2 goes first.

> **We have full GPU access.** `sceAgc` from the app module = render target on
> the VideoOut buffer, viewport, scissor, arbitrary vertex/index buffers,
> constant buffers, shader linkage, textured+blended draws, flip. ProsperoLight
> (fullscreen, hardware-proven) + SharpProspero (arbitrary geometry, clean-room
> ABI) document it call-by-call. Missing: the PSSL compiler (→ `llvm-mc-18`
> assembles GCN) and Gnmx helpers (→ SharpProspero rebuilt the register model).
> **ProsperoLight draws its own RmlUi UI on the CPU** (`SDL_CreateSoftwareRenderer`)
> — it is a Step 1+2 reference, not Step 3.

`sceAgc` only works from a **registered app module**, not an ELF payload
(proven this session from both ends). So all GPU/decode work targets EVO as app
module **`PPSA99039`**. That already boots to the menu (Phase 1b tasks 1–7
done), but **task 8 — picking a media file crashes it** — is unresolved, and
Step 2 is the video path, so task 8 gates it.

**Two questions the next console session answers, in one launch.**

---

## Do this first (needs console)

**Build staged locally (2026-09-02):** `output/app/PPSA99039.ffpfsc` (22 MB) +
`output/app/PPSA99039/`. Rebuild if the tree changed since:

```bash
docker compose run --rm ps5-dev bash ./scripts/package-app.sh --agc-probe --ffpfsc
```

### What's in this build

The `.ffpfsc` = the `output/app/PPSA99039/` folder wrapped in an inner exFAT
image then PFS-compressed (ProsperoLight's exact format — `mkpfs inspect` only
sees `PPSA99039.exfat`; the files are inside it). Contents:

| file | what |
|---|---|
| `eboot.bin` (34 MB) | the signed EVO Player, built with the flags below |
| `sce_module/libc.prx` (1.3 MB) | clean-room runtime shim (byte-pinned) |
| `sce_sys/param.json` | `PPSA99039`, game category, `downloadDataSize>0` |
| `sce_sys/icon0.png` | app icon |
| `assets/` | RmlUi `.rml`/`.rcss`, icons, fonts, `renderer_reset_assets.h` |

`eboot.bin` compile-time flags (set by `package-app.sh --agc-probe`):

- **`EVO_APP_MODULE=1`** — `/download0/evoplayer` data paths, `getdents`
  directory enum, **self-unjail** (`evo_jailbreak.c`, Lapy/etaHEN file-drop —
  covers `/mnt/usb0` + `/data`), **`P8_*` playback breadcrumbs**,
  `av_log`→notify, `pp_stage_bc`→notify.
- **`EVO_AGC_PROBE=1`** — boot-time `sceAgc` reachability recon
  (`evo_agc_probe.c` → `EVO agc:` notification).
- **`EVO_BOOT_TRACE=1`** — `evo_bt` init breadcrumbs.
- Always in the app build: `malloc_shim` (mmap allocator, R1), `libc_ext`
  (locale/setjmp stubs), the RmlUi **Step 1 surface cache**.

**Not in the ffpfsc** (separate artifacts): `pp/shaders/rgba_ps.bin` (not
wired to the build yet), `agc_probe` / `sandbox_unjail` (elfldr payloads).

When the console is up (powered, jailbroken — `nc -vz $PS5_HOST 2121` responds):

```bash
docker compose run --rm ps5-dev bash ./scripts/deploy-app.sh --ffpfsc
```

The user's **ShadowMountPlus auto-mounts on folder/image change** — the deploy
triggers the mount. **Launch is still manual**: on the console, launch EVO from
the **Games row**. Do **not** stack launches — PS button to close before any
rebuild.

> **Confirm the fresh build is running.** `main()`'s first popup is
> `EVO boot: BUILD <git-sha> <MMDD-HHMM>`. If you don't see it, or the sha is
> wrong, ShadowMount+ is serving a stale/cached mount — force a rescan or
> reboot. (2026-09-02: a leftover `/data/homebrew/PPSA99039/` folder shadowed a
> new `.ffpfsc` for a whole session — ShadowMount+ prints "Duplicate PPSA99039
> ignored". `deploy-app.sh --ffpfsc` now deletes the folder first.)

USB + internal browse: EVO **self-unjails** — drops `{"PID":"<pid>"}` to
`/download0/etahen_jailbreak` for **PS5-Lapy-JB-Daemon**, once at boot
(`evo_jailbreak_self`, short) and again on every browser entry
(`evo_jailbreak_ensure`, two harder tries). Reports
`jailbreak: pid=N file=… daemon_saw=yes/NO sandbox=OPEN/closed (/data errno=…)`.
One promotion opens the real root → `/mnt/usb0` + `/data` both resolve.
Launch Lapy via HBL first (or add to `/data/autoload.txt`). Daemon absent /
fails → `tools/sandbox-unjail.sh` per launch. Lapy README says fw 3.00→12.00;
**12.70 unconfirmed** — the `daemon_saw` / `sandbox` fields say which part failed.

The `EVO agc:` / `P8_*` answers come out as **system-notification popups** —
someone has to read the TV (`sceKernelDebugOutText` does **not** reach klog from
the app-module sandbox; `/mnt/usb0` is ENOENT so screenshots can't save either).

### Read the notifications

| Notification | Meaning | → next |
|---|---|---|
| `EVO agc: … Step 2 VIABLE` | `libSceAgc.sprx` loads + NIDs resolve in `PPSA99039` | **Step 2 is unblocked** — see A below |
| `EVO agc: … NOT AVAILABLE` / `partial` | AGC unreachable even from the app module | Step 2 needs raw GNM or is dead — reassess with [agc-implementation.md](agc-implementation.md) |
| `EVO bc:` / `EVO P8: P8_00 … P8_31` sequence | playback breadcrumbs; the **last one before the crash names the failing call** | fix that call — see B below |
| `P8_AVLOG …` right before the crash | FFmpeg's own error text for the failure | that's the direct cause |
| `boot ok - frame loop` and menu renders, but crash on file-select | expected — task 8 | proceed to B |

Pick a **1080p file first** (`rabbit.mp4` or `test.mp4` at the repo root, or copy
one to the console). Then repeat with a **4K file**.

---

## A — if the AGC gate passes: build Step 2

The design is fully worked out in **[agc-implementation.md](agc-implementation.md)**
(read it — `native_agc_present.cpp` annotated, shader blobs disassembled, the
port plan). Summary:

1. Write `pp/src/{pp_agc.h, pp_agc.c, agc_blobs.S}` — a mechanical strip of
   `third_party/ProsperoLight/src/native_agc_present.cpp`: the AGC ABI
   (runtime-resolved NIDs, like `projects/evoplayer/src/evo_agc_probe.c`), the
   `shader_memory` layout, `initialize_presenter` + `render_frame`, minus the
   HUD / keyboard / LAN telemetry. `.incbin` the 5 blobs from
   `third_party/ProsperoLight/assets/private/`. `#ifdef __PROSPERO__` — a stub
   on host.
2. Scope: **video path only.** `pp/src/pp_videoout.c` + `pp/src/pp_playback.c`
   route the decoder's NV12 frame straight to `pp_agc_present_nv12()` instead of
   `pp_converter_*` + swizzle + `sceVideoOutSubmitFlip`. Menus keep Step 1's CPU
   memcpy. OSD composite stays CPU (small, only redraws on state change) until a
   hand-written RGBA passthrough PS exists.
3. Settings toggle `Playback → Renderer: Auto / CPU / GPU`.
4. First device test: `pp_agc_init` + one `render_frame` of a test-pattern NV12
   buffer → a picture on the TV via the GPU. Then real frames, then plane-hash
   A/B vs the CPU path.

`llvm-mc-18` in the container assembles GCN (`--mcpu=gfx1030`), so the RGBA PS
and the Step 3 shaders are hand-writable — see agc-implementation.md §1/§5.

### Then Step 3 — complete UI on the GPU (committed scope)

Once Step 2's pipeline runs on device:

1. Hand-write + assemble the shader set: RGBA passthrough (textured quad),
   solid-colour triangle, textured+vertex-colour-modulated triangle, all with
   scissor. Each needs a Sony header — start from ProsperoLight's
   `pixel.header.bin` / `geometry.header.bin` and adapt (agc-implementation.md
   §1/§5).
2. New `ui_rml/src/evo_rmlui_render_agc.cpp` — an `Rml::RenderInterface` that
   emits AGC draw calls into a per-frame DCB instead of CPU-rasterising:
   `CompileGeometry` → a GPU vertex/index buffer; `RenderGeometry` → bind
   shader + textures + `DrawIndexAuto`; `RenderToClipMask` / `EnableScissorRegion`
   → scissor rects or a stencil; `SetTransform` → the MVP constant buffer.
3. `evo_rmlui_app.cpp` picks the AGC interface when `pp_agc_available()`, else
   the existing CPU `EvoRenderInterface` (Step 1). One `#ifdef`-free runtime
   switch.
4. The menu/video/OSD composites become draws in the same DCB — `pp_agc.c`'s
   `render_frame` gains a "UI pass" between the video quad and the flip.
5. Delete the CPU coverage rasteriser from `evo_rmlui_render.cpp` once parity
   holds (plane-hash A/B every screen).

## B — fixing task 8 (the playback crash)

The `P8_*` breadcrumb that fires last tells you which call. Likely suspects
([phase-1b-app-module.md](phase-1b-app-module.md) §8 risk table):

- **R1 — heap.** `malloc_shim.c` is mmap-backed and covers `posix_memalign`, so
  FFmpeg allocs *should* be fine; the `P8_31` line reports mmap high-water — if
  it's near a ceiling, that's it. Fix: raise the proc-param heap, or route more
  through `evo_direct_mem`.
- **R3 — libm.** Downgraded: host check shows `libm.a` supplies real impls and
  `package-app.sh` links it. Unlikely; confirm via `P8_AVLOG` / audio A/B.
- **`exit()` SIGSYS** — a known teardown-path signal; may or may not be the
  file-select crash.
- **Sandbox limits** — thread count (decoder frame threads + convert pool +
  audio), or `sceAudioOutOpen` behaving differently in the app-module sandbox.
- **Mid-run cred swap** — if `sandbox-unjail` was run, that's a new vector.

Once playback is stable in `PPSA99039`, re-check A/V sync and 4K against `main`
([validation.md](../validation.md)), then Step 2 layers on top.

---

## Recovery (if something wedges)

- **Wedged elfldr payload** (stuck in a syscall): re-run the jailbreak to
  restart the elfldr host. Console stays otherwise healthy — check with
  `curl http://$PS5_HOST:8080/fs/` (kernel-R/W route; 200 = exploit still live).
  A kernel panic takes *all* network services down.
- **Wedged app slot**: PS button → close. No remote kill exists for it.
- **Every new probe payload must carry a watchdog thread** that `_exit()`s on a
  hang — see `projects/agc_probe/main.c`. Prevents the above.
- Consider adding `ps5-payload-shsrv` to the jailbreak chain for remote
  `ps`/`kill`.

---

## What's done (host, this session — all committed)

| commit | |
|---|---|
| `b3d00ac` | Step 1 — RmlUi surface cache. Hardware-verified. |
| `768cb3e` | `agc_probe` — elfldr AGC recon (dead end). |
| `9c7b3fa` | app-module task-8 diagnostics (`P8_*` breadcrumbs, `av_log`→notify, mmap high-water) + `--agc-probe` + `--ffpfsc` packaging. |
| `2a582e2` | docs — ELF track abandoned, app module is the release path. |
| `ca471ad` / `5b12844` | tooling.md + CLAUDE.md — two packaging routes. |
| `5e39f2d` | **[agc-implementation.md](agc-implementation.md)** — Step 2/3 how-to, blobs disassembled, port plan. |

Builds verified: `build-evoplayer.sh` (payload) and `package-app.sh --agc-probe`
(app module) both compile clean.

## Host work done since (commits after `73cf154`)

- **Shader toolchain proven** — `tools/build-shader.sh` (`llvm-mc-18` +
  `llvm-objcopy-18`): hand-written GCN `.s` → raw `.text` blob for
  `sceAgcCreateShader`. `pp/shaders/rgba_ps.s` (RGBA × vertex colour, first
  shader Step 2 + Step 3 both need) assembles + round-trips. **Not run on
  hardware.** The `.header.bin` half is still open — plan: reuse ProsperoLight's.
- **GLSL→ISA path scoped** — container has `llc-18`/`clang-18` with the amdgcn
  target but no `glslang`/`spirv-tools`; a nicer GLSL→SPIR-V→RDNA2 route needs a
  Dockerfile change. Documented in [agc-implementation.md](agc-implementation.md) §7.

## Remaining host work (not blocking — can start before the console session)

Toward Step 2/3:

1. **`pp/src/pp_agc.c` scaffold** — ~80% of Step 2, written + compile-checked
   before the gate result is known. Mechanical strip of `native_agc_present.cpp`.
2. **Solid-colour PS + 2D-ortho UI VS** — next shaders (`pp/shaders/`), same
   `build-shader.sh` workflow.
3. Sketch `ui_rml/src/evo_rmlui_render_agc.cpp` — the `Rml::RenderInterface`→AGC
   mapping (Step 3 §2 in [agc-implementation.md](agc-implementation.md)).

Housekeeping:

4. Fix `projects/app_ctl/` (broken `sysctl` form — use `mib[4]={1,14,8,0}`,
   `KERN_PROC_PROC`, namelen 4). Gives a `list / kill <pid>` payload.

> Optimising the CPU rasteriser (`render_triangles_accumulated`) is **not**
> worth it — Step 3 deletes it. Only touch it if Step 3 slips badly and
> held-scroll needs a stopgap.

---

## Key docs

- [gpu-rendering-plan.md](gpu-rendering-plan.md) — the 3-step ladder, Step 1
  profile + result
- [agc-implementation.md](agc-implementation.md) — Step 2/3 how-to
- [phase-1b-app-module.md](phase-1b-app-module.md) — app-module bring-up, §8 =
  task 8 risk table + procedure
- [videodec2-abi.md](videodec2-abi.md) — native decode ABI (Phase 3, later)
- [../tooling.md](../tooling.md#packaging-two-routes) — the two packaging routes
