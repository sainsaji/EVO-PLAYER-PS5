/*
 * evo_ui — themed, antialiased UI primitives.
 *
 * Why SDF rather than the obvious nested loops: a rounded rectangle drawn by
 * testing "is this pixel inside" gives binary coverage and therefore a visible
 * staircase on every curve. Measuring the *distance* to the shape instead
 * makes coverage a continuous value, so a one-pixel transition band falls out
 * for free at any radius. That single change is the difference between the
 * player looking hand-drawn and looking finished.
 */
#include "evo_ui.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------ */

uint32_t evo_ui_blend(uint32_t dst, uint32_t src, int alpha)
{
    if (alpha <= 0)   return dst;
    if (alpha >= 255) return src;

    int inv = 255 - alpha;
    int r = (( src        & 0xFF) * alpha + ( dst        & 0xFF) * inv) / 255;
    int g = (((src >>  8) & 0xFF) * alpha + ((dst >>  8) & 0xFF) * inv) / 255;
    int b = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inv) / 255;

    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

/* Linear interpolation between two packed colours, t in [0,256). */
static uint32_t lerp_colour(uint32_t a, uint32_t b, int t)
{
    int it = 256 - t;
    int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF, aa = (a >> 24) & 0xFF;
    int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = (b >> 24) & 0xFF;

    return (uint32_t)(((aa * it + ba * t) >> 8) & 0xFF) << 24
         | (uint32_t)(((ab * it + bb * t) >> 8) & 0xFF) << 16
         | (uint32_t)(((ag * it + bg * t) >> 8) & 0xFF) << 8
         | (uint32_t)(((ar * it + br * t) >> 8) & 0xFF);
}

/* Signed distance to a rounded box, negative inside. */
static float sd_round_box(float px, float py, float cx, float cy,
                          float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float inside = (qx > qy ? qx : qy);
    return sqrtf(mx * mx + my * my) + (inside < 0.0f ? inside : 0.0f) - r;
}

/* ------------------------------------------------------------------------ */

void evo_ui_vgrad(uint32_t *fb, int x, int y, int w, int h,
                  uint32_t top, uint32_t bottom)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h;
    if (x1 > EVO_UI_W) x1 = EVO_UI_W;
    if (y1 > EVO_UI_H) y1 = EVO_UI_H;
    if (h <= 0) return;

    for (int py = y0; py < y1; py++) {
        int t = ((py - y) * 256) / h;
        uint32_t c = lerp_colour(top, bottom, t);
        uint32_t *row = &fb[py * EVO_UI_W];
        for (int px = x0; px < x1; px++)
            row[px] = c;
    }
}

/*
 * The page background: three cheap layers that together read as designed
 * rather than as a flat fill.
 *
 *   1. vertical gradient          - the theme's base
 *   2. soft accent bloom, top right - somewhere for the eye to rest, and the
 *                                     main thing that gives each theme its
 *                                     character
 *   3. vignette                   - pulls attention back to the content column
 *
 * Deliberately no sqrt: this touches every one of 2M pixels each frame, and
 * quadratic falloff on squared distance looks the same as a linear falloff on
 * true distance once it is this diffuse.
 */
void evo_ui_background(uint32_t *fb)
{
    const evo_theme *th = evo_theme_current();

    const int bloom_cx = (int)(EVO_UI_W * 0.74f);
    const int bloom_cy = (int)(EVO_UI_H * 0.20f);
    const float bloom_r  = EVO_UI_H * 0.95f;
    const float bloom_r2 = bloom_r * bloom_r;
    const unsigned bloom_a = (th->accent_soft >> 24) & 0xFF;

    /* Vignette reaches this far in from the corners. */
    const float vig_r  = EVO_UI_W * 0.78f;
    const float vig_r2 = vig_r * vig_r;
    const int   vig_cx = EVO_UI_W / 2, vig_cy = EVO_UI_H / 2;

    for (int y = 0; y < EVO_UI_H; y++) {
        uint32_t *row = &fb[y * EVO_UI_W];
        const uint32_t base = lerp_colour(th->bg_top, th->bg_bottom,
                                          (y * 256) / EVO_UI_H);
        const int dyb = y - bloom_cy;
        const int dyv = y - vig_cy;
        const int dyb2 = dyb * dyb;
        const int dyv2 = dyv * dyv;

        for (int x = 0; x < EVO_UI_W; x++) {
            uint32_t c = base;

            if (bloom_a) {
                const int dxb = x - bloom_cx;
                const float d2 = (float)(dxb * dxb + dyb2);
                if (d2 < bloom_r2) {
                    float k = 1.0f - d2 / bloom_r2;      /* 0..1 */
                    int a = (int)(k * k * (float)bloom_a);
                    if (a > 0)
                        c = evo_ui_blend(c, th->accent_soft, a);
                }
            }

            const int dxv = x - vig_cx;
            const float vd2 = (float)(dxv * dxv + dyv2);
            if (vd2 > vig_r2 * 0.25f) {
                float k = (vd2 - vig_r2 * 0.25f) / (vig_r2 * 0.75f);
                if (k > 1.0f) k = 1.0f;
                int a = (int)(k * k * 90.0f);
                if (a > 0)
                    c = evo_ui_blend(c, 0xFF000000u, a);
            }

            row[x] = c;
        }
    }
}

void evo_ui_round_rect(uint32_t *fb, int x, int y, int w, int h, int radius,
                       uint32_t fill_top, uint32_t fill_bottom,
                       uint32_t border, int border_px,
                       uint32_t shadow, int shadow_px)
{
    if (w <= 0 || h <= 0) return;

    const float hw = w * 0.5f, hh = h * 0.5f;
    const float cx = x + hw,   cy = y + hh;

    float r = (float)radius;
    float rmax = hw < hh ? hw : hh;
    if (r > rmax) r = rmax;
    if (r < 0)    r = 0;

    const float bw = (float)(border_px > 0 ? border_px : 0);
    const unsigned fa = (fill_top >> 24) & 0xFF;
    const unsigned ba = (border   >> 24) & 0xFF;
    const unsigned sa = (shadow   >> 24) & 0xFF;
    const int sp = shadow_px > 0 ? shadow_px : 0;

    /* The shadow is offset downward: light is assumed to come from above, and
     * a symmetric blur reads as a glow rather than elevation. */
    const int sy_off = sp / 3;

    int x0 = x - sp, x1 = x + w + sp;
    int y0 = y - sp, y1 = y + h + sp + sy_off;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EVO_UI_W) x1 = EVO_UI_W;
    if (y1 > EVO_UI_H) y1 = EVO_UI_H;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = &fb[py * EVO_UI_W];
        const float fy = py + 0.5f;

        /* Gradient position within the card. */
        int t = h > 0 ? ((py - y) * 256) / h : 0;
        if (t < 0) t = 0;
        if (t > 255) t = 255;
        const uint32_t fill = lerp_colour(fill_top, fill_bottom, t);

        for (int px = x0; px < x1; px++) {
            const float fx = px + 0.5f;
            const float d = sd_round_box(fx, fy, cx, cy, hw, hh, r);

            /* --- shadow, outside the shape only --------------------------- */
            if (sa && sp && d > 0.0f) {
                float sd = sd_round_box(fx, fy - (float)sy_off,
                                        cx, cy, hw, hh, r);
                if (sd > 0.0f && sd < (float)sp) {
                    float k = 1.0f - (sd / (float)sp);
                    int a = (int)(k * k * (float)sa);
                    if (a > 0)
                        row[px] = evo_ui_blend(row[px], shadow, a);
                }
            }

            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f)  cov = 1.0f;

            /* Inner shape, inset by the border width. */
            float icov = 0.5f - (d + bw);
            if (icov > 1.0f) icov = 1.0f;
            if (icov < 0.0f) icov = 0.0f;

            if (fa && icov > 0.0f) {
                int a = (int)(icov * (float)fa);
                if (a > 0)
                    row[px] = evo_ui_blend(row[px], fill, a);
            }

            if (ba && bw > 0.0f) {
                float bcov = cov - icov;
                if (bcov > 0.0f) {
                    int a = (int)(bcov * (float)ba);
                    if (a > 0)
                        row[px] = evo_ui_blend(row[px], border, a);
                }
            }
        }
    }
}

void evo_ui_card(uint32_t *fb, int x, int y, int w, int h, int selected)
{
    const evo_theme *th = evo_theme_current();

    const uint32_t ftop = selected ? th->surface_sel     : th->surface;
    const uint32_t fbot = selected ? th->surface_sel_alt : th->surface_alt;
    const uint32_t bord = selected ? th->border_sel      : th->border;

    /* A selected card is lifted further off the page. */
    const int shadow_px = selected ? th->shadow_px + 6 : th->shadow_px;

    evo_ui_round_rect(fb, x, y, w, h, th->radius,
                      ftop, fbot, bord, th->border_px,
                      th->shadow, shadow_px);

    if (!selected || th->rail_px <= 0)
        return;

    /*
     * Accent rail down the left edge.
     *
     * Drawn as a rounded bar inset into the card rather than a straight strip,
     * so it follows the card's own corner radius instead of poking out of it.
     */
    const int inset = 10;
    const int rail_h = h - inset * 2;
    const int rail_w = th->rail_px;
    if (rail_h <= 0)
        return;

    evo_ui_round_rect(fb, x + inset, y + inset, rail_w, rail_h,
                      rail_w / 2,
                      th->accent, th->accent,
                      0, 0, 0, 0);
}

void evo_ui_circle(uint32_t *fb, int cx, int cy, int r, uint32_t colour)
{
    if (r <= 0) return;
    const unsigned ca = (colour >> 24) & 0xFF;

    int x0 = cx - r - 1, x1 = cx + r + 1;
    int y0 = cy - r - 1, y1 = cy + r + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EVO_UI_W) x1 = EVO_UI_W;
    if (y1 > EVO_UI_H) y1 = EVO_UI_H;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = &fb[py * EVO_UI_W];
        for (int px = x0; px < x1; px++) {
            float d = sqrtf((px + 0.5f - cx) * (px + 0.5f - cx) +
                            (py + 0.5f - cy) * (py + 0.5f - cy)) - (float)r;
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f)  cov = 1.0f;
            int a = (int)(cov * (float)ca);
            if (a > 0)
                row[px] = evo_ui_blend(row[px], colour, a);
        }
    }
}

void evo_ui_hline(uint32_t *fb, int x, int y, int w, uint32_t colour)
{
    if (y < 0 || y >= EVO_UI_H) return;
    int x0 = x < 0 ? 0 : x, x1 = x + w;
    if (x1 > EVO_UI_W) x1 = EVO_UI_W;
    const unsigned a = (colour >> 24) & 0xFF;
    uint32_t *row = &fb[y * EVO_UI_W];
    for (int px = x0; px < x1; px++)
        row[px] = evo_ui_blend(row[px], colour, a);
}

void evo_ui_vline(uint32_t *fb, int x, int y, int h, uint32_t colour)
{
    if (x < 0 || x >= EVO_UI_W) return;
    int y0 = y < 0 ? 0 : y, y1 = y + h;
    if (y1 > EVO_UI_H) y1 = EVO_UI_H;
    const unsigned a = (colour >> 24) & 0xFF;
    for (int py = y0; py < y1; py++)
        fb[py * EVO_UI_W + x] = evo_ui_blend(fb[py * EVO_UI_W + x], colour, a);
}
