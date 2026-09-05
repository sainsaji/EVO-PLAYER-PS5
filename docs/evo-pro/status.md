# EVO Pro — status & next actions

> **Purpose:** point an AI (or yourself) here when console access is available.
> It says exactly what to run, what each result means, and where to go next.
> Last updated **2026-09-04**. Branch: **`feat/28-gpu-ui`** (off `refactor/main-c-media-modules`).
>
> ## #28 (GPU Step 3 — RmlUi/OSD on sceAgc) — IN PROGRESS on `feat/28-gpu-ui`
>
> **Shader question settled (4 `--agc-probe` runs, 2026-09-04):** `ui_vs` +
> `solid_ps` create + link (triangle list) clean — the **solid-colour GPU
> geometry path is hand-writable**. **Every textured pixel shader fails
> `0x8a6c001f`** (`image_sample` and typed `buffer_load_format`, hand-written and
> spliced-onto-the-reference-trailer alike) — `sceAgcCreateShader` validates code
> against the header's `sl00` resource-metadata whenever memory is touched, and
> ProsperoLight ships headers only for its NV12/P010 shaders. Textured UI (text,
> icons, art) as GPU geometry needs a GLSL→SPIR-V→AMD-ISA compiler (Dockerfile).
>
> **Research payoff:** ProsperoLight renders its HUD with **zero custom shaders**
> — CPU-raster → NV12 → 2nd quad in the same DCB (constant-alpha blend `0x1e0 =
> 0x40001413`), reusing the NV12 shader. This is the basis for Phases 1–3.
>
> | Phase | State | Gate |
> |---|---|---|
> | 0 — shader set + `build-shader.sh --all` + `agc_ui_blobs.S` | landed | — |
> | 1 — overlay quad in `agc_render_frame` (ProsperoLight `draw_overlay`) | **hw-verified** — test bar over GTA 4K, `render_frame rc=0`, 196-word DCB, ~1 ms, no wedge | `/mnt/usb0/evo_agc_test_overlay` |
> | 2 — player OSD composited over 4K (`pp_agc_osd`, `pp_agc_present_nv12_overlay`) | **HW-VERIFIED, WORKS.** YUV-space premultiplied blend (~1-2 ms/frame), double-buffered publish (no flicker), held-frame snapshot keeps OSD+frame on screen through a seek (`agc_present_held_osd`), 1 px overlay inset (no edge line). "Buttery smooth" over GTA 4K. RGB↔YUV coeffs fixed (were ~0.4% dark). #32's 1080-drop still used for the *committed*-seek settle. | `/mnt/usb0/evo_agc_osd` |
> | 3 — GPU menu present (`pp_agc_present_ui`, linear 1080 VO) | **HW-VERIFIED but SHELVED — net negative.** Renders fine, no artifacts, but adds a full-frame BGRA→NV12 convert (~4 ms) to an already-CPU-bound menu → held-scroll 30→16 fps. Doesn't touch the real bottleneck (RmlUi raster). Code stays hook-gated + off; keep for a future Phase-4 linear-menu-VO need. | `/mnt/usb0/evo_agc_ui` (leave unset) |
> | 4 — held-scroll: GPU *geometry* (`evo_rmlui_render_agc.cpp`) | **SOLIDS PATH HW-VERIFIED WORKING 2026-09-04** (`feat/28-gpu-ui`, committed). The GPU draws the RmlUi solid stream (boxes, gradients, rounded-corner backgrounds, borders) through SharpProspero's `sp_mesh_{vs,ps}`: `agc_geo_init` (CreateShader + LinkShaders(4) = 0x0 on HW) + `agc_render_geo` (DCB from `Graphics/Renderer3D.cs` DrawMesh — RT block byte-identical to `agc_render_frame`, AgcViewport, blend `0x1e0=0x65010501`, ortho MVP, persisted 34-reg link, `AgcBufferDescriptor` Constant/Structured → VS user data, one `DrawIndex(sub-range addr)` + inline-`SetCxRegisterDirect` scissor per batch) + watchdog'd `agc_geo_worker` + per-VO-buf vtx/idx slab. `EvoAgcGeoSink` (`evo_rmlui_render_agc.{h,cpp}`, NOT an Rml::RenderInterface — RmlUi 6 can't swap) fed by `EvoRenderInterface::SetAgcSink` diverting untextured/untransformed/unclipped batches. `EvoRmlApp::AgcGeo{Active,Present}` + bridge + `main.c`. On HW: correct layout + rounded corners, `agc_render_geo rc=0x0` every frame, watchdog never trips, navigable. **Colour fixed** from the real shader source (`Shaders/mesh_{vs,ps}.pssl`): `mesh_vs` unpacks colour as ARGB (fed RGBA → R/B swap); `mesh_ps` does `colour*(0.25+0.75·N·L)` → vertex normal set to `(-0.4,1,0.6)` so N·L=1 = passthrough. **Text/icons still CPU, NOT yet composited over the GPU solids** — the additive-NV12 2nd-pass attempt crashed EVO at boot every time and was reverted; needs offline debug. | `/mnt/usb0/evo_agc_ui` |
>
> **#28 Phase 4 text-pass — BLOCKED at the toolchain level (bisected on HW 2026-09-04).**
> Compiling the additive-NV12 2nd pass into `agc_render_geo` crashes EVO at
> *load*, before any log — **even though the code is unreachable** without
> `/mnt/usb0/evo_agc_geo_text`. Bisected:
> - 16-arg `pp_agc_present_geo` + `g_geo_submit` fields + all plumbing, pass
>   body `#if 0` → **boots**.
> - pass body compiled in (`#if 1`), never executed → **crashes at load**.
>   Even just its CPU-setup half (fopen + `*(void**)(g_agc.vs+24)` header reads
>   + `memcpy` + the `NADD` viewport block) → **crashes**.
> => not logic (path unreachable). native_app_builder ELF→eboot conversion, or
> `-ffunction-sections`/`-fdata-sections`, or an `agc_render_geo` size threshold.
> `396a8ef`: `PP_AGC_GEO_TEXT` defaults 0 — pass compiled out, default build =
> the hw-verified solids path. **Next (not line-bisecting):** (a) move the pass
> to its own `pp_agc_geo_text.c`; if a separate-TU fn compiled-but-uncalled also
> bricks load → `readelf -l` diff the eboot between a booting vs crashing build,
> check segment count/sizes vs any native_app_builder limit. (b) try
> `-DPP_AGC_GEO_TEXT=1` with `-fno-function-sections -fno-data-sections` on
> pp_agc.o only. Archived builds for A/B in `output/app/archive/`.
>
> **✅ 2026-09-04 evening — `feat/28-gpu-ui` UN-BOOTABLE bug FIXED (#71 closed, `df7cbf2`).**
> Every build off `93ce6ec` had SIGSEGV'd at LOAD (CE-108255-1, "before KStuff
> pause", no log). `tools/klog.sh` capture during a launch gave the fault:
> `_start → _init → _GLOBAL__I_000100` (libc++ `<iostream>`) `→ ios_base::Init::Init()
> → std::locale::locale() → read [null+0x48] → signal 11`. libc++'s `<iostream>`
> emits a priority-100 static init that builds the global locale; in the native-app
> CRT's `_init()` `__init_array` walk it derefs null. **Layout-sensitive** —
> `9f1a88a` survived by luck; `93ce6ec`'s bigger `.text` shifted addresses and it
> faulted every launch. Bisect ruled out PRX imports (`9f1a88a` + `93ce6ec`'s full
> 17-sym `libSceAgc.syms` booted) and the toolchain (3wk stale). Fix: dropped
> `<iostream>` from `evo_rmlui_app.cpp` + `evo_rmlui_system.cpp` (debug prints →
> `fprintf(stderr,…)`; the sandbox has no stdout). **Rule: never `#include
> <iostream>` in the app module.**
>
> Also fixed en route (`1cd070e`): `sceAgcDcbDrawIndexOffset` dead PRX import +
> `package-app.sh` now FAILS on any `.syms` symbol no object imports.
>
> **GPU geometry path HW-VERIFIED (`aa5996b`, `evo_agc_ui`):** `agc_render_geo
> rc=0x0`, 38 batches, no wedge. Renders the RmlUi solid stream (hero/tiles/rows/
> rail/pills) with the #68 radii. **Solids-only** without the text pass.
>
> **#67 (text over GPU solids) — 2nd-pass approach WORKS on HW (`--geo-text` /
> `PP_AGC_GEO_TEXT=1` / `evo_agc_geo_text`).** `Gt text pass`, DCB 663→723 words,
> `rc=0x0`. Settings screen shows text+icons+pills over the GPU solids. **Bug:
> colours wash out** — the pass blends additively (NV12 has no alpha), correct
> only if `m_surface` is pure black off the glyphs; some solids leak into
> `m_surface` AND draw on GPU → double-exposed. Next: audit `RenderCachedScreen`
> geo path + `RenderGeometry` sink rules (clipped solids → sink w/ GPU scissor;
> `m_surface` = text/icons-on-transparent only).
>
> **#68 first pass committed (`aa5996b`):** RCSS corner radii (12 stylesheets) +
> `EvoAgcGeoSink::Add` folds RmlUi transforms into vertex positions (focus
> `scale()` stays on the geo path). MSAA → **#69**, SDF AA/shadow shaders → **#70**.
>
> **`EVO vdec native: Decode FAIL rc=0x811d0303`** seen 2026-09-04 — resident 4K
> decoder not coming up this session. Separate from UI; `play <4K H.264>` check
> needed next session.
>
> **A `--agc-probe` deploy on 2026-09-04 ended in a kernel panic** (deploy exit 1
> then KP — likely the OSD build left the app slot dirty + ShadowMount
> auto-relaunch stacking). Every #28 GPU path is now behind its own
> `/mnt/usb0/evo_agc_*` hook — the default `--agc-probe` build presents the
> proven #27 video-only path. **Boot the previous build first to confirm console
> health before deploying a new one.**
>
> **RESUME #28 →** console session: `evo-remote.sh build --agc-probe`, boot the
> OLD image once (health check), PS-close, deploy new, then per phase drop the
> hook file (`ftp STOR /mnt/usb0/evo_agc_osd` etc.), play a 4K H.264, check
> `pp_agc_osd: compose avg=…us`, `012C_AGC_OSD_OVERLAY`, no wedge. Full plan:
> `~/.claude/plans/optimized-honking-wilkinson.md`.
>
> ---
>
> **#27 (GPU Step 2) — ✅ CLOSED 2026-09-04, PR #61 merged** to
> `refactor/main-c-media-modules`. GTA 4K plays through the sceAgc GPU present
> path: decode → NV12 → GPU YUV→RGB → GPU flip, correct colour, `fatal=0`, CPU
> swizzle off the 4K hot path, **982 µs/frame (3% of the 30 fps budget)**. On
> AGC fault/wedge the VO re-registers tiled and the CPU path resumes. `--agc-probe`
> gated, default build unchanged. #55 fixed alongside. The four fixes that got
> there: watchdog worker thread (B), V8-gate reachability, linear UHD VO attr (A),
> retile-on-AGC-death. Full write-up in the 2026-09-04 sections below.
>
> **RESUME HERE → the #27 leftovers, then #28 (GPU Step 3).**
> - **#62** — plane-hash A/B parity: AGC present vs the CPU converter, pixel-level.
>   The A/B is built in (default `.ffpfsc` = CPU, `--agc-probe` = AGC). Needs a
>   plane-dump hook + `tools/bench.sh`.
> - **#37** — the `Renderer: Auto/CPU/GPU` settings row (after #37's decoder-row append).
> - **#41** — P010 / HEVC Main10 present (the `hdr=1` shader is wired; needs a
>   P010 `bind_pixel_source` + the 2nd resident decoder feeding it).
> - **#28** — RmlUi on sceAgc + GPU OSD-over-4K composite. Unblocked; reuses all
>   of #27's plumbing (shader setup, DCB submit, VideoOut, panic behaviour).
> Open bugs from #31: #32 (scrub blanks player UI on the V8 4K path — fix landed
> via a 1080 scrub overlay, hw-verify pending), #33 (harness cleanup).
> Console `192.168.0.6` (ethernet, `.env`).
> Test loop: `tools/evo-remote.sh build --agc-probe` -> launch from Games row
> -> `evo-remote.sh boot` -> `evo-remote.sh play <4K H.264>`.

## 2026-09-03 (evening) — remote-close investigation + console state

The console is **powered on and reachable** (`192.168.0.6`: FTP 2121, websrv
8080, elfldr 9021). **EVO is running on it** — the pre-`#27` build
(`03c0d6b6-dirty_0903-0322`), idle on the launch screen (`evo_status` `t=`
advancing ~1/s, `scr=0 active=0`). The deployed image is
`/data/homebrew/PPSA99039.ffpfsc` (Sep 3 03:47, pre-render_frame).

**Can a running EVO app be closed remotely?** Researched — practically, **no**,
not without an ELF payload:

| Route | Verdict |
|---|---|
| `sceSystemServiceLaunchApp` / `...KillApp` from elfldr context | Launch returns `0x80940005`; Kill likely the same class. That's why `evo-remote.sh` says "launch from the Games row". |
| etaHEN IPC `KILL_APP_CMD` (opcode 4, `0xDEADBEEF` struct → `127.0.0.1:9028`) | The clean route — **but bound to localhost** (needs an on-console process) **and 9028 is refused on this console** (etaHEN's IPC isn't running; the JB here is the Lapy daemon + a manual autoload chain). |
| `projects/app_ctl/` `kill` | Raw `kill(pid, SIGKILL)` on the `eboot.bin` process via kernel-R/W creds. Would probably work, **but it is an ELF payload** (pushed via `/hbldr` or elfldr) — the user asked not to use ELF payloads for this — and it's untested on hardware, and a bare SIGKILL may leave the shell's app-slot state inconsistent. `launch` afterwards is still blocked (`0x80940005`). |
| **PS Remote Play** (mobile / PC) | The only no-payload route: on-screen PS button → Control Center → close game, and it can also navigate the Games row to relaunch. Needs Remote Play registered + PSN on the console (JB consoles are usually offline → typically unavailable). |
| ShadowMount+ auto-launch | Its `debug.log` shows it **does** `[GAME] started: PPSA99039` after mounting a *new* image. So a headless `deploy-app.sh --ffpfsc` can relaunch EVO with no controller — **observed on first registration; re-launch on an updated image unconfirmed.** |

**Conclusion / rule:** do **not** deploy a new `.ffpfsc` over a running EVO
(ShadowMount may unmount/remount a resident title or stack a launch → kernel
panic → all network services down → dead until someone is physically there;
see the 50-minute-loss precedent). Wait for the user at the console: PS button
to close → `evo-remote.sh build --agc-probe` (deploy) → launch from the Games
row (or let ShadowMount auto-launch) → `evo-remote.sh boot` / `play`.

## 2026-09-03 (later still) — #27 render_frame ported + wired (untested on HW)

`agc_render_frame` is a near-verbatim strip of ProsperoLight's `render_frame`
(SDR / NV12, no HUD/overlay) in `pp/src/pp_agc.c`. The present path is wired:

- **`pp_agc_present_nv12(vout_handle, buf_idx, gpu_target, nv12, pitch, coded_h,
  vis_w, vis_h, out_w, out_h, marker)`** — reshaped to take EVO's *own*
  `pp_videoout` handle + an already-acquired GPU plane. **Never
  `sceVideoOutOpen`.** The DCB owns its `sceAgcDcbSetFlip`.
- **NV12 staging.** The decoder hands an NV12 copy in *flexible* memory
  (`malloc` → `sceKernelMapNamedFlexibleMemory`, `PROT_RW` only — **not
  GPU-samplable**). `pp_agc_present_nv12` stages each frame into a per-VO-buffer
  direct-memory scratch (`prot 0x33`, `PP_VO_MAX_BUFFERS` slots keyed by
  `buf_idx` so `pp_videoout_acquire`'s retire guarantee covers slot reuse).
  ~12 MB × 3 at 4K, lazy-allocated, every rc checked.
- **`pp_videoout_adopt_flip`** — marks the VO buffer in-flight for a flip the
  GPU DCB queued itself (no `SubmitFlip`), so `retire_old_inflight` frees it
  when `flipArg` reaches the marker. `main.c`'s V8 `present_pre_tiled` is *not*
  called for AGC frames (they set no `pending_present`).
- **`evo_vdec_native.c`** — `ro_harvest` emits straight NV12 (skips the
  NV12→I420 de-interleave) when `pp_agc_available()`; `pp_frame` gained
  `coded_height`. `pp_playback.c` de-interleaves NV12→YUV420P as a fallback if
  AGC is unavailable / faults.
- **Fault guard** — the *first* `agc_render_frame` runs under a
  SIGSEGV/BUS/ILL `sigsetjmp` (evo_agc_probe.c pattern); a fault logs to
  `evo_boot.log`, sets `pp_agc` unavailable, playback drops to the CPU
  converter. (It does **not** catch a GPU-side hang — see the failure note
  above.) `main()` used to call `pp_agc_init` unconditionally; **PR #54 gated
  that behind `--agc-probe`** after the hardware hang. Default build = CPU path.

**Open hardware unknowns (first run answers these):**
1. Does AGC render into EVO's VO buffer? It's registered
   `SetBufferAttribute2(0x8000000022000000, tiling=0)`; ProsperoLight registers
   its *own* pool with `0x8000000000000000`. The `render_frame` CX
   render-target bits (`cx[2]/cx[4]/cx[15]`, tiling/format) are tuned to
   ProsperoLight's buffer. Mismatch = garbled/swapped, not a crash. Fix:
   register the VO buffer with ProsperoLight's attr when AGC is on, or adjust cx.
2. `sceAgcDcbSetFlip` against a handle whose buffers `pp_videoout` (not AGC)
   registered.
3. Colour: `0xAABBGGRR` framebuffer order vs the shader's MRT0 export.
4. ~~`sceAgcDriverSubmitDcb` latency on the playback push thread~~ — **plan B
   landed (`feat/27-agc-submit-watchdog`):** `render_frame` runs on a dedicated
   worker, caller `pthread_cond_timedwait`s 250 ms; a wedge → `-2` → `pp_agc`
   dead + CPU path, no app-slot freeze. Unknown #1's attr fix (plan A) also
   landed — VO registers `0x8000000000000000` under `pp_agc_available()`.
5. Type-12 direct-memory headroom for 0xD0000 + ~36 MB staging alongside the
   resident decoder.

**Not done this pass:** settings row `Playback → Renderer: Auto/CPU/GPU`;
GPU OSD composite over 4K video (AGC frames present with no overlay — deferred,
agc-implementation.md §4); P010/HDR present; plane-hash A/B vs CPU.

## 2026-09-03 (later still) — render_frame present path FAILS on hardware

First hardware run of `pp_agc_present_nv12` (via #6's branch, the first `.ffpfsc`
deployed that contains `90c890b`): **GTA VI 4K hangs then crashes** right after
`006B_VO_RECONFIG_APPLIED` / the first valid 4K decode. The decoder emits NV12,
`pp_playback`'s V8 branch calls `pp_agc_present_nv12`, and the GPU submit
(`sceAgcDriverSubmitDcb` → `sceAgcSuspendPoint`) never returns; a second fault
then `_exit`s. Matches open unknown #1 (RT format/tiling) and/or #4 (submit).

**Mitigation shipped (PR #54):** `main.c`'s unconditional `pp_agc_init()` is now
gated behind `--agc-probe` (`#if defined(EVO_APP_MODULE) && defined(EVO_AGC_PROBE)`).
The default `.ffpfsc` uses the CPU V8 converter (YUV420P, #31-proven); GTA 4K
plays again. `--agc-probe` still arms the GPU path for #27 work via
`evo_agc_probe()`. Next #27 step: register the VO buffer with ProsperoLight's
`0x80..00` attr (or adjust the `cx` RT bits) before the next `--agc-probe` run.

## 2026-09-04 — #27 AGC present path WORKS on hardware

`tools/evo-remote.sh` loop, `--agc-probe`, GTA 4K H.264 (`GTAVI_An_Extended_Look.mp4`):

```
pp_agc: READY - GPU present path armed (render_frame ported, submit watchdog live, #27)
pp_agc: A0 present enter  buf=0 pitch=3840 codedh=2160 vis=3840x2160 out=3840x2160
pp_agc: A1 staging ok need=12441600 -> memcpy + flush     (12 MB x3 direct-mem scratch)
pp_agc: W0 worker picked up frame -> agc_render_frame
pp_agc: R1 cx base done (31 regs) -> WaitUntilSafeForRendering
pp_agc: R2 WaitUntilSafeForRendering returned
pp_agc: R4 DCB built (156 words) -> SubmitDcb
pp_agc: R5 SubmitDcb=0x0 -> SuspendPoint
pp_agc: R6 SuspendPoint done, result=0x0
pp_agc: render_frame rc=0x0 words=156
012_AGC_FIRST_PRESENT agc gpu flip
EVO vdec native: decodes=538 framesout=522 fatal=0     (then seek, clean CLOSE)
```

Colour correct, motion smooth, no crash, no wedge. **The CPU `pp_converter_*` +
swizzle + `sceVideoOutSubmitFlip` are off the 4K path.** The three fixes that got
here (B watchdog / reachability / A linear attr) are in the resume block above.

### Iteration log (what each hardware run showed)

| Run | Build | Result |
|---|---|---|
| 1 | `70e32029` A+B | "weird colors" + app-slot hang. Neither the sigsetjmp guard nor the watchdog fired. |
| 2 | `a78996b2` + instrumentation, plan A backed out | UI colour correct (tiled VO), still hung on playback — **no `A0`/`R0` breadcrumbs at all**. |
| 3 | `f138be4e` V8-gate reachability fix | GPU present works! `render_frame rc=0`, `012_AGC_FIRST_PRESENT`, plays — but **R↔B swapped** (blue "avatars") against the tiled VO. |
| 4 | `3ef157d` re-apply plan A (linear UHD VO) | **colour correct, no crash.** ✅ |

Root cause of runs 1-2: `agc_path = use_v8 && format==NV12`, but `use_v8`'s
`pp_v8_frame_gate` rejects NV12 → `use_v8=0` → NV12 4K de-interleaved to YUV420P
→ **V3/1080 fallback path → overflow at 3840x2160 (#55)**. The AGC branch was
dead code. Run 1's "weird colors" was the linear-registered VO scanned out while
the CPU menu drew tiled into it — misattributed to "plan A hangs the compositor".

### 2026-09-04 (later) — fallback recovery + #55 + timing, all hardware-verified

- **GPU present timing** (`--agc-probe`, GTA 4K): `pp_agc: heartbeat presented=900
  dropped=1 avg=982us max=1205us`. The convert+flip uses **3% of the 33 ms 30 fps
  budget** — huge headroom. (~1 ms is mostly the 12 MB NV12 `memcpy` into the
  GPU-visible staging + clflush; decode-into-staging could remove it later.)
- **AGC-death recovery.** A plain linear CPU write into the `0x8000000000000000`
  plane *also* garbles (it's a GPU tile layout `render_frame` produces, not
  CPU-linear — no CPU converter matches it). So on AGC fault/wedge, EVO now
  **re-registers the VO with the proven tiled attr** and the normal CPU
  converter takes over. Verified with the `/mnt/usb0/evo_agc_no_present` hook
  (simulated wedge at frame 121):
  ```
  012_AGC_FIRST_PRESENT → pp_agc: TEST WEDGE after 121 frames
  011_AGC_SUBMIT_WEDGED → 011_AGC_VO_RETILE_REQ
  005B_VO_RECONFIG_QUEUED req 3840x2160 chg=0 → 013_AGC_VO_RETILE
  006B_VO_RECONFIG_APPLIED → 012_FIRST_FRAME_PRESENTED v8 pre_tiled
  ```
  ~306 ms transition (one dropped frame, last AGC frame held), then playback
  continues on the CPU tiled path — user reported "plays without any issue".
- **#55** folded in: `pp_product_reconfigure_vo` takes `open_gate` so the decode
  gate opens only after `pp_playback_set_output` resizes `pb->display`;
  `pb->display_cap` tracks the alloc and the V3 branch reallocs to `out_w*out_h*4`
  regardless of `pb->backend`.

### Remaining for #27

- Plane-hash A/B: AGC output vs the CPU converter, pixel-level (`tools/bench.sh`,
  `docs/validation.md`). Visual + math parity look right; not automated.
- Settings `Renderer: Auto/CPU/GPU` row — land #37's config-append first.
- P010 / 4K60 HEVC Main10 present (#41 wants this shader).
- GPU OSD-over-4K composite (needs the RGBA passthrough PS; #28 plumbing).
- Optional: decode straight into GPU-visible memory to drop the ~1 ms staging copy.

## 2026-09-03 (night) — #27 plan A+B landed (`feat/27-agc-submit-watchdog`)

The issue's "Plan to finish" comment, B → A in one PR, to make the `--agc-probe`
loop safe to iterate. **No hardware run yet** — code only, all three builds
green (host `build-evoplayer.sh`, `package-app.sh --agc-probe`, bare
`package-app.sh`, `uiview.sh --all`).

- **B — watchdog'd worker (`pp/src/pp_agc.c`).** New `agc_submit_worker` thread
  (started at the end of `pp_agc_init`) owns the *entire* `agc_render_frame`
  call — `WaitUntilSafeForRendering` + `SubmitDcb` + `SuspendPoint` — plus the
  first-frame `sigsetjmp` guard (moved off the caller, since that thread is the
  one that faults). `pp_agc_present_nv12` stages the NV12 (unchanged), publishes
  a single-slot mailbox, then `pthread_cond_timedwait`s **250 ms** (CLOCK_REALTIME
  deadline + a CLOCK_MONOTONIC re-check so a wrong clock basis only re-waits).
  On timeout: `g_agc.submit_wedged = 1`, `g_agc.ready = 0`, worker abandoned
  (never joined — it's stuck in the syscall), return **`-2`**.
- **`pp_playback.c` V8 `agc_path`**: `-2` → `pp_videoout_adopt_flip` (not
  release — the abandoned worker may still queue that flip), checkpoint
  `011_AGC_SUBMIT_WEDGED`, CPU path for every frame after. `-1` unchanged.
- **A — linear VO attr (`pp/src/pp_videoout.c`).** `pp_videoout_init` picks
  `PP_VO_ATTR_SDR_LINEAR` (`0x8000000000000000`, new in `pp_platform.h`) over
  `PP_VO_ATTR_TILED_BGRA` when `pp_agc_available()` && `EVO_APP_MODULE`. Auto-
  scoped to `--agc-probe` builds (default `.ffpfsc` never calls `pp_agc_init`).
  Known limit: if AGC dies mid-session the CPU fallback tiler writes tiled into
  a linear-registered buffer → garbled until restart (logged loudly on `-2`).

**Next: `tools/evo-remote.sh build --agc-probe` on hardware.** Expect
`pp_agc: render_frame rc=…` in `evo_boot.log` with the app still alive; then
drive the picture per §8 below.

## 2026-09-03 (later) — #27 shader setup VALIDATED on hardware

`pp_agc_init` (the first half of ProsperoLight's `initialize_presenter`, ported
to `pp/src/pp_agc.c`) runs clean from the app module:
```
pp_agc: sceAgcInit=0x8a6c0004 (already-init, ok)   # 2nd call after the gate; benign
pp_agc: create=0x00000000 link=0x00000000 vs=0x223fac000 ps=0x223fad000  1920x1080 hdr=0
```
`sceAgcCreateShader` ×2 → 0, `sceAgcLinkShaders` → 0, with the **vendored
ProsperoLight blobs** (`pp/blobs/*.bin`, 6 files, `.incbin` via
`pp/src/agc_blobs.S`). So: sceAgcInit, the 0xD0000 shader-scratch alloc/map,
`copy_asset` of the blobs to their fixed offsets, `prepare_resources` (NGR1
rebase + SDR full-range coeffs), CreateShader, LinkShaders — **all work**.

### Next — port `render_frame` (agc-implementation.md §3) + wire  — ✅ DONE, see the "evening" section above

- **`agc_render_frame`** into `pp_agc.c`: the per-frame DCB — CX register block
  (from `sceAgcGetRegisterDefaults`, 16 `target_offsets` entries patched with
  the RT base / dims / blend, + the `ADD_REG` viewport/scissor/guardband set),
  `geometry_cb`/`pixel_cb` constant buffers, `sceAgcDriverWaitUntilSafeForRendering`,
  link-register concat from the shader objects (`*(void**)(shader+24)` cx,
  `+32` sh, `shader[91]`/`[92]` counts), `bind_pixel_source` (30-word NV12
  descriptor at SH `0x0c`), `sceAgcDcbDrawIndexAuto(4, 2)`, `sceAgcDcbSetFlip`,
  `flush_gpu_data(mem, 0x10000)`, `sceAgcDriverSubmitDcb` + `sceAgcSuspendPoint`.
- **New `.syms`**: `sceAgcDcbSetCxRegistersIndirect`,
  `sceAgcDcbSetUcRegistersIndirect`, `sceAgcSuspendPoint` (libSceAgc);
  `sceAgcDriverGetWaitRenderingPacketSizeInDwords` (libSceAgcDriver).
- **`pp_agc_present_nv12(vout_handle, buf_idx, gpu_target, nv12, pitch,
  coded_h, vis_w, vis_h, out_w, out_h, marker)`** — takes EVO's acquired VO
  buffer as params; **reuses `pp_videoout`'s handle**, never `sceVideoOutOpen`
  (two opens is bad; also the `sceVideoOutOpen`→compute-queue panic vector, and
  #31's resident decoder already holds a compute queue). Does its own
  `SetFlip` + submit, so main.c's V8 present path must not also flip that frame.
- **Feed it NV12 directly.** #31's `ro_harvest` de-interleaves NV12→I420 for
  the CPU converters; the AGC path wants NV12, so `evo_vdec_native` should hand
  `SceVideodec2OutputInfo.buffer` straight through when `pp_agc_available()` —
  no CPU touch of the pixels at all. FFmpeg frames still need the CPU path.
- **`pp_playback.c`** `use_v8` branch: when `pp_agc_available()`, skip
  `pp_compute_pipeline_convert` / `pp_converter_yuv420p_to_tiled_bgra_parallel`
  and call `pp_agc_present_nv12`.
- Settings row `Playback -> Renderer: Auto/CPU/GPU` — coordinate with **#37**'s
  `Video decoder` row (same screen, same config-append pattern).
- Panic discipline: AGC present up after VO, down before VO reconfig; watchdog
  the submit; `flush_gpu_data` before every submit.

## 2026-09-03 (later) — #27 AGC gate PASSED

`EVO agc: init=0x00000000 defaults=0x8005b4180 -> Step 2 VIABLE` from the app
module. `sceAgcInit` returns 0 and `sceAgcGetRegisterDefaults()` a valid table
pointer, via the **positional PRX import stubs** (`libSceAgc` +
`libSceAgcDriver`, already `DT_NEEDED` for `MODE == player` since #31). No
`sceKernelLoadStartModule`, no NID/dlsym — `evo_agc_probe.c` rewritten to
`extern` + call. `evo_boot_log` (new, `src/evo_boot_log.{c,h}`) buffers
pre-unjail probe output and flushes it to `/mnt/usb0/evo_boot.log` — pulled
over FTP, no screenshots. **#27 Step 2 port (`pp/src/pp_agc.{h,c}` +
`agc_blobs.S`) is unblocked.**

## 2026-09-03 (later) — PHASE 4 (#31) DONE: native 4K decode plays in EVO 🎉

`media/src/evo_vdec_native.c` — the `sceVideodec2` backend behind `evo_vdec.h`.
**On hardware (PPSA99039):** GTA VI trailer (3840×2160 H.264, the clip that
crashed the FFmpeg frame pool) **plays real-time on native decode** — `be=1`
NATIVE, `pos` climbs 1.0×, `fatal=0`, colours correct, no judder. BBB 1080p and
Clarksons 720p too.

**How it works:**
- **Resident decoder.** Every `libSceVideodec2` call fails `0x811D0111` once
  `evo_jailbreak_self()` has run, so the compute queue + a 4K AVC-High decoder
  (`max_dpb_frames = -1` auto, `pipeline_depth = 4` — SharpProspero's prod
  config) are brought up **once at boot** in `evo_vdec_native_probe()`, before
  the unjail, and held for the session. Each `evo_vdec_native_open()` just
  `sceVideodec2Reset`s it. HEVC / >4K fall back to FFmpeg.
- **mp4 → Annex-B** via the `h264_mp4toannexb` bitstream filter.
- **NV12 → planar I420** de-interleave in `ro_harvest` — EVO's fast/parallel/4K
  converters only accept `PP_FRAME_YUV420P`; NV12 at 4K silently draws black.
- **`pp_playback.c` fix:** the V8 4K path was still mallocing 2×33 MB of
  `pb->display` buffers it never uses; that alloc now fails under the resident
  decoder's memory pressure and left `out_w` at 1920 → `use_v8` off → black.
  Skip the alloc for V8 (matches `pp_playback_on_file_open`).
- Dispatch stays in `evo_vdec_ffmpeg.c`; PRX stubs unconditional for
  `MODE == player`; `tools/vdec-test.sh` + `package-app.sh --autoplay` drive
  the unattended hardware-test loop (FTP log pull, one manual launch press).

**#46 — app module persists nothing. ✅ CLOSED 2026-09-03, HARDWARE-VERIFIED**
(theme + favorite + resume → PS-close → relaunch → all restored). Was: settings
/ recent / favorites / resume reset every launch because `/download0/evoplayer/`
isn't durable for a fake-signed ShadowMount title. Fix landed on
`refactor/main-c-media-modules` (`788b1a0`):
`evo_data_dir()`/`evo_data_path()` (`src/evo_data_path.c`) now resolve the root
at **runtime** — `/data/evoplayer` once `evo_jailbreak_is_open()`, the
`/download0` literal only as a pre-unjail fallback (uncached, self-heals).
`evo_jailbreak_is_open()` made public. Every compile-time site
(`RECENT_FILE_DB`, `FAVORITES_FILE_DB`, `EMBY_CONF_PATH`,
`PROSPERO_LAST_FOLDER_FILE`, `PROSPERO_SETTINGS_FILE`, `RESUME_FILE` — was a
bare `/data/ps5_media_resume.txt`) routed through `evo_data_path()`.
`evo_mkdir()` shim uses `sceKernelMkdir` on the app module (libc `mkdir` not in
the clean-room `libc.prx` surface). Late-unjail race handled by
`evo_persistence_rebind()` (browser entry re-binds + reloads if the boot
`attempt(12)` lost the race — chose this over hardening the boot attempt).
One-shot migration copies `/download0/evoplayer/*` and the old bare resume file
into `/data/evoplayer/` on first bind. Follow-up sub-issues still open: **#50**
(host tests for the new data-root logic + parsers), **#51** (quiet the boot
breadcrumbs — keep klog + `evo_boot.log`, drop the popups).

**Asset shadowing (found during the #44 pass, fixed `edc3a08`):** the app reads
`.rml`/`.rcss`/fonts from `/data/evoplayer/app/assets/` (the only asset path
reachable in the sandbox — `/app0` was never confirmed to resolve). That dir is
**not in the `.ffpfsc`** and `deploy-app.sh` never refreshed it, so it served
stale payload-era UI assets for weeks and shadowed every #16 RCSS clamp on
hardware. `deploy-app.sh --ffpfsc` now wipes + re-pushes the packaged asset
tree there on every deploy.

**#32 — scrub blanks player UI on the V8 4K path (high). FIX LANDED (`a0cc708` +
`5d305ab`), HW-VERIFY PENDING.** Not a freeze — seek works, app responsive. The
bug: the `pp_product_k4_live` V8 4K present path never composites the OSD and
`v8_hold` skips the draw + flip during the seek-discard window.

Fix: run the interactive scrub under the 1080 overlay VO (like Media Info),
where `draw_player_screen` composites the OSD. **The trap:** `pp_playback`'s
convert (`pp_converter_to_display`, `pp_playback.c:555`) runs UNLOCKED on the
decode thread while the 1080↔4K VO reconfigure frees `pb->display`;
`present_pp_frame`'s `g_vo_decode_gate` check is check-then-act →
use-after-free. Frame pacing hid it; the first attempt at the discard speedup
(`b24f5c1`, reverted) made it crash every seek.

Real fix — overlay reworked as a render-loop **state machine**
(`prospero_scrub_ovl_state` NONE/ENTERING/ACTIVE/LEAVING +
`prospero_scrub_overlay_pump()`, `main.c`) that only calls
`pp_product_overlay_enter/leave` while the new `volatile int
video_decode_parked` (`evo_playback.c`, =1 when the decode thread is in its
idle branch, i.e. not converting) is set. A committed seek: pump waits for
`g_pp_pb.seek_discarding` to clear, re-pauses, waits parked, flips the VO,
restores `player_paused`. Then `5d305ab` re-lands the flat-out discard
(safe now the reconfigure is off the decode thread).

**Chapter jump dropped from the overlay** (fire-and-forget seek → pre-#32
held-frame behaviour). `SQUARE → Media Info` blocked while
`prospero_scrub_overlay_engaged()`. Video behind the scrub bar is black during
the overlay; a held 4K frame there needs the #27 GPU composite. Residual: the
seek is still GOP-bound on long-GOP 4K (flat-out only shaves ~1.5-2×).
Separately still open: verify the `AVSEEK_FLAG_ANY` byte-seek fallback
(`fe6c6ed`) lands GTA's 14 GB file on a keyframe.

## 2026-09-03 (late) — NATIVE HW DECODE WORKS IN EVO 🎉

`EVO vdec2: HARDWARE DECODE OK (Route B viable)` from the **full 43 MB EVO
Player** (PPSA99039), which then booted on to the menu. All calls `0`,
`out valid=1 err=0 pics=1 1920x1088 pitch=2048 codec=1`. `sceVideodec2`
(Route B). Probe: `projects/evoplayer/src/evo_videodec2_probe.c`
(`package-app.sh --videodec2-probe`).

**Two things had to be true (found over ~14 launches):**

1. **PRX import stubs linked POSITIONALLY** — `libSceVideodec2` + `libSceAgc` +
   `libSceAgcDriver` as unconditional `DT_NEEDED` (`package-app.sh` step 6b,
   `tools/native-app/stubs/prx/*.syms`). `--as-needed` is not enough. AGC is
   needed so libSceVideodec2's own GPU imports resolve. **This also unblocks
   #27** — switch `evo_agc_probe` from `sceKernelLoadStartModule` to the stub.
2. **Decode init must run BEFORE `evo_jailbreak_self()`.** The self-unjail's
   mid-run credential swap makes `sceSysmoduleLoadModule(207)` return
   `ESDKVERSION` and `libSceVideodec2` never finishes loading → first call
   SIGSEGVs. `main()` now runs `evo_videodec2_probe()` first (commit `fea7d1c`).

**Route A (`sceAvPlayer`) is dead** — sysmodule `0xA5` refused even pre-unjail.

### Phase 4: `media/src/evo_vdec_native.c` — CODE LANDED, needs hardware

Implemented on `refactor/main-c-media-modules` (#31):
- **`media/src/evo_vdec_native.c`** — ports `evo_videodec2_probe.c`'s
  bring-up / decode / teardown behind the `evo_vdec.h` seam. mp4/mkv AUs go
  through the `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream filter first
  (sceVideodec2 wants Annex-B). NV12 pictures are copied out of the frame pool
  into a small PTS-sorted reorder window, cropped to display size, handed back
  as `pp_frame`. HEVC Main10 / >8-bit falls back to FFmpeg (no `PP_FRAME_P010`
  yet). Guarded by `EVO_APP_MODULE`; host + payload get stubs.
- **Dispatcher** stays in `evo_vdec_ffmpeg.c` — `evo_vdec_open()` tries native
  when `p->backend == NATIVE`, falls back to FFmpeg on any failure (never
  returns NULL if FFmpeg could open). `evo_vdec_probe()` → `main()` before
  `evo_jailbreak_self()`. Hidden AUTO: `main.c` requests NATIVE when the probe
  armed (the real Auto/FFmpeg/Native row is Phase 5).
- **`package-app.sh`** — the `libSceVideodec2`+`libSceAgc`+`libSceAgcDriver`
  positional PRX stubs are now unconditional for `MODE == player`.
- **`evo_playback.c`** — sends one NULL AU at EOF so the decoder drains its
  buffered tail (helps both backends; `evo_vdec_receive` now treats
  `AVERROR_EOF` as "no frame", not fatal).

**First hardware run must answer (instrument `note()` output):**
1. **Frame order** — does `sceVideodec2Decode` emit pictures in DISPLAY or
   DECODE order? The backend assumes display order + min-PTS pairing. If
   B-frame content stutters / A-V drifts, rebuild with
   `-DEVO_VDEC_NATIVE_DECODE_ORDER=1` and a deeper `-DEVO_VDEC_REORDER_DEPTH`.
2. Does `CreateDecoder` succeed when playback starts (i.e. *after* a later
   `evo_jailbreak_ensure()` on browser entry), given the module was loaded
   pre-unjail? If not → hold a decoder from boot, or reload the module.
3. `evo_pb_active_backend()` should report NATIVE; a native open failure must
   fall back to FFmpeg with no crash.
4. 4K clip that exhausts the FFmpeg frame pool (GTA VI trailer) — plays on
   native?
- `--videodec2-probe` build stays as the regression check.

---

## 2026-09-03 — Route A gate ran on hardware: BLOCKED (shared with #27)

`EVO avplayer: libSceAvPlayer.sprx load FAILED - Route A blocked` (PPSA99039,
fw 12.70). Boot trace otherwise clean — `jailbreak: … sandbox=OPEN (/data
errno=0)`, `boot ok - frame loop`. `EVO agc:` still `load FAILED` too.

**Root cause = the #27 blocker, generalised.** A fake-signed app module can't
`sceKernelLoadStartModule` a system PRX it didn't declare NEEDED. This now
blocks **three** things behind one piece of work:

| needs a NEEDED import stub | for |
|---|---|
| `libSceAgc` + `libSceAgcDriver` | #27 GPU Step 2 (AGC convert/present) |
| `libSceAvPlayer` | #29 native decode Route A |
| `libSceVideodec2` | #29 native decode Route B |

The SDK ships `.so` stubs for 31 modules — **none of these three**. The
reference for building them: SharpProspero `tools/SharpProspero.Prx/`
(`PrxStubEmitter.cs`, `StubCatalog.cs`) + `prospero-nid`. Once a stub is linked,
the loader auto-loads the `.sprx` at process start and the symbols resolve as
ordinary imports — `evo_agc_probe.c` / `evo_avplayer_probe.c`'s runtime
`sceKernelDlsym` + inline-NID approach becomes unnecessary.

**Done same day — PRX import-stub mechanism built.**
`tools/native-app/stubs/prx/<module>.syms` (one symbol name per line) →
`package-app.sh` emits a tiny ELF `.so` (SONAME `<module>.sprx`), links it
`--as-needed`, and passes it to `native_app_builder link --stub`. Verified in
the build: `libSceAvPlayer.prx` NEEDED in the converted eboot,
`sceAvPlayerInit → aS66RI0gGgo#I#J` (NIDs match `prospero-nid`),
`integrity: valid`. `evo_avplayer_probe.c` rewritten to call `sceAvPlayer*`
directly (no load, no dlsym). `libSceAgc`/`libSceAgcDriver`/`libSceVideodec2`
`.syms` also staged — stubs build but aren't NEEDED until something references
them (#27 / Phase 4 flip a one-liner).

**Next:** hardware run #2 — does the loader auto-load `libSceAvPlayer.sprx` for
a fake-signed module? Deploy the `--avplayer-probe` build (`/data/probe.mp4`
already on the console), launch, read `EVO avplayer:` popups.

## 2026-09-02 (late) — host work, no console

- **#30 (Phase 3, `evo_vdec.h` seam) signed off.** Dead `ffmpeg_mkv_test()`
  inline decoder removed; cover/poster extractor documented as staying out of
  the seam (→ `evo_cover`, Track B); `evo_vdec.h` doc pass (Phase 4 slot-in +
  accessor contract). `evo_vdec_ffmpeg.c` is now the only file with
  play-stream `avcodec_*`/`sws_*`. Builds clean: `build-evoplayer.sh`,
  `package-app.sh --ffpfsc`, `uiview_playback_rml.sh`. Commit on the branch.
  Remaining: bit-exact codec-sweep parity check (hardware).
- **#29 Route A (`sceAvPlayer`) Phase 0 done + gate probe built into the app module.**
  - New header `projects/evoplayer/media/include/sce/sce_avplayer.h` from
    SharpProspero (sizes/offsets `_Static_assert`ed, C + C++ clean).
  - **`projects/evoplayer/src/evo_avplayer_probe.c`** — boot-time probe behind
    `-DEVO_AVPLAYER_PROBE` (`package-app.sh --avplayer-probe`), modelled on
    `evo_agc_probe.c`. Payloads can't reach hardware decode, so this runs
    **inside `PPSA99039`**: inline-SHA1 NID resolve, `MediaPlayer.cs` callback
    port (texture via `AllocateMainDirectMemory` 12→3), `Init → AddSource →
    EnableStream → Start → GetVideoDataEx`, frame characterisation, dump to
    `/download0/evoplayer/avpx_frame0.*`, watchdog `_exit()`s EVO on a hang.
    Output = `EVO avplayer:` notification popups. Compiled into the eboot
    (verified: symbol + strings present).
  - `projects/avplayer_test/` (payload) kept as a compile-checked callback-port
    reference only — it cannot decode.
  - Write-up + verdict table + memory-typing diff: [avplayer-abi.md](avplayer-abi.md).
  - **Staged build:** `output/app/PPSA99039.ffpfsc` includes `--avplayer-probe`
    `--agc-probe`. **Next console session:** deploy, drop a small H.264 `.mp4`
    at `/data/probe.mp4` (FTP) or `/mnt/usb0/probe.mp4`, launch from Games row,
    relay the `EVO avplayer:` popups → verdict table.

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
- **4K playback — reasonable encodes work, demanding ones can't.** Verified on
  hardware 2026-09-02:
  - **Tears of Steel 4K** (3840×1714 H.264) — plays + seeks fine.
  - **GTA VI trailer** (3840×2160 H.264, deep refs) — a few frames then crash.
  - `P8_31` on the GTA VI run: `heap live=62M peak=142M flex_maps=25958
    anon_maps=0 fail=0 flex_avail=281M`. The flexible-memory allocator works
    perfectly — the problem is the **hard ~450 MB flexible-memory budget** the
    kernel gives a fake-signed game module. A deep-ref 4K frame pool wants
    more; `sceKernelMapNamedFlexibleMemory` starts refusing and the decoder
    desyncs (POC errors) and faults.
  - **This ceiling is structural** — can't meaningfully raise it for this title
    category. Software 4K in the sandbox is permanently fragile.
  - **Landed (`bb80de1`):** `malloc_shim` → `sceKernelMapNamedFlexibleMemory`
    (+ split flex/anon map counters); UHD in the app module → slice threading
    (~6× smaller pool); `evo_playback` aborts cleanly to the finished screen
    with a toast after a fatal-decode streak instead of crashing;
    `evo_av_log_cb` → `AV_LOG_ERROR` + dedup.
  - **The real fix is native decode (#29)** — `sceVideodec2` allocates frames
    from its own direct-memory pool, not this budget.
- **AGC gate (#27) — `EVO agc: libSceAgc.sprx load FAILED - Step 2 blocked`.**
  `sceKernelLoadStartModule` can't pull a system PRX an app module didn't
  declare NEEDED. ProsperoLight/SharpProspero link a libSceAgc stub so the
  loader auto-loads it; the SDK has no such stub. Step 2's runtime-resolve
  approach is dead — #27 needs a libSceAgc stub added to the app-module link
  first. Noted on the issue.
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

**Next console session — #27 first device run** (EVO must be closed first; see
the "remote-close investigation" section — there is no clean remote close):
- [ ] Console online — `nc -vz $PS5_HOST 2121`
- [ ] PS button → close the running EVO (frees the app slot)
- [ ] `tools/evo-remote.sh build --agc-probe` (packages `--usb-remote` + deploys)
- [ ] Launch `PPSA99039` from the Games row (or let ShadowMount+ auto-launch on
      the image change)
- [ ] `tools/evo-remote.sh boot` — confirm `EVO agc: … Step 2 VIABLE` and
      `pp_agc: READY`
- [ ] `tools/evo-remote.sh play /mnt/usb0/bbb_1080p_h264.mp4` then a 4K H.264
      (`/mnt/usb0/GTAVI_An_Extended_Look.mp4`)
- [ ] `tools/evo-remote.sh boot` again — read `pp_agc: render_frame rc=…`
  - rc=0 + correct picture → Step 2 works, move to plane-hash A/B
  - garbled / channel-swapped → RT format/tiling — adjust `cx[2/4/15]` (plan A's
    linear VO attr is already applied)
  - `pp_agc: FAULT …` → first-frame guard fired, AGC disabled, CPU fallback
  - `pp_agc: SUBMIT WEDGED` / `011_AGC_SUBMIT_WEDGED` → plan-B watchdog fired
    (submit >250 ms), AGC abandoned, CPU path — app stays alive, debug offline

**Then, in order:**
- [ ] **#27 Step 2 finish** — plane-hash A/B vs the CPU converter; then remove
      `pp_converter_fused` / the CPU swizzle from the 4K hot path; P010/HDR
      present path.
- [ ] **#27 settings row** `Playback → Renderer: Auto / CPU / GPU` — coordinate
      with **#37**'s `Video decoder` row (same screen, same fscanf-append
      pattern — land decoder-append first, then renderer).
- [~] **#32** — scrub blanks player UI on the V8 4K path (not a freeze;
      k4_live never composites the OSD + `v8_hold` skips the flip) (high).
      Fix landed (`a0cc708`+`5d305ab`): interactive scrub drops to the 1080
      overlay VO via a render-loop state machine gated on `video_decode_parked`
      (the naive version raced the unlocked converter and crashed). HW-verify
      pending.
- [ ] **#37** — Phase 5: `Video decoder: Auto / FFmpeg / Native` settings
      toggle + `evo_vdec_probe()` gate + config migration.
- [ ] **#28 Step 3** — solid/UI-VS/scissored shaders (`build-shader.sh`);
      `ui_rml/src/evo_rmlui_render_agc.cpp` (RenderInterface→AGC); fold the UI
      pass into `pp_agc` `render_frame`; **delete `evo_rmlui_render.cpp`'s CPU
      rasteriser** after plane-hash parity.

**Optional / housekeeping:**
- [ ] GLSL→SPIR-V→RDNA2 toolchain in the container (Dockerfile) — nicer shader path ([agc-implementation.md](agc-implementation.md) §7)
- [ ] `projects/app_ctl` — test `list` / `kill` on hardware (the only remote
      "close EVO" lever, ELF-payload based, currently unverified)

---

## 30-second picture

EVO Player's RmlUi UI was ~11 fps and 4K playback is CPU-bound. The fix is a
3-step GPU ladder ([gpu-rendering-plan.md](gpu-rendering-plan.md)):

| Step | | State |
|---|---|---|
| **1** | Cache the RmlUi menu surface, re-raster only on change (CPU) | ✅ **shipped + hardware-verified** — idle menus 11 → ~55–60 fps. Stays as the AGC-unavailable / host fallback. |
| **2** | GPU YUV(NV12)→RGB convert + present for the **video path** (`sceAgc`) | 🟢 gate PASSED, `pp_agc_init` hw-verified, `render_frame` **ported + wired** (`pp/src/pp_agc.c`, `pp_playback` V8 branch) — builds green, **awaiting a first device run** |
| **3** | **Complete UI rendering on the GPU** — bind `Rml::RenderInterface` to AGC draw calls; delete the CPU rasteriser | 🔒 committed scope ("full attempt"), sequenced after Step 2 (#28) |

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
(proven from both ends). So all GPU/decode work targets EVO as app module
**`PPSA99039`**, which now boots to the menu **and plays** (Phase 1b done; task 8
was `posix_fadvise` from the sandbox, fixed `55685aa0`) and does native 4K
decode (#31). Step 2's video path rides on top.

---

## Do this first (needs console)

Rebuild + deploy the current tree:

```bash
tools/evo-remote.sh build --agc-probe    # package --usb-remote --ffpfsc + deploy
# — OR just the eboot check —
docker compose run --rm ps5-dev bash ./scripts/package-app.sh --agc-probe
```

### Build contents

`package-app.sh --agc-probe` → `output/app/PPSA99039.ffpfsc` (inner exFAT
image, PFS-compressed — ProsperoLight's format). `eboot.bin` +
`sce_module/libc.prx` + `sce_sys/param.json` (`PPSA99039`, game category) +
`assets/`. Compile flags: `EVO_APP_MODULE=1` (data paths, self-unjail,
`P8_*` breadcrumbs, `av_log`→notify), `EVO_AGC_PROBE=1` (`evo_agc_probe.c`
→ `EVO agc:` popup + `pp_agc_init`). `EVO_BOOT_TRACE_POPUP=1` (`--breadcrumbs`,
#51) is now off by default — klog carries the `evo_bt`/`EVO_P8` lines
unconditionally in the app module; the popup is opt-in for a TV-only session.
Always: `malloc_shim`
(flexible-mem allocator), `libc_ext`, the RmlUi Step 1 surface cache. #31's
native decode + #27's `pp_agc` are unconditional for `MODE == player`.
`tools/evo-remote.sh build` adds `--usb-remote` (the `/mnt/usb0/evo_cmd` /
`evo_status` remote) + deploys.

Deploy: `tools/evo-remote.sh build` or `scripts/deploy-app.sh --ffpfsc` (FTP →
ShadowMount+ auto-mounts on the image change; it may also auto-launch — see the
remote-close section). **Confirm the fresh build:** `main()`'s first popup is
`EVO boot: BUILD <git-sha>_<MMDD-HHMM>`. Diagnostics: `evo-remote.sh boot`
(pulls `/mnt/usb0/evo_boot.log`), `status`, `watch`. Popups also show on the TV
(`sceKernelDebugOutText` doesn't reach klog from the app sandbox).

Self-unjail: EVO drops `{"PID":…}` to `/download0/etahen_jailbreak` for
**PS5-Lapy-JB-Daemon** at boot + on browser entry — one promotion opens
`/mnt/usb0` + `/data`. Launch Lapy via HBL first (or `/data/autoload.txt`).

### First device run (#27)

**AGC gate PASSED, `pp_agc_init` hw-verified, `render_frame` ported + wired.**
What the first run answers:

| `evo_boot.log` line | Meaning |
|---|---|
| `EVO agc: … Step 2 VIABLE` + `pp_agc: create=0x0 link=0x0` + `pp_agc: READY` | init path good (already seen) |
| `pp_agc: staging …KB x3 @ …` then `pp_agc: render_frame rc=0x0 words=…` + correct picture | **Step 2 works** → plane-hash A/B, then strip the CPU converter from the hot path |
| `render_frame rc=0x0` but garbled / channel-swapped picture | RT format/tiling — EVO's VO attr `0x8000000022000000` vs ProsperoLight's `0x80..00`; adjust `cx[2/4/15]` or register the VO buffer with ProsperoLight's attr |
| `pp_agc: FAULT in first agc_render_frame` | the `sigsetjmp` guard fired — AGC disabled, playback on the CPU converter; debug offline from the DCB build |

Full port write-up + the as-built deltas: **[agc-implementation.md](agc-implementation.md) §3–4a**.

## After Step 2 — Step 3: complete UI on the GPU (#28, committed scope)

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

## B — task 8 (the playback crash) — RESOLVED

Root cause was `posix_fadvise()` in `evo_stream_io.c` faulting (SIGSYS-class)
from the `PPSA99039` sandbox on file-open. Fixed `55685aa0` (both call sites
`#ifndef EVO_APP_MODULE`). 1080p + reasonable-4K play; demanding 4K (GTA
trailer) needed native decode → #31 (done). History in
[phase-1b-app-module.md](phase-1b-app-module.md) §8.

---

## Recovery (if something wedges)

- **Wedged elfldr payload** (stuck in a syscall): re-run the jailbreak to
  restart the elfldr host. Console stays otherwise healthy — check with
  `curl http://$PS5_HOST:8080/fs/` (kernel-R/W route; 200 = exploit still live).
  A kernel panic takes *all* network services down.
- **Wedged app slot**: PS button → close. No *clean* remote close exists — see
  the "remote-close investigation" section. `projects/app_ctl/` `kill` (ELF
  payload, `kill(pid,SIGKILL)` via kernel R/W) is the only remote lever and is
  unverified on hardware; etaHEN's IPC `KILL_APP_CMD` would be clean but isn't
  running here. Never `_exit()` from the app module itself.
- **Every new probe payload must carry a watchdog thread** that `_exit()`s on a
  hang — see `projects/agc_probe/main.c`.
- Consider adding `ps5-payload-shsrv` to the jailbreak chain for remote
  `ps`/`kill` (would also give a real remote-close path).

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

## Remaining host work

Toward Step 2/3:

1. ~~`pp/src/pp_agc.c` scaffold~~ ✅ — `pp_agc_init` + `agc_render_frame` +
   `pp_agc_present_nv12` ported; wired into `pp_playback.c`. Builds green.
   **Uncommitted** on `refactor/main-c-media-modules` pending review + the
   first device run.
2. **Solid-colour PS + 2D-ortho UI VS** — the Step 3 shader set
   (`build-shader.sh` workflow; the `.header.bin` half is still open — reuse
   ProsperoLight's).
3. Sketch `ui_rml/src/evo_rmlui_render_agc.cpp` — the `Rml::RenderInterface`→AGC
   mapping (Step 3 §2 in [agc-implementation.md](agc-implementation.md)).
4. `projects/app_ctl/` — the `sysctl` form is already fixed in-tree
   (`mib[4]={1,14,8,0}`); `list` / `kill` still need a hardware test.

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
