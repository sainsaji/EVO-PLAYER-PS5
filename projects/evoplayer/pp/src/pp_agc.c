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

/* --- constants (ProsperoLight) ----------------------------------------- */
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

    volatile int first_frame_ok;   /* a render_frame has returned 0 (worker)   */
    int       first_frame_logged;
    int       tv_safe;             /* 0 = full-frame (parity with CPU path)  */
    volatile int submit_wedged;    /* a submit blew the 250ms watchdog once    */
} g_agc;

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

    /* response */
    int32_t     rc;
    uint32_t    word_count;
} g_agc_submit;

static int agc_submit_start(void);

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

/* One presented frame. Verbatim port of ProsperoLight render_frame(), SDR path
 * only, HUD/overlay branch removed. Scratch offsets inside `memory`. */
static int agc_render_frame(int video, int buffer_index, void *target, uint8_t *memory,
                            void *vertex_shader, void *pixel_shader,
                            const void *source, size_t source_bytes, uint32_t pitch,
                            uint32_t surface_height, uint32_t visible_width,
                            uint32_t visible_height, uint32_t output_width,
                            uint32_t output_height, int64_t render_marker,
                            uint32_t *word_count)
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

    if (!defaults || visible_width == 0 || visible_height == 0 || visible_width > pitch ||
        visible_height > surface_height || (pitch & 1u) != 0 || y_bytes > UINT32_MAX ||
        uv_bytes > UINT32_MAX || y_bytes + uv_bytes > source_bytes || output_width == 0 ||
        output_height == 0)
        return -1;

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

    sceAgcDriverWaitUntilSafeForRendering(&command.up,
                                          sceAgcDriverGetWaitRenderingPacketSizeInDwords(), 0,
                                          (uint32_t)video, buffer_index);

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

    bind_pixel_source(&command, resources, source, y_bytes, uv_bytes, pixel_cb);
    sceAgcDcbDrawIndexAuto(&command, 4, 2);
    sceAgcDcbSetFlip(&command, (uint32_t)video, buffer_index, 1, render_marker);

    submit.words = words;
    submit.word_count = (uint32_t)(command.up - words);
    *word_count = submit.word_count;
    flush_gpu_data(memory, SHADER_STATIC_BYTES);
    {
        int32_t result = sceAgcDriverSubmitDcb(&submit);
        if (result == 0)
            result = sceAgcSuspendPoint();
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
    return 0;
}

int pp_agc_available(void)
{
    return g_agc.ready;
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

/* --- fault guard for the first render_frame (evo_agc_probe.c pattern) -----
 * Runs on the submit worker thread now (the thread that would fault). Only a
 * CPU fault is caught here - the GPU-side stall is the watchdog's job. */
static sigjmp_buf g_agc_jmp;
static volatile sig_atomic_t g_agc_armed;

static void agc_fault_h(int sig)
{
    if (g_agc_armed) {
        g_agc_armed = 0;
        siglongjmp(g_agc_jmp, sig);
    }
    _exit(150 + sig);
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
        pthread_mutex_unlock(&g_agc_submit.lock);

        guard = !g_agc.first_frame_ok;
        if (guard) {
            struct sigaction sa, o_segv, o_bus, o_ill;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = agc_fault_h;
            sigaction(SIGSEGV, &sa, &o_segv);
            sigaction(SIGBUS, &sa, &o_bus);
            sigaction(SIGILL, &sa, &o_ill);

            if (sigsetjmp(g_agc_jmp, 1) != 0) {
                g_agc_armed = 0;
                sigaction(SIGSEGV, &o_segv, NULL);
                sigaction(SIGBUS, &o_bus, NULL);
                sigaction(SIGILL, &o_ill, NULL);
                evo_boot_log("pp_agc: FAULT in first agc_render_frame - GPU present "
                             "DISABLED, playback falls back to the CPU converter");
                evo_boot_log_flush();
                g_agc.ready = 0;
                rc = -99;
                words = 0;
                goto reply;
            }
            g_agc_armed = 1;
            rc = agc_render_frame(video, buffer_index, target, g_agc.mem,
                                  g_agc.vs, g_agc.ps, source, source_bytes, pitch,
                                  surface_height, visible_width, visible_height,
                                  output_width, output_height, render_marker, &words);
            g_agc_armed = 0;
            sigaction(SIGSEGV, &o_segv, NULL);
            sigaction(SIGBUS, &o_bus, NULL);
            sigaction(SIGILL, &o_ill, NULL);

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
                                  output_width, output_height, render_marker, &words);
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
 * there is never more than one request in flight. */
int pp_agc_present_nv12(int vout_handle, uint32_t buf_idx, void *gpu_target,
                        const void *nv12, uint32_t pitch_bytes, uint32_t coded_height,
                        uint32_t vis_w, uint32_t vis_h, uint32_t out_w, uint32_t out_h,
                        int64_t flip_marker)
{
    if (g_agc.submit_wedged)
        return -2;
    if (!g_agc.ready || !g_agc.mem || !g_agc.vs || !g_agc.ps || !g_agc_submit.started)
        return -1;
    if (!nv12 || !gpu_target || vout_handle < 0 || buf_idx >= (uint32_t)PP_VO_MAX_BUFFERS ||
        pitch_bytes == 0 || (pitch_bytes & 1u) || coded_height == 0 ||
        vis_w == 0 || vis_h == 0 || out_w == 0 || out_h == 0)
        return -1;

    size_t need = (size_t)pitch_bytes * coded_height                     /* Y  */
                + (size_t)pitch_bytes * ((coded_height + 1u) / 2u);      /* UV */
    if (agc_staging_ensure(need) != 0)
        return -1;

    uint8_t *stage = g_agc.stage[buf_idx];
    memcpy(stage, nv12, need);
    flush_gpu_data(stage, need);

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
                     "present ABANDONED for the session, CPU converter from here. "
                     "The VO is linear-registered while AGC is on, so the CPU "
                     "fallback picture may be garbled until the app restarts.");
        evo_boot_log_flush();
        return -2;
    }

    int32_t rc = g_agc_submit.rc;
    g_agc_submit.have_done = 0;
    pthread_mutex_unlock(&g_agc_submit.lock);

    return rc == 0 ? 0 : -1;
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
    if (g_agc.submit_wedged)
        return;

    agc_staging_free();
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
int  pp_agc_present_nv12(int vh, uint32_t bi, void *gt, const void *n, uint32_t p,
                         uint32_t ch, uint32_t vw, uint32_t vh2, uint32_t ow,
                         uint32_t oh, int64_t m)
{
    (void)vh; (void)bi; (void)gt; (void)n; (void)p; (void)ch;
    (void)vw; (void)vh2; (void)ow; (void)oh; (void)m;
    return -1;
}
void pp_agc_shutdown(void) {}

#endif /* EVO_APP_MODULE */
