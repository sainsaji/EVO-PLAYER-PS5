/*
 * evo_chrome — page description + shared layout geometry.
 *
 * The immediate-mode chrome renderer (background, rail, header, footer) was
 * retired with the RmlUi migration (#44). What survives here is the page
 * description (`evo_page`, `evo_hint`, the standard hint sets) and the pure
 * geometry helpers — content bounds, row capacity, rail metrics — still used
 * by main.c to keep the RmlUi DOM in step with the focus model. The
 * implementations live in ui/src/evo_layout.c.
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

/* The standard footer hint set, still fed to the RmlUi list screens. */
extern const evo_hint EVO_HINTS_LIST[3];      /* select, back, page */
extern const int      EVO_HINTS_LIST_N;

/* ---- content bounds ----------------------------------------------------- */

/*
 * Where content may draw. These account for the rail, so a screen never has
 * to know whether it has one. main.c derives the RmlUi row window from
 * evo_chrome_row_capacity() so the DOM stays in step with the focus model.
 */
int evo_chrome_content_x(const evo_page *p);
int evo_chrome_content_r(const evo_page *p);
int evo_chrome_content_y(const evo_page *p);
int evo_chrome_content_b(const evo_page *p);
int evo_chrome_content_h(const evo_page *p);

/* How many EVO_ROW_PITCH rows fit in the content area of this page. Screens
 * used to hardcode "6", which stopped being true the moment the header grew. */
int evo_chrome_row_capacity(const evo_page *p);

/*
 * Move focus within the rail. Wraps, because a seven-item rail is short
 * enough that wrapping is faster than clamping and never disorienting.
 */
int  evo_sidenav_step(int index, int delta);

#ifdef __cplusplus
}
#endif

#endif /* EVO_CHROME_H */
