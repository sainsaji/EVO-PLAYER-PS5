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
    EVO_AGC_PIPE_UI_BLUR = 5,
    EVO_AGC_PIPE_COUNT = 6,

    EVO_AGC_FRAME_SLOTS = 3,
};

/* Full-canvas RGBA8 layer surfaces for RmlUi PushLayer / CompositeLayers
 * (backdrop blur, drop-shadow, filter composition). Sized for the maximum
 * render size (4K): pitch-aligned 256, standard pitch = 4K*4 = 15360 bytes.
 * The GPU writes RGBA (no COMP_SWAP) so the C++ side can read them back via
 * the rgba8 T# swizzle; the scanout backbuffer uses COMP_SWAP=ALT for BGRA,
 * so its T# must use the bgra8 swizzle. */
#define EVO_AGC_MAX_LAYERS 4
typedef struct evo_agc_layer_surface {
    uint32_t width;          /* render-size canvas width */
    uint32_t height;         /* render-size canvas height */
    uint32_t pitch_bytes;    /* aligned to 256 */
    uint64_t gpu_addr;       /* GPU-mapped direct-mem base of the surface pixels */
    uint8_t *cpu_base;       /* CPU VA for one-time cache flush / clear */
    SceAgcRegister *mrt;     /* colour-target registers, GPU-mapped (inside gpu_regs) */
    int      in_use;         /* 1 = acquired by CompositeLayers, 0 = free */
    int      pool_index;     /* 0..EVO_AGC_MAX_LAYERS-1, for debug tracking */
} evo_agc_layer_surface_t;

/* Where each [ResourceMapping] pointer goes in the user-SGPR block, copied
 * from the compiled pipeline's PAL metadata. Callers writing user data must
 * read these rather than hardcoding dword indices. */
typedef struct evo_agc_user_data_layout {
    uint32_t vs_count;
    uint32_t ps_count;
    int32_t  vs_const_table_dword;
    int32_t  vs_vertex_table_dword;
    /* The blur pipe's BlurConstants live in the fragment stage, so its const
     * table has a PS slot; ui/video pipes leave this at -1. */
    int32_t  ps_const_table_dword;
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

/* Block until every submitted command buffer has retired, or timeout_ms passes.
 * Used by shutdown, and by the soft close, which parks the app with the GPU
 * quiescent so the switcher can reap it without work in flight. */
void evo_agc_runtime_wait_idle(unsigned timeout_ms);
int  evo_agc_runtime_is_active(void);

void evo_agc_runtime_frame_begin(void);
void evo_agc_runtime_frame_end(void);
void evo_agc_runtime_present(void);
void evo_agc_runtime_set_player_mode(int is_player);
/* Mark the current backbuffer as carrying composited UI, so the next frame that
 * reuses it clears first instead of stacking OSD/subtitle pixels. */
void evo_agc_runtime_note_ui_drawn(void);

/* Whether the backbuffer about to be drawn already holds this video PTS. With
 * two scanout buffers, presenting without redrawing the quad shows the picture
 * from two presents ago; the render loop uses this to redraw a frame that is
 * slower than the panel into both buffers. Stamp with note_video_pts after the
 * blit, before the present. */
int  evo_agc_runtime_video_slot_stale(int64_t pts_us);
void evo_agc_runtime_note_video_pts(int64_t pts_us);

/* Fill a GPU-visible range with a 32-bit pattern using non-temporal stores, so
 * it needs no cache flush. Falls back to stores + clflush when the range is
 * not 16-byte aligned/sized. */
void evo_agc_runtime_stream_fill(void *dst, uint32_t value32, size_t bytes);

void evo_agc_runtime_bind_pipeline(int pipeline_id);
void evo_agc_runtime_set_scissor(int x, int y, int w, int h);
void evo_agc_runtime_set_blend(int blend_mode);

/* RmlUi clip masks, backed by the stencil buffer. Without these, border-radius
 * clipping and masked overlays silently do nothing - children of a rounded
 * container are not clipped to it. operation matches Rml::ClipMaskOperation:
 * 0 = Set, 1 = SetInverse, 2 = Intersect. Bracket the mask geometry with
 * _begin()/_end(); _set_clip_mask() toggles the test for normal drawing. */
/* 0 when the stencil clip-mask path is compiled out. Callers MUST skip drawing
 * the mask geometry entirely in that case: it is shape-only geometry that is
 * never meant to reach the colour buffer, so drawing it paints opaque
 * rectangles over the UI (RmlUi issues ~13 of them per frame). */
int  evo_agc_runtime_clip_mask_supported(void);
void evo_agc_runtime_set_clip_mask(int enable);
void evo_agc_runtime_clip_mask_begin(int operation);
void evo_agc_runtime_clip_mask_end(void);

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

/* Compiled draw modifier of a pipeline. The blur pass draws the backdrop quad
 * with sceAgcDcbDrawIndex + the pipeline's modifier so the VS reads
 * gl_VertexIndex from the index buffer; 0 when the pipeline is not valid. */
uint64_t evo_agc_runtime_get_pipe_draw_modifier(int pipeline_id);

SceAgcCommandBuffer      *evo_agc_runtime_get_current_cb(void);
evo_agc_transient_ring_t *evo_agc_runtime_get_transient_ring(void);
uint32_t                  evo_agc_runtime_get_current_slot(void);
void                      evo_agc_runtime_get_size(int *width, int *height);
int                       evo_agc_runtime_is_display_hdr(void);
int                       evo_agc_runtime_get_display_dynamic_range(void);

/* Layer surfaces for RmlUi PushLayer / CompositeLayers (backdrop-filter: blur).
 *
 * Each layer is a full-canvas RGBA8 render target, allocated from the direct
 * memory carve and built with standard COMP_SWAP (memory = R,G,B,A bytes)
 * so the C++ render interface can sample it back with the rgba8 T# swizzle.
 * The scanout backbuffer uses COMP_SWAP=ALT (memory = B,G,R,A bytes) and
 * therefore requires the bgra8 T# swizzle for sampling.  This distinction
 * is critical and silent-on-failure: getting it wrong produces visually
 * identical textures with swapped red and blue, not a crash.
 *
 * The render target is switched by evo_agc_set_layer_target(); the blur pipe
 * and the copy pass draw through it.  Layers are cleaned to transparent black
 * on acquire (memset the scissor region + clflush, not the full surface) and
 * released when CompositeLayers finishes.  Pool exhaustion returns NULL and
 * the C++ side degrades gracefully: the UI draws, but the backdrop blur
 * simply does not appear. */
int                       evo_agc_has_layers(void);
int                       evo_agc_layer_acquire(evo_agc_layer_surface_t **out);
void                      evo_agc_layer_release(evo_agc_layer_surface_t *layer);
/* NULL = switch to the scanout backbuffer (the default render target for UI
 * drawing).  Builds SceAgcRegister colour-target registers internally;
 * the caller must emit the write through the DCB.  Flushes the new MRT
 * register block so the GPU sees it even when the flush in frame_begin
 * already happened. */
int                       evo_agc_set_layer_target(const evo_agc_layer_surface_t *layer);
/* Fill a surface descriptor for the active scanout backbuffer, for use by
 * the blur pipeline's source texture (the base layer is always the scanout
 * in non-layer drawing, and must be sampled with the bgra8 T# swizzle). */
void                      evo_agc_get_scanout_layer(evo_agc_layer_surface_t *out);
/* CB colour-buffer flush (event 45).  Mandatory between H and V passes of
 * the blur and between any two same-frame passes that read-then-write the
 * same surface. */
void                      evo_agc_flush_color_target(void);

/* Composite a premultiplied 0xAABBGGRR OSD buffer over the current frame.
 * `upload` = "the buffer changed since last call"; when 0 the previous upload is
 * redrawn. Called via evo_gl_composite_bgra() so main.c stays backend-agnostic. */
void evo_agc_composite_bgra(const uint32_t *fb, int w, int h, int upload);

/* Draw the video quad. Returns 0 when the quad was emitted (and the current
 * backbuffer stamped with pts_us), -1 when the frame was rejected. */
int  evo_agc_blit_yuv(const uint8_t *y,  int y_pitch,
                      const uint8_t *uv, int uv_pitch,
                      const uint8_t *u,  int u_pitch,
                      const uint8_t *v,  int v_pitch,
                      int coded_w, int coded_h,
                      int disp_w, int disp_h,
                      int view_mode, int ten_bit, int color_trc,
                      int is_direct, int64_t pts_us);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AGC_RUNTIME_H */
