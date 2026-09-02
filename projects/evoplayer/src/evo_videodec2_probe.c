/*
 * evo_videodec2_probe.c - sceVideodec2 (Route B) hardware-decode gate, run from
 * inside the PPSA99039 app module. See evo_videodec2_probe.h and
 * docs/evo-pro/videodec2-abi.md. Compiled only under -DEVO_VIDEODEC2_PROBE.
 *
 * A near-verbatim C port of ProsperoLight's PROSPEROLIGHT_VDEC_SELF_TEST
 * (third_party/ProsperoLight/_EVO_local/vdec-self-test.patch), which decoded a
 * 1080p H.264 IDR on the VCN block from a fake-signed game module on 2026-09-01.
 * The bundled access unit is that self-test's bitstream, copied verbatim.
 *
 * libSceVideodec2 is a link-time NEEDED dep (PRX import stub
 * tools/native-app/stubs/prx/libSceVideodec2.syms); the symbols are called
 * directly. sceSysmoduleLoadModule(207) still runs the module_start - unlike
 * AvPlayer's 0xA5, module 207 is not gated for a fake-signed module.
 *
 * App-module context: NO stdout. Result comes back as a notification popup.
 */
#ifdef EVO_VIDEODEC2_PROBE

#include "evo_videodec2_probe.h"
#include "sce/sce_videodec2.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "evo_vdec_selftest_au.inc"   /* kVdecSelfTestBitstream[] */

/* --- externs (libkernel + libSceSysmodule, both already NEEDED) ---------- */
extern int      sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern unsigned int sceKernelUsleep(unsigned int us);
extern int      sceSysmoduleLoadModule(uint16_t id);
extern int      sceSysmoduleUnloadModule(uint16_t id);
extern int64_t  sceKernelGetDirectMemorySize(void);
extern int      sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int      sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int      sceKernelReleaseDirectMemory(int64_t, size_t);
extern int      sceKernelMunmap(void *, size_t);
extern int      sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
extern int      sceKernelReleaseFlexibleMemory(void *, size_t);
extern int      sceKernelAvailableFlexibleMemorySize(size_t *);

#define PIPELINE_BUFFER_COUNT 3u
#define INPUT_SLOT_BYTES      0x800000u   /* 8 MiB */
#define SCE_SYSMODULE_VIDEODEC2_NUM 207

struct v2_note { char pad[45]; char msg[3075]; };
static void note(const char *fmt, ...)
{
    struct v2_note n;
    va_list ap;
    memset(&n, 0, sizeof n);
    va_start(ap, fmt);
    vsnprintf(n.msg, sizeof n.msg, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

/* watchdog: _exit() EVO if a call hangs (safer than a wedged app slot). */
static volatile int g_stage = 0, g_stage_seen = -1;
static void *watchdog(void *a)
{
    (void)a;
    for (int i = 0; i < 40; i++) {
        sceKernelUsleep(1000 * 1000);
        if (g_stage != g_stage_seen) { g_stage_seen = g_stage; i = 0; }
    }
    note("EVO vdec2: WATCHDOG at stage %d - closing app", g_stage);
    _exit(80 + g_stage);
    return 0;
}

static size_t align16k(size_t v) { return (v + 0x3fff) & ~(size_t)0x3fff; }

/* ProsperoLight allocate_direct: AllocateDirectMemory type 12, align 0x4000,
 * then MapDirectMemory with the given protection. */
static int alloc_direct(size_t size, int prot, int64_t limit,
                        int64_t *start, void **addr)
{
    int rc = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(addr, size, prot, 0, *start, 0x4000);
    return rc;
}
static void free_direct(void *addr, int64_t start, size_t size)
{
    if (addr)      sceKernelMunmap(addr, size);
    if (start >= 0) sceKernelReleaseDirectMemory(start, size);
}

void evo_videodec2_probe(void)
{
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    /* H.264 1080p High@L5.1 — ProsperoLight's video_modes[0]. */
    SceVideodec2DecoderConfigInfo config;
    SceVideodec2DecoderMemoryInfo memory;
    SceVideodec2ComputeConfigInfo compute_config;
    SceVideodec2ComputeMemoryInfo compute_memory;
    SceVideodec2InputData  input;
    SceVideodec2FrameBuffer frame;
    SceVideodec2OutputInfo  output;
    memset(&config, 0, sizeof config);
    memset(&memory, 0, sizeof memory);
    memset(&compute_config, 0, sizeof compute_config);
    memset(&compute_memory, 0, sizeof compute_memory);
    memset(&input, 0, sizeof input);
    memset(&frame, 0, sizeof frame);
    memset(&output, 0, sizeof output);

    void *decoder = NULL, *compute_queue = NULL, *input_mem = NULL, *frame_mem = NULL;
    int64_t compute_start = -1, gpu_start = -1, cpu_gpu_start = -1,
            input_start = -1, frame_start = -1;
    size_t compute_size = 0, gpu_size = 0, cpu_gpu_size = 0,
           input_pool = 0, frame_pool = 0, frame_size = 0, cpu_map = 0;
    size_t flex_avail = 0;

    g_stage = 2;
    int sm = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2_NUM);
    if (sm != 0) {
        note("EVO vdec2: sceSysmoduleLoadModule(207) -> 0x%08x — Route B blocked "
             "(module gated for a fake-signed app, like AvPlayer 0xA5)", (unsigned)sm);
        _exit(1);
    }

    int64_t dm_limit = sceKernelGetDirectMemorySize();

    /* --- compute queue --- */
    g_stage = 3;
    compute_memory.size = sizeof compute_memory;
    int q_query = sceVideodec2QueryComputeMemoryInfo(&compute_memory);
    compute_size = align16k((size_t)compute_memory.cpu_gpu_size);
    int q_alloc = alloc_direct(compute_size, 0x33, dm_limit,
                               &compute_start, &compute_memory.cpu_gpu);
    int q_make = -1;
    if (q_alloc == 0) {
        compute_memory.cpu_gpu_size = compute_size;
        compute_config.size = sizeof compute_config;
        compute_config.pipe_id = 0;
        compute_config.queue_id = 0;
        q_make = sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory,
                                                  &compute_queue);
    }

    /* --- decoder config + memory --- */
    g_stage = 4;
    config.size = sizeof config;
    config.resource_type = SCE_VIDEODEC2_RESOURCE_COMPUTE;
    config.codec_type = SCE_VIDEODEC2_CODEC_AVC;
    config.profile = 100;                 /* High */
    config.max_level = 51;                /* 5.1 (60 fps path) */
    config.max_width = 1920;
    config.max_height = 1088;
    config.max_dpb_frames = 4;
    config.pipeline_depth = 1;
    config.compute_queue = (uint64_t)compute_queue;
    config.cpu_affinity = 0x3f;
    config.cpu_priority = 700;
    config.optimize_progressive = 1;

    memory.size = sizeof memory;
    int d_query = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);

    cpu_map = align16k((size_t)memory.cpu_size);
    sceKernelAvailableFlexibleMemorySize(&flex_avail);
    int cpu_rc = sceKernelMapNamedFlexibleMemory(&memory.cpu, cpu_map, 0x03, 0,
                                                 "EvoVdec2Probe");

    gpu_size    = align16k((size_t)memory.gpu_size);
    cpu_gpu_size = align16k((size_t)memory.cpu_gpu_size);
    frame_size  = align16k((size_t)memory.max_frame_size);
    input_pool  = (size_t)INPUT_SLOT_BYTES * PIPELINE_BUFFER_COUNT;
    frame_pool  = frame_size * PIPELINE_BUFFER_COUNT;
    memory.gpu_size = gpu_size;
    if (cpu_gpu_size) memory.cpu_gpu_size = cpu_gpu_size;

    int a_rc = alloc_direct(gpu_size, 0x32, dm_limit, &gpu_start, &memory.gpu);
    if (a_rc == 0 && cpu_gpu_size)
        a_rc = alloc_direct(cpu_gpu_size, 0x33, dm_limit, &cpu_gpu_start, &memory.cpu_gpu);
    if (a_rc == 0)
        a_rc = alloc_direct(input_pool, 0x32, dm_limit, &input_start, &input_mem);
    if (a_rc == 0)
        a_rc = alloc_direct(frame_pool, 0x32, dm_limit, &frame_start, &frame_mem);

    /* --- create + decode --- */
    g_stage = 5;
    int c_rc = -1, r_rc = -1, dec_rc = -1, flush_rc = -1, valid = 0;
    if (compute_queue && d_query == 0 && cpu_rc == 0 && a_rc == 0 && memory.cpu) {
        c_rc = sceVideodec2CreateDecoder(&config, &memory, &decoder);
        if (c_rc == 0 && decoder) {
            r_rc = sceVideodec2Reset(decoder);

            const size_t au = sizeof kVdecSelfTestBitstream;
            memcpy(input_mem, kVdecSelfTestBitstream, au);
            input.size = sizeof input;
            input.au = input_mem;
            input.au_size = au;
            input.pts = 0;
            input.dts = UINT64_MAX;
            input.attached = 0;
            frame.size = sizeof frame;
            frame.buffer = frame_mem;
            frame.buffer_size = frame_size;
            output.size = sizeof output;

            g_stage = 6;
            dec_rc = sceVideodec2Decode(decoder, &input, &frame, &output);
            if (dec_rc == 0 && !output.valid) {
                memset(&output, 0, sizeof output);
                output.size = sizeof output;
                flush_rc = sceVideodec2Flush(decoder, &frame, &output);
            }
            valid = (dec_rc == 0 || flush_rc == 0) && output.valid && !output.error;
        }
    }

    note("EVO vdec2: %s\n"
         "sysmod207=0 cQ:q=%08x a=%08x mk=%08x  dMem:q=%08x cpuMap=%08x alloc=%08x\n"
         "CREATE=%08x reset=%08x DECODE=%08x flush=%08x\n"
         "out valid=%u err=%u pics=%u %ux%u pitch=%u codec=%u  flexAvail=%zx",
         valid ? "HARDWARE DECODE OK (Route B viable)"
               : c_rc == 0 ? "decoder created, DECODE failed"
                           : "decoder creation FAILED",
         (unsigned)q_query, (unsigned)q_alloc, (unsigned)q_make,
         (unsigned)d_query, (unsigned)cpu_rc, (unsigned)a_rc,
         (unsigned)c_rc, (unsigned)r_rc, (unsigned)dec_rc, (unsigned)flush_rc,
         (unsigned)output.valid, (unsigned)output.error, (unsigned)output.picture_count,
         output.width, output.height, output.pitch, output.codec, flex_avail);

    /* teardown (rc ignored) */
    g_stage = 7;
    if (decoder) sceVideodec2DeleteDecoder(decoder);
    free_direct(frame_mem, frame_start, frame_pool);
    free_direct(input_mem, input_start, input_pool);
    free_direct(memory.cpu_gpu, cpu_gpu_start, cpu_gpu_size);
    free_direct(memory.gpu, gpu_start, gpu_size);
    if (memory.cpu) {
        sceKernelReleaseFlexibleMemory(memory.cpu, cpu_map);
        sceKernelMunmap(memory.cpu, cpu_map);
    }
    if (compute_queue) sceVideodec2ReleaseComputeQueue(compute_queue);
    free_direct(compute_memory.cpu_gpu, compute_start, compute_size);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_VIDEODEC2_NUM);

    g_stage = 9;
}

#endif /* EVO_VIDEODEC2_PROBE */
