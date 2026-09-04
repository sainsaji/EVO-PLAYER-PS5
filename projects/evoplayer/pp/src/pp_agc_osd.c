/* pp_agc_osd.c - see pp_agc_osd.h. #28 Phase 2. */
#include "pp_agc_osd.h"
#include "evo_boot_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/*
 * The render thread publishes a 1920x1080 premultiplied BGRA OSD image. We
 * convert it there (throttled, off the decode thread) into premultiplied YUV +
 * alpha so the decode-thread compose is just a couple of mul-adds per pixel and
 * never round-trips the video through RGB.
 *
 * DOUBLE-BUFFERED: publish fills the back plane set then atomically bumps
 * g_front. The decode-thread compose reads g_front. No window where the OSD
 * blinks off (that was the flicker).
 *
 * Premultiplied-alpha "over": out = src + dst*(255-a)/255, and Y/Cb/Cr are all
 * linear in RGB, so the blend works directly in YUV; g_cb/g_cr carry the "+128"
 * chroma bias scaled by alpha. BT.709 full-range, matching pp_agc.c's
 * pixel_constants (the sceAgc NV12->RGB shader). Fixed point <<8.
 */
#define W  ((int)PP_AGC_OSD_W)
#define H  ((int)PP_AGC_OSD_H)
#define FX 8

struct plane {
    int16_t *y, *cb, *cr;    /* W*H premultiplied luma / Cb / Cr */
    uint8_t *a;              /* W*H alpha                        */
    int row0, row1;          /* non-transparent row span [row0,row1) */
    int valid;
};
static struct plane g_p[2];
static volatile int g_front = -1;   /* index of the last fully-published plane */

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

int pp_agc_osd_enabled(void)
{
    static int e = -1;
    if (e < 0) {
        e = 0;
        if (getenv("EVO_AGC_OSD")) e = 1;
        else { FILE *f = fopen("/mnt/usb0/evo_agc_osd", "r"); if (f) { e = 1; fclose(f); } }
    }
    return e;
}

static int alloc_plane(struct plane *p)
{
    if (p->y) return 0;
    p->y  = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    p->cb = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    p->cr = (int16_t *)malloc((size_t)W * H * sizeof(int16_t));
    p->a  = (uint8_t *)malloc((size_t)W * H);
    return (p->y && p->cb && p->cr && p->a) ? 0 : -1;
}

void pp_agc_osd_publish(const uint32_t *bgra, int active)
{
    if (!pp_agc_osd_enabled() || !active || !bgra) {
        g_front = -1;
        return;
    }

    int b = (g_front == 0) ? 1 : 0;        /* write the buffer we're not showing */
    struct plane *p = &g_p[b];
    if (alloc_plane(p) != 0) { g_front = -1; return; }

    int r0 = H, r1 = 0;
    for (int r = 0; r < H; r++) {
        const uint32_t *row = bgra + (size_t)r * W;
        int16_t *yr = p->y + (size_t)r * W, *cbr = p->cb + (size_t)r * W, *crr = p->cr + (size_t)r * W;
        uint8_t *ar = p->a + (size_t)r * W;
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
            yr[c]  = (int16_t)((54 * pr + 183 * pg + 19 * pb + 128) >> FX);        /* .2126/.7152/.0722 */
            cbr[c] = (int16_t)(((-29 * pr - 99 * pg + 128 * pb + 128) >> FX) + 128 * a / 255);
            crr[c] = (int16_t)(((128 * pr - 116 * pg - 12 * pb + 128) >> FX) + 128 * a / 255);
        }
        if (any) { if (r < r0) r0 = r; r1 = r + 1; }
    }

    if (r1 <= r0) { g_front = -1; return; }   /* fully transparent = no OSD */
    p->row0 = r0;
    p->row1 = r1;
    p->valid = 1;
    __sync_synchronize();
    g_front = b;
}

int pp_agc_osd_active(void)
{
    return g_front >= 0;
}

const uint8_t *pp_agc_osd_compose(const void *vid_nv12, uint32_t pitch, uint32_t vh,
                                  uint32_t ow, uint32_t oh)
{
    static uint8_t *out;
    int f = g_front;
    if (f < 0 || !vid_nv12 || !pitch || !vh || !ow || !oh)
        return 0;
    const struct plane *p = &g_p[f];
    if (!p->valid || !p->y)
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

    int r0 = p->row0 & ~1, r1 = (p->row1 + 1) & ~1;
    if (r0 < 0) r0 = 0;
    if (r1 > H) r1 = H;

    /* rows above / below the OSD band: video luma verbatim */
    for (int r = 0; r < H; r++) {
        if (r == r0) { r = r1 - 1; continue; }
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr = vy + (size_t)vsy * pitch;
        uint8_t *oyr = oy + (size_t)r * W;
        for (int c = 0; c < W; c++) { uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1; oyr[c] = vyr[vsx]; }
    }

    /* OSD band: premultiplied blend in YUV */
    for (int r = r0; r < r1; r++) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr = vy + (size_t)vsy * pitch;
        const int16_t *yr = p->y + (size_t)r * W;
        const uint8_t *ar = p->a + (size_t)r * W;
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
        const int16_t *cb0 = p->cb + (size_t)r * W, *cr0 = p->cr + (size_t)r * W;
        const int16_t *cb1 = p->cb + (size_t)(r + 1 < H ? r + 1 : r) * W;
        const int16_t *cr1 = p->cr + (size_t)(r + 1 < H ? r + 1 : r) * W;
        const uint8_t *a0 = p->a + (size_t)r * W, *a1 = p->a + (size_t)(r + 1 < H ? r + 1 : r) * W;
        for (int c = 0; c < W; c += 2) {
            uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1;
            int vcb = vuvr[vsx & ~1u], vcr = vuvr[(vsx & ~1u) + 1];
            int c2 = (c + 1 < W) ? c + 1 : c;
            int amax = band ? (a0[c] | a0[c2] | a1[c] | a1[c2]) : 0;
            if (amax == 0) { ouvr[c] = (uint8_t)vcb; ouvr[c + 1] = (uint8_t)vcr; continue; }
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
