#include "pp_playback.h"
#include "pp_agc.h"
#include "pp_compute_pipeline.h"
#include "pp_converter_fused.h"
#include "pp_converter_parallel.h"
#include "pp_output_policy.h"

#include "pp_4k_sdr_policy.h"
#include "pp_product_path.h"
#include "pp_v8_gate.h"
#include "pp_stage_breadcrumb.h"
#include "evo_boot_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static uint64_t now_ms(void)
{
    return now_us() / 1000ull;
}

static pthread_mutex_t *mtx(pp_playback *pb)
{
    return (pthread_mutex_t *)pb->lock;
}

void pp_playback_init(pp_playback *pb)
{
    pthread_mutex_t *m;
    if (!pb)
        return;
    memset(pb, 0, sizeof(*pb));
    m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m) {
        pthread_mutex_init(m, NULL);
        pb->lock = m;
    }
    /*
     * Soft UHD: very wide late window (never freeze on late-drop).
     * Early sleep capped short so convert+decode can stay realtime.
     */
    pp_clock_init(&pb->clock, 800000, 25000);
    pb->cfg.aspect = PP_ASPECT_FIT;
    pb->cfg.clear_color_bgra = 0xFF000000u;
    pb->out_w = 1920;
    pb->out_h = 1080;
    pb->display = NULL;
    pb->display_back = NULL;
    pb->backend = PP_BACKEND_1080_STANDARD;
    pb->force_v3_fallback = 0;
    pb->pending_present = 0;
    pb->pending_vo_idx = 0;
    pb->pending_frame_id = 0;
    pb->present_seq = 0;
    pb->stats.output_w = 1920;
    pb->stats.output_h = 1080;
    pb->stats.aspect = (int)PP_ASPECT_FIT;
    pb->stats.backend = (int)PP_BACKEND_1080_STANDARD;
}

void pp_playback_set_backend(pp_playback *pb, pp_video_backend backend)
{
    if (!pb)
        return;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->backend = backend;
    pb->stats.backend = (int)backend;
    pb->pending_present = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_force_v3_fallback(pp_playback *pb, int enable)
{
    if (!pb)
        return;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->force_v3_fallback = enable ? 1 : 0;
    if (enable && pb->backend == PP_BACKEND_4K_V8_FUSED)
        pb->backend = PP_BACKEND_4K_V3_FALLBACK;
    pb->stats.backend = (int)pb->backend;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_shutdown(pp_playback *pb)
{
    if (!pb)
        return;
    pp_playback_on_file_close(pb);
    free(pb->display);
    pb->display = NULL;
    free(pb->display_back);
    pb->display_back = NULL;
    pb->display_cap = 0;
    free(pb->nv12_fb);
    pb->nv12_fb = NULL;
    pb->nv12_fb_cap = 0;
    if (pb->lock) {
        pthread_mutex_destroy(mtx(pb));
        free(pb->lock);
        pb->lock = NULL;
    }
    memset(pb, 0, sizeof(*pb));
}

void pp_playback_attach_videoout(pp_playback *pb, pp_videoout *vo)
{
    if (!pb)
        return;
    pb->vo = vo;
}

int pp_playback_set_output(pp_playback *pb, uint32_t w, uint32_t h,
                           pp_aspect_mode aspect)
{
    uint32_t *front = NULL, *back = NULL;
    size_t bytes;
    if (!pb || w < 64 || h < 64)
        return -1;

    /*
     * The V8 fused 4K path writes straight to the VideoOut GPU plane and never
     * touches pb->display / pb->display_back — allocating 2×(w*h*4) for them
     * (66 MB at 3840x2160) is pure waste, and if that alloc fails (tight flex
     * budget once the native decoder is resident) out_w silently stays at the
     * old size and use_v8 turns off -> black screen. Skip it for V8; the
     * V3/1080 path allocates on demand in the fallback branch of push_frame.
     */
    bytes = (size_t)w * (size_t)h * 4u;
    if (pb->backend != PP_BACKEND_4K_V8_FUSED) {
        front = (uint32_t *)malloc(bytes);
        back = (uint32_t *)malloc(bytes);
        if (!front || !back) {
            free(front);
            free(back);
            return -2;
        }
    }
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (front) {
        free(pb->display);
        free(pb->display_back);
        pb->display = front;
        pb->display_back = back;
        pb->display_cap = bytes;
    } else if (pb->display && pb->display_cap < bytes) {
        /*
         * #55: V8 path skips the display alloc, but if the backend later drops
         * to V3 at this (larger) resolution, converting into a stale
         * smaller-resolution pb->display overflows the heap. Drop the stale
         * buffers now; the V3 branch of push_frame reallocs to the right size.
         */
        free(pb->display);
        free(pb->display_back);
        pb->display = NULL;
        pb->display_back = NULL;
        pb->display_cap = 0;
    }
    pb->out_w = w;
    pb->out_h = h;
    pb->cfg.aspect = aspect;
    pb->display_ready = 0;
    pb->stats.output_w = (int)w;
    pb->stats.output_h = (int)h;
    pb->stats.aspect = (int)aspect;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 0;
}

void pp_playback_on_file_open(pp_playback *pb)
{
    if (!pb)
        return;
    if (!pb->display && pb->backend != PP_BACKEND_4K_V8_FUSED)
        (void)pp_playback_set_output(pb, pb->out_w ? pb->out_w : 1920,
                                     pb->out_h ? pb->out_h : 1080, pb->cfg.aspect);
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    pb->active = 1;
    pb->seek_discarding = 0;
    pb->display_ready = 0;
    pb->display_pts_us = 0;
    if (pb->pending_present && pb->vo)
        pp_videoout_release(pb->vo, pb->pending_vo_idx);
    pb->pending_present = 0;
    pb->stats.frames_in = 0;
    pb->stats.frames_converted = 0;
    pb->stats.frames_published = 0;
    pb->stats.frames_late_dropped = 0;
    pb->stats.frames_discarded_seek = 0;
    pb->stats.convert_us_total = 0;
    pb->stats.convert_us_max = 0;
    pb->stats.convert_ring_count = 0;
    pb->stats.sample_bgra = 0;
}

void pp_playback_on_file_close(pp_playback *pb)
{
    if (!pb)
        return;
    pb->active = 0;
    pb->seek_discarding = 0;
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (pb->pending_present && pb->vo)
        pp_videoout_release(pb->vo, pb->pending_vo_idx);
    pb->pending_present = 0;
    pb->display_ready = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_pause(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_pause(&pb->clock);
}

void pp_playback_resume(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_resume(&pb->clock);
}

static void note_convert(pp_playback *pb, uint64_t us)
{
    uint32_t i;
    pb->stats.convert_us_total += us;
    if (us > pb->stats.convert_us_max)
        pb->stats.convert_us_max = us;
    /* Rolling ring of last 256 convert times for p95. */
    if (pb->stats.convert_ring_count < 256u) {
        i = pb->stats.convert_ring_count;
        pb->stats.convert_ring[i] = us;
        pb->stats.convert_ring_count++;
    } else {
        i = (uint32_t)(pb->stats.frames_converted % 256u);
        pb->stats.convert_ring[i] = us;
    }
}

/*
 * Late frames in a row before the presentation clock is assumed wrong and
 * re-based. Comfortably more than a genuine catch-up needs (a few frames),
 * comfortably less than a second at any frame rate.
 */
#define PP_LATE_DROP_RESYNC 20

/*
 * The native decoder emits NV12 for the sceAgc GPU present path (#27). When
 * that path is not taken — host preview, or a first-frame AGC fault disabled
 * it — de-interleave NV12 -> planar YUV420P into pb->nv12_fb so the CPU
 * converters (YUV420P-only) can run. Mirrors evo_vdec_native.c's ro_harvest
 * non-AGC branch. Returns 0 and retargets *out, or <0 on OOM.
 */
static int nv12_to_yuv420p(pp_playback *pb, const pp_frame *s, pp_frame *out)
{
    uint32_t pitch   = (uint32_t)s->strides[0];
    uint32_t uvpitch = s->strides[1] > 0 ? (uint32_t)s->strides[1] : pitch;
    uint32_t coded_h = s->coded_height ? s->coded_height : s->height;
    uint32_t cpitch  = pitch / 2u;
    uint32_t chh     = (coded_h + 1u) / 2u;
    uint32_t chw     = (s->width + 1u) / 2u;
    size_t   y_sz    = (size_t)pitch * coded_h;
    size_t   c_sz    = (size_t)cpitch * chh;
    size_t   need    = y_sz + 2u * c_sz;
    uint8_t *du, *dv;

    if (!s->planes[0] || !s->planes[1] || pitch < 2u || (pitch & 1u))
        return -1;
    if (pb->nv12_fb_cap < need) {
        uint8_t *p = (uint8_t *)realloc(pb->nv12_fb, need);
        if (!p)
            return -1;
        pb->nv12_fb = p;
        pb->nv12_fb_cap = need;
    }
    memcpy(pb->nv12_fb, s->planes[0], y_sz);
    du = pb->nv12_fb + y_sz;
    dv = du + c_sz;
    for (uint32_t r = 0; r < chh; r++) {
        const uint8_t *row = s->planes[1] + (size_t)r * uvpitch;
        uint8_t *pu = du + (size_t)r * cpitch;
        uint8_t *pv = dv + (size_t)r * cpitch;
        for (uint32_t x = 0; x < chw; x++) {
            pu[x] = row[2u * x];
            pv[x] = row[2u * x + 1u];
        }
    }
    memset(out, 0, sizeof *out);
    out->format       = PP_FRAME_YUV420P;
    out->width        = s->width;
    out->height       = s->height;
    out->coded_height = coded_h;
    out->planes[0]    = pb->nv12_fb;
    out->planes[1]    = du;
    out->planes[2]    = dv;
    out->strides[0]   = (int)pitch;
    out->strides[1]   = (int)cpitch;
    out->strides[2]   = (int)cpitch;
    out->pts_us       = s->pts_us;
    return 0;
}

/* #27 test hook: force the CPU-into-linear-VO fallback path while pp_agc stays
 * "available" (so the VO still registers linear) - the only way to exercise it
 * on demand without an actual GPU fault. Toggled by the env var OR, on the
 * console where env is awkward, by FTP-dropping /mnt/usb0/evo_agc_no_present. */
static int agc_no_present(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("EVO_AGC_NO_PRESENT") ? 1 : 0;
#ifdef EVO_APP_MODULE
        if (!v) {
            FILE *f = fopen("/mnt/usb0/evo_agc_no_present", "r");
            if (f) { v = 1; fclose(f); }
        }
#endif
    }
    return v;
}

/* #27: steady-state GPU present timing, mirrors evo_vdec_native's decode
 * heartbeat. One line to evo_boot.log per window so evo-remote.sh boot shows
 * whether the convert+flip holds the frame budget. */
#define PP_AGC_HEARTBEAT_FRAMES 300u
static void agc_heartbeat_note(pp_playback *pb, uint64_t present_us, int dropped)
{
    if (dropped) {
        pb->agc_present_dropped++;
        return;
    }
    pb->agc_present_us_sum += present_us;
    if (present_us > pb->agc_present_us_max)
        pb->agc_present_us_max = present_us;
    if (++pb->agc_hb_frames < PP_AGC_HEARTBEAT_FRAMES)
        return;
    evo_boot_log("pp_agc: heartbeat presented=%llu dropped=%llu avg=%lluus max=%lluus",
                 (unsigned long long)pb->agc_frames,
                 (unsigned long long)pb->agc_present_dropped,
                 (unsigned long long)(pb->agc_present_us_sum / pb->agc_hb_frames),
                 (unsigned long long)pb->agc_present_us_max);
    evo_boot_log_flush();
    pb->agc_present_us_sum = 0;
    pb->agc_present_us_max = 0;
    pb->agc_hb_frames = 0;
}

int pp_playback_push_frame(pp_playback *pb, const pp_frame *src)
{
    uint64_t t0, t1, cus;
    int rc;
    size_t need;
    int use_v8;
    int agc_path;
    pp_frame nv12_local;

    if (!pb || !src || !pb->active)
        return -1;

    pb->stats.frames_in++;

    if (pb->seek_discarding) {
        if (src->pts_us < pb->seek_target_us) {
            pb->stats.frames_discarded_seek++;
            return 1;
        }
        pp_clock_reset(&pb->clock);
        pb->stats.clock_resets++;
        pp_clock_start(&pb->clock, src->pts_us);
        pb->seek_discarding = 0;
        if (pb->seek_begin_ms) {
            pb->stats.seek_to_first_frame_ms = now_ms() - pb->seek_begin_ms;
            pb->seek_begin_ms = 0;
        }
    }

    if (pp_clock_is_paused(&pb->clock))
        return 1;

    need = (size_t)pb->out_w * (size_t)pb->out_h;
    if (need == 0 || need > (size_t)3840u * 2160u)
        return -2;

    /*
     * V8 fused: 1:1 yuv420p → tiled BGRA at live VO size.
     * Supports full 3840x2160 and cinema UHD (e.g. 3840x1920).
     *
     * #27: pp_v8_frame_gate only clears YUV420P (the CPU converters' format).
     * An NV12 UHD frame from the native decoder is also a valid V8 source — the
     * AGC GPU converter samples NV12 directly (agc_path below), and if AGC is
     * not taken the NV12→YUV420P de-interleave a few lines down feeds the CPU
     * converter. Without this an NV12 frame falls to the V3/1080 path, which
     * overflows at 4K (#55) — the crash seen on the first #27 hardware runs.
     */
    int v8_src_ok = (pp_v8_frame_gate(src, 30.0) == PP_V8_GATE_OK) ||
                    (src->format == PP_FRAME_NV12 &&
                     pp_v8_is_uhd_size(src->width, src->height));

    use_v8 = (pb->backend == PP_BACKEND_4K_V8_FUSED && !pb->force_v3_fallback &&
              pb->vo && pb->vo->inited &&
              pb->out_w == pb->vo->width && pb->out_h == pb->vo->height &&
              src->width == pb->out_w && src->height == pb->out_h &&
              v8_src_ok);

    /*
     * V8 is strict 1:1 tile (no FIT/FILL/STRETCH). If the user forced V3 via
     * view-mode apply (force_v3_fallback), use_v8 stays 0 and aspect convert runs.
     */

#if !PP_4K_V8_PRODUCT_ENABLE
    use_v8 = 0; /* crash isolation: product never uses V8 path */
#endif

    /*
     * #27: the native decoder emits NV12 when the sceAgc GPU present path is
     * up. That path is only taken on the V8 branch; anywhere else (host, a
     * disabled AGC, the 1080/V3 branch) the CPU converters need planar
     * YUV420P, so de-interleave here.
     */
    agc_path = !agc_no_present() &&
               use_v8 && pp_agc_available() && src->format == PP_FRAME_NV12 &&
               src->planes[0] && src->planes[1] && src->strides[0] > 0 &&
               /* AGC stages one contiguous Y+UV block — require it (the native
                * decoder guarantees this; a stray FFmpeg NV12 frame may not). */
               src->planes[1] == src->planes[0] +
                   (size_t)src->strides[0] *
                       (src->coded_height ? src->coded_height : src->height);
    if (src->format == PP_FRAME_NV12 && !agc_path) {
        if (nv12_to_yuv420p(pb, src, &nv12_local) != 0)
            return -4;
        src = &nv12_local;
    }

    /* ---- V8 fused: write tiled GPU plane; main present_pre_tiled ---- */
    if (use_v8) {
        uint32_t idx = 0, pitch = 0;
        uint32_t *lin;
        uint32_t *gpu;
        int v8_workers = 8;

        /* VO must match output — drop if mid-reconfig */
        if (!pb->vo->inited ||
            pb->vo->width != pb->out_w || pb->vo->height != pb->out_h)
            return 1;

        if (pp_clock_wait_or_drop(&pb->clock, src->pts_us) == PP_CLOCK_DROP) {
            pb->stats.frames_late_dropped++;
            if (agc_path)
                agc_heartbeat_note(pb, 0, 1);
            return 1;
        }

        lin = (uint32_t *)pp_videoout_acquire(pb->vo, &idx, &pitch);
        gpu = (uint32_t *)pp_videoout_gpu_plane(pb->vo, idx);
        if (!lin || !gpu)
            return -5;

        if (pb->stats.frames_converted == 0)
            pp_stage_bc_checkpoint("010_FIRST_FRAME_DECODED", "entering V8 convert");

        /*
         * #27: GPU convert + present via sceAgc. The DCB does NV12->RGB into
         * the VO plane AND queues its own flip, so on success there is nothing
         * for main.c to present_pre_tiled — mark the buffer in-flight and
         * publish. A failure (or a first-frame fault, which disables pp_agc)
         * releases the buffer; the next frame is de-interleaved to YUV420P by
         * the nv12_to_yuv420p normalise above and takes the CPU path.
         */
        if (agc_path) {
            int64_t marker;
            uint32_t coded_h = src->coded_height ? src->coded_height : src->height;

            if (pb->lock)
                pthread_mutex_lock(mtx(pb));
            marker = (int64_t)(++pb->present_seq);
            if (pb->lock)
                pthread_mutex_unlock(mtx(pb));

            t0 = now_us();
            rc = pp_agc_present_nv12(pb->vo->handle, idx, gpu,
                                     src->planes[0], (uint32_t)src->strides[0],
                                     coded_h, src->width, src->height,
                                     pb->out_w, pb->out_h, marker);
            t1 = now_us();

            if (rc == 0) {
                /* The DCB has queued the flip on `idx` regardless of what the
                 * bookkeeping call returns — never release this buffer now. */
                (void)pp_videoout_adopt_flip(pb->vo, idx, (uint64_t)marker);
                cus = t1 - t0;
                pb->stats.frames_converted++;
                pb->agc_frames++;
                note_convert(pb, cus);
                agc_heartbeat_note(pb, cus, 0);
                if (pb->agc_frames == 1)
                    pp_stage_bc_checkpoint("012_AGC_FIRST_PRESENT", "agc gpu flip");

                if (pb->lock)
                    pthread_mutex_lock(mtx(pb));
                /* Drop any earlier CPU-path frame that main never presented. */
                if (pb->pending_present) {
                    pp_videoout_release(pb->vo, pb->pending_vo_idx);
                    pb->pending_present = 0;
                }
                pb->display_ready = 1;
                pb->display_pts_us = src->pts_us;
                pb->stats.frames_published++;
                if (pb->lock)
                    pthread_mutex_unlock(mtx(pb));
                return 0;
            }

            /* #27 B: rc == -2 — the GPU submit blew the 250 ms watchdog. The
             * worker is abandoned but may still write `idx` and queue its flip,
             * so hand the buffer to the VO retire watchdog (adopt) rather than
             * releasing it into the free pool. pp_agc is permanently
             * unavailable for the rest of the session now. */
            if (rc == -2) {
                (void)pp_videoout_adopt_flip(pb->vo, idx, (uint64_t)marker);
                pp_stage_bc_checkpoint("011_AGC_SUBMIT_WEDGED", "cpu path from here");
                return -4;
            }

            /* rc == -1: AGC present failed cleanly — hand the buffer back and
             * drop this frame. pp_agc disables itself after a first-frame
             * fault, so subsequent frames fall through nv12_to_yuv420p + the
             * CPU converter. */
            pp_videoout_release(pb->vo, idx);
            pp_stage_bc_checkpoint("011_AGC_PRESENT_FAIL", "cpu fallback next");
            return -4;
        }

        /* More workers on wide UHD frames (was 4 — under-used CPU) */
        if (pb->out_w * pb->out_h >= 3840u * 1600u)
            v8_workers = 8;


        t0 = now_us();
        if (pp_videoout_is_linear(pb->vo)) {
            /*
             * #27: the VO is linear-registered for the sceAgc path but this
             * frame is on the CPU path (AGC faulted/wedged, EVO_AGC_NO_PRESENT,
             * or a non-AGC frame). The tiled converters would garble a linear
             * plane — emit linear BGRA 1:1 instead. present_pre_tiled below just
             * flips, which is correct for the linear plane.
             */
            rc = pp_converter_yuv420p_to_bgra_parallel(
                src, gpu, pb->out_w * 4u, v8_workers);
        } else {
            rc = pp_compute_pipeline_convert(
                src, gpu, pb->out_w, pb->out_h, v8_workers);
            if (rc != 0) {
                rc = pp_converter_yuv420p_to_tiled_bgra_parallel(
                    src, gpu, pb->out_w, pb->out_h, v8_workers);
            }
        }
        t1 = now_us();

        if (rc != 0) {
            pp_videoout_release(pb->vo, idx);
            pp_stage_bc_checkpoint("011_FIRST_FRAME_CONVERT_FAIL", "fused rc");
            return -4;
        }
        cus = t1 - t0;
        pb->stats.frames_converted++;
        note_convert(pb, cus);
        if (pb->stats.frames_converted == 1)
            pp_stage_bc_checkpoint("011_FIRST_FRAME_CONVERTED", "fused ok");

        if (pb->lock)
            pthread_mutex_lock(mtx(pb));
        /* Drop previous unpresented V8 frame if any */
        if (pb->pending_present)
            pp_videoout_release(pb->vo, pb->pending_vo_idx);
        pb->present_seq++;
        pb->pending_vo_idx = idx;
        pb->pending_frame_id = pb->present_seq;
        pb->pending_present = 1;
        pb->display_ready = 1;
        pb->display_pts_us = src->pts_us;
        pb->stats.frames_published++;
        if (pb->lock)
            pthread_mutex_unlock(mtx(pb));
        return 0;
    }

    /* ---- 1080 / V3 fallback: linear display then main tiles ---- */
    {
        /*
         * #55: size the display buffers to the ACTUAL output, independent of
         * pb->backend (which can still say V8 here — use_v8 also gates on the
         * VO match / force_v3 / clock). pp_playback_set_output skips the alloc
         * when backend==V8, so do it directly.
         */
        size_t need_disp = (size_t)pb->out_w * (size_t)pb->out_h * 4u;
        if (!pb->display || !pb->display_back || pb->display_cap < need_disp) {
            uint32_t *nf = (uint32_t *)malloc(need_disp);
            uint32_t *nb = (uint32_t *)malloc(need_disp);
            if (!nf || !nb) {
                free(nf);
                free(nb);
                return -3;
            }
            if (pb->lock)
                pthread_mutex_lock(mtx(pb));
            free(pb->display);
            free(pb->display_back);
            pb->display = nf;
            pb->display_back = nb;
            pb->display_cap = need_disp;
            pb->display_ready = 0;
            if (pb->lock)
                pthread_mutex_unlock(mtx(pb));
        }
    }

    {
        /*
         * Soft UHD: audio-master pacing lives in decode_next_video_frame.
         * Do not host-clock sleep or late-drop here (double-pace / freezes).
         * Non-soft paths still use the normal PTS clock.
         */
        int soft_uhd = (src->width >= 2560u && pb->out_w <= 1920u);
        if (soft_uhd) {
            if (!pb->clock.started)
                pp_clock_start(&pb->clock, src->pts_us);
        } else if (pp_clock_wait_or_drop(&pb->clock, src->pts_us) ==
                   PP_CLOCK_DROP) {
            pb->stats.frames_late_dropped++;

            /*
             * Dropping late frames is how the clock catches up, and normally
             * it does so within a handful of frames. A long unbroken run
             * means the clock is not behind the video - it is wrong: its
             * wall-time base advanced during a period when nothing was being
             * pushed, so every frame now looks late and the picture freezes
             * for good while audio carries on.
             *
             * Re-base on the frame in hand and let it through. Recovering one
             * frame late is invisible; not recovering is a dead picture.
             */
            if (++pb->late_drop_streak < PP_LATE_DROP_RESYNC)
                return 1;

            pp_clock_reset(&pb->clock);
            pp_clock_start(&pb->clock, src->pts_us);
            pb->stats.clock_resets++;
            pb->late_drop_streak = 0;
        } else {
            pb->late_drop_streak = 0;
        }
    }

    /* Convert into back buffer WITHOUT holding display lock (UI can blit). */
    t0 = now_us();
    rc = pp_converter_to_display(src, pb->display_back, pb->out_w, pb->out_h,
                                 pb->out_w * 4u, pb->cfg.aspect);
    t1 = now_us();
    if (rc != 0)
        return -4;
    cus = t1 - t0;
    pb->stats.frames_converted++;
    note_convert(pb, cus);

    /* Swap back → front under lock (pointer swap, no memcpy). */
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    {
        uint32_t *tmp = pb->display;
        pb->display = pb->display_back;
        pb->display_back = tmp;
    }

    /*
     * Only re-anchor on large host lag (not every soft-UHD frame).
     * Re-anchoring every frame made residual early-sleep tiny → free-run.
     */
    {
        int soft_uhd = (src->width >= 2560u && pb->out_w <= 1920u);
        if (!soft_uhd) {
            int64_t media = pp_clock_media_us(&pb->clock);
            int64_t lag = media - src->pts_us;
            if (lag > 120000 || lag < -120000)
                pp_clock_reanchor(&pb->clock, src->pts_us);
        }
    }

    pb->display_ready = 1;
    pb->display_pts_us = src->pts_us;
    pb->stats.frames_published++;
    if (pb->stats.sample_bgra == 0 && need > 0)
        pb->stats.sample_bgra = pb->display[need / 2u];
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 0;
}

int pp_playback_take_pending_present(pp_playback *pb, uint32_t *idx, uint64_t *frame_id)
{
    int ok = 0;
    if (!pb || !idx || !frame_id)
        return 0;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (pb->pending_present) {
        *idx = pb->pending_vo_idx;
        *frame_id = pb->pending_frame_id;
        pb->pending_present = 0;
        ok = 1;
    }
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return ok;
}

/**
 * Apply V3 product output policy for a newly opened source.
 * Does not reconfigure VideoOut — caller must pp_videoout_reconfigure + set_output.
 * Returns selected pp_output_mode.
 */
int pp_playback_choose_output_mode(const pp_source_caps *src,
                                   uint32_t display_max_w,
                                   uint32_t display_max_h)
{
    return (int)pp_select_output_mode(src, display_max_w, display_max_h);
}

/* Opaque black, the same value every clear path in the converters uses. */
#define PP_DISPLAY_CLEAR_BGRA 0xFF000000u

static void fill_rect_bgra(uint32_t *dst, uint32_t pitch_px, uint32_t x,
                           uint32_t y, uint32_t w, uint32_t h, uint32_t v)
{
    uint32_t r, c;

    for (r = 0; r < h; r++) {
        uint32_t *row = dst + (size_t)(y + r) * (size_t)pitch_px + x;
        for (c = 0; c < w; c++)
            row[c] = v;
    }
}

int pp_playback_copy_display(pp_playback *pb, uint32_t *dst, uint32_t pitch_bytes,
                             uint32_t dst_w, uint32_t dst_h)
{
    uint32_t copy_w = 0, copy_h = 0, p;
    int copied = 0;

    if (!dst || !dst_w || !dst_h || pitch_bytes < dst_w * 4u)
        return 0;

    p = pitch_bytes / 4u;

    if (pb && pb->display) {
        if (pb->lock)
            pthread_mutex_lock(mtx(pb));
        if (pb->display_ready) {
            copy_w = pb->out_w < dst_w ? pb->out_w : dst_w;
            copy_h = pb->out_h < dst_h ? pb->out_h : dst_h;

            /*
             * The full-width case is the one that runs every frame at 1080p.
             * Row-at-a-time there is 1080 calls to memcpy for what is one
             * contiguous 8 MB run in both buffers.
             */
            if (copy_w == p && copy_w == pb->out_w) {
                memcpy(dst, pb->display, (size_t)copy_w * copy_h * 4u);
            } else {
                uint32_t y;
                for (y = 0; y < copy_h; y++)
                    memcpy(dst + (size_t)y * p, pb->display + (size_t)y * pb->out_w,
                           copy_w * 4u);
            }
            copied = 1;
        }
        if (pb->lock)
            pthread_mutex_unlock(mtx(pb));
    }

    /*
     * Everything the frame did not cover. Deliberately outside the lock: this
     * is only reached before the first frame, during a seek, or when the
     * display is smaller than the target, and holding the display mutex across
     * a multi-megabyte fill would stall the decode thread's buffer swap.
     */
    if (copy_h < dst_h)
        fill_rect_bgra(dst, p, 0, copy_h, dst_w, dst_h - copy_h,
                       PP_DISPLAY_CLEAR_BGRA);
    if (copy_w < dst_w)
        fill_rect_bgra(dst, p, copy_w, 0, dst_w - copy_w, copy_h,
                       PP_DISPLAY_CLEAR_BGRA);

    return copied;
}

int pp_playback_has_display(const pp_playback *pb)
{
    return pb && pb->display_ready;
}

void pp_playback_notify_seek_begin(pp_playback *pb, int64_t target_pts_us)
{
    if (!pb)
        return;
    pb->stats.seek_requests++;
    pb->seek_discarding = 1;
    pb->seek_target_us = target_pts_us;
    pb->seek_begin_ms = now_ms();
    pp_clock_pause(&pb->clock);
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->display_ready = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_notify_seek_end(pp_playback *pb, int success,
                                 uint64_t discarded, uint64_t elapsed_ms)
{
    if (!pb)
        return;
    (void)discarded;
    (void)elapsed_ms;
    if (success)
        pb->stats.seek_successes++;
    else {
        pb->stats.seek_failures++;
        pb->seek_discarding = 0;
    }
    /* clock re-start happens on first post-seek push_frame */
}

void pp_playback_get_stats(const pp_playback *pb, pp_playback_stats *out)
{
    if (!pb || !out)
        return;
    *out = pb->stats;
}

uint64_t pp_playback_convert_p95_us(const pp_playback *pb)
{
    uint64_t tmp[256];
    uint32_t n, i, j, idx;
    if (!pb)
        return 0;
    n = pb->stats.convert_ring_count;
    if (n == 0)
        return 0;
    if (n > 256)
        n = 256;
    memcpy(tmp, pb->stats.convert_ring, n * sizeof(uint64_t));
    /* insertion sort small */
    for (i = 1; i < n; i++) {
        uint64_t v = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return tmp[idx];
}

void pp_playback_write_stats_file(const pp_playback *pb, const char *path)
{
    FILE *f;
    pp_clock_stats cs;
    uint64_t avg = 0;
    if (!pb || !path)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    pp_clock_get_stats(&pb->clock, &cs);
    if (pb->stats.frames_converted)
        avg = pb->stats.convert_us_total / pb->stats.frames_converted;
    fprintf(f, "pp_playback_stats\n");
    fprintf(f, "output=%ux%u aspect=%d\n", pb->out_w, pb->out_h, (int)pb->cfg.aspect);
    fprintf(f, "frames_in=%llu converted=%llu published=%llu late_drop=%llu seek_disc=%llu\n",
            (unsigned long long)pb->stats.frames_in,
            (unsigned long long)pb->stats.frames_converted,
            (unsigned long long)pb->stats.frames_published,
            (unsigned long long)pb->stats.frames_late_dropped,
            (unsigned long long)pb->stats.frames_discarded_seek);
    fprintf(f, "convert_us_avg=%llu convert_us_p95=%llu convert_us_max=%llu\n",
            (unsigned long long)avg,
            (unsigned long long)pp_playback_convert_p95_us(pb),
            (unsigned long long)pb->stats.convert_us_max);
    fprintf(f, "seek_requests=%llu seek_ok=%llu seek_fail=%llu clock_resets=%llu seek_to_first_ms=%llu\n",
            (unsigned long long)pb->stats.seek_requests,
            (unsigned long long)pb->stats.seek_successes,
            (unsigned long long)pb->stats.seek_failures,
            (unsigned long long)pb->stats.clock_resets,
            (unsigned long long)pb->stats.seek_to_first_frame_ms);
    fprintf(f, "clock_late_drops=%llu early_sleeps=%llu pause=%llu resume=%llu\n",
            (unsigned long long)cs.late_drops,
            (unsigned long long)cs.early_sleeps,
            (unsigned long long)cs.pause_count,
            (unsigned long long)cs.resume_count);
    fprintf(f, "sample_bgra=0x%08X\n", (unsigned)pb->stats.sample_bgra);
    fclose(f);
}
