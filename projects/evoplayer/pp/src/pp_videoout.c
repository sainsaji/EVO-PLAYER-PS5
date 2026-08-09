/*
 * pp_videoout — direct VideoOut presentation (Probe 002 proven path).
 * Decoder / convert live outside this file.
 */
#include "pp_videoout.h"
#include "pp_platform.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

enum {
    PP_BUF_FREE = 0,
    PP_BUF_ACQUIRED = 1,
    PP_BUF_IN_FLIGHT = 2
};

#define PP_VO_MIN_FLIP_US 32000u
/* EVO: watchdog for retire_old_inflight - only fires if flip status stops
 * advancing (dropped flip, suspended app). ~4 frames at 60 Hz. */
#define PP_VO_RETIRE_WATCHDOG_US 66000u

/* Last init failure detail for probes (not thread-safe; diagnostic only). */
int pp_videoout_last_step;
int pp_videoout_last_rc;

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/*
 * Plane size: known-good is 0x1000000 per buffer at 1080p (total 0x2000000 for 2).
 * Scale roughly with pixel count but never below that half-block.
 */
static size_t calc_plane_bytes(uint32_t w, uint32_t height)
{
    size_t pixels = (size_t)w * (size_t)height;
    size_t ref = (size_t)1920u * 1080u;
    size_t need = 0x1000000u; /* proven half of PP_MEM_1080P_DOUBLE */
    if (pixels > ref) {
        /* 4K path: grow proportionally */
        need = (size_t)((double)need * ((double)pixels / (double)ref));
        need = align_up(need, (size_t)PP_ALIGN_128K);
    }
    return need;
}

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/*
 * EVO: retire buffers on ACTUAL flip completion, not on elapsed time.
 *
 * Upstream freed a buffer once PP_VO_MIN_FLIP_US (32 ms) had passed since
 * submit, regardless of whether the flip had retired. When the renderer kept
 * up, the next frame was written into a buffer that was still being scanned
 * out - visible as tearing during camera motion, and invisible on static
 * scenes because consecutive frames are identical there.
 *
 * sceVideoOutGetFlipStatus reports flipArg: the argument passed to the last
 * COMPLETED flip. We submit frame_id as that argument and record it in
 * submit_frame[], so a buffer is genuinely free once flipArg has reached its
 * frame_id.
 *
 * The old time-based rule is kept only as a watchdog, with a much longer
 * threshold. If flip status ever stops advancing - a dropped flip, a
 * suspended app - the pipeline recovers instead of deadlocking. It should
 * essentially never fire; stats.retire_timeouts counts it if it does.
 */
/* Set to 0 to fall back to upstream's time-based retirement (for bisecting). */
#ifndef EVO_FLIP_SYNC
#define EVO_FLIP_SYNC 1
#endif

static void retire_old_inflight(pp_videoout *vo)
{
    /* Over-sized backing store, deliberately.
     *
     * The exact size of the flip-status struct is not published anywhere we
     * can verify. If the real one is larger than PP_VideoOutFlipStatus, the
     * system would write past a bare struct and smash the stack. Giving it a
     * 256-byte landing area costs nothing and removes that whole class of
     * failure; we only read the leading fields we have declared. */
    union {
        PP_VideoOutFlipStatus s;
        unsigned char         pad[256];
    } u;
    PP_VideoOutFlipStatus st;
    uint32_t i;
    uint64_t t = now_us();
    int have_status = 0;

    memset(&u, 0, sizeof(u));
#if EVO_FLIP_SYNC
    have_status = (sceVideoOutGetFlipStatus(vo->handle, &u.s) == 0);
#endif
    st = u.s;

    for (i = 0; i < vo->buffer_count; i++) {
        if (vo->state[i] != PP_BUF_IN_FLIGHT)
            continue;

        if (have_status && st.flipArg >= vo->submit_frame[i]) {
            vo->state[i] = PP_BUF_FREE;
            continue;
        }

        /* Watchdog only - see comment above. */
        if (t - vo->submit_tsc[i] >= PP_VO_RETIRE_WATCHDOG_US) {
            vo->state[i] = PP_BUF_FREE;
            vo->stats.retire_timeouts++;
        }
    }
}

static void fail(int step, int rc)
{
    pp_videoout_last_step = step;
    pp_videoout_last_rc = rc;
}

int pp_videoout_init(pp_videoout *vo,
                     uint32_t width,
                     uint32_t height,
                     pp_pixel_format format,
                     uint32_t buffer_count)
{
    int rc;
    uint32_t i;
    uint64_t attr_pix;
    PP_VideoBuf vbuf[PP_VO_MAX_BUFFERS];
    PP_VideoAttr attr;

    pp_videoout_last_step = 0;
    pp_videoout_last_rc = 0;

    if (!vo) {
        fail(1, -1);
        return -1;
    }
    memset(vo, 0, sizeof(*vo));
    vo->handle = -1;

    if (width < 64 || height < 64) {
        fail(2, (int)width);
        return -2;
    }
    if (buffer_count < 2 || buffer_count > PP_VO_MAX_BUFFERS) {
        fail(3, (int)buffer_count);
        return -3;
    }
    if (format != PP_PIXEL_BGRA32_TILED && format != PP_PIXEL_RGBA32_TILED) {
        fail(4, (int)format);
        return -4;
    }

    vo->width = width;
    vo->height = height;
    vo->format = format;
    vo->buffer_count = buffer_count;
    vo->pitch = width * 4u;
    vo->flip_rate = 0;
    vo->plane_bytes = calc_plane_bytes(width, height);
    vo->memsize = vo->plane_bytes * (size_t)buffer_count;
    vo->cpu_bytes = (size_t)width * (size_t)height * 4u;

    vo->handle = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (vo->handle < 0) {
        fail(10, vo->handle);
        return -10;
    }

    rc = sceKernelAllocateMainDirectMemory(vo->memsize, PP_ALIGN_128K, 3, &vo->paddr);
    if (rc != 0) {
        fail(11, rc);
        pp_videoout_shutdown(vo);
        return -11;
    }

    rc = sceKernelMapDirectMemory(&vo->vaddr, vo->memsize, 0x33, 0, vo->paddr, PP_ALIGN_128K);
    if (rc != 0 || !vo->vaddr) {
        fail(12, rc);
        pp_videoout_shutdown(vo);
        return -12;
    }

    memset(vbuf, 0, sizeof(vbuf));
    for (i = 0; i < buffer_count; i++) {
        vo->gpu_bufs[i] = (void *)((uintptr_t)vo->vaddr + vo->plane_bytes * (size_t)i);
        vbuf[i].data = vo->gpu_bufs[i];
        vo->cpu_bufs[i] = (uint32_t *)malloc(vo->cpu_bytes);
        if (!vo->cpu_bufs[i]) {
            fail(15, (int)i);
            pp_videoout_shutdown(vo);
            return -15;
        }
        memset(vo->cpu_bufs[i], 0, vo->cpu_bytes);
        vo->state[i] = PP_BUF_FREE;
    }

    /* Equeue is best-effort — Probe 002 continues if flip event setup is soft-fail. */
    rc = sceKernelCreateEqueue(&vo->equeue, "pp_videoout_flip");
    if (rc == 0 && vo->equeue) {
        (void)sceVideoOutAddFlipEvent(vo->equeue, vo->handle, NULL);
    } else {
        vo->equeue = NULL;
        /* non-fatal */
    }
    (void)sceVideoOutSetFlipRate(vo->handle, vo->flip_rate);

    attr_pix = PP_VO_ATTR_TILED_BGRA;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute2(&attr, attr_pix, 0, width, height, 0, 0, 0);
    rc = sceVideoOutRegisterBuffers2(vo->handle, 0, 0, vbuf, (int)buffer_count, &attr, 0, NULL);
    if (rc != 0) {
        fail(14, rc);
        pp_videoout_shutdown(vo);
        return -14;
    }

    vo->registered = 1;
    vo->inited = 1;
    vo->next_index = 0;
    fail(0, 0);
    return 0;
}

void *pp_videoout_acquire(pp_videoout *vo, uint32_t *buffer_index, uint32_t *pitch)
{
    uint32_t i, tries;

    if (!vo || !vo->inited || !buffer_index || !pitch)
        return NULL;

    for (tries = 0; tries < 120; tries++) {
        retire_old_inflight(vo);
        for (i = 0; i < vo->buffer_count; i++) {
            uint32_t idx = (vo->next_index + i) % vo->buffer_count;
            if (vo->state[idx] == PP_BUF_FREE) {
                vo->state[idx] = PP_BUF_ACQUIRED;
                vo->next_index = (idx + 1) % vo->buffer_count;
                vo->stats.acquires++;
                *buffer_index = idx;
                *pitch = vo->pitch;
                return vo->cpu_bufs[idx];
            }
        }
        vo->stats.reuse_blocked++;
        for (i = 0; i < vo->buffer_count; i++) {
            if (vo->state[i] == PP_BUF_IN_FLIGHT) {
                (void)pp_videoout_wait_available(vo, i, 16);
                break;
            }
        }
    }
    return NULL;
}

void pp_videoout_release(pp_videoout *vo, uint32_t buffer_index)
{
    if (!vo || !vo->inited)
        return;
    if (buffer_index >= vo->buffer_count)
        return;
    if (vo->state[buffer_index] == PP_BUF_ACQUIRED)
        vo->state[buffer_index] = PP_BUF_FREE;
}

int pp_videoout_present(pp_videoout *vo, uint32_t buffer_index, uint64_t frame_id)
{
    int rc;

    if (!vo || !vo->inited)
        return -1;
    if (buffer_index >= vo->buffer_count)
        return -2;
    if (vo->state[buffer_index] != PP_BUF_ACQUIRED)
        return -3;

    pp_draw_pixels_as_tiles(vo->cpu_bufs[buffer_index],
                            (uint32_t *)vo->gpu_bufs[buffer_index],
                            (int)vo->width,
                            (int)vo->height);

    vo->stats.presents++;
    rc = sceVideoOutSubmitFlip(vo->handle, (int)buffer_index, 1, (int64_t)frame_id);
    if (rc == 0) {
        vo->stats.flips_ok++;
        vo->state[buffer_index] = PP_BUF_IN_FLIGHT;
        vo->submit_tsc[buffer_index] = now_us();
        vo->submit_frame[buffer_index] = frame_id;
    } else {
        vo->stats.flips_fail++;
        vo->state[buffer_index] = PP_BUF_FREE;
    }
    return rc;
}

void *pp_videoout_gpu_plane(pp_videoout *vo, uint32_t buffer_index)
{
    if (!vo || !vo->inited)
        return NULL;
    if (buffer_index >= vo->buffer_count)
        return NULL;
    return vo->gpu_bufs[buffer_index];
}

int pp_videoout_present_pre_tiled(pp_videoout *vo, uint32_t buffer_index, uint64_t frame_id)
{
    int rc;

    if (!vo || !vo->inited)
        return -1;
    if (buffer_index >= vo->buffer_count)
        return -2;
    if (vo->state[buffer_index] != PP_BUF_ACQUIRED)
        return -3;

    vo->stats.presents++;
    rc = sceVideoOutSubmitFlip(vo->handle, (int)buffer_index, 1, (int64_t)frame_id);
    if (rc == 0) {
        vo->stats.flips_ok++;
        vo->state[buffer_index] = PP_BUF_IN_FLIGHT;
        vo->submit_tsc[buffer_index] = now_us();
        vo->submit_frame[buffer_index] = frame_id;
    } else {
        vo->stats.flips_fail++;
        vo->state[buffer_index] = PP_BUF_FREE;
    }
    return rc;
}

int pp_videoout_wait_available(pp_videoout *vo, uint32_t buffer_index, uint32_t timeout_ms)
{
    uint64_t start, deadline, now;

    if (!vo || !vo->inited)
        return -1;
    if (buffer_index >= vo->buffer_count)
        return -2;

    vo->stats.waits++;
    if (vo->state[buffer_index] == PP_BUF_FREE ||
        vo->state[buffer_index] == PP_BUF_ACQUIRED)
        return 0;

    start = now_us();
    deadline = start + (uint64_t)timeout_ms * 1000ull;
    if (timeout_ms == 0)
        deadline = start;

    for (;;) {
        retire_old_inflight(vo);
        if (vo->state[buffer_index] == PP_BUF_FREE)
            return 0;
        now = now_us();
        if (timeout_ms != UINT32_MAX && now >= deadline) {
            vo->stats.wait_timeouts++;
            return -1;
        }
        usleep(2000);
    }
}

void pp_videoout_shutdown(pp_videoout *vo)
{
    uint32_t i;
    if (!vo)
        return;

    /* Let in-flight flips drain briefly. */
    for (i = 0; i < vo->buffer_count && i < PP_VO_MAX_BUFFERS; i++) {
        if (vo->state[i] == PP_BUF_IN_FLIGHT)
            usleep(PP_VO_MIN_FLIP_US);
        vo->state[i] = PP_BUF_FREE;
    }

    if (vo->registered && vo->handle >= 0) {
        (void)sceVideoOutUnregisterBuffers(vo->handle, 0);
        vo->registered = 0;
    }

    if (vo->handle >= 0) {
        (void)sceVideoOutClose(vo->handle);
        vo->handle = -1;
    }

    if (vo->equeue) {
        (void)sceKernelDeleteEqueue(vo->equeue);
        vo->equeue = NULL;
    }

    if (vo->vaddr && vo->memsize) {
        (void)sceKernelMunmap(vo->vaddr, vo->memsize);
        vo->vaddr = NULL;
    }
    if (vo->paddr && vo->memsize) {
        (void)sceKernelReleaseDirectMemory(vo->paddr, vo->memsize);
        vo->paddr = 0;
    }
    vo->memsize = 0;
    vo->plane_bytes = 0;

    for (i = 0; i < PP_VO_MAX_BUFFERS; i++) {
        if (vo->cpu_bufs[i]) {
            free(vo->cpu_bufs[i]);
            vo->cpu_bufs[i] = NULL;
        }
        vo->gpu_bufs[i] = NULL;
    }

    vo->inited = 0;
    vo->registered = 0;
    vo->buffer_count = 0;
}

void pp_videoout_get_stats(const pp_videoout *vo, pp_videoout_stats *out)
{
    if (!vo || !out)
        return;
    *out = vo->stats;
}

int pp_videoout_reconfigure(pp_videoout *vo,
                            uint32_t width,
                            uint32_t height,
                            pp_pixel_format format,
                            uint32_t buffer_count)
{
    pp_videoout_shutdown(vo);
    return pp_videoout_init(vo, width, height, format, buffer_count);
}
