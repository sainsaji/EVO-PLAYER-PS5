/*
 * evo_stream_io.h — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput asynchronous AVIO streaming buffers with 8MB–16MB
 * read-ahead ring buffering, sequential disk prefetching (posix_fadvise),
 * and direct memory buffer management for 100+ Mbps 4K REMUX media streams.
 */
#ifndef EVO_STREAM_IO_H
#define EVO_STREAM_IO_H

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t ring_buffer_size;  /* default 8 MiB (8 * 1024 * 1024) */
    size_t io_block_size;     /* default 512 KiB (512 * 1024) */
    int    enable_fadvise;    /* POSIX_FADV_SEQUENTIAL / WILLNEED */
} evo_stream_io_config_t;

typedef struct evo_stream_io_ctx evo_stream_io_ctx_t;

/**
 * Open a media stream with High-Throughput I/O ring buffering and direct memory.
 * Sets up custom AVIOContext with sequential kernel readahead and enlarged buffer.
 *
 * @param path          File path (e.g. /mnt/usb0/movie.mkv) or URL (http://...)
 * @param out_fmt_ctx   Pointer to AVFormatContext* to be populated
 * @param cfg           Optional config overrides (pass NULL for defaults)
 * @param out_io_ctx    Pointer to receive stream I/O context handle
 * @return 0 on success, negative error code on failure
 */
int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx);

/**
 * Close stream I/O context and release buffers.
 */
void evo_stream_io_close(evo_stream_io_ctx_t *io_ctx);

/**
 * Apply kernel sequential read-ahead hints on open file descriptors.
 */
void evo_stream_io_hint_sequential(int fd);

#ifdef __cplusplus
}
#endif

#endif /* EVO_STREAM_IO_H */
