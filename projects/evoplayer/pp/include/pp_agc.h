/*
 * pp_agc.h - GPU video convert + present via sceAgc (GPU rendering Step 2, #27).
 *
 * A strip of blackbearreloaded/ProsperoLight src/native_agc_present.cpp: the
 * fullscreen NV12/P010 -> RGB sample + flip that runs on the VCN-adjacent GPU
 * from a registered app module. Replaces the CPU pp_converter_* + swizzle +
 * sceVideoOutSubmitFlip on the 4K/1080p video path. Menus keep the Step 1 CPU
 * surface cache.
 *
 * Only real under EVO_APP_MODULE. Everywhere else pp_agc_available() is 0 and
 * the caller uses the CPU path. Design: docs/evo-pro/agc-implementation.md §3-4.
 *
 * STATUS: initialize (sceAgcInit -> shader memory -> CreateShader x2 ->
 * LinkShaders) is hardware-verified; render_frame is ported (SDR / NV12, no
 * overlay) and pp_agc_present_nv12() drives it. Wired into pp_playback.c's V8
 * branch. The first hardware run hung on a GPU-side submit stall; #27 B now
 * runs render_frame on a dedicated worker thread behind a 250 ms watchdog, so a
 * wedged submit drops to the CPU path instead of freezing the app, and #27 A
 * registers the VO with ProsperoLight's linear SDR attribute when armed. The
 * end-to-end GPU picture is still unproven on hardware.
 */
#ifndef PP_AGC_H
#define PP_AGC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up: sceAgcInit, allocate + map the shader scratch, copy the
 * blobs, create + link the VS/PS. `hdr` selects the P010 pixel shader.
 * Returns 0 on success; <0 leaves pp_agc_available() == 0. Idempotent. */
int  pp_agc_init(uint32_t width, uint32_t height, int hdr);

/* 1 once pp_agc_init() has succeeded and the GPU present path is usable. */
int  pp_agc_available(void);

/*
 * #28 Phase 1 go/no-go: create + link the hand-written UI shaders (ui_vs +
 * solid/glyph/rgba PS) against ProsperoLight's reused headers and log every rc
 * to evo_boot.log. Pure library calls - no DCB, no draw, no flip; changes no
 * state. Needs pp_agc_init() to have mapped the shader scratch first. Called
 * from evo_agc_probe() (--agc-probe builds only). Returns 0 iff all four
 * CreateShader calls returned 0.
 */
int  pp_agc_probe_ui_shaders(void);

/*
 * Present one decoded NV12 frame on the GPU: YUV->RGB fullscreen convert into
 * `gpu_target`, then a flip queued in the same DCB.
 *
 *   vout_handle  EVO's own sceVideoOut handle (pp_videoout.handle) - reused,
 *                never sceVideoOutOpen'd here.
 *   buf_idx      the VO buffer index just returned by pp_videoout_acquire
 *                (0..PP_VO_MAX_BUFFERS-1). Also selects the NV12 staging slot.
 *   gpu_target   pp_videoout_gpu_plane(vo, buf_idx) - the registered tiled
 *                render target for that buffer.
 *   nv12         Y plane; interleaved UV follows at nv12 + pitch_bytes*coded_height.
 *                May be plain (malloc / non-GPU) memory - it is staged into
 *                GPU-visible direct memory internally.
 *   pitch_bytes  luma row pitch (== chroma row pitch for NV12).
 *   coded_height MB-padded luma height (where the UV plane starts).
 *   vis_w/vis_h  display crop.
 *   out_w/out_h  the VO output dimensions (== vo->width/height).
 *   flip_marker  handed to sceAgcDcbSetFlip AND expected as the frame_id passed
 *                to pp_videoout_adopt_flip() by the caller, so the VO retire
 *                logic tracks the GPU-queued flip. Must be monotonic.
 *
 * The DCB owns the flip, so the caller must NOT also SubmitFlip this buffer.
 *
 * Return:
 *    0   submitted — caller adopts the GPU-queued flip (pp_videoout_adopt_flip).
 *   -1   failed before/at submit — caller RELEASES the buffer, next frame takes
 *        the CPU path (a first-frame CPU fault also disables pp_agc + returns -1).
 *   -2   the submit blew the 250 ms watchdog (#27 B). The worker thread is
 *        abandoned mid-GPU-call and may still write the target + queue its flip,
 *        so the caller must ADOPT the flip, not release. pp_agc is permanently
 *        unavailable for the rest of the session; playback stays on the CPU path.
 */
int  pp_agc_present_nv12(int vout_handle, uint32_t buf_idx, void *gpu_target,
                         const void *nv12, uint32_t pitch_bytes, uint32_t coded_height,
                         uint32_t vis_w, uint32_t vis_h, uint32_t out_w, uint32_t out_h,
                         int64_t flip_marker);

void pp_agc_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PP_AGC_H */
