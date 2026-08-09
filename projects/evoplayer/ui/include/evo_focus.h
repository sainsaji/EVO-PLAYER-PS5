/*
 * evo_focus — one selection model, shared by every list.
 *
 * Each screen used to carry its own selection integer and its own wrap
 * arithmetic, written out longhand at the point of use:
 *
 *     settings_selected = (settings_selected + 1) % 6;
 *
 * with the 6 typed in by hand. When the settings list grew to seven rows the
 * modulus was not updated, and the last row became unreachable - you could
 * see REMOVE HOME TILE but never select it. That is not a slip anyone would
 * catch by reading; it is the structure inviting the mistake.
 *
 * Here the count lives with the cursor, so it cannot drift out of sync. The
 * scroll window and the glide animation come along with it, because every
 * list needed those too and each had reimplemented them slightly differently.
 */
#ifndef EVO_FOCUS_H
#define EVO_FOCUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_focus {
    int index;      /* selected item, always in [0, count) when count > 0 */
    int count;      /* total items */
    int scroll;     /* index of the first visible item */
    int visible;    /* rows that fit on screen */
    int wrap;       /* 1: past the end returns to the start */

    /* Fixed-point (<<8) y of the selection highlight, so it can glide
     * between rows instead of teleporting. */
    int glide_fp;
    int glide_target_fp;
    int glide_ready; /* 0 until the first tick seeds the position */

    /* Frames since the selection last changed. Marquees and reveal
     * animations key off this rather than each keeping their own counter. */
    int settled_frames;
} evo_focus;

/* Result of a move, so the caller can pick the right feedback. */
typedef enum {
    EVO_MOVE_NONE = 0,   /* nothing happened (empty list) */
    EVO_MOVE_OK,         /* moved normally */
    EVO_MOVE_WRAPPED,    /* moved, and wrapped around the end */
    EVO_MOVE_BLOCKED     /* already at the end of a non-wrapping list */
} evo_move_result;

/* Initialise. Safe to call every frame with a changing count. */
void evo_focus_init(evo_focus *f, int count, int visible, int wrap);

/*
 * Update the item count, keeping the cursor sensible: an index past the new
 * end clamps to the last item rather than pointing off the end of the array.
 * Lists that reload under the cursor (a folder refresh, a favourite removed)
 * all need this and all used to do it by hand, or not at all.
 */
void evo_focus_set_count(evo_focus *f, int count);

/* Move by `delta` items. */
evo_move_result evo_focus_move(evo_focus *f, int delta);

/* Move by a whole page, clamped at the ends (never wraps: a page jump that
 * teleports to the far end of a long list is disorienting). */
evo_move_result evo_focus_page(evo_focus *f, int direction);

/* Jump to the first / last item. */
evo_move_result evo_focus_first(evo_focus *f);
evo_move_result evo_focus_last(evo_focus *f);

/* Select an exact index, clamped. */
evo_move_result evo_focus_set(evo_focus *f, int index);

/*
 * Per-frame update. `row_origin` is the y of the first visible row and
 * `row_pitch` the distance between rows; together they give the highlight
 * something to glide towards. Call once per frame, before drawing.
 */
void evo_focus_tick(evo_focus *f, int row_origin, int row_pitch);

/* Current highlight y, in pixels - the glided position, not the row grid. */
int  evo_focus_glide_y(const evo_focus *f);

/* Row position of the selection within the visible window, or -1 if the
 * list is empty. */
int  evo_focus_visible_index(const evo_focus *f);

/* Is item `index` currently on screen? */
int  evo_focus_is_visible(const evo_focus *f, int index);

/* Fraction of the list scrolled past, 0..1000 - for a scrollbar. Returns
 * -1 when everything fits and no scrollbar should be drawn. */
int  evo_focus_scroll_permille(const evo_focus *f);

/* ---- two-dimensional focus (the launch screen's shelves) ---------------- */

#define EVO_GRID_ROWS 4

/*
 * Rows of horizontally scrolling items. Two behaviours here are not
 * negotiable if the screen is to feel right:
 *
 *   - each row remembers its own column, so dropping from the fifth tile of
 *     one shelf to another and coming back returns you to the fifth tile
 *     rather than to the start
 *   - vertical movement skips rows with no items, so a shelf that is empty
 *     this boot (no recent files yet) does not become a dead stop the cursor
 *     lands on and cannot leave
 */
typedef struct evo_grid {
    int row;
    int rows;

    int count[EVO_GRID_ROWS];    /* items in each row */
    int col[EVO_GRID_ROWS];      /* remembered cursor per row */
    int scroll[EVO_GRID_ROWS];   /* first visible item per row */
    int visible[EVO_GRID_ROWS];  /* items that fit per row */

    /*
     * Fixed-point (<<8) x of the highlight, so a shelf cursor slides between
     * tiles instead of teleporting. Lists have glided since evo_focus existed;
     * without this the launch screen was the one place that snapped, and it
     * made the home screen feel stiffer than the sections for no reason.
     */
    int glide_fp;
    int glide_target_fp;
    int glide_ready;

    int settled_frames;
} evo_grid;

void evo_grid_init(evo_grid *g, int rows);

/* Declare a row's contents. Safe to call every frame. */
void evo_grid_set_row(evo_grid *g, int row, int count, int visible);

evo_move_result evo_grid_move_h(evo_grid *g, int delta);
evo_move_result evo_grid_move_v(evo_grid *g, int delta);

/* Cursor within the current row, or -1 if it is empty. */
int evo_grid_col(const evo_grid *g);

/* Cursor and scroll for an arbitrary row, for drawing. */
int evo_grid_row_col(const evo_grid *g, int row);
int evo_grid_row_scroll(const evo_grid *g, int row);

/* Does `row` currently hold the cursor? */
int evo_grid_row_active(const evo_grid *g, int row);

/*
 * Per-frame update for the horizontal highlight. `origin` is the x of the
 * first visible tile and `pitch` the distance between tiles.
 */
void evo_grid_tick(evo_grid *g, int origin, int pitch);

/* Current highlight x in pixels - the glided position, not the tile grid. */
int  evo_grid_glide_x(const evo_grid *g);

#ifdef __cplusplus
}
#endif

#endif /* EVO_FOCUS_H */
