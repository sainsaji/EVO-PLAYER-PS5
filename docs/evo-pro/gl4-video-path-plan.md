# GL-4 (#80) — video into the GL funnel

> **Status: DONE — all stages hw-verified 2026-09-10.** GTA 4K native + 1080p
> play smooth through the GL NV12 path, OSD composites over the video, aspect
> (Fit/Fill/Stretch) works, colours correct and measured (`#62`, see below).
> Stage 2d freezes the last frame under the scrub OSD for the whole
> seek-discard window; Stage 3 deleted the CPU converters, `tile_copy`, the
> backend enum, the 5-way present dispatch and the `#32` scrub-overlay machine,
> and retired `--no-gl`. Two issues the hardware pass caught, both fixed the
> same day: a subtitle regression (subs vanished 4.2 s after the last input —
> the OSD scratch was only composited while the controls were up), and a
> ~65 ms hitch per subtitle-cue / OSD change (the scratch went up as a
> full-frame RGBA8 texture — the ps5-opengl staging wall; now uploaded as RG8,
> ~0.2 ms). Plus a boot shader warm-up so the first video frame does not pay a
> lazy GLSL compile inside the loop. `late_drop` 0 on a clean session,
> `blit~5 ms`. Parent: [opengl-render-overhaul.md](opengl-render-overhaul.md)
> (GL-4 row).

## Stage 2 progress (hardware, 2026-09-10)

- **2a (NV12 video quad) — works.** GTA 4K native + 1080p play through the GL
  path; `GL-3 player: blit~3-4ms` (was ~65). Zero-copy: decoder planes →
  R8/RG8 textures directly, `pp_playback_get_nv12()` hands out borrowed
  pointers, no copy anywhere. Native decoder emits NV12 straight
  (`evo_vdec_prefer_nv12`).
- **Colour fix:** the ps5-opengl default framebuffer scans out BGRA, so the
  YUV→RGB shader output is swizzled `.bgr` (same as `evo_gl_blit_bgra`).
- **2b (aspect) + 2c (OSD) — done + hw-verified.** Aspect is a `uScale` on the
  vertex quad (FIT letterbox / FILL crop / STRETCH; 16:9 content shows no
  change on a 16:9 panel, as expected); the OSD rasterises into `gl_scratch`
  via `draw_player_screen`'s `g_k4_osd_publish` mode and `evo_gl_composite_bgra`
  blends it over the video — the slow RGBA8 OSD upload is sample-hash-gated so a
  static OSD costs only a draw. Per-frame OSD cost ~7 ms at 4K (memset + OSD
  rasterise + hash) → `blit~10-13ms`, well inside the 33 ms budget.
- **2d (seek) — done + hw-verified 2026-09-10.** See "Stage 2d" below.
- **Not GL-4:** Tears of Steel 4K (H.264 3840×1714) hits the resident
  decoder's `0x811d0303` on decode #1 → FFmpeg-4K fallback → struggles. This
  is the pre-existing `native-decode` limitation (main.c ~L11390), not the GL
  path. GTA 4K's encode is accepted; ToS4K's isn't.

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

## Stage 2 — the NV12 GL video path — done

The design sketch that used to sit here has been replaced by the code. What
actually shipped, and where to read it:

**Decoder → planes, no copy.** `evo_vdec_prefer_nv12(1)` at boot makes
`ro_harvest` (`media/src/evo_vdec_native.c`) take its borrow branch and emit
`PP_FRAME_NV12` — `planes[0]` = Y, `planes[1]` = interleaved UV, plus strides and
the MB-padded coded height. The I420 de-interleave it used to do existed only
because the CPU converters could not read NV12. `pp_playback_push_frame` paces
the frame on the presentation clock and stashes those borrowed pointers;
`pp_playback_get_nv12()` hands them out under the display lock. No pixel is
copied between the decoder and the GPU — except once per seek, see Stage 2d.

**Upload + convert.** `evo_gl_blit_yuv()` in
`ui_rml/src/evo_gl_context_device.cpp`: a `GL_R8` luma texture at `coded_w ×
coded_h` and a `GL_RG8` chroma texture at half that (or three `GL_R8` planes for
an FFmpeg I420 source), `glTexStorage2D` once per resolution and
`glTexSubImage2D` per frame with `GL_UNPACK_ROW_LENGTH` carrying the stride. A
quad-strip vertex shader and a fragment shader that does the YUV→RGB.

Two things the sketch got wrong, both settled by hardware and by
`tools/gl_yuv_parity.py`:

- The output **is** `.bgr`-swizzled. The ps5-opengl default framebuffer scans
  out BGRA, so the shader has to swap R and B exactly as `evo_gl_blit_bgra`'s
  sampler does. Without it the picture is R↔B swapped.
- The offsets are **16/255 and 128/255**, not 16/256 and 0.5. The CPU reference
  works in 0–255 units. See the parity write-up.

Parity target is BT.601 **limited** range — what every CPU converter did. A move
to BT.709 would be a separate, deliberate change; do not slip it in. `uRange`
exists as a hook but is not wired to `evo_settings`. **P010 / 10-bit moved to
GL-5 (#81)** — GL-4 is 8-bit only; the shader leaves room (`GL_R16` / `GL_RG16`,
`* 64.0` unpack, plus an SDR tone-map) but it is not wired.

**Aspect.** FIT / FILL / STRETCH is a `uScale` on the quad's clip-space corners,
computed from the frame's display aspect against the panel's. That is the whole
of `#76`'s fix: `prospero_apply_view_mode` now only records the mode, instead of
forcing the 4K V8 path down to the V3 converter and re-targeting the output at
the live VO size — which left the VideoOut registered linear while a tiled
converter wrote it, and corrupted the plane.

**OSD.** `draw_player_screen` under `g_k4_osd_publish` paints the OSD (and the
subtitles) onto a transparent 1920×1080 scratch and skips the video draw
entirely; the present block alpha-composites it over the quad with
`evo_gl_composite_bgra`. The scratch is uploaded as an **RG8** texture 2× the
width (source pixel x = texels 2x/2x+1, the shader reassembles with
`texelFetch`) — a 4-channel RGBA8 `glTexSubImage2D` is the ~65 ms staging wall,
RG8 dodges it (~0.2 ms), the same trick as the video Y/UV planes. It is still
gated on a sample hash so a static overlay costs one composite draw and no
upload. One `eglSwapBuffers` for video + OSD together. `_osd_active` (the gate
that decides whether to draw + composite the scratch) covers the controls
window, pause / scrub / stats / seek-discard, **a visible subtitle track**, a
live toast and the dev FPS overlay — the subtitle term is why "no subs 4 s
after the last button press" was a regression the hardware pass caught.

**Not on the quad:** the subtitle picker and the stop-playback prompt. Both are
RmlUi documents with a full-screen scrim (`dialog.rcss` `#dialog-scrim`,
`#000000b8`) and EVO's CPU rasteriser blends source-over to an **opaque** result
(`evo_blend.h`), so compositing their scratch over the video would paint solid
black across it anyway. They keep the whole-scratch blit until GL-5 renders
RmlUi through GL with real alpha.


## Stage 2d — seek — done + hw-verified 2026-09-10

**The bug.** In the steady state the published planes are *borrowed* from the
decoder: `push_frame` pace-sleeps to the frame's PTS before returning, so the
decoder cannot recycle the pool slot before the render loop has uploaded it. A
seek's discard window is the one place that argument fails — the decode thread
runs flat out, publishing nothing, decoding straight through the pool. The
render loop keeps re-uploading `gl_src_y`, which is now whatever discarded frame
the decoder happens to be writing. On a long-GOP 4K seek that is 2–6 seconds of
tearing garbage under the scrub bar.

**The fix.** `pp_playback` takes one snapshot of the last published frame into
memory it owns, at `notify_seek_begin` — the instant the seek is submitted, while
the native decoder's 12-slot pool and FFmpeg's receive `AVFrame` both still hold
it. `pp_playback_get_nv12()` serves that copy (`f->held == 1`) until the first
post-seek frame publishes, which clears it. One copy per seek; the steady state
stays zero-copy.

`main.c` keeps the OSD up for the whole window (`|| g_pp_pb.seek_discarding` in
the OSD-visible test) rather than the usual 4.2 s idle timeout, so a slow seek
shows a scrub bar over a still picture instead of going quiet.

**What this replaced.** The `#32` `prospero_scrub_ovl_state` machine — a
four-state ENTERING/ACTIVE/LEAVING handshake with `video_decode_parked`, whose
whole job was to drop the 4K VideoOut to a 1080 one for the duration of a scrub
because the V8 present path never composited the OSD. There is one surface now
and the OSD composites over the quad at any resolution, so the premise is gone.
Deleted, along with `pp_product_overlay_enter/leave`'s surface half — those two
are now just the presentation-clock pause that stops an overlay coming back and
judging every frame late (the 0.1.3 Media Info freeze).

**Still GOP-bound.** `seek_to_first_ms` is unchanged: the discard window is as
long as the codec makes it. Stage 2d makes it *look* like a seek instead of like
a fault; it does not make it shorter.

**Regression caught + fixed before the hardware pass (2026-09-10).** Stage 2c's
OSD gate (`_osd_active`) only composited the `gl_scratch` overlay while the
playback controls were up — but `draw_player_screen()` also rasterises
**subtitles** onto that same scratch, so subs vanished 4.2 s after the last
button press during normal playback. `_osd_active` now also covers "subtitles
enabled and the file has a subtitle track", a visible toast (`evo_toast_visible()`
— new accessor in `evo_toast.c`), and the dev FPS overlay; and the present block
re-draws the toast / FPS overlay onto the scratch *after* `draw_player_screen`'s
memset (the dispatch draws them earlier in the frame, where that clear wiped
them). This makes the "subtitles stay up with the OSD hidden" and "a toast fired
without a recent button press" checks part of the hardware pass.

## Stage 3 — demolition — done + hw-verified 2026-09-10

**Deleted outright**

| | |
|---|---|
| `pp/src/pp_converter.c` + `.h` | the BT.601 reference matrix — preserved in `tools/gl_yuv_parity.py` and in git (`b8c42b7`) |
| `pp/src/pp_converter_parallel.c` + `.h` | V3 worker-pool convert |
| `pp/src/pp_converter_fused.c` + `.h` | V8 fused convert+tile |
| `pp/src/pp_compute_pipeline.c` + `.h` | the SIMD/workgroup convert front end |
| `pp/src/tile_copy.c` | `pp_draw_pixels_as_tiles`, the CPU scanout swizzle |
| `pp/include/pp_product_path.h` | `pp_video_backend` + `pp_select_video_backend` |
| `pp_output_policy.h`, `pp_4k_sdr_policy.h`, `pp_4k_product_stage.h`, `pp_v8_gate.h` | the 4K stage ladder and its gates — nothing selects a backend any more |
| `tools/bench.sh`, `tools/bench_converter.c` | the converter benchmark harness ([converter-perf.md](../converter-perf.md) is now history) |

**Cut back**

- `pp_playback` — 1134 → ~500 lines. Gone: `display`/`display_back`, the backend
  field, `force_v3_fallback`, the pending-present handshake, the `nv12_fb`
  de-interleave scratch, the whole sceAgc present branch and the `agc_hold_*`
  frame. What is left is decode + pace + clock + seek + publish. `pp_aspect_mode`
  moved to `pp_frame.h`, which is where a frame-presentation type belongs now
  that the converter header that owned it is gone.
- `pp_videoout.c` — `pp_videoout_acquire()` / `pp_videoout_present()` and the
  `cpu_bufs` linear staging are gone (they were the CPU present path). The rest
  of the file is dead code until GL-6 deletes it.
- `main.c` — 13694 → ~12700 lines. Gone: `pp_product_reconfigure_vo` /
  `_request_vo` / `_apply_pending_vo` / `_k4_live` / `_ensure_ui_1080`, the
  `g_vo_*` / `g_pending_*` / `g_4k_*` globals, the 5-way present dispatch, the
  4K source pre-scan and backend selection, the `#32` overlay machine, and the
  `PP_BACKEND_ENABLED` / `EVO_GL_DEVICE` conditional pairs (there is one path, so
  the branches went with them).

**The build**

`--no-gl` is retired: it selected a present path that no longer exists, and
`package-app.sh` now fails with that explanation rather than producing an eboot
that boots to nothing. `main.c` calls the `evo_gl_*` seam unconditionally; the
app module links `evo_gl_context_device.cpp`, and every other configuration
(`build-evoplayer.sh`'s ELF compile check, `--probe`, `--gl-smoke`) links
`ui_rml/src/evo_gl_context_stub.c`, a set of no-ops. The ELF build therefore
still compiles `main.c` / `pp` / `media` for the modularisation work, and still
has no graphics — which it never had.

**Left for GL-6**, because they still compile and something still references
them: `pp_agc.c` / `pp_agc_osd.c` (nothing calls them — `pp_agc_init` went with
the boot cutover), `pp_videoout.c`'s remainder, `evo_rmlui_render_agc.cpp`.

**#62 parity** — `tools/gl_yuv_parity.py` sweeps all 2^24 `(Y,U,V)` triples
through both the deleted CPU matrix and the GLSL shader: 99.390% bit-exact, max
per-channel delta 1/255, and it found a real error (16/256 vs 16/255 offsets,
worth up to 2/255 on 3.8% of triples) that the panel could not show. Written up
in [../validation.md](../validation.md#gl-video-path-colour-parity-62-delivered-by-gl-4--80).

## Done when (from #80)

- [x] GTA 4K (native) + a 1080p clip through the GL video path, correct colour,
      real-time. *(hw-verified 2026-09-10, Stage 2a-c)*
- [x] `#62` parity vs the reference conversion, in
      [../validation.md](../validation.md#gl-video-path-colour-parity-62-delivered-by-gl-4--80).
      *(exhaustive host sweep - stronger than any single frame, which only
      visits the few thousand triples it happens to contain)*
- [x] `#76` fixed - aspect is a vertex-quad scale; `force_v3_fallback` and the
      linear-vs-tiled attr mismatch that corrupted the 4K plane are both deleted.
- [x] CPU converters + `tile_copy` + backend enum + 5-way dispatch deleted.
- [x] Seek / scrub works without the `#32` overlay machine - the machine is
      deleted; the held-frame snapshot replaces it. **hw-verified 2026-09-10.**
- [x] `pp_playback` stats file still sane - `pp_playback_log_stats` keeps every
      counter; `convert_us_*` now measures publish cost and the line is
      relabelled `publish_us_*`. **hw-verified** — `late_drop=0` on a clean
      session, `blit~5 ms`, no `clock_late_drops` growth.

## Hardware pass — done 2026-09-10

All checks passed on `192.168.0.7` (FW 12.70). Residual: a one-time ~20-frame
late-drop burst at the very first file open (texture alloc + decode-thread
ramp, not a stall) — not perceptible, left as-is.

1. GTA 4K native - plays real-time, correct colour, `late_drop` 0-1.
2. A 1080p clip - same.
3. Triangle cycles Fit/Fill/Stretch on the 4K file: no corruption, no stutter
   (`#76`).
4. D-pad scrub then Cross on the 4K file: the picture **freezes** on the last
   frame under the scrub bar for the whole discard window, then resumes at the
   target. No tearing, no black.
5. Square (Media Info) and Circle (stop prompt) over playback still return to
   moving video with audio in sync - the clock-pause half of the old overlay.
6. **Subtitles** on a subtitled file: they stay on screen after the playback
   controls fade (this is the Stage 2c regression that was fixed). Cycle to a
   non-subtitled file and back - no stale caption.
7. A toast during playback with no recent button press (e.g. let one auto-fire,
   or take a screenshot then wait) shows over the video.
8. `tools/evo-remote.sh log` - the `stats ...` block has sane numbers.

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
