# GL-4 (#80) — video into the GL funnel

> **Status: Stage 1 (bump + re-measure) done + hw-verified 2026-09-10.** The
> re-measure **removed the zero-copy requirement** — see below. Stage 2 (the
> NV12 GL video path) and Stage 3 (converter demolition) are scoped but not
> started. Parent: [opengl-render-overhaul.md](opengl-render-overhaul.md) (GL-4
> row). Handoff: [#80 comment](https://github.com/sainsaji/EVO-PLAYER-PS5/issues/80#issuecomment-5607427903).

## The upload wall — and the way past it

GL-3 B3 (`8056627`) presents the player frame through GL, but at **~14 fps**:
`glTexSubImage2D` of one 1080p **RGBA8** frame is ~68 ms — the driver does a
synchronous CPU staging copy on 4-channel uploads.

Stage 1 bumped `ps5-opengl` G47 → G55 and added `smoke_bench_uploads()` to the
`--gl-smoke` probe. **Hardware (G55, FW 12.70, 2026-09-10):**

| upload | mean | p95 |
|---|---|---|
| 1080p RGBA8         | 65.5 ms  | 65.7 ms |
| 1080p RGBA8 via PBO | 65.7 ms  | 66.2 ms |
| **1080p luma R8**   | **0.10 ms** | 0.11 ms |
| **1080p chroma RG8**| **0.03 ms** | 0.03 ms |
| 4K RGBA8            | 252.8 ms | 254.2 ms |
| **4K luma R8**      | **0.90 ms** | 0.96 ms |
| **4K chroma RG8**   | **0.46 ms** | 0.50 ms |

GL-1 regression on G55: `result=PASS … renderer="PS5 AGC" gl="3.3 (Core Profile)
Mesa 26.2.0"`, pixel-exact.

**The staging copy is RGBA8-only.** R8 / RG8 uploads take a fast path — free.
So an **NV12 two-plane upload (R8 luma + RG8 chroma)** costs **0.13 ms/frame at
1080p, 1.36 ms/frame at 4K** — real-time with huge margin.

**→ Zero-copy surface import is not needed. No ps5-opengl Gallium contribution.**
GL-4 Stage 2 is the #80 issue body as written: NV12 upload → GLSL YUV→RGB →
fullscreen quad → composite → one flip → delete the CPU converters.

(If a future ps5-opengl bump *regresses* the R8/RG8 fast path, the zero-copy
design is still viable — the decoder's frame pool is linear GPU-visible direct
memory — but it is a fallback, not the plan. Sketch kept in "Appendix" below.)

## Stage 1 — done (this session)

- `third_party/ps5-opengl` G47 (`23a594c`) → G55 (`9eb75fc`,
  `sdk-0.1.0-perf20260909-g55-hfr-sdl2-focused`).
- `patches/ps5-opengl/0001-recoverable-fail.patch` re-anchored to G55 (same 4
  `_Exit` → `ps5gl_fatal` sites, `ps5gl_fatal.h` unchanged; `git apply --check`
  clean vs pristine G55).
- `pp_gl_smoke.c` — `smoke_bench_uploads()` (bench only; smoke `result`
  unchanged). Numbers above.
- SDK rebuilt from G55 source (needed a pristine re-extract of
  `third_party/mesa-26.2.0/` — G55's `mesa-ps5.patch` differs from G47's).

## Stage 2 — the NV12 GL video path

### 2a. Keep NV12 out of `evo_vdec_native.c`'s I420 branch

`ro_harvest` (`media/src/evo_vdec_native.c:393`) already has an `agc_out` path
that **borrows** the decoder's NV12 pointer (no copy). The non-`agc_out` branch
de-interleaves NV12 → planar I420 *only* because the CPU converters need it.
When the GL path is active, always take the NV12/borrow branch — emit
`PP_FRAME_NV12` with `planes[0]` = Y, `planes[1]` = interleaved UV, strides +
coded height. (`pp_frame` already carries `planes[]` + `strides[]`; the CPU
converters ignore them, `pp_agc` uses them.)

### 2b. NV12 upload + GLSL YUV→RGB — `ui_rml/src/evo_gl_context_device.cpp`

Replace the RGBA path in `evo_gl_blit_bgra()` (or add `evo_gl_blit_nv12()`):

- Two textures: `GL_R8` (`w`×`coded_h`, luma), `GL_RG8` (`w/2`×`coded_h/2`,
  chroma). `glTexStorage2D` once; `glTexSubImage2D` per frame from `planes[0]` /
  `planes[1]`. Respect `strides[]` via `glPixelStorei(GL_UNPACK_ROW_LENGTH, …)`.
- Fullscreen triangle (existing attribute-less VS) + a fragment shader:

  **Parity target = BT.601 limited-range** — that is what every CPU converter
  does today (`pp_converter.c:24`, integer `298/409/516/-100/-208`, `Y-16` /
  `UV-128`), *not* BT.709. `#62` parity = match this. A move to BT.709 is a
  separate, deliberate change — flag it, don't slip it in.

  ```glsl
  float y = texture(uLuma,  vUV).r;
  vec2  uv = texture(uChroma, vUV).rg;
  float c = (y - 16.0/255.0) * 1.16438;
  float d = uv.x - 128.0/255.0;
  float e = uv.y - 128.0/255.0;
  vec3 rgb = vec3(c + 1.59603*e,
                  c - 0.39176*d - 0.81297*e,
                  c + 2.01723*d);
  fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
  ```
  - `uRange` uniform (limited/full) for a later `evo_settings` hook; default
    limited.
  - Default framebuffer byte order matches what B2/B3 already write — emit RGB
    straight, **no `.bgr` swizzle** (that swizzle in the B2 blit compensates for
    EVO's *CPU rasteriser* buffer, not this path).
  - Leave room for **P010** (`#4` tail): `GL_R16` / `GL_RG16`, `* 64.0` unpack,
    same matrix. Implement 8-bit NV12 only now.

### 2c. Aspect in the vertex quad — closes #76

FIT / FILL / STRETCH (`video_view_mode` 0/1/2, `prospero_view_mode_to_aspect`,
`main.c:1606`) become a scale on the quad's clip-space corners (display AR vs
16:9 output). Removes `pp_playback_force_v3_fallback` + the
`PP_BACKEND_4K_V8_FUSED → _V3_FALLBACK` switch in `prospero_apply_view_mode`
(`main.c:1621`) and the linear-vs-tiled attr mismatch that corrupts the 4K plane
on an aspect cycle (**#76**).

### 2d. Present integration

- Player branch in `main.c` ~L13449 (`EVO_GL_DEVICE`): swap still gated on
  `g_pp_pb.display_pts_us`; calls `evo_gl_blit_nv12()` with the NV12 planes
  instead of `evo_gl_blit_bgra(gl_scratch,…)`.
- OSD composites in the same GL frame — `draw_player_screen` renders the OSD to
  a small RGBA overlay texture (or the RmlUi GL3 context) drawn over the video
  quad; one `eglSwapBuffers`.
- The `#32` `prospero_scrub_ovl_state` machine + `pp_product_overlay_enter/leave`
  go inert (GL-6 deletes them). Stage 2 must **verify seek/scrub needs no
  overlay drop** on the GL path.
- `pp_product_request_vo()`'s `EVO_GL_DEVICE` early-return unchanged.

## Stage 3 — demolition

Once GTA 4K native + a 1080p clip play real-time through the GL NV12 path with
#62 parity:

- **Delete:** `pp/src/pp_converter_fused.c`, `pp_converter_parallel.c`,
  `pp_compute_pipeline.c`, `tile_copy.c`; the CPU present path in
  `pp_videoout.c` + its linear/tiled attr juggling + `013_AGC_VO_RETILE`; the
  5-way present dispatch in `main.c` (~L13210–13505); the
  `PP_BACKEND_4K_V8_FUSED` / `_V3_FALLBACK` / `_1080_STANDARD` enum and every
  branch; `pp_playback`'s convert/present halves + the backend field.
- **Keep:** `pp_playback` decode + pace + clock + seek; `pp_playback_log_stats`
  / the stats file (verify it still produces sane numbers — #80 "done when").
- `pp_agc.c` present/geo/osd stop being called here; **GL-6** deletes the files.
- `#62` parity write-up + reference frame → `docs/validation.md`.

## Done when (from #80)

- [ ] GTA 4K (native) + a 1080p clip through the GL video path, correct colour,
      real-time.
- [ ] `#62` parity vs a known-good reference frame in `docs/validation.md`.
- [ ] `#76` fixed — aspect cycle on 4K native, no corruption, no stutter.
- [ ] CPU converters + `tile_copy` + backend enum + 5-way dispatch deleted.
- [ ] Seek / scrub works without the `#32` overlay machine.
- [ ] `pp_playback` stats file still sane.

## Appendix — zero-copy (fallback only, not the plan)

If the R8/RG8 fast path ever regresses: the session-resident decoder writes
linear contiguous NV12 into a direct-memory frame pool
(`evo_vdec_native.c` ~L235–256: `alloc_direct(frame_pool, 0x32, …)` →
`g_boot.frame_start` offset + `g_boot.frame_mem` VA, 12 × `frame_size`). A GL
texture could wrap that VA instead of uploading:

- `ps5-opengl` has **no** `resource_from_handle` / `resource_from_user_memory` /
  EGLImage path (`src/gallium/ps5/ps5_screen.c`, 10.6k lines). Would need a
  private `ps5_resource_from_external(offset, size, pitch, fmt, w, h)` building a
  `struct ps5_resource` (`:295`) with `direct_start` = the external offset,
  linear layout (`ps5_linear_sampled_layout`, `:1066`), a "borrowed" flag so
  `resource_destroy` skips unmap/release. Carried as
  `patches/ps5-opengl/0002-*.patch`.
- Open question that made this risky: whether ps5-opengl's `sceAgc` context can
  bind a foreign direct-memory VA as a shader SRV (prot `0x32`), inspect
  `src/platform/ps5_agc_native_runtime.c` + `ps5_agc_package.c`.

## References

- `projects/evoplayer/pp/src/` — `pp_converter*.c` (`pp_converter.c:24` = the
  parity matrix), `pp_compute_pipeline.c`, `tile_copy.c`, `pp_videoout.c`,
  `pp_playback.c`, `pp_agc.c`
- `projects/evoplayer/main.c` — present dispatch ~L13210–13505,
  `prospero_apply_view_mode` ~L1621, `EVO_GL_DEVICE` player branch ~L13449
- `projects/evoplayer/media/src/evo_vdec_native.c` — frame pool, `ro_harvest`
- `projects/evoplayer/ui_rml/src/evo_gl_context_device.cpp` — `evo_gl_blit_bgra`
- `projects/evoplayer/pp/src/pp_gl_smoke.c` — `smoke_bench_uploads()`
- `docs/converter-perf.md`, `docs/evo-pro/agc-implementation.md`,
  `docs/evo-pro/gl1-spike.md`
