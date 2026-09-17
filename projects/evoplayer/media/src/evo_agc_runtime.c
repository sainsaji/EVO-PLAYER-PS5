#include "evo_agc_runtime.h"
#include "evo_agc_shader_header.h"
#include "evo_agc_pipes.h"
#include "evo_boot_log.h"
#include "evo_direct_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* access() for the diagnostic flag files */

#define EVO_AGC_LOG_PREFIX "[evo_agc] "

#define EVO_AGC_DIRECT_MEM_TYPE 12  /* SCE_KERNEL_WB_ONION */
#define EVO_AGC_MAP_PROTECTION  0x33 /* PROT_CPU_RW | PROT_GPU_RW */

/*
 * The scanout surface is TILED - 128x128 pixel tiles of 0x10000 bytes each -
 * and its base address must be 2MB aligned. ps5-opengl states both as a
 * build-time contract (src/platform/ps5_scanout.h):
 *
 *   #define PS5_SCANOUT_ALIGNMENT 0x200000u
 *   #define PS5_SCANOUT_TILED_BYTES \
 *      (((W + 127)/128) * ((H + 127)/128) * 0x10000u)
 *   #if PS5_SCANOUT_BYTES < PS5_SCANOUT_TILED_BYTES || ...ALIGNMENT != 0
 *   #error Invalid tiled display buffer size or alignment
 *
 * This pool was allocated with 0x20000 (128KB), which put the buffers at
 * 0x2_7a7e0000 - 0x1e0000 past a 2MB boundary. The tiling hardware derives
 * every tile's address from an aligned base, so a misaligned one shuffles the
 * image into a regular repeating pattern: the "railway track" artifact chased
 * across this whole investigation. 1920x1080 needs 15*9*0x10000 = 0x870000
 * tiled bytes, well inside the 64MB per-buffer stride below.
 */
#define EVO_AGC_DIRECT_MEM_ALIGN UINT64_C(0x200000)

/*
 * The composite / stencil / depth budgets below are sized for the largest
 * render size this runtime will drive, not for 1080p: the panel is rendered
 * at its own resolution. Hardware reports full=3840x2160 on the dev console,
 * so 4K is the cap and the budgets are cut for it:
 *
 *   composite BGRA  align(3840*4,256) * 2160 = 33.2 MB  -> 40 MB
 *   depth D32F      3840*2160*4       = 33.2 MB          -> 40 MB
 *   stencil S8      3840*2160         =  8.3 MB          -> 16 MB
 *
 * each with room for the 64KB tiled padding. The 64 MB scanout stride already
 * had the headroom: 4K tiled needs ceil(3840/128)*ceil(2160/128)*0x10000 =
 * 30*17*0x10000 = 0x1FE0000, just under 32 MB.
 */
#define EVO_AGC_MAX_RENDER_W 3840
#define EVO_AGC_MAX_RENDER_H 2160

#define EVO_AGC_SCANOUT_STRIDE      UINT64_C(0x04000000) /* 64 MB per scanout buffer */
#define EVO_AGC_SCANOUT_TOTAL       UINT64_C(0x08000000) /* 128 MB for 2 buffers */
#define EVO_AGC_TRANSIENT_RING_SIZE UINT64_C(0x04000000) /* 64 MB transient ring */
#define EVO_AGC_COMMAND_BUFFER_SIZE UINT64_C(0x00600000) /* 6 MB (2 MB per slot * 3) */
#define EVO_AGC_SHADER_STORAGE_SIZE UINT64_C(0x00400000) /* 4 MB shader storage */
#define EVO_AGC_FENCE_STORAGE_SIZE  UINT64_C(0x00010000) /* 64 KB fence storage */
/* Persistent 1920x1080 BGRA staging texture for the OSD composite, plus its
 * quad. main.c rasterises the OSD into gl_scratch and hands it to
 * evo_gl_composite_bgra(); in --agc builds that used to be a no-op stub, so the
 * OSD simply never reached the panel during playback. */
#define EVO_AGC_COMPOSITE_SIZE      UINT64_C(0x02800000) /* 40 MB */
/* Stencil buffer for RmlUi clip masks (border-radius clipping and masked
 * overlays). S8 at 1920x1080 is ~2 MB raw; 8 MB covers the 64KB_Z_X tiled
 * padding with room to spare. Depth is left disabled - nothing here needs a
 * Z test, only stencil. */
#define EVO_AGC_STENCIL_SIZE        UINT64_C(0x01000000) /* 16 MB */
/* D32F at 1920x1080 is ~8 MB raw; 16 MB covers the tiled padding. The depth
 * TEST is never enabled - the surface exists because configuring DB with an
 * invalid Z format stopped the stencil planes working. */
#define EVO_AGC_DEPTH_SIZE          UINT64_C(0x02800000) /* 40 MB */
/* Full-canvas RGBA8 layer surfaces for backdrop-filter / filter composition.
 * Sized for the maximum render size (4K): pitch-aligned 256, standard pitch =
 * 4K*4 = 15360 bytes. Each layer is 32 MB, covering 4K RGBA8 (31.64 MB) with
 * margin. Layers use COMP_SWAP=STD (memory = R,G,B,A) so they can be sampled
 * back via the rgba8 T#; only the scanout uses COMP_SWAP=ALT for BGRA. */
#define EVO_AGC_LAYER_BYTES         UINT64_C(0x02000000) /* 32 MB per layer */
#define EVO_AGC_LAYER_TOTAL         (EVO_AGC_MAX_LAYERS * EVO_AGC_LAYER_BYTES)

#define EVO_AGC_TOTAL_DIRECT_MEM \
    (EVO_AGC_SCANOUT_TOTAL + EVO_AGC_TRANSIENT_RING_SIZE + \
     EVO_AGC_COMMAND_BUFFER_SIZE + EVO_AGC_SHADER_STORAGE_SIZE + \
     EVO_AGC_FENCE_STORAGE_SIZE + EVO_AGC_COMPOSITE_SIZE + \
     EVO_AGC_STENCIL_SIZE + EVO_AGC_DEPTH_SIZE + \
     EVO_AGC_LAYER_TOTAL)

/*
 * VideoOut buffer attribute. 0x...22000000 is the TILED BGRA attribute and
 * 0x...00000000 is the linear SDR one - EVO's own hardware-verified VideoOut
 * code had both as PP_VO_ATTR_TILED_BGRA / PP_VO_ATTR_SDR_LINEAR (deleted with
 * pp_videoout.c in GL-6, still in git at c588037^).
 *
 * This path MUST register LINEAR. Everything writing these buffers writes
 * linearly - the CPU backdrop clear, and the GPU colour target set up by
 * setup_color_target() - so registering them tiled makes the display walk the
 * same bytes in tile order, which paints a regular repeating block/stripe
 * pattern instead of the picture. It was registering tiled (this constant was
 * the tiled value under an "SDR" name), which is what the "railway track"
 * artifact was, and why even a pure-CPU linear test pattern never appeared.
 * #27 hit the same wall from the other side and recorded it: the sceAgc
 * present path needs the linear attribute, the CPU-tiler attribute comes out
 * R<->B swapped / garbled (hw 2026-09-04).
 */
#define EVO_AGC_VIDEO_FORMAT_SDR UINT64_C(0x8000000000000000)
/* Untested: the HDR attribute still carries the 0x22000000 tiled bits, so it
 * will need the same treatment before --agc HDR is used (hdr is 0 today). */
#define EVO_AGC_VIDEO_FORMAT_HDR UINT64_C(0x8100070422000000)

/* Platform declarations */
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                      size_t bytes, size_t alignment,
                                      int memory_type, int64_t *offset);
int32_t sceKernelMapDirectMemory(void **address, size_t bytes, int protection,
                                 int flags, int64_t offset, size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);
int32_t sceKernelMunmap(void *address, size_t bytes);
int     sceKernelUsleep(unsigned int microseconds);

int32_t sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int32_t sceVideoOutClose(int32_t handle);
int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
/* Output-mode readback. ABI verified in third_party/ps5-opengl
 * (ps5_agc_native_runtime.c: runtime_resolution_status_t). */
typedef struct evo_vo_resolution_status {
    uint32_t full_width, full_height, pane_width, pane_height;
    uint64_t refresh_rate;
    float    screen_inches;
    uint32_t reserved[4];
} evo_vo_resolution_status;
int32_t sceVideoOutGetResolutionStatus(int32_t handle, evo_vo_resolution_status *status);

/* Display output status & dynamic range (SDR/HDR) readback. ABI verified in third_party/SharpProspero. */
typedef struct evo_vo_output_status {
    uint32_t resolution;
    uint32_t dynamic_range; /* 0 unknown, 1 SDR, 2 HDR */
    uint64_t refresh_rate;
    uint64_t flags;        /* bit 0: HDR output active */
    uint64_t reserved[3];
} evo_vo_output_status;
int32_t sceVideoOutGetOutputStatus(int32_t handle, evo_vo_output_status *status);
void    sceVideoOutSetBufferAttribute2(void *attribute, uint64_t format, uint32_t tiling,
                                       uint32_t width, uint32_t height, uint64_t option,
                                       uint32_t reserved0, uint64_t reserved1);
int32_t sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t start_index,
                                    void *buffers, int32_t count, void *attribute,
                                    int32_t option, void *reserved);
int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t set_index);
int32_t sceVideoOutWaitVblank(int32_t handle);
/* CPU-side flip - what pp_videoout.c used (hardware-verified) rather than
 * relying solely on the GPU's sceAgcDcbSetFlip packet. */
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index,
                              int32_t flip_mode, int64_t flip_arg);
int32_t sceVideoOutGetFlipStatus(int32_t handle, void *status);

typedef struct {
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} evo_video_buffer_t;

typedef struct {
    uint8_t reserved[80];
} evo_video_attribute_t;

typedef struct evo_agc_device {
    int                     initialized;
    int                     width;
    int                     height;
    int                     is_hdr;
    int                     display_is_hdr;
    int                     display_dynamic_range;
    uint32_t                display_resolution_token;
    int                     is_player_mode;
    /* Per-scanout-buffer: UI was composited into it, so it cannot be reused
     * without a clear even in player mode. See evo_agc_runtime_note_ui_drawn. */
    int                     ui_dirty[2];

    int32_t                 video_handle;
    int                     active_backbuffer;
    uint32_t                current_slot;
    uint64_t                frame_counter;
    int64_t                 flip_arg;

    int64_t                 direct_mem_offset;
    uint8_t                *direct_mem_base;
    size_t                  direct_mem_bytes;

    uint8_t                *scanout_buffers[2];
    evo_agc_transient_ring_t transient_ring;

    uint32_t               *dcb_slots[EVO_AGC_FRAME_SLOTS];
    uint32_t                dcb_slot_capacity_dwords;
    SceAgcCommandBuffer     current_cb;

    /* The GPU writes a 32-bit marker here at end-of-pipe (RELEASE_MEM event
     * 40). A slot is retired when its fence reads back the marker that slot's
     * last submit asked for - "wait for it to become 0" cannot work, because
     * the value written is whatever we pass to the packet. */
    volatile uint32_t      *fences[EVO_AGC_FRAME_SLOTS];
    uint32_t                fence_expect[EVO_AGC_FRAME_SLOTS];
    uint32_t                fence_marker;
    /* The token each slot's transient-ring region was sealed with. Reopening a
     * sealed slot requires handing that exact token back once the GPU is proven
     * done with it. */
    uint64_t                ring_token[EVO_AGC_FRAME_SLOTS];

    struct evo_agc_gpu_regs *gpu_regs;
    evo_agc_pipeline_t      pipelines[EVO_AGC_PIPE_COUNT];
    int                     bound_pipeline;

    uint16_t               *quad_indices;
    uint8_t                *composite_pixels;   /* 256-aligned, pitched */
    uint32_t                composite_pitch;
    void                   *composite_quad;     /* 4 verts, Rml::Vertex layout */
    uint8_t                *stencil_base;
    uint8_t                *depth_base;
    /* RmlUi layer surfaces (backdrop-filter / filter composition). Each is an
     * independent full-canvas RGBA8 target; `current_target` records which one
     * the DCB's MRT0 currently points at (NULL = scanout backbuffer), so
     * switching can skip redundant *RegistersIndirect packets. */
    evo_agc_layer_surface_t layers[EVO_AGC_MAX_LAYERS];
    const evo_agc_layer_surface_t *current_layer_target;
    /* Active scissor, mirrored from evo_agc_runtime_set_scissor. A pushed
     * layer is only required to be transparent-black inside it, so the
     * acquire clear uses this instead of wiping the whole canvas. */
    int scissor_x, scissor_y, scissor_w, scissor_h;
    /* RmlUi clip-mask state. Rather than clearing the stencil buffer before
     * every Set (a multi-MB fill per mask), each Set claims the next unused
     * value and the test compares against it. The buffer is zeroed once per
     * frame, so an unwritten texel can never collide with a live mask. */
    uint32_t                stencil_ref;        /* value the test compares to */
    uint32_t                stencil_counter;    /* last value handed out */
    int                     stencil_func_equal; /* 0 = NOTEQUAL (SetInverse) */
    int                     clip_mask_enabled;
    uint32_t                clip_mask_calls;    /* RenderToClipMask this frame */
    uint32_t                clip_enable_calls;  /* EnableClipMask this frame   */
    int                     frame_active;
    /* Whether any draw was emitted into the current frame's DCB. */
    int                     frame_has_draws;
    /* Health counters. Content silently vanishing as a frame gets busier looks
     * like "a black area creeping across the screen while things load", so each
     * way a draw can be dropped is counted rather than ignored. */
    uint32_t                ring_alloc_fail;
    uint32_t                tex_alloc_fail;
    uint32_t                dcb_peak_dwords;
    /* Smallest DCB actually presented in the current reporting window. A frame
     * that clears the whole back buffer and then draws only a fraction of the
     * UI flips a mostly-black screen - which is what a "black flash" is. */
    uint32_t                dcb_min_presented;
    uint32_t                presents;
    uint32_t                flip_waits;
    uint32_t                flip_timeouts;
} evo_agc_device_t;

typedef struct evo_agc_gpu_regs {
    SceAgcRegister color_targets[2][16];
    SceAgcRegister layer_targets[EVO_AGC_MAX_LAYERS][16];
    SceAgcRegister depth_target[16];
    struct {
        SceAgcRegister cx_regs[128];
        SceAgcRegister sh_regs[32];
        SceAgcRegister uc_regs[8];
    } pipes[EVO_AGC_PIPE_COUNT];
} evo_agc_gpu_regs_t;

static evo_agc_device_t g_agc_dev = {0};

/*
 * Present path: no GPU SetFlip in the DCB; the CPU calls sceVideoOutSubmitFlip
 * once the frame's end-of-pipe fence retires.
 *
 * This was picked over the GPU-side flip because it is the one that is actually
 * proven on hardware here, and because the fence it waits on is what turned a
 * blank screen into a per-frame yes/no signal: a DCB with no draws retired in
 * ~0ms while the first DCB containing draws never retired at all, which is what
 * localised the GPU wedge to the draw. Revisit the in-DCB flip once the picture
 * is correct; it saves a CPU round trip per frame.
 */

/*
 * Read back what is ACTUALLY in the scanout after a frame, instead of reasoning
 * about registers. Logs a coarse luminance thumbnail plus how many pixels
 * differ from the backdrop clear. This is what turned "blank screen" into a
 * fact: changed=0 proved no fragment was reaching the colour target at all,
 * which killed the tiling theory and localised the bug to the draw.
 *
 *   all blank, changed==0  -> draws are not reaching the colour target
 *   recognisable layout    -> GPU drew correctly; the panel is descrambling it
 *   changed>0, no structure-> GPU wrote, but to the wrong addresses
 *
 * Runs once, on memory the GPU has already fenced as complete.
 */
static void agc_dump_scanout(const uint32_t *buf, int w, int h)
{
    static const char ramp[] = " .:-=+*#%@";
    const uint32_t backdrop = 0xff100d0du;
    size_t changed = 0;

    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i)
        if (buf[i] != backdrop)
            ++changed;

    evo_boot_log("agc scanout dump %dx%d changed=%zu (%.2f%%) px0=%08x px_mid=%08x",
                 w, h, changed, 100.0 * (double)changed / ((double)w * (double)h),
                 buf[0], buf[((size_t)h / 2) * (size_t)w + (size_t)w / 2]);

    /*
     * The most common non-backdrop colours actually sitting in the scanout.
     * Compare these against the RCSS palette (#38bdf8 sky blue, #2a3b55 navy,
     * #121b2e near-black navy): if the buffer holds 0x..38bdf8 the GPU wrote
     * the right bytes and the DISPLAY is reordering them; if it holds
     * 0x..f8bd38 the swap happened before the scanout. Guessing channel order
     * from screenshots has cost two builds - this settles which side it is on.
     */
    struct { uint32_t colour; uint32_t count; } top[6] = {{0, 0}};
    for (size_t i = 0; i < (size_t)w * (size_t)h; i += 16) {
        const uint32_t c = buf[i];
        if (c == backdrop)
            continue;
        int slot = -1;
        for (int k = 0; k < 6; ++k) {
            if (top[k].count && top[k].colour == c) { slot = k; break; }
            if (!top[k].count && slot < 0) slot = k;
        }
        if (slot < 0) continue;
        top[slot].colour = c;
        top[slot].count++;
    }
    for (int k = 0; k < 6; ++k)
        if (top[k].count)
            evo_boot_log("agc scanout top[%d] %08x n=%u", k, top[k].colour,
                         top[k].count);

    for (int cy = 0; cy < 18; ++cy) {
        char row[40];
        for (int cx = 0; cx < 32; ++cx) {
            const int x = (cx * w) / 32 + w / 64;
            const int y = (cy * h) / 18 + h / 36;
            const uint32_t p = buf[(size_t)y * (size_t)w + (size_t)x];
            const unsigned lum = (((p >> 16) & 0xff) * 77u +
                                  ((p >> 8) & 0xff) * 151u +
                                  (p & 0xff) * 28u) >> 8;
            row[cx] = ramp[(lum * 9u) / 255u];
        }
        row[32] = 0;
        evo_boot_log("agc scan[%02d] |%s|", cy, row);
    }
}

void evo_agc_runtime_cache_flush(const void *address, size_t bytes)
{
    if (!address || !bytes)
        return;
    /* Walk whole 64B cache lines: round the start down and the end up so a
     * range that doesn't happen to start/end on a line boundary still gets
     * every line it touches evicted. */
    const uint8_t *at  = (const uint8_t *)((uintptr_t)address & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)address + bytes;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static SceAgcRegister *alloc_transient_cx(uint32_t count)
{
    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(&g_agc_dev.transient_ring,
                                     g_agc_dev.current_slot,
                                     count * sizeof(SceAgcRegister),
                                     16, &slice) != EVO_AGC_TRANSIENT_OK)
        return NULL;
    return (SceAgcRegister *)slice.cpu;
}

/* -------------------------------------------------------------------------
 * Pipeline construction
 *
 * The opengnm-psbc "package" path that used to live here - an ELF carrying
 * .shader_header / .shader_text, parsed by extract_shader_sections() and
 * validate_shader_header() - is gone along with the hand-written build wrapper
 * that produced it. Shaders now come from tools/build_agc_pipes.py: amdllpc
 * compiles a .pipe whose [ResourceMapping] declares the user-data layout next
 * to the shader source, and every AGC register is DERIVED from the PAL metadata
 * in the resulting ELF rather than chosen by a wrapper's command line.
 *
 * That wrapper shipped two silent, hardware-only defects this replaces:
 * VGT_ESGS_RING_ITEMSIZE packaged as 4 instead of 1 (which wedged the GPU on
 * the first DCB containing a real draw), and a vertex stage compiled with no
 * descriptor binding for its own uniform block (which drew zero fragments while
 * faulting nothing). Neither is expressible in the .pipe form.
 * ------------------------------------------------------------------------- */

static int compile_agc_pipeline(evo_agc_pipeline_t *pipe, uint8_t *storage_base,
                                size_t *storage_used,
                                const evo_agc_shader_metadata_t *meta,
                                const char *name)
{
    pipe->vs_shader = NULL;
    pipe->ps_shader = NULL;
    pipe->cx_reg_count = 0;
    pipe->sh_reg_count = 0;
    pipe->uc_reg_count = 0;
    pipe->draw_modifier = 0;
    pipe->valid = 0;

    if (!meta || !meta->gs_isa || !meta->ps_isa)
        return -1;

    const uint32_t gs_bytes = meta->gs_isa_bytes + EVO_AGC_SHADER_FOOTER_BYTES;
    const uint32_t ps_bytes = meta->ps_isa_bytes + EVO_AGC_SHADER_FOOTER_BYTES;

    /* Everything below lives in direct memory the GPU reads; the whole region
     * is cache-flushed once at the end of evo_agc_runtime_init(). */
    size_t at = (*storage_used + 0x3fffu) & ~(size_t)0x3fff;
    evo_agc_shader_arena_t *gs_arena = (evo_agc_shader_arena_t *)(storage_base + at);
    at = (at + sizeof(*gs_arena) + 0xffu) & ~(size_t)0xff;
    evo_agc_shader_arena_t *ps_arena = (evo_agc_shader_arena_t *)(storage_base + at);
    at = (at + sizeof(*ps_arena) + 0xffu) & ~(size_t)0xff;

    uint8_t *gs_code = storage_base + at;
    at = (at + gs_bytes + 0xffu) & ~(size_t)0xff;
    uint8_t *ps_code = storage_base + at;
    at = (at + ps_bytes + 0xffu) & ~(size_t)0xff;

    uint8_t *linked_cx = storage_base + at;
    at = (at + 0x1000u + 0xffu) & ~(size_t)0xff;
    uint8_t *linked_uc = storage_base + at;
    at = (at + 0x1000u + 0xffu) & ~(size_t)0xff;
    *storage_used = at;

    if (evo_agc_shader_header_build(gs_arena, EVO_AGC_SHADER_PRE_RASTER,
                                    gs_bytes, meta) != 0 ||
        evo_agc_shader_header_build(ps_arena, EVO_AGC_SHADER_PIXEL,
                                    ps_bytes, meta) != 0) {
        evo_boot_log("agc pipe %s: header build failed", name);
        return -2;
    }
    evo_agc_shader_write_code(gs_code, meta->gs_isa, meta->gs_isa_bytes);
    evo_agc_shader_write_code(ps_code, meta->ps_isa, meta->ps_isa_bytes);

    /* sceAgcCreateShader resolves the arena's self-relative pointer fields in
     * place and hands the arena back as the object, so a returned pointer that
     * is not the arena means the header was rejected. */
    int ret = sceAgcCreateShader(&pipe->vs_shader, gs_arena, gs_code);
    if (ret != 0 || pipe->vs_shader != gs_arena) {
        evo_boot_log("agc pipe %s: CreateShader(GS) rc=%d obj=%p arena=%p",
                     name, ret, pipe->vs_shader, (void *)gs_arena);
        return -3;
    }
    ret = sceAgcCreateShader(&pipe->ps_shader, ps_arena, ps_code);
    if (ret != 0 || pipe->ps_shader != ps_arena) {
        evo_boot_log("agc pipe %s: CreateShader(PS) rc=%d obj=%p arena=%p",
                     name, ret, pipe->ps_shader, (void *)ps_arena);
        return -4;
    }

    /* 4 = triangle list. */
    ret = sceAgcLinkShaders(linked_cx, linked_uc, NULL,
                            pipe->vs_shader, pipe->ps_shader, 4);
    if (ret != 0) {
        evo_boot_log("agc pipe %s: LinkShaders rc=%d", name, ret);
        return -5;
    }

    /* Register plan order matches ps5-opengl's append_shader_state and
     * ps5-xash3d's ps5_pipeline_build: the linker's 34 context registers, then
     * the pre-raster stage's 10, then the pixel stage's 9. Read straight out of
     * our own arenas rather than chasing pointers inside the shader object. */
    uint32_t cx_total = 0;
    memcpy(pipe->cx_regs, linked_cx, 34u * sizeof(SceAgcRegister));
    cx_total = 34u;
    memcpy(pipe->cx_regs + cx_total, gs_arena->cx,
           meta->pre_raster_cx_count * sizeof(SceAgcRegister));
    cx_total += meta->pre_raster_cx_count;
    memcpy(pipe->cx_regs + cx_total, ps_arena->cx,
           meta->pixel_cx_count * sizeof(SceAgcRegister));
    cx_total += meta->pixel_cx_count;
    /* SPI_PS_INPUT_CNTL_0..N - the parameter slot and flat/interpolated mode
     * for each pixel-stage input. These cannot go in the arena (its CX array
     * is a fixed 9 for the pixel stage), so they are appended here. */
    if (meta->ps_input_cntl && meta->ps_input_cntl_count &&
        cx_total + meta->ps_input_cntl_count <= 128u) {
        memcpy(pipe->cx_regs + cx_total, meta->ps_input_cntl,
               meta->ps_input_cntl_count * sizeof(SceAgcRegister));
        cx_total += meta->ps_input_cntl_count;
    }
    pipe->cx_reg_count = cx_total;

    memcpy(pipe->sh_regs, gs_arena->sh, 6u * sizeof(SceAgcRegister));
    memcpy(pipe->sh_regs + 6, ps_arena->sh, 6u * sizeof(SceAgcRegister));
    pipe->sh_reg_count = 12u;

    memcpy(pipe->uc_regs, linked_uc, 3u * sizeof(SceAgcRegister));
    pipe->uc_reg_count = 3u;

    pipe->draw_modifier = meta->draw_modifier;
    pipe->user_data = (evo_agc_user_data_layout_t){
        .vs_count = meta->vs_user_sgpr_count,
        .ps_count = meta->ps_user_sgpr_count,
        .vs_const_table_dword = meta->vs_const_table_dword,
        .vs_vertex_table_dword = meta->vs_vertex_table_dword,
        .ps_const_table_dword = meta->ps_const_table_dword,
        .ps_texture_table_dword = meta->ps_texture_table_dword,
    };
    pipe->valid = 1;

    /* The VS-out/PS-in linkage, logged the way ps5-opengl logs it under
     * PSBC_DEBUG_IO. gs_pgm/ps_pgm must be non-zero: those are the shader entry
     * addresses sceAgcCreateShader filled in. */
    evo_boot_log("agc pipe %s cx=%u sh=%u uc=%u modifier=%#llx "
                 "gs_pgm=%08x:%08x ps_pgm=%08x:%08x",
                 name, pipe->cx_reg_count, pipe->sh_reg_count,
                 pipe->uc_reg_count, (unsigned long long)pipe->draw_modifier,
                 gs_arena->sh[4].value, gs_arena->sh[5].value,
                 ps_arena->sh[2].value, ps_arena->sh[3].value);
    evo_boot_log("agc pipe %s user_data vs_n=%u ps_n=%u const=%d vtx=%d ps_const=%d tex=%d",
                 name, meta->vs_user_sgpr_count, meta->ps_user_sgpr_count,
                 meta->vs_const_table_dword, meta->vs_vertex_table_dword,
                 meta->ps_const_table_dword, meta->ps_texture_table_dword);
    return 0;
}

/* -------------------------------------------------------------------------
 * Target & Surface Defaults Setup
 * ------------------------------------------------------------------------- */

static int setup_color_target(SceAgcRegister out[16], void *defaults, void *target_addr,
                              uint32_t width, uint32_t height, int comp_swap_alt)
{
    static const uint16_t target_offsets[16] = {
        0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
        0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8
    };

    SceAgcRegister **blocks = *(SceAgcRegister ***)defaults;
    uint32_t default_count = *(uint32_t *)((uint8_t *)defaults + 0x20);

    if (!blocks || !blocks[0])
        return -1;

    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t candidate;
        out[i] = (SceAgcRegister){target_offsets[i], 0};
        for (candidate = 0; candidate < default_count; ++candidate) {
            if (blocks[0][candidate].offset == target_offsets[i]) {
                out[i].value = blocks[0][candidate].value;
                break;
            }
        }
        if (candidate == default_count)
            return -1;
    }

    uintptr_t target = (uintptr_t)target_addr;
    out[0].value = (uint32_t)(target >> 8);
    out[1].value &= 0xfc001fffu;
    /*
     * CB_COLOR0_INFO. FORMAT=COLOR_8_8_8_8 (0x28), plus COMP_SWAP=ALT in bits
     * [12:11] when comp_swap_alt is set, so the colour block stores B,G,R,A
     * instead of R,G,B,A.
     *
     * The scanout really is BGRA-ordered. Measured, not assumed: with COMP_SWAP
     * left at STD the framebuffer held ff160a05 and ffedbe00 - correct RGBA for
     * the theme's #121b2e navy and #00cdff cyan - while the panel displayed
     * those same pixels as brown and gold. So the GPU was writing the right
     * bytes and the display was reading them in the other order.
     *
     * ps5-opengl gets away with COMP_SWAP=STD because Mesa bakes the swizzle
     * into the shader for a BGRA pipe format; doing it here keeps the shader
     * exporting plain RGBA, which is what the .pipe and the texture descriptors
     * already agree on.
     *
     * Layer surfaces pass comp_swap_alt=0 and keep COMP_SWAP=STD: their memory
     * order (R,G,B,A) must match the rgba8 T# swizzle the blur/composite
     * shaders sample them with. Only the scanout backbuffer is BGRA (and is
     * sampled with the bgra8 swizzle instead).
     */
    out[2].value = (out[2].value &
                    ~(0x7cu | 0x700u | 0x1800u | 0x10000000u |
                      0x10000u | 0x8000u | 0x40000u | 0x4000u)) |
                   0x28u | 0x8000u | (comp_swap_alt ? 0x800u /* COMP_SWAP = ALT (BGRA) */ : 0u);
    out[3].value &= ~(0x7000u | 0x18000u);
    out[4].value = (out[4].value &
                    ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) |
                   0x48u;
    out[5].value = out[6].value = out[9].value = 0;
    out[10].value = (out[10].value & 0xffffff00u) | (uint32_t)(target >> 40);
    out[11].value &= 0xffffff00u;
    out[12].value &= 0xffffff00u;
    out[13].value &= 0xffffff00u;
    out[14].value = (height - 1u) | ((width - 1u) << 14);
    out[15].value = (out[15].value &
                     ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) |
                    0x6c000u | 0x01000000u | 0x44000000u;
    return 0;
}

/* -------------------------------------------------------------------------
 * Stencil / clip mask
 *
 * RmlUi clips to non-rectangular shapes - rounded-corner containers and masked
 * overlays - through EnableClipMask()/RenderToClipMask(). The AGC interface
 * implemented neither, so those calls hit base-class no-ops: rounded containers
 * drew rounded but their children were never clipped to them (square thumbnail
 * corners), and gradient masks covered the wrong region. Anything rectangular
 * still worked, because that goes through the scissor instead.
 *
 * Register layout follows ps5-opengl's append_depth_target_state().
 * ------------------------------------------------------------------------- */

enum {
    /* DB_DEPTH_CONTROL (0x200) */
    DB_STENCIL_ENABLE   = 1u << 0,
    DB_Z_ENABLE         = 1u << 1,      /* must be on even for stencil-only */
    DB_ZFUNC_SHIFT      = 4,            /* bits [6:4] */
    DB_STENCILFUNC_SHIFT = 8,           /* bits [10:8] */
    /* Compare functions */
    DB_CMP_NEVER = 0, DB_CMP_EQUAL = 2, DB_CMP_NOTEQUAL = 5, DB_CMP_ALWAYS = 7,
    /* Stencil ops, DB_STENCIL_CONTROL (0x10b) */
    DB_STENCIL_KEEP = 0, DB_STENCIL_REPLACE = 2, DB_STENCIL_INCR_CLAMP = 3,
};

static int setup_depth_target(SceAgcRegister out[16], const uint8_t *depth,
                              const uint8_t *stencil,
                              uint32_t width, uint32_t height)
{
    static const uint16_t db_offsets[16] = {
        0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x001a, 0x001b,
        0x001c, 0x001d, 0x001e, 0x0002, 0x0005, 0x0007, 0x000b, 0x000a,
    };
    const uintptr_t z = (uintptr_t)depth;
    const uintptr_t s = (uintptr_t)stencil;
    for (uint32_t i = 0; i < 16; ++i)
        out[i] = (SceAgcRegister){db_offsets[i], 0u};

    /* Exactly ps5-opengl's values. An earlier revision set FORMAT=0 here to
     * avoid allocating a depth surface, reasoning that only stencil was needed;
     * the stencil planes then never went live and every clip mask tested as
     * empty, so content inside a rounded container vanished. The depth surface
     * is allocated and configured even though the depth TEST stays off. */
    out[0].value = 0x80000183u;                 /* DB_Z_INFO: D32F, 64KB_Z_X   */
    out[1].value = 0x20000181u;                 /* DB_STENCIL_INFO: S8, 64KB_Z_X */
    out[2].value = (uint32_t)(z >> 8);          /* Z_READ_BASE        */
    out[3].value = (uint32_t)(s >> 8);          /* STENCIL_READ_BASE  */
    out[4].value = (uint32_t)(z >> 8);          /* Z_WRITE_BASE       */
    out[5].value = (uint32_t)(s >> 8);          /* STENCIL_WRITE_BASE */
    out[6].value = (uint32_t)(z >> 40);         /* Z_READ_BASE_HI     */
    out[7].value = (uint32_t)(s >> 40);         /* STENCIL_READ_BASE_HI  */
    out[8].value = (uint32_t)(z >> 40);         /* Z_WRITE_BASE_HI    */
    out[9].value = (uint32_t)(s >> 40);         /* STENCIL_WRITE_BASE_HI */
    out[13].value = (width - 1u) | ((height - 1u) << 16);
    return 0;
}

/* Emit the four stencil state registers for the current mask state. */
static void emit_stencil_state(int stencil_enable, uint32_t func, uint32_t ref,
                               uint32_t zpass_op, int colour_writes)
{
    SceAgcRegister *r = alloc_transient_cx(5);
    if (!r)
        return;
    /* Z_ENABLE with ZFUNC=ALWAYS and Z_WRITE_ENABLE off: the depth test always
     * passes and nothing is written, but the depth block stays active. Leaving
     * Z_ENABLE clear bypasses DB entirely, so the stencil op never executes and
     * every mask reads back empty - three earlier attempts at this bug were
     * looking at the stencil registers when the depth enable was the problem. */
    r[0] = (SceAgcRegister){0x0200, stencil_enable
                                ? (DB_STENCIL_ENABLE | DB_Z_ENABLE |
                                   ((uint32_t)DB_CMP_ALWAYS << DB_ZFUNC_SHIFT) |
                                   (func << DB_STENCILFUNC_SHIFT))
                                : 0u};
    r[1] = (SceAgcRegister){0x010b, (uint32_t)DB_STENCIL_KEEP |
                                    (zpass_op << 4) |
                                    ((uint32_t)DB_STENCIL_KEEP << 8)};
    /* ref | read mask | write mask | STENCILOPVAL.
     * REPLACE writes STENCILTESTVAL, but INCR/DECR step by STENCILOPVAL in bits
     * [31:24]; leaving it 0 made Intersect increment by nothing. */
    const uint32_t refmask = (ref & 0xffu) | (0xffu << 8) | (0xffu << 16) |
                             (1u << 24);
    r[2] = (SceAgcRegister){0x010c, refmask};
    r[3] = (SceAgcRegister){0x010d, refmask};
    /* CB_TARGET_MASK: writing the mask must not touch colour. */
    r[4] = (SceAgcRegister){0x008e, colour_writes ? 0x0000000fu : 0u};
    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, r, 5);
}

/*
 * Clip masks are OFF by default.
 *
 * The stencil path is written and the DB registers demonstrably bind
 * (z_info=0x80000183 s_info=0x20000181 in the boot log), but on hardware every
 * mask tests as empty: content drawn inside a rounded container disappears
 * while everything outside one renders normally. Until that is understood, the
 * feature fails OPEN - square corners with visible content beats rounded
 * corners with none.
 *
 * Modes: 0 = off entirely. 1 = full (write + test). 2 = DIAGNOSTIC - write the
 * masks but never enable the test, so the UI is identical to mode 0 while the
 * stencil buffer still gets written. Mode 2 plus the read-back below answers
 * "do the mask writes land at all?" without shipping a broken screen.
 *
 * Build with -DEVO_AGC_CLIP_MASK=1 to turn it fully on for debugging. The
 * counters below are logged either way; they show RmlUi issuing ~13 masks per
 * frame, so this is worth finishing.
 *
 * Ruled out by hardware, do not retry:
 *   1. DB_Z_INFO FORMAT=0 with no depth surface  - allocate D32F, use
 *      0x80000183 exactly as ps5-opengl does.
 *   2. STENCILOPVAL left 0 (bits [31:24] of DB_STENCILREFMASK) - breaks
 *      INCR/Intersect, but not plain Set.
 *   3. DB_DEPTH_CONTROL with Z_ENABLE clear - tried both with and without;
 *      neither makes the mask test pass.
 *   4. Clearing only width*height bytes of a tiled S8 surface instead of its
 *      full ~2.6 MB footprint.
 * All four are fixed in the code below and the masks still test empty, so the
 * fault is elsewhere. The next step is NOT another register guess: read the
 * stencil buffer back after a masked frame (as agc_dump_scanout does for
 * colour) and count non-zero bytes. Zero means the mask WRITE never happens and
 * the fault is in the DB/write path; non-zero means writes land and the
 * comparison is wrong. That splits the problem in one launch.
 */
#ifndef EVO_AGC_CLIP_MASK
#define EVO_AGC_CLIP_MASK 0
#endif

int evo_agc_runtime_clip_mask_supported(void)
{
    return EVO_AGC_CLIP_MASK;
}

void evo_agc_runtime_set_clip_mask(int enable)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    g_agc_dev.clip_enable_calls++;
    if (EVO_AGC_CLIP_MASK != 1)
        return;          /* mode 2 writes masks but never tests against them */
    g_agc_dev.clip_mask_enabled = enable ? 1 : 0;
    emit_stencil_state(g_agc_dev.clip_mask_enabled,
                       g_agc_dev.stencil_func_equal ? DB_CMP_EQUAL : DB_CMP_NOTEQUAL,
                       g_agc_dev.stencil_ref, DB_STENCIL_KEEP, 1);
}

void evo_agc_runtime_clip_mask_begin(int operation)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    g_agc_dev.clip_mask_calls++;
    if (!EVO_AGC_CLIP_MASK)
        return;
    uint32_t zpass;
    switch (operation) {
    case 0:  /* Set - claim a fresh value and stamp it where the geometry covers */
    case 1:  /* SetInverse - same stamp, but the test is inverted afterwards */
        if (g_agc_dev.stencil_counter >= 255u)
            g_agc_dev.stencil_counter = 0u;   /* 255 masks/frame is not a real case */
        g_agc_dev.stencil_ref = ++g_agc_dev.stencil_counter;
        g_agc_dev.stencil_func_equal = (operation == 0);
        zpass = DB_STENCIL_REPLACE;
        break;
    default: /* Intersect - increment, so only texels already carrying the
              * previous value reach ref+1 */
        g_agc_dev.stencil_ref += 1u;
        g_agc_dev.stencil_func_equal = 1;
        zpass = DB_STENCIL_INCR_CLAMP;
        break;
    }
    /* Always pass the test while writing the mask; colour writes off. */
    emit_stencil_state(1, DB_CMP_ALWAYS, g_agc_dev.stencil_ref, zpass, 0);
}

void evo_agc_runtime_clip_mask_end(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    if (EVO_AGC_CLIP_MASK != 1) {
        /* Diagnostic mode: stop writing the mask and restore colour writes, but
         * leave the stencil test disabled so drawing is unaffected. */
        emit_stencil_state(0, DB_CMP_ALWAYS, 0u, DB_STENCIL_KEEP, 1);
        return;
    }
    g_agc_dev.clip_mask_enabled = 1;
    emit_stencil_state(1, g_agc_dev.stencil_func_equal ? DB_CMP_EQUAL : DB_CMP_NOTEQUAL,
                       g_agc_dev.stencil_ref, DB_STENCIL_KEEP, 1);
}

/* -------------------------------------------------------------------------
 * Public AGC Runtime Lifecycle
 * ------------------------------------------------------------------------- */

/*
 * Bind a render size to the surfaces. Split out of evo_agc_runtime_init so it
 * can run twice: once with the size the caller asked for, then again once the
 * VideoOut handle exists and the panel has told us what it is actually
 * running at. Only touches CPU-side register images and offsets into memory
 * that is already carved, so the second call is free of any sce* ordering -
 * in particular it never re-opens VideoOut or allocates a queue.
 */
static int evo_agc_apply_render_size(void *agc_defaults, int w, int h)
{
    g_agc_dev.width  = w;
    g_agc_dev.height = h;

    g_agc_dev.composite_pitch = ((uint32_t)w * 4u + 255u) & ~255u;
    g_agc_dev.composite_quad  = g_agc_dev.composite_pixels +
        ((((size_t)g_agc_dev.composite_pitch * (size_t)h) + 255u) & ~(size_t)255);

    int ct0 = setup_color_target(g_agc_dev.gpu_regs->color_targets[0], agc_defaults,
                                 g_agc_dev.scanout_buffers[0], w, h, 1);
    int ct1 = setup_color_target(g_agc_dev.gpu_regs->color_targets[1], agc_defaults,
                                 g_agc_dev.scanout_buffers[1], w, h, 1);
    setup_depth_target(g_agc_dev.gpu_regs->depth_target, g_agc_dev.depth_base,
                       g_agc_dev.stencil_base, (uint32_t)w, (uint32_t)h);

    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        /* COMP_SWAP stays STD for layers: their R,G,B,A memory order must match
         * the rgba8 T# they are sampled back through. */
        if (setup_color_target(layer->mrt, agc_defaults, layer->cpu_base, w, h, 0) != 0) {
            printf(EVO_AGC_LOG_PREFIX "setup layer target %d failed\n", i);
            return -1;
        }
        layer->width = (uint32_t)w;
        layer->height = (uint32_t)h;
        layer->pitch_bytes = ((uint32_t)w * 4u + 255u) & ~255u;
    }
    evo_agc_runtime_cache_flush(g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));

    if (ct0 != 0 || ct1 != 0) {
        printf(EVO_AGC_LOG_PREFIX "setup_color_target failed: %d/%d\n", ct0, ct1);
        return -1;
    }
    return 0;
}

int evo_agc_runtime_init(int width, int height, int hdr)
{
    if (g_agc_dev.initialized)
        return 0;

    printf(EVO_AGC_LOG_PREFIX "Initializing Bare-Metal AGC (%dx%d, HDR=%d)...\n",
           width, height, hdr);

    memset(&g_agc_dev, 0, sizeof(g_agc_dev));
    g_agc_dev.width = width ? width : 1920;
    g_agc_dev.height = height ? height : 1080;
    g_agc_dev.is_hdr = hdr;
    g_agc_dev.video_handle = -1;

    /* 1. Allocate Direct Memory Pool */
    int ret = sceKernelAllocateDirectMemory(
        0, (off_t)16 * 1024 * 1024 * 1024ULL, EVO_AGC_TOTAL_DIRECT_MEM,
        EVO_AGC_DIRECT_MEM_ALIGN, EVO_AGC_DIRECT_MEM_TYPE, &g_agc_dev.direct_mem_offset);
    if (ret != 0 || g_agc_dev.direct_mem_offset < 0) {
        printf(EVO_AGC_LOG_PREFIX "Failed to allocate direct memory: %d\n", ret);
        return -1;
    }

    void *mapped = NULL;
    ret = sceKernelMapDirectMemory(&mapped, EVO_AGC_TOTAL_DIRECT_MEM,
                                   EVO_AGC_MAP_PROTECTION, 0,
                                   g_agc_dev.direct_mem_offset, EVO_AGC_DIRECT_MEM_ALIGN);
    if (ret != 0 || !mapped) {
        printf(EVO_AGC_LOG_PREFIX "Failed to map direct memory: %d\n", ret);
        sceKernelReleaseDirectMemory(g_agc_dev.direct_mem_offset, EVO_AGC_TOTAL_DIRECT_MEM);
        return -2;
    }

    g_agc_dev.direct_mem_base = (uint8_t *)mapped;
    g_agc_dev.direct_mem_bytes = EVO_AGC_TOTAL_DIRECT_MEM;
    memset(g_agc_dev.direct_mem_base, 0, g_agc_dev.direct_mem_bytes);

    /* 2. Subdivide Direct Memory */
    size_t cur_offset = 0;

    /* Scanout buffer 0 & 1 */
    g_agc_dev.scanout_buffers[0] = g_agc_dev.direct_mem_base + cur_offset;
    cur_offset += EVO_AGC_SCANOUT_STRIDE;
    g_agc_dev.scanout_buffers[1] = g_agc_dev.direct_mem_base + cur_offset;
    cur_offset += EVO_AGC_SCANOUT_STRIDE;

    /* Transient ring buffer (16 MB) */
    uint8_t *transient_base = g_agc_dev.direct_mem_base + cur_offset;
    evo_agc_transient_ring_init(&g_agc_dev.transient_ring, transient_base,
                                (uint64_t)(uintptr_t)transient_base,
                                EVO_AGC_TRANSIENT_RING_SIZE,
                                EVO_AGC_FRAME_SLOTS, 256);
    cur_offset += EVO_AGC_TRANSIENT_RING_SIZE;

    /* Command buffers (3 slots, 2 MB each = 6 MB) */
    const size_t dcb_slot_bytes = 2 * 1024 * 1024;
    g_agc_dev.dcb_slot_capacity_dwords = (uint32_t)(dcb_slot_bytes / sizeof(uint32_t));
    for (int i = 0; i < EVO_AGC_FRAME_SLOTS; ++i) {
        g_agc_dev.dcb_slots[i] = (uint32_t *)(g_agc_dev.direct_mem_base + cur_offset);
        cur_offset += dcb_slot_bytes;
    }

    /* Fence markers */
    uint8_t *fence_base = g_agc_dev.direct_mem_base + cur_offset;
    for (int i = 0; i < EVO_AGC_FRAME_SLOTS; ++i) {
        g_agc_dev.fences[i] = (volatile uint32_t *)(fence_base + i * 256);
        *g_agc_dev.fences[i] = 0;
        g_agc_dev.fence_expect[i] = 0; /* 0 = never submitted, nothing to wait for */
    }
    g_agc_dev.fence_marker = 0;
    cur_offset += EVO_AGC_FENCE_STORAGE_SIZE;

    /* GPU-mapped registers for color targets and pipeline states */
    g_agc_dev.gpu_regs = (evo_agc_gpu_regs_t *)(g_agc_dev.direct_mem_base + cur_offset);
    cur_offset += (sizeof(evo_agc_gpu_regs_t) + 255u) & ~255u;
    memset(g_agc_dev.gpu_regs, 0, sizeof(evo_agc_gpu_regs_t));

    for (int i = 0; i < EVO_AGC_PIPE_COUNT; ++i) {
        g_agc_dev.pipelines[i].cx_regs = g_agc_dev.gpu_regs->pipes[i].cx_regs;
        g_agc_dev.pipelines[i].sh_regs = g_agc_dev.gpu_regs->pipes[i].sh_regs;
        g_agc_dev.pipelines[i].uc_regs = g_agc_dev.gpu_regs->pipes[i].uc_regs;
    }

    /* OSD composite staging texture + its quad. 256-byte aligned because a
     * GFX10 image descriptor stores address>>8. */
    {
        uint8_t *comp = g_agc_dev.direct_mem_base + cur_offset;
        comp = (uint8_t *)(((uintptr_t)comp + 255u) & ~(uintptr_t)255);
        g_agc_dev.composite_pitch =
            ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
        g_agc_dev.composite_pixels = comp;
        g_agc_dev.composite_quad = comp +
            ((size_t)g_agc_dev.composite_pitch * (size_t)g_agc_dev.height + 255u
             & ~(size_t)255);
        cur_offset += EVO_AGC_COMPOSITE_SIZE;
    }

    /* Stencil buffer, 2 MB aligned like the scanout: DB bases are address>>8
     * and the surface is tiled, so a tightly-aligned base is not enough. */
    {
        uint8_t *st = g_agc_dev.direct_mem_base + cur_offset;
        g_agc_dev.stencil_base =
            (uint8_t *)(((uintptr_t)st + 0x1fffffu) & ~(uintptr_t)0x1fffff);
        cur_offset += EVO_AGC_STENCIL_SIZE;

        uint8_t *dp = g_agc_dev.direct_mem_base + cur_offset;
        g_agc_dev.depth_base =
            (uint8_t *)(((uintptr_t)dp + 0x1fffffu) & ~(uintptr_t)0x1fffff);
        cur_offset += EVO_AGC_DEPTH_SIZE;
    }

    /* Layer surfaces for RmlUi backdrop-filter: full-canvas RGBA8 targets,
     * COMP_SWAP=STD so the rgba8 T# can sample them back. 256-byte aligned for
     * the image descriptors (base stored as >>8), and each is 32 MB - 4K RGBA8
     * needs 31.64 MB, so 4K straddles a 32 MB slot exactly. */
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        uint8_t *lg = g_agc_dev.direct_mem_base + cur_offset;
        layer->cpu_base = (uint8_t *)(((uintptr_t)lg + 255u) & ~(uintptr_t)255);
        layer->gpu_addr = (uint64_t)(uintptr_t)layer->cpu_base;
        layer->pool_index = i;
        layer->in_use = 0;
        layer->pitch_bytes = 0; /* set with width/height below */
        layer->mrt = g_agc_dev.gpu_regs->layer_targets[i];
        cur_offset += EVO_AGC_LAYER_BYTES;
    }

    /* Shader storage */
    uint8_t *shader_storage = g_agc_dev.direct_mem_base + cur_offset;
    size_t shader_storage_used = 0;

    /* 3. Initialize AGC Hardware */
    ret = sceAgcInit(8);
    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcInit failed: %d\n", ret);
        goto cleanup_fail;
    }

    void *agc_defaults = sceAgcGetRegisterDefaults();
    if (!agc_defaults) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcGetRegisterDefaults failed\n");
        goto cleanup_fail;
    }

    /* 4. Setup MRT0 Color Targets for Scanouts */
    /* A failure here leaves the MRT0 registers partly zeroed, which the GPU
     * happily accepts and then draws nothing into - exactly the symptom this
     * path spent a session chasing. It was being called for its side effect
     * with the result dropped; fail the init instead. */
    int ct0 = setup_color_target(g_agc_dev.gpu_regs->color_targets[0], agc_defaults,
                                 g_agc_dev.scanout_buffers[0], g_agc_dev.width, g_agc_dev.height, 1);
    int ct1 = setup_color_target(g_agc_dev.gpu_regs->color_targets[1], agc_defaults,
                                 g_agc_dev.scanout_buffers[1], g_agc_dev.width, g_agc_dev.height, 1);
    setup_depth_target(g_agc_dev.gpu_regs->depth_target, g_agc_dev.depth_base,
                       g_agc_dev.stencil_base,
                       (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        if (setup_color_target(layer->mrt, agc_defaults, layer->cpu_base,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height, 0) != 0) {
            printf(EVO_AGC_LOG_PREFIX "setup layer target %d failed\n", i);
            goto cleanup_fail;
        }
        layer->width = (uint32_t)g_agc_dev.width;
        layer->height = (uint32_t)g_agc_dev.height;
        layer->pitch_bytes = ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
    }
    evo_boot_log("agc stencil base=%p z_info=%#x s_info=%#x size_xy=%#x",
                 (void *)g_agc_dev.stencil_base,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[0].value,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[1].value,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[13].value);
    evo_boot_log("agc color_target rc=%d/%d base0=%#x info=%#x (comp_swap=%u) view=%#x attrib=%#x",
                 ct0, ct1,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][0].value,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][2].value,
                 (unsigned)((g_agc_dev.gpu_regs->color_targets[0][2].value >> 11) & 3u),
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][1].value,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][15].value);
    if (ct0 != 0 || ct1 != 0) {
        printf(EVO_AGC_LOG_PREFIX "setup_color_target failed: %d/%d\n", ct0, ct1);
        goto cleanup_fail;
    }

    /* 5. Compile All AGC Pipelines */
    /* Only the UI pipeline is required. The video pipelines are converted to
     * .pipe form one at a time; a missing or failed one disables that path
     * rather than aborting the whole runtime, so the UI can be brought up and
     * verified independently. */
    ret = compile_agc_pipeline(&g_agc_dev.pipelines[EVO_AGC_PIPE_UI],
                               shader_storage, &shader_storage_used,
                               &ui_screen_2d_metadata, "ui_screen_2d");
    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "UI pipeline failed to compile: %d\n", ret);
        goto cleanup_fail;
    }

#ifdef EVO_AGC_HAVE_VIDEO_PIPES
    static const struct {
        int pipe_id;
        const evo_agc_shader_metadata_t *meta;
        const char *name;
    } video_pipes[] = {
        {EVO_AGC_PIPE_VIDEO_NV12,   &video_yuv_nv12_metadata,     "video_yuv_nv12"},
        {EVO_AGC_PIPE_VIDEO_HDR,    &video_yuv_p010_hdr_metadata, "video_yuv_p010_hdr"},
        {EVO_AGC_PIPE_VIDEO_HLG,    &video_yuv_p010_hlg_metadata, "video_yuv_p010_hlg"},
        {EVO_AGC_PIPE_VIDEO_PLANAR, &video_yuv_planar_metadata,   "video_yuv_planar"},
    };
    for (unsigned i = 0; i < sizeof(video_pipes) / sizeof(video_pipes[0]); ++i) {
        int vrc = compile_agc_pipeline(&g_agc_dev.pipelines[video_pipes[i].pipe_id],
                                       shader_storage, &shader_storage_used,
                                       video_pipes[i].meta, video_pipes[i].name);
        if (vrc != 0)
            evo_boot_log("agc pipe %s unavailable (rc=%d); that video path is off",
                         video_pipes[i].name, vrc);
    }
#else
    evo_boot_log("agc video pipelines not built yet (.pipe conversion pending)");
#endif

    /* The backdrop-blur pipe (backdrop-filter for RmlUi): the fragment shader
     * owns both its BlurConstants buffer AND its source texture, so its user
     * data has a PS const table in addition to the PS texture table. Optional
     * like the video pipes - a failure just disables backdrop blur. */
    {
        int brc = compile_agc_pipeline(&g_agc_dev.pipelines[EVO_AGC_PIPE_UI_BLUR],
                                       shader_storage, &shader_storage_used,
                                       &ui_backdrop_blur_metadata, "ui_backdrop_blur");
        if (brc != 0)
            evo_boot_log("agc pipe ui_backdrop_blur unavailable (rc=%d); "
                         "backdrop-filter is off", brc);
    }

    /* Quad index buffer for fullscreen video drawing */
    size_t qat = (shader_storage_used + 255u) & ~255u;
    g_agc_dev.quad_indices = (uint16_t *)(shader_storage + qat);
    const uint16_t initial_quad_indices[6] = { 0, 1, 2, 2, 1, 3 };
    memcpy(g_agc_dev.quad_indices, initial_quad_indices, sizeof(initial_quad_indices));
    shader_storage_used = qat + sizeof(initial_quad_indices);

    /*
     * Flush everything the GPU will DMA-read for the rest of the process's
     * life. This pool is write-back cached (memory type 12), and all of it was
     * just written by the CPU:
     *
     *   shader_storage - the shader HEADERS, the MACHINE CODE the GPU fetches
     *       instructions from, the sceAgcLinkShaders output, and the quad index
     *       buffer.
     *   gpu_regs       - the MRT0 colour targets plus every pipeline's cx/sh/uc
     *       register array. The *RegistersIndirect packets make the command
     *       processor DMA-read these at submit time, and the sh array carries
     *       SPI_SHADER_PGM_LO/HI - the shader's entry address.
     *
     * Written once, never flushed, so the GPU read stale lines for both: it was
     * being pointed at a garbage program address and fetching garbage code.
     * That is why a DCB with no draws retired its fence in 0ms while the first
     * DCB containing a draw never retired at all and wedged the GPU. The
     * per-frame flushes (DCB, transient ring) and the per-resource ones in the
     * render interface (vertices, indices, textures) were already right; these
     * two init-time regions were simply missed.
     */
    evo_agc_runtime_cache_flush(shader_storage, shader_storage_used);
    evo_agc_runtime_cache_flush(g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));
    evo_boot_log("agc init flush shader_storage=%p bytes=%zu gpu_regs=%p bytes=%zu",
                 (void *)shader_storage, shader_storage_used,
                 (void *)g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));

    /* 6. Initialize VideoOut */
    for (int attempt = 1; attempt <= 3; ++attempt) {
        g_agc_dev.video_handle = sceVideoOutOpen(0xff, 0, 0, NULL);
        if (g_agc_dev.video_handle >= 0)
            break;
        sceKernelUsleep(500000);
    }
    if (g_agc_dev.video_handle < 0) {
        printf(EVO_AGC_LOG_PREFIX "sceVideoOutOpen failed\n");
        goto cleanup_fail;
    }

    sceVideoOutSetFlipRate(g_agc_dev.video_handle, 0);

    /*
     * What the panel is actually running at. The render size below is still
     * whatever evo_agc_runtime_init() was handed - the UI is authored at a
     * fixed 1920x1080 canvas (ui/include/evo_metrics.h, every .rcss) - so this
     * is a readback, not a mode request. It is the input to deciding whether
     * rendering at the panel's own resolution is worth doing.
     */
    {
        evo_vo_resolution_status vres;
        memset(&vres, 0, sizeof(vres));
        int32_t vrc = sceVideoOutGetResolutionStatus(g_agc_dev.video_handle, &vres);
        evo_boot_log("agc display probe rc=%d full=%ux%u pane=%ux%u refresh_id=%llu "
                     "inches=%d render=%dx%d",
                     vrc, vres.full_width, vres.full_height,
                     vres.pane_width, vres.pane_height,
                     (unsigned long long)vres.refresh_rate,
                     (int)vres.screen_inches,
                     g_agc_dev.width, g_agc_dev.height);

        /*
         * Drive the panel at its own resolution. The UI is resolution
         * independent (dp against the EVO_UI_DESIGN canvas), so this is a
         * render-size change, not a layout change. Clamped at
         * EVO_AGC_MAX_RENDER: the composite/depth/stencil budgets above are
         * cut for that size, and a 4K panel would need four times the 1080p
         * footprint of each.
         */
        int panel_w = (vrc == 0) ? (int)vres.full_width  : 0;
        int panel_h = (vrc == 0) ? (int)vres.full_height : 0;
        if (panel_w > 0 && panel_h > 0 &&
            (panel_w != g_agc_dev.width || panel_h != g_agc_dev.height)) {
            if (panel_w > EVO_AGC_MAX_RENDER_W || panel_h > EVO_AGC_MAX_RENDER_H) {
                evo_boot_log("agc display: panel %dx%d over the %dx%d render cap, "
                             "staying at %dx%d", panel_w, panel_h,
                             EVO_AGC_MAX_RENDER_W, EVO_AGC_MAX_RENDER_H,
                             g_agc_dev.width, g_agc_dev.height);
            } else if (evo_agc_apply_render_size(agc_defaults, panel_w, panel_h) == 0) {
                evo_boot_log("agc display: render size -> %dx%d (composite pitch %u)",
                             g_agc_dev.width, g_agc_dev.height,
                             (unsigned)g_agc_dev.composite_pitch);
            } else {
                evo_boot_log("agc display: %dx%d target setup failed, reverting to %dx%d",
                             panel_w, panel_h, width, height);
                (void)evo_agc_apply_render_size(agc_defaults, width, height);
            }
        }

        evo_vo_output_status vout;
        memset(&vout, 0, sizeof(vout));
        int32_t vorc = sceVideoOutGetOutputStatus(g_agc_dev.video_handle, &vout);
        if (vorc == 0) {
            g_agc_dev.display_dynamic_range = (int)vout.dynamic_range;
            g_agc_dev.display_resolution_token = vout.resolution;
            g_agc_dev.display_is_hdr = (vout.dynamic_range == 2 || (vout.flags & 1)) ? 1 : 0;
            evo_boot_log("agc display output probe rc=%d res_token=%u dynamic_range=%u (%s) refresh=%llu flags=%#llx",
                         vorc, vout.resolution, vout.dynamic_range,
                         vout.dynamic_range == 2 ? "HDR" : (vout.dynamic_range == 1 ? "SDR" : "Unknown"),
                         (unsigned long long)vout.refresh_rate,
                         (unsigned long long)vout.flags);
        } else {
            evo_boot_log("agc display output probe failed rc=%d", vorc);
        }
        evo_boot_log_flush();
    }

    evo_video_buffer_t video_buffers[2] = {
        {g_agc_dev.scanout_buffers[0], NULL, NULL, NULL},
        {g_agc_dev.scanout_buffers[1], NULL, NULL, NULL},
    };
    evo_video_attribute_t attr;
    memset(&attr, 0, sizeof(attr));

    uint64_t vfmt = g_agc_dev.is_hdr ? EVO_AGC_VIDEO_FORMAT_HDR : EVO_AGC_VIDEO_FORMAT_SDR;
    sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                   (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height,
                                   0, 0, 0);

    ret = sceVideoOutRegisterBuffers2(g_agc_dev.video_handle, 0, 0,
                                      video_buffers, 2, &attr, 0, NULL);
    /* evo_boot_log, not printf: this file's printf output never reaches
     * evo.log or klog, so every one of these diagnostics was invisible. */
    /* Both scanout bases must be 2MB aligned or the tiled display surface is
     * shuffled; mis0/mis1 are the offsets past that boundary and must read 0. */
    evo_boot_log("agc scanout align mis0=%#llx mis1=%#llx tiled_need=%#x stride=%#llx",
                 (unsigned long long)((uintptr_t)g_agc_dev.scanout_buffers[0] &
                                      (EVO_AGC_DIRECT_MEM_ALIGN - 1u)),
                 (unsigned long long)((uintptr_t)g_agc_dev.scanout_buffers[1] &
                                      (EVO_AGC_DIRECT_MEM_ALIGN - 1u)),
                 (unsigned)((((uint32_t)g_agc_dev.width + 127u) / 128u) *
                            (((uint32_t)g_agc_dev.height + 127u) / 128u) * 0x10000u),
                 (unsigned long long)EVO_AGC_SCANOUT_STRIDE);
    evo_boot_log("agc vo handle=%d register_rc=%d (0x%08x) fmt=%#llx %dx%d "
                 "buf0=%p buf1=%p memtype=%d",
                 g_agc_dev.video_handle, ret, (unsigned)ret,
                 (unsigned long long)vfmt, g_agc_dev.width, g_agc_dev.height,
                 (void *)g_agc_dev.scanout_buffers[0],
                 (void *)g_agc_dev.scanout_buffers[1],
                 EVO_AGC_DIRECT_MEM_TYPE);
    if (ret != 0 && (g_agc_dev.width != width || g_agc_dev.height != height)) {
        /*
         * VideoOut would not take the panel's own size. Nothing has been
         * registered, so going back to the size the caller asked for is just
         * a second attempt, not a re-registration.
         */
        evo_boot_log("agc vo register rc=%d at %dx%d -> falling back to %dx%d",
                     ret, g_agc_dev.width, g_agc_dev.height, width, height);
        if (evo_agc_apply_render_size(agc_defaults, width, height) == 0) {
            sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                           (uint32_t)g_agc_dev.width,
                                           (uint32_t)g_agc_dev.height,
                                           0, 0, 0);
            ret = sceVideoOutRegisterBuffers2(g_agc_dev.video_handle, 0, 0,
                                              video_buffers, 2, &attr, 0, NULL);
        }
    }

    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceVideoOutRegisterBuffers2 failed: %d\n", ret);
        goto cleanup_fail;
    }

    g_agc_dev.active_backbuffer = 0;
    g_agc_dev.current_slot = 0;
    g_agc_dev.frame_counter = 0;
    g_agc_dev.flip_arg = 1;
    g_agc_dev.bound_pipeline = -1;
    g_agc_dev.initialized = 1;

    printf(EVO_AGC_LOG_PREFIX "AGC runtime successfully initialized! 100%% GPU ready.\n");
    return 0;

cleanup_fail:
    evo_agc_runtime_shutdown();
    return -1;
}

void evo_agc_runtime_shutdown(void)
{
    if (g_agc_dev.video_handle >= 0) {
        sceVideoOutUnregisterBuffers(g_agc_dev.video_handle, 0);
        sceVideoOutClose(g_agc_dev.video_handle);
        g_agc_dev.video_handle = -1;
    }

    if (g_agc_dev.direct_mem_base) {
        sceKernelMunmap(g_agc_dev.direct_mem_base, g_agc_dev.direct_mem_bytes);
        g_agc_dev.direct_mem_base = NULL;
    }

    if (g_agc_dev.direct_mem_offset >= 0) {
        sceKernelReleaseDirectMemory(g_agc_dev.direct_mem_offset, g_agc_dev.direct_mem_bytes);
        g_agc_dev.direct_mem_offset = -1;
    }

    g_agc_dev.initialized = 0;
}

int evo_agc_runtime_is_active(void)
{
    return g_agc_dev.initialized;
}

void evo_agc_runtime_frame_begin(void)
{
    if (!g_agc_dev.initialized || g_agc_dev.frame_active)
        return;

    const uint32_t slot = g_agc_dev.current_slot;

    /* Diagnostic trace for the first dozen frames only - enough to cross every
     * slot's fence-wait more than once without spamming evo.log forever. Drop
     * once the black-screen-then-hang symptom is understood; not gated behind
     * a flag file since the very first frames are exactly what's in question
     * and there is no window to drop one in before they run. */
    static int s_trace_frames = 12;
    int tracing = s_trace_frames > 0;
    if (tracing) {
        s_trace_frames--;
        evo_boot_log("agc frame_begin slot=%u fence=%u expect=%u", slot,
                     (unsigned)*g_agc_dev.fences[slot],
                     (unsigned)g_agc_dev.fence_expect[slot]);
    }

    /* Wait for this slot's last submit to retire. The fence line must be
     * clflush'd before every read: it lives in the same write-back pool the
     * CPU wrote, so without evicting it the CPU re-reads its own stale copy
     * forever. ps5-opengl's poll loop does exactly this. Bounded so a wedged
     * GPU degrades to a dropped frame instead of hanging the render thread. */
    if (g_agc_dev.fence_expect[slot]) {
        unsigned waits = 0;
        for (; waits < 2000; ++waits) {
            evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);
            if (*g_agc_dev.fences[slot] == g_agc_dev.fence_expect[slot])
                break;
            sceKernelUsleep(1000);
        }
        if (waits >= 2000) {
            static int s_timeout_log = 4;
            if (s_timeout_log > 0) {
                s_timeout_log--;
                evo_boot_log("agc frame_begin slot=%u FENCE TIMEOUT fence=%u expect=%u",
                             slot, (unsigned)*g_agc_dev.fences[slot],
                             (unsigned)g_agc_dev.fence_expect[slot]);
            }
        } else if (tracing) {
            evo_boot_log("agc frame_begin slot=%u retired after %u waits", slot, waits);
        }
    }

    /*
     * Menu backdrop - dark neutral (0x0d,0x0d,0x10,0xff). In player mode the
     * video quad is drawn straight into the tiled backbuffer, so the clear is
     * normally skipped: it is an 8.3 MB CPU write plus ~130,000 clflushes,
     * 4-6 ms per frame at 1080p and far worse at 4K.
     *
     * But the quad only covers the *image*. Anything the UI composited on top
     * - OSD, scrub bar, subtitles - and everything outside a letterboxed image
     * survives into the next use of this buffer. Skipping the clear there left
     * each subtitle line stacked on the last and the OSD frozen on screen once
     * it faded out, which is only obvious when the quad is not being redrawn
     * every frame (software decode). So the clear is skipped only for a buffer
     * that carried nothing but video.
     */
    if (!g_agc_dev.is_player_mode ||
        g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer]) {
        g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer] = 0;
        uint32_t *backbuffer = (uint32_t *)g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
        if (backbuffer) {
            uint64_t val = (uint64_t)0xff100d0d | ((uint64_t)0xff100d0d << 32);
            uint64_t *p64 = (uint64_t *)backbuffer;
            size_t count = ((size_t)g_agc_dev.width * (size_t)g_agc_dev.height) / 2;
            for (size_t i = 0; i < count; ++i) {
                p64[i] = val;
            }
            /* Write-back cached pool (memory type 12): a CPU write sits in L1/L2
             * until something evicts it while the display controller reads DRAM
             * directly. Costs a full-screen clflush walk per frame; a
             * write-combined GARLIC pool (memory type 3, as pp_videoout.c used)
             * would need none, worth revisiting once the picture is correct. */
            evo_agc_runtime_cache_flush(backbuffer, count * sizeof(uint64_t));
        }
    }

    /* Open transient ring slot */
    /*
     * Reopen this slot's transient-ring region with the token it was sealed
     * under. This used to pass (0, 1) - a proven completion with a zero token -
     * which transient_ring_begin rejects as TOKEN_MISMATCH for any SEALED slot.
     * The return value was ignored, so each slot silently stayed sealed after
     * its first present; once all three had been used, every allocation
     * returned SLOT_BUSY, RenderGeometry bailed before drawing, and the UI
     * froze on the last frame that made it out. The fence wait above is what
     * proves the GPU is done, so the token is safe to hand back here.
     */
    int ring_rc = evo_agc_transient_ring_begin(&g_agc_dev.transient_ring, slot,
                                               g_agc_dev.ring_token[slot], 1);
    if (ring_rc != EVO_AGC_TRANSIENT_OK) {
        static int s_ring_log = 6;
        if (s_ring_log > 0) {
            s_ring_log--;
            evo_boot_log("agc frame_begin slot=%u transient_ring_begin FAILED rc=%d token=%llu",
                         slot, ring_rc,
                         (unsigned long long)g_agc_dev.ring_token[slot]);
        }
    }

    /* Initialize DCB writer for this slot */
    evo_agc_writer_init(&g_agc_dev.current_cb,
                        g_agc_dev.dcb_slots[slot],
                        g_agc_dev.dcb_slot_capacity_dwords);

    /* Wait rendering packet for VideoOut scanout safety */
    uint32_t wait_size = sceAgcDriverGetWaitRenderingPacketSizeInDwords();
    if (wait_size > 0) {
        sceAgcDriverWaitUntilSafeForRendering(&g_agc_dev.current_cb.up, wait_size, 0,
                                              (uint32_t)g_agc_dev.video_handle,
                                              g_agc_dev.active_backbuffer);
    }

    /* Set MRT0 Color Target to the current scanout backbuffer */
    evo_agc_writer_set_target(&g_agc_dev.current_cb,
                              g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer], 16);
    g_agc_dev.current_layer_target = NULL;

    /* Bind the stencil target, then zero the stencil planes for this frame.
     * Clearing once here is what lets each RmlUi clip mask claim its own value
     * instead of clearing a multi-MB surface per mask. The DMA fill is CP-synced
     * so it completes before any draw in this DCB reads the buffer. */
    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb,
                                   g_agc_dev.gpu_regs->depth_target, 16);
    if (g_agc_dev.stencil_base) {
        sceAgcDcbDmaData(&g_agc_dev.current_cb, 0u, 3u, 0u,
                         (uint64_t)(uintptr_t)g_agc_dev.stencil_base,
                         2u, 0u, 0u,
                         /* 4 MB covers the 64KB_Z_X tiled footprint of an S8
                          * 1920x1080 surface (~2.6 MB) with margin; a
                          * width*height linear fill leaves the padding dirty. */
                         (uint32_t)(4u * 1024u * 1024u),
                         0u, 0u, 1u /* cp_sync */);
    }
    g_agc_dev.stencil_ref = 0u;
    g_agc_dev.stencil_counter = 0u;
    g_agc_dev.stencil_func_equal = 1;
    g_agc_dev.clip_mask_enabled = 0;

    /* Set default viewport & scissor */
    evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12), 0.0f, 0.0f,
                                (float)g_agc_dev.width, (float)g_agc_dev.height);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    /* Default blend: premultiplied alpha */
    evo_agc_writer_set_blend(&g_agc_dev.current_cb, alloc_transient_cx(2), EVO_AGC_BLEND_PREMULTIPLIED);

    g_agc_dev.bound_pipeline = -1;
    g_agc_dev.frame_has_draws = 0;
    g_agc_dev.frame_active = 1;
}

void evo_agc_runtime_bind_pipeline(int pipeline_id)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT)
        return;
    if (g_agc_dev.bound_pipeline == pipeline_id)
        return;

    evo_agc_pipeline_t *pipe = &g_agc_dev.pipelines[pipeline_id];
    if (!pipe->valid)
        return;

    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, pipe->cx_regs, pipe->cx_reg_count);
    evo_agc_writer_set_sh_indirect(&g_agc_dev.current_cb, pipe->sh_regs, pipe->sh_reg_count);
    evo_agc_writer_set_uc_indirect(&g_agc_dev.current_cb, pipe->uc_regs, pipe->uc_reg_count);

    /*
     * Rasteriser state this path never programmed, emitted AFTER the pipeline
     * registers so it wins over anything sceAgcLinkShaders put in linked_cx.
     *
     * Every one of these was left to whatever sceAgcGetRegisterDefaults()
     * happened to contain. ps5-opengl programs all of them explicitly from its
     * own pipeline state (runtime_rasterizer_control at 0x205,
     * runtime_color_control at 0x202, runtime_depth_control at 0x200) and only
     * relies on defaults for registers it never uses. With blending forced off
     * the scanout still came back completely untouched (changed=0), so no
     * fragment is reaching the colour block - and a default that culls both
     * faces, disables the viewport transform, or leaves the colour block
     * disabled produces exactly that: draws that execute, retire and export
     * nothing.
     *
     *   0x205 PA_SU_SC_MODE_CNTL - 0: cull nothing, solid fill, CCW front.
     *         RmlUi emits both windings, so any culling silently drops half or
     *         all of the UI.
     *   0x206 PA_CL_VTE_CNTL - 0x43f: enable the viewport X/Y/Z scale+offset
     *         transform and VTX_W0_FMT. Without it clip space is never mapped
     *         through the viewport we set, so the geometry lands nowhere.
     *   0x200 DB_DEPTH_CONTROL - 0: depth test/write off. There is no depth
     *         buffer bound at all here, and a default with Z_ENABLE set kills
     *         every fragment against a target that does not exist.
     *   0x202 CB_COLOR_CONTROL - MODE=CB_NORMAL(1)<<4, ROP3=0xcc (copy).
     *         MODE=CB_DISABLE is a completely silent "write nothing".
     */
    SceAgcRegister *ff = alloc_transient_cx(4);
    if (ff) {
        ff[0] = (SceAgcRegister){0x205, 0u};
        ff[1] = (SceAgcRegister){0x206, 0x43fu};
        ff[2] = (SceAgcRegister){0x200, 0u};
        ff[3] = (SceAgcRegister){0x202, 0x00cc0010u};
        evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, ff, 4);
    }

    g_agc_dev.bound_pipeline = pipeline_id;
}

void evo_agc_runtime_set_scissor(int x, int y, int w, int h)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    SceAgcRegister *sc = alloc_transient_cx(2);
    if (!sc)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) {
        evo_agc_writer_set_scissor(&g_agc_dev.current_cb, sc, 0, 0, 0, 0);
        g_agc_dev.scissor_x = g_agc_dev.scissor_y = 0;
        g_agc_dev.scissor_w = g_agc_dev.scissor_h = 0;
        return;
    }
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, sc,
                               (uint32_t)x, (uint32_t)y,
                               (uint32_t)(x + w), (uint32_t)(y + h));
    g_agc_dev.scissor_x = x; g_agc_dev.scissor_y = y;
    g_agc_dev.scissor_w = w; g_agc_dev.scissor_h = h;
}

void evo_agc_runtime_set_blend(int blend_mode)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    SceAgcRegister *bl = alloc_transient_cx(2);
    if (!bl)
        return;
    evo_agc_writer_set_blend(&g_agc_dev.current_cb, bl, blend_mode);
}

int evo_agc_has_layers(void)
{
    return g_agc_dev.initialized;
}

/* Clear a freshly-acquired layer surface. RmlUi only requires a pushed layer to
 * be transparent black within the ACTIVE SCISSOR REGION - which is what
 * evo_agc_runtime.h has always documented ("memset the scissor region + clflush,
 * not the full surface"). Wiping the whole canvas instead cost ~33 MB of stores
 * plus a 33 MB cache flush per acquire, and a blurred element takes three
 * acquires per frame; for a card-sized region this is roughly 70x less work.
 * RmlUi sets the backdrop scissor before PushLayer, so the rect is current. */
static void layer_surface_clear(evo_agc_layer_surface_t *layer)
{
    if (!layer || !layer->cpu_base || !layer->pitch_bytes)
        return;

    int x = g_agc_dev.scissor_x, y = g_agc_dev.scissor_y;
    int w = g_agc_dev.scissor_w, h = g_agc_dev.scissor_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w > (int)layer->width  - x) w = (int)layer->width  - x;
    if (h > (int)layer->height - y) h = (int)layer->height - y;
    if (w <= 0 || h <= 0)
        return;

    const size_t pitch = (size_t)layer->pitch_bytes;
    const size_t span  = (size_t)w * 4u;
    uint8_t *row = layer->cpu_base + (size_t)y * pitch + (size_t)x * 4u;
    for (int i = 0; i < h; ++i, row += pitch) {
        memset(row, 0, span);
        /* Flush only the span just written. Flushing whole rows instead walked
         * pitch/span times the cache lines - 2.8x for a sidebar-width element,
         * and this runs three times per blurred element per frame. */
        evo_agc_runtime_cache_flush(row, span);
    }
}

int evo_agc_layer_acquire(evo_agc_layer_surface_t **out)
{
    if (!g_agc_dev.initialized || !out)
        return -1;
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        if (!layer->in_use) {
            layer->in_use = 1;
            /* No logging here: this runs three times per blurred element
             * per frame, and evo_log_flush() fsyncs to USB (~3-4 ms a
             * line), which cost ~42 ms/frame and pinned the UI at ~20fps. */
            layer_surface_clear(layer);
            *out = layer;
            return 0;
        }
    }
    return -1;
}

void evo_agc_layer_release(evo_agc_layer_surface_t *layer)
{
    if (!layer)
        return;
    layer->in_use = 0;
}

int evo_agc_set_layer_target(const evo_agc_layer_surface_t *layer)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return -1;

    if (layer != g_agc_dev.current_layer_target) {
        if (layer) {
            /* The layer MRT register block lives inside GPU-mapped gpu_regs
             * (carved at init, rebuilt+flushed by apply_render_size), so SetTarget
             * can DMA-read it at submit time. */
            evo_agc_writer_set_target(&g_agc_dev.current_cb, layer->mrt, 16);
        } else {
            evo_agc_writer_set_target(&g_agc_dev.current_cb,
                                      g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer],
                                      16);
        }
        g_agc_dev.current_layer_target = layer;
        /* A target switch returns the CP to the frame's default full-canvas
         * viewport: the caller (CompositeLayers) diverges via scissor alone.
         * Re-emit the viewport here so a stale 12-register viewport block from
         * an earlier frame cannot survive into the new target. */
        SceAgcRegister *vp = alloc_transient_cx(12);
        if (vp)
            evo_agc_writer_set_viewport(&g_agc_dev.current_cb, vp, 0.0f, 0.0f,
                                        (float)g_agc_dev.width, (float)g_agc_dev.height);
    }
    return 0;
}

void evo_agc_get_scanout_layer(evo_agc_layer_surface_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->width       = (uint32_t)g_agc_dev.width;
    out->height      = (uint32_t)g_agc_dev.height;
    out->pitch_bytes = ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
    out->gpu_addr    = (uint64_t)(uintptr_t)g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
    out->cpu_base    = g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
    out->pool_index  = -1;
}

void evo_agc_flush_color_target(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    evo_agc_writer_flush_color_target(&g_agc_dev.current_cb);
}

void evo_agc_runtime_frame_end(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;

    const uint32_t slot = g_agc_dev.current_slot;

    /*
     * A frame with no draws must not be presented.
     *
     * frame_begin() runs whenever the render loop wants a frame (gl_active),
     * but RmlUi only actually redraws a couple of times a second - the loop
     * itself runs at ~370/s. Presenting the frames in between flips a buffer
     * carrying nothing but the backdrop clear, so the panel alternates between
     * the UI and a blank screen: on hardware that reads as the UI flashing once
     * and then going black. The per-frame DCB sizes show it exactly - 2830
     * dwords when RmlUi drew, 68 when it did not.
     *
     * So close the frame and leave the displayed buffer alone: no submit, no
     * flip, and no back-buffer toggle. The scanout keeps showing the last frame
     * that actually had content, which is what a retained-mode UI wants.
     */
    if (!g_agc_dev.frame_has_draws) {
        /* ABORT, not seal. Sealing hands the slot a retire token that only a
         * submitted frame can ever clear: the next frame_begin calls
         * transient_ring_begin(slot, 0, 1), which sees SEALED with a zero token
         * and returns TOKEN_MISMATCH, so the slot is never reopened, every
         * subsequent alloc returns SLOT_BUSY, RenderGeometry bails before
         * drawing - and the frame is then empty for that reason, which seals it
         * again. One sealed slot and the UI never draws again. abort() is the
         * call meant for a frame that is discarded rather than submitted: it
         * drops the slot straight back to EMPTY. */
        evo_agc_transient_ring_abort(&g_agc_dev.transient_ring, slot);
        g_agc_dev.ring_token[slot] = 0u;   /* nothing outstanding on this slot */
        g_agc_dev.frame_active = 0;
        return;
    }

    /* Mirrors frame_begin's trace - the two are called once each per frame in
     * lockstep, so an independent counter here stays in sync with it. */
    static int s_trace_frames = 12;
    int tracing = s_trace_frames > 0;
    if (tracing) {
        s_trace_frames--;
        evo_boot_log("agc frame_end slot=%u dwords_before_flip=%u", slot,
                     evo_agc_writer_dwords_written(&g_agc_dev.current_cb));
    }

    /* Prepare this frame's fence marker up front; every mode below uses it. */
    if (++g_agc_dev.fence_marker == 0u)
        g_agc_dev.fence_marker = 1u;
    const uint32_t marker = g_agc_dev.fence_marker;
    *g_agc_dev.fences[slot] = 0;
    g_agc_dev.fence_expect[slot] = marker;
    evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);

    const uint64_t fence_addr = (uint64_t)(uintptr_t)g_agc_dev.fences[slot];
    g_agc_dev.flip_arg++;

    /* Flush the colour-block caches out to memory, then stamp the frame's
     * end-of-pipe fence (which also writes GPU L2 back). The flip itself is
     * issued from the CPU after the fence retires - see the note above. */
    evo_agc_writer_flush_color_target(&g_agc_dev.current_cb);
    evo_agc_writer_release_mem(&g_agc_dev.current_cb, fence_addr, marker);

    /* 3. Seal transient ring */
    /* Remember the token so the next frame_begin on this slot can reopen it. */
    g_agc_dev.ring_token[slot] = (uint64_t)g_agc_dev.flip_arg;
    evo_agc_transient_ring_seal(&g_agc_dev.transient_ring, slot,
                                g_agc_dev.ring_token[slot]);

    /* 4. Submit DCB to GPU Driver.
     *
     * Mandatory before every submit (see evo_agc_runtime_cache_flush's doc
     * comment, and the historical hardware-verified #27 present path this
     * bare-metal rewrite otherwise mirrors): everything the GPU CP is about to
     * DMA-read this frame - the command words themselves, plus every register/
     * constant/descriptor slice this frame allocated from the transient ring
     * (color target + viewport/scissor/blend registers, pipeline registers on
     * a bind, video constants, texture descriptors) - was just written by the
     * CPU into write-back memory and needs evicting before the GPU can see it.
     */
    uint32_t dwords = evo_agc_writer_dwords_written(&g_agc_dev.current_cb);
    if (dwords > g_agc_dev.dcb_peak_dwords)
        g_agc_dev.dcb_peak_dwords = dwords;
    if (!g_agc_dev.dcb_min_presented || dwords < g_agc_dev.dcb_min_presented)
        g_agc_dev.dcb_min_presented = dwords;
    g_agc_dev.presents++;
    /* A presented frame far smaller than the busiest one is a partial render
     * going to the panel over a freshly cleared buffer. Log the first few. */
    if (g_agc_dev.dcb_peak_dwords > 1000u &&
        dwords < g_agc_dev.dcb_peak_dwords / 4u) {
        static int s_partial_log = 10;
        if (s_partial_log > 0) {
            s_partial_log--;
            evo_boot_log("agc PARTIAL present frame=%llu dwords=%u peak=%u",
                         (unsigned long long)g_agc_dev.frame_counter,
                         dwords, g_agc_dev.dcb_peak_dwords);
        }
    }
    if ((g_agc_dev.frame_counter % 120u) == 0u) {
        evo_direct_mem_stats_t dm;
        evo_direct_mem_get_stats(&dm);
        evo_boot_log("agc health frame=%llu dcb=%u/%u peak=%u ring_fail=%u tex_fail=%u "
                     "direct_mem=%zu/%zu peak=%zu allocs=%zu",
                     (unsigned long long)g_agc_dev.frame_counter,
                     dwords, g_agc_dev.dcb_slot_capacity_dwords,
                     g_agc_dev.dcb_peak_dwords,
                     g_agc_dev.ring_alloc_fail, g_agc_dev.tex_alloc_fail,
                     dm.allocated_bytes, dm.total_bytes, dm.peak_bytes,
                     dm.num_allocations);
        evo_boot_log("agc health clip_masks=%u clip_enables=%u (feature=%d)",
                     g_agc_dev.clip_mask_calls, g_agc_dev.clip_enable_calls,
                     EVO_AGC_CLIP_MASK);
        g_agc_dev.clip_mask_calls = 0;
        g_agc_dev.clip_enable_calls = 0;
        evo_boot_log("agc health presents=%u dcb_min_presented=%u flip_waits=%u timeouts=%u",
                     g_agc_dev.presents, g_agc_dev.dcb_min_presented,
                     g_agc_dev.flip_waits, g_agc_dev.flip_timeouts);
        g_agc_dev.dcb_min_presented = 0;
        g_agc_dev.presents = 0;
        g_agc_dev.flip_waits = 0;
    }
    evo_agc_runtime_cache_flush(g_agc_dev.dcb_slots[slot], (size_t)dwords * sizeof(uint32_t));
    {
        size_t ring_used = evo_agc_transient_ring_used(&g_agc_dev.transient_ring, slot);
        if (ring_used)
            evo_agc_runtime_cache_flush(g_agc_dev.transient_ring.base +
                                        g_agc_dev.transient_ring.slots[slot].offset,
                                        ring_used);
    }

    SceAgcSubmit submit = {
        .words = g_agc_dev.dcb_slots[slot],
        .count = dwords,
        .flag = 0,
        .padding = {0, 0, 0},
    };
    if (tracing)
        evo_boot_log("agc frame_end slot=%u dwords=%u submitting", slot, dwords);
    int32_t submit_rc = sceAgcDriverSubmitDcb(&submit);
    if (tracing)
        evo_boot_log("agc frame_end slot=%u submit_rc=%d", slot, submit_rc);
    if (submit_rc != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcDriverSubmitDcb failed: %d\n", submit_rc);
    } else {
        /* Cooperative yield the platform's GPU scheduler requires: go too long
         * between suspend points and the OS force-kills the process as a GPU
         * hang (GPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_RUN_ASYNC) - which is exactly
         * what happened before this call existed. One per submitted DCB,
         * matching the historical #27 present path's SubmitDcb -> SuspendPoint
         * sequence. */
        int32_t sp_rc = sceAgcSuspendPoint();
        if (tracing)
            evo_boot_log("agc frame_end slot=%u suspend_rc=%d", slot, sp_rc);
        if (sp_rc != 0) {
            printf(EVO_AGC_LOG_PREFIX "sceAgcSuspendPoint failed: %d\n", sp_rc);
        }

        /* Mode 3: no SetFlip went into the DCB. Wait for this frame's own fence
         * so we never hand VideoOut a half-drawn buffer, then flip from the CPU
         * - the path already proven to reach the panel. */
        {
            unsigned waits = 0;
            for (; waits < 500; ++waits) {
                evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);
                if (*g_agc_dev.fences[slot] == marker)
                    break;
                sceKernelUsleep(1000);
            }
            int32_t fliprc = sceVideoOutSubmitFlip(g_agc_dev.video_handle,
                                                   g_agc_dev.active_backbuffer,
                                                   1 /* VSYNC */,
                                                   (int64_t)g_agc_dev.flip_arg);
            g_agc_dev.fence_expect[slot] = 0; /* already waited; don't re-wait */

            /*
             * Wait for the flip to actually retire before this frame ends.
             *
             * sceVideoOutSubmitFlip is asynchronous, and there are only two
             * scanout buffers. Without this wait the sequence is: flip A, then
             * clear/draw/flip B, then clear A again - while A may still be the
             * buffer the display is scanning out. The CPU memset then wipes the
             * live framebuffer top to bottom, which on screen is a black band
             * sweeping down the picture, worst while something slow is loading
             * and frames are queueing. The counters showed nothing because no
             * draw was ever dropped: the content was correct, it was being
             * erased after the fact.
             *
             * ps5-opengl's native runtime does exactly this poll
             * (wait_for_flip_marker: get_flip_status until status[3] == marker).
             * status[3] carries the flip_arg of the last completed flip.
             * Bounded so a stalled display degrades to tearing, not a hang.
             */
            if (fliprc == 0) {
                uint64_t status[16];
                unsigned fwaits = 0;
                for (; fwaits < 120u; ++fwaits) {
                    memset(status, 0, sizeof(status));
                    if (sceVideoOutGetFlipStatus(g_agc_dev.video_handle, status) == 0 &&
                        status[3] == (uint64_t)g_agc_dev.flip_arg)
                        break;
                    sceVideoOutWaitVblank(g_agc_dev.video_handle);
                }
                g_agc_dev.flip_waits += fwaits;
                if (fwaits >= 120u)
                    g_agc_dev.flip_timeouts++;
            }

            /* Frame 40: late enough that RmlUi has drawn a real screen, and the
             * fence above guarantees the GPU is done with this buffer. */
            static int s_dumped = 0;
            if (!s_dumped && g_agc_dev.frame_counter >= 40) {
                s_dumped = 1;
                evo_agc_runtime_cache_flush(
                    g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer],
                    (size_t)g_agc_dev.width * (size_t)g_agc_dev.height * 4u);
                agc_dump_scanout((const uint32_t *)
                                     g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer],
                                 g_agc_dev.width, g_agc_dev.height);
            }
            if (tracing)
                evo_boot_log("agc frame_end slot=%u cpu_flip buf=%d waits=%u "
                             "fence=%u marker=%u rc=%d",
                             slot, g_agc_dev.active_backbuffer, waits,
                             (unsigned)*g_agc_dev.fences[slot],
                             (unsigned)marker, fliprc);
        }
    }

    /* 5. Flip buffers and advance slot */
    g_agc_dev.active_backbuffer = 1 - g_agc_dev.active_backbuffer;
    g_agc_dev.current_slot = (g_agc_dev.current_slot + 1) % EVO_AGC_FRAME_SLOTS;
    g_agc_dev.frame_counter++;
    g_agc_dev.frame_active = 0;
}

void evo_agc_runtime_present(void)
{
    evo_agc_runtime_frame_end();
}

/*
 * Mark the buffer being drawn now as carrying UI pixels, so the next frame that
 * reuses it clears first. Called by the render loop after the UI pass on the
 * player screen; harmless outside player mode, where every frame clears anyway.
 */
void evo_agc_runtime_note_ui_drawn(void)
{
    if (!g_agc_dev.initialized)
        return;
    g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer] = 1;
}

void evo_agc_runtime_set_player_mode(int is_player)
{
    if (g_agc_dev.is_player_mode == is_player)
        return;
    g_agc_dev.is_player_mode = is_player;
    if (is_player) {
        /* Clear both scanout buffers once upon entering player mode so letterbox borders are dark */
        for (int b = 0; b < 2; ++b) {
            uint32_t *buf = (uint32_t *)g_agc_dev.scanout_buffers[b];
            if (buf) {
                uint64_t val = (uint64_t)0xff100d0d | ((uint64_t)0xff100d0d << 32);
                uint64_t *p64 = (uint64_t *)buf;
                size_t count = ((size_t)g_agc_dev.width * (size_t)g_agc_dev.height) / 2;
                for (size_t i = 0; i < count; ++i) p64[i] = val;
                evo_agc_runtime_cache_flush(buf, count * sizeof(uint64_t));
            }
        }
    }
}

void evo_agc_runtime_read_scanout(uint32_t *bgra, int width, int height)
{
    if (!g_agc_dev.initialized || !bgra || width <= 0 || height <= 0)
        return;
    /* frame_end flips active_backbuffer AFTER handing the just-rendered one to
     * VideoOut, so the buffer actually on screen is the other one. */
    const int front = 1 - g_agc_dev.active_backbuffer;
    const uint32_t *src = (const uint32_t *)g_agc_dev.scanout_buffers[front];
    if (!src)
        return;
    /* The GPU wrote this buffer; the CPU's copy of those lines is stale. Evict
     * before reading or every capture returns whatever the CPU last put there
     * (i.e. the backdrop clear), which would make a working GPU frame look
     * black in a screenshot. */
    evo_agc_runtime_cache_flush(src,
                                (size_t)g_agc_dev.width * (size_t)g_agc_dev.height * 4u);
    int w = width  < g_agc_dev.width  ? width  : g_agc_dev.width;
    int h = height < g_agc_dev.height ? height : g_agc_dev.height;
    for (int y = 0; y < h; ++y)
        memcpy(bgra + (size_t)y * width, src + (size_t)y * g_agc_dev.width,
               (size_t)w * sizeof(uint32_t));
}

void evo_agc_runtime_note_draw(void)
{
    g_agc_dev.frame_has_draws = 1;
}

void evo_agc_runtime_note_drop(int kind)
{
    if (kind == 0)
        g_agc_dev.ring_alloc_fail++;
    else
        g_agc_dev.tex_alloc_fail++;
}

evo_agc_user_data_layout_t evo_agc_runtime_get_user_data_layout(int pipeline_id)
{
    evo_agc_user_data_layout_t empty = {0, 0, -1, -1, -1, -1};
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT ||
        !g_agc_dev.pipelines[pipeline_id].valid)
        return empty;
    return g_agc_dev.pipelines[pipeline_id].user_data;
}

uint64_t evo_agc_runtime_get_pipe_draw_modifier(int pipeline_id)
{
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT ||
        !g_agc_dev.pipelines[pipeline_id].valid)
        return 0;
    return g_agc_dev.pipelines[pipeline_id].draw_modifier;
}

SceAgcCommandBuffer *evo_agc_runtime_get_current_cb(void)
{
    return g_agc_dev.frame_active ? &g_agc_dev.current_cb : NULL;
}

evo_agc_transient_ring_t *evo_agc_runtime_get_transient_ring(void)
{
    return &g_agc_dev.transient_ring;
}

uint32_t evo_agc_runtime_get_current_slot(void)
{
    return g_agc_dev.current_slot;
}

void evo_agc_runtime_get_size(int *width, int *height)
{
    if (width) *width = g_agc_dev.width;
    if (height) *height = g_agc_dev.height;
}

int evo_agc_runtime_is_display_hdr(void)
{
    return g_agc_dev.initialized ? g_agc_dev.display_is_hdr : 0;
}

int evo_agc_runtime_get_display_dynamic_range(void)
{
    return g_agc_dev.initialized ? g_agc_dev.display_dynamic_range : 0;
}

static int stage_plane(evo_agc_transient_ring_t *ring, uint32_t slot,
                       const uint8_t *src, int src_pitch,
                       uint32_t width, uint32_t height, uint32_t bpp,
                       int is_direct,
                       uint32_t *out_pitch, uint64_t *out_gpu)
{
    if (!ring || !src || src_pitch <= 0 || width == 0 || height == 0 || bpp == 0) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t row_bytes = width * bpp;
    if ((uint32_t)src_pitch < row_bytes) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t pitch = (row_bytes + 255u) & ~255u;

    if (is_direct && ((uintptr_t)src & 255u) == 0u && ((uint32_t)src_pitch & 255u) == 0u) {
        *out_pitch = (uint32_t)src_pitch;
        *out_gpu = (uint64_t)(uintptr_t)src;
        return 0;
    }

    size_t total_bytes = (size_t)pitch * (size_t)height;
    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(ring, slot, total_bytes, 256, &slice) != EVO_AGC_TRANSIENT_OK) {
        *out_pitch = pitch;
        *out_gpu = 0;
        return -1;
    }

    uint8_t *dst = (uint8_t *)slice.cpu;
    for (uint32_t r = 0; r < height; ++r) {
        memcpy(dst + (size_t)r * pitch, src + (size_t)r * src_pitch, row_bytes);
    }
    evo_agc_runtime_cache_flush(slice.cpu, total_bytes);

    *out_pitch = pitch;
    *out_gpu = slice.gpu_addr;
    return 0;
}

static int stage_planar_uv_to_rg16(evo_agc_transient_ring_t *ring, uint32_t slot,
                                   const uint8_t *u, int u_pitch,
                                   const uint8_t *v, int v_pitch,
                                   uint32_t cw2, uint32_t ch2,
                                   uint32_t *out_pitch, uint64_t *out_gpu)
{
    if (!ring || !u || !v || u_pitch <= 0 || v_pitch <= 0 || cw2 == 0 || ch2 == 0) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    /* cw2 chroma pixels per row. Each pixel has a 16-bit U and a 16-bit V (4 bytes total). */
    uint32_t row_bytes = cw2 * 4u;
    if ((uint32_t)u_pitch < cw2 * 2u || (uint32_t)v_pitch < cw2 * 2u) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t pitch = (row_bytes + 255u) & ~255u;
    size_t total_bytes = (size_t)pitch * (size_t)ch2;

    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(ring, slot, total_bytes, 256, &slice) != EVO_AGC_TRANSIENT_OK) {
        if (out_pitch) *out_pitch = pitch;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint8_t *dst = (uint8_t *)slice.cpu;
    for (uint32_t r = 0; r < ch2; ++r) {
        const uint16_t * __restrict src_u = (const uint16_t *)(u + (size_t)r * (size_t)u_pitch);
        const uint16_t * __restrict src_v = (const uint16_t *)(v + (size_t)r * (size_t)v_pitch);
        uint32_t * __restrict dst_row = (uint32_t *)(dst + (size_t)r * (size_t)pitch);
        for (uint32_t c = 0; c < cw2; ++c) {
            dst_row[c] = (uint32_t)src_u[c] | ((uint32_t)src_v[c] << 16);
        }
    }
    evo_agc_runtime_cache_flush(slice.cpu, total_bytes);

    if (out_pitch) *out_pitch = pitch;
    if (out_gpu) *out_gpu = slice.gpu_addr;
    return 0;
}

/*
 * Composite the CPU-rasterised OSD over whatever is already in the frame.
 *
 * main.c rasterises the playback OSD into gl_scratch and hands it to
 * evo_gl_composite_bgra(). In --agc builds that symbol resolves to the no-op in
 * evo_gl_context_stub.c, so the OSD was rendered every frame and then thrown
 * away - video played with no scrub bar, no subtitles, no HUD. The stub now
 * forwards here.
 *
 * `fb` is 0xAABBGGRR, which little-endian is the byte order R,G,B,A - exactly
 * what the texture descriptor's XYZW selects expect, so no swizzle is needed.
 * Alpha is premultiplied (see evo_rmlui_render.cpp), hence BLEND_PREMULTIPLIED.
 *
 * `upload` is main.c's "the OSD actually changed" hint: the 8 MB copy is skipped
 * when it has not, and the previous contents are drawn again.
 */
void evo_agc_composite_bgra(const uint32_t *fb, int w, int h, int upload)
{
    if (!g_agc_dev.initialized || !fb || w <= 0 || h <= 0 ||
        !g_agc_dev.composite_pixels || !g_agc_dev.frame_active)
        return;
    if (w > g_agc_dev.width || h > g_agc_dev.height)
        return;

    const uint32_t pitch = g_agc_dev.composite_pitch;
    if (upload) {
        for (int row = 0; row < h; ++row)
            memcpy(g_agc_dev.composite_pixels + (size_t)row * pitch,
                   fb + (size_t)row * w, (size_t)w * 4u);
        evo_agc_runtime_cache_flush(g_agc_dev.composite_pixels,
                                    (size_t)pitch * (size_t)h);
    }

    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;

    /* Full-screen quad in pixel space, white vertex colour so the texture
     * passes through untinted. */
    struct { float x, y; uint32_t rgba; float u, v; } *quad =
        (void *)g_agc_dev.composite_quad;
    quad[0] = (typeof(*quad)){0.0f,        0.0f,        0xffffffffu, 0.0f, 0.0f};
    quad[1] = (typeof(*quad)){(float)w,    0.0f,        0xffffffffu, 1.0f, 0.0f};
    quad[2] = (typeof(*quad)){0.0f,        (float)h,    0xffffffffu, 0.0f, 1.0f};
    quad[3] = (typeof(*quad)){(float)w,    (float)h,    0xffffffffu, 1.0f, 1.0f};
    evo_agc_runtime_cache_flush(quad, 4 * 20);

    evo_agc_transient_slice_t cons, cons_d, vsh, tex_d;
    if (evo_agc_transient_ring_alloc(ring, slot, 80, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48, 16, &tex_d) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return;
    }

    float *m = (float *)cons.cpu;                 /* pixel space -> NDC */
    memset(m, 0, 80);
    m[0]  =  2.0f / (float)g_agc_dev.width;
    m[5]  = -2.0f / (float)g_agc_dev.height;
    m[10] =  1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;

    evo_agc_build_constant_vsharp((uint32_t *)cons_d.cpu, cons.gpu_addr, 80);
    evo_agc_build_vsharp((uint32_t *)vsh.cpu,
                         (uint64_t)(uintptr_t)g_agc_dev.composite_quad, 20, 4);
    if (evo_agc_build_tsharp_rgba8((uint32_t *)tex_d.cpu,
                                   (uint64_t)(uintptr_t)g_agc_dev.composite_pixels,
                                   (uint32_t)w, (uint32_t)h, pitch) != 0) {
        g_agc_dev.tex_alloc_fail++;
        return;
    }
    evo_agc_build_ssharp((uint32_t *)tex_d.cpu + 8, 1 /* clamp */, 0 /* point */);

    g_agc_dev.bound_pipeline = -1;                /* video pipeline was bound */
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    const evo_agc_user_data_layout_t ud =
        evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 ||
        ud.vs_vertex_table_dword < 0 || ud.ps_texture_table_dword < 0 ||
        ud.vs_count > 16 || ud.ps_count > 16)
        return;

    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword]  = (uint32_t)cons_d.gpu_addr;
    vs_user[ud.vs_vertex_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, ud.vs_count);

    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_d.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices);
    evo_agc_runtime_note_draw();
}

void evo_agc_blit_yuv(const uint8_t *y,  int y_pitch,
                      const uint8_t *uv, int uv_pitch,
                      const uint8_t *u,  int u_pitch,
                      const uint8_t *v,  int v_pitch,
                      int coded_w, int coded_h,
                      int disp_w, int disp_h,
                      int view_mode, int ten_bit, int color_trc,
                      int is_direct)
{
    if (!g_agc_dev.initialized || !y || y_pitch <= 0 || coded_w <= 0 || coded_h <= 0)
        return;
    const int planar = (uv == NULL);
    if (planar ? (!u || !v) : (uv == NULL))
        return;

    evo_agc_runtime_frame_begin();

    const uint32_t slot = g_agc_dev.current_slot;
    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;

    /* 1. Select Pipeline */
    int pipe_id;
    if (ten_bit) {
        if (color_trc == 16)
            pipe_id = EVO_AGC_PIPE_VIDEO_HDR;
        else if (color_trc == 18)
            pipe_id = EVO_AGC_PIPE_VIDEO_HLG;
        else
            pipe_id = EVO_AGC_PIPE_VIDEO_HDR;
    } else {
        pipe_id = planar ? EVO_AGC_PIPE_VIDEO_PLANAR : EVO_AGC_PIPE_VIDEO_NV12;
    }
    evo_agc_runtime_bind_pipeline(pipe_id);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);

    /* 2. Fullscreen viewport and scissor */
    evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12), 0.0f, 0.0f,
                                (float)g_agc_dev.width, (float)g_agc_dev.height);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    /* 3. Compute VideoConstants (Crop & Aspect Scale) */
    struct {
        float crop[2];
        float scale[2];
    } constants;

    float cx = (disp_w > 0 && disp_w <= coded_w) ? (float)disp_w / (float)coded_w : 1.0f;
    float cy = (disp_h > 0 && disp_h <= coded_h) ? (float)disp_h / (float)coded_h : 1.0f;
    constants.crop[0] = cx;
    constants.crop[1] = cy;

    float sx = 1.0f, sy = 1.0f;
    if (view_mode != 2 && disp_w > 0 && disp_h > 0 && g_agc_dev.width > 0 && g_agc_dev.height > 0) {
        float va = (float)disp_w / (float)disp_h;
        float sa = (float)g_agc_dev.width / (float)g_agc_dev.height;
        if (view_mode == 0) { /* FIT (letterbox) */
            if (va > sa) sy = sa / va; else sx = va / sa;
        } else {               /* FILL (crop overflow) */
            if (va > sa) sx = va / sa; else sy = sa / va;
        }
    }
    constants.scale[0] = sx;
    constants.scale[1] = sy;

    /* Allocate VideoConstants in transient ring (64 bytes) */
    evo_agc_transient_slice_t const_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 64, 16, &const_slice) != EVO_AGC_TRANSIENT_OK)
        return;
    memcpy(const_slice.cpu, &constants, sizeof(constants));

    /* Build uniform buffer V# descriptor in transient ring (16 bytes) */
    evo_agc_transient_slice_t vsharp_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsharp_slice) != EVO_AGC_TRANSIENT_OK)
        return;
    evo_agc_build_constant_vsharp((uint32_t *)vsharp_slice.cpu, const_slice.gpu_addr, sizeof(constants));

    /* VS user SGPR (GS stage: compact register 0x8c).
     *
     * Slots come from the compiled pipeline's PAL metadata, never from
     * constants here - this used to hardcode the psbc layout (const buffer at
     * dword 2 plus an ngg_lds_layout at 3), and LLPC puts the constant table at
     * dword 1 with no ngg_lds entry at all. Writing a pointer to the wrong
     * dword is silent: the shader reads its crop/scale through an unset pointer
     * and the quad lands nowhere. */
    const evo_agc_user_data_layout_t vud =
        evo_agc_runtime_get_user_data_layout(pipe_id);
    if (!vud.vs_count || vud.vs_const_table_dword < 0 ||
        vud.ps_texture_table_dword < 0 || vud.vs_count > 16 || vud.ps_count > 16)
        return;

    uint32_t vs_user[16] = {0};
    vs_user[vud.vs_const_table_dword] = (uint32_t)vsharp_slice.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, vud.vs_count);

    /* 4. Prepare Texture Descriptors (T#) and Samplers (S#) */
    const uint32_t bpp = ten_bit ? 2u : 1u;
    uint32_t y_pitch_gpu = 0;
    uint64_t y_gpu = 0;
    if (stage_plane(ring, slot, y, y_pitch, coded_w, coded_h, bpp, is_direct, &y_pitch_gpu, &y_gpu) != 0) {
        evo_boot_log("agc_blit_yuv: stage Y plane failed");
        evo_boot_log_flush();
        return;
    }

    evo_agc_transient_slice_t desc_slice;
    if (planar && !ten_bit) {
        /* Planar 3-plane (SDR): 144 bytes descriptor table (3 * 48B) */
        if (evo_agc_transient_ring_alloc(ring, slot, 144, 16, &desc_slice) != EVO_AGC_TRANSIENT_OK) {
            evo_boot_log("agc_blit_yuv: desc_slice alloc failed");
            evo_boot_log_flush();
            return;
        }
        uint32_t *desc = (uint32_t *)desc_slice.cpu;
        memset(desc, 0, 144);

        uint32_t u_pitch_gpu = 0, v_pitch_gpu = 0;
        uint64_t u_gpu = 0, v_gpu = 0;
        uint32_t cw2 = (uint32_t)(coded_w / 2);
        uint32_t ch2 = (uint32_t)(coded_h / 2);
        if (stage_plane(ring, slot, u, u_pitch, cw2, ch2, 1u, is_direct, &u_pitch_gpu, &u_gpu) != 0) {
            evo_boot_log("agc_blit_yuv: stage U plane failed");
            evo_boot_log_flush();
            return;
        }
        if (stage_plane(ring, slot, v, v_pitch, cw2, ch2, 1u, is_direct, &v_pitch_gpu, &v_gpu) != 0) {
            evo_boot_log("agc_blit_yuv: stage V plane failed");
            evo_boot_log_flush();
            return;
        }

        /* Binding 0: Y plane */
        int r0 = evo_agc_build_tsharp_r8(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu);
        evo_agc_build_ssharp(desc + 8, 1, 1);

        /* Binding 1: U plane (offset 48 = 12 dwords) */
        int r1 = evo_agc_build_tsharp_r8(desc + 12, u_gpu, cw2, ch2, u_pitch_gpu);
        evo_agc_build_ssharp(desc + 20, 1, 1);

        /* Binding 2: V plane (offset 96 = 24 dwords) */
        int r2 = evo_agc_build_tsharp_r8(desc + 24, v_gpu, cw2, ch2, v_pitch_gpu);
        evo_agc_build_ssharp(desc + 32, 1, 1);

        if (r0 != 0 || r1 != 0 || r2 != 0) {
            evo_boot_log("agc_blit_yuv: build tsharp failed rc=%d/%d/%d", r0, r1, r2);
            evo_boot_log_flush();
            return;
        }
    } else {
        /* NV12 / P010 2-plane: 96 bytes descriptor table (2 * 48B).
         * Used for NV12 (SDR 8-bit), NV12_10 (HDR 10-bit), and planar 10-bit (interleaved to RG16). */
        if (evo_agc_transient_ring_alloc(ring, slot, 96, 16, &desc_slice) != EVO_AGC_TRANSIENT_OK) {
            evo_boot_log("agc_blit_yuv: 2-plane desc_slice alloc failed");
            evo_boot_log_flush();
            return;
        }
        uint32_t *desc = (uint32_t *)desc_slice.cpu;
        memset(desc, 0, 96);

        uint32_t uv_pitch_gpu = 0;
        uint64_t uv_gpu = 0;
        uint32_t cw2 = (uint32_t)(coded_w / 2);
        uint32_t ch2 = (uint32_t)(coded_h / 2);

        if (ten_bit && planar) {
            if (stage_planar_uv_to_rg16(ring, slot, u, u_pitch, v, v_pitch, cw2, ch2, &uv_pitch_gpu, &uv_gpu) != 0) {
                evo_boot_log("agc_blit_yuv: stage planar UV to RG16 failed");
                evo_boot_log_flush();
                return;
            }
        } else {
            uint32_t uv_bpp = bpp * 2u;
            if (stage_plane(ring, slot, uv, uv_pitch, cw2, ch2, uv_bpp, is_direct, &uv_pitch_gpu, &uv_gpu) != 0) {
                evo_boot_log("agc_blit_yuv: stage UV plane failed");
                evo_boot_log_flush();
                return;
            }
        }

        /* Binding 0: Y plane */
        int r0 = ten_bit ? evo_agc_build_tsharp_r16(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu)
                         : evo_agc_build_tsharp_r8(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu);
        evo_agc_build_ssharp(desc + 8, 1, 1);

        /* Binding 1: UV plane (offset 48 = 12 dwords) */
        int r1 = ten_bit ? evo_agc_build_tsharp_rg16(desc + 12, uv_gpu, cw2, ch2, uv_pitch_gpu)
                         : evo_agc_build_tsharp_rg8(desc + 12, uv_gpu, cw2, ch2, uv_pitch_gpu);
        evo_agc_build_ssharp(desc + 20, 1, 1);

        if (r0 != 0 || r1 != 0) {
            evo_boot_log("agc_blit_yuv: build 2-plane tsharp failed rc=%d/%d", r0, r1);
            evo_boot_log_flush();
            return;
        }
    }

    /* PS user SGPR (PS stage: compact register 0x0c) - same derivation. */
    uint32_t ps_user[16] = {0};
    ps_user[vud.ps_texture_table_dword] = (uint32_t)desc_slice.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, vud.ps_count);

    /* 5. Dispatch hardware quad draw call with pipeline draw modifier */
    evo_agc_writer_draw_index_modifier(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices,
                                      g_agc_dev.pipelines[pipe_id].draw_modifier);
    /* Without this the frame carries no recorded draw and frame_end discards it
     * instead of presenting - video would decode and never reach the panel. */
    evo_agc_runtime_note_draw();
}
