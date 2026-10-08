/*
 * evo_parallel_io.h - parallel read-ahead for big network files.
 */
#ifndef EVO_PARALLEL_IO_H
#define EVO_PARALLEL_IO_H

#include <libavformat/avio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_pio evo_pio;

/* NULL when the URL is not a big file served with byte ranges (then FFmpeg's
 * own single connection is used as before). */
evo_pio *evo_pio_open(const char *url, const char *headers, const char *user_agent);
AVIOContext *evo_pio_avio(evo_pio *p);
/* Fail the reads that are waiting (playback is being stopped). */
void evo_pio_abort(evo_pio *p);
/* After avformat_close_input(). */
void evo_pio_close(evo_pio *p);

/* For the /stats dashboard. Counters run for the life of the open reader. */
typedef struct {
    int       active;          /* a parallel reader is open */
    int       connections;
    int       ready_chunks;    /* fetched and waiting for the reader */
    int       window_chunks;
    int       chunk_mb;
    long long bytes;           /* fetched since open */
    long long chunk_failures;  /* a worker gave up on a chunk (it is retried) */
    long long stall_ms;        /* how long the reader has been waiting now, 0 if not */
} evo_pio_stats_t;
void evo_pio_get_stats(evo_pio_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif
