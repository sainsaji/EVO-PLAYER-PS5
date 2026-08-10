#include "pp_converter_fused.h"

#include <pthread.h>
#include <string.h>

/* Same constants as reference/SDL_ps5tilemap.inc — data lives in tile_copy.c only */
#define PS5_TILE_WIDTH 512
#define PS5_TILE_HEIGHT 128
#define PS5_TILE_SIZE (PS5_TILE_WIDTH * PS5_TILE_HEIGHT)
extern unsigned short PS5_tilemap[PS5_TILE_HEIGHT][PS5_TILE_WIDTH];

/* Both dimensions are powers of two, so the divisions in the tile address are
 * shifts and the modulos are masks. Asserted rather than assumed, exactly as
 * tile_copy.c does it. */
#define PS5_TILE_W_SHIFT 9 /* 1 << 9 == 512 */
#define PS5_TILE_H_SHIFT 7 /* 1 << 7 == 128 */
#define PS5_TILE_W_MASK  (PS5_TILE_WIDTH - 1)
#define PS5_TILE_H_MASK  (PS5_TILE_HEIGHT - 1)

typedef char pp_fused_tile_dims_must_be_pow2[
    ((1 << PS5_TILE_W_SHIFT) == PS5_TILE_WIDTH &&
     (1 << PS5_TILE_H_SHIFT) == PS5_TILE_HEIGHT) ? 1 : -1];

typedef struct {
    const pp_frame *src;
    uint32_t *dst;
    uint32_t frame_w;
    uint32_t y0, y1;
} fused_band_job;

/*
 * The address math is UNCHANGED from the original double-precision form; only
 * the way it is computed. Same proof as tile_copy.c:
 *
 *     t = TILE_SIZE * (tx + ty * w / TILE_WIDTH)
 *       = TILE_SIZE*tx + (TILE_SIZE / TILE_WIDTH) * ty * w
 *       = TILE_SIZE*tx + TILE_HEIGHT * ty * w        [65536/512 = 128 exactly]
 *
 * The division is exact for every w, so the double arithmetic never rounded -
 * including 1920, where w/512 = 3.75. This is NOT ceil(w/512); that remains
 * wrong, as the old comment warns.
 *
 * Kept because it is public API and it documents the mapping. The band worker
 * no longer calls it: it walks whole tile spans instead, so the base is
 * computed once per 512 pixels rather than once per pixel.
 */
size_t pp_tiled_pixel_offset(int x, int y, int frame_width)
{
    size_t t = (size_t)(y >> PS5_TILE_H_SHIFT) * (size_t)PS5_TILE_HEIGHT *
                   (size_t)frame_width +
               (size_t)(x >> PS5_TILE_W_SHIFT) * (size_t)PS5_TILE_SIZE;
    size_t i = (size_t)PS5_tilemap[y & PS5_TILE_H_MASK][x & PS5_TILE_W_MASK];
    return t + i;
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

/*
 * Walks the band a tile span at a time, not a pixel at a time.
 *
 * The previous version called pp_tiled_pixel_offset four times per 2×2 block,
 * and that helper evaluated a double divide and a double multiply per call:
 * at 2160p that is 8.3M double divides per frame purely to compute addresses.
 * docs/converter-perf.md named it as the next experiment, and tile_copy.c had
 * already made the same move for the linear tiling path.
 *
 * Now the tile base is hoisted out - once per 512 pixels instead of once per
 * pixel - and the intra-tile index is the tilemap row lookup the loop was
 * doing anyway. The inner loop is a load, two table lookups and two indexed
 * stores. Addresses are bit-for-bit the ones the old formula produced; the
 * benchmark's plane hashes are the check.
 */
static void *fused_band_worker(void *arg)
{
    fused_band_job *j = (fused_band_job *)arg;
    const pp_frame *s = j->src;
    const uint32_t w = s->width;
    const uint32_t fw = j->frame_w;
    uint32_t y;

    /* Process 2×2 blocks: load U/V once per block */
    for (y = j->y0; y < j->y1; y += 2u) {
        const uint8_t *Y0 = s->planes[0] + (size_t)y * (size_t)s->strides[0];
        const uint8_t *U = s->planes[1] + (size_t)(y / 2u) * (size_t)s->strides[1];
        const uint8_t *V = s->planes[2] + (size_t)(y / 2u) * (size_t)s->strides[2];

        /* The odd row exists when both the band and the frame still have one.
         * Bands are cut on even boundaries, so this is only false on a final
         * odd row - but 4:2:0 with an odd height is the caller's problem, not
         * something to read past the plane for. */
        const int has_odd_row = (y + 1u < j->y1) && (y + 1u < s->height);
        const uint8_t *Y1 =
            has_odd_row ? s->planes[0] + (size_t)(y + 1u) * (size_t)s->strides[0] : Y0;

        /* y is even and tiles are 128 rows tall, so both rows of every block
         * land in the same tile row: one base serves the pair. */
        const size_t row_base =
            (size_t)(y >> PS5_TILE_H_SHIFT) * (size_t)PS5_TILE_HEIGHT * (size_t)fw;
        const unsigned short *lut0 = PS5_tilemap[y & PS5_TILE_H_MASK];
        const unsigned short *lut1 = PS5_tilemap[(y + 1u) & PS5_TILE_H_MASK];

        uint32_t x0;
        for (x0 = 0; x0 < w; x0 += PS5_TILE_WIDTH) {
            uint32_t *d = j->dst + row_base +
                          ((size_t)(x0 >> PS5_TILE_W_SHIFT) * (size_t)PS5_TILE_SIZE);
            const uint8_t *y0p = Y0 + x0;
            const uint8_t *y1p = Y1 + x0;
            const uint8_t *up = U + x0 / 2u;
            const uint8_t *vp = V + x0 / 2u;
            uint32_t k;

            uint32_t n = w - x0;
            if (n > PS5_TILE_WIDTH)
                n = PS5_TILE_WIDTH; /* 1920 leaves a 384-wide remainder */

            for (k = 0; k < n; k += 2u) {
                int uu = up[k / 2u];
                int vv = vp[k / 2u];

                d[lut0[k]] = yuv_to_bgra_r_low(y0p[k], uu, vv);
                if (k + 1u < n)
                    d[lut0[k + 1u]] = yuv_to_bgra_r_low(y0p[k + 1u], uu, vv);

                if (has_odd_row) {
                    d[lut1[k]] = yuv_to_bgra_r_low(y1p[k], uu, vv);
                    if (k + 1u < n)
                        d[lut1[k + 1u]] = yuv_to_bgra_r_low(y1p[k + 1u], uu, vv);
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
