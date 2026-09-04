/*
 * pp_agc_osd.h - #28 Phase 2: the player OSD on the GPU 4K present path.
 *
 * main.c's render thread rasterises the RmlUi playback OSD into a 1920x1080
 * premultiplied-BGRA image and publishes it here. pp_playback's decode thread,
 * on the sceAgc present path, composites that image over the (downscaled)
 * decoded frame and hands the result to pp_agc_present_nv12_overlay() as a
 * full-frame opaque overlay quad (upscaled 2x on the GPU). Where the OSD is
 * transparent the output pixel is the video pixel verbatim - no YUV round trip
 * - so only the area actually under the OSD is touched.
 *
 * This replaces #32's drop-to-1080-VO scrub stopgap: the OSD now composites on
 * the 4K plane in the same DCB as the video.
 */
#ifndef PP_AGC_OSD_H
#define PP_AGC_OSD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_AGC_OSD_W 1920u
#define PP_AGC_OSD_H 1080u
/* tightly-packed NV12 1920x1080 */
#define PP_AGC_OSD_NV12_BYTES ((size_t)PP_AGC_OSD_W * PP_AGC_OSD_H * 3u / 2u)

/*
 * Render thread. `bgra` is PP_AGC_OSD_W*PP_AGC_OSD_H premultiplied 0xAABBGGRR
 * pixels (the same format EvoRenderInterface writes). active == 0 (or bgra ==
 * NULL) means "no OSD this frame" - the present path goes video-only.
 * Cheap: one memcpy of the changed image, double-buffered.
 */
void pp_agc_osd_publish(const uint32_t *bgra, int active);

/*
 * Decode thread. If an OSD is currently active, compose it over the video
 * frame (`vid_nv12`: Y plane at pitch, interleaved UV at vid_nv12 + pitch*vh;
 * displayed at ow x oh) and return a pointer to a tightly-packed NV12 1920x1080
 * surface (internal, valid until the next call - single decode-thread caller).
 * Return NULL if no OSD is active (present video-only).
 */
const uint8_t *pp_agc_osd_compose(const void *vid_nv12, uint32_t pitch, uint32_t vh,
                                  uint32_t ow, uint32_t oh);

/* 1 if an OSD is currently published active (render/decode thread poll). */
int  pp_agc_osd_active(void);

/*
 * Opt-in gate for the whole GPU-OSD path. Until #28 Phase 2 is hardware-proven
 * the default --agc-probe build presents video-only (the #27 path); the OSD
 * compositor engages only when /mnt/usb0/evo_agc_osd (or env EVO_AGC_OSD) is
 * present. Checked once, cached.
 */
int  pp_agc_osd_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PP_AGC_OSD_H */
