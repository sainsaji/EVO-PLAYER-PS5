/*
 * tools/bench_converter.c - measure the YUV->BGRA+swizzle path on the host.
 *
 * Why this exists
 * ---------------
 * With no hardware GL or Vulkan driver in the sysroot (see docs/gpu-notes.md -
 * what ships is OSMesa/llvmpipe, a software rasteriser), the CPU converter is
 * the only remaining lever on 4K playback performance. tile_copy.c already
 * records what happens when it misses: "the swizzle could not finish a frame
 * inside its budget, so the renderer was blocked on buffer acquire nearly
 * every frame."
 *
 * That is a throughput question, and throughput questions should not need a
 * console. The converter takes a plain pp_frame in and writes a plain buffer
 * out, with no FFmpeg and no VideoOut in the way, so it runs anywhere.
 *
 *   ./tools/bench.sh
 *
 * Reports milliseconds per frame against the 60fps and 30fps budgets, and
 * scaling across worker counts. Numbers from a dev container are not the
 * console's numbers - the point is *relative* measurement, so an optimisation
 * can be accepted or rejected before it is ever deployed.
 *
 * Correctness is checked first, and the benchmark refuses to report timings if
 * it fails. A faster converter that produces different pixels is not faster.
 */

#include "pp_converter_fused.h"
#include "pp_converter_parallel.h"
#include "pp_frame.h"
#include "pp_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* tile_copy.c expects the player to supply this; main.c gets it by including
 * the .inc exactly once. The benchmark has to do the same. */
#include "SDL_ps5tilemap.inc"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/*
 * Size of the tiled destination plane, in pixels.
 *
 * This is NOT width*height. The swizzle addresses whole 512x128 tiles, so the
 * plane is padded out to tile boundaries: at 1920x1080 the highest offset the
 * formula produces is
 *
 *     65536 * (3 + 8 * (1920/512.0)) + 65535  =  2,228,223
 *
 * against only 2,073,600 pixels in the visible frame. On the console this is
 * invisible because VideoOut registers an aligned buffer for us; a host
 * harness that allocates width*height gets heap corruption on the very first
 * frame. Worth knowing before writing any other host test against this code.
 */
#define PS5_TILE_W 512
#define PS5_TILE_H 128

static size_t tiled_plane_pixels(uint32_t w, uint32_t h)
{
    size_t tiles_x = (w + PS5_TILE_W - 1) / PS5_TILE_W;
    size_t tiles_y = (h + PS5_TILE_H - 1) / PS5_TILE_H;

    return tiles_x * PS5_TILE_W * tiles_y * PS5_TILE_H;
}

/* ---- synthetic source ---------------------------------------------------- */

/*
 * A gradient plus a moving-ish pattern rather than flat colour. Flat input can
 * flatter a converter: constant data is friendlier to the branch predictor and
 * to any accidental early-out, and it would hide a regression that only shows
 * on real video.
 */
typedef struct {
    uint8_t *y, *u, *v;
    uint32_t w, h;
} test_frame;

static int test_frame_alloc(test_frame *f, uint32_t w, uint32_t h)
{
    uint32_t cw = w / 2, ch = h / 2;
    uint32_t x, yy;

    f->w = w;
    f->h = h;
    f->y = malloc((size_t)w * h);
    f->u = malloc((size_t)cw * ch);
    f->v = malloc((size_t)cw * ch);

    if (!f->y || !f->u || !f->v) return -1;

    for (yy = 0; yy < h; yy++)
        for (x = 0; x < w; x++)
            f->y[(size_t)yy * w + x] = (uint8_t)((x * 3 + yy * 5) & 0xFF);

    for (yy = 0; yy < ch; yy++) {
        for (x = 0; x < cw; x++) {
            f->u[(size_t)yy * cw + x] = (uint8_t)((x ^ yy) & 0xFF);
            f->v[(size_t)yy * cw + x] = (uint8_t)((x + yy * 2) & 0xFF);
        }
    }

    return 0;
}

static void test_frame_free(test_frame *f)
{
    free(f->y); free(f->u); free(f->v);
}

static void test_frame_bind(const test_frame *f, pp_frame *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->format    = PP_FRAME_YUV420P;
    pf->width     = f->w;
    pf->height    = f->h;
    pf->planes[0] = f->y;
    pf->planes[1] = f->u;
    pf->planes[2] = f->v;
    pf->strides[0] = (int)f->w;
    pf->strides[1] = (int)(f->w / 2);
    pf->strides[2] = (int)(f->w / 2);
}

/* ---- correctness --------------------------------------------------------- */

/*
 * A hash over the whole output plane. Cheap, order-sensitive, and enough to
 * catch "the pixels moved" as well as "the pixels changed" - which matters
 * here, because the swizzle is address math and the classic regression is
 * writing correct colours to the wrong offsets.
 */
static uint64_t plane_hash(const uint32_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/*
 * The single-worker result is the reference. Any worker count must produce a
 * byte-identical plane: band splitting that drops or double-writes a row is
 * exactly the kind of bug that only appears at some thread counts, and it
 * would otherwise show up as an intermittent visual glitch on hardware.
 */
static int check_worker_consistency(const pp_frame *src, uint32_t w, uint32_t h)
{
    size_t    n   = tiled_plane_pixels(w, h);
    uint32_t *ref = calloc(n, 4);
    uint32_t *got = calloc(n, 4);
    const int workers[] = { 1, 2, 4, 6, 8 };
    uint64_t  ref_hash;
    int       i, ok = 1;

    if (!ref || !got) { free(ref); free(got); return 0; }

    pp_converter_yuv420p_to_tiled_bgra_parallel(src, ref, w, h, 1);
    ref_hash = plane_hash(ref, n);

    printf("  reference (1 worker) hash %016llx\n",
           (unsigned long long)ref_hash);

    for (i = 1; i < (int)(sizeof(workers) / sizeof(workers[0])); i++) {
        uint64_t hh;

        memset(got, 0, n * 4);
        pp_converter_yuv420p_to_tiled_bgra_parallel(src, got, w, h, workers[i]);
        hh = plane_hash(got, n);

        printf("  %d workers            hash %016llx  %s\n",
               workers[i], (unsigned long long)hh,
               hh == ref_hash ? "match" : "*** MISMATCH ***");

        if (hh != ref_hash) ok = 0;
    }

    free(ref);
    free(got);
    return ok;
}

/* ---- timing -------------------------------------------------------------- */

static double bench_fused(const pp_frame *src, uint32_t w, uint32_t h,
                          int workers, int iters, uint32_t *dst)
{
    double t0, t1;
    int i;

    /* One untimed pass so the destination is resident and the threads exist;
     * otherwise the first iteration measures page faults. */
    pp_converter_yuv420p_to_tiled_bgra_parallel(src, dst, w, h, workers);

    t0 = now_ms();
    for (i = 0; i < iters; i++)
        pp_converter_yuv420p_to_tiled_bgra_parallel(src, dst, w, h, workers);
    t1 = now_ms();

    return (t1 - t0) / iters;
}

static double bench_linear(const pp_frame *src, uint32_t w, uint32_t h,
                           int workers, int iters, uint32_t *dst)
{
    double t0, t1;
    int i;

    pp_converter_yuv420p_to_bgra_parallel(src, dst, w * 4, workers);

    t0 = now_ms();
    for (i = 0; i < iters; i++)
        pp_converter_yuv420p_to_bgra_parallel(src, dst, w * 4, workers);
    t1 = now_ms();

    (void)h;
    return (t1 - t0) / iters;
}

static void run_size(uint32_t w, uint32_t h, int iters)
{
    test_frame tf;
    pp_frame   src;
    uint32_t  *dst_tiled;
    uint32_t  *dst_linear;
    const int  workers[] = { 1, 2, 4, 6, 8 };
    double     base_fused = 0.0, base_linear = 0.0;
    size_t     i;

    printf("\n=== %ux%u, %d iterations ===\n", w, h, iters);

    if (test_frame_alloc(&tf, w, h) != 0) {
        printf("  allocation failed\n");
        return;
    }
    test_frame_bind(&tf, &src);

    /* Two destinations: the fused path writes a tile-padded plane, the linear
     * path writes a plain w*h*4 surface. Sharing one buffer would either
     * over-allocate the linear case or under-allocate the tiled one - and
     * under-allocating it is a heap overflow, confirmed with ASan. */
    dst_tiled  = calloc(tiled_plane_pixels(w, h), 4);
    dst_linear = calloc((size_t)w * h, 4);

    if (!dst_tiled || !dst_linear) {
        free(dst_tiled); free(dst_linear); test_frame_free(&tf);
        printf("  dst alloc failed\n");
        return;
    }

    printf(" tiled plane %zu px, %.1f%% larger than the %ux%u frame (%zu px)\n",
           tiled_plane_pixels(w, h),
           100.0 * ((double)tiled_plane_pixels(w, h) / ((double)w * h) - 1.0),
           w, h, (size_t)w * h);

    printf("\n consistency across worker counts\n");
    if (!check_worker_consistency(&src, w, h)) {
        printf("\n  REFUSING TO REPORT TIMINGS - output differs by worker "
               "count.\n  Fix correctness first; a faster wrong answer is not "
               "faster.\n");
        free(dst_tiled); free(dst_linear);
        test_frame_free(&tf);
        return;
    }

    printf("\n %-8s %-14s %-9s %-14s %-9s\n",
           "workers", "fused(tiled)", "vs 1thr", "linear(bgra)", "vs 1thr");

    for (i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
        double f = bench_fused(&src, w, h, workers[i], iters, dst_tiled);
        double l = bench_linear(&src, w, h, workers[i], iters, dst_linear);

        if (i == 0) { base_fused = f; base_linear = l; }

        printf(" %-8d %8.2f ms   %5.2fx    %8.2f ms   %5.2fx\n",
               workers[i], f, base_fused / f, l, base_linear / l);
    }

    /*
     * The budget is what actually matters. A converter that takes 20ms is
     * fine at 24fps content and hopeless at 60, and "milliseconds" alone does
     * not say which.
     */
    {
        double f4 = bench_fused(&src, w, h, 4, iters, dst_tiled);
        printf("\n at 4 workers: %.2f ms/frame\n", f4);
        printf("   60fps budget 16.67 ms  -> %s (%.0f%% of budget)\n",
               f4 <= 16.67 ? "fits" : "OVER", 100.0 * f4 / 16.67);
        printf("   30fps budget 33.33 ms  -> %s (%.0f%% of budget)\n",
               f4 <= 33.33 ? "fits" : "OVER", 100.0 * f4 / 33.33);
        printf("   24fps budget 41.67 ms  -> %s (%.0f%% of budget)\n",
               f4 <= 41.67 ? "fits" : "OVER", 100.0 * f4 / 41.67);
    }

    free(dst_tiled);
    free(dst_linear);
    test_frame_free(&tf);
}

/* ---- the 1080p present path ---------------------------------------------- */

/*
 * 1080p never got the fusion 4K got: the fused converter writes YUV -> BGRA ->
 * tiled in one pass straight to the VideoOut plane, but it is gated to UHD
 * sizes. Below that gate the render thread runs three more full-frame passes
 * over 8 MB each, and this measures them separately so the cost of each is a
 * number rather than an argument.
 *
 *   clear   draw_player_screen blacked all 2M pixels ...
 *   copy    ... and pp_playback_copy_display then overwrote every one of them
 *   tile    swizzle the composited frame into the tiled VO plane
 *
 * The clear is the one that was pure waste, and it is gone. The copy is the
 * price of compositing the OSD over the video in linear space: removing it
 * means the converter writing into the VideoOut buffer directly, which is a
 * change of buffer ownership between the decode and render threads and not
 * something to land on host timings alone.
 */
static void run_present_path(uint32_t w, uint32_t h, int iters)
{
    size_t    px    = (size_t)w * h;
    uint32_t *display = malloc(px * 4);          /* pb->display  */
    uint32_t *cpu     = malloc(px * 4);          /* VO cpu_buf ("linear") */
    uint32_t *tiled   = calloc(tiled_plane_pixels(w, h), 4);
    double    t0, t_clear, t_copy, t_tile;
    int       i;

    if (!display || !cpu || !tiled) {
        printf("\n present-path alloc failed\n");
        free(display); free(cpu); free(tiled);
        return;
    }

    for (i = 0; i < (int)px; i++)
        display[i] = 0xFF000000u | (uint32_t)i;

    printf("\n=== %ux%u present path, %d iterations ===\n", w, h, iters);
    printf(" each pass touches %.1f MB\n", px * 4.0 / (1024.0 * 1024.0));

    /*
     * pp_draw_pixels_as_tiles has no worker-count knob, so the consistency
     * trick used above does not apply. Check it against an independent
     * reference instead: the same addresses computed one pixel at a time by
     * pp_tiled_pixel_offset, which the plane hashes above already vouch for.
     * The threaded version splits by rows, and a band split that drops or
     * double-writes a row is exactly what this has to catch.
     */
    {
        size_t    tp  = tiled_plane_pixels(w, h);
        uint32_t *ref = calloc(tp, 4);
        uint32_t  x, yy;

        if (!ref) {
            printf(" reference alloc failed\n");
            free(display); free(cpu); free(tiled);
            return;
        }

        for (yy = 0; yy < h; yy++)
            for (x = 0; x < w; x++)
                ref[pp_tiled_pixel_offset((int)x, (int)yy, (int)w)] =
                    display[(size_t)yy * w + x];

        memset(tiled, 0, tp * 4);
        pp_draw_pixels_as_tiles(display, tiled, (int)w, (int)h);

        printf(" swizzle vs per-pixel reference: %s\n",
               memcmp(ref, tiled, tp * 4) == 0 ? "match" : "*** MISMATCH ***");

        if (memcmp(ref, tiled, tp * 4) != 0) {
            printf("\n  REFUSING TO REPORT TIMINGS - the tiled plane is wrong.\n");
            free(ref); free(display); free(cpu); free(tiled);
            return;
        }
        free(ref);
    }

    /* Warm both destinations so page faults are not in the measurement. */
    memset(cpu, 0, px * 4);
    pp_draw_pixels_as_tiles(cpu, tiled, (int)w, (int)h);

    t0 = now_ms();
    for (i = 0; i < iters; i++) {
        size_t k;
        for (k = 0; k < px; k++)
            cpu[k] = 0xFF000000u;
    }
    t_clear = (now_ms() - t0) / iters;

    t0 = now_ms();
    for (i = 0; i < iters; i++)
        memcpy(cpu, display, px * 4);
    t_copy = (now_ms() - t0) / iters;

    t0 = now_ms();
    for (i = 0; i < iters; i++)
        pp_draw_pixels_as_tiles(cpu, tiled, (int)w, (int)h);
    t_tile = (now_ms() - t0) / iters;

    printf("\n %-28s %8s\n", "stage", "ms/frame");
    printf(" %-28s %8.2f   (removed)\n", "clear to black", t_clear);
    printf(" %-28s %8.2f\n", "copy display -> VO buffer", t_copy);
    printf(" %-28s %8.2f\n", "swizzle -> tiled plane", t_tile);
    printf(" %-28s %8.2f -> %.2f  (%.0f%% less)\n", "render-thread total",
           t_clear + t_copy + t_tile, t_copy + t_tile,
           100.0 * t_clear / (t_clear + t_copy + t_tile));

    free(display);
    free(cpu);
    free(tiled);
}

int main(int argc, char **argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 30;

    if (iters < 1) iters = 1;

    printf("EVO converter benchmark\n");
    printf("-----------------------\n");
    printf("Host timings. Not the console's numbers - use them to compare\n");
    printf("changes against each other, not to predict absolute frame rate.\n");

    run_size(1920, 1080, iters);
    run_size(3840, 2160, iters > 10 ? iters / 3 : iters);
    run_present_path(1920, 1080, iters);

    return 0;
}
