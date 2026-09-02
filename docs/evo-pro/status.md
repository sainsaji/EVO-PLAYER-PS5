# EVO Pro — status & next actions

> **Purpose:** point an AI (or yourself) here when console access is available.
> It says exactly what to run, what each result means, and where to go next.
> Last updated **2026-09-02**. Branch: **`refactor/main-c-media-modules`**.

---

## 30-second picture

EVO Player's RmlUi UI was ~11 fps and 4K playback is CPU-bound. The fix is a
3-step GPU ladder ([gpu-rendering-plan.md](gpu-rendering-plan.md)):

| Step | | State |
|---|---|---|
| **1** | Cache the RmlUi menu surface, re-raster only on change (CPU) | ✅ **shipped + hardware-verified** — idle menus 11 → ~55–60 fps |
| **2** | GPU YUV→RGB convert + present for the video path (`sceAgc`) | 🔒 designed, blocked on a gate + on task 8 |
| **3** | Full RmlUi geometry on the GPU | ⏸ deferred (low value after Step 1) |

Step 2 needs `sceAgc`, which only works from a **registered app module**, not
an ELF payload (proven this session from both ends). So all GPU/decode work now
targets EVO running as app module **`PPSA99039`**. That already boots to the
menu (Phase 1b tasks 1–7 done), but **task 8 — picking a media file crashes it**
— is unresolved, and Step 2 is the video path, so task 8 gates it.

**Two questions the next console session answers, in one launch.**

---

## Do this first (needs console)

Console at the IP in `.env` (`PS5_HOST`). Jailbreak re-run after any reboot;
`websrv` 8080, `ftpsrv` 2121 up. If a payload got wedged earlier, re-run the
jailbreak to clear the elfldr host (see "Recovery" below).

```bash
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --agc-probe --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc'
```

Then **on the console**: ShadowMountPlus → mount `PPSA99039` → launch from the
**Games row**. Do **not** stack launches — PS button to close before any
rebuild ([tooling.md](../tooling.md#packaging-two-routes)).

Watch the TV for **system-notification popups** (the only channel out of the
sandbox — `/mnt/usb0` is ENOENT inside it). Also capture klog:

```bash
docker compose run --rm ps5-dev ./tools/klog.sh
```

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

## Remaining host work (optional, not blocking)

1. `pp/src/pp_agc.c` scaffold — ~80% of Step 2, can be written + compile-checked
   before the gate result is known.
2. Optimise `render_triangles_accumulated` in `evo_rmlui_render.cpp` — the
   profile's dominant cost; helps held-scroll (which Step 1 didn't touch).
   Verify with `tools/prof_rmlui.sh`.
3. Hand-write + assemble the RGBA-passthrough PS — proves the shader toolchain.
4. Fix `projects/app_ctl/` (broken `sysctl` form — use `mib[4]={1,14,8,0}`).
5. `tools/bench_rmlui.cpp` — stale, references dead symbols; align to
   `evo_rmlui_prof.h` or delete.
6. Clean the loose test media at the repo root (`bbb_au.bin`, `rabbit.mp4`,
   `test.264`, `test.mp4`, `test_au.bin`) + root-level `moonlight_stream.cpp` /
   `native_agc_present.cpp`.

---

## Key docs

- [gpu-rendering-plan.md](gpu-rendering-plan.md) — the 3-step ladder, Step 1
  profile + result
- [agc-implementation.md](agc-implementation.md) — Step 2/3 how-to
- [phase-1b-app-module.md](phase-1b-app-module.md) — app-module bring-up, §8 =
  task 8 risk table + procedure
- [videodec2-abi.md](videodec2-abi.md) — native decode ABI (Phase 3, later)
- [../tooling.md](../tooling.md#packaging-two-routes) — the two packaging routes
