/*
 * CPU tiling helper — shares PS5_tilemap defined by the player (main.c includes
 * SDL_ps5tilemap.inc once). Do not include the .inc here (duplicate symbol).
 *
 * EVO: rewritten for speed. The address math is UNCHANGED - see the proof
 * below - only the way it is computed.
 *
 * The original evaluated, for every one of 2,073,600 pixels at 1080p:
 *     x  = ind % frame_width          integer division
 *     y  = ind / frame_width          integer division
 *     ty = y / 128                    integer division
 *     tx = x / 512                    integer division
 *     t  = (int)(65536 * (tx + ty * ((double)frame_width / 512)))
 *                                     double divide + multiply
 *     i  = PS5_tilemap[y % 128][x % 512]
 *                                     two more modulos
 *
 * That is the reason playback juddered once flip synchronisation was correct:
 * the swizzle could not finish a frame inside its budget, so the renderer was
 * blocked on buffer acquire nearly every frame (telemetry: blocked ~= flips).
 *
 * PROOF THE ADDRESSES ARE IDENTICAL
 *     t = TILE_SIZE * (tx + ty * w / TILE_WIDTH)
 *       = TILE_SIZE*tx + (TILE_SIZE / TILE_WIDTH) * ty * w
 *       = TILE_SIZE*tx + TILE_HEIGHT * ty * w          [65536/512 = 128 exactly]
 * The division is exact for every w, so the original double arithmetic never
 * rounded and the integer form is equivalent - including the 1920 case where
 * w/512 = 3.75. The old comment warns against ceil(w/512); that warning still
 * stands and is NOT what this does.
 *
 * The rewrite also walks whole tile rows, so the destination base is computed
 * once per 512 pixels instead of per pixel, and the tilemap row pointer once
 * per scanline. The inner loop is a load, a table lookup and an indexed store.
 */
#include "pp_platform.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#ifndef PS5_TILE_WIDTH
#define PS5_TILE_WIDTH  512
#define PS5_TILE_HEIGHT 128
#define PS5_TILE_SIZE   (PS5_TILE_WIDTH * PS5_TILE_HEIGHT)
#endif

/* Both are powers of two, so the divisions become shifts and the modulos
 * become masks. Asserted at compile time rather than assumed. */
#define PS5_TILE_W_SHIFT 9    /* 1 << 9  == 512 */
#define PS5_TILE_H_SHIFT 7    /* 1 << 7  == 128 */
#define PS5_TILE_W_MASK  (PS5_TILE_WIDTH  - 1)
#define PS5_TILE_H_MASK  (PS5_TILE_HEIGHT - 1)

typedef char pp_tile_dims_must_be_pow2[
    ((1 << PS5_TILE_W_SHIFT) == PS5_TILE_WIDTH &&
     (1 << PS5_TILE_H_SHIFT) == PS5_TILE_HEIGHT) ? 1 : -1];

extern unsigned short PS5_tilemap[PS5_TILE_HEIGHT][PS5_TILE_WIDTH];

#define PP_TILE_THREADS 12

typedef struct {
    const uint32_t *src;
    uint32_t       *dst;
    int             frame_width;
    int             y_start;      /* rows, not pixel indices */
    int             y_end;
} PP_DrawChunk;

static void *pp_tile_thread(void *arg)
{
    const PP_DrawChunk *c = (const PP_DrawChunk *)arg;
    const int w = c->frame_width;
    int y;

    for (y = c->y_start; y < c->y_end; y++) {
        const unsigned short *lut = PS5_tilemap[y & PS5_TILE_H_MASK];
        const uint32_t *s = c->src + (size_t)y * (size_t)w;

        /* Base of this tile ROW: TILE_HEIGHT * ty * w  (see proof above). */
        const size_t row_base =
            (size_t)(y >> PS5_TILE_H_SHIFT) * (size_t)PS5_TILE_HEIGHT * (size_t)w;

        int x0;
        for (x0 = 0; x0 < w; x0 += PS5_TILE_WIDTH) {
            /* Base of this tile within the row: TILE_SIZE * tx. */
            uint32_t *d = c->dst + row_base +
                          ((size_t)(x0 >> PS5_TILE_W_SHIFT) * (size_t)PS5_TILE_SIZE);
            const uint32_t *ss = s + x0;

            int n = w - x0;
            if (n > PS5_TILE_WIDTH)
                n = PS5_TILE_WIDTH;   /* 1920 leaves a 384-wide remainder */

            for (int k = 0; k < n; k++)
                d[lut[k]] = ss[k];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Persistent worker pool.
 *
 * This function used to pthread_create and pthread_join twelve workers on
 * every frame. That is the exact pattern the comment at the top of
 * pp_converter_parallel.c names as the Soft-UHD freeze root cause - "that
 * stalls every few frames under scheduler pressure" - and both converters were
 * rewritten around persistent pools because of it. This one was missed, and it
 * is not a dormant path: pp_videoout_present calls it for every 1080p frame,
 * so at 60fps it was asking the scheduler for 720 thread creations a second on
 * the render thread, the one thread that must not stall.
 *
 * Same structure as the pool in pp_converter_fused.c, including honouring a
 * change in worker count rather than silently keeping the original size.
 * ------------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  work_cv;
    pthread_cond_t  done_cv;

    pthread_t       th[PP_TILE_THREADS];
    PP_DrawChunk    jobs[PP_TILE_THREADS];

    int             n_workers;
    int             started;
    int             shutdown;

    /* Bumped once per frame. A worker runs when it sees a generation it has
     * not handled yet, which avoids the lost-wakeup a plain flag has. */
    uint64_t        generation;
    int             done_count;
} pp_tile_pool;

static pp_tile_pool   g_tpool;
static pthread_mutex_t g_tpool_lock = PTHREAD_MUTEX_INITIALIZER;

static void *pp_tile_pool_worker(void *arg)
{
    int      id   = (int)(intptr_t)arg;
    uint64_t mine = 0;

    for (;;) {
        pthread_mutex_lock(&g_tpool.m);
        while (!g_tpool.shutdown && g_tpool.generation == mine)
            pthread_cond_wait(&g_tpool.work_cv, &g_tpool.m);

        if (g_tpool.shutdown) {
            pthread_mutex_unlock(&g_tpool.m);
            return NULL;
        }

        mine = g_tpool.generation;
        pthread_mutex_unlock(&g_tpool.m);

        if (id < g_tpool.n_workers)
            pp_tile_thread(&g_tpool.jobs[id]);

        pthread_mutex_lock(&g_tpool.m);
        g_tpool.done_count++;
        pthread_cond_signal(&g_tpool.done_cv);
        pthread_mutex_unlock(&g_tpool.m);
    }
}

static void pp_tile_pool_teardown(void)
{
    int i;

    if (!g_tpool.started)
        return;

    pthread_mutex_lock(&g_tpool.m);
    g_tpool.shutdown = 1;
    pthread_cond_broadcast(&g_tpool.work_cv);
    pthread_mutex_unlock(&g_tpool.m);

    for (i = 0; i < g_tpool.n_workers; i++)
        pthread_join(g_tpool.th[i], NULL);

    pthread_mutex_destroy(&g_tpool.m);
    pthread_cond_destroy(&g_tpool.work_cv);
    pthread_cond_destroy(&g_tpool.done_cv);

    memset(&g_tpool, 0, sizeof(g_tpool));
}

/* Returns the usable worker count, or 0 if no pool could be built. */
static int pp_tile_pool_ensure(int n)
{
    int i;

    if (g_tpool.started && g_tpool.n_workers == n)
        return n;

    pp_tile_pool_teardown();

    pthread_mutex_init(&g_tpool.m, NULL);
    pthread_cond_init(&g_tpool.work_cv, NULL);
    pthread_cond_init(&g_tpool.done_cv, NULL);

    g_tpool.n_workers = n;
    g_tpool.started   = 1;

    for (i = 0; i < n; i++) {
        if (pthread_create(&g_tpool.th[i], NULL,
                           pp_tile_pool_worker, (void *)(intptr_t)i) != 0) {
            g_tpool.n_workers = i;   /* run with what we got */
            break;
        }
    }

    return g_tpool.n_workers;
}

void pp_draw_pixels_as_tiles(uint32_t *src, uint32_t *dst, int frame_width,
                             int frame_height)
{
    int n = PP_TILE_THREADS;
    int i;

    if (!src || !dst || frame_width <= 0 || frame_height <= 0)
        return;

    if (n > frame_height)
        n = frame_height;   /* more threads than rows */

    /* One frame at a time. The pool is process-wide state and the caller is
     * the single render thread, but a stray concurrent call would otherwise
     * interleave two frames' jobs into the same slots. */
    pthread_mutex_lock(&g_tpool_lock);

    n = pp_tile_pool_ensure(n);

    if (n <= 0) {
        /* No threads available - do the whole frame inline rather than
         * dropping it. Slow beats blank. */
        PP_DrawChunk c;

        pthread_mutex_unlock(&g_tpool_lock);

        c.src         = src;
        c.dst         = dst;
        c.frame_width = frame_width;
        c.y_start     = 0;
        c.y_end       = frame_height;
        pp_tile_thread(&c);
        return;
    }

    /* Split by whole rows so each thread owns complete scanlines - a row never
     * straddles two threads, which keeps the per-row setup out of the inner
     * loop and avoids two threads writing into the same tile. */
    for (i = 0; i < n; i++) {
        g_tpool.jobs[i].src         = src;
        g_tpool.jobs[i].dst         = dst;
        g_tpool.jobs[i].frame_width = frame_width;
        g_tpool.jobs[i].y_start     = (int)(((long)frame_height * i) / n);
        g_tpool.jobs[i].y_end       = (int)(((long)frame_height * (i + 1)) / n);
    }

    pthread_mutex_lock(&g_tpool.m);
    g_tpool.done_count = 0;
    g_tpool.generation++;
    pthread_cond_broadcast(&g_tpool.work_cv);

    while (g_tpool.done_count < n)
        pthread_cond_wait(&g_tpool.done_cv, &g_tpool.m);
    pthread_mutex_unlock(&g_tpool.m);

    pthread_mutex_unlock(&g_tpool_lock);
}
