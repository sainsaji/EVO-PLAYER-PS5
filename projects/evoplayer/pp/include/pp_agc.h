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
 * LinkShaders) is ported; render_frame / present is NOT yet wired -
 * pp_agc_present_nv12() returns <0.
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
 * Present one decoded frame. `nv12` points at the Y plane; the interleaved UV
 * plane follows at `nv12 + pitch_bytes * coded_height`. `vis_w`/`vis_h` are the
 * display crop. `flip_marker` is handed to sceAgcDcbSetFlip. Blocks until the
 * previous flip for this buffer is safe. Returns 0 on present, <0 on failure
 * (caller falls back to CPU for the session).
 */
int  pp_agc_present_nv12(const void *nv12, uint32_t pitch_bytes,
                         uint32_t coded_height, uint32_t vis_w, uint32_t vis_h,
                         int64_t flip_marker);

void pp_agc_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PP_AGC_H */
