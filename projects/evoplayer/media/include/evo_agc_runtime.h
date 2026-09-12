#ifndef EVO_AGC_RUNTIME_H
#define EVO_AGC_RUNTIME_H

#include "sce/sce_agc.h"
#include "evo_agc_writer.h"
#include "evo_agc_transient_ring.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EVO_AGC_PIPE_UI = 0,
    EVO_AGC_PIPE_VIDEO_NV12 = 1,
    EVO_AGC_PIPE_VIDEO_HDR = 2,
    EVO_AGC_PIPE_VIDEO_HLG = 3,
    EVO_AGC_PIPE_VIDEO_PLANAR = 4,
    EVO_AGC_PIPE_COUNT = 5,

    EVO_AGC_FRAME_SLOTS = 3,
};

/* Where each [ResourceMapping] pointer goes in the user-SGPR block, copied
 * from the compiled pipeline's PAL metadata. Callers writing user data must
 * read these rather than hardcoding dword indices. */
typedef struct evo_agc_user_data_layout {
    uint32_t vs_count;
    uint32_t ps_count;
    int32_t  vs_const_table_dword;
    int32_t  vs_vertex_table_dword;
    int32_t  ps_texture_table_dword;
} evo_agc_user_data_layout_t;

typedef struct evo_agc_pipeline {
    void            *vs_shader;
    void            *ps_shader;
    evo_agc_user_data_layout_t user_data;
    /* From the compiled pipeline's PAL metadata: which of the automatic
     * user-data values (base vertex / base instance / draw index) the shader
     * consumes. sceAgcDcbDrawIndex/DrawIndexAuto need it to emit the matching
     * packet; the psbc path passed a hardcoded 0 and 2. */
    uint64_t         draw_modifier;
    SceAgcRegister  *cx_regs;
    uint32_t         cx_reg_count;
    SceAgcRegister  *sh_regs;
    uint32_t         sh_reg_count;
    SceAgcRegister  *uc_regs;
    uint32_t         uc_reg_count;
    int              valid;
} evo_agc_pipeline_t;

int  evo_agc_runtime_init(int width, int height, int hdr);
void evo_agc_runtime_shutdown(void);
int  evo_agc_runtime_is_active(void);

void evo_agc_runtime_frame_begin(void);
void evo_agc_runtime_frame_end(void);
void evo_agc_runtime_present(void);

void evo_agc_runtime_bind_pipeline(int pipeline_id);
void evo_agc_runtime_set_scissor(int x, int y, int w, int h);
void evo_agc_runtime_set_blend(int blend_mode);

/* clflush + mfence a range of CPU-written, GPU-read direct memory. Mandatory
 * before the GPU (shader texture-fetch, PM4 indirect-register DMA, or VideoOut
 * scanout) reads anything the CPU just wrote into it - direct memory here is
 * SCE_KERNEL_WB_ONION (write-back cached), so a fresh CPU write can sit in
 * L1/L2 indefinitely with nothing to force it out to the DRAM the GPU actually
 * reads. Matches the historical hardware-verified AGC present path (#27,
 * pp_agc.c's flush_gpu_data() - "Mandatory before every SubmitDcb"), which
 * this bare-metal rewrite omitted. No-op on NULL/zero-length. */
void evo_agc_runtime_cache_flush(const void *address, size_t bytes);

/* Copy the scanout buffer currently being DISPLAYED (the front buffer, i.e.
 * the one the last SetFlip handed to VideoOut) into a caller BGRA buffer -
 * the AGC equivalent of evo_gl_read_default_fb(). Without this the L3+R3
 * screenshot path captures gl_scratch, which an --agc build never renders
 * into, so every capture came back pure black. Reads LINEARLY: if the display
 * is actually interpreting the buffer as tiled, a capture taken through here
 * will look CORRECT while the panel looks scrambled - that difference is
 * itself the diagnosis. No-op when the runtime is not up. */
void evo_agc_runtime_read_scanout(uint32_t *bgra, int width, int height);

/* Mark the current frame as having real draw content. A frame that never calls
 * this is closed without being submitted or flipped, so it cannot blank the
 * screen between the UI's infrequent redraws. */
void evo_agc_runtime_note_draw(void);

/* Record that a draw was dropped: kind 0 = transient-ring allocation failed,
 * kind 1 = texture allocation/descriptor rejected. Both make UI content simply
 * not appear, with no error anywhere else. */
void evo_agc_runtime_note_drop(int kind);

/* User-SGPR layout of a compiled pipeline; zeroed counts mean "not valid". */
evo_agc_user_data_layout_t evo_agc_runtime_get_user_data_layout(int pipeline_id);

SceAgcCommandBuffer      *evo_agc_runtime_get_current_cb(void);
evo_agc_transient_ring_t *evo_agc_runtime_get_transient_ring(void);
uint32_t                  evo_agc_runtime_get_current_slot(void);
void                      evo_agc_runtime_get_size(int *width, int *height);

void evo_agc_blit_yuv(const uint8_t *y,  int y_pitch,
                      const uint8_t *uv, int uv_pitch,
                      const uint8_t *u,  int u_pitch,
                      const uint8_t *v,  int v_pitch,
                      int coded_w, int coded_h,
                      int disp_w, int disp_h,
                      int view_mode, int ten_bit, int color_trc,
                      int is_direct);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AGC_RUNTIME_H */
