/*
 * evo_chrome — the frame every page sits inside.
 *
 * Before this, each screen drew its own background wash, its own header rule,
 * its own footer bar and its own button hints, at its own coordinates. The
 * browser header started at x=82 while its rows started at x=180; the menu
 * used x=180 for both. Nothing lined up between pages because nothing shared
 * a definition of what a page *is*.
 *
 * Now a screen declares what it is - title, subtitle, section, which hints to
 * show - and the chrome draws the rest. Consistency stops being something
 * every screen has to remember and becomes the only thing available.
 *
 * Typical use:
 *
 *     evo_page page = {0};
 *     page.title    = "USB MEDIA";
 *     page.subtitle = breadcrumb;
 *     page.section  = EVO_SECTION_BROWSER;
 *
 *     evo_chrome_begin(fb, &page);
 *     ... draw content between evo_chrome_content_x() and _r() ...
 *     evo_chrome_end(fb, &page, hints, hint_count);
 */
#ifndef EVO_CHROME_H
#define EVO_CHROME_H

#include <stdint.h>

#include "evo_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- page description --------------------------------------------------- */

typedef struct evo_page {
    const char *title;      /* required */
    const char *subtitle;   /* breadcrumb or one-line description, may be NULL */
    const char *badge;      /* right-aligned in the header, e.g. "3 OF 42" */

    evo_section section;    /* lights the matching rail entry */
    int         no_rail;    /* full-bleed page (the launch screen) */

    /*
     * Set while the rail has focus. The rail expands to show labels and the
     * content dims slightly, so it is obvious which column the stick is
     * driving. Content does not reflow - see the note in evo_metrics.h.
     */
    int         rail_focused;
    int         rail_index;  /* highlighted rail entry while focused */
} evo_page;

/* ---- footer hints ------------------------------------------------------- */

typedef struct evo_hint {
    int         glyph;   /* EVO_GLYPH_* */
    const char *label;
} evo_hint;

/* A few standard hint sets, so "X SELECT / O BACK" reads identically
 * everywhere instead of being retyped per screen. */
extern const evo_hint EVO_HINTS_LIST[3];      /* select, back, page */
extern const int      EVO_HINTS_LIST_N;
extern const evo_hint EVO_HINTS_BROWSE[4];    /* select, back, favourite, info */
extern const int      EVO_HINTS_BROWSE_N;
extern const evo_hint EVO_HINTS_ROOT[2];      /* select, and no back */
extern const int      EVO_HINTS_ROOT_N;

/* ---- drawing ------------------------------------------------------------ */

/* Background, rail and header. Call first, before any content. */
void evo_chrome_begin(uint32_t *fb, const evo_page *p);

/* Footer rule, hints and the expanded rail overlay. Call last: the rail
 * overlay has to land on top of content, and hints must not be drawn over. */
void evo_chrome_end(uint32_t *fb, const evo_page *p,
                    const evo_hint *hints, int hint_count);

/* ---- content bounds ----------------------------------------------------- */

/*
 * Where content may draw. These account for the rail, so a screen never has
 * to know whether it has one.
 */
int evo_chrome_content_x(const evo_page *p);
int evo_chrome_content_r(const evo_page *p);
int evo_chrome_content_w(const evo_page *p);
int evo_chrome_content_y(const evo_page *p);
int evo_chrome_content_b(const evo_page *p);
int evo_chrome_content_h(const evo_page *p);

/* How many EVO_ROW_PITCH rows fit in the content area of this page. Screens
 * used to hardcode "6", which stopped being true the moment the header grew. */
int evo_chrome_row_capacity(const evo_page *p);

/* ---- side navigation ---------------------------------------------------- */

/*
 * Drawn by evo_chrome_begin/end; exposed because the launch screen wants the
 * rail's geometry to line its first shelf up against it.
 */
int  evo_sidenav_width(const evo_page *p);
int  evo_sidenav_item_y(int index);

/*
 * Move focus within the rail. Wraps, because a seven-item rail is short
 * enough that wrapping is faster than clamping and never disorienting.
 */
int  evo_sidenav_step(int index, int delta);

#ifdef __cplusplus
}
#endif

#endif /* EVO_CHROME_H */
