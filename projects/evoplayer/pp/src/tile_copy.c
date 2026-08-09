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

void pp_draw_pixels_as_tiles(uint32_t *src, uint32_t *dst, int frame_width,
                             int frame_height)
{
    PP_DrawChunk chunks[PP_TILE_THREADS];
    pthread_t threads[PP_TILE_THREADS];
    int started = 0;
    int i;

    if (!src || !dst || frame_width <= 0 || frame_height <= 0)
        return;

    /* Split by whole rows so each thread owns complete scanlines - a row never
     * straddles two threads, which keeps the per-row setup out of the inner
     * loop and avoids two threads writing into the same tile. */
    for (i = 0; i < PP_TILE_THREADS; i++) {
        int y0 = (int)(((long)frame_height * i) / PP_TILE_THREADS);
        int y1 = (int)(((long)frame_height * (i + 1)) / PP_TILE_THREADS);

        if (y1 <= y0)
            continue;   /* more threads than rows */

        chunks[started].src         = src;
        chunks[started].dst         = dst;
        chunks[started].frame_width = frame_width;
        chunks[started].y_start     = y0;
        chunks[started].y_end       = y1;

        if (pthread_create(&threads[started], 0,
                           pp_tile_thread, &chunks[started]) == 0) {
            started++;
        } else {
            /* Out of threads: do this slice on the calling thread rather than
             * silently dropping part of the frame. */
            pp_tile_thread(&chunks[started]);
        }
    }

    for (i = 0; i < started; i++)
        pthread_join(threads[i], 0);
}
