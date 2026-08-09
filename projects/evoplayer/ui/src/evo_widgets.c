#include "evo_widgets.h"
#include "evo_metrics.h"
#include "evo_theme.h"
#include "evo_ui.h"

#include <string.h>

/* ---- shared helpers ------------------------------------------------------ */

static uint32_t with_alpha(uint32_t c, int a)
{
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

static uint32_t mix(uint32_t a, uint32_t b, int t)
{
    int ar =  a        & 0xFF, ag = (a >>  8) & 0xFF;
    int ab = (a >> 16) & 0xFF, aa = (a >> 24) & 0xFF;
    int br =  b        & 0xFF, bg = (b >>  8) & 0xFF;
    int bb = (b >> 16) & 0xFF, ba = (b >> 24) & 0xFF;

    int r  = ar + ((br - ar) * t) / 255;
    int g  = ag + ((bg - ag) * t) / 255;
    int bl = ab + ((bb - ab) * t) / 255;
    int al = aa + ((ba - aa) * t) / 255;

    return ((uint32_t)al << 24) | ((uint32_t)bl << 16) |
           ((uint32_t)g  <<  8) |  (uint32_t)r;
}

/*
 * Blit a BGRA source into a destination rect, scaled.
 *
 * Bilinear when magnifying, point-sampled when the scale is 1:1 or smaller.
 * Nearest-neighbour magnification is what made previews look blocky: the
 * inspector panel is 560px wide and sources are often smaller, so every
 * source pixel became a visible square. Bilinear costs three extra lerps per
 * pixel over an area that is a few hundred pixels across - nothing next to
 * the per-pixel blending the rest of the frame already does.
 *
 * Minification is left point-sampled deliberately. Averaging on the way down
 * belongs where the thumbnail is produced, not here: doing it per frame would
 * pay for the same filter sixty times a second to get the same answer. See
 * prospero_cover_scale_rgba().
 */
static uint32_t lerp_px(uint32_t a, uint32_t b, int t)
{
    int ar =  a        & 0xFF, ag = (a >>  8) & 0xFF, ab = (a >> 16) & 0xFF;
    int br =  b        & 0xFF, bg = (b >>  8) & 0xFF, bb = (b >> 16) & 0xFF;

    return 0xFF000000u
         | ((uint32_t)(ab + (((bb - ab) * t) >> 8)) << 16)
         | ((uint32_t)(ag + (((bg - ag) * t) >> 8)) <<  8)
         |  (uint32_t)(ar + (((br - ar) * t) >> 8));
}

static void blit_cover(uint32_t *fb, int dx, int dy, int dw, int dh,
                       const uint32_t *src, int sw, int sh, int alpha)
{
    int x, y;
    int magnify;

    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    magnify = (dw > sw || dh > sh);

    for (y = 0; y < dh; y++) {
        int fy = dy + y;
        int sy, fyf;

        if (fy < 0 || fy >= EVO_SCREEN_H) continue;

        /* 8-bit sub-pixel position within the source. */
        {
            int syq = (y * sh * 256) / dh;
            sy  = syq >> 8;
            fyf = syq & 0xFF;
            if (sy >= sh - 1) { sy = sh - 1; fyf = 0; }
        }

        for (x = 0; x < dw; x++) {
            int fx = dx + x;
            int sx, fxf;
            uint32_t px;

            if (fx < 0 || fx >= EVO_SCREEN_W) continue;

            {
                int sxq = (x * sw * 256) / dw;
                sx  = sxq >> 8;
                fxf = sxq & 0xFF;
                if (sx >= sw - 1) { sx = sw - 1; fxf = 0; }
            }

            if (magnify && (fxf || fyf)) {
                const uint32_t *r0 = src + (size_t)sy * sw;
                const uint32_t *r1 = r0 + (fyf ? sw : 0);
                int             nx = fxf ? 1 : 0;

                px = lerp_px(lerp_px(r0[sx], r0[sx + nx], fxf),
                             lerp_px(r1[sx], r1[sx + nx], fxf), fyf);
            } else {
                px = src[(size_t)sy * sw + sx];
            }

            if (alpha >= 255) {
                fb[fy * EVO_SCREEN_W + fx] = px;
            } else {
                fb[fy * EVO_SCREEN_W + fx] =
                    evo_ui_blend(fb[fy * EVO_SCREEN_W + fx], px, alpha);
            }
        }
    }
}

/* ---- list row ------------------------------------------------------------ */

void evo_widget_row_highlight(uint32_t *fb, int x, int y, int w, int h)
{
    evo_ui_card(fb, x, y, w, h, 1);
}

void evo_widget_row(uint32_t *fb, int x, int y, int w, int h,
                    const evo_row *r)
{
    const evo_theme *th = evo_theme_current();
    int text_x = x + EVO_ROW_PAD_X;
    int text_r = x + w - EVO_ROW_PAD_X;
    int title_y, detail_y;

    if (!r) return;

    if (r->icon >= 0) {
        evo_icon_tinted(fb, x + EVO_ROW_PAD_X,
                        y + (h - EVO_ROW_ICON) / 2 - 8,
                        r->icon,
                        r->selected ? th->accent
                                    : mix(th->text_muted, th->accent, 110));
        text_x = x + EVO_ROW_TEXT_X;
    }

    if (r->chevron) text_r -= 56;
    if (r->badge_icon >= 0) text_r -= 48;
    if (r->swatches && r->swatch_count > 0)
        text_r -= r->swatch_count * (EVO_SWATCH_W + EVO_SWATCH_GAP) + 12;

    if (r->detail && *r->detail) {
        evo_text_y_stacked(y, h, EVO_FACE_MENU, EVO_FACE_SUB, 10,
                           &title_y, &detail_y);
    } else {
        title_y  = evo_text_y_centred(y, h, EVO_FACE_MENU);
        detail_y = 0;
    }

    {
        uint32_t title_col = r->info ? th->text_secondary : th->text_primary;

        if (r->selected && r->marquee_phase > 0)
            evo_text_marquee(fb, text_x, title_y, text_r - text_x, r->title,
                             title_col, EVO_FACE_MENU, r->marquee_phase);
        else
            evo_text_fit(fb, text_x, title_y, text_r - text_x, r->title,
                         title_col, EVO_FACE_MENU);
    }

    if (r->detail && *r->detail) {
        uint32_t detail_col = r->info
            ? th->text_primary
            : (r->selected ? th->text_secondary : th->text_muted);

        evo_text_fit(fb, text_x, detail_y, text_r - text_x, r->detail,
                     detail_col, EVO_FACE_SUB);
    }

    if (r->swatches && r->swatch_count > 0) {
        int sw_total = r->swatch_count * (EVO_SWATCH_W + EVO_SWATCH_GAP)
                       - EVO_SWATCH_GAP;
        int sx = x + w - (r->chevron ? 68 : EVO_ROW_PAD_X) - sw_total;
        int sy = y + (h - EVO_SWATCH_H) / 2;
        int i;

        for (i = 0; i < r->swatch_count; i++) {
            evo_ui_round_rect(fb, sx, sy, EVO_SWATCH_W, EVO_SWATCH_H, 5,
                              r->swatches[i], r->swatches[i],
                              with_alpha(th->border, 170), 1,
                              with_alpha(th->shadow, 0), 0);
            sx += EVO_SWATCH_W + EVO_SWATCH_GAP;
        }
    }

    if (r->badge_icon >= 0) {
        evo_icon_tinted(fb, x + w - (r->chevron ? 104 : 60),
                        y + (h - 40) / 2 - 16,
                        r->badge_icon, th->accent_alt);
    }

    if (r->chevron) {
        evo_icon_tinted(fb, x + w - 56, y + (h - 40) / 2 - 16,
                        EVO_IC_CHEVRON,
                        r->selected ? th->accent
                                    : with_alpha(th->text_muted, 150));
    }

    if (r->progress >= 0) {
        /*
         * Inset from the card edge and dimmed. Flush against the bottom at
         * full accent it read as a rule *between* rows rather than as this
         * row's watch progress - the brightest thing on an unselected card,
         * touching its border.
         */
        evo_widget_progress_a(fb, x + EVO_ROW_PAD_X, y + h - 12,
                              w - EVO_ROW_PAD_X * 2, 3,
                              r->progress, r->selected ? 210 : 120);
    }
}

/* ---- tile ---------------------------------------------------------------- */

void evo_widget_tile(uint32_t *fb, int x, int y, int w, int h,
                     const evo_tile *t)
{
    const evo_theme *th = evo_theme_current();
    int pad = 18;
    int title_y;

    if (!t) return;

    /* Base card first: it supplies the shadow, border and rounded corners,
     * and artwork is laid inside it. */
    evo_ui_card(fb, x, y, w, h, t->selected);

    if (t->art && t->art_w > 0 && t->art_h > 0 && t->art_inset) {
        /* Crisp 1:1 thumbnail in the icon position, with a hairline so it
         * reads as artwork rather than as a smudge on the card. */
        int tw = t->art_w, thh = t->art_h;
        int tx = x + pad, ty = y + pad - 4;

        if (tw > w - pad * 2)                       tw = w - pad * 2;
        if (thh > h - EVO_TILE_CAPTION_H - pad * 2) thh = h - EVO_TILE_CAPTION_H
                                                          - pad * 2;

        blit_cover(fb, tx, ty, tw, thh, t->art, t->art_w, t->art_h, 255);

        evo_ui_round_rect(fb, tx - 1, ty - 1, tw + 2, thh + 2, 4,
                          with_alpha(th->border, 0), with_alpha(th->border, 0),
                          with_alpha(th->border, 160), 1,
                          with_alpha(th->shadow, 0), 0);
    } else if (t->art && t->art_w > 0 && t->art_h > 0) {
        blit_cover(fb, x + 2, y + 2, w - 4, h - 4,
                   t->art, t->art_w, t->art_h, 255);

        /* Scrim under the caption. Without it a bright frame makes the title
         * unreadable, which is the failure mode of every poster grid. */
        evo_ui_vgrad_over(fb, x + 2, y + h - (EVO_TILE_CAPTION_H + 16), w - 4,
                          EVO_TILE_CAPTION_H + 14,
                          with_alpha(th->scrim, 0), with_alpha(th->scrim, 245));

        /* Re-draw the selection border on top of the artwork. */
        if (t->selected)
            evo_ui_round_rect(fb, x, y, w, h, th->radius,
                              with_alpha(th->surface_sel, 0),
                              with_alpha(th->surface_sel, 0),
                              th->border_sel, th->border_px + 1,
                              with_alpha(th->shadow, 0), 0);
    } else if (t->icon >= 0) {
        /* Destination tile: a large soft icon, top-left, with the accent
         * bloom behind it when focused. */
        if (t->selected)
            evo_ui_circle(fb, x + pad + 36, y + pad + 36, 44,
                          with_alpha(th->accent_soft, 70));

        evo_icon_tinted(fb, x + pad, y + pad, t->icon,
                        t->selected ? th->accent
                                    : mix(th->text_secondary, th->accent, 130));
    }

    /*
     * A tile carries its own, smaller type ramp than a full-width row: SUB
     * over SMALL rather than MENU over SUB.
     *
     * At MENU a 274px tile could not hold an ordinary filename - four of the
     * six recent tiles ellipsised, and "Dolby TrueHD 7.1" became
     * "Dolby TrueH..", which is the part that identifies the file. The atlas
     * only offers four sizes, so this is a whole step down; taking the detail
     * line down with it keeps the two levels distinguishable instead of
     * flattening the tile into one weight.
     */
    if (t->detail && *t->detail) {
        int detail_y;
        evo_text_y_stacked(y + h - EVO_TILE_CAPTION_H, EVO_TILE_CAPTION_H - 8,
                           EVO_FACE_SUB, EVO_FACE_SMALL, 6,
                           &title_y, &detail_y);
        evo_text_fit(fb, x + pad, title_y, w - pad * 2, t->title,
                     th->text_primary, EVO_FACE_SUB);
        evo_text_fit(fb, x + pad, detail_y, w - pad * 2, t->detail,
                     th->text_secondary, EVO_FACE_SMALL);
    } else {
        title_y = evo_text_y_centred(y + h - EVO_TILE_CAPTION_H,
                                     EVO_TILE_CAPTION_H - 12, EVO_FACE_SUB);
        evo_text_fit(fb, x + pad, title_y, w - pad * 2, t->title,
                     th->text_primary, EVO_FACE_SUB);
    }

    if (t->progress >= 0)
        evo_widget_progress(fb, x + pad, y + h - 12, w - pad * 2, 4,
                            t->progress);
}

void evo_widget_shelf_label(uint32_t *fb, int x, int y, const char *label)
{
    const evo_theme *th = evo_theme_current();

    evo_ui_round_rect(fb, x, y + 8, 3, 20, 1,
                      th->accent, th->accent, th->accent, 0,
                      with_alpha(th->shadow, 0), 0);

    evo_text(fb, x + 14, y, label, th->text_secondary, EVO_FACE_SUB);
}

/* ---- property inspector -------------------------------------------------- */

int evo_widget_props(uint32_t *fb, int x, int y, int w,
                     const evo_prop *props, int count)
{
    const evo_theme *th = evo_theme_current();
    /*
     * 34, not 40. The inspector shows up to nine properties under a 315px
     * preview, and at 40 the ninth row (AUDIO, on any file that has both a
     * video and an audio stream) landed past the footer rule. Measured
     * against EVO_CONTENT_B rather than adjusted by eye.
     */
    const int line_h    = 34;
    int key_w           = 0;
    int i;

    if (!props || count <= 0) return y;

    /*
     * Values share one column, positioned off the widest key actually being
     * drawn. A fixed column either wraps on "VIDEO CODEC" or leaves a gulf
     * after "SIZE", depending on which file is selected.
     */
    for (i = 0; i < count; i++) {
        int kw;
        if (!props[i].value || !*props[i].value) continue;
        kw = evo_text_w(props[i].key, EVO_FACE_SMALL);
        if (kw > key_w) key_w = kw;
    }

    key_w += 24;
    if (key_w > w / 2) key_w = w / 2;

    for (i = 0; i < count; i++) {
        int ty;

        if (!props[i].value || !*props[i].value) continue;

        ty = evo_text_y_centred(y, line_h, EVO_FACE_SMALL);

        evo_text(fb, x, ty, props[i].key, th->text_muted, EVO_FACE_SMALL);
        evo_text_fit(fb, x + key_w, ty, w - key_w, props[i].value,
                     th->text_primary, EVO_FACE_SMALL);

        y += line_h;

        if (i + 1 < count && props[i + 1].value && *props[i + 1].value)
            evo_ui_hline(fb, x, y - 1, w, with_alpha(th->border, 90));
    }

    return y;
}

/* ---- preview panel ------------------------------------------------------- */

void evo_widget_preview(uint32_t *fb, int x, int y, int w, int h,
                        const uint32_t *art, int art_w, int art_h,
                        const char *badge, int placeholder_icon)
{
    const evo_theme *th = evo_theme_current();

    evo_ui_round_rect(fb, x, y, w, h, th->radius,
                      mix(th->bg_bottom, th->surface, 120),
                      mix(th->bg_bottom, th->surface, 60),
                      th->border, th->border_px,
                      th->shadow, th->shadow_px);

    if (art && art_w > 0 && art_h > 0) {
        /*
         * Fit rather than cover, letterboxed. A thumbnail cropped to fill
         * hides exactly the part of the frame that identifies the film.
         */
        int dw = w - 4, dh = h - 4;
        int sw, sh, ox, oy;

        if (art_w * dh > art_h * dw) {
            sw = dw;
            sh = (art_h * dw) / art_w;
        } else {
            sh = dh;
            sw = (art_w * dh) / art_h;
        }

        ox = x + 2 + (dw - sw) / 2;
        oy = y + 2 + (dh - sh) / 2;

        blit_cover(fb, ox, oy, sw, sh, art, art_w, art_h, 255);
    } else if (placeholder_icon >= 0) {
        evo_icon_tinted(fb, x + (w - EVO_RAIL_ICON) / 2,
                        y + (h - EVO_RAIL_ICON) / 2 - 12,
                        placeholder_icon, with_alpha(th->text_muted, 110));

        evo_text_centre(fb, x + w / 2, y + h / 2 + 44,
                        "NO PREVIEW", with_alpha(th->text_muted, 170),
                        EVO_FACE_SMALL);
    }

    if (badge && *badge) {
        int bw = evo_text_w(badge, EVO_FACE_SMALL) + 24;

        evo_ui_round_rect(fb, x + w - bw - 12, y + h - 42, bw, 30, 8,
                          with_alpha(th->scrim, 220),
                          with_alpha(th->scrim, 240),
                          with_alpha(th->border, 120), 1,
                          with_alpha(th->shadow, 0), 0);

        evo_text_centre(fb, x + w - bw / 2 - 12,
                        evo_text_y_centred(y + h - 42, 30, EVO_FACE_SMALL),
                        badge, th->text_primary, EVO_FACE_SMALL);
    }
}

/* ---- indicators ---------------------------------------------------------- */

void evo_widget_scrollbar(uint32_t *fb, int x, int y, int h,
                          int permille, int visible, int count)
{
    const evo_theme *th = evo_theme_current();
    int track_w = 4;
    int thumb_h;
    int thumb_y;

    if (permille < 0 || count <= 0 || visible <= 0 || visible >= count)
        return;

    evo_ui_round_rect(fb, x, y, track_w, h, 2,
                      with_alpha(th->border, 90), with_alpha(th->border, 90),
                      with_alpha(th->border, 0), 0,
                      with_alpha(th->shadow, 0), 0);

    thumb_h = (h * visible) / count;
    if (thumb_h < 28) thumb_h = 28;
    if (thumb_h > h)  thumb_h = h;

    thumb_y = y + ((h - thumb_h) * permille) / 1000;

    evo_ui_round_rect(fb, x, thumb_y, track_w, thumb_h, 2,
                      th->accent, th->accent,
                      with_alpha(th->accent, 0), 0,
                      with_alpha(th->shadow, 0), 0);
}

void evo_widget_progress_a(uint32_t *fb, int x, int y, int w, int h,
                           int permille, int alpha)
{
    const evo_theme *th = evo_theme_current();
    int fill;

    if (permille < 0 || w <= 0) return;
    if (permille > 1000) permille = 1000;

    evo_ui_round_rect(fb, x, y, w, h, h / 2,
                      with_alpha(th->text_muted, (70 * alpha) / 255),
                      with_alpha(th->text_muted, (70 * alpha) / 255),
                      with_alpha(th->border, 0), 0,
                      with_alpha(th->shadow, 0), 0);

    fill = (w * permille) / 1000;
    if (fill <= 0) return;

    evo_ui_round_rect(fb, x, y, fill, h, h / 2,
                      with_alpha(th->accent, alpha),
                      with_alpha(th->accent, alpha),
                      with_alpha(th->accent, 0), 0,
                      with_alpha(th->shadow, 0), 0);
}

void evo_widget_progress(uint32_t *fb, int x, int y, int w, int h,
                         int permille)
{
    evo_widget_progress_a(fb, x, y, w, h, permille, 255);
}

/* ---- toast --------------------------------------------------------------- */

void evo_widget_toast(uint32_t *fb, const evo_toast *t)
{
    const evo_theme *th = evo_theme_current();
    uint32_t accent;
    int      icon;
    int      x, y, title_y, msg_y;
    int      a;

    if (!t || t->alpha <= 0) return;

    a = t->alpha > 255 ? 255 : t->alpha;

    switch (t->kind) {
        case EVO_TOAST_ERROR: accent = th->danger;     icon = EVO_IC_ABOUT;   break;
        case EVO_TOAST_OK:    accent = th->accent_alt; icon = EVO_IC_RESUME;  break;
        default:              accent = th->accent;     icon = EVO_IC_ABOUT;   break;
    }

    x = EVO_SCREEN_W - EVO_TOAST_W - EVO_TOAST_MARGIN + t->slide;
    y = EVO_TOAST_Y;

    /*
     * Card first, then an accent rail down the left edge - the same vocabulary
     * a selected row uses, so a toast reads as part of the same system rather
     * than as a floating notification borrowed from somewhere else.
     *
     * The whole thing is drawn at the caller's alpha, including the shadow;
     * fading only the fill left a hard-edged shadow hanging in the air for the
     * last frames of the dismissal.
     */
    evo_ui_round_rect(fb, x, y, EVO_TOAST_W, EVO_TOAST_H, th->radius + 6,
                      with_alpha(th->surface,     (a * 245) / 255),
                      with_alpha(th->surface_alt, (a * 245) / 255),
                      with_alpha(accent,          (a * 210) / 255),
                      th->border_px + 1,
                      with_alpha(th->shadow, (a * ((th->shadow >> 24) & 0xFF))
                                             / 255),
                      th->shadow_px);

    evo_ui_round_rect(fb, x + EVO_TOAST_PAD, y + 26,
                      EVO_TOAST_RAIL_W, EVO_TOAST_H - 52, 2,
                      with_alpha(accent, a), with_alpha(accent, a),
                      with_alpha(accent, 0), 0,
                      with_alpha(th->shadow, 0), 0);

    evo_icon_tinted(fb, x + EVO_TOAST_PAD + 22,
                    y + (EVO_TOAST_H - EVO_RAIL_ICON) / 2,
                    icon, with_alpha(accent, a));

    {
        const int tx = x + EVO_TOAST_PAD + 22 + EVO_RAIL_ICON + 16;
        const int tw = EVO_TOAST_W - (tx - x) - EVO_TOAST_PAD;

        if (t->message && *t->message) {
            evo_text_y_stacked(y, EVO_TOAST_H, EVO_FACE_SUB, EVO_FACE_SMALL,
                               10, &title_y, &msg_y);
        } else {
            title_y = evo_text_y_centred(y, EVO_TOAST_H, EVO_FACE_SUB);
            msg_y   = 0;
        }

        /* Ellipsised by measurement. The old toast cut the message at a
         * hardcoded 54 characters and appended three dots, which truncated
         * short-glyph strings far too early and long-glyph ones too late. */
        evo_text_fit(fb, tx, title_y, tw, t->title,
                     with_alpha(th->text_primary, a), EVO_FACE_SUB);

        if (t->message && *t->message)
            evo_text_fit(fb, tx, msg_y, tw, t->message,
                         with_alpha(th->text_secondary, a), EVO_FACE_SMALL);
    }
}

void evo_widget_empty(uint32_t *fb, int x, int y, int w, int h,
                      const char *title, const char *hint, int icon)
{
    const evo_theme *th = evo_theme_current();
    int cx = x + w / 2;
    int cy = y + h / 2;

    if (icon >= 0) {
        evo_ui_circle(fb, cx, cy - 70, 58, with_alpha(th->surface, 200));
        evo_icon_tinted(fb, cx - EVO_RAIL_ICON / 2, cy - 70 - EVO_RAIL_ICON / 2,
                        icon, with_alpha(th->text_muted, 150));
    }

    if (title)
        evo_text_centre(fb, cx, cy + 10, title, th->text_secondary,
                        EVO_FACE_MENU);

    if (hint)
        evo_text_centre(fb, cx, cy + 62, hint, th->text_muted, EVO_FACE_SUB);
}
