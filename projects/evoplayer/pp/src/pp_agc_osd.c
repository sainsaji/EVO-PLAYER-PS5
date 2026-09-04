/* pp_agc_osd.c - see pp_agc_osd.h. #28 Phase 2. */
#include "pp_agc_osd.h"
#include "evo_boot_log.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

/*
 * The render thread publishes a 1920x1080 premultiplied BGRA OSD image. We
 * convert it there (throttled, off the decode thread) into premultiplied YUV +
 * alpha so the decode-thread compose is just a couple of mul-adds per pixel and
 * never round-trips the video through RGB.
 *
 * Premultiplied-alpha "over": out = src + dst*(255-a)/255, and Y/Cb/Cr are all
 * linear in RGB, so the blend works directly in YUV. src here carries the
 * matrix constant offsets scaled by a (the "+128" chroma bias, "+0" luma).
 *
 * BT.709 full-range, matching pp_agc.c's pixel_constants (the sceAgc NV12->RGB
 * shader). Fixed point <<8.
 */
#define W  ((int)PP_AGC_OSD_W)
#define H  ((int)PP_AGC_OSD_H)
#define FX 8

static int16_t *g_y;      /* W*H  premultiplied luma            */
static int16_t *g_cb;     /* W*H  premultiplied Cb (incl 128*a) */
static int16_t *g_cr;     /* W*H  premultiplied Cr              */
static uint8_t *g_a;      /* W*H  alpha                         */
static volatile int g_active;
static int g_row0, g_row1;   /* non-transparent row span [row0,row1) */

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int alloc_planes(void)
{
    if (g_y) return 0;
    g_y  = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    g_cb = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    g_cr = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    g_a  = (uint8_t *)malloc((size_t)W * H);
    if (!g_y || !g_cb || !g_cr || !g_a) return -1;
    return 0;
}

void pp_agc_osd_publish(const uint32_t *bgra, int active)
{
    if (!active || !bgra || alloc_planes() != 0) {
        g_active = 0;
        return;
    }

    /* No lock with the decode-thread reader: drop active while the planes are
     * being rewritten so a mid-write compose just skips the overlay for one
     * frame rather than reading a torn image. */
    g_active = 0;

    int r0 = H, r1 = 0;
    for (int r = 0; r < H; r++) {
        const uint32_t *row = bgra + (size_t)r * W;
        int16_t *yr = g_y + (size_t)r * W, *cbr = g_cb + (size_t)r * W, *crr = g_cr + (size_t)r * W;
        uint8_t *ar = g_a + (size_t)r * W;
        int any = 0;
        for (int c = 0; c < W; c++) {
            uint32_t px = row[c];
            int a = (int)(px >> 24);
            ar[c] = (uint8_t)a;
            if (a == 0) { yr[c] = cbr[c] = crr[c] = 0; continue; }
            any = 1;
            int pr = (int)(px & 0xff);          /* premultiplied R,G,B */
            int pg = (int)((px >> 8) & 0xff);
            int pb = (int)((px >> 16) & 0xff);
            yr[c]  = (int16_t)((54 * pr + 183 * pg + 18 * pb) >> FX);              /* .2126/.7152/.0722 */
            cbr[c] = (int16_t)(((-29 * pr - 99 * pg + 128 * pb) >> FX) + 128 * a / 255);
            crr[c] = (int16_t)(((128 * pr - 116 * pg - 12 * pb) >> FX) + 128 * a / 255);
        }
        if (any) { if (r < r0) r0 = r; r1 = r + 1; }
    }

    if (r1 <= r0) { g_active = 0; return; }
    g_row0 = r0;
    g_row1 = r1;
    g_active = 1;
}

int pp_agc_osd_active(void)
{
    return g_active;
}

const uint8_t *pp_agc_osd_compose(const void *vid_nv12, uint32_t pitch, uint32_t vh,
                                  uint32_t ow, uint32_t oh)
{
    static uint8_t *out;
    if (!g_active || !vid_nv12 || !pitch || !vh || !ow || !oh || !g_y)
        return 0;
    if (!out) {
        out = (uint8_t *)malloc(PP_AGC_OSD_NV12_BYTES);
        if (!out) return 0;
    }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    const uint8_t *vy  = (const uint8_t *)vid_nv12;
    const uint8_t *vuv = vy + (size_t)pitch * vh;
    uint8_t *oy  = out;
    uint8_t *ouv = out + (size_t)W * H;

    uint32_t sxr = (ow << 16) / (uint32_t)W;
    uint32_t syr = (oh << 16) / (uint32_t)H;

    int r0 = g_row0 & ~1, r1 = (g_row1 + 1) & ~1;
    if (r0 < 0) r0 = 0;
    if (r1 > H) r1 = H;

    /* rows above / below the OSD band: video luma verbatim */
    for (int r = 0; r < r0; r++) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr = vy + (size_t)vsy * pitch;
        uint8_t *oyr = oy + (size_t)r * W;
        for (int c = 0; c < W; c++) { uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1; oyr[c] = vyr[vsx]; }
    }
    for (int r = r1; r < H; r++) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr = vy + (size_t)vsy * pitch;
        uint8_t *oyr = oy + (size_t)r * W;
        for (int c = 0; c < W; c++) { uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1; oyr[c] = vyr[vsx]; }
    }

    /* OSD band: premultiplied blend in YUV */
    for (int r = r0; r < r1; r++) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr = vy + (size_t)vsy * pitch;
        const int16_t *yr = g_y + (size_t)r * W;
        const uint8_t *ar = g_a + (size_t)r * W;
        uint8_t *oyr = oy + (size_t)r * W;
        for (int c = 0; c < W; c++) {
            uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1;
            int a = ar[c];
            if (a == 0) { oyr[c] = vyr[vsx]; continue; }
            int ia = 255 - a;
            oyr[c] = (uint8_t)clamp8(yr[c] + ((vyr[vsx] * ia * 257) >> 16));
        }
    }

    /* chroma: rows above / below -> video verbatim; band -> blend */
    for (int r = 0; r < H; r += 2) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vuvr = vuv + (size_t)(vsy >> 1) * pitch;
        uint8_t *ouvr = ouv + (size_t)(r >> 1) * W;
        int band = (r >= r0 && r < r1);
        const int16_t *cb0 = g_cb + (size_t)r * W, *cr0 = g_cr + (size_t)r * W;
        const int16_t *cb1 = g_cb + (size_t)(r + 1 < H ? r + 1 : r) * W;
        const int16_t *cr1 = g_cr + (size_t)(r + 1 < H ? r + 1 : r) * W;
        const uint8_t *a0 = g_a + (size_t)r * W, *a1 = g_a + (size_t)(r + 1 < H ? r + 1 : r) * W;
        for (int c = 0; c < W; c += 2) {
            uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1;
            int vcb = vuvr[vsx & ~1u], vcr = vuvr[(vsx & ~1u) + 1];
            int c2 = (c + 1 < W) ? c + 1 : c;
            int amax = band ? (a0[c] | a0[c2] | a1[c] | a1[c2]) : 0;
            if (amax == 0) { ouvr[c] = (uint8_t)vcb; ouvr[c + 1] = (uint8_t)vcr; continue; }
            /* premultiplied "over" in YUV: out = src_premul + dst*(255-a)/255.
             * g_cb/g_cr already carry the 128 bias scaled by a; average the 2x2. */
            int scb = (cb0[c] + cb0[c2] + cb1[c] + cb1[c2]) >> 2;
            int scr = (cr0[c] + cr0[c2] + cr1[c] + cr1[c2]) >> 2;
            int aa  = (a0[c] + a0[c2] + a1[c] + a1[c2]) >> 2;
            int ia = 255 - aa;
            ouvr[c]     = (uint8_t)clamp8(scb + ((vcb * ia * 257) >> 16));
            ouvr[c + 1] = (uint8_t)clamp8(scr + ((vcr * ia * 257) >> 16));
        }
    }

    {
        struct timespec ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        uint64_t us = (uint64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000ull
                    + (ts1.tv_nsec - ts0.tv_nsec) / 1000ull;
        static uint64_t sum, mx; static uint32_t n;
        sum += us; if (us > mx) mx = us; n++;
        if (n == 120) {
            evo_boot_log("pp_agc_osd: compose n=%u avg=%lluus max=%lluus band=[%d,%d)",
                         n, (unsigned long long)(sum / n), (unsigned long long)mx, r0, r1);
            evo_boot_log_flush();
            sum = mx = 0; n = 0;
        }
    }

    return out;
}
