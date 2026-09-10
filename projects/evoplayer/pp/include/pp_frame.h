/*
 * Neutral decoded-frame structure — no FFmpeg types.
 * FFmpeg (or any decoder) adapts *into* this shape; videoout never sees AVFrame.
 */
#ifndef PP_FRAME_H
#define PP_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_frame_format {
    PP_FRAME_NV12 = 0,
    PP_FRAME_YUV420P = 1,
    PP_FRAME_RGBA = 2,
    PP_FRAME_BGRA = 3
} pp_frame_format;

/*
 * How the frame is fitted to the panel. Since GL-4 (#80) this is a scale on the
 * video quad's clip-space corners (evo_gl_blit_yuv), not a CPU rescale — it
 * lives here because it describes the frame's presentation, and the converter
 * header that used to own it is gone.
 */
typedef enum pp_aspect_mode {
    PP_ASPECT_FIT = 0,     /* letterbox / pillarbox, preserve AR */
    PP_ASPECT_FILL = 1,    /* crop source to fill, preserve AR */
    PP_ASPECT_STRETCH = 2  /* ignore AR */
} pp_aspect_mode;

typedef struct pp_frame {
    pp_frame_format format;
    uint32_t width;
    uint32_t height;
    /* MB-padded luma height for NV12 — the row count at which the interleaved
     * UV plane starts (planes[1] == planes[0] + strides[0]*coded_height).
     * 0 means "same as height". Only the native NV12 decode path sets it;
     * the GPU (sceAgc) present path needs it, the CPU converters ignore it. */
    uint32_t coded_height;
    const uint8_t *planes[4];
    int strides[4];
    int64_t pts_us;
} pp_frame;

#ifdef __cplusplus
}
#endif

#endif /* PP_FRAME_H */
