/*
 * evo_stream_io.c — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput 8MB–16MB streaming buffers with sequential disk
 * prefetching (posix_fadvise) and optimized dictionary options for 100+ Mbps 4K media.
 */
#include "evo_stream_io.h"
#include "evo_direct_mem.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#if defined(POSIX_FADV_WILLNEED)
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
}

static void prefetch_file_sequential(const char *path)
{
    if (!path || strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
        return;

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        evo_stream_io_hint_sequential(fd);
        close(fd);
    }
}

int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx)
{
    if (!path || !out_fmt_ctx) return -1;

    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)evo_direct_mem_calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) ctx = (evo_stream_io_ctx_t *)calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) return -1;

    ctx->is_network = (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
    snprintf(ctx->media_path, sizeof(ctx->media_path), "%s", path);

    size_t ring_size = (cfg && cfg->ring_buffer_size > 0) ? cfg->ring_buffer_size : EVO_STREAM_DEFAULT_RING_SIZE;
    char buf_size_str[32];
    snprintf(buf_size_str, sizeof(buf_size_str), "%zu", ring_size);

    AVDictionary *opts = NULL;

    /* Fast probe & high-throughput streaming buffer options */
    av_dict_set(&opts, "probesize", "4194304", 0);
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    av_dict_set(&opts, "buffer_size", buf_size_str, 0);

    if (ctx->is_network) {
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "5", 0);
        av_dict_set(&opts, "timeout", "5000000", 0);
    } else {
        /* Prime the kernel storage controller for sequential read-ahead */
        prefetch_file_sequential(path);
    }

    AVFormatContext *fmt = NULL;
    int rc = avformat_open_input(&fmt, path, NULL, &opts);
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
