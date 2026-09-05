/*
 * evo_widgets — the pieces screens are built from.
 *
 * Each of these existed already, open-coded two or three times with slightly
 * different metrics: the browser drew a row one way, favourites another,
 * settings a third. They looked related rather than identical, and a fix to
 * one never reached the others.
 *
 * Everything here takes a small description struct rather than a long
 * argument list, so adding an optional detail later does not touch every
 * call site.
 */
#ifndef EVO_WIDGETS_H
#define EVO_WIDGETS_H

#include <stdint.h>

#include "evo_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- list row ------------------------------------------------------------ */

typedef struct evo_row {
    const char *title;
    const char *detail;      /* second line, may be NULL */
    int         icon;        /* EVO_IC_*, or -1 for none */
    int         selected;
    int         chevron;     /* draw a disclosure arrow on the right */

    /*
     * A row that states a fact rather than offering an action - the About
     * page. Emphasis inverts: the value is what you came to read, so it takes
     * the primary colour and the label steps back. Without this an
     * informational row is indistinguishable from a selectable one.
     */
    int         info;
    int         badge_icon;  /* EVO_IC_*, right side, or -1 */

    /* 0..1000, or -1 for none. Drawn as a thin resume bar along the bottom
     * edge of the card - the only place watch progress can live without
     * stealing a text line. */
    int         progress;

    /* Marquee phase for the title when it overflows; pass the focus model's
     * settled_ms. 0 disables. */
    int         marquee_phase;

    /*
     * Colour chips shown on the right of the row. The settings THEME row uses
     * them so cycling themes is not blind - the name alone tells you nothing
     * about what you are about to switch to.
     */
    const uint32_t *swatches;
    int             swatch_count;
} evo_row;

void evo_widget_row(uint32_t *fb, int x, int y, int w, int h,
                    const evo_row *r);

/*
 * The selection highlight, drawn separately so it can glide between rows
 * independently of the row content. Draw all resting rows, then this, then
 * all row content.
 */
void evo_widget_row_highlight(uint32_t *fb, int x, int y, int w, int h);

/* ---- tile (launch screen) ------------------------------------------------ */

typedef struct evo_tile {
    const char *title;
    const char *detail;      /* may be NULL */
    int         icon;        /* EVO_IC_*, or -1 */
    int         selected;

    /*
     * Optional artwork: a BGRA buffer the caller owns, drawn scaled to fill
     * the tile with a scrim under the text. NULL falls back to a tinted
     * surface, which is what the destination tiles use.
     */
    const uint32_t *art;
    int             art_w;
    int             art_h;

    /*
     * Draw the artwork at 1:1 where the icon would go, rather than stretched
     * across the tile. The cover cache holds 80x80 thumbnails; blown up to
     * fill a 274x166 tile they are a visibly blocky 3x upscale, and a crisp
     * small thumbnail reads as deliberate where a blurry large one reads as
     * broken.
     */
    int             art_inset;

    int         progress;    /* 0..1000, or -1 */
} evo_tile;

void evo_widget_tile(uint32_t *fb, int x, int y, int w, int h,
                     const evo_tile *t);

/* Section heading above a shelf of tiles. */
void evo_widget_shelf_label(uint32_t *fb, int x, int y, const char *label);

/* ---- property inspector -------------------------------------------------- */

typedef struct evo_prop {
    const char *key;
    const char *value;
} evo_prop;

/*
 * A key/value table: keys muted and left aligned, values primary and aligned
 * to a common column so they scan vertically. Skips entries whose value is
 * NULL or empty, so a caller can build a fixed array and let the ones it
 * could not determine fall out.
 *
 * Returns the y just past the last row drawn.
 */
int evo_widget_props(uint32_t *fb, int x, int y, int w,
                     const evo_prop *props, int count);

/* ---- preview panel ------------------------------------------------------- */

/*
 * Artwork in a rounded frame, with an optional duration badge in the corner.
 * `art` is BGRA at art_w x art_h and is nearest-scaled to fit while keeping
 * its aspect; NULL draws a placeholder with the given icon.
 */
void evo_widget_preview(uint32_t *fb, int x, int y, int w, int h,
                        const uint32_t *art, int art_w, int art_h,
                        const char *badge, int placeholder_icon);

/* ---- indicators ---------------------------------------------------------- */

/* Vertical scrollbar. `permille` is evo_focus_scroll_permille(); negative
 * draws nothing, which is what a list that fits should show. */
void evo_widget_scrollbar(uint32_t *fb, int x, int y, int h,
                          int permille, int visible, int count);

/* Horizontal progress / resume bar. */
void evo_widget_progress(uint32_t *fb, int x, int y, int w, int h,
                         int permille);

/*
 * As above, at a given opacity. A resume bar sitting on a card wants to be
 * quieter than a hero's progress bar: at full accent it stopped reading as
 * part of the card and started reading as a separator rule between rows.
 */
void evo_widget_progress_a(uint32_t *fb, int x, int y, int w, int h,
                           int permille, int alpha);

/* Centred "nothing here" state, with a reason and a suggestion. */
void evo_widget_empty(uint32_t *fb, int x, int y, int w, int h,
                      const char *title, const char *hint, int icon);

/* ---- badge / pill tags --------------------------------------------------- */

enum evo_badge_category {
    EVO_BADGE_ACCENT  = 0, /* Theme Accent (NEW, INFO, 4K) */
    EVO_BADGE_SUCCESS = 1, /* Green (FIXED, READY, ONLINE) */
    EVO_BADGE_WARNING = 2, /* Amber (IMPROVED, STANDBY, HDR) */
    EVO_BADGE_DANGER  = 3, /* Red (REMOVED, ERROR, OFFLINE) */
    EVO_BADGE_MUTED   = 4  /* Subtle text muted pill */
};

/* Generic pill badge with caller-provided colors. */
void evo_widget_badge(uint32_t *fb, int x, int y, int w, int h,
                      const char *text, uint32_t bg, uint32_t border,
                      uint32_t text_col, int face);

/* Standardized categorical pill badge. */
void evo_widget_category_badge(uint32_t *fb, int x, int y, int w, int h,
                               int category, const char *text);

/* ---- stat / telemetry card ----------------------------------------------- */

typedef struct evo_stat_card {
    const char *header_label;
    const char *title;
    const char *line1;
    const char *line2;
    const char *status_text;
    int         is_active;
} evo_stat_card;

void evo_widget_stat_card(uint32_t *fb, int x, int y, int w, int h,
                          const evo_stat_card *card);

/* ---- audio / speaker stage node ------------------------------------------ */

typedef struct evo_speaker_node {
    const char *label;
    const char *sub;
    int         is_active;
    int         is_selected;
} evo_speaker_node;

void evo_widget_speaker_node(uint32_t *fb, int x, int y, int w, int h,
                             const evo_speaker_node *node);

#ifdef __cplusplus
}
#endif

#endif /* EVO_WIDGETS_H */
