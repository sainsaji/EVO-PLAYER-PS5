/*
 * pp_agc.c - see pp_agc.h. Ported from ProsperoLight native_agc_present.cpp.
 * Real body only under EVO_APP_MODULE.
 *
 * #27 GPU Step 2:
 *   - pp_agc_init      : sceAgcInit -> shader scratch -> CreateShader x2 ->
 *                        LinkShaders (hardware-verified 2026-09-03).
 *   - agc_render_frame : verbatim port of ProsperoLight render_frame(), minus
 *                        the HUD / keyboard overlay branch. One call = one
 *                        NV12->RGB fullscreen convert + queued flip.
 *   - pp_agc_present_nv12 : reshaped to take EVO's *own* sceVideoOut handle +
 *                        an already-acquired GPU plane. Never sceVideoOutOpen
 *                        (two opens is bad; the sceVideoOutOpen->compute-queue
 *                        panic vector; #31's resident decoder holds a queue).
 *
 * The decoder hands us an NV12 copy in flexible memory (malloc / PROT_RW) which
 * the GPU cannot sample. pp_agc_present_nv12 stages each frame into a
 * per-VO-buffer direct-memory scratch (prot 0x33) before the draw.
 */
#include "pp_agc.h"
#include "pp_videoout.h"
#include "evo_boot_log.h"

#ifdef EVO_APP_MODULE

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* --- sceAgc / libSceAgcDriver / libkernel: stub-linked (package-app.sh 6b) - */
extern int32_t  sceAgcInit(void *state, uint32_t defaults_revision);
extern int32_t  sceAgcCreateShader(void **shader, void *header, void *code);
extern int32_t  sceAgcLinkShaders(void *cx, void *uc, void *reserved,
                                  void *vertex_shader, void *pixel_shader,
                                  uint32_t primitive_type);
extern void    *sceAgcGetRegisterDefaults(void);
extern uint32_t *sceAgcDcbSetCxRegistersIndirect(void *command, const void *registers, uint32_t count);
extern uint32_t *sceAgcDcbSetShRegistersIndirect(void *command, const void *registers, uint32_t count);
extern uint32_t *sceAgcDcbSetUcRegistersIndirect(void *command, const void *registers, uint32_t count);
extern uint32_t *sceAgcCbSetShRegisterRangeDirect(void *command, uint32_t offset,
                                                  const uint32_t *values, uint32_t count);
extern uint32_t *sceAgcDcbDrawIndexAuto(void *command, uint32_t count, uint64_t modifier);
/* #28 Phase 4: indexed-draw builders (SharpProspero DrawCommandBuffer.cs). */
extern uint32_t *sceAgcDcbSetCxRegisterDirect(void *command, uint64_t packed_reg_value);
extern uint32_t *sceAgcDcbSetIndexSize(void *command, uint8_t index_size, uint8_t cache_policy);
extern uint32_t *sceAgcDcbSetIndexBuffer(void *command, void *index_addr);
extern uint32_t *sceAgcDcbSetIndexCount(void *command, uint32_t index_count);
extern uint32_t *sceAgcDcbDrawIndex(void *command, uint32_t index_count, void *index_addr,
                                    uint64_t modifier);
/* sceAgcDcbDrawIndexOffset: NOT used - the geo path draws with sceAgcDcbDrawIndex
 * and an explicit sub-range address. Do NOT add it to libSceAgc.syms: its NID
 * does not resolve on 12.70 and a dead positional import bricks the app at load
 * (CE-108255-1, #28 2026-09-04). */
extern uint32_t *sceAgcDcbSetFlip(void *command, uint32_t handle, int buffer_index,
                                  uint32_t flip_mode, int64_t flip_argument);
extern int32_t  sceAgcDriverSubmitDcb(void *description);
extern int32_t  sceAgcSuspendPoint(void);
extern uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
extern uint32_t sceAgcDriverWaitUntilSafeForRendering(uint32_t **command, uint32_t packet_size,
                                                      uint32_t reserved, uint32_t handle,
                                                      int buffer_index);

extern int64_t  sceKernelGetDirectMemorySize(void);
extern int32_t  sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int32_t  sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int32_t  sceKernelMunmap(void *, size_t);
extern int32_t  sceKernelReleaseDirectMemory(int64_t, size_t);

/* --- embedded blobs (agc_blobs.S) --------------------------------------- */
extern const uint8_t pp_agc_geometry_header_start[], pp_agc_geometry_header_end[];
extern const uint8_t pp_agc_geometry_code_start[],   pp_agc_geometry_code_end[];
extern const uint8_t pp_agc_pixel_header_start[],    pp_agc_pixel_header_end[];
extern const uint8_t pp_agc_pixel_code_start[],      pp_agc_pixel_code_end[];
extern const uint8_t pp_agc_pixel_hdr_code_start[],  pp_agc_pixel_hdr_code_end[];
extern const uint8_t pp_agc_resources_start[],       pp_agc_resources_end[];

/* --- embedded UI shader blobs (#28 Phase 0, agc_ui_blobs.S) ------------- */
extern const uint8_t pp_agc_ui_vs_header_start[],   pp_agc_ui_vs_header_end[];
extern const uint8_t pp_agc_ui_ps_header_start[],   pp_agc_ui_ps_header_end[];
extern const uint8_t pp_agc_ui_vs_code_start[],     pp_agc_ui_vs_code_end[];
extern const uint8_t pp_agc_solid_ps_code_start[],  pp_agc_solid_ps_code_end[];
extern const uint8_t pp_agc_glyph_ps_code_start[],  pp_agc_glyph_ps_code_end[];
extern const uint8_t pp_agc_rgba_ps_code_start[],   pp_agc_rgba_ps_code_end[];
extern const uint8_t pp_agc_spvs_header_start[], pp_agc_spvs_header_end[];
extern const uint8_t pp_agc_spvs_code_start[],   pp_agc_spvs_code_end[];
extern const uint8_t pp_agc_spps_header_start[], pp_agc_spps_header_end[];
extern const uint8_t pp_agc_spps_code_start[],   pp_agc_spps_code_end[];
extern const uint8_t pp_agc_blit_ps_header_start[], pp_agc_blit_ps_header_end[];
extern const uint8_t pp_agc_blit_ps_code_start[],   pp_agc_blit_ps_code_end[];

/* --- constants (ProsperoLight) ----------------------------------------- */
/* #28 Phase 4: the GPU text-overlay 2nd pass in agc_render_geo.
 * DEFAULT 0 - compiling the pass body into agc_render_geo crashes EVO at LOAD
 * on hardware (2026-09-04), even though the code never executes without the
 * /mnt/usb0/evo_agc_geo_text hook. Bisected to: any of the pass's CPU-side
 * setup (fopen + shader-header reads + register memcpys + the NADD block) is
 * enough to brick boot. It is NOT a logic bug (the path is unreachable) - it's
 * toolchain/loader-level (native_app_builder ELF->eboot, or -fdata-sections
 * section handling, or a function-size threshold). Needs a separate
 * investigation: try the pass in its own .c file, or diff the eboot segment
 * table. Build with -DPP_AGC_GEO_TEXT=1 only for that. */
#ifndef PP_AGC_GEO_TEXT
#define PP_AGC_GEO_TEXT 0
#endif

#define SHADER_MEMORY_BYTES  0x0d0000u
#define SHADER_STATIC_BYTES  0x10000u        /* flushed before every submit    */
#define DIRECT_MEMORY_TYPE   12
#define MAP_PROTECTION       0x33            /* CPU + GPU R/W                   */
#define SHADER_ALIGN         0x4000u
#define BASE_OUTPUT_WIDTH    1920u
#define BASE_OUTPUT_HEIGHT   1080u
#define TV_SAFE_INSET_X      64u
#define TV_SAFE_INSET_Y      36u
#define AGC_BILINEAR_SAMPLER_WORD 0x09500000u  /* kNativeAgcBilinearSamplerWord */

/* shader_memory fixed layout (agc-implementation.md §3):
 *   0x0000 geometry header   0x1000 pixel header   0x2000 pixel code
 *   0x3700 geometry code     0x5000/0x6000 link scratch   0xc000 resources   */
#define OFF_GEO_HDR   0x0000u
#define OFF_PIX_HDR   0x1000u
#define OFF_PIX_CODE  0x2000u
#define OFF_GEO_CODE  0x3700u
#define OFF_LINK_A    0x5000u
#define OFF_LINK_B    0x6000u
#define OFF_RESOURCES 0xc000u

/* #28 Phase 1 probe: scratch for the UI shader create/link go/no-go check.
 * Well above the video path's 0x8000..0xc000 DCB + resources; g_agc.mem is
 * 0xD0000, so 0xd000..0x15000 is free headroom. */
#define OFF_UI_VS_HDR    0x0d000u
#define OFF_UI_VS_CODE   0x0e000u
#define OFF_UI_PS_HDR    0x0f000u
#define OFF_UI_PS_A_CODE 0x10000u   /* solid_ps */
#define OFF_UI_PS_B_CODE 0x11000u   /* glyph_ps */
#define OFF_UI_PS_C_CODE 0x12000u   /* rgba_ps  */
#define OFF_UI_LINK_A    0x13000u
#define OFF_UI_LINK_B    0x14000u
#define OFF_UI_SPLICE_A  0x15000u   /* Phase 4 probe scratch: sp_mesh_{vs,ps} */
/* #67: ripped blit_ps, paired with the already-created g_agc.vs (well clear
 * of every offset above - highest in-use address is OFF_GEO_SH + 0x1000;
 * SHADER_MEMORY_BYTES is 0xd0000, plenty of room). HDR/CODE are read-only
 * CreateShader input, reused by both the one-shot --agc-probe check and
 * agc_geo_init(); LINK_A/B there are throwaway probe scratch. The real
 * render path needs its OWN persistent link storage (read every frame, like
 * OFF_MESH_LINK_CX/UC below) so a probe run can never clobber it. */
#define OFF_BLIT_PS_HDR       0x30000u
#define OFF_BLIT_PS_CODE      0x31000u
#define OFF_BLIT_LINK_A       0x32000u   /* --agc-probe scratch only */
#define OFF_BLIT_LINK_B       0x33000u   /* --agc-probe scratch only */
#define OFF_BLIT_GEO_LINK_CX  0x34000u   /* persistent: agc_geo_init -> every geo-text frame */
#define OFF_BLIT_GEO_LINK_UC  0x35000u

/* #28 Phase 4 geometry path - permanent homes, clear of the video path's
 * 0x0..0xd000 and the probe scratch at 0xd000..0x19000. g_agc.mem is 0xd0000. */
#define OFF_MESH_VS_HDR   0x1a000u
#define OFF_MESH_VS_CODE  0x1b000u
#define OFF_MESH_PS_HDR   0x1c000u
#define OFF_MESH_PS_CODE  0x1d000u
#define OFF_MESH_LINK_CX  0x1e000u   /* sceAgcLinkShaders 34-reg cx block      */
#define OFF_MESH_LINK_UC  0x1f000u   /* sceAgcLinkShaders 3-reg uc prim state  */
#define OFF_GEO_CX        0x20000u   /* per-frame combined cx register block   */
#define OFF_GEO_CB        0x22000u   /* MVP + model constant buffer (128 B)    */
#define OFF_GEO_DCB       0x24000u   /* DCB words, 0x8000 bytes -> 0x2c000     */
#define OFF_GEO_SH        0x2c000u   /* combined vs+ps sh register block       */
#define GEO_DCB_BYTES     0x8000u

/* --- AGC ABI structs (ProsperoLight) ---------------------------------- */
typedef struct agc_register {
    uint16_t offset;
    uint16_t pad;
    uint32_t value;
} agc_register_t;

typedef struct agc_command_buffer {
    uint32_t *bottom;
    uint32_t *top;
    uint32_t *up;
    uint32_t *down;
    uintptr_t callback;
    void     *user_data;
    uint32_t  reserved_dwords;
    uint32_t  pad;
} agc_command_buffer_t;

typedef struct agc_submit_description {
    void    *words;
    uint32_t word_count;
    uint8_t  flag;
    uint8_t  pad[3];
} agc_submit_description_t;

static struct {
    volatile int ready;            /* cleared cross-thread by the watchdog     */
    int       tried;
    uint64_t  state;
    uint8_t  *mem;
    int64_t   mem_start;
    void     *vs;
    void     *ps;
    uint32_t  w, h;

    /* per-VO-buffer NV12 staging in GPU-visible direct memory */
    uint8_t  *stage[PP_VO_MAX_BUFFERS];
    int64_t   stage_start;
    size_t    stage_total;
    size_t    stage_cap;

    /* #28 Phase 1: per-VO-buffer overlay-NV12 staging (the OSD quad). Separate
     * slab because the overlay is a different size and updates independently. */
    uint8_t  *ovl_stage[PP_VO_MAX_BUFFERS];
    int64_t   ovl_stage_start;
    size_t    ovl_stage_total;
    size_t    ovl_stage_cap;

    /* #28 Phase 4: GPU geometry present */
    volatile int geo_ready;        /* mesh shaders created + linked            */
    void     *mesh_vs;
    void     *mesh_ps;
    /* #67: ripped RGBA-blit PS, paired with the video path's g_agc.vs (same
     * fullscreen-quad VS - blit_ps reads UV from attr0 exactly like the NV12
     * pixel shader it sits next to). Created once here, not per-frame. */
    void     *blit_ps;
    uint8_t  *geo_stage[PP_VO_MAX_BUFFERS];  /* [vertices | indices] per VO buf */
    int64_t   geo_stage_start;
    size_t    geo_stage_total;
    size_t    geo_stage_cap;                 /* per-slot capacity              */
    volatile int geo_first_ok;
    volatile int geo_wedged;

    volatile int first_frame_ok;   /* a render_frame has returned 0 (worker)   */
    int       first_frame_logged;
    int       tv_safe;             /* 0 = full-frame (parity with CPU path)  */
    volatile int submit_wedged;    /* a submit blew the 250ms watchdog once    */
    uint64_t  present_calls;       /* pp_agc_present_nv12 entries (test hook)   */
} g_agc;

/* #27 test hook: simulate a mid-clip GPU wedge on frame ~120 when
 * /mnt/usb0/evo_agc_no_present (or EVO_AGC_NO_PRESENT) is present. */
static int agc_test_wedge_due(uint64_t call_index)
{
    static int mode = -1;   /* -1 unknown, 0 off, N = wedge on this frame */
    if (mode < 0) {
        mode = 0;
        if (getenv("EVO_AGC_NO_PRESENT"))
            mode = 120;
        else {
            FILE *f = fopen("/mnt/usb0/evo_agc_no_present", "r");
            if (f) { mode = 120; fclose(f); }
        }
    }
    return mode > 0 && call_index == (uint64_t)mode;
}

/*
 * #27 B: the GPU submit half of the present runs on a dedicated worker thread
 * with a 250 ms watchdog. render_frame's GPU-side stall
 * (sceAgcDriverSubmitDcb -> sceAgcSuspendPoint never returns) is not a CPU
 * fault, so the first-frame sigsetjmp guard can't catch it - it just wedges
 * whatever thread called it. Keeping that off the playback push thread means a
 * wrong GPU-register guess costs a dropped frame + a log line, not a console
 * power-cycle. Single-slot mailbox: the caller stages NV12, fills the request,
 * signals `req`, then pthread_cond_timedwait's on `done`. On timeout the worker
 * is abandoned (still blocked in the syscall - never joined), pp_agc goes
 * permanently unavailable for the session, and playback drops to the CPU path.
 */
static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  req_cv;
    pthread_cond_t  done_cv;
    int             started;
    int             have_req;
    int             have_done;
    int             quit;

    /* request */
    int         video;
    int         buffer_index;
    void       *target;
    const void *source;
    size_t      source_bytes;
    uint32_t    pitch, surface_height;
    uint32_t    visible_width, visible_height;
    uint32_t    output_width, output_height;
    int64_t     render_marker;

    /* #28 overlay quad (0 = video only) */
    int         draw_overlay;
    const void *ovl_source;                     /* staged overlay NV12, pitch == ovl_w */
    uint32_t    ovl_w, ovl_h, ovl_x, ovl_y, ovl_scale;
    float       ovl_alpha;

    /* response */
    int32_t     rc;
    uint32_t    word_count;
} g_agc_submit;

static int agc_submit_start(void);
static void agc_geo_init(void);           /* #28 Phase 4 */
static void agc_geo_staging_free(void);
static int  agc_ovl_staging_ensure(size_t need);
static void agc_bgra_to_nv12(uint8_t *y, uint8_t *uv, const uint32_t *bgra,
                             uint32_t w, uint32_t h);

/* First-frame breadcrumbs -> evo_boot.log, flushed each call, silent once a
 * frame has presented. #27 phase C: pin down where the present path dies when
 * neither the sigsetjmp guard nor the submit watchdog fires. */
#define agc_dbg(...) do { \
        if (!g_agc.first_frame_ok) { \
            evo_boot_log("pp_agc: " __VA_ARGS__); \
            evo_boot_log_flush(); \
        } \
    } while (0)

/* #28 Phase 4: same, but keyed to the geometry path's own first-frame flag so
 * it goes quiet after one good geo frame even when no video has played (else it
 * floods evo_boot.log every menu frame and the USB write dominates the frame). */
#define geo_dbg(...) do { \
        if (!g_agc.geo_first_ok) { \
            evo_boot_log("pp_agc: " __VA_ARGS__); \
            evo_boot_log_flush(); \
        } \
    } while (0)

/* Per-thread fault guard: the decode-thread caller and the submit worker each
 * arm one around their first frame. Handlers are installed once (agc_submit_start)
 * and left in place; an un-armed fault re-raises with default disposition so an
 * unrelated crash still behaves normally. */
static __thread sigjmp_buf t_agc_jmp;
static __thread volatile sig_atomic_t t_agc_armed;

static void agc_fault_h(int sig)
{
    if (t_agc_armed) {
        t_agc_armed = 0;
        siglongjmp(t_agc_jmp, sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int copy_asset(void *dst, size_t cap, const uint8_t *s, const uint8_t *e)
{
    if (e < s)
        return -1;
    size_t n = (size_t)(e - s);
    if (n > cap)
        return -1;
    memcpy(dst, s, n);
    return 0;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* clflush + mfence the CPU-written scratch so the GPU sees it. Mandatory
 * before every SubmitDcb - the scratch is write-back CPU memory. */
static void flush_gpu_data(const void *address, size_t bytes)
{
    const uint8_t *at  = (const uint8_t *)address;
    const uint8_t *end = at + bytes;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static uint8_t agc_out_of_space(agc_command_buffer_t *buffer, uint32_t words, void *user_data)
{
    (void)buffer;
    (void)words;
    (void)user_data;
    return 0;
}

/* prepare_resources: verbatim from ProsperoLight - rebase the NGR1 descriptor
 * table's pointers to the mapped `resources` address, and (SDR) swap the
 * limited-range YUV coefficients for full-range. */
static int prepare_resources(uint8_t *resources, int hdr)
{
    uint32_t *header = (uint32_t *)resources;
    uint32_t table_offsets[2] = { header[1], header[3] };
    static const uint32_t table_counts[2] = { 2, 8 };
    uint32_t *limited_offset = (uint32_t *)(resources + 0x500);
    uint32_t *limited_scale  = (uint32_t *)(resources + 0x600);
    uint32_t *sample_scale   = (uint32_t *)(resources + 0x700);

    if (limited_offset[0] != 0x3d802008u || limited_offset[1] != 0x3d802008u ||
        limited_offset[2] != 0x3d802008u || limited_scale[0]  != 0x3f957abdu ||
        limited_scale[1]  != 0x3f922492u || limited_scale[2]  != 0x3f922492u ||
        sample_scale[0]   != 0x42801f88u)
        return -1;

    if (!hdr) {
        limited_offset[0] = limited_offset[1] = limited_offset[2] = 0x3d808081u;
        limited_scale[0] = 0x3f950a85u;
        limited_scale[1] = limited_scale[2] = 0x3f91b6dbu;
        sample_scale[0]  = 0x3f800000u;
    }

    for (uint32_t t = 0; t < 2; ++t) {
        uint32_t *entry = (uint32_t *)(resources + table_offsets[t]);
        for (uint32_t i = 0; i < table_counts[t]; ++i, entry += 4) {
            uintptr_t addr = (uintptr_t)resources + entry[0];
            entry[0] = (uint32_t)addr;
            entry[1] = (entry[1] & 0xffff0000u) | (uint32_t)(addr >> 32);
        }
    }
    return 0;
}

static int shader_resource_offset(void *shader, unsigned kind, uint32_t *offset)
{
    uint8_t *layout = *(uint8_t **)((uint8_t *)shader + 8);
    uint16_t *counts;
    uint16_t *entries;

    if (!layout || kind >= 4)
        return -1;
    counts = (uint16_t *)(layout + 46);
    if (!counts[kind])
        return -1;
    entries = *(uint16_t **)(layout + 8 + kind * sizeof(void *));
    *offset = entries[0] & 0x7fffu;
    return 0;
}

/* NV12: Y image desc + interleaved-UV image desc + samplers + CB pointer, at
 * SH register 0x0c. Verbatim from ProsperoLight bind_pixel_source(). */
static void bind_pixel_source(agc_command_buffer_t *command, uint8_t *resources,
                              const void *source, size_t y_bytes, size_t uv_bytes,
                              const void *pixel_cb)
{
    uint32_t descriptor[30] = { 0 };
    uintptr_t uv = (uintptr_t)source + y_bytes;
    uint32_t *header = (uint32_t *)resources;
    uintptr_t table = (uintptr_t)resources + header[3];

    descriptor[0] = (uint32_t)(uintptr_t)source;
    descriptor[1] = (uint32_t)((uintptr_t)source >> 32);
    descriptor[2] = (uint32_t)y_bytes;
    descriptor[3] = 0x31016facu;
    descriptor[5] = 0x00700000u;
    descriptor[8] = (uint32_t)uv;
    descriptor[9] = (uint32_t)(uv >> 32);
    descriptor[10] = (uint32_t)uv_bytes;
    descriptor[11] = 0x31016facu;
    descriptor[13] = 0x00700000u;
    descriptor[16] = descriptor[20] = 0x00000092u;
    descriptor[17] = descriptor[21] = 0x00fff000u;
    descriptor[18] = descriptor[22] = AGC_BILINEAR_SAMPLER_WORD;
    descriptor[24] = (uint32_t)(uintptr_t)pixel_cb;
    descriptor[25] = (uint32_t)((uintptr_t)pixel_cb >> 32) | (16u << 16);
    descriptor[26] = 4;
    descriptor[27] = 0x0004dfacu;
    descriptor[28] = (uint32_t)table;
    descriptor[29] = (uint32_t)(table >> 32);
    sceAgcCbSetShRegisterRangeDirect(command, 0x0c, descriptor, 30);
}

/* One presented frame. Port of ProsperoLight render_frame(), SDR path only.
 * The optional overlay quad (ovl_*) is #28's OSD composite - a second
 * DrawIndexAuto in the same DCB, constant-alpha blended, from a tightly-packed
 * NV12 surface at `ovl_source`. Scratch offsets inside `memory`. */
static int agc_render_frame(int video, int buffer_index, void *target, uint8_t *memory,
                            void *vertex_shader, void *pixel_shader,
                            const void *source, size_t source_bytes, uint32_t pitch,
                            uint32_t surface_height, uint32_t visible_width,
                            uint32_t visible_height, uint32_t output_width,
                            uint32_t output_height, int64_t render_marker,
                            uint32_t *word_count,
                            int draw_overlay, const void *ovl_source,
                            uint32_t ovl_w, uint32_t ovl_h, uint32_t ovl_x,
                            uint32_t ovl_y, uint32_t ovl_scale, float ovl_alpha)
{
    static const uint16_t target_offsets[16] = { 0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f,
                                                 0x321, 0x323, 0x324, 0x325, 0x390, 0x398,
                                                 0x3a0, 0x3a8, 0x3b0, 0x3b8 };
    static const uint32_t geometry_constants[16] = {
        0x3fa24ce6, 0, 0,          0x3e2f0fdd, 0, 0x3fa21449, 0, 0x3e111049,
        0,          0, 0xbf800000, 0x80000000, 0, 0,          0, 0x3f800000 };
    static const uint32_t pixel_constants[16] = {
        0x3f800000, 0x3f800000, 0x3f800000, 0, 0x3fed844d, 0xbe3fd0d0, 0, 0,
        0,          0xbeefad6d, 0x3fc9930c, 0, 0,          0,          0, 0 };
    agc_register_t *cx = (agc_register_t *)(memory + 0x7000);
    uint8_t *geometry_cb = memory + 0x7800;
    uint8_t *pixel_cb = memory + 0x7900;
    uint8_t *ovl_pixel_cb = memory + 0x7a00;               /* #28 overlay CB */
    agc_register_t *ovl_state = (agc_register_t *)(memory + 0x7b00); /* 11 regs */
    uint8_t *resources = memory + 0xc000;
    uint32_t *words = (uint32_t *)(memory + 0x8000);
    agc_command_buffer_t command;
    agc_submit_description_t submit;
    uint32_t descriptor[30];
    void *defaults = sceAgcGetRegisterDefaults();
    agc_register_t **blocks;
    uint32_t default_count;
    uint32_t cx_count = 16;
    uint32_t slot;
    size_t y_bytes = (size_t)pitch * surface_height;
    size_t uv_bytes = (size_t)pitch * ((surface_height + 1u) / 2u);
    uint32_t inset_x = g_agc.tv_safe ? output_width * TV_SAFE_INSET_X / BASE_OUTPUT_WIDTH : 0u;
    uint32_t inset_y = g_agc.tv_safe ? output_height * TV_SAFE_INSET_Y / BASE_OUTPUT_HEIGHT : 0u;

    memset(&command, 0, sizeof command);
    memset(&submit, 0, sizeof submit);
    memset(descriptor, 0, sizeof descriptor);

    agc_dbg("R0 render_frame enter  target=%p defaults=%p  ybytes=%zu uvbytes=%zu srcbytes=%zu",
            target, defaults, y_bytes, uv_bytes, source_bytes);

    if (!defaults || visible_width == 0 || visible_height == 0 || visible_width > pitch ||
        visible_height > surface_height || (pitch & 1u) != 0 || y_bytes > UINT32_MAX ||
        uv_bytes > UINT32_MAX || y_bytes + uv_bytes > source_bytes || output_width == 0 ||
        output_height == 0) {
        agc_dbg("R0! validation failed  vis=%ux%u pitch=%u surfh=%u",
                visible_width, visible_height, pitch, surface_height);
        return -1;
    }

    blocks = *(agc_register_t ***)defaults;
    default_count = *(uint32_t *)((uint8_t *)defaults + 0x20);
    for (uint32_t index = 0; index < 16; ++index) {
        cx[index].offset = target_offsets[index];
        cx[index].pad = 0;
        cx[index].value = 0;
        for (uint32_t candidate = 0; blocks && blocks[0] && candidate < default_count; ++candidate) {
            if (blocks[0][candidate].offset == target_offsets[index]) {
                cx[index].value = blocks[0][candidate].value;
                break;
            }
        }
    }

    cx[0].value = (uint32_t)((uintptr_t)target >> 8);
    cx[1].value &= 0xfc001fffu;
    cx[2].value = (cx[2].value & ~(0x7cu | 0x700u | 0x1800u | 0x10000000u | 0x10000u | 0x8000u |
                                   0x40000u | 0x4000u)) |
                  0x28u | 0x8000u;
    cx[3].value &= ~(0x7000u | 0x18000u);
    cx[4].value = (cx[4].value & ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) | 0x48u;
    cx[5].value = cx[6].value = cx[9].value = 0;
    cx[10].value = (cx[10].value & 0xffffff00u) | (uint32_t)((uintptr_t)target >> 40);
    cx[11].value &= 0xffffff00u;
    cx[12].value &= 0xffffff00u;
    cx[13].value &= 0xffffff00u;
    cx[14].value = (output_height - 1u) | ((output_width - 1u) << 14);
    cx[15].value = (cx[15].value & ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) | 0x6c000u |
                   0x01000000u | 0x44000000u;

#define ADD_REG(register_offset, register_value)                                                   \
    do {                                                                                           \
        cx[cx_count].offset = (register_offset);                                                   \
        cx[cx_count].pad = 0;                                                                      \
        cx[cx_count].value = (register_value);                                                     \
        cx_count++;                                                                                \
    } while (0)
    ADD_REG(0x10f, float_bits((output_width - inset_x * 2u) * .5f));
    ADD_REG(0x110, float_bits(output_width * .5f));
    ADD_REG(0x111, float_bits((output_height - inset_y * 2u) * -.5f));
    ADD_REG(0x112, float_bits(output_height * .5f));
    ADD_REG(0x113, float_bits(1));
    ADD_REG(0x114, 0);
    ADD_REG(0x0b4, 0);
    ADD_REG(0x0b5, float_bits(1));
    ADD_REG(0x2fa, float_bits(1));
    ADD_REG(0x2fb, float_bits(1));
    ADD_REG(0x2fc, float_bits(1));
    ADD_REG(0x2fd, float_bits(1));
    ADD_REG(0x090, 0x80000000u);
    ADD_REG(0x091, output_width | (output_height << 16));
    ADD_REG(0x08e, 0x0f);
#undef ADD_REG

    memcpy(geometry_cb, geometry_constants, sizeof(geometry_constants));
    memcpy(pixel_cb, pixel_constants, sizeof(pixel_constants));
    ((uint32_t *)pixel_cb)[12] = float_bits((float)visible_width);
    ((uint32_t *)pixel_cb)[13] = float_bits((float)visible_height);
    ((uint32_t *)pixel_cb)[14] = pitch;
    ((uint32_t *)pixel_cb)[15] = pitch / 2u;

    command.bottom = words;
    command.top = words + 0x4000u / sizeof(*words);
    command.up = words;
    command.down = command.top;
    command.callback = (uintptr_t)agc_out_of_space;

    agc_dbg("R1 cx base done (%u regs) -> WaitUntilSafeForRendering", cx_count);
    sceAgcDriverWaitUntilSafeForRendering(&command.up,
                                          sceAgcDriverGetWaitRenderingPacketSizeInDwords(), 0,
                                          (uint32_t)video, buffer_index);
    agc_dbg("R2 WaitUntilSafeForRendering returned");

    {
        agc_register_t *link_cx = (agc_register_t *)(memory + 0x5000);
        agc_register_t *combined_sh = (agc_register_t *)(memory + 0x6800);
        agc_register_t *vs_cx = *(agc_register_t **)((uint8_t *)vertex_shader + 24);
        agc_register_t *ps_cx = *(agc_register_t **)((uint8_t *)pixel_shader + 24);
        agc_register_t *vs_sh = *(agc_register_t **)((uint8_t *)vertex_shader + 32);
        agc_register_t *ps_sh = *(agc_register_t **)((uint8_t *)pixel_shader + 32);
        uint32_t vs_cx_count = *((uint8_t *)vertex_shader + 91);
        uint32_t ps_cx_count = *((uint8_t *)pixel_shader + 91);
        uint32_t vs_sh_count = *((uint8_t *)vertex_shader + 92);
        uint32_t ps_sh_count = *((uint8_t *)pixel_shader + 92);

        memcpy(cx + cx_count, link_cx, 34 * sizeof(*cx));
        cx_count += 34;
        memcpy(cx + cx_count, vs_cx, vs_cx_count * sizeof(*cx));
        cx_count += vs_cx_count;
        memcpy(cx + cx_count, ps_cx, ps_cx_count * sizeof(*cx));
        cx_count += ps_cx_count;
        memcpy(combined_sh, vs_sh, vs_sh_count * sizeof(*combined_sh));
        memcpy(combined_sh + vs_sh_count, ps_sh, ps_sh_count * sizeof(*combined_sh));
        sceAgcDcbSetCxRegistersIndirect(&command, cx, cx_count);
        sceAgcDcbSetUcRegistersIndirect(&command, memory + 0x6000, 3);
        sceAgcDcbSetShRegistersIndirect(&command, combined_sh, vs_sh_count + ps_sh_count);
    }

    if (shader_resource_offset(vertex_shader, 3, &slot) != 0)
        return -2;
    descriptor[0] = (uint32_t)(uintptr_t)geometry_cb;
    descriptor[1] = (uint32_t)((uintptr_t)geometry_cb >> 32) | (16u << 16);
    descriptor[2] = 4;
    descriptor[3] = 0x0004dfacu;
    {
        uint32_t *header = (uint32_t *)resources;
        uintptr_t table = (uintptr_t)resources + header[1];
        uintptr_t vertex = (uintptr_t)resources + header[2];
        descriptor[4] = (uint32_t)table;
        descriptor[5] = (uint32_t)(table >> 32);
        descriptor[6] = (uint32_t)vertex;
        descriptor[7] = (uint32_t)(vertex >> 32);
    }
    sceAgcCbSetShRegisterRangeDirect(&command, 0x8c + slot, descriptor, 8);

    agc_dbg("R3 link/sh done -> bind_pixel_source + draw + flip");
    bind_pixel_source(&command, resources, source, y_bytes, uv_bytes, pixel_cb);
    sceAgcDcbDrawIndexAuto(&command, 4, 2);

    /* #28: overlay quad - a 2nd DrawIndexAuto in the same DCB, viewport moved
     * to the OSD rect + constant-alpha blend, from a tightly-packed NV12
     * surface. Verbatim from ProsperoLight render_frame()'s draw_overlay
     * branch (SDR: no shader rebind, reuse the NV12 pipeline + resources). */
    if (draw_overlay && ovl_source && ovl_w && ovl_h) {
        float sc = ovl_scale ? (float)ovl_scale : 1.f;
        size_t ovl_y_bytes  = (size_t)ovl_w * ovl_h;
        size_t ovl_uv_bytes = (size_t)ovl_w * ((ovl_h + 1u) / 2u);
        /* Inset the overlay 1 output px on every side: the bilinear sampler
         * would otherwise reach an out-of-bounds texel at u/v == 1 (the surface
         * is up to `sc`x upscaled), which shows as a flickering white edge
         * line. The outer 1px shows the video quad instead (correct content). */
        float vw = ovl_w * sc - 2.f, vh_ = ovl_h * sc - 2.f;
        const agc_register_t state[11] = {
            { 0x10f, 0, float_bits(vw * .5f) },
            { 0x110, 0, float_bits(ovl_x + 1.f + vw * .5f) },
            { 0x111, 0, float_bits(vh_ * -.5f) },
            { 0x112, 0, float_bits(ovl_y + 1.f + vh_ * .5f) },
            { 0x113, 0, float_bits(1) },
            { 0x114, 0, 0 },
            { 0x105, 0, float_bits(ovl_alpha) },
            { 0x106, 0, float_bits(ovl_alpha) },
            { 0x107, 0, float_bits(ovl_alpha) },
            { 0x108, 0, float_bits(ovl_alpha) },
            /* src * constAlpha + dst * (1 - constAlpha) */
            { 0x1e0, 0, 0x40001413u },
        };
        memcpy(ovl_pixel_cb, pixel_constants, sizeof(pixel_constants));
        ((uint32_t *)ovl_pixel_cb)[12] = float_bits((float)ovl_w);
        ((uint32_t *)ovl_pixel_cb)[13] = float_bits((float)ovl_h);
        ((uint32_t *)ovl_pixel_cb)[14] = ovl_w;
        ((uint32_t *)ovl_pixel_cb)[15] = ovl_w / 2u;
        memcpy(ovl_state, state, sizeof(state));
        sceAgcDcbSetCxRegistersIndirect(&command, ovl_state, 11);
        bind_pixel_source(&command, resources, ovl_source, ovl_y_bytes, ovl_uv_bytes, ovl_pixel_cb);
        sceAgcDcbDrawIndexAuto(&command, 4, 2);
        agc_dbg("R3b overlay quad  %ux%u @ %u,%u a=%d/100",
                ovl_w, ovl_h, ovl_x, ovl_y, (int)(ovl_alpha * 100.f));
    }

    sceAgcDcbSetFlip(&command, (uint32_t)video, buffer_index, 1, render_marker);

    submit.words = words;
    submit.word_count = (uint32_t)(command.up - words);
    *word_count = submit.word_count;
    flush_gpu_data(memory, SHADER_STATIC_BYTES);
    agc_dbg("R4 DCB built (%u words) -> SubmitDcb", submit.word_count);
    {
        int32_t result = sceAgcDriverSubmitDcb(&submit);
        agc_dbg("R5 SubmitDcb=0x%08x -> SuspendPoint", (unsigned)result);
        if (result == 0)
            result = sceAgcSuspendPoint();
        agc_dbg("R6 SuspendPoint done, result=0x%08x", (unsigned)result);
        return result;
    }
}

int pp_agc_init(uint32_t width, uint32_t height, int hdr)
{
    if (g_agc.ready)
        return 0;
    if (g_agc.tried)
        return -1;
    g_agc.tried = 1;
    g_agc.mem_start = -1;
    g_agc.stage_start = -1;
    g_agc.ovl_stage_start = -1;

    /* sceAgcInit is not idempotent — a second call (e.g. after evo_agc_probe's
     * gate) returns 0x8A6C0004 "already initialized". Only a hard failure to
     * make the library usable matters, and CreateShader/LinkShaders below will
     * catch that; so log the rc and press on. */
    int32_t rc = sceAgcInit(&g_agc.state, 8);
    evo_boot_log("pp_agc: sceAgcInit=0x%08x (0x8a6c0004 = already-init, ok)",
                 (unsigned)rc);

    int64_t limit = sceKernelGetDirectMemorySize();
    rc = sceKernelAllocateDirectMemory(0, limit, SHADER_MEMORY_BYTES, SHADER_ALIGN,
                                       DIRECT_MEMORY_TYPE, &g_agc.mem_start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory((void **)&g_agc.mem, SHADER_MEMORY_BYTES,
                                      MAP_PROTECTION, 0, g_agc.mem_start, SHADER_ALIGN);
    if (rc != 0 || !g_agc.mem) {
        evo_boot_log("pp_agc: shader mem alloc/map=0x%08x", (unsigned)rc);
        pp_agc_shutdown();
        return -1;
    }
    memset(g_agc.mem, 0, SHADER_MEMORY_BYTES);

    const uint8_t *px_s = hdr ? pp_agc_pixel_hdr_code_start : pp_agc_pixel_code_start;
    const uint8_t *px_e = hdr ? pp_agc_pixel_hdr_code_end   : pp_agc_pixel_code_end;
    if (copy_asset(g_agc.mem + OFF_GEO_HDR,   0x1000, pp_agc_geometry_header_start, pp_agc_geometry_header_end) != 0 ||
        copy_asset(g_agc.mem + OFF_GEO_CODE,  0x1000, pp_agc_geometry_code_start,   pp_agc_geometry_code_end)   != 0 ||
        copy_asset(g_agc.mem + OFF_PIX_HDR,   0x1000, pp_agc_pixel_header_start,    pp_agc_pixel_header_end)     != 0 ||
        copy_asset(g_agc.mem + OFF_PIX_CODE,  0x1000, px_s, px_e)                                               != 0 ||
        copy_asset(g_agc.mem + OFF_RESOURCES, 0x1000, pp_agc_resources_start,       pp_agc_resources_end)       != 0 ||
        prepare_resources(g_agc.mem + OFF_RESOURCES, hdr) != 0) {
        evo_boot_log("pp_agc: blob copy / prepare_resources failed");
        pp_agc_shutdown();
        return -1;
    }

    evo_boot_log("pp_agc: blobs copied, mem=%p - CreateShader", g_agc.mem);
    rc = sceAgcCreateShader(&g_agc.vs, g_agc.mem + OFF_GEO_HDR, g_agc.mem + OFF_GEO_CODE);
    if (rc == 0)
        rc = sceAgcCreateShader(&g_agc.ps, g_agc.mem + OFF_PIX_HDR, g_agc.mem + OFF_PIX_CODE);
    int32_t link = -1;
    if (rc == 0)
        link = sceAgcLinkShaders(g_agc.mem + OFF_LINK_A, g_agc.mem + OFF_LINK_B, 0,
                                 g_agc.vs, g_agc.ps, 6 /* PrimitiveTriangleStrip */);

    evo_boot_log("pp_agc: create=0x%08x link=0x%08x vs=%p ps=%p  %ux%u hdr=%d",
                 (unsigned)rc, (unsigned)link, g_agc.vs, g_agc.ps, width, height, hdr);
    if (rc != 0 || link != 0) {
        pp_agc_shutdown();
        return -1;
    }

    g_agc.w = width;
    g_agc.h = height;
    g_agc.ready = 1;

    if (agc_submit_start() != 0) {
        evo_boot_log("pp_agc: submit worker thread create FAILED - GPU present disabled");
        pp_agc_shutdown();
        return -1;
    }

    evo_boot_log("pp_agc: READY - GPU present path armed (render_frame ported, "
                 "submit watchdog live, #27)");

    /* #28 Phase 4: bring up the GPU geometry path too. Best-effort - a failure
     * here leaves pp_agc_geo_available() == 0 and the CPU raster unchanged. */
    agc_geo_init();
    return 0;
}

int pp_agc_available(void)
{
    return g_agc.ready;
}

/*
 * #28 Phase 1 go/no-go: does sceAgcCreateShader accept the hand-written UI
 * shaders paired with ProsperoLight's reused headers, and does
 * sceAgcLinkShaders work with primitive type 4 (triangle list)?
 *
 * Pure library calls - no DCB, no draw, no submit, no flip. Runs under
 * evo_agc_probe()'s fault guard, logs every rc to evo_boot.log. Needs
 * pp_agc_init() to have mapped g_agc.mem first. Changes no state.
 *
 * The pixel headers (pixel.header.bin) should work - each PS is a structural
 * subset of the NV12 PS. ui_vs against geometry.header.bin is the real
 * question (see the risk note in pp/shaders/ui_vs.s).
 */
int pp_agc_probe_ui_shaders(void)
{
    if (!g_agc.mem) {
        evo_boot_log("pp_agc UI: mem not mapped - run pp_agc_init first");
        evo_boot_log_flush();
        return -1;
    }

    uint8_t *m = g_agc.mem;
    int bad = 0;
    bad |= copy_asset(m + OFF_UI_VS_HDR,    0x1000, pp_agc_ui_vs_header_start,  pp_agc_ui_vs_header_end);
    bad |= copy_asset(m + OFF_UI_PS_HDR,    0x1000, pp_agc_ui_ps_header_start,  pp_agc_ui_ps_header_end);
    bad |= copy_asset(m + OFF_UI_VS_CODE,   0x1000, pp_agc_ui_vs_code_start,    pp_agc_ui_vs_code_end);
    bad |= copy_asset(m + OFF_UI_PS_A_CODE, 0x1000, pp_agc_solid_ps_code_start, pp_agc_solid_ps_code_end);
    bad |= copy_asset(m + OFF_UI_PS_B_CODE, 0x1000, pp_agc_glyph_ps_code_start, pp_agc_glyph_ps_code_end);
    bad |= copy_asset(m + OFF_UI_PS_C_CODE, 0x1000, pp_agc_rgba_ps_code_start,  pp_agc_rgba_ps_code_end);
    if (bad) {
        evo_boot_log("pp_agc UI: blob copy failed");
        evo_boot_log_flush();
        return -1;
    }

    void *vs = 0, *ps_solid = 0, *ps_glyph = 0, *ps_rgba = 0;
    int32_t c_vs    = sceAgcCreateShader(&vs,       m + OFF_UI_VS_HDR, m + OFF_UI_VS_CODE);
    int32_t c_solid = sceAgcCreateShader(&ps_solid, m + OFF_UI_PS_HDR, m + OFF_UI_PS_A_CODE);
    int32_t c_glyph = sceAgcCreateShader(&ps_glyph, m + OFF_UI_PS_HDR, m + OFF_UI_PS_B_CODE);
    int32_t c_rgba  = sceAgcCreateShader(&ps_rgba,  m + OFF_UI_PS_HDR, m + OFF_UI_PS_C_CODE);

    evo_boot_log("pp_agc UI: CreateShader ui_vs=0x%08x solid=0x%08x glyph=0x%08x rgba=0x%08x",
                 (unsigned)c_vs, (unsigned)c_solid, (unsigned)c_glyph, (unsigned)c_rgba);
    evo_boot_log("pp_agc UI:   vs=%p solid=%p glyph=%p rgba=%p", vs, ps_solid, ps_glyph, ps_rgba);
    evo_boot_log_flush();

    /* LinkShaders(..., 4 == PrimitiveTriangleList) - only where both halves
     * were created. */
    if (c_vs == 0 && c_solid == 0) {
        int32_t l = sceAgcLinkShaders(m + OFF_UI_LINK_A, m + OFF_UI_LINK_B, 0, vs, ps_solid, 4);
        evo_boot_log("pp_agc UI: LinkShaders ui_vs+solid (TriList) = 0x%08x", (unsigned)l);
    }
    if (c_vs == 0 && c_glyph == 0) {
        int32_t l = sceAgcLinkShaders(m + OFF_UI_LINK_A, m + OFF_UI_LINK_B, 0, vs, ps_glyph, 4);
        evo_boot_log("pp_agc UI: LinkShaders ui_vs+glyph (TriList) = 0x%08x", (unsigned)l);
    }
    if (c_vs == 0 && c_rgba == 0) {
        int32_t l = sceAgcLinkShaders(m + OFF_UI_LINK_A, m + OFF_UI_LINK_B, 0, vs, ps_rgba, 4);
        evo_boot_log("pp_agc UI: LinkShaders ui_vs+rgba (TriList) = 0x%08x", (unsigned)l);
    }

    /* #28 Phase 4: SharpProspero's compiler-generated mesh_vs / mesh_ps. */
    if (copy_asset(m + OFF_UI_SPLICE_A + 0x0000, 0x1000, pp_agc_spvs_header_start, pp_agc_spvs_header_end) == 0 &&
        copy_asset(m + OFF_UI_SPLICE_A + 0x1000, 0x1000, pp_agc_spvs_code_start,   pp_agc_spvs_code_end)   == 0 &&
        copy_asset(m + OFF_UI_SPLICE_A + 0x2000, 0x1000, pp_agc_spps_header_start, pp_agc_spps_header_end) == 0 &&
        copy_asset(m + OFF_UI_SPLICE_A + 0x3000, 0x1000, pp_agc_spps_code_start,   pp_agc_spps_code_end)   == 0) {
        void *spvs = 0, *spps = 0;
        int32_t c_v = sceAgcCreateShader(&spvs, m + OFF_UI_SPLICE_A + 0x0000, m + OFF_UI_SPLICE_A + 0x1000);
        int32_t c_p = sceAgcCreateShader(&spps, m + OFF_UI_SPLICE_A + 0x2000, m + OFF_UI_SPLICE_A + 0x3000);
        evo_boot_log("pp_agc UI: CreateShader sp_mesh_vs=0x%08x sp_mesh_ps=0x%08x  (v=%p p=%p)",
                     (unsigned)c_v, (unsigned)c_p, spvs, spps);
        if (c_v == 0 && c_p == 0) {
            int32_t l = sceAgcLinkShaders(m + OFF_UI_LINK_A, m + OFF_UI_LINK_B, 0, spvs, spps, 4);
            evo_boot_log("pp_agc UI: LinkShaders sp_mesh_vs+sp_mesh_ps (TriList) = 0x%08x", (unsigned)l);
        }
        evo_boot_log_flush();
    }

    /* #67: blit_ps - a REAL compiler-emitted textured PS this time (ripped,
     * not hand-written), paired with g_agc.vs (the already-created,
     * hardware-proven fullscreen-quad VS from pp_agc_init - not re-created
     * here). If this creates+links where every hand-written textured PS
     * above failed 0x8a6c001f, it's the #67 fix: composite the RmlUi text/
     * icon surface onto the GPU solids via this shader instead of abusing
     * the NV12 path. */
    if (g_agc.vs &&
        copy_asset(m + OFF_BLIT_PS_HDR,  0x1000, pp_agc_blit_ps_header_start, pp_agc_blit_ps_header_end) == 0 &&
        copy_asset(m + OFF_BLIT_PS_CODE, 0x1000, pp_agc_blit_ps_code_start,   pp_agc_blit_ps_code_end)   == 0) {
        void *ps_blit = 0;
        int32_t c_blit = sceAgcCreateShader(&ps_blit, m + OFF_BLIT_PS_HDR, m + OFF_BLIT_PS_CODE);
        evo_boot_log("pp_agc UI: CreateShader blit_ps=0x%08x  (ps=%p)",
                     (unsigned)c_blit, ps_blit);
        if (c_blit == 0) {
            /* TriangleStrip (6), matching g_agc.vs's real topology - not the
             * TriList (4) the hand-written UI probes above use. */
            int32_t l = sceAgcLinkShaders(m + OFF_BLIT_LINK_A, m + OFF_BLIT_LINK_B, 0,
                                          g_agc.vs, ps_blit, 6);
            evo_boot_log("pp_agc UI: LinkShaders geometry_vs+blit_ps (TriStrip) = 0x%08x",
                         (unsigned)l);

            /* Diagnostic only, no draw: where did the compiler put blit_ps's
             * texture (kind 1) and sampler (kind 2) resources? bind_pixel_source
             * hardcodes SH 0x0c for the NV12 PS (a DIFFERENT compiled shader) -
             * that base is not safe to assume here. Read it back instead of
             * guessing before writing any real binding code (#67 real
             * compositing pass, next step). kind 0=CB, 3=buffer, logged too for
             * completeness. */
            for (unsigned kind = 0; kind < 4; kind++) {
                uint32_t off = 0xffffffffu;
                int32_t r = shader_resource_offset(ps_blit, kind, &off);
                evo_boot_log("pp_agc UI: blit_ps resource kind=%u -> rc=%d off=0x%x",
                             kind, r, (unsigned)off);
            }
        }
        evo_boot_log_flush();
    } else {
        evo_boot_log("pp_agc UI: blit_ps probe skipped (g_agc.vs=%p)", g_agc.vs);
        evo_boot_log_flush();
    }

    /*
     * Hardware result (2026-09-04, four --agc-probe runs): ui_vs + solid_ps
     * create + link (TriList) clean. Every textured PS - image_sample OR typed
     * buffer_load_format, hand-written OR spliced onto the reference NV12 body's
     * exact trailer - fails 0x8a6c001f. sceAgcCreateShader validates the code
     * body against the "sl00" resource-metadata block whenever a memory
     * resource is touched, and ProsperoLight only ships headers for its
     * NV12/P010 shaders. => the solid GPU path is hand-writable; textured UI
     * (text, icons, art) needs a compiler that emits header+code+sl00 as a
     * matched set (GLSL -> SPIR-V -> AMD ISA; agc-implementation.md §7).
     * blit_ps result: see the CreateShader/LinkShaders log lines just above -
     * update this comment once a real hardware run reports back.
     */
    evo_boot_log("pp_agc UI: probe done (no state changed)");
    evo_boot_log_flush();
    return (c_vs == 0 && c_solid == 0) ? 0 : -1;
}

/* ===================================================================== *
 * #28 Phase 4 - GPU geometry present (SharpProspero Renderer3D.DrawMesh) *
 * ===================================================================== */

/* One request in flight, its own watchdog'd worker - same shape as
 * g_agc_submit but with a geometry batch instead of an NV12 frame. The menu
 * geo path and the video NV12 path never run in the same instant (menu vs
 * player screen), but they get separate workers so the proven #27 path is
 * untouched. */
static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  req_cv;
    pthread_cond_t  done_cv;
    int             started;
    int             have_req;
    int             have_done;
    int             quit;

    int             video, buffer_index;
    void           *target;
    const void     *vtx;
    uint32_t        vtx_count;
    const void     *idx;
    uint32_t        idx_count;
    const pp_agc_geo_draw_t *draws;
    uint32_t        draw_count;
    uint32_t        out_w, out_h;
    int64_t         render_marker;
    const void     *text_nv12;              /* staged premult NV12, or NULL */
    uint32_t        text_w, text_h;

    int32_t         rc;
    uint32_t        word_count;
} g_geo_submit;

/* One presented UI frame. Transcribed from SharpProspero
 * Graphics/Renderer3D.cs DrawMesh(): render-target block + viewport + shader
 * linkage + constant/vertex descriptors, then one DrawIndex per batch,
 * then the flip - all in one DCB. The render-target block is byte-identical to
 * agc_render_frame's (k8_8_8_8 UNorm, kRenderTarget tiling), which is the layout
 * #27 proved against EVO's linear-attr VO plane. */
static int agc_render_geo(int video, int buffer_index, void *target,
                          const void *vtx, uint32_t vtx_count,
                          const void *idx, uint32_t idx_count,
                          const pp_agc_geo_draw_t *draws, uint32_t draw_count,
                          uint32_t out_w, uint32_t out_h, int64_t render_marker,
                          const void *text_nv12, uint32_t text_w, uint32_t text_h,
                          uint32_t *word_count)
{
    static const uint16_t target_offsets[16] = { 0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f,
                                                 0x321, 0x323, 0x324, 0x325, 0x390, 0x398,
                                                 0x3a0, 0x3a8, 0x3b0, 0x3b8 };
    uint8_t *memory = g_agc.mem;
    agc_register_t *cx = (agc_register_t *)(memory + OFF_GEO_CX);
    float          *cb = (float *)(memory + OFF_GEO_CB);          /* Mvp[16], Model[16] */
    agc_register_t *combined_sh = (agc_register_t *)(memory + OFF_GEO_SH);
    uint32_t       *words = (uint32_t *)(memory + OFF_GEO_DCB);
    agc_command_buffer_t command;
    agc_submit_description_t submit;
    void *defaults = sceAgcGetRegisterDefaults();
    agc_register_t **blocks;
    uint32_t default_count;
    uint32_t cx_count = 16;

    if (!defaults || !target || !g_agc.mesh_vs || !g_agc.mesh_ps ||
        out_w == 0 || out_h == 0 || vtx_count == 0 || idx_count == 0 || draw_count == 0)
        return -1;

    /* bring-up bisect: /mnt/usb0/evo_agc_geo_min -> emit only the first batch,
     * so a hang can be pinned to "any GPU draw" vs "many draws / accumulation". */
    {
        static int mn = -1;
        if (mn < 0) { mn = 0; FILE *f = fopen("/mnt/usb0/evo_agc_geo_min", "r");
                      if (f) { mn = 1; fclose(f); } }
        if (mn && draw_count > 1) draw_count = 1;
    }

    memset(&command, 0, sizeof command);
    memset(&submit, 0, sizeof submit);

    geo_dbg("G0 render_geo enter target=%p vtx=%u idx=%u draws=%u out=%ux%u",
            target, vtx_count, idx_count, draw_count, out_w, out_h);

    /* --- render-target block (verbatim from agc_render_frame) --- */
    blocks = *(agc_register_t ***)defaults;
    default_count = *(uint32_t *)((uint8_t *)defaults + 0x20);
    for (uint32_t i = 0; i < 16; ++i) {
        cx[i].offset = target_offsets[i];
        cx[i].pad = 0;
        cx[i].value = 0;
        for (uint32_t c = 0; blocks && blocks[0] && c < default_count; ++c)
            if (blocks[0][c].offset == target_offsets[i]) { cx[i].value = blocks[0][c].value; break; }
    }
    cx[0].value  = (uint32_t)((uintptr_t)target >> 8);
    cx[1].value &= 0xfc001fffu;
    cx[2].value  = (cx[2].value & ~(0x7cu | 0x700u | 0x1800u | 0x10000000u | 0x10000u |
                                    0x8000u | 0x40000u | 0x4000u)) | 0x28u | 0x8000u;
    cx[3].value &= ~(0x7000u | 0x18000u);
    cx[4].value  = (cx[4].value & ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) | 0x48u;
    cx[5].value  = cx[6].value = cx[9].value = 0;
    cx[10].value = (cx[10].value & 0xffffff00u) | (uint32_t)((uintptr_t)target >> 40);
    cx[11].value &= 0xffffff00u;
    cx[12].value &= 0xffffff00u;
    cx[13].value &= 0xffffff00u;
    cx[14].value = (out_h - 1u) | ((out_w - 1u) << 14);
    cx[15].value = (cx[15].value & ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) |
                   0x6c000u | 0x01000000u | 0x44000000u;

    /* --- viewport, guardband, write mask, alpha-over blend (AgcViewport +
     *     CxBlendControl for target 0). Full-frame viewport; per-draw scissor
     *     is re-written in the loop. --- */
#define ADD_REG(o, v) do { cx[cx_count].offset = (o); cx[cx_count].pad = 0; \
                           cx[cx_count].value = (v); cx_count++; } while (0)
    ADD_REG(0x10f, float_bits(out_w * .5f));
    ADD_REG(0x110, float_bits(out_w * .5f));
    ADD_REG(0x111, float_bits(out_h * -.5f));
    ADD_REG(0x112, float_bits(out_h * .5f));
    ADD_REG(0x113, float_bits(1));
    ADD_REG(0x114, 0);
    ADD_REG(0x0b4, 0);
    ADD_REG(0x0b5, float_bits(1));
    ADD_REG(0x2fa, float_bits(1));
    ADD_REG(0x2fb, float_bits(1));
    ADD_REG(0x2fc, float_bits(1));
    ADD_REG(0x2fd, float_bits(1));
    ADD_REG(0x090, 0x80000000u);
    ADD_REG(0x091, out_w | (out_h << 16));
    ADD_REG(0x08e, 0x0f);
    /* premultiplied alpha-over: rgb = src*1 + dst*(1-srcA), a = src*1 + dst*(1-srcA).
     * CxBlendControl for target 0 (offset 0x1e0): enable | sepAlpha |
     * colorSrc kOne | colorDst kOneMinusSrcAlpha | alphaSrc kOne |
     * alphaDst kOneMinusSrcAlpha. */
    ADD_REG(0x1e0, 0x65010501u);
#undef ADD_REG

    /* --- MVP: pixel space (y-down, top-left origin) -> clip. System.Numerics
     *     row-major / row-vector convention (Renderer3D feeds the CB untransposed).
     *     Model = identity (our normals are +Z). --- */
    memset(cb, 0, 128);
    cb[0]  = 2.0f / (float)out_w;    /* clip.x = px*2/w - 1  */
    cb[5]  = -2.0f / (float)out_h;   /* clip.y = 1 - py*2/h  */
    cb[12] = -1.0f;
    cb[13] = 1.0f;
    cb[15] = 1.0f;                   /* clip.w = 1           */
    cb[16] = cb[21] = cb[26] = cb[31] = 1.0f;  /* Model = identity */

    command.bottom = words;
    command.top    = words + GEO_DCB_BYTES / sizeof(*words);
    command.up     = words;
    command.down   = command.top;
    command.callback = (uintptr_t)agc_out_of_space;

    sceAgcDriverWaitUntilSafeForRendering(&command.up,
                                          sceAgcDriverGetWaitRenderingPacketSizeInDwords(), 0,
                                          (uint32_t)video, buffer_index);

    /* --- shader linkage (persisted from agc_geo_init) + each shader's own
     *     context / shader registers (AgcShader field offsets 24/32/91/92). --- */
    {
        agc_register_t *vs_cx = *(agc_register_t **)((uint8_t *)g_agc.mesh_vs + 24);
        agc_register_t *ps_cx = *(agc_register_t **)((uint8_t *)g_agc.mesh_ps + 24);
        agc_register_t *vs_sh = *(agc_register_t **)((uint8_t *)g_agc.mesh_vs + 32);
        agc_register_t *ps_sh = *(agc_register_t **)((uint8_t *)g_agc.mesh_ps + 32);
        uint32_t vs_cx_n = *((uint8_t *)g_agc.mesh_vs + 91);
        uint32_t ps_cx_n = *((uint8_t *)g_agc.mesh_ps + 91);
        uint32_t vs_sh_n = *((uint8_t *)g_agc.mesh_vs + 92);
        uint32_t ps_sh_n = *((uint8_t *)g_agc.mesh_ps + 92);

        memcpy(cx + cx_count, memory + OFF_MESH_LINK_CX, 34 * sizeof(*cx));
        cx_count += 34;
        memcpy(cx + cx_count, vs_cx, vs_cx_n * sizeof(*cx)); cx_count += vs_cx_n;
        memcpy(cx + cx_count, ps_cx, ps_cx_n * sizeof(*cx)); cx_count += ps_cx_n;
        memcpy(combined_sh, vs_sh, vs_sh_n * sizeof(*combined_sh));
        memcpy(combined_sh + vs_sh_n, ps_sh, ps_sh_n * sizeof(*combined_sh));

        sceAgcDcbSetCxRegistersIndirect(&command, cx, cx_count);
        sceAgcDcbSetUcRegistersIndirect(&command, memory + OFF_MESH_LINK_UC, 3);
        sceAgcDcbSetShRegistersIndirect(&command, combined_sh, vs_sh_n + ps_sh_n);
        if (!g_agc.geo_first_ok) {
            evo_boot_log("pp_agc: G1 link: cx_count=%u (rt16+vp15+link34+vs%u+ps%u) sh=%u+%u",
                         cx_count, vs_cx_n, ps_cx_n, vs_sh_n, ps_sh_n);
            evo_boot_log_flush();
        }
    }

    /* --- bind the MVP constant buffer + the vertex structured buffer into the
     *     vertex program's user data (AgcBufferDescriptor.Constant / .Structured,
     *     GsUserDataBaseOffset 0x8c + the slot the program declares). --- */
    {
        uint32_t cb_slot = 0, vb_slot = 0;
        uint32_t d[4];
        uintptr_t a;
        if (shader_resource_offset(g_agc.mesh_vs, 3, &cb_slot) != 0 ||
            shader_resource_offset(g_agc.mesh_vs, 0, &vb_slot) != 0) {
            geo_dbg("G1! mesh_vs missing cb/vb resource slot");
            return -2;
        }
        a = (uintptr_t)(memory + OFF_GEO_CB);
        d[0] = (uint32_t)a;
        d[1] = (uint32_t)((a >> 32) & 0xffffu) | (16u << 16);
        d[2] = 8;                        /* 128 bytes / 16-byte records */
        d[3] = 0xfacu | (77u << 12);     /* X,Y,Z,W / four 32-bit floats */
        sceAgcCbSetShRegisterRangeDirect(&command, 0x8c + cb_slot, d, 4);

        a = (uintptr_t)vtx;
        d[0] = (uint32_t)a;
        d[1] = (uint32_t)((a >> 32) & 0xffffu) | (PP_AGC_GEO_VERTEX_STRIDE << 16);
        d[2] = vtx_count;
        d[3] = 0x204u | (5u << 12);      /* structured, stride-addressed */
        sceAgcCbSetShRegisterRangeDirect(&command, 0x8c + vb_slot, d, 4);
        if (!g_agc.geo_first_ok) {
            evo_boot_log("pp_agc: G2 bind cb_slot=%u vb_slot=%u  cb=%p vtx=%p idx=%p vtxN=%u",
                         cb_slot, vb_slot, (void *)(memory + OFF_GEO_CB), vtx, idx, vtx_count);
            evo_boot_log_flush();
        }
    }

    /* --- one DrawIndex per batch, re-writing the scissor between them.
     *     The scissor regs go in as DIRECT (inline) packets - the value is
     *     copied into the DCB word stream, so there is no external register
     *     buffer whose lifetime has to outlast the GPU. (SetCxRegistersIndirect
     *     would record a pointer the GPU reads later - fatal for stack data.)
     *     Packed form matches DrawCommandBuffer.Pack: (offset & 0xffff) | (value << 32). --- */
    sceAgcDcbSetIndexSize(&command, 1 /* 32-bit */, 0);
    sceAgcDcbSetIndexBuffer(&command, (void *)idx);
    sceAgcDcbSetIndexCount(&command, idx_count);

#define SET_CX(off, val) sceAgcDcbSetCxRegisterDirect(&command, \
        ((uint64_t)((off) & 0xffffu)) | ((uint64_t)(uint32_t)(val) << 32))

    for (uint32_t i = 0; i < draw_count; ++i) {
        const pp_agc_geo_draw_t *dr = &draws[i];
        uint32_t tl, br;
        if (dr->index_count == 0 ||
            (uint64_t)dr->first_index + dr->index_count > idx_count)
            continue;
        if (dr->scissor_w > 0 && dr->scissor_h > 0) {
            int32_t l = dr->scissor_x < 0 ? 0 : dr->scissor_x;
            int32_t t = dr->scissor_y < 0 ? 0 : dr->scissor_y;
            int32_t r = dr->scissor_x + dr->scissor_w;
            int32_t b = dr->scissor_y + dr->scissor_h;
            if (r > (int32_t)out_w) r = (int32_t)out_w;
            if (b > (int32_t)out_h) b = (int32_t)out_h;
            if (l > 0x7fff) l = 0x7fff;
            if (t > 0x7fff) t = 0x7fff;
            if (r < l || b < t) continue;
            tl = ((uint32_t)l | ((uint32_t)t << 16)) | 0x80000000u;
            br =  (uint32_t)r | ((uint32_t)b << 16);
        } else {
            tl = 0x80000000u;
            br = out_w | (out_h << 16);
        }
        SET_CX(0x090, tl);
        SET_CX(0x091, br);
        /* explicit sub-range address + count, matching SharpProspero Renderer3D's
         * SetIndexCount + DrawIndex(count, addr) form. */
        sceAgcDcbSetIndexCount(&command, dr->index_count);
        sceAgcDcbDrawIndex(&command, dr->index_count,
                           (void *)((const uint8_t *)idx + (size_t)dr->first_index * 4u), 0);
        if (!g_agc.geo_first_ok && i < 3)
            evo_boot_log("pp_agc: G3 draw[%u] first=%u n=%u scis tl=%08x br=%08x",
                         i, dr->first_index, dr->index_count, tl, br);
    }
#undef SET_CX
    if (!g_agc.geo_first_ok) evo_boot_log_flush();

    /* --- #28 Phase 4 (staged, OFF by default - /mnt/usb0/evo_agc_geo_text):
     *     composite the CPU text/icon layer (premultiplied over black, staged as
     *     NV12 by the caller) ADDITIVELY over the GPU solids - a 2nd pass in this
     *     DCB reusing the #27 NV12 pipeline (g_agc.vs/ps, OFF_LINK_A/B,
     *     bind_pixel_source). src ONE / dst ONE so black adds nothing.
     *     NOTE: this pass crashed EVO at boot on 2026-09-04 for reasons never
     *     pinned; kept behind the runtime hook + all-local scratch so the
     *     default geo build is byte-safe. Bisect plan in docs/evo-pro/status.md. --- */
#if PP_AGC_GEO_TEXT   /* bisect: body compiled out; see docs/evo-pro/status.md */
    {
        static int want_text = -1;
        if (want_text < 0) {
            want_text = 0;
            FILE *tf = fopen("/mnt/usb0/evo_agc_geo_text", "r");
            if (tf) { want_text = 1; fclose(tf); }
        }
        if (want_text && text_nv12 && text_w >= 2 && text_h >= 2 &&
            g_agc.vs && g_agc.blit_ps) {
            static const uint32_t geo_geom_c[16] = {
                0x3fa24ce6, 0, 0, 0x3e2f0fdd, 0, 0x3fa21449, 0, 0x3e111049,
                0, 0, 0xbf800000, 0x80000000, 0, 0, 0, 0x3f800000 };
            agc_register_t *ncx = (agc_register_t *)(memory + OFF_GEO_CX + 0x1000u);
            uint8_t *ngeo_cb = memory + OFF_GEO_CB + 0x100u;
            agc_register_t *nsh = (agc_register_t *)(memory + OFF_GEO_SH + 0x200u);
            uint32_t nc = 0, tslot = 0;

            if (shader_resource_offset(g_agc.vs, 3, &tslot) == 0) {
                agc_register_t *vcx = *(agc_register_t **)((uint8_t *)g_agc.vs + 24);
                agc_register_t *pcx = *(agc_register_t **)((uint8_t *)g_agc.blit_ps + 24);
                agc_register_t *vsh = *(agc_register_t **)((uint8_t *)g_agc.vs + 32);
                agc_register_t *psh = *(agc_register_t **)((uint8_t *)g_agc.blit_ps + 32);
                uint32_t vcn = *((uint8_t *)g_agc.vs + 91), pcn = *((uint8_t *)g_agc.blit_ps + 91);
                uint32_t vsn = *((uint8_t *)g_agc.vs + 92), psn = *((uint8_t *)g_agc.blit_ps + 92);
                uint32_t d[8];
                uint32_t tex[12];
                uintptr_t g, src = (uintptr_t)text_nv12;
                uint32_t width_m1 = text_w - 1u, height_m1 = text_h - 1u;

                memcpy(ncx, cx, 16 * sizeof(*ncx));   nc = 16;
#define NADD(o, v) do { ncx[nc].offset = (o); ncx[nc].pad = 0; ncx[nc].value = (v); nc++; } while (0)
                NADD(0x10f, float_bits(out_w * .5f));
                NADD(0x110, float_bits(out_w * .5f));
                NADD(0x111, float_bits(out_h * -.5f));
                NADD(0x112, float_bits(out_h * .5f));
                NADD(0x113, float_bits(1)); NADD(0x114, 0);
                NADD(0x0b4, 0); NADD(0x0b5, float_bits(1));
                NADD(0x2fa, float_bits(1)); NADD(0x2fb, float_bits(1));
                NADD(0x2fc, float_bits(1)); NADD(0x2fd, float_bits(1));
                NADD(0x090, 0x80000000u); NADD(0x091, out_w | (out_h << 16));
                NADD(0x08e, 0x0f);
                /* premultiplied alpha-over - the SAME formula already proven
                 * for the solids pass a few dozen lines above (0x65010501):
                 * enable | sepAlpha | colorSrc kOne | colorDst
                 * kOneMinusSrcAlpha | alphaSrc kOne | alphaDst
                 * kOneMinusSrcAlpha. NOT additive (0x41010101) - that was
                 * #67's actual bug (wash + navbar doubling): additive only
                 * ever brightens, it never replaces. */
                NADD(0x1e0, 0x65010501u);
#undef NADD
                memcpy(ncx + nc, memory + OFF_BLIT_GEO_LINK_CX, 34 * sizeof(*ncx)); nc += 34;
                memcpy(ncx + nc, vcx, vcn * sizeof(*ncx)); nc += vcn;
                memcpy(ncx + nc, pcx, pcn * sizeof(*ncx)); nc += pcn;
                memcpy(nsh, vsh, vsn * sizeof(*nsh));
                memcpy(nsh + vsn, psh, psn * sizeof(*nsh));
                sceAgcDcbSetCxRegistersIndirect(&command, ncx, nc);
                sceAgcDcbSetUcRegistersIndirect(&command, memory + OFF_BLIT_GEO_LINK_UC, 3);
                sceAgcDcbSetShRegistersIndirect(&command, nsh, vsn + psn);

                /* VS's own constant buffer - the same geometry_constants block
                 * the video path uses (position/scale for the fullscreen
                 * quad). kind=3=CB, cross-checked against the ALSO-hardware-
                 * verified mesh_vs CB binding earlier in this function. */
                g = (uintptr_t)ngeo_cb;
                memcpy(ngeo_cb, geo_geom_c, 64);
                d[0] = (uint32_t)g;
                d[1] = (uint32_t)((g >> 32) & 0xffffu) | (16u << 16);
                d[2] = 4; d[3] = 0x0004dfacu;
                d[4] = 0; d[5] = 0; d[6] = 0; d[7] = 0;
                sceAgcCbSetShRegisterRangeDirect(&command, 0x8c + tslot, d, 8);

                /* blit_ps's texture (T#, kind=0, SH 0x0c+0) + sampler (S#,
                 * kind=2, SH 0x0c+8) - offsets confirmed against blit_ps's own
                 * reflection data via shader_resource_offset (2026-09-04
                 * hardware probe: kind0 off=0x0, kind2 off=0x8), matching its
                 * disassembly's s[0:7]=image / s[8:11]=sampler exactly. A REAL
                 * T# (SQ_RSRC_IMG_2D) this time, not bind_pixel_source's
                 * buffer-shaped NV12 layout - bit layout from
                 * third_party/SharpProspero's AgcTextureDescriptor. Linear
                 * tiling (0): a CPU-written surface, no tiler involved. Format
                 * 56 = k8_8_8_8UNorm; m_surface is R,G,B,A ascending in memory
                 * (confirmed via agc_bgra_to_nv12's own R=px&0xff decode a
                 * few hundred lines down), so identity channel order matches
                 * directly. */
                memset(tex, 0, sizeof tex);
                tex[0] = (uint32_t)(src >> 8);
                tex[1] = (uint32_t)((src >> 32) & 0xffu) | (56u << 20) | ((width_m1 & 0x3u) << 30);
                tex[2] = ((width_m1 >> 2) & 0xfffu) | ((height_m1 & 0x3fffu) << 14);
                tex[3] = 0xfacu | (9u << 28);   /* identity R,G,B,A + Texture2D */
                /* proven bilinear S# words - same as bind_pixel_source uses
                 * for the hardware-verified NV12 present path. */
                tex[8]  = 0x00000092u;
                tex[9]  = 0x00fff000u;
                tex[10] = AGC_BILINEAR_SAMPLER_WORD;
                sceAgcCbSetShRegisterRangeDirect(&command, 0x0c, tex, 12);

                sceAgcDcbDrawIndexAuto(&command, 4, 2);
                geo_dbg("Gt text pass %ux%u nc=%u", text_w, text_h, nc);
            }
        }
    }
#else
    (void)text_nv12; (void)text_w; (void)text_h;
#endif

    sceAgcDcbSetFlip(&command, (uint32_t)video, buffer_index, 1, render_marker);

    submit.words = words;
    submit.word_count = (uint32_t)(command.up - words);
    *word_count = submit.word_count;
    /* CPU-write-back scratch the GPU reads: cx / cb / dcb / sh + the persisted
     * mesh link block. Vertex + index staging is flushed by the caller.
     * The text pass's own resource table (T#/S# for blit_ps) goes through
     * sceAgcCbSetShRegisterRangeDirect from a LOCAL stack array - same
     * pattern the (unconditionally hardware-verified) mesh_vs CB/VB binding
     * above already uses - so it needs no flush of its own, and its
     * persistent link block (OFF_BLIT_GEO_LINK_CX/UC) is flushed once in
     * agc_geo_init, not per frame. Nothing below OFF_MESH_LINK_CX is touched
     * per frame any more (the old NV12-link-block reuse this comment used to
     * describe is gone with it), so one flush range covers both cases. */
    flush_gpu_data(memory + OFF_MESH_LINK_CX, (OFF_GEO_SH + 0x1000u) - OFF_MESH_LINK_CX);

    geo_dbg("G4 DCB built (%u words) -> SubmitDcb", submit.word_count);
    {
        int32_t result = sceAgcDriverSubmitDcb(&submit);
        geo_dbg("G5 SubmitDcb=0x%08x -> SuspendPoint", (unsigned)result);
        if (result == 0)
            result = sceAgcSuspendPoint();
        geo_dbg("G6 SuspendPoint done, result=0x%08x", (unsigned)result);
        return result;
    }
}

static void *agc_geo_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int video, buffer_index;
        void *target;
        const void *vtx, *idx, *text_nv12;
        uint32_t vtx_count, idx_count, out_w, out_h, draw_count, text_w, text_h;
        const pp_agc_geo_draw_t *draws;
        int64_t render_marker;
        uint32_t words = 0;
        int32_t rc;
        int guard;

        pthread_mutex_lock(&g_geo_submit.lock);
        while (!g_geo_submit.have_req && !g_geo_submit.quit)
            pthread_cond_wait(&g_geo_submit.req_cv, &g_geo_submit.lock);
        if (g_geo_submit.quit) { pthread_mutex_unlock(&g_geo_submit.lock); break; }
        video        = g_geo_submit.video;
        buffer_index = g_geo_submit.buffer_index;
        target       = g_geo_submit.target;
        vtx          = g_geo_submit.vtx;
        vtx_count    = g_geo_submit.vtx_count;
        idx          = g_geo_submit.idx;
        idx_count    = g_geo_submit.idx_count;
        draws        = g_geo_submit.draws;
        draw_count   = g_geo_submit.draw_count;
        out_w        = g_geo_submit.out_w;
        out_h        = g_geo_submit.out_h;
        render_marker = g_geo_submit.render_marker;
        text_nv12    = g_geo_submit.text_nv12;
        text_w       = g_geo_submit.text_w;
        text_h       = g_geo_submit.text_h;
        pthread_mutex_unlock(&g_geo_submit.lock);

        guard = !g_agc.geo_first_ok;
        if (guard) {
            if (sigsetjmp(t_agc_jmp, 1) != 0) {
                t_agc_armed = 0;
                evo_boot_log("pp_agc: FAULT (geo worker) in first agc_render_geo - "
                             "GPU geometry DISABLED");
                evo_boot_log_flush();
                g_agc.geo_ready = 0;
                rc = -99; words = 0;
                goto reply;
            }
            t_agc_armed = 1;
        }
        rc = agc_render_geo(video, buffer_index, target, vtx, vtx_count, idx, idx_count,
                            draws, draw_count, out_w, out_h, render_marker,
                            text_nv12, text_w, text_h, &words);
        if (guard) {
            t_agc_armed = 0;
            evo_boot_log("pp_agc: agc_render_geo rc=0x%08x words=%u draws=%u %ux%u",
                         (unsigned)rc, words, draw_count, out_w, out_h);
            evo_boot_log_flush();
            if (rc == 0)
                g_agc.geo_first_ok = 1;
        }

    reply:
        pthread_mutex_lock(&g_geo_submit.lock);
        g_geo_submit.rc = rc;
        g_geo_submit.word_count = words;
        g_geo_submit.have_req = 0;
        g_geo_submit.have_done = 1;
        pthread_cond_signal(&g_geo_submit.done_cv);
        pthread_mutex_unlock(&g_geo_submit.lock);
    }
    return NULL;
}

/* Create + link SharpProspero's compiler-built mesh shaders. Best-effort: a
 * failure leaves pp_agc_geo_available() == 0 and the CPU raster stands; it does
 * NOT fail pp_agc_init. Called at the tail of pp_agc_init once g_agc.mem is up. */
static void agc_geo_init(void)
{
    uint8_t *m = g_agc.mem;
    int bad = 0;
    bad |= copy_asset(m + OFF_MESH_VS_HDR,  0x1000, pp_agc_spvs_header_start, pp_agc_spvs_header_end);
    bad |= copy_asset(m + OFF_MESH_VS_CODE, 0x1000, pp_agc_spvs_code_start,   pp_agc_spvs_code_end);
    bad |= copy_asset(m + OFF_MESH_PS_HDR,  0x1000, pp_agc_spps_header_start, pp_agc_spps_header_end);
    bad |= copy_asset(m + OFF_MESH_PS_CODE, 0x1000, pp_agc_spps_code_start,   pp_agc_spps_code_end);
    if (bad) {
        evo_boot_log("pp_agc geo: mesh blob copy failed - GPU geometry off");
        return;
    }

    int32_t cv = sceAgcCreateShader(&g_agc.mesh_vs, m + OFF_MESH_VS_HDR, m + OFF_MESH_VS_CODE);
    int32_t cp = 0;
    if (cv == 0)
        cp = sceAgcCreateShader(&g_agc.mesh_ps, m + OFF_MESH_PS_HDR, m + OFF_MESH_PS_CODE);
    int32_t lk = -1;
    if (cv == 0 && cp == 0)
        lk = sceAgcLinkShaders(m + OFF_MESH_LINK_CX, m + OFF_MESH_LINK_UC, 0,
                               g_agc.mesh_vs, g_agc.mesh_ps, 4 /* triangle list */);
    evo_boot_log("pp_agc geo: mesh create vs=0x%08x ps=0x%08x link=0x%08x (vs=%p ps=%p)",
                 (unsigned)cv, (unsigned)cp, (unsigned)lk, g_agc.mesh_vs, g_agc.mesh_ps);
    if (cv != 0 || cp != 0 || lk != 0) {
        g_agc.mesh_vs = g_agc.mesh_ps = 0;
        return;
    }
    flush_gpu_data(m + OFF_MESH_VS_HDR, (OFF_MESH_LINK_UC + 0x1000u) - OFF_MESH_VS_HDR);

    /* #67: blit_ps, paired with g_agc.vs (created earlier in pp_agc_init, the
     * same fullscreen-quad VS the video path uses). Best-effort - a failure
     * here just leaves the text/icon layer un-composited, same as any other
     * geo-init failure; it does not affect solids or video. */
    if (g_agc.vs &&
        copy_asset(m + OFF_BLIT_PS_HDR,  0x1000, pp_agc_blit_ps_header_start, pp_agc_blit_ps_header_end) == 0 &&
        copy_asset(m + OFF_BLIT_PS_CODE, 0x1000, pp_agc_blit_ps_code_start,   pp_agc_blit_ps_code_end)   == 0) {
        int32_t bc = sceAgcCreateShader(&g_agc.blit_ps, m + OFF_BLIT_PS_HDR, m + OFF_BLIT_PS_CODE);
        int32_t bl = -1;
        if (bc == 0)
            bl = sceAgcLinkShaders(m + OFF_BLIT_GEO_LINK_CX, m + OFF_BLIT_GEO_LINK_UC, 0,
                                   g_agc.vs, g_agc.blit_ps, 6 /* PrimitiveTriangleStrip */);
        evo_boot_log("pp_agc geo: blit_ps create=0x%08x link=0x%08x (ps=%p)",
                     (unsigned)bc, (unsigned)bl, g_agc.blit_ps);
        if (bc != 0 || bl != 0) {
            g_agc.blit_ps = 0;
        } else {
            flush_gpu_data(m + OFF_BLIT_GEO_LINK_CX, 0x2000u);
        }
    }
    g_agc.geo_stage_start = -1;

    pthread_mutex_init(&g_geo_submit.lock, NULL);
    pthread_cond_init(&g_geo_submit.req_cv, NULL);
    pthread_cond_init(&g_geo_submit.done_cv, NULL);
    if (pthread_create(&g_geo_submit.thread, NULL, agc_geo_worker, NULL) != 0) {
        evo_boot_log("pp_agc geo: worker thread create FAILED - GPU geometry off");
        g_agc.mesh_vs = g_agc.mesh_ps = 0;
        return;
    }
    g_geo_submit.started = 1;
    g_agc.geo_ready = 1;
    evo_boot_log("pp_agc geo: READY - GPU geometry present armed (#28 Phase 4)");
}

static void agc_geo_staging_free(void)
{
    if (g_agc.geo_stage[0]) {
        sceKernelMunmap(g_agc.geo_stage[0], g_agc.geo_stage_total);
        for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
            g_agc.geo_stage[i] = 0;
    }
    if (g_agc.geo_stage_start >= 0) {
        sceKernelReleaseDirectMemory(g_agc.geo_stage_start, g_agc.geo_stage_total);
        g_agc.geo_stage_start = -1;
    }
    g_agc.geo_stage_total = 0;
    g_agc.geo_stage_cap = 0;
}

static int agc_geo_staging_ensure(size_t need)
{
    if (g_agc.geo_stage[0] && g_agc.geo_stage_cap >= need)
        return 0;
    agc_geo_staging_free();

    size_t cap = (need + 0xffffu) & ~(size_t)0xffffu;
    size_t total = cap * (size_t)PP_VO_MAX_BUFFERS;
    int64_t start = -1;
    void *base = 0;
    int64_t limit = sceKernelGetDirectMemorySize();
    int32_t rc = sceKernelAllocateDirectMemory(0, limit, total, SHADER_ALIGN,
                                               DIRECT_MEMORY_TYPE, &start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&base, total, MAP_PROTECTION, 0, start, SHADER_ALIGN);
    if (rc != 0 || !base) {
        evo_boot_log("pp_agc geo: staging alloc/map=0x%08x need=%zu", (unsigned)rc, need);
        if (start >= 0)
            sceKernelReleaseDirectMemory(start, total);
        return -1;
    }
    g_agc.geo_stage_start = start;
    g_agc.geo_stage_total = total;
    g_agc.geo_stage_cap = cap;
    for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
        g_agc.geo_stage[i] = (uint8_t *)base + (size_t)i * cap;
    evo_boot_log("pp_agc geo: staging %zuKB x%d @ %p", cap >> 10, PP_VO_MAX_BUFFERS, base);
    return 0;
}

int pp_agc_geo_available(void)
{
    return g_agc.geo_ready && !g_agc.geo_wedged;
}

int pp_agc_present_geo(int vout_handle, uint32_t buf_idx, void *gpu_target,
                       int target_linear,
                       const void *vertices, uint32_t vertex_count,
                       const uint32_t *indices, uint32_t index_count,
                       const pp_agc_geo_draw_t *draws, uint32_t draw_count,
                       uint32_t out_w, uint32_t out_h,
                       const uint32_t *text_bgra, uint32_t text_w, uint32_t text_h,
                       int64_t flip_marker)
{
    (void)target_linear;
    if (g_agc.geo_wedged)
        return -2;
    if (!g_agc.geo_ready || !g_agc.mem || !g_agc.mesh_vs || !g_agc.mesh_ps ||
        !g_geo_submit.started)
        return -1;
    if (!vertices || !indices || !draws || vout_handle < 0 ||
        buf_idx >= (uint32_t)PP_VO_MAX_BUFFERS || !gpu_target ||
        vertex_count == 0 || index_count == 0 || draw_count == 0 ||
        out_w == 0 || out_h == 0)
        return -1;

    size_t vtx_bytes = (size_t)vertex_count * PP_AGC_GEO_VERTEX_STRIDE;
    size_t idx_bytes = (size_t)index_count * sizeof(uint32_t);
    size_t idx_off   = (vtx_bytes + 0xffu) & ~(size_t)0xffu;   /* 256-align the index block */
    size_t need      = idx_off + idx_bytes;

    int fg = !g_agc.geo_first_ok;
    if (fg) {
        if (sigsetjmp(t_agc_jmp, 1) != 0) {
            t_agc_armed = 0;
            evo_boot_log("pp_agc: FAULT (caller) staging geo - GPU geometry DISABLED");
            evo_boot_log_flush();
            g_agc.geo_ready = 0;
            return -1;
        }
        t_agc_armed = 1;
    }

    if (agc_geo_staging_ensure(need) != 0) {
        if (fg) t_agc_armed = 0;
        return -1;
    }

    uint8_t *slot = g_agc.geo_stage[buf_idx];
    memcpy(slot, vertices, vtx_bytes);
    memcpy(slot + idx_off, indices, idx_bytes);
    flush_gpu_data(slot, need);

    /* text/icon layer -> staged verbatim (premultiplied RGBA, no conversion)
     * in the (reused) overlay slab, for blit_ps to sample directly. #67: used
     * to go through a BGRA->NV12 conversion so the video pixel shader's YUV
     * path could (mis)handle it - that's what caused the wash + doubling.
     * blit_ps samples RGBA directly, so no conversion, and no even-dimension
     * requirement either (that was only for NV12 chroma subsampling).
     * Best-effort: a failure just drops the text layer, never the frame. */
    const uint8_t *text_stage = 0;
    uint32_t tw = 0, th = 0;
#if PP_AGC_GEO_TEXT
    if (text_bgra && text_w >= 2 && text_h >= 2) {
        size_t tneed = (size_t)text_w * text_h * 4u;
        if (agc_ovl_staging_ensure(tneed) == 0) {
            uint8_t *ts = g_agc.ovl_stage[buf_idx];
            memcpy(ts, text_bgra, tneed);
            flush_gpu_data(ts, tneed);
            text_stage = ts; tw = text_w; th = text_h;
        }
    }
#else
    (void)text_bgra;
#endif
    if (fg) t_agc_armed = 0;

    struct timespec mono_start, deadline;
    clock_gettime(CLOCK_MONOTONIC, &mono_start);
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 250L * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_geo_submit.lock);
    g_geo_submit.video        = vout_handle;
    g_geo_submit.buffer_index = (int)buf_idx;
    g_geo_submit.target       = gpu_target;
    g_geo_submit.vtx          = slot;
    g_geo_submit.vtx_count    = vertex_count;
    g_geo_submit.idx          = slot + idx_off;
    g_geo_submit.idx_count    = index_count;
    g_geo_submit.draws        = draws;
    g_geo_submit.draw_count   = draw_count;
    g_geo_submit.out_w        = out_w;
    g_geo_submit.out_h        = out_h;
    g_geo_submit.render_marker = flip_marker;
    g_geo_submit.text_nv12    = text_stage;
    g_geo_submit.text_w       = tw;
    g_geo_submit.text_h       = th;
    g_geo_submit.have_done    = 0;
    g_geo_submit.have_req     = 1;
    pthread_cond_signal(&g_geo_submit.req_cv);

    int timed_out = 0;
    while (!g_geo_submit.have_done) {
        if (pthread_cond_timedwait(&g_geo_submit.done_cv, &g_geo_submit.lock,
                                   &deadline) == ETIMEDOUT) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t ms = (int64_t)(now.tv_sec - mono_start.tv_sec) * 1000
                       + (now.tv_nsec - mono_start.tv_nsec) / 1000000;
            if (ms >= 240) { timed_out = 1; break; }
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 50L * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
        }
    }

    if (timed_out) {
        g_geo_submit.have_req = 0;
        g_agc.geo_wedged = 1;
        g_agc.geo_ready = 0;
        pthread_mutex_unlock(&g_geo_submit.lock);
        evo_boot_log("pp_agc: GEO SUBMIT WEDGED (>250ms) - GPU geometry ABANDONED, "
                     "CPU raster from here.");
        evo_boot_log_flush();
        return -2;
    }

    int32_t rc = g_geo_submit.rc;
    g_geo_submit.have_done = 0;
    pthread_mutex_unlock(&g_geo_submit.lock);
    return rc == 0 ? 0 : -1;
}

/* (re)allocate the double/triple NV12 staging pool in GPU-visible direct
 * memory. One slot per VO buffer — pp_videoout_acquire only hands back a
 * buffer whose previous flip has fully retired, so keying the staging slot to
 * the VO buffer index means the GPU is provably done with slot[buf_idx]
 * before we memcpy the next frame into it. */
static void agc_staging_free(void)
{
    if (g_agc.stage[0]) {
        sceKernelMunmap(g_agc.stage[0], g_agc.stage_total);
        for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
            g_agc.stage[i] = 0;
    }
    if (g_agc.stage_start >= 0) {
        sceKernelReleaseDirectMemory(g_agc.stage_start, g_agc.stage_total);
        g_agc.stage_start = -1;
    }
    g_agc.stage_total = 0;
    g_agc.stage_cap = 0;
}

static int agc_staging_ensure(size_t need)
{
    if (g_agc.stage[0] && g_agc.stage_cap >= need)
        return 0;
    agc_staging_free();

    size_t cap = (need + 0xffffu) & ~(size_t)0xffffu;   /* 64K round */
    size_t total = cap * (size_t)PP_VO_MAX_BUFFERS;
    int64_t start = -1;
    void *base = 0;
    int64_t limit = sceKernelGetDirectMemorySize();
    int32_t rc = sceKernelAllocateDirectMemory(0, limit, total, SHADER_ALIGN,
                                               DIRECT_MEMORY_TYPE, &start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&base, total, MAP_PROTECTION, 0, start, SHADER_ALIGN);
    if (rc != 0 || !base) {
        evo_boot_log("pp_agc: staging alloc/map=0x%08x need=%zu total=%zu",
                     (unsigned)rc, need, total);
        if (start >= 0)
            sceKernelReleaseDirectMemory(start, total);
        return -1;
    }
    g_agc.stage_start = start;
    g_agc.stage_total = total;
    g_agc.stage_cap = cap;
    for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
        g_agc.stage[i] = (uint8_t *)base + (size_t)i * cap;
    evo_boot_log("pp_agc: staging %zuKB x%d @ %p", cap >> 10, PP_VO_MAX_BUFFERS, base);
    return 0;
}

/* #28: same grow-only per-VO-buffer slab pattern for the overlay NV12. */
static void agc_ovl_staging_free(void)
{
    if (g_agc.ovl_stage[0]) {
        sceKernelMunmap(g_agc.ovl_stage[0], g_agc.ovl_stage_total);
        for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
            g_agc.ovl_stage[i] = 0;
    }
    if (g_agc.ovl_stage_start >= 0) {
        sceKernelReleaseDirectMemory(g_agc.ovl_stage_start, g_agc.ovl_stage_total);
        g_agc.ovl_stage_start = -1;
    }
    g_agc.ovl_stage_total = 0;
    g_agc.ovl_stage_cap = 0;
}

static int agc_ovl_staging_ensure(size_t need)
{
    if (g_agc.ovl_stage[0] && g_agc.ovl_stage_cap >= need)
        return 0;
    agc_ovl_staging_free();

    size_t cap = (need + 0xffffu) & ~(size_t)0xffffu;
    size_t total = cap * (size_t)PP_VO_MAX_BUFFERS;
    int64_t start = -1;
    void *base = 0;
    int64_t limit = sceKernelGetDirectMemorySize();
    int32_t rc = sceKernelAllocateDirectMemory(0, limit, total, SHADER_ALIGN,
                                               DIRECT_MEMORY_TYPE, &start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&base, total, MAP_PROTECTION, 0, start, SHADER_ALIGN);
    if (rc != 0 || !base) {
        evo_boot_log("pp_agc: overlay staging alloc/map=0x%08x need=%zu", (unsigned)rc, need);
        if (start >= 0)
            sceKernelReleaseDirectMemory(start, total);
        return -1;
    }
    g_agc.ovl_stage_start = start;
    g_agc.ovl_stage_total = total;
    g_agc.ovl_stage_cap = cap;
    for (int i = 0; i < PP_VO_MAX_BUFFERS; i++)
        g_agc.ovl_stage[i] = (uint8_t *)base + (size_t)i * cap;
    evo_boot_log("pp_agc: overlay staging %zuKB x%d @ %p", cap >> 10, PP_VO_MAX_BUFFERS, base);
    return 0;
}

/* The GPU submit worker. One request in flight; see g_agc_submit. */
static void *agc_submit_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int         video, buffer_index;
        void       *target;
        const void *source;
        size_t      source_bytes;
        uint32_t    pitch, surface_height, visible_width, visible_height;
        uint32_t    output_width, output_height;
        int64_t     render_marker;
        int         draw_overlay;
        const void *ovl_source;
        uint32_t    ovl_w, ovl_h, ovl_x, ovl_y, ovl_scale;
        float       ovl_alpha;
        uint32_t    words = 0;
        int32_t     rc;
        int         guard;

        pthread_mutex_lock(&g_agc_submit.lock);
        while (!g_agc_submit.have_req && !g_agc_submit.quit)
            pthread_cond_wait(&g_agc_submit.req_cv, &g_agc_submit.lock);
        if (g_agc_submit.quit) {
            pthread_mutex_unlock(&g_agc_submit.lock);
            break;
        }
        video          = g_agc_submit.video;
        buffer_index   = g_agc_submit.buffer_index;
        target         = g_agc_submit.target;
        source         = g_agc_submit.source;
        source_bytes   = g_agc_submit.source_bytes;
        pitch          = g_agc_submit.pitch;
        surface_height = g_agc_submit.surface_height;
        visible_width  = g_agc_submit.visible_width;
        visible_height = g_agc_submit.visible_height;
        output_width   = g_agc_submit.output_width;
        output_height  = g_agc_submit.output_height;
        render_marker  = g_agc_submit.render_marker;
        draw_overlay   = g_agc_submit.draw_overlay;
        ovl_source     = g_agc_submit.ovl_source;
        ovl_w          = g_agc_submit.ovl_w;
        ovl_h          = g_agc_submit.ovl_h;
        ovl_x          = g_agc_submit.ovl_x;
        ovl_y          = g_agc_submit.ovl_y;
        ovl_scale      = g_agc_submit.ovl_scale;
        ovl_alpha      = g_agc_submit.ovl_alpha;
        pthread_mutex_unlock(&g_agc_submit.lock);

        guard = !g_agc.first_frame_ok;
        if (guard) {
            agc_dbg("W0 worker picked up frame -> agc_render_frame");
            if (sigsetjmp(t_agc_jmp, 1) != 0) {
                t_agc_armed = 0;
                evo_boot_log("pp_agc: FAULT (worker) in first agc_render_frame - "
                             "GPU present DISABLED, CPU fallback");
                evo_boot_log_flush();
                g_agc.ready = 0;
                rc = -99;
                words = 0;
                goto reply;
            }
            t_agc_armed = 1;
            rc = agc_render_frame(video, buffer_index, target, g_agc.mem,
                                  g_agc.vs, g_agc.ps, source, source_bytes, pitch,
                                  surface_height, visible_width, visible_height,
                                  output_width, output_height, render_marker, &words,
                                  draw_overlay, ovl_source, ovl_w, ovl_h, ovl_x, ovl_y,
                                  ovl_scale, ovl_alpha);
            t_agc_armed = 0;

            evo_boot_log("pp_agc: render_frame rc=0x%08x words=%u  %ux%u -> %ux%u  "
                         "pitch=%u codedh=%u marker=%lld",
                         (unsigned)rc, words, visible_width, visible_height,
                         output_width, output_height, pitch, surface_height,
                         (long long)render_marker);
            evo_boot_log_flush();
            g_agc.first_frame_logged = 1;
            if (rc == 0)
                g_agc.first_frame_ok = 1;
        } else {
            rc = agc_render_frame(video, buffer_index, target, g_agc.mem,
                                  g_agc.vs, g_agc.ps, source, source_bytes, pitch,
                                  surface_height, visible_width, visible_height,
                                  output_width, output_height, render_marker, &words,
                                  draw_overlay, ovl_source, ovl_w, ovl_h, ovl_x, ovl_y,
                                  ovl_scale, ovl_alpha);
        }

    reply:
        pthread_mutex_lock(&g_agc_submit.lock);
        g_agc_submit.rc = rc;
        g_agc_submit.word_count = words;
        g_agc_submit.have_req = 0;
        g_agc_submit.have_done = 1;
        pthread_cond_signal(&g_agc_submit.done_cv);
        pthread_mutex_unlock(&g_agc_submit.lock);
    }
    return NULL;
}

static int agc_submit_start(void)
{
    if (g_agc_submit.started)
        return 0;

    /* Installed once, left in place. agc_fault_h re-raises with default
     * disposition for any fault that isn't inside an armed (TLS) guard. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = agc_fault_h;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    pthread_mutex_init(&g_agc_submit.lock, NULL);
    pthread_cond_init(&g_agc_submit.req_cv, NULL);
    pthread_cond_init(&g_agc_submit.done_cv, NULL);
    g_agc_submit.have_req = g_agc_submit.have_done = g_agc_submit.quit = 0;
    if (pthread_create(&g_agc_submit.thread, NULL, agc_submit_worker, NULL) != 0)
        return -1;
    g_agc_submit.started = 1;
    return 0;
}

/* Single caller only: pp_playback_push_frame runs on one decode thread and
 * every call here blocks until the worker replies or the watchdog fires, so
 * there is never more than one request in flight.
 *
 * ovl_nv12 (may be NULL) is a tightly-packed NV12 OSD surface (pitch == ovl_w,
 * interleaved UV at ovl_nv12 + ovl_w*ovl_h); it is composited as a second quad
 * in the same DCB, upscaled ovl_scale x (0/1 = none) with the bilinear sampler,
 * its top-left at (ovl_x, ovl_y) in output space, constant-alpha blended
 * (ovl_alpha 0..1). */
int pp_agc_present_nv12_overlay(int vout_handle, uint32_t buf_idx, void *gpu_target,
                                const void *nv12, uint32_t pitch_bytes, uint32_t coded_height,
                                uint32_t vis_w, uint32_t vis_h, uint32_t out_w, uint32_t out_h,
                                int64_t flip_marker,
                                const void *ovl_nv12, uint32_t ovl_w, uint32_t ovl_h,
                                uint32_t ovl_x, uint32_t ovl_y, uint32_t ovl_scale,
                                float ovl_alpha)
{
    if (g_agc.submit_wedged)
        return -2;
    if (!g_agc.ready || !g_agc.mem || !g_agc.vs || !g_agc.ps || !g_agc_submit.started)
        return -1;
    if (!nv12 || !gpu_target || vout_handle < 0 || buf_idx >= (uint32_t)PP_VO_MAX_BUFFERS ||
        pitch_bytes == 0 || (pitch_bytes & 1u) || coded_height == 0 ||
        vis_w == 0 || vis_h == 0 || out_w == 0 || out_h == 0)
        return -1;

    /* Overlay is best-effort: a bad rect just disables it, never fails the frame. */
    uint32_t ovl_sc = ovl_scale ? ovl_scale : 1u;
    int want_overlay = ovl_nv12 && ovl_w >= 2 && ovl_h >= 2 && (ovl_w & 1u) == 0 &&
                       (ovl_h & 1u) == 0 &&
                       ovl_x + ovl_w * ovl_sc <= out_w && ovl_y + ovl_h * ovl_sc <= out_h;
    if (ovl_alpha < 0.f) ovl_alpha = 0.f;
    if (ovl_alpha > 1.f) ovl_alpha = 1.f;

    /* #27 test hook: /mnt/usb0/evo_agc_no_present (or EVO_AGC_NO_PRESENT env)
     * simulates a mid-clip GPU wedge after ~120 good frames, so the AGC-death ->
     * VO-retile -> CPU-path recovery can be exercised without a real fault. */
    if (agc_test_wedge_due(g_agc.present_calls++)) {
        g_agc.submit_wedged = 1;
        g_agc.ready = 0;
        evo_boot_log("pp_agc: TEST WEDGE (evo_agc_no_present) after %llu frames - "
                     "simulating a mid-clip GPU stall",
                     (unsigned long long)g_agc.present_calls);
        evo_boot_log_flush();
        return -2;
    }

    int fg = !g_agc.first_frame_ok;   /* instrument + guard the first frame only */
    if (fg)
        agc_dbg("A0 present enter  vh=%d buf=%u pitch=%u codedh=%u vis=%ux%u out=%ux%u "
                "nv12=%p gpu=%p marker=%lld",
                vout_handle, buf_idx, pitch_bytes, coded_height, vis_w, vis_h,
                out_w, out_h, nv12, gpu_target, (long long)flip_marker);

    size_t need = (size_t)pitch_bytes * coded_height                     /* Y  */
                + (size_t)pitch_bytes * ((coded_height + 1u) / 2u);      /* UV */

    /* Guard the decode-thread side (staging alloc + the NV12 memcpy + clflush) -
     * a bad source pointer / size faults HERE, on this thread, not the worker. */
    if (fg) {
        if (sigsetjmp(t_agc_jmp, 1) != 0) {
            t_agc_armed = 0;
            evo_boot_log("pp_agc: FAULT (caller) staging the NV12 frame - GPU present "
                         "DISABLED, CPU fallback");
            evo_boot_log_flush();
            g_agc.ready = 0;
            return -1;
        }
        t_agc_armed = 1;
    }

    if (agc_staging_ensure(need) != 0) {
        if (fg) t_agc_armed = 0;
        return -1;
    }

    uint8_t *stage = g_agc.stage[buf_idx];
    if (fg) agc_dbg("A1 staging ok stage=%p need=%zu -> memcpy + flush", stage, need);
    memcpy(stage, nv12, need);
    flush_gpu_data(stage, need);

    /* Stage the overlay NV12 into its own per-VO-buffer slab. Under the same
     * first-frame fault guard as the video staging (a bad ovl_nv12 pointer
     * faults on this thread). On any overlay-staging failure, drop the overlay
     * and present video-only rather than failing the frame. */
    const uint8_t *ovl_stage = 0;
    if (want_overlay) {
        size_t ovl_need = (size_t)ovl_w * ovl_h + (size_t)ovl_w * ((ovl_h + 1u) / 2u);
        if (agc_ovl_staging_ensure(ovl_need) == 0) {
            uint8_t *os = g_agc.ovl_stage[buf_idx];
            memcpy(os, ovl_nv12, ovl_need);
            flush_gpu_data(os, ovl_need);
            ovl_stage = os;
        } else {
            want_overlay = 0;
        }
        if (fg) agc_dbg("A1b overlay staged=%p %ux%u @ %u,%u", ovl_stage, ovl_w, ovl_h, ovl_x, ovl_y);
    }

    if (fg) {
        t_agc_armed = 0;
        agc_dbg("A2 staged + flushed -> dispatch worker (250ms watchdog)");
    }

    /* 250 ms watchdog on the whole render_frame call (WaitUntilSafeForRendering
     * + SubmitDcb + SuspendPoint). The cond deadline is CLOCK_REALTIME (the BSD
     * libc default for pthread_cond_timedwait); a MONOTONIC start is also taken
     * so a wrong clock basis can only make us re-wait, never declare a wedge
     * before ~240 ms of real wall time has actually passed. */
    struct timespec mono_start;
    clock_gettime(CLOCK_MONOTONIC, &mono_start);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 250L * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_agc_submit.lock);
    g_agc_submit.video          = vout_handle;
    g_agc_submit.buffer_index   = (int)buf_idx;
    g_agc_submit.target         = gpu_target;
    g_agc_submit.source         = stage;
    g_agc_submit.source_bytes   = need;
    g_agc_submit.pitch          = pitch_bytes;
    g_agc_submit.surface_height = coded_height;
    g_agc_submit.visible_width  = vis_w;
    g_agc_submit.visible_height = vis_h;
    g_agc_submit.output_width   = out_w;
    g_agc_submit.output_height  = out_h;
    g_agc_submit.render_marker  = flip_marker;
    g_agc_submit.draw_overlay   = want_overlay;
    g_agc_submit.ovl_source     = ovl_stage;
    g_agc_submit.ovl_w          = ovl_w;
    g_agc_submit.ovl_h          = ovl_h;
    g_agc_submit.ovl_x          = ovl_x;
    g_agc_submit.ovl_y          = ovl_y;
    g_agc_submit.ovl_scale      = ovl_sc;
    g_agc_submit.ovl_alpha      = ovl_alpha;
    g_agc_submit.have_done      = 0;
    g_agc_submit.have_req       = 1;
    pthread_cond_signal(&g_agc_submit.req_cv);

    int timed_out = 0;
    while (!g_agc_submit.have_done) {
        if (pthread_cond_timedwait(&g_agc_submit.done_cv, &g_agc_submit.lock,
                                   &deadline) == ETIMEDOUT) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed_ms = (int64_t)(now.tv_sec - mono_start.tv_sec) * 1000
                               + (now.tv_nsec - mono_start.tv_nsec) / 1000000;
            if (elapsed_ms >= 240) {
                timed_out = 1;
                break;
            }
            /* spurious / clock-basis mismatch - extend the cond deadline and
             * keep waiting for the real 240 ms wall-clock budget. */
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 50L * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
        }
    }

    if (timed_out) {
        /* Abandon the worker - it is still inside the GPU call. Never dispatch
         * to it again; a late completion has stale request state cleared so it
         * won't re-run, but it may still write `target` + queue that flip, so
         * the caller must adopt_flip (not release) this buffer. */
        g_agc_submit.have_req = 0;
        g_agc.submit_wedged = 1;
        g_agc.ready = 0;
        pthread_mutex_unlock(&g_agc_submit.lock);
        evo_boot_log("pp_agc: SUBMIT WEDGED (>250ms in agc_render_frame) - GPU "
                     "present ABANDONED for the session, CPU converter from here.");
        evo_boot_log_flush();
        return -2;
    }

    int32_t rc = g_agc_submit.rc;
    g_agc_submit.have_done = 0;
    pthread_mutex_unlock(&g_agc_submit.lock);

    if (fg) agc_dbg("A3 caller got rc=0x%08x", (unsigned)rc);
    return rc == 0 ? 0 : -1;
}

/* #28 Phase 1 test hook: /mnt/usb0/evo_agc_test_overlay draws a translucent
 * cyan NV12 bar bottom-centre over the live video, exercising the overlay quad
 * without the Phase 2 OSD compositor. Verifies the 2nd DrawIndexAuto + blend +
 * overlay staging + flush path on hardware. */
#define AGC_TEST_OVL_W 960u
#define AGC_TEST_OVL_H 160u
static int agc_test_overlay_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = 0;
        if (getenv("EVO_AGC_TEST_OVERLAY")) mode = 1;
        else { FILE *f = fopen("/mnt/usb0/evo_agc_test_overlay", "r"); if (f) { mode = 1; fclose(f); } }
    }
    return mode;
}
static const uint8_t *agc_test_overlay_surface(void)
{
    static uint8_t buf[AGC_TEST_OVL_W * AGC_TEST_OVL_H
                       + AGC_TEST_OVL_W * (AGC_TEST_OVL_H / 2u)];
    static int built = 0;
    if (!built) {
        uint8_t *y  = buf;
        uint8_t *uv = buf + AGC_TEST_OVL_W * AGC_TEST_OVL_H;
        for (uint32_t r = 0; r < AGC_TEST_OVL_H; r++)
            for (uint32_t c = 0; c < AGC_TEST_OVL_W; c++)
                y[r * AGC_TEST_OVL_W + c] = (uint8_t)(40u + (c * 180u) / AGC_TEST_OVL_W);
        for (uint32_t r = 0; r < AGC_TEST_OVL_H / 2u; r++)
            for (uint32_t c = 0; c < AGC_TEST_OVL_W / 2u; c++) {
                uv[(r * (AGC_TEST_OVL_W / 2u) + c) * 2u + 0] = 200; /* U -> cyan/blue */
                uv[(r * (AGC_TEST_OVL_W / 2u) + c) * 2u + 1] = 60;  /* V */
            }
        built = 1;
    }
    return buf;
}

int pp_agc_present_nv12(int vout_handle, uint32_t buf_idx, void *gpu_target,
                        const void *nv12, uint32_t pitch_bytes, uint32_t coded_height,
                        uint32_t vis_w, uint32_t vis_h, uint32_t out_w, uint32_t out_h,
                        int64_t flip_marker)
{
    const void *ovl = 0;
    uint32_t ox = 0, oy = 0;
    if (agc_test_overlay_mode() && out_w >= AGC_TEST_OVL_W && out_h >= AGC_TEST_OVL_H) {
        ovl = agc_test_overlay_surface();
        ox = (out_w - AGC_TEST_OVL_W) / 2u;
        oy = out_h - AGC_TEST_OVL_H - out_h / 20u;
    }
    return pp_agc_present_nv12_overlay(vout_handle, buf_idx, gpu_target, nv12, pitch_bytes,
                                       coded_height, vis_w, vis_h, out_w, out_h, flip_marker,
                                       ovl, ovl ? AGC_TEST_OVL_W : 0u,
                                       ovl ? AGC_TEST_OVL_H : 0u, ox, oy, 1u, 0.6f);
}

/* #28 Phase 3: opt-in gate for the GPU menu present path (linear menu VO +
 * pp_agc_present_ui). Default --agc-probe build keeps the CPU-tiled menu VO. */
int pp_agc_ui_ready(void)
{
    static int e = -1;
    if (e < 0) {
        e = 0;
        if (getenv("EVO_AGC_UI")) e = 1;
        else { FILE *f = fopen("/mnt/usb0/evo_agc_ui", "r"); if (f) { e = 1; fclose(f); } }
    }
    return e && g_agc.ready;
}

/* BT.709 full-range RGB->YUV, matching the sceAgc NV12->RGB shader
 * (agc_pixel_constants). BGRA 0xAABBGGRR in, tightly-packed NV12 out (Y plane
 * w*h, interleaved UV at y + w*h). w,h must be even. */
static void agc_bgra_to_nv12(uint8_t *y, uint8_t *uv, const uint32_t *bgra,
                             uint32_t w, uint32_t h)
{
    for (uint32_t r = 0; r < h; r++) {
        const uint32_t *sr = bgra + (size_t)r * w;
        uint8_t *yr = y + (size_t)r * w;
        for (uint32_t c = 0; c < w; c++) {
            uint32_t px = sr[c];
            int R = (int)(px & 0xff), G = (int)((px >> 8) & 0xff), B = (int)((px >> 16) & 0xff);
            int Y = (54 * R + 183 * G + 19 * B + 128) >> 8;   /* .2126/.7152/.0722 */
            yr[c] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);
        }
    }
    for (uint32_t r = 0; r < h; r += 2) {
        const uint32_t *s0 = bgra + (size_t)r * w;
        const uint32_t *s1 = bgra + (size_t)(r + 1) * w;
        uint8_t *ur = uv + (size_t)(r / 2u) * w;
        for (uint32_t c = 0; c < w; c += 2) {
            uint32_t p0 = s0[c], p1 = s0[c + 1], p2 = s1[c], p3 = s1[c + 1];
            int R = ((int)(p0 & 0xff) + (int)(p1 & 0xff) + (int)(p2 & 0xff) + (int)(p3 & 0xff)) >> 2;
            int G = ((int)((p0 >> 8) & 0xff) + (int)((p1 >> 8) & 0xff) +
                     (int)((p2 >> 8) & 0xff) + (int)((p3 >> 8) & 0xff)) >> 2;
            int Bb = ((int)((p0 >> 16) & 0xff) + (int)((p1 >> 16) & 0xff) +
                      (int)((p2 >> 16) & 0xff) + (int)((p3 >> 16) & 0xff)) >> 2;
            int Cb = ((-29 * R - 99 * G + 128 * Bb + 128) >> 8) + 128;   /* -.1146/-.3854/.5 */
            int Cr = ((128 * R - 116 * G - 12 * Bb + 128) >> 8) + 128;   /* .5/-.4542/-.0458 */
            ur[c]     = (uint8_t)(Cb < 0 ? 0 : Cb > 255 ? 255 : Cb);
            ur[c + 1] = (uint8_t)(Cr < 0 ? 0 : Cr > 255 ? 255 : Cr);
        }
    }
}

int pp_agc_present_ui(int vout_handle, uint32_t buf_idx, void *gpu_target,
                      const uint32_t *bgra, uint32_t w, uint32_t h,
                      uint32_t out_w, uint32_t out_h, int64_t flip_marker)
{
    static uint8_t *nv;
    static size_t   nv_cap;
    if (!bgra || !w || !h || (w & 1u) || (h & 1u))
        return -1;

    size_t need = (size_t)w * h + (size_t)w * (h / 2u);
    if (nv_cap < need) {
        free(nv);
        nv = (uint8_t *)malloc(need);
        nv_cap = nv ? need : 0;
        if (!nv) return -1;
    }
    agc_bgra_to_nv12(nv, nv + (size_t)w * h, bgra, w, h);

    /* nv12=nv, pitch=w, coded_h=h, vis=w x h, out=out_w x out_h, no overlay */
    return pp_agc_present_nv12_overlay(vout_handle, buf_idx, gpu_target, nv, w, h,
                                       w, h, out_w, out_h, flip_marker,
                                       0, 0, 0, 0, 0, 0, 0.f);
}

void pp_agc_shutdown(void)
{
    /* Stop the worker first - unless it is wedged, in which case it is blocked
     * in a GPU syscall forever and joining would hang shutdown. Leaking it (and
     * the mappings it may still touch) is fine: shutdown only runs at exit. */
    if (g_agc_submit.started && !g_agc.submit_wedged) {
        pthread_mutex_lock(&g_agc_submit.lock);
        g_agc_submit.quit = 1;
        pthread_cond_signal(&g_agc_submit.req_cv);
        pthread_mutex_unlock(&g_agc_submit.lock);
        pthread_join(g_agc_submit.thread, NULL);
        g_agc_submit.started = 0;
    }
    if (g_geo_submit.started && !g_agc.geo_wedged) {
        pthread_mutex_lock(&g_geo_submit.lock);
        g_geo_submit.quit = 1;
        pthread_cond_signal(&g_geo_submit.req_cv);
        pthread_mutex_unlock(&g_geo_submit.lock);
        pthread_join(g_geo_submit.thread, NULL);
        g_geo_submit.started = 0;
    }

    if (g_agc.submit_wedged || g_agc.geo_wedged)
        return;

    agc_staging_free();
    agc_ovl_staging_free();
    agc_geo_staging_free();
    if (g_agc.mem) {
        sceKernelMunmap(g_agc.mem, SHADER_MEMORY_BYTES);
        g_agc.mem = 0;
    }
    if (g_agc.mem_start >= 0) {
        sceKernelReleaseDirectMemory(g_agc.mem_start, SHADER_MEMORY_BYTES);
        g_agc.mem_start = -1;
    }
    g_agc.ready = 0;
    g_agc.vs = g_agc.ps = 0;
}

#else  /* host / payload */

int  pp_agc_init(uint32_t w, uint32_t h, int hdr) { (void)w; (void)h; (void)hdr; return -1; }
int  pp_agc_available(void) { return 0; }
int  pp_agc_probe_ui_shaders(void) { return -1; }
int  pp_agc_present_nv12(int vh, uint32_t bi, void *gt, const void *n, uint32_t p,
                         uint32_t ch, uint32_t vw, uint32_t vh2, uint32_t ow,
                         uint32_t oh, int64_t m)
{
    (void)vh; (void)bi; (void)gt; (void)n; (void)p; (void)ch;
    (void)vw; (void)vh2; (void)ow; (void)oh; (void)m;
    return -1;
}
int  pp_agc_present_nv12_overlay(int vh, uint32_t bi, void *gt, const void *n, uint32_t p,
                                 uint32_t ch, uint32_t vw, uint32_t vh2, uint32_t ow,
                                 uint32_t oh, int64_t m, const void *on, uint32_t ow2,
                                 uint32_t oh2, uint32_t ox, uint32_t oy, uint32_t os, float oa)
{
    (void)vh; (void)bi; (void)gt; (void)n; (void)p; (void)ch; (void)vw; (void)vh2; (void)ow;
    (void)oh; (void)m; (void)on; (void)ow2; (void)oh2; (void)ox; (void)oy; (void)os; (void)oa;
    return -1;
}
int  pp_agc_present_ui(int vh, uint32_t bi, void *gt, const uint32_t *b, uint32_t w,
                       uint32_t h, uint32_t ow, uint32_t oh, int64_t m)
{
    (void)vh; (void)bi; (void)gt; (void)b; (void)w; (void)h; (void)ow; (void)oh; (void)m;
    return -1;
}
int  pp_agc_ui_ready(void) { return 0; }
int  pp_agc_geo_available(void) { return 0; }
int  pp_agc_present_geo(int vh, uint32_t bi, void *gt, int tl, const void *v, uint32_t vc,
                        const uint32_t *ix, uint32_t ic, const pp_agc_geo_draw_t *dr,
                        uint32_t dc, uint32_t ow, uint32_t oh,
                        const uint32_t *tb, uint32_t tw, uint32_t th, int64_t m)
{
    (void)vh; (void)bi; (void)gt; (void)tl; (void)v; (void)vc; (void)ix; (void)ic;
    (void)dr; (void)dc; (void)ow; (void)oh; (void)tb; (void)tw; (void)th; (void)m;
    return -1;
}
void pp_agc_shutdown(void) {}

#endif /* EVO_APP_MODULE */
