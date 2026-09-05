/*
 * evo_vdec_native.c — sceVideodec2 (Route B) hardware-decode backend for the
 * evo_vdec.h seam. Native-decode plan Phase 4 (#31).
 *
 * A port of the hardware-verified bring-up / decode / teardown sequence in
 * projects/evoplayer/src/evo_videodec2_probe.c (which decoded a 1920x1088 NV12
 * H.264 frame inside the full PPSA99039 player on 2026-09-03 — every call 0).
 * The ABI transcription lives in docs/evo-pro/videodec2-abi.md; this file does
 * not re-derive it.
 *
 * Real body only under EVO_APP_MODULE — sceVideodec2, a user session and the
 * GPU driver stack exist only in the registered app module. Host + payload
 * builds get the stubs at the bottom, so the dispatcher (evo_vdec_ffmpeg.c)
 * always downgrades to FFmpeg.
 *
 * RESIDENT DECODER — the sequencing constraint, learned on hardware 2026-09-03.
 * The self-unjail (evo_jailbreak_self / _ensure) swaps process credentials
 * mid-run, after which EVERY libSceVideodec2 call fails
 * (sceVideodec2QueryComputeMemoryInfo -> 0x811D0111), not just
 * sceSysmoduleLoadModule. So the compute queue + decoder are brought up ONCE
 * at boot in evo_vdec_native_probe() — before the first evo_jailbreak_self() —
 * as a session-resident singleton (g_boot), sized for 4K AVC High. Each
 * evo_vdec_native_open() just sceVideodec2Reset()s that decoder and feeds it;
 * nothing is created or destroyed at playback time. HEVC and >4K fall back to
 * FFmpeg (a second resident HEVC decoder is a later step).
 *
 * DEMUX -> AU FORMAT. sceVideodec2 wants Annex-B (start-code NALs, SPS/PPS
 * in-band). For mp4/mkv (avcC extradata) we run AUs through the
 * h264_mp4toannexb bitstream filter; already-Annex-B streams pass through.
 *
 * FRAME ORDER. SceVideodec2OutputInfo carries no PTS and no picture-detail
 * hook is bound. This backend assumes DISPLAY-order emission + min-PTS pairing,
 * with a small PTS-sorted reorder window as a safety net. If B-frame content
 * misbehaves, build with -DEVO_VDEC_NATIVE_DECODE_ORDER=1 and a deeper
 * -DEVO_VDEC_REORDER_DEPTH.
 */
#include "evo_vdec_native.h"
#include "evo_boot_log.h"
#include "pp_agc.h"

#ifdef EVO_APP_MODULE

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/rational.h>

#include "sce/sce_videodec2.h"

/* --- externs (libkernel + libSceSysmodule, both already DT_NEEDED) --------- */
extern int      sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int      sceSysmoduleLoadModule(uint16_t id);
extern int64_t  sceKernelGetDirectMemorySize(void);
extern int      sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int      sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int      sceKernelReleaseDirectMemory(int64_t, size_t);
extern int      sceKernelMunmap(void *, size_t);
extern int      sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
extern int      sceKernelReleaseFlexibleMemory(void *, size_t);

#define SCE_SYSMODULE_VIDEODEC2_NUM 207
#define INPUT_SLOT_BYTES            0x1000000u  /* 16 MiB — 4K high-bitrate IDR headroom */

/* Match SharpProspero's production config: DPB auto-sized by the decoder
 * (MaxDpbFrameCount = -1), DecodeInputQueueDepth = 4. The frame buffers we
 * hand sceVideodec2Decode are output/detile targets consumed immediately
 * (ro_harvest copies out); the decoder keeps its own DPB internally in
 * GpuMemory. A small ring with margin over the input-queue depth suffices. */
#define SCE_VIDEODEC2_AUTO_FRAMES   (-1)
#define DECODE_INPUT_QUEUE_DEPTH    4u
#define PIPELINE_BUFFER_COUNT       4u          /* input-AU ring */
#define FRAME_POOL_SLOTS            12

#ifndef EVO_VDEC_REORDER_DEPTH
#define EVO_VDEC_REORDER_DEPTH      4
#endif
#define RO_SLOTS   (EVO_VDEC_REORDER_DEPTH + 1)
#define PTS_POOL   (EVO_VDEC_REORDER_DEPTH + (int)PIPELINE_BUFFER_COUNT + 8)

/* Resident decoder size. 4K AVC High covers the case native decode exists for
 * (4K H.264 that blows the software frame pool — e.g. the GTA trailer). */
#ifndef EVO_VDEC_NATIVE_MAX_W
#define EVO_VDEC_NATIVE_MAX_W  3840
#endif
#ifndef EVO_VDEC_NATIVE_MAX_H
#define EVO_VDEC_NATIVE_MAX_H  2176
#endif

/* App-module diagnostics: under -DEVO_VDEC_LOG (package-app.sh --usb-remote)
 * an fsync'd append to /mnt/usb0/evo_vdec.log for an FTP pull; the per-frame
 * fsync is dev-only. The notification popup below used to be unconditional -
 * "always visible on the TV" - but this fires on every decode heartbeat/
 * error, i.e. routinely *during playback*, not just at boot/open/close, and
 * was reported as recurring "EVO vdec native:" popups mid-playback (#51
 * follow-up). It now shares evo_bt()'s EVO_BOOT_TRACE_POPUP opt-in
 * (scripts/package-app.sh --breadcrumbs). */
#if defined(EVO_BOOT_TRACE_POPUP)
struct v2n_note { char pad[45]; char msg[3075]; };
#endif
static void note(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

#if defined(EVO_BOOT_TRACE_POPUP)
    struct v2n_note n;
    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "%s", msg);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
#endif

    /* evo_vdec_native_probe() runs pre-unjail — route it to the buffered
     * boot log too (harmless overlap once /mnt/usb0 is up). */
    evo_boot_log("%s", msg);

#ifdef EVO_VDEC_LOG
    FILE *f = fopen("/mnt/usb0/evo_vdec.log", "a");
    if (f) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(f, "[%lld.%03ld] %s\n", (long long)ts.tv_sec,
                ts.tv_nsec / 1000000L, msg);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
#endif
}

static size_t align16k(size_t v) { return (v + 0x3fffu) & ~(size_t)0x3fffu; }
static int    roundup16(int v)   { return (v + 15) & ~15; }

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
    if (addr)       sceKernelMunmap(addr, size);
    if (start >= 0) sceKernelReleaseDirectMemory(start, size);
}

/* ---------------------------------------------------------------------------
 * Session-resident decoder — created ONCE, pre-unjail, in evo_vdec_native_probe().
 * ------------------------------------------------------------------------ */

static struct {
    int      tried;
    int      ready;
    void    *compute_queue;
    void    *compute_mem;   int64_t compute_start; size_t compute_size;
    void    *decoder;
    void    *cpu_mem;       size_t  cpu_map;
    void    *gpu_mem;       int64_t gpu_start;     size_t gpu_size;
    void    *cpu_gpu_mem;   int64_t cpu_gpu_start; size_t cpu_gpu_size;
    void    *input_mem;     int64_t input_start;   size_t input_pool;
    void    *frame_mem;     int64_t frame_start;   size_t frame_pool;
    size_t   frame_size;
    uint32_t max_w, max_h;
} g_boot;

static void boot_teardown(void)
{
    if (g_boot.decoder) { sceVideodec2DeleteDecoder(g_boot.decoder); g_boot.decoder = NULL; }
    free_direct(g_boot.frame_mem,   g_boot.frame_start,   g_boot.frame_pool);
    free_direct(g_boot.input_mem,   g_boot.input_start,   g_boot.input_pool);
    free_direct(g_boot.cpu_gpu_mem, g_boot.cpu_gpu_start, g_boot.cpu_gpu_size);
    free_direct(g_boot.gpu_mem,     g_boot.gpu_start,     g_boot.gpu_size);
    if (g_boot.cpu_mem) {
        sceKernelReleaseFlexibleMemory(g_boot.cpu_mem, g_boot.cpu_map);
        sceKernelMunmap(g_boot.cpu_mem, g_boot.cpu_map);
    }
    if (g_boot.compute_queue) { sceVideodec2ReleaseComputeQueue(g_boot.compute_queue); g_boot.compute_queue = NULL; }
    free_direct(g_boot.compute_mem, g_boot.compute_start, g_boot.compute_size);
    memset(&g_boot, 0, sizeof g_boot);
    g_boot.tried = 1;
}

/* Full bring-up for an AVC High decoder at (w x h). Returns 0 on success with
 * everything stored in g_boot; non-zero rc (and g_boot left torn down) on any
 * failure. MUST be called before the first evo_jailbreak_self(). */
static int boot_bringup(int w, int h, const char **stage)
{
    int64_t dm = sceKernelGetDirectMemorySize();
    int rc;

    g_boot.compute_start = g_boot.gpu_start = g_boot.cpu_gpu_start =
        g_boot.input_start = g_boot.frame_start = -1;

    SceVideodec2ComputeMemoryInfo cm;
    SceVideodec2ComputeConfigInfo cc;
    memset(&cm, 0, sizeof cm);
    memset(&cc, 0, sizeof cc);
    cm.size = sizeof cm;
    *stage = "QueryComputeMemoryInfo";
    if ((rc = sceVideodec2QueryComputeMemoryInfo(&cm)) != 0) return rc;
    g_boot.compute_size = align16k((size_t)cm.cpu_gpu_size);
    *stage = "alloc(compute)";
    if ((rc = alloc_direct(g_boot.compute_size, 0x33, dm,
                           &g_boot.compute_start, &g_boot.compute_mem)) != 0) return rc;
    cm.cpu_gpu      = g_boot.compute_mem;
    cm.cpu_gpu_size = g_boot.compute_size;
    cc.size = sizeof cc;
    *stage = "AllocateComputeQueue";
    if ((rc = sceVideodec2AllocateComputeQueue(&cc, &cm, &g_boot.compute_queue)) != 0) return rc;
    if (!g_boot.compute_queue) return -1;

    SceVideodec2DecoderConfigInfo config;
    SceVideodec2DecoderMemoryInfo mem;
    memset(&config, 0, sizeof config);
    memset(&mem, 0, sizeof mem);
    config.size                 = sizeof config;
    config.resource_type        = SCE_VIDEODEC2_RESOURCE_COMPUTE;
    config.codec_type           = SCE_VIDEODEC2_CODEC_AVC;
    config.profile              = 100;                       /* High (superset)  */
    config.max_level            = (w > 1920 || h > 1088) ? 52 : 51;
    config.max_width            = w;
    config.max_height           = h;
    config.max_dpb_frames       = SCE_VIDEODEC2_AUTO_FRAMES;   /* decoder self-sizes */
    config.pipeline_depth       = DECODE_INPUT_QUEUE_DEPTH;
    config.compute_queue        = (uint64_t)g_boot.compute_queue;
    config.cpu_affinity         = 0x3f;
    config.cpu_priority         = 700;
    config.optimize_progressive = 1;

    mem.size = sizeof mem;
    *stage = "QueryDecoderMemoryInfo";
    if ((rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem)) != 0) return rc;

    g_boot.cpu_map = align16k((size_t)mem.cpu_size);
    *stage = "MapNamedFlexibleMemory";
    if ((rc = sceKernelMapNamedFlexibleMemory(&mem.cpu, g_boot.cpu_map, 0x03, 0,
                                              "EvoVdecNative")) != 0) return rc;
    if (!mem.cpu) return -1;
    g_boot.cpu_mem = mem.cpu;

    g_boot.gpu_size     = align16k((size_t)mem.gpu_size);
    g_boot.cpu_gpu_size = align16k((size_t)mem.cpu_gpu_size);
    g_boot.frame_size   = align16k((size_t)mem.max_frame_size);
    g_boot.input_pool   = (size_t)INPUT_SLOT_BYTES * PIPELINE_BUFFER_COUNT;
    g_boot.frame_pool   = g_boot.frame_size * FRAME_POOL_SLOTS;
    if (g_boot.frame_size == 0) return -1;
    mem.gpu_size = g_boot.gpu_size;
    if (g_boot.cpu_gpu_size) mem.cpu_gpu_size = g_boot.cpu_gpu_size;

    *stage = "alloc(gpu)";
    if ((rc = alloc_direct(g_boot.gpu_size, 0x32, dm, &g_boot.gpu_start, &g_boot.gpu_mem)) != 0) return rc;
    mem.gpu = g_boot.gpu_mem;
    if (g_boot.cpu_gpu_size) {
        *stage = "alloc(cpu_gpu)";
        if ((rc = alloc_direct(g_boot.cpu_gpu_size, 0x33, dm,
                               &g_boot.cpu_gpu_start, &g_boot.cpu_gpu_mem)) != 0) return rc;
        mem.cpu_gpu = g_boot.cpu_gpu_mem;
    }
    *stage = "alloc(input)";
    if ((rc = alloc_direct(g_boot.input_pool, 0x32, dm,
                           &g_boot.input_start, &g_boot.input_mem)) != 0) return rc;
    *stage = "alloc(frame)";
    if ((rc = alloc_direct(g_boot.frame_pool, 0x32, dm,
                           &g_boot.frame_start, &g_boot.frame_mem)) != 0) return rc;

    *stage = "CreateDecoder";
    if ((rc = sceVideodec2CreateDecoder(&config, &mem, &g_boot.decoder)) != 0) return rc;
    if (!g_boot.decoder) return -1;
    *stage = "Reset";
    if ((rc = sceVideodec2Reset(g_boot.decoder)) != 0) return rc;

    g_boot.max_w = (uint32_t)w;
    g_boot.max_h = (uint32_t)h;
    *stage = "ok";
    return 0;
}

int evo_vdec_native_probe(void)
{
    if (g_boot.tried)
        return g_boot.ready;
    g_boot.tried = 1;

    int sm = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2_NUM);

    const char *stage = "?";
    int rc = boot_bringup(EVO_VDEC_NATIVE_MAX_W, EVO_VDEC_NATIVE_MAX_H, &stage);
    if (rc != 0) {
        note("EVO vdec native: 4K bring-up FAILED at [%s] rc=0x%08x - trying 1080p",
             stage, (unsigned)rc);
        boot_teardown();
        g_boot.tried = 1;
        rc = boot_bringup(1920, 1088, &stage);
    }
    if (rc != 0) {
        note("EVO vdec native: bring-up FAILED at [%s] rc=0x%08x sysmod=0x%08x "
             "-> FFmpeg only", stage, (unsigned)rc, (unsigned)sm);
        boot_teardown();
        return 0;
    }

    g_boot.ready = 1;
    note("EVO vdec native: RESIDENT decoder up  %ux%u  frame=%zuKB  sysmod=0x%08x",
         g_boot.max_w, g_boot.max_h, g_boot.frame_size >> 10, (unsigned)sm);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Per-playback wrapper — borrows g_boot.decoder, never creates/destroys it.
 * ------------------------------------------------------------------------ */

/*
 * The decoder outputs NV12, but EVO's fast (SIMD / parallel / 4K) converters
 * only accept planar YUV420P — an NV12 4K frame silently produces a black
 * screen. So ro_harvest de-interleaves NV12 -> I420 into the slot: Y copied
 * as-is, the interleaved UV plane split into separate U and V planes. The
 * chroma pass is ~w*h/2 bytes — trivial next to the decode it replaces.
 */
struct nat_slot {
    uint8_t *data;
    size_t   cap;
    int      used;
    int      borrowed;         /* 1 = data points into the decoder frame     */
                               /*     pool (AGC path), not owned - do not     */
                               /*     realloc or free it                     */
    int      nv12;              /* 1 = data is straight NV12 (AGC path)      */
    int64_t  pts;
    uint32_t w, h;              /* display size                             */
    uint32_t coded_h;           /* MB-padded luma rows (UV starts here)      */
    uint32_t y_stride, c_stride;
    size_t   u_off, v_off;      /* byte offsets of U and V within data       */
};

struct evo_vdec_native {
    void    *dec;           /* == g_boot.decoder (borrowed)   */
    uint8_t *input_mem;     /* == g_boot.input_mem            */
    uint8_t *frame_mem;     /* == g_boot.frame_mem            */
    size_t   frame_size;

    uint32_t disp_w, disp_h;
    unsigned au_ring;
    unsigned dec_calls;
    unsigned frames_out;
    int      flushing;
    int      fatal;
    int      agc_out;       /* pp_agc_available() at open — emit NV12, not I420 */
    int      first_valid_logged;
    int      first_err_logged;

    AVBSFContext        *bsf;
    const char          *bsf_name;   /* for rebuild on seek */
    AVCodecParameters   *bsf_par;    /* owned copy, for rebuild on seek */
    AVPacket            *in_pkt;
    AVPacket            *filt_pkt;

    int64_t pts_pool[PTS_POOL];
    int     pts_n;

    struct nat_slot ro[RO_SLOTS];
    int             ro_count;
};

static void pts_push(evo_vdec_native *n, int64_t pts)
{
    if (n->pts_n >= PTS_POOL) {
        memmove(n->pts_pool, n->pts_pool + 1, (PTS_POOL - 1) * sizeof n->pts_pool[0]);
        n->pts_n = PTS_POOL - 1;
    }
    n->pts_pool[n->pts_n++] = pts;
}

static int64_t pts_take(evo_vdec_native *n)
{
    if (n->pts_n <= 0)
        return INT64_MIN;
#ifdef EVO_VDEC_NATIVE_DECODE_ORDER
    int64_t v = n->pts_pool[0];
    memmove(n->pts_pool, n->pts_pool + 1, (n->pts_n - 1) * sizeof n->pts_pool[0]);
    n->pts_n--;
    return v;
#else
    int mi = 0;
    for (int i = 1; i < n->pts_n; i++) {
        if (n->pts_pool[i] == INT64_MIN) continue;
        if (n->pts_pool[mi] == INT64_MIN || n->pts_pool[i] < n->pts_pool[mi])
            mi = i;
    }
    int64_t v = n->pts_pool[mi];
    n->pts_pool[mi] = n->pts_pool[--n->pts_n];
    return v;
#endif
}

static void ro_reset(evo_vdec_native *n)
{
    for (int i = 0; i < RO_SLOTS; i++)
        n->ro[i].used = 0;
    n->ro_count = 0;
}

static void ro_harvest(evo_vdec_native *n, const SceVideodec2OutputInfo *out)
{
    struct nat_slot *s = NULL;
    for (int i = 0; i < RO_SLOTS; i++)
        if (!n->ro[i].used) { s = &n->ro[i]; break; }
    if (!s)
        return;

    if (!out->buffer)
        return;

    const uint32_t cw    = out->pitch_bytes ? out->pitch_bytes : out->pitch;
    const uint32_t codeh = out->height;                 /* MB-padded luma rows */
    const uint32_t dw = (n->disp_w && n->disp_w < out->width)  ? n->disp_w : out->width;
    const uint32_t dh = (n->disp_h && n->disp_h < out->height) ? n->disp_h : out->height;
    const uint32_t chw = (dw + 1u) / 2u;                /* display chroma w/h  */
    const uint32_t chh = (dh + 1u) / 2u;
    const uint32_t cstride = cw / 2u;                   /* per plane, samples  */

    /* I420 layout in the slot: [Y: cw*c+dh][U: cstride*chh][V: cstride*chh] */
    const size_t y_sz = (size_t)cw * codeh;
    const size_t c_sz = (size_t)cstride * ((codeh + 1u) / 2u);
    const size_t need = y_sz + 2u * c_sz;

    if (n->agc_out) {
        /* AGC path: the decoder already laid out contiguous NV12 (Y then the
         * interleaved UV plane) in the frame-pool slot it just wrote. Borrow
         * that pointer instead of copying it into an owned buffer - the reorder
         * window (<= RO_SLOTS frames) is far shorter than the FRAME_POOL_SLOTS
         * cycle before the decoder reuses the slot, so it stays valid until the
         * present consumes it. Saves ~62 MiB of heap (RO_SLOTS owned frames)
         * and a ~12 MiB/frame memcpy - both matter at 4K under the fake-signed
         * flexible-memory ceiling, especially alongside pp_agc's staging. */
        s->data     = (uint8_t *)out->buffer;
        s->borrowed = 1;
        s->nv12     = 1;
        s->y_stride = cw;
        s->c_stride = cw;              /* NV12: chroma row pitch == luma pitch */
        s->u_off    = y_sz;
        s->v_off    = y_sz;
    } else {
        if (s->borrowed) { s->data = NULL; s->cap = 0; s->borrowed = 0; }
        if (s->cap < need) {
            uint8_t *g = (uint8_t *)realloc(s->data, need);
            if (!g)
                return;
            s->data = g;
            s->cap  = need;
        }
        /* Y: verbatim */
        memcpy(s->data, out->buffer, y_sz);

        /* UV interleaved (NV12) -> planar U, V */
        const uint8_t *uv = (const uint8_t *)out->buffer + (size_t)cw * codeh;
        uint8_t *du = s->data + y_sz;
        uint8_t *dv = du + c_sz;
        for (uint32_t row = 0; row < chh; row++) {
            const uint8_t *src = uv + (size_t)row * cw;
            uint8_t *pu = du + (size_t)row * cstride;
            uint8_t *pv = dv + (size_t)row * cstride;
            for (uint32_t x = 0; x < chw; x++) {
                pu[x] = src[2u * x];
                pv[x] = src[2u * x + 1u];
            }
        }

        s->nv12     = 0;
        s->y_stride = cw;
        s->c_stride = cstride;
        s->u_off    = y_sz;
        s->v_off    = y_sz + c_sz;
    }

    s->coded_h  = codeh;
    s->w        = dw;
    s->h        = dh;
    s->pts      = pts_take(n);
    s->used     = 1;
    n->ro_count++;
}

static int decode_one(evo_vdec_native *n, const uint8_t *au, int size, int64_t pts)
{
    if (size <= 0 || (size_t)size > INPUT_SLOT_BYTES)
        return -1;

    unsigned islot = n->au_ring % PIPELINE_BUFFER_COUNT;
    unsigned fslot = n->au_ring % FRAME_POOL_SLOTS;
    n->au_ring++;

    memcpy(n->input_mem + (size_t)islot * INPUT_SLOT_BYTES, au, (size_t)size);

    SceVideodec2InputData  in;
    SceVideodec2FrameBuffer fb;
    SceVideodec2OutputInfo  out;
    memset(&in, 0, sizeof in);
    memset(&fb, 0, sizeof fb);
    memset(&out, 0, sizeof out);
    in.size     = sizeof in;
    in.au       = n->input_mem + (size_t)islot * INPUT_SLOT_BYTES;
    in.au_size  = (uint64_t)size;
    in.pts      = (uint64_t)pts;
    in.dts      = UINT64_MAX;
    in.attached = 0;
    fb.size        = sizeof fb;
    fb.buffer      = n->frame_mem + (size_t)fslot * n->frame_size;
    fb.buffer_size = n->frame_size;
    out.size       = sizeof out;

    int rc = sceVideodec2Decode(n->dec, &in, &fb, &out);
    n->dec_calls++;
    if (n->dec_calls <= 3 || (out.valid && !n->first_valid_logged)) {
        if (out.valid) n->first_valid_logged = 1;
        note("EVO vdec native: Decode #%u rc=0x%08x acc=%u valid=%u err=%u %ux%u "
             "pitch=%u", n->dec_calls, (unsigned)rc, (unsigned)fb.accepted,
             (unsigned)out.valid, (unsigned)out.error,
             out.width, out.height, out.pitch_bytes ? out.pitch_bytes : out.pitch);
    }
    if (rc != 0 || out.error) {
        if (!n->first_err_logged) {
            n->first_err_logged = 1;
            note("EVO vdec native: Decode FAIL #%u rc=0x%08x err=%u acc=%u au=%dB "
                 "valid=%u %ux%u", n->dec_calls, (unsigned)rc, (unsigned)out.error,
                 (unsigned)fb.accepted, size, (unsigned)out.valid,
                 out.width, out.height);
        }
        return -1;
    }

    pts_push(n, pts);
    if (out.valid && !out.error && out.picture_count) {
        n->frames_out++;
        ro_harvest(n, &out);
    }
    if (n->dec_calls % 300u == 0)
        note("EVO vdec native: heartbeat  decodes=%u framesout=%u ro=%d",
             n->dec_calls, n->frames_out, n->ro_count);
    return 0;
}

static void drain_decoder(evo_vdec_native *n)
{
    for (int guard = 0; guard < 64; guard++) {
        unsigned slot = n->au_ring % FRAME_POOL_SLOTS;
        n->au_ring++;

        SceVideodec2FrameBuffer fb;
        SceVideodec2OutputInfo  out;
        memset(&fb, 0, sizeof fb);
        memset(&out, 0, sizeof out);
        fb.size        = sizeof fb;
        fb.buffer      = n->frame_mem + (size_t)slot * n->frame_size;
        fb.buffer_size = n->frame_size;
        out.size       = sizeof out;

        if (sceVideodec2Flush(n->dec, &fb, &out) != 0)
            break;
        if (!(out.valid && !out.error && out.picture_count))
            break;
        ro_harvest(n, &out);
        if (n->ro_count >= RO_SLOTS - 1)
            break;
    }
}

/* (Re)create the Annex-B bitstream filter from the stored params. Called at
 * open and again on every flush/seek — a plain av_bsf_flush() does NOT make
 * h264_mp4toannexb re-emit SPS/PPS, so a post-seek IDR would reach the decoder
 * with no parameter sets and it would never restart. A fresh bsf re-injects
 * them on its first packet. */
static int bsf_build(evo_vdec_native *n)
{
    const AVBitStreamFilter *bf = av_bsf_get_by_name(n->bsf_name);
    if (!bf)
        return -1;
    if (n->bsf)
        av_bsf_free(&n->bsf);
    if (av_bsf_alloc(bf, &n->bsf) < 0) {
        n->bsf = NULL;
        return -1;
    }
    if (avcodec_parameters_copy(n->bsf->par_in, n->bsf_par) < 0) {
        av_bsf_free(&n->bsf);
        return -1;
    }
    n->bsf->time_base_in = (AVRational){ 1, 1000000 };
    if (av_bsf_init(n->bsf) < 0) {
        av_bsf_free(&n->bsf);
        return -1;
    }
    return 0;
}

evo_vdec_native *evo_vdec_native_open(const evo_vdec_open_params *p)
{
    if (!evo_vdec_native_probe())
        return NULL;
    if (!p || p->backend != EVO_VDEC_BACKEND_NATIVE || !p->avctx_params)
        return NULL;

    const AVCodecParameters *par = (const AVCodecParameters *)p->avctx_params;

    /* Resident decoder is AVC High only for now. */
    if (par->codec_id != AV_CODEC_ID_H264)
        return NULL;
    if ((par->profile > 100 && par->profile != 578) || par->bits_per_raw_sample > 8) {
        note("EVO vdec native: profile %d unsupported -> FFmpeg", par->profile);
        return NULL;
    }

    int w = roundup16(par->width  > 0 ? par->width  : p->width);
    int h = roundup16(par->height > 0 ? par->height : p->height);
    if (w < 16 || h < 16 ||
        (uint32_t)w > g_boot.max_w || (uint32_t)h > g_boot.max_h) {
        note("EVO vdec native: %dx%d exceeds resident %ux%u -> FFmpeg",
             w, h, g_boot.max_w, g_boot.max_h);
        return NULL;
    }

    evo_vdec_native *n = (evo_vdec_native *)calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->dec        = g_boot.decoder;
    n->input_mem  = (uint8_t *)g_boot.input_mem;
    n->frame_mem  = (uint8_t *)g_boot.frame_mem;
    n->frame_size = g_boot.frame_size;
    n->disp_w     = par->width  > 0 ? (uint32_t)par->width  : 0;
    n->disp_h     = par->height > 0 ? (uint32_t)par->height : 0;
    /* #27: when the GPU present path is up, emit NV12 straight from the decoder
     * — pp_agc's shader samples NV12 and does the YUV->RGB + scale on-GPU, so
     * the CPU never touches the pixels. Fixed for the stream's lifetime. */
    n->agc_out    = pp_agc_available();

    if (par->extradata && par->extradata_size >= 4 && par->extradata[0] == 1) {
        n->bsf_name = "h264_mp4toannexb";
        n->bsf_par  = avcodec_parameters_alloc();
        if (!n->bsf_par || avcodec_parameters_copy(n->bsf_par, par) < 0 ||
            bsf_build(n) != 0) {
            note("EVO vdec native: bsf setup failed (ex=%d) -> FFmpeg",
                 par->extradata_size);
            evo_vdec_native_close(n);
            return NULL;
        }
    }
    n->in_pkt   = av_packet_alloc();
    n->filt_pkt = av_packet_alloc();
    if (!n->in_pkt || !n->filt_pkt) {
        evo_vdec_native_close(n);
        return NULL;
    }

    sceVideodec2Reset(n->dec);   /* fresh state for this stream */

    note("EVO vdec native: OPEN ok  resident AVC decoder  %dx%d (disp %ux%u) depth=%d",
         w, h, n->disp_w, n->disp_h, EVO_VDEC_REORDER_DEPTH);
    return n;
}

int evo_vdec_native_send(evo_vdec_native *v, const uint8_t *data, int size,
                         int64_t pts_us)
{
    if (!v || v->fatal)
        return -1;

    if (!data || size <= 0) {          /* end-of-stream: drain everything */
        if (v->bsf) {
            (void)av_bsf_send_packet(v->bsf, NULL);
            while (av_bsf_receive_packet(v->bsf, v->filt_pkt) == 0) {
                int64_t fp = v->filt_pkt->pts == AV_NOPTS_VALUE
                                 ? INT64_MIN : v->filt_pkt->pts;
                int dr = decode_one(v, v->filt_pkt->data, v->filt_pkt->size, fp);
                av_packet_unref(v->filt_pkt);
                if (dr < 0) break;
            }
        }
        drain_decoder(v);
        v->flushing = 1;
        return 0;
    }

    if (v->ro_count > EVO_VDEC_REORDER_DEPTH)
        return 1;

    if (v->bsf) {
        av_packet_unref(v->in_pkt);
        if (av_new_packet(v->in_pkt, size) < 0)
            return -1;
        memcpy(v->in_pkt->data, data, (size_t)size);
        v->in_pkt->pts = (pts_us == INT64_MIN) ? AV_NOPTS_VALUE : pts_us;
        v->in_pkt->dts = AV_NOPTS_VALUE;
        if (av_bsf_send_packet(v->bsf, v->in_pkt) < 0)
            return -1;
        while (av_bsf_receive_packet(v->bsf, v->filt_pkt) == 0) {
            int64_t fp = v->filt_pkt->pts == AV_NOPTS_VALUE
                             ? INT64_MIN : v->filt_pkt->pts;
            int dr = decode_one(v, v->filt_pkt->data, v->filt_pkt->size, fp);
            av_packet_unref(v->filt_pkt);
            if (dr < 0) { v->fatal = 1; return -1; }
        }
        return 0;
    }

    if (decode_one(v, data, size, pts_us) < 0) {
        v->fatal = 1;
        return -1;
    }
    return 0;
}

int evo_vdec_native_receive(evo_vdec_native *v, pp_frame *out)
{
    if (!v || !out || v->fatal)
        return -1;

    int threshold = v->flushing ? 0 : EVO_VDEC_REORDER_DEPTH;
    if (v->ro_count <= threshold)
        return 0;

    struct nat_slot *best = NULL;
    for (int i = 0; i < RO_SLOTS; i++) {
        if (!v->ro[i].used)
            continue;
        if (!best) { best = &v->ro[i]; continue; }
        if (best->pts == INT64_MIN ||
            (v->ro[i].pts != INT64_MIN && v->ro[i].pts < best->pts))
            best = &v->ro[i];
    }
    if (!best)
        return 0;

    memset(out, 0, sizeof(*out));
    out->format       = best->nv12 ? PP_FRAME_NV12 : PP_FRAME_YUV420P;
    out->width        = best->w;
    out->height       = best->h;
    out->coded_height = best->coded_h;
    out->planes[0]    = best->data;
    out->planes[1]    = best->data + best->u_off;
    out->planes[2]    = best->data + best->v_off;
    out->strides[0]   = (int)best->y_stride;
    out->strides[1]   = (int)best->c_stride;
    out->strides[2]   = (int)best->c_stride;
    out->pts_us       = best->pts;

    best->used = 0;
    v->ro_count--;
    return 1;
}

void evo_vdec_native_flush(evo_vdec_native *v)   /* seek */
{
    if (!v)
        return;
    ro_reset(v);
    v->pts_n          = 0;
    v->au_ring        = 0;
    v->flushing       = 0;
    v->fatal          = 0;
    v->first_err_logged = 0;
    /*
     * #57: a plain av_bsf_flush() drops h264_mp4toannexb's buffered state but
     * does NOT re-arm its one-shot SPS/PPS injection, so the first IDR after a
     * seek reaches sceVideodec2 with no in-band parameter sets and it faults
     * (0x811d0303) on every AU until playback gives up. Streams that repeat
     * SPS/PPS in-band per keyframe (e.g. the GTA trailer) happen to survive;
     * ones that keep them in avcC extradata only (mkv, most .mov) do not.
     * Rebuild the bsf instead — a fresh filter re-injects the parameter sets
     * on its first output packet. On rebuild failure, fault so the playback
     * layer falls back cleanly rather than feeding raw avcC to the decoder.
     */
    if (v->bsf_name && v->bsf_par) {
        if (bsf_build(v) != 0) {
            v->fatal = 1;
            note("EVO vdec native: FLUSH (seek) bsf rebuild FAILED -> fatal");
        }
    } else if (v->bsf) {
        av_bsf_flush(v->bsf);
    }
    if (v->dec)
        sceVideodec2Reset(v->dec);
    note("EVO vdec native: FLUSH (seek)  decodes=%u framesout=%u",
         v->dec_calls, v->frames_out);
}

void evo_vdec_native_close(evo_vdec_native *v)
{
    if (!v)
        return;
    note("EVO vdec native: CLOSE  decodes=%u framesout=%u fatal=%d",
         v->dec_calls, v->frames_out, v->fatal);
    if (v->dec)
        sceVideodec2Reset(v->dec);   /* leave the resident decoder alive */
    if (v->bsf)
        av_bsf_free(&v->bsf);
    if (v->bsf_par)
        avcodec_parameters_free(&v->bsf_par);
    if (v->in_pkt)
        av_packet_free(&v->in_pkt);
    if (v->filt_pkt)
        av_packet_free(&v->filt_pkt);
    for (int i = 0; i < RO_SLOTS; i++)
        if (!v->ro[i].borrowed)
            free(v->ro[i].data);
    free(v);
}

#else /* !EVO_APP_MODULE — host + payload: native decode is unavailable */

int evo_vdec_native_probe(void) { return 0; }
evo_vdec_native *evo_vdec_native_open(const evo_vdec_open_params *p) { (void)p; return 0; }
int evo_vdec_native_send(evo_vdec_native *v, const uint8_t *d, int s, int64_t p)
{ (void)v; (void)d; (void)s; (void)p; return -1; }
int evo_vdec_native_receive(evo_vdec_native *v, pp_frame *o) { (void)v; (void)o; return -1; }
void evo_vdec_native_flush(evo_vdec_native *v) { (void)v; }
void evo_vdec_native_close(evo_vdec_native *v) { (void)v; }

#endif /* EVO_APP_MODULE */
