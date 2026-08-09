#include "pp_converter_fused.h"

#include <pthread.h>
#include <string.h>

/* Same constants as reference/SDL_ps5tilemap.inc — data lives in tile_copy.c only */
#define PS5_TILE_WIDTH 512
#define PS5_TILE_HEIGHT 128
#define PS5_TILE_SIZE (PS5_TILE_WIDTH * PS5_TILE_HEIGHT)
extern unsigned short PS5_tilemap[PS5_TILE_HEIGHT][PS5_TILE_WIDTH];

typedef struct {
    const pp_frame *src;
    uint32_t *dst;
    uint32_t frame_w;
    uint32_t y0, y1;
} fused_band_job;

size_t pp_tiled_pixel_offset(int x, int y, int frame_width)
{
    int ty = y / PS5_TILE_HEIGHT;
    int tx = x / PS5_TILE_WIDTH;
    int t = (int)(PS5_TILE_SIZE *
                  (tx + ty * ((double)frame_width / (double)PS5_TILE_WIDTH)));
    int i = (int)PS5_tilemap[y % PS5_TILE_HEIGHT][x % PS5_TILE_WIDTH];
    return (size_t)(t + i);
}

static uint32_t yuv_to_bgra_r_low(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void *fused_band_worker(void *arg)
{
    fused_band_job *j = (fused_band_job *)arg;
    const pp_frame *s = j->src;
    uint32_t y, x;
    uint32_t w = s->width;
    uint32_t fw = j->frame_w;

    /* Process 2×2 blocks: load U/V once per block */
    for (y = j->y0; y < j->y1; y += 2u) {
        const uint8_t *Y0 = s->planes[0] + y * (uint32_t)s->strides[0];
        const uint8_t *Y1 =
            (y + 1u < s->height) ? s->planes[0] + (y + 1u) * (uint32_t)s->strides[0] : Y0;
        const uint8_t *U = s->planes[1] + (y / 2u) * (uint32_t)s->strides[1];
        const uint8_t *V = s->planes[2] + (y / 2u) * (uint32_t)s->strides[2];

        for (x = 0; x < w; x += 2u) {
            int uu = U[x / 2u];
            int vv = V[x / 2u];
            uint32_t p00 = yuv_to_bgra_r_low(Y0[x], uu, vv);
            j->dst[pp_tiled_pixel_offset((int)x, (int)y, (int)fw)] = p00;

            if (x + 1u < w) {
                uint32_t p10 = yuv_to_bgra_r_low(Y0[x + 1u], uu, vv);
                j->dst[pp_tiled_pixel_offset((int)(x + 1u), (int)y, (int)fw)] = p10;
            }
            if (y + 1u < j->y1 && y + 1u < s->height) {
                uint32_t p01 = yuv_to_bgra_r_low(Y1[x], uu, vv);
                j->dst[pp_tiled_pixel_offset((int)x, (int)(y + 1u), (int)fw)] = p01;
                if (x + 1u < w) {
                    uint32_t p11 = yuv_to_bgra_r_low(Y1[x + 1u], uu, vv);
                    j->dst[pp_tiled_pixel_offset((int)(x + 1u), (int)(y + 1u), (int)fw)] = p11;
                }
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Persistent worker pool.
 *
 * This path used to pthread_create and pthread_join every worker on every
 * frame. pp_converter_parallel.c carries a comment naming that exact pattern
 * as the root cause of the Soft-UHD freeze - "that stalls every few frames
 * under scheduler pressure" - and was rewritten around a persistent pool. The
 * fused V8 converter, which is the one 4K playback actually runs through, was
 * never given the same treatment: at 2160p60 with 8 workers it was asking the
 * scheduler for 480 thread creations a second.
 *
 * Same structure as the pool in pp_converter_parallel.c, deliberately: two
 * pools of at most 8 threads each is a known, bounded cost, and sharing one
 * would mean reaching across a module boundary into the 1080 path that is
 * currently working.
 *
 * Unlike that pool, this one honours a change in worker count. The other
 * silently keeps its original size ("Resize not supported mid-run"), which
 * makes a request for more workers quietly do nothing.
 * ------------------------------------------------------------------------ */

#define FUSED_POOL_MAX 8

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  work_cv;
    pthread_cond_t  done_cv;

    pthread_t       th[FUSED_POOL_MAX];
    fused_band_job  jobs[FUSED_POOL_MAX];

    int             n_workers;
    int             started;
    int             shutdown;

    /* Bumped once per frame. A worker runs when it sees a generation it has
     * not handled yet, which avoids the lost-wakeup that a plain flag has. */
    uint64_t        generation;
    uint64_t        seen[FUSED_POOL_MAX];
    int             done_count;
} fused_pool;

static fused_pool g_fpool;
static pthread_mutex_t g_fpool_lock = PTHREAD_MUTEX_INITIALIZER;

static void *fused_pool_worker(void *arg)
{
    int      id  = (int)(intptr_t)arg;
    uint64_t mine = 0;

    for (;;) {
        pthread_mutex_lock(&g_fpool.m);
        while (!g_fpool.shutdown && g_fpool.generation == mine)
            pthread_cond_wait(&g_fpool.work_cv, &g_fpool.m);

        if (g_fpool.shutdown) {
            pthread_mutex_unlock(&g_fpool.m);
            return NULL;
        }

        mine = g_fpool.generation;
        pthread_mutex_unlock(&g_fpool.m);

        if (id < g_fpool.n_workers)
            fused_band_worker(&g_fpool.jobs[id]);

        pthread_mutex_lock(&g_fpool.m);
        g_fpool.seen[id] = mine;
        g_fpool.done_count++;
        pthread_cond_signal(&g_fpool.done_cv);
        pthread_mutex_unlock(&g_fpool.m);
    }
}

static void fused_pool_teardown_locked(void)
{
    int i;

    if (!g_fpool.started) return;

    pthread_mutex_lock(&g_fpool.m);
    g_fpool.shutdown = 1;
    pthread_cond_broadcast(&g_fpool.work_cv);
    pthread_mutex_unlock(&g_fpool.m);

    for (i = 0; i < g_fpool.n_workers; i++)
        pthread_join(g_fpool.th[i], NULL);

    pthread_mutex_destroy(&g_fpool.m);
    pthread_cond_destroy(&g_fpool.work_cv);
    pthread_cond_destroy(&g_fpool.done_cv);

    memset(&g_fpool, 0, sizeof(g_fpool));
}

/* Returns the usable worker count, or 0 if no pool could be built. */
static int fused_pool_ensure(int n)
{
    int i;

    if (g_fpool.started && g_fpool.n_workers == n)
        return n;

    /* Worker count changed - rebuild. Only happens when the caller genuinely
     * asks for a different width, which is rare and never mid-frame. */
    fused_pool_teardown_locked();

    pthread_mutex_init(&g_fpool.m, NULL);
    pthread_cond_init(&g_fpool.work_cv, NULL);
    pthread_cond_init(&g_fpool.done_cv, NULL);

    g_fpool.n_workers = n;
    g_fpool.started   = 1;

    for (i = 0; i < n; i++) {
        if (pthread_create(&g_fpool.th[i], NULL,
                           fused_pool_worker, (void *)(intptr_t)i) != 0) {
            g_fpool.n_workers = i;   /* run with what we got */
            break;
        }
    }

    return g_fpool.n_workers;
}

int pp_converter_yuv420p_to_tiled_bgra_parallel(const pp_frame *source,
                                                uint32_t *tiled_destination,
                                                uint32_t frame_width,
                                                uint32_t frame_height,
                                                int workers)
{
    int i, n = workers;
    uint32_t band;

    if (!source || !tiled_destination || !source->planes[0] || !source->planes[1] ||
        !source->planes[2])
        return -1;
    if (source->format != PP_FRAME_YUV420P)
        return -2;
    if (source->width != frame_width || source->height != frame_height)
        return -3;
    if (n < 1)
        n = 1;
    if (n > FUSED_POOL_MAX)
        n = FUSED_POOL_MAX;

    band = (frame_height + (uint32_t)n - 1u) / (uint32_t)n;
    /* even rows for 420 + 2×2 block processing */
    if (band & 1u)
        band++;

    /* One frame at a time. The pool is process-wide state and the caller is
     * the single render thread, but a stray concurrent call would otherwise
     * interleave two frames' jobs into the same slots. */
    pthread_mutex_lock(&g_fpool_lock);

    n = fused_pool_ensure(n);

    if (n <= 0) {
        /* No threads available - do the whole frame inline rather than
         * dropping it. Slow beats blank. */
        fused_band_job job;

        pthread_mutex_unlock(&g_fpool_lock);

        job.src     = source;
        job.dst     = tiled_destination;
        job.frame_w = frame_width;
        job.y0      = 0;
        job.y1      = frame_height;
        fused_band_worker(&job);
        return 0;
    }

    for (i = 0; i < n; i++) {
        g_fpool.jobs[i].src     = source;
        g_fpool.jobs[i].dst     = tiled_destination;
        g_fpool.jobs[i].frame_w = frame_width;
        g_fpool.jobs[i].y0      = (uint32_t)i * band;
        g_fpool.jobs[i].y1      = g_fpool.jobs[i].y0 + band;
        if (g_fpool.jobs[i].y1 > frame_height)
            g_fpool.jobs[i].y1 = frame_height;
        if (g_fpool.jobs[i].y0 >= frame_height)
            g_fpool.jobs[i].y0 = g_fpool.jobs[i].y1 = frame_height;
    }

    pthread_mutex_lock(&g_fpool.m);
    g_fpool.done_count = 0;
    g_fpool.generation++;
    pthread_cond_broadcast(&g_fpool.work_cv);

    while (g_fpool.done_count < n)
        pthread_cond_wait(&g_fpool.done_cv, &g_fpool.m);
    pthread_mutex_unlock(&g_fpool.m);

    pthread_mutex_unlock(&g_fpool_lock);
    return 0;
}
