#include "evo_draw.h"

#include "evo_metrics.h"   /* EVO_SCREEN_W/H, for the marquee bleed band */

#include <stdio.h>
#include <string.h>

static const evo_draw_vtable *g_dv;

void evo_draw_bind(const evo_draw_vtable *v) { g_dv = v; }

/*
 * Measured ink bounds of the glyph atlas: the first and last row inside each
 * glyph box that actually carries ink, descenders included. These were read
 * out of the atlas rather than estimated, because estimating is how the
 * previous UI ended up drawing subtitles past the bottom of their cards.
 *
 *   face 0 (SMALL)  rows  6..20 of 25
 *   face 1 (SUB)    rows  7..23 of 29
 *   face 2 (MENU)   rows  9..41 of 46
 *   face 3 (TITLE)  rows 11..52 of 57
 */
static const int INK_TOP[4] = {  6,  7,  9, 11 };
static const int INK_BOT[4] = { 20, 23, 41, 52 };

static int face_clamp(int f) { return (f < 0) ? 0 : (f > 3 ? 3 : f); }

int evo_face_ink_h(evo_face face)
{
    int f = face_clamp((int)face);
    return INK_BOT[f] - INK_TOP[f] + 1;
}

char evo_text_unsupported(const char *s)
{
    if (!s) return 0;

    for (; *s; s++)
        if (!strchr(EVO_TEXT_CHARSET, *s))
            return *s;

    return 0;
}

/* ---- primitives -------------------------------------------------------- */

void evo_text(uint32_t *fb, int x, int y, const char *s,
              uint32_t colour, evo_face face)
{
    if (g_dv && g_dv->text && s && *s)
        g_dv->text(fb, x, y, s, colour, (int)face);
}

int evo_text_w(const char *s, evo_face face)
{
    if (!g_dv || !g_dv->text_w || !s) return 0;
    return g_dv->text_w(s, (int)face);
}

void evo_icon(uint32_t *fb, int x, int y, int index)
{
    if (g_dv && g_dv->icon) g_dv->icon(fb, x, y, index);
}

void evo_icon_tinted(uint32_t *fb, int x, int y, int index, uint32_t tint)
{
    if (g_dv && g_dv->icon_tinted) g_dv->icon_tinted(fb, x, y, index, tint);
}

void evo_glyph(uint32_t *fb, int x, int y, int index)
{
    if (g_dv && g_dv->glyph) g_dv->glyph(fb, x, y, index);
}

void evo_glyph_tinted(uint32_t *fb, int x, int y, int index, uint32_t tint)
{
    if (g_dv && g_dv->glyph_tinted) g_dv->glyph_tinted(fb, x, y, index, tint);
    else                            evo_glyph(fb, x, y, index);
}

/* ---- alignment --------------------------------------------------------- */

void evo_text_right(uint32_t *fb, int right_x, int y, const char *s,
                    uint32_t colour, evo_face face)
{
    evo_text(fb, right_x - evo_text_w(s, face), y, s, colour, face);
}

void evo_text_centre(uint32_t *fb, int centre_x, int y, const char *s,
                     uint32_t colour, evo_face face)
{
    evo_text(fb, centre_x - evo_text_w(s, face) / 2, y, s, colour, face);
}

/*
 * Longest prefix of `s` whose width plus the ellipsis fits in `max_w`.
 * Copies into `out` and returns its width.
 */
static int fit_prefix(char *out, size_t out_sz, const char *s,
                      int max_w, evo_face face)
{
    size_t n = strlen(s);
    size_t i;

    if (out_sz < 4) { if (out_sz) out[0] = 0; return 0; }

    if (evo_text_w(s, face) <= max_w) {
        size_t copy = (n < out_sz - 1) ? n : out_sz - 1;
        memcpy(out, s, copy);
        out[copy] = 0;
        return evo_text_w(out, face);
    }

    /* Walk back from the full string. The atlas is proportional, so there is
     * no arithmetic shortcut; strings are short enough that this is free next
     * to the per-pixel blending that follows. */
    for (i = (n < out_sz - 3) ? n : out_sz - 3; i > 0; i--) {
        memcpy(out, s, i);
        out[i]     = '.';
        out[i + 1] = '.';
        out[i + 2] = 0;

        if (evo_text_w(out, face) <= max_w)
            return evo_text_w(out, face);
    }

    out[0] = 0;
    return 0;
}

int evo_text_fit(uint32_t *fb, int x, int y, int max_w, const char *s,
                 uint32_t colour, evo_face face)
{
    char buf[256];
    int w;

    if (!s || !*s || max_w <= 0) return 0;

    w = fit_prefix(buf, sizeof(buf), s, max_w, face);
    evo_text(fb, x, y, buf, colour, face);
    return w;
}


/*
 * Full glyph-box heights per face, as laid out in the atlas. INK_TOP/INK_BOT
 * above describe where the ink sits inside these boxes.
 */
static const int FACE_BOX_H[4] = { 25, 29, 46, 57 };

/*
 * A marquee draws one partial glyph past each end of its box. These keep the
 * pixels either side so they can be restored afterwards, which clips the
 * overhang without a clipping blitter.
 *
 * Sized for the widest advance in the largest face with room to spare - the
 * overhang either side is bounded by one glyph advance. The shift is clamped
 * to it too, so an atlas whose glyphs outgrew this could only make the
 * marquee step rather than glide; it could not overrun the buffers.
 */
#define EVO_MARQUEE_BLEED   72
#define EVO_MARQUEE_BAND_H  64

static uint32_t g_marquee_left[EVO_MARQUEE_BLEED * EVO_MARQUEE_BAND_H];
static uint32_t g_marquee_right[EVO_MARQUEE_BLEED * EVO_MARQUEE_BAND_H];

/* Clamp the band to the framebuffer and hand back the usable rectangle. */
static int marquee_band(int x, int y, int max_w, int box_h,
                        int *lx, int *rx, int *y0, int *rows)
{
    if (box_h > EVO_MARQUEE_BAND_H) box_h = EVO_MARQUEE_BAND_H;

    *y0 = y;
    if (*y0 < 0) *y0 = 0;

    *rows = y + box_h - *y0;
    if (*y0 + *rows > EVO_SCREEN_H) *rows = EVO_SCREEN_H - *y0;
    if (*rows < 0) *rows = 0;

    *lx = x - EVO_MARQUEE_BLEED;
    *rx = x + max_w;

    return *rows > 0;
}

static void marquee_bleed_save(uint32_t *fb, int x, int y, int max_w,
                               int box_h)
{
    int lx, rx, y0, rows, r, c;

    if (!fb || !marquee_band(x, y, max_w, box_h, &lx, &rx, &y0, &rows))
        return;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < EVO_MARQUEE_BLEED; c++) {
            int sx = lx + c;

            g_marquee_left[r * EVO_MARQUEE_BLEED + c] =
                (sx >= 0 && sx < EVO_SCREEN_W)
                    ? fb[(y0 + r) * EVO_SCREEN_W + sx] : 0;

            sx = rx + c;

            g_marquee_right[r * EVO_MARQUEE_BLEED + c] =
                (sx >= 0 && sx < EVO_SCREEN_W)
                    ? fb[(y0 + r) * EVO_SCREEN_W + sx] : 0;
        }
    }
}

static void marquee_bleed_restore(uint32_t *fb, int x, int y, int max_w,
                                  int box_h)
{
    int lx, rx, y0, rows, r, c;

    if (!fb || !marquee_band(x, y, max_w, box_h, &lx, &rx, &y0, &rows))
        return;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < EVO_MARQUEE_BLEED; c++) {
            int sx = lx + c;

            if (sx >= 0 && sx < EVO_SCREEN_W)
                fb[(y0 + r) * EVO_SCREEN_W + sx] =
                    g_marquee_left[r * EVO_MARQUEE_BLEED + c];

            sx = rx + c;

            if (sx >= 0 && sx < EVO_SCREEN_W)
                fb[(y0 + r) * EVO_SCREEN_W + sx] =
                    g_marquee_right[r * EVO_MARQUEE_BLEED + c];
        }
    }
}

void evo_text_marquee(uint32_t *fb, int x, int y, int max_w, const char *s,
                      uint32_t colour, evo_face face, int phase)
{
    /*
     * Pixels of travel per *second*, not per frame. This was frame-counted
     * and the result was a marquee that changed pace with the scene: the
     * render loop runs anywhere from 36fps with a preview decoding to 60fps
     * on a settled list, so the same filename scrolled at two visibly
     * different speeds. Real time is the only stable basis.
     *
     * 180px/s crosses a long filename in a couple of seconds - brisk enough
     * not to read as broken, slow enough to stay legible while moving.
     */
    const int speed_px_s = 180;

    int full_w;
    int overflow;
    int hold   = 600;  /* ms parked at each end */
    int travel;        /* ms to cross the overflow */
    int cycle;
    int p;
    int shift  = 0;

    if (!s || !*s || max_w <= 0) return;

    full_w = evo_text_w(s, face);

    if (full_w <= max_w) {
        evo_text(fb, x, y, s, colour, face);
        return;
    }

    /*
     * Ping-pong rather than wrap-around. A wrapping marquee needs the string
     * drawn twice with a clip, and without a clip in this blitter the second
     * copy would spill across the inspector panel.
     */
    overflow = full_w - max_w;
    travel   = overflow * 1000 / speed_px_s;
    if (travel < 1) travel = 1;
    cycle    = hold * 2 + travel * 2;
    p        = phase % cycle;

    if (p < hold)                     shift = 0;
    else if (p < hold + travel)       shift = (p - hold) * speed_px_s / 1000;
    else if (p < hold * 2 + travel)   shift = overflow;
    else                              shift = overflow -
                                              (p - hold * 2 - travel) *
                                              speed_px_s / 1000;

    if (shift < 0)        shift = 0;
    if (shift > overflow) shift = overflow;

    /*
     * Rendering the shift.
     *
     * Dropping whole characters until their combined advance reaches `shift`
     * is how this used to work, and it is why the scroll was still not smooth
     * after being put on a real clock: the offset was computed in pixels but
     * applied in glyphs, so the text sat still and then jumped a whole
     * character width. Whole characters are still dropped - that is what
     * keeps the run short - but the sub-character remainder is applied as a
     * negative x, which is what actually moves it a pixel at a time.
     *
     * That leaves a partial glyph hanging past each end of the box, and there
     * is no clipping primitive to trim it. The bleed is bounded by one glyph
     * advance, so the cheap fix is to keep the pixels either side, draw over
     * them, and put them back - rather than build a clipping blitter for it.
     */
    {
        char        buf[256];
        const char *cursor = s;
        int         dropped = 0;
        int         rem;
        int         box_h;
        int         used = 0;
        int         n = 0;

        while (*cursor) {
            char one[2];
            int  adv;

            one[0] = *cursor;
            one[1] = 0;
            adv = evo_text_w(one, face);

            if (dropped + adv > shift) break;
            dropped += adv;
            cursor++;
        }

        rem = shift - dropped;
        if (rem < 0) rem = 0;
        if (rem > EVO_MARQUEE_BLEED) rem = EVO_MARQUEE_BLEED;

        /*
         * Take glyphs while they still *start* inside the box. Drawing the
         * whole remaining string would run it off the right of the screen on
         * a long name, and every pixel of that is blended and thrown away.
         *
         * The test is on where a glyph starts, not on where the run ends: a
         * glyph that begins past the right edge contributes nothing visible
         * but does put ink beyond the strip that gets restored, so it escapes
         * the clip. Stopping at the first one bounds the overhang to a single
         * glyph advance, which is what EVO_MARQUEE_BLEED is sized for.
         */
        while (cursor[n] && n < (int)sizeof(buf) - 1) {
            char one[2];

            if (used >= max_w + rem) break;

            one[0] = cursor[n];
            one[1] = 0;
            used += evo_text_w(one, face);
            n++;
        }

        memcpy(buf, cursor, (size_t)n);
        buf[n] = 0;

        box_h = FACE_BOX_H[face_clamp((int)face)];

        marquee_bleed_save(fb, x, y, max_w, box_h);
        evo_text(fb, x - rem, y, buf, colour, face);
        marquee_bleed_restore(fb, x, y, max_w, box_h);
    }
}

/* ---- vertical placement ------------------------------------------------ */

int evo_text_y_centred(int box_y, int box_h, evo_face face)
{
    int f     = face_clamp((int)face);
    int ink_h = INK_BOT[f] - INK_TOP[f] + 1;

    return box_y + (box_h - ink_h) / 2 - INK_TOP[f];
}

void evo_text_y_stacked(int box_y, int box_h, evo_face top_face,
                        evo_face bottom_face, int gap,
                        int *out_top_y, int *out_bottom_y)
{
    int tf     = face_clamp((int)top_face);
    int bf     = face_clamp((int)bottom_face);
    int top_h  = INK_BOT[tf] - INK_TOP[tf] + 1;
    int bot_h  = INK_BOT[bf] - INK_TOP[bf] + 1;
    int group  = top_h + gap + bot_h;
    int ink_y  = box_y + (box_h - group) / 2;

    if (out_top_y)    *out_top_y    = ink_y - INK_TOP[tf];
    if (out_bottom_y) *out_bottom_y = ink_y + top_h + gap - INK_TOP[bf];
}
