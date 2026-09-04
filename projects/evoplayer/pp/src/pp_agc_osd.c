/* pp_agc_osd.c - see pp_agc_osd.h. #28 Phase 2. */
#include "pp_agc_osd.h"

#include <string.h>
#include <stdlib.h>

/* Double-buffered 1920x1080 premultiplied BGRA. Single writer (render thread),
 * single reader (decode thread). The reader may see the previous frame's image
 * for one present - the OSD changes slowly (a ticking clock), so a frame of
 * latency is invisible. */
static uint32_t *g_buf[2];
static volatile int g_ready = -1;     /* index of the last fully-written buffer */
static volatile int g_active;
static volatile unsigned g_gen;

static uint32_t *osd_slot(int i)
{
    if (!g_buf[i]) {
        g_buf[i] = (uint32_t *)malloc((size_t)PP_AGC_OSD_W * PP_AGC_OSD_H * 4u);
        if (g_buf[i])
            memset(g_buf[i], 0, (size_t)PP_AGC_OSD_W * PP_AGC_OSD_H * 4u);
    }
    return g_buf[i];
}

void pp_agc_osd_publish(const uint32_t *bgra, int active)
{
    if (!active || !bgra) {
        g_active = 0;
        return;
    }
    int w = (g_ready == 0) ? 1 : 0;
    uint32_t *dst = osd_slot(w);
    if (!dst) { g_active = 0; return; }
    memcpy(dst, bgra, (size_t)PP_AGC_OSD_W * PP_AGC_OSD_H * 4u);
    g_ready = w;
    g_gen++;
    g_active = 1;
}

int pp_agc_osd_active(void)
{
    return g_active && g_ready >= 0;
}

/* BT.709 full-range, matching pp_agc.c's pixel_constants (the sceAgc NV12->RGB
 * shader coefficients). Fixed point, <<8. */
#define FX 8
static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

const uint8_t *pp_agc_osd_compose(const void *vid_nv12, uint32_t pitch, uint32_t vh,
                                  uint32_t ow, uint32_t oh)
{
    static uint8_t *out;
    if (!pp_agc_osd_active() || !vid_nv12 || !pitch || !vh || !ow || !oh)
        return 0;
    if (!out) {
        out = (uint8_t *)malloc(PP_AGC_OSD_NV12_BYTES);
        if (!out)
            return 0;
    }

    const uint32_t *osd = g_buf[g_ready];
    if (!osd)
        return 0;

    const uint8_t *vy  = (const uint8_t *)vid_nv12;
    const uint8_t *vuv = vy + (size_t)pitch * vh;

    const uint32_t W = PP_AGC_OSD_W, H = PP_AGC_OSD_H;
    uint8_t *oy  = out;
    uint8_t *ouv = out + (size_t)W * H;

    /* nearest-sample scale factors, <<16 */
    uint32_t sxr = (ow << 16) / W;
    uint32_t syr = (oh << 16) / H;

    for (uint32_t r = 0; r < H; r++) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vyr  = vy  + (size_t)vsy * pitch;
        const uint8_t *vuvr = vuv + (size_t)(vsy >> 1) * pitch;
        const uint32_t *osdr = osd + (size_t)r * W;
        uint8_t *oyr = oy + (size_t)r * W;

        for (uint32_t c = 0; c < W; c++) {
            uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1;
            uint32_t px = osdr[c];
            unsigned a = px >> 24;

            if (a == 0) {
                /* no OSD here - copy the video luma verbatim, no round trip */
                oyr[c] = vyr[vsx];
                continue;
            }

            /* video YUV -> RGB */
            int Y  = vyr[vsx];
            int Cb = vuvr[(vsx & ~1u)]     - 128;
            int Cr = vuvr[(vsx & ~1u) + 1] - 128;
            int vr = clamp8(Y + ((403 * Cr) >> FX));                 /* 1.5748 */
            int vg = clamp8(Y - ((48 * Cb) >> FX) - ((120 * Cr) >> FX)); /* .1873/.4681 */
            int vb = clamp8(Y + ((475 * Cb) >> FX));                 /* 1.8556 */

            int sr = (int)(px & 0xff);            /* premultiplied */
            int sg = (int)((px >> 8) & 0xff);
            int sb = (int)((px >> 16) & 0xff);
            int ia = 255 - (int)a;
            int rr = clamp8(sr + vr * ia / 255);
            int rg = clamp8(sg + vg * ia / 255);
            int rb = clamp8(sb + vb * ia / 255);

            oyr[c] = (uint8_t)clamp8(((54 * rr + 183 * rg + 18 * rb) >> FX)); /* .2126/.7152/.0722 */
        }
    }

    /* chroma: for each 2x2 output block, if any of the 4 OSD pixels is
     * non-transparent, recompute from the composited RGB (average the block);
     * otherwise copy the video chroma verbatim. */
    for (uint32_t r = 0; r < H; r += 2) {
        uint32_t vsy = (r * syr) >> 16; if (vsy >= vh) vsy = vh - 1;
        const uint8_t *vuvr = vuv + (size_t)(vsy >> 1) * pitch;
        const uint32_t *osdr0 = osd + (size_t)r * W;
        const uint32_t *osdr1 = osd + (size_t)(r + 1 < H ? r + 1 : r) * W;
        uint8_t *ouvr = ouv + (size_t)(r >> 1) * W;

        for (uint32_t c = 0; c < W; c += 2) {
            uint32_t vsx = (c * sxr) >> 16; if (vsx >= ow) vsx = ow - 1;
            unsigned amax = (osdr0[c] >> 24) | (osdr0[c + (c + 1 < W)] >> 24) |
                            (osdr1[c] >> 24) | (osdr1[c + (c + 1 < W)] >> 24);
            if (amax == 0) {
                ouvr[c]     = vuvr[(vsx & ~1u)];
                ouvr[c + 1] = vuvr[(vsx & ~1u) + 1];
                continue;
            }
            /* recompute the 2x2 average from the composited output RGB. Rather
             * than re-derive RGB, approximate: blend video chroma toward the
             * OSD's colour by the max alpha. Cheap and visually fine under a
             * mostly-opaque OSD. */
            uint32_t px = osdr0[c];
            int sr = (int)(px & 0xff), sg = (int)((px >> 8) & 0xff), sb = (int)((px >> 16) & 0xff);
            unsigned a = px >> 24 ? px >> 24 : amax;
            /* de-premultiply approx for chroma calc */
            if (a) { sr = sr * 255 / (int)a; sg = sg * 255 / (int)a; sb = sb * 255 / (int)a; }
            sr = clamp8(sr); sg = clamp8(sg); sb = clamp8(sb);
            int scb = clamp8(128 + ((-29 * sr - 99 * sg + 128 * sb) >> FX));  /* -.1146/-.3854/.5 */
            int scr = clamp8(128 + ((128 * sr - 116 * sg - 12 * sb) >> FX));  /* .5/-.4542/-.0458 */
            int vcb = vuvr[(vsx & ~1u)];
            int vcr = vuvr[(vsx & ~1u) + 1];
            ouvr[c]     = (uint8_t)clamp8(scb + (vcb - scb) * (255 - (int)a) / 255);
            ouvr[c + 1] = (uint8_t)clamp8(scr + (vcr - scr) * (255 - (int)a) / 255);
        }
    }

    return out;
}
