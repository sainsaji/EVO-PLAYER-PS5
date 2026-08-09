/*
 * evo_screens — screen drawing, kept out of main.c.
 *
 * Screens take a *model*: a plain description of what to show, filled in by
 * main.c from its own state. Nothing in here reaches back into the player,
 * the decoder or the asset headers, which is what lets the whole UI layer
 * compile in a second and be exercised on the host.
 *
 * The models are deliberately dumb - arrays of strings and a few integers.
 * Building one per frame costs nothing next to the per-pixel blending, and
 * it means the drawing code cannot accidentally depend on decoder state
 * that is only valid on some frames.
 */
#ifndef EVO_SCREENS_H
#define EVO_SCREENS_H

#include <stdint.h>

#include "evo_chrome.h"
#include "evo_focus.h"
#include "evo_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- launch screen ------------------------------------------------------ */

#define EVO_LAUNCH_RECENT_MAX 8

/* Row indices in the launch screen's grid. */
enum {
    EVO_LAUNCH_ROW_HERO   = 0,
    EVO_LAUNCH_ROW_RECENT = 1,
    EVO_LAUNCH_ROW_LIBRARY = 2,
    EVO_LAUNCH_ROWS       = 3
};

typedef struct evo_art {
    const uint32_t *pixels;   /* BGRA, may be NULL */
    int             w;
    int             h;
} evo_art;

typedef struct evo_launch_item {
    const char *title;
    const char *detail;      /* "1H 42M LEFT", "MKV - 4.2 GB" */
    int         progress;    /* 0..1000, or -1 */
    evo_art     art;
} evo_launch_item;

typedef struct evo_launch_model {
    /*
     * The hero is the thing to resume. When there is nothing to resume it
     * becomes a branded panel pointing at the browser, rather than being
     * hidden - a home screen that changes shape depending on history is
     * disorienting, and the grid would have to renumber its rows.
     */
    int         has_resume;
    const char *hero_title;
    const char *hero_detail;
    const char *hero_action;   /* text on the call-to-action chip */
    int         hero_progress; /* 0..1000, or -1 */
    evo_art     hero_art;

    const evo_launch_item *recent;
    int                    recent_count;

    const char *clock;         /* "04:55 PM", or NULL to omit */
    const char *theme_name;
} evo_launch_model;

/*
 * Tell the grid how many items each row holds. Call before moving the cursor
 * and before drawing, every frame - the counts change as files are played.
 */
void evo_screen_launch_sync(evo_grid *g, const evo_launch_model *m);

void evo_screen_launch(uint32_t *fb, const evo_launch_model *m,
                       const evo_grid *g);

/*
 * Where the cursor currently is, resolved to something main.c can act on.
 * Returns the screen to navigate to, or EVO_SCREEN_LAUNCH when the selection
 * is not a destination (the hero, or a recent item). `out_recent_index` is
 * set to the recent item to play, or -1.
 */
evo_screen_id evo_screen_launch_activate(const evo_grid *g,
                                         const evo_launch_model *m,
                                         int *out_recent_index,
                                         int *out_play_hero);

/* ---- file browser ------------------------------------------------------- */

/* Kinds drive the row icon and the "TYPE" property. */
typedef enum {
    EVO_FILE_FOLDER = 0,
    EVO_FILE_VIDEO,
    EVO_FILE_AUDIO,
    EVO_FILE_IMAGE,
    EVO_FILE_SUBTITLE,
    EVO_FILE_OTHER
} evo_file_kind;

typedef struct evo_browser_entry {
    const char   *name;
    const char   *detail;      /* second line: type, size, duration */
    evo_file_kind kind;
    int           favorite;
    int           progress;    /* 0..1000 resume position, or -1 */
} evo_browser_entry;

/*
 * The inspector's contents. main.c fills whichever fields it has managed to
 * determine; empty strings are skipped, so a file whose codecs have not been
 * probed yet shows the fields that are already known instead of an empty
 * panel or a stale one.
 */
typedef struct evo_browser_inspect {
    const char *name;
    const char *kind;
    const char *extension;
    const char *size;
    const char *duration;
    const char *resolution;
    const char *video_codec;
    const char *audio_codec;
    const char *subtitles;
    const char *container;

    evo_art     preview;
    const char *preview_badge;   /* duration overlay on the thumbnail */
    int         probing;         /* show a "reading..." state */
} evo_browser_inspect;

typedef struct evo_browser_model {
    const char              *path;          /* breadcrumb */

    /*
     * `entries` covers only the visible window: entries[0] is absolute index
     * `first`. A folder can hold hundreds of files, and formatting all of
     * them - each needing a stat() for its size - every frame to draw six
     * rows is work nobody sees.
     */
    const evo_browser_entry *entries;
    int                      first;
    int                      entry_count;   /* how many `entries` holds */
    int                      count;         /* items in the folder */

    const evo_browser_inspect *inspect;     /* may be NULL */

    int at_root;    /* hides the "back" affordance */
} evo_browser_model;

/* Rows that fit in the browser's list column. */
int evo_screen_browser_capacity(void);

void evo_screen_browser(uint32_t *fb, const evo_browser_model *m,
                        const evo_focus *f, int rail_focused, int rail_index);

/* ---- generic list page -------------------------------------------------- */

/*
 * Recent files, favourites, settings, tools and about are all the same shape:
 * a titled page with a column of rows. They had four separate implementations
 * that had already drifted apart in row height, margin and footer content.
 */
typedef struct evo_list_entry {
    const char *title;
    const char *detail;
    int         icon;        /* EVO_IC_*, or -1 */
    int         chevron;
    int         progress;    /* 0..1000, or -1 */
    int         info;        /* states a fact rather than offering an action */

    /* Colour chips on the right - the settings THEME row. */
    const uint32_t *swatches;
    int             swatch_count;
} evo_list_entry;

typedef struct evo_list_model {
    const char           *title;
    const char           *subtitle;
    evo_section           section;
    const evo_list_entry *entries;
    int                   count;

    const char *empty_title;
    const char *empty_hint;
    int         empty_icon;
} evo_list_model;

int  evo_screen_list_capacity(void);

void evo_screen_list(uint32_t *fb, const evo_list_model *m,
                     const evo_focus *f, int rail_focused, int rail_index,
                     const evo_hint *hints, int hint_count);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SCREENS_H */
