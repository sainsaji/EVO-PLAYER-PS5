/*
 * evo_stream_io.c — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput 8MB–16MB streaming buffers with sequential disk
 * prefetching (posix_fadvise) and optimized dictionary options for 100+ Mbps 4K media.
 */
#include "evo_stream_io.h"
#include "evo_direct_mem.h"

#ifdef EVO_APP_MODULE
extern void pp_stage_bc(const char *stage_id, const char *detail);
#  define SIO_BC(id, d) pp_stage_bc((id), (d))
#else
#  define SIO_BC(id, d) ((void)0)
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EVO_STREAM_DEFAULT_RING_SIZE (8 * 1024 * 1024) /* 8 MiB */

struct evo_stream_io_ctx {
    int     is_network;
    char    media_path[512];
};

void evo_stream_io_hint_sequential(int fd)
{
    if (fd < 0) return;
    /* posix_fadvise is not wired on the PS5 kernel from the app-module
     * process - it faults (SIGSYS-class, same as exit()). It is only a
     * read-ahead hint, so skip it there. Hardware-confirmed 2026-09-02:
     * the crash on file-open was here, between P8_01c and P8_01d. */
#ifndef EVO_APP_MODULE
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#if defined(POSIX_FADV_WILLNEED)
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
#endif
}

static void prefetch_file_sequential(const char *path)
{
    if (!path || strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
        return;
#ifndef EVO_APP_MODULE
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        evo_stream_io_hint_sequential(fd);
        close(fd);
    }
#endif
}

/*
 * The frame rate a raw .obu's file name states, as an AVRational string, or 0.
 * Takes the digits before "fps" ("2397fps", "23.976fps", "24fps"). Four
 * undotted digits are hundredths (Netflix's 2397 / 2997 / 5994), and anything
 * within 0.02 of an NTSC rate snaps to its exact n*1000/1001 form.
 */
static int raw_av1_rate_from_name(const char *path, char *out, size_t out_len)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".obu") != 0)
        return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    for (const char *p = strstr(base, "fps"); p; p = strstr(p + 1, "fps")) {
        const char *q = p;
        while (q > base && ((q[-1] >= '0' && q[-1] <= '9') || q[-1] == '.'))
            q--;
        if (q == p)
            continue;
        char num[16];
        size_t n = (size_t)(p - q);
        if (n >= sizeof num)
            continue;
        memcpy(num, q, n);
        num[n] = 0;
        double v = atof(num);
        if (!strchr(num, '.') && n == 4)
            v /= 100.0;
        if (v < 1.0 || v > 240.0)
            continue;
        static const int ntsc[] = { 24, 30, 48, 60, 120 };
        for (size_t i = 0; i < sizeof ntsc / sizeof ntsc[0]; i++) {
            double r = ntsc[i] * 1000.0 / 1001.0;
            if (v > r - 0.02 && v < r + 0.02) {
                snprintf(out, out_len, "%d/1001", ntsc[i] * 1000);
                return 1;
            }
        }
        snprintf(out, out_len, "%d/1000", (int)(v * 1000.0 + 0.5));
        return 1;
    }
    return 0;
}

int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx)
{
    if (!path || !out_fmt_ctx) return -1;

    SIO_BC("P8_01a_SIO_ENTER", path);
    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)evo_direct_mem_calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) ctx = (evo_stream_io_ctx_t *)calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) return -1;
    SIO_BC("P8_01b_SIO_CTX", "ctx alloc ok");

    ctx->is_network = (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
    snprintf(ctx->media_path, sizeof(ctx->media_path), "%s", path);

    size_t ring_size = (cfg && cfg->ring_buffer_size > 0) ? cfg->ring_buffer_size : EVO_STREAM_DEFAULT_RING_SIZE;
    char buf_size_str[32];
    snprintf(buf_size_str, sizeof(buf_size_str), "%zu", ring_size);

    AVDictionary *opts = NULL;

    /*
     * Fast probe & high-throughput streaming buffer options.
     *
     * analyzeduration is 4 s, not the 2 s this file used while it had no
     * callers: PlaybackController's own open (the one that actually shipped)
     * used 4 s, and #90 routes that open through here. Halving it would change
     * stream detection on awkward MPEG-TS - the files where a second audio
     * track appears late - as a side effect of a refactor, which is the kind of
     * regression that gets blamed on the codec pass rather than on this line.
     */
    av_dict_set(&opts, "probesize", "4194304", 0);
    av_dict_set(&opts, "analyzeduration", "4000000", 0);
    av_dict_set(&opts, "buffer_size", buf_size_str, 0);

    if (ctx->is_network) {
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "5", 0);
        av_dict_set(&opts, "timeout", "5000000", 0);
    } else {
        /* Prime the kernel storage controller for sequential read-ahead */
        SIO_BC("P8_01c_PREFETCH", "open+fadvise");
        prefetch_file_sequential(path);
    }

    /* A raw AV1 .obu carries no timestamps, and the obu demuxer invents them
     * from its `framerate` option - 25 by default. 23.976 fps film then plays
     * 4% fast (#94, Chimera on hardware 2026-09-25). The sequence header's
     * timing_info is optional and Netflix's Open Content streams omit it; the
     * name is the one place the rate is written down ("...-2397fps-..."). */
    if (!ctx->is_network) {
        char rate[16];
        if (raw_av1_rate_from_name(path, rate, sizeof rate)) {
            av_dict_set(&opts, "framerate", rate, 0);
            SIO_BC("P8_01c2_OBU_RATE", rate);
        }
    }

    SIO_BC("P8_01d_PRE_OPEN", "-> avformat_open_input");
    AVFormatContext *fmt = NULL;
    int rc = avformat_open_input(&fmt, path, NULL, &opts);
    {
        char d[32]; snprintf(d, sizeof d, "rc=%d", rc);
        SIO_BC("P8_01e_OPEN_RC", d);
    }
    av_dict_free(&opts);

    if (rc < 0) {
        evo_direct_mem_free(ctx);
        return rc;
    }

    *out_fmt_ctx = fmt;
    if (out_io_ctx) *out_io_ctx = ctx;
    return 0;
}

void evo_stream_io_close(evo_stream_io_ctx_t *ctx)
{
    if (!ctx) return;
    evo_direct_mem_free(ctx);
}
