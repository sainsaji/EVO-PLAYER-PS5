/*
 * pp_compute_pipeline.h — CPU SIMD & Vectorized Workgroup YUV Pipeline.
 *
 * Implements a high-throughput CPU AVX2 SIMD & Workgroup Pipeline for
 * YUV420P / NV12 / P010 -> PS5 Tiled BGRA Direct VideoOut conversion.
 */
#ifndef PP_COMPUTE_PIPELINE_H
#define PP_COMPUTE_PIPELINE_H

#include "pp_frame.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PP_COMPUTE_BACKEND_AUTO = 0,
    PP_COMPUTE_BACKEND_VECTOR_WORKGROUP,
    PP_COMPUTE_BACKEND_CPU_SIMD,
    PP_COMPUTE_BACKEND_FALLBACK
} pp_compute_backend_t;

typedef struct {
    pp_compute_backend_t backend;
    int                  num_workers;
    int                  color_matrix; /* 0 = BT.709 (HD/4K), 1 = BT.601 (SD) */
} pp_compute_config_t;

/* Initialize the CPU SIMD Workgroup Pipeline */
int pp_compute_pipeline_init(const pp_compute_config_t *cfg);

/* Shutdown compute workers */
void pp_compute_pipeline_shutdown(void);

/* Query active compute pipeline backend name */
const char *pp_compute_pipeline_get_backend_name(void);

/**
 * Execute CPU SIMD YUV Pipeline:
 * Converts YUV420P frame into PS5 tiled BGRA VideoOut direct memory buffer.
 *
 * @param source            Input YUV420P frame
 * @param tiled_destination Output PS5 tiled BGRA buffer
 * @param frame_width       Video frame width (e.g. 1920 or 3840)
 * @param frame_height      Video frame height (e.g. 1080 or 2160)
 * @param workers           Number of compute worker workgroups (1..16)
 * @return 0 on success, negative error code on failure
 */
int pp_compute_pipeline_convert(const pp_frame *source,
                                uint32_t *tiled_destination,
                                uint32_t frame_width,
                                uint32_t frame_height,
                                int workers);

#ifdef __cplusplus
}
#endif

#endif /* PP_COMPUTE_PIPELINE_H */
