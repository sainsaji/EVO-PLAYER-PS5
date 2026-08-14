#include "evo_chrome.h"
#include "evo_draw.h"
#include "evo_metrics.h"
#include "evo_theme.h"
#include "evo_ui.h"

/* ---- standard hint sets -------------------------------------------------- */

const evo_hint EVO_HINTS_LIST[3] = {
    { EVO_GLYPH_CROSS,  "SELECT" },
    { EVO_GLYPH_CIRCLE, "BACK"   },
    { EVO_GLYPH_DPAD,   "MOVE"   }
};
const int EVO_HINTS_LIST_N = 3;

const evo_hint EVO_HINTS_BROWSE[4] = {
    { EVO_GLYPH_CROSS,    "OPEN"     },
    { EVO_GLYPH_CIRCLE,   "BACK"     },
    { EVO_GLYPH_TRIANGLE, "FAVORITE" },
    { EVO_GLYPH_SQUARE,   "SEARCH"   }
};
const int EVO_HINTS_BROWSE_N = 4;


const evo_hint EVO_HINTS_ROOT[2] = {
    { EVO_GLYPH_CROSS, "SELECT" },
    { EVO_GLYPH_DPAD,  "MOVE"   }
};
const int EVO_HINTS_ROOT_N = 2;

/* ---- helpers ------------------------------------------------------------- */

/*
 * Blend two theme colours. Used to derive surfaces the theme does not carry a
 * token for - the rail wants to sit between the page background and a card,
 * and inventing a literal here would be exactly the hardcoded colour the
 * theming rules exist to prevent.
 */
static uint32_t mix(uint32_t a, uint32_t b, int t /* 0..255, towards b */)
{
    int ar =  a        & 0xFF, ag = (a >>  8) & 0xFF;
    int ab = (a >> 16) & 0xFF, aa = (a >> 24) & 0xFF;
    int br =  b        & 0xFF, bg = (b >>  8) & 0xFF;
    int bb = (b >> 16) & 0xFF, ba = (b >> 24) & 0xFF;

    int r = ar + ((br - ar) * t) / 255;
    int g = ag + ((bg - ag) * t) / 255;
    int bl = ab + ((bb - ab) * t) / 255;
    int al = aa + ((ba - aa) * t) / 255;

    return ((uint32_t)al << 24) | ((uint32_t)bl << 16) |
           ((uint32_t)g  <<  8) |  (uint32_t)r;
}

static uint32_t with_alpha(uint32_t c, int a)
{
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

/* ---- content bounds ------------------------------------------------------ */

int evo_chrome_content_x(const evo_page *p)
{
    return (p && p->no_rail) ? EVO_BLEED_X : EVO_CONTENT_X;
}

int evo_chrome_content_r(const evo_page *p)
{
    return (p && p->no_rail) ? EVO_BLEED_R : EVO_CONTENT_R;
}

int evo_chrome_content_w(const evo_page *p)
{
    return evo_chrome_content_r(p) - evo_chrome_content_x(p);
}

int evo_chrome_content_y(const evo_page *p)
{
    (void)p;
    return EVO_CONTENT_Y;
}

int evo_chrome_content_b(const evo_page *p)
{
    (void)p;
    return EVO_CONTENT_B;
}

int evo_chrome_content_h(const evo_page *p)
{
    return evo_chrome_content_b(p) - evo_chrome_content_y(p);
}

int evo_chrome_row_capacity(const evo_page *p)
{
    int h = evo_chrome_content_h(p);
    int n;

    /*
     * The last row needs EVO_ROW_H, the ones before it a full pitch each.
     * Deriving this is the point: the browser hardcoded six visible rows in a
     * comment that did the arithmetic by hand, so any change to the header
     * height silently pushed the sixth row under the footer.
     */
    if (h < EVO_ROW_H) return 0;

    n = 1 + (h - EVO_ROW_H) / EVO_ROW_PITCH;
    return n;
}

/* ---- side navigation ----------------------------------------------------- */

int evo_sidenav_width(const evo_page *p)
{
    if (p && p->no_rail) return 0;
    return (p && p->rail_focused) ? EVO_RAIL_W_OPEN : EVO_RAIL_W;
}

int evo_sidenav_item_y(int index)
{
    return EVO_RAIL_TOP + index * EVO_RAIL_ITEM_PITCH;
}

int evo_sidenav_step(int index, int delta)
{
    int n = EVO_SECTION_COUNT;

    index = (index + delta) % n;
    if (index < 0) index += n;

    return index;
}

/*
 * The application mark, at the top of the rail.
 *
 * This is the EVO logo icon - the ring-and-play mark the Media tile shows on
 * the console's home screen - and not, as it was, two concentric circles. A
 * bare accent dot above six real icons read as a bullet point rather than as
 * the product, and it was the one thing on the page that carried no meaning.
 *
 * The soft disc behind it stays: it is what makes the mark sit above the rail
 * items rather than look like a seventh one.
 */
static void draw_app_mark(uint32_t *fb)
{
    const evo_theme *th = evo_theme_current();

    evo_ui_circle(fb, EVO_RAIL_W / 2, EVO_RAIL_MARK_CY, 30,
                  with_alpha(th->accent_soft, 70));

    /* Same x as every rail icon, so the mark and the section icons share one
     * vertical centre line. */
    evo_icon_tinted(fb, EVO_RAIL_ICON_X,
                    EVO_RAIL_MARK_CY - EVO_RAIL_ICON / 2,
                    EVO_IC_LOGO, th->accent);
}

/*
 * The resting rail: a full-height band, the app mark at the top, icons down
 * the left edge, and an accent bar marking the active section.
 */
static void draw_rail_base(uint32_t *fb, const evo_page *p)
{
    const evo_theme *th = evo_theme_current();
    uint32_t band       = mix(th->bg_top, th->surface, 150);
    int i;

    evo_ui_vgrad(fb, 0, 0, EVO_RAIL_W, EVO_SCREEN_H,
                 band, mix(th->bg_bottom, th->surface, 120));

    /* Hairline separating rail from page, so the band has an edge rather than
     * fading into the background wash. */
    evo_ui_vline(fb, EVO_RAIL_W, 0, EVO_SCREEN_H, th->border);

    draw_app_mark(fb);

    for (i = 0; i < EVO_SECTION_COUNT; i++) {
        const evo_section_info *info = evo_section_get((evo_section)i);
        int      y      = evo_sidenav_item_y(i);
        int      active = (p->section == (evo_section)i);
        uint32_t tint;

        if (active) {
            /* Selected pill behind the icon plus a rail on the outer edge -
             * the same vocabulary the cards use for selection, so the two
             * read as one system. */
            evo_ui_round_rect(fb, 8, y, EVO_RAIL_W - 16, EVO_RAIL_ITEM_H,
                              th->radius,
                              th->surface_sel, th->surface_sel_alt,
                              th->border_sel, th->border_px,
                              with_alpha(th->shadow, 0), 0);
            evo_ui_round_rect(fb, 0, y + 14, 4, EVO_RAIL_ITEM_H - 28, 2,
                              th->accent, th->accent,
                              th->accent, 0, with_alpha(th->shadow, 0), 0);
            tint = th->accent;
        } else {
            tint = mix(th->text_muted, th->text_secondary, 90);
        }

        evo_icon_tinted(fb, EVO_RAIL_ICON_X,
                        y + (EVO_RAIL_ITEM_H - EVO_RAIL_ICON) / 2,
                        info->icon, tint);
    }
}

/*
 * The expanded rail, drawn over content once focus is in it. An overlay
 * rather than a reflow: content that shifts sideways every time the cursor
 * brushes the left edge of a list makes the whole page feel unstable.
 */
static void draw_rail_expanded(uint32_t *fb, const evo_page *p)
{
    const evo_theme *th = evo_theme_current();
    int i;

    /* Dim the page behind the overlay so the rail is unambiguously in front. */
    evo_ui_vgrad_over(fb, EVO_RAIL_W, 0,
                      EVO_SCREEN_W - EVO_RAIL_W, EVO_SCREEN_H,
                      with_alpha(th->scrim, 120), with_alpha(th->scrim, 120));

    /*
     * Forced opaque. Card surfaces carry alpha (MIDNIGHT's is 235) so that
     * they layer over the background, and mixing one into the panel fill left
     * it around 240 - enough for the page title behind to ghost visibly
     * through the rail. An overlay that is nearly opaque reads as a rendering
     * fault, not as a design.
     */
    evo_ui_round_rect(fb, 0, 0, EVO_RAIL_W_OPEN, EVO_SCREEN_H, 0,
                      with_alpha(mix(th->bg_top, th->surface, 190), 255),
                      with_alpha(mix(th->bg_bottom, th->surface, 165), 255),
                      th->border, th->border_px,
                      th->shadow, th->shadow_px);

    draw_app_mark(fb);
    evo_text(fb, EVO_RAIL_W + 12,
             evo_text_y_centred(96, 44, EVO_FACE_MENU),
             "EVO", th->text_primary, EVO_FACE_MENU);

    for (i = 0; i < EVO_SECTION_COUNT; i++) {
        const evo_section_info *info = evo_section_get((evo_section)i);
        int      y       = evo_sidenav_item_y(i);
        int      focused = (p->rail_index == i);
        int      active  = (p->section == (evo_section)i);
        uint32_t tint;
        int      label_y;

        if (focused) {
            evo_ui_card(fb, 8, y, EVO_RAIL_W_OPEN - 24, EVO_RAIL_ITEM_H, 1);
            tint = th->accent;
        } else if (active) {
            evo_ui_round_rect(fb, 8, y, EVO_RAIL_W_OPEN - 24, EVO_RAIL_ITEM_H,
                              th->radius,
                              th->surface, th->surface_alt,
                              th->border, th->border_px,
                              with_alpha(th->shadow, 0), 0);
            tint = th->accent;
        } else {
            tint = mix(th->text_muted, th->text_secondary, 90);
        }

        evo_icon_tinted(fb, EVO_RAIL_ICON_X,
                        y + (EVO_RAIL_ITEM_H - EVO_RAIL_ICON) / 2,
                        info->icon, tint);

        label_y = evo_text_y_centred(y, EVO_RAIL_ITEM_H, EVO_FACE_MENU);
        evo_text_fit(fb, EVO_RAIL_ICON_X + EVO_RAIL_ICON + 20, label_y,
                     EVO_RAIL_W_OPEN - (EVO_RAIL_ICON_X + EVO_RAIL_ICON + 44),
                     info->label,
                     (focused || active) ? th->text_primary : th->text_secondary,
                     EVO_FACE_MENU);
    }
}

/* ---- header -------------------------------------------------------------- */

static void draw_header(uint32_t *fb, const evo_page *p)
{
    const evo_theme *th = evo_theme_current();
    int x = evo_chrome_content_x(p);

    /* Accent tick. The mark sits at the content margin so the title, the
     * rows below it and the footer hints all share one left edge. */
    evo_ui_round_rect(fb, x, EVO_HEADER_MARK_Y,
                      EVO_HEADER_MARK_W, EVO_HEADER_MARK_H, 2,
                      th->accent, th->accent, th->accent, 0,
                      with_alpha(th->shadow, 0), 0);

    evo_text(fb, x + 40, EVO_HEADER_TITLE_Y,
             p->title ? p->title : "", th->text_primary, EVO_FACE_TITLE);

    if (p->subtitle && *p->subtitle) {
        int avail = evo_chrome_content_r(p) - (x + 40) - 260;
        evo_text_fit(fb, x + 42, EVO_HEADER_SUB_Y, avail,
                     p->subtitle, th->text_secondary, EVO_FACE_SUB);
    }

    if (p->badge && *p->badge) {
        /* text_secondary, not text_muted: at muted the position counter was
         * effectively invisible against the background bloom on the right of
         * the page, which is exactly where it sits. */
        evo_text_right(fb, evo_chrome_content_r(p), EVO_HEADER_SUB_Y + 4,
                       p->badge, th->text_secondary, EVO_FACE_SMALL);
    }

    /*
     * Full strength, not a faded border. At alpha 120 the rule lifted the
     * dark left of the page by 17 levels but the lighter right - where the
     * background bloom sits - by 2, so it visibly stopped halfway across.
     * Measured with tools/shot.sh probe rather than judged by eye.
     */
    evo_ui_hline(fb, x, EVO_HEADER_RULE_Y,
                 evo_chrome_content_r(p) - x, th->border);
}

/* ---- footer -------------------------------------------------------------- */

static void draw_footer(uint32_t *fb, const evo_page *p,
                        const evo_hint *hints, int n)
{
    const evo_theme *th = evo_theme_current();
    int x = evo_chrome_content_x(p);
    int i;

    evo_ui_hline(fb, 0, EVO_FOOTER_RULE_Y, EVO_SCREEN_W, th->border);
    evo_ui_vgrad_over(fb, 0, EVO_FOOTER_Y, EVO_SCREEN_W, EVO_FOOTER_H,
                      with_alpha(th->scrim, 200), th->scrim);

    for (i = 0; i < n && hints; i++) {
        /*
         * Glyph and label share one centre line, so each hint reads as a
         * single item.
         *
         * They used not to: the glyph was pinned to EVO_FOOTER_Y + 14 while
         * the label was centred in the footer, leaving the two 27px apart
         * vertically - close enough to look like a mistake rather than a
         * style, and it made the row read as two staggered lines. The advance
         * was 44 for a 48px glyph as well, so the label overlapped it.
         *
         * Hints are laid out by measuring, not on a fixed pitch. A fixed
         * pitch either wastes space after "OPEN" or collides after
         * "FAVORITE", and the previous UI had both problems on different
         * screens.
         */
        int text_y = evo_text_y_centred(EVO_FOOTER_Y, EVO_FOOTER_H,
                                        EVO_FACE_SMALL);

        evo_glyph(fb, x,
                  EVO_FOOTER_Y + (EVO_FOOTER_H - EVO_FOOTER_GLYPH) / 2,
                  hints[i].glyph);

        x += EVO_FOOTER_GLYPH + EVO_FOOTER_GLYPH_GAP;

        evo_text(fb, x, text_y, hints[i].label,
                 th->text_secondary, EVO_FACE_SMALL);

        x += evo_text_w(hints[i].label, EVO_FACE_SMALL) + EVO_FOOTER_HINT_GAP;
    }

    /* Version, right aligned. One literal, injected by the Makefile. */
#ifdef EVO_PLAYER_VERSION
    evo_text_right(fb, evo_chrome_content_r(p),
                   evo_text_y_centred(EVO_FOOTER_Y, EVO_FOOTER_H,
                                      EVO_FACE_SMALL),
                   "V" EVO_PLAYER_VERSION, th->text_muted, EVO_FACE_SMALL);
#endif
}

/* ---- entry points -------------------------------------------------------- */

void evo_chrome_begin(uint32_t *fb, const evo_page *p)
{
    evo_ui_background(fb);

    if (!p) return;

    if (!p->no_rail)
        draw_rail_base(fb, p);

    draw_header(fb, p);
}

void evo_chrome_end(uint32_t *fb, const evo_page *p,
                    const evo_hint *hints, int hint_count)
{
    if (!p) return;

    draw_footer(fb, p, hints, hint_count);

    /* Last, so it covers content and the footer both. */
    if (!p->no_rail && p->rail_focused)
        draw_rail_expanded(fb, p);
}
