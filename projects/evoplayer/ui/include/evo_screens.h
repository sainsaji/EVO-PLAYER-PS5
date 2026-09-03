/*
 * evo_screens — the screen *models*, kept out of main.c.
 *
 * The immediate-mode screen renderers were retired with the RmlUi migration
 * (#44). What remains is the model vocabulary: plain descriptions of what each
 * screen shows (`evo_launch_model`, `evo_browser_model`, `evo_list_model`, …),
 * which main.c still fills in from its own state and then hands to the RmlUi
 * bridge. A few pure layout helpers (grid sync, row capacity, reader wrap
 * width) also survive here; they are implemented in ui/src/evo_layout.c.
 *
 * The models are deliberately dumb - arrays of strings and a few integers -
 * so the UI cannot accidentally depend on decoder state that is only valid
 * on some frames.
 */
#ifndef EVO_SCREENS_H
#define EVO_SCREENS_H

#include <stdint.h>

#include "evo_chrome.h"
#include "evo_focus.h"
#include "evo_nav.h"
#include "evo_widgets.h"

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
    const char              *title;         /* header title, e.g. the active source's name */

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

/* ---- modal dialog ------------------------------------------------------- */

/*
 * The resume prompt and the end-of-playback prompt. Both are "here is what
 * you were watching, pick one of these" and both used to draw their own
 * panel, their own button row and their own scrim.
 *
 * A dialog is drawn OVER whatever is already in the framebuffer - the player
 * leaves the last video frame there - so it scrims rather than clearing.
 */
typedef struct evo_dialog_action {
    int         glyph;    /* EVO_GLYPH_* */
    const char *label;
} evo_dialog_action;

typedef struct evo_dialog_model {
    const char *eyebrow;   /* "RESUME PLAYBACK" */
    const char *title;     /* what it is about, usually the file */
    const char *detail;    /* "STOPPED AT 12M 04S OF 21M 07S" */
    int         progress;  /* 0..1000, or -1 */
    evo_art     art;

    const evo_dialog_action *actions;
    int                      action_count;
} evo_dialog_model;

/* ---- media info --------------------------------------------------------- */

/*
 * The property panel for whatever is playing. Same shape as the browser's
 * inspector, given the whole page instead of a 560px column - so it is two
 * columns of properties beside a preview rather than a third implementation
 * of a key/value table.
 */
typedef struct evo_info_model {
    const char *title;
    const char *subtitle;

    const evo_prop *props;
    int             prop_count;

    evo_art     art;
    const char *art_badge;
} evo_info_model;

/* ---- changelog screen --------------------------------------------------- */

#include "evo_changelog.h"

typedef struct evo_changelog_model {
    const evo_changelog_release *releases;
    int                          release_count;
} evo_changelog_model;

/* ---- surround sound studio screen --------------------------------------- */

typedef struct evo_surround_speaker_info {
    const char *name;
    const char *label;
    double      hz;
    int         dx;
    int         dy;
    int         w;
    int         h;
    int         ch;
    int         item_idx;
} evo_surround_speaker_info;

typedef struct evo_surround_test_model {
    int         is_51_layout;
    int         selected_item;
    int         active_channel;
    int         surround_mode;
    const evo_surround_speaker_info *speakers;
    int         speaker_count;
} evo_surround_test_model;

/* ---- overlay picker ------------------------------------------------------ */

/*
 * A scrollable list drawn over whatever is already on screen. Subtitle track
 * selection needs this and neither of the other two shapes fits: the dialog
 * is a fixed panel with a row of buttons, and the list page paints full-page
 * chrome, which would throw away the video frame behind it.
 *
 * A retail disc rip can carry thirty-odd subtitle tracks, so it scrolls and
 * shows a scrollbar rather than assuming everything fits.
 */
typedef struct evo_picker_entry {
    const char *label;    /* "ENGLISH SDH" */
    const char *detail;   /* "1001 CUES", "SIGNS ONLY", "EXTERNAL FILE" */
    int         current;  /* the track in use - gets a tick */
    /*
     * Dimmed. A track the container admits holds almost nothing is still
     * offered - the count can be wrong - but it should not look like the
     * obvious choice, because picking it looks exactly like subtitles being
     * broken.
     */
    int         weak;
} evo_picker_entry;

typedef struct evo_picker_model {
    const char *eyebrow;
    const char *title;

    /* Visible window only, same convention as the browser: entries[0] is
     * absolute index `first`. */
    const evo_picker_entry *entries;
    int                     first;
    int                     entry_count;
    int                     count;

    int                     preview_face;   /* -1 if no preview, or 0 (SMALL), 1 (MEDIUM), 2 (LARGE) */
    const char             *preview_text;   /* optional custom text, or NULL for default */
} evo_picker_model;

int  evo_screen_picker_capacity(void);

/* ---- text reader --------------------------------------------------------- */

/*
 * A page of already-wrapped text.
 *
 * The wrapping, the encoding and the scroll position all belong to
 * media/evo_textreader.c; this draws the window it hands over and nothing
 * more. Lines arrive NUL-terminated because the draw vtable takes C strings -
 * the reader copies its visible window into a small buffer rather than making
 * every text primitive in the UI learn about lengths.
 */

/* Enough for the smallest face over the full content height, with room spare.
 * The reader clamps to this; it is a bound on the caller's buffer, not a
 * limit on the document. */
#define EVO_READER_MAX_VISIBLE 64

typedef struct evo_reader_model {
    const char *title;         /* file name */
    const char *subtitle;      /* folder, or a note about the file */
    const char *badge;         /* right of the header: "LINE 412 OF 9210" */

    const char *const *lines;  /* the visible window, top first */
    int         line_count;

    int         face;          /* EVO_FACE_* - the reading size */
    int         line_pitch;    /* px between lines at that face */

    /*
     * Both 0..1. `progress` is how far down the document the top of the view
     * is; `visible_frac` is how much of it is on screen, which is what sets
     * the size of the scrollbar thumb. A thumb that does not encode length
     * tells the reader nothing about how much is left.
     */
    double      progress;
    double      visible_frac;

    /* Shown in place of the text: an error, or why the page is empty. */
    const char *notice;
    /* Shown under the text: "FIRST 2 MB OF 47 MB". */
    const char *footnote;
} evo_reader_model;

/* How many lines fit at `face`. main.c needs this to size its window before
 * it can fill the model. */
int  evo_screen_reader_capacity(int face);

/* Line pitch for a face, so the caller and the screen agree. */
int  evo_screen_reader_pitch(int face);

/*
 * The width text is wrapped to.
 *
 * Published rather than left for the caller to work out, because the caller
 * has to wrap to exactly the column this screen will draw into - and that
 * column depends on the rail and the scrollbar, which are the screen's
 * business. A caller computing it from metrics constants would be a second
 * definition of the layout, wrong the first time either moved.
 */
int  evo_screen_reader_wrap_w(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SCREENS_H */
