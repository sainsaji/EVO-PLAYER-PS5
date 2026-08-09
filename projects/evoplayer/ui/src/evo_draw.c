#include "evo_draw.h"

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

void evo_text_marquee(uint32_t *fb, int x, int y, int max_w, const char *s,
                      uint32_t colour, evo_face face, int phase)
{
    int full_w;
    int overflow;
    int hold   = 48;   /* frames parked at each end */
    int travel;
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
    travel   = overflow * 2;          /* 2 frames per pixel: readable pace */
    cycle    = hold * 2 + travel * 2;
    p        = phase % cycle;

    if (p < hold)                     shift = 0;
    else if (p < hold + travel)       shift = (p - hold) / 2;
    else if (p < hold * 2 + travel)   shift = overflow;
    else                              shift = overflow -
                                              (p - hold * 2 - travel) / 2;

    if (shift < 0)        shift = 0;
    if (shift > overflow) shift = overflow;

    /*
     * No clipping primitive exists, so the tail is truncated by fitting the
     * shifted substring instead of translating the whole run off its box.
     * Advance-accurate because it measures the same way it draws.
     */
    {
        char buf[256];
        const char *cursor = s;
        int dropped = 0;

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

        snprintf(buf, sizeof(buf), "%s", cursor);
        evo_text_fit(fb, x, y, max_w, buf, colour, face);
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
