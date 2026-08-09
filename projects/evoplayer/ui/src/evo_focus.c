#include "evo_focus.h"
#include "evo_metrics.h"

static void clamp_scroll(evo_focus *f)
{
    if (f->visible <= 0) { f->scroll = 0; return; }

    if (f->index < f->scroll)
        f->scroll = f->index;

    if (f->index >= f->scroll + f->visible)
        f->scroll = f->index - f->visible + 1;

    /*
     * Keep the window full at the bottom of the list. Without this, scrolling
     * to the last item of a 7-item list with 6 visible leaves one blank row
     * and the list appears to have lost an entry.
     */
    if (f->scroll > f->count - f->visible)
        f->scroll = f->count - f->visible;

    if (f->scroll < 0)
        f->scroll = 0;
}

void evo_focus_init(evo_focus *f, int count, int visible, int wrap)
{
    f->index           = 0;
    f->count           = count < 0 ? 0 : count;
    f->scroll          = 0;
    f->visible         = visible < 1 ? 1 : visible;
    f->wrap            = wrap;
    f->glide_fp        = 0;
    f->glide_target_fp = 0;
    f->glide_ready     = 0;
    f->settled_frames  = 0;
}

void evo_focus_set_count(evo_focus *f, int count)
{
    if (count < 0) count = 0;
    if (f->count == count) return;

    f->count = count;

    if (count == 0) {
        f->index  = 0;
        f->scroll = 0;
        return;
    }

    if (f->index >= count)
        f->index = count - 1;

    clamp_scroll(f);
}

static evo_move_result finish_move(evo_focus *f, int before, int wrapped)
{
    clamp_scroll(f);

    if (f->index == before)
        return wrapped ? EVO_MOVE_WRAPPED : EVO_MOVE_BLOCKED;

    f->settled_frames = 0;
    return wrapped ? EVO_MOVE_WRAPPED : EVO_MOVE_OK;
}

evo_move_result evo_focus_move(evo_focus *f, int delta)
{
    int before = f->index;
    int next;
    int wrapped = 0;

    if (f->count <= 0) return EVO_MOVE_NONE;
    if (delta == 0)    return EVO_MOVE_BLOCKED;

    next = f->index + delta;

    if (f->wrap) {
        /* Positive modulo: -1 must land on the last item, not on -1. */
        next %= f->count;
        if (next < 0) next += f->count;
        wrapped = ((delta > 0 && next < before) || (delta < 0 && next > before));
    } else {
        if (next < 0)            next = 0;
        if (next >= f->count)    next = f->count - 1;
    }

    f->index = next;
    return finish_move(f, before, wrapped);
}

evo_move_result evo_focus_page(evo_focus *f, int direction)
{
    int before = f->index;
    int step;

    if (f->count <= 0) return EVO_MOVE_NONE;

    step = f->visible > 1 ? f->visible - 1 : 1;
    f->index += (direction >= 0 ? step : -step);

    if (f->index < 0)         f->index = 0;
    if (f->index >= f->count) f->index = f->count - 1;

    return finish_move(f, before, 0);
}

evo_move_result evo_focus_first(evo_focus *f)
{
    int before = f->index;
    if (f->count <= 0) return EVO_MOVE_NONE;
    f->index = 0;
    return finish_move(f, before, 0);
}

evo_move_result evo_focus_last(evo_focus *f)
{
    int before = f->index;
    if (f->count <= 0) return EVO_MOVE_NONE;
    f->index = f->count - 1;
    return finish_move(f, before, 0);
}

evo_move_result evo_focus_set(evo_focus *f, int index)
{
    int before = f->index;

    if (f->count <= 0) return EVO_MOVE_NONE;

    if (index < 0)         index = 0;
    if (index >= f->count) index = f->count - 1;

    f->index = index;
    return finish_move(f, before, 0);
}

void evo_focus_tick(evo_focus *f, int row_origin, int row_pitch)
{
    int vis = evo_focus_visible_index(f);
    int distance;

    f->settled_frames++;

    if (vis < 0) {
        f->glide_ready = 0;
        return;
    }

    f->glide_target_fp = (row_origin + vis * row_pitch) << 8;

    if (!f->glide_ready) {
        f->glide_fp    = f->glide_target_fp;
        f->glide_ready = 1;
        return;
    }

    distance = f->glide_target_fp - f->glide_fp;

    /*
     * Snap when close. An exponential approach never actually arrives, and
     * the leftover sub-pixel drift shows up as a highlight that shimmers
     * against the card underneath it for as long as the screen is open.
     */
    if (distance > -EVO_GLIDE_SNAP_FP && distance < EVO_GLIDE_SNAP_FP)
        f->glide_fp = f->glide_target_fp;
    else
        f->glide_fp += distance / EVO_GLIDE_DIV;
}

int evo_focus_glide_y(const evo_focus *f)
{
    return f->glide_fp >> 8;
}

int evo_focus_visible_index(const evo_focus *f)
{
    int vis;

    if (f->count <= 0) return -1;

    vis = f->index - f->scroll;

    if (vis < 0)            vis = 0;
    if (vis >= f->visible)  vis = f->visible - 1;

    return vis;
}

int evo_focus_is_visible(const evo_focus *f, int index)
{
    return index >= f->scroll && index < f->scroll + f->visible;
}

int evo_focus_scroll_permille(const evo_focus *f)
{
    int span = f->count - f->visible;

    if (f->count <= 0 || span <= 0) return -1;

    return (f->scroll * 1000) / span;
}

/* ---- two-dimensional focus ---------------------------------------------- */

static void grid_clamp_row(evo_grid *g, int row)
{
    int count   = g->count[row];
    int visible = g->visible[row] > 0 ? g->visible[row] : 1;

    if (count <= 0) {
        g->col[row]    = 0;
        g->scroll[row] = 0;
        return;
    }

    if (g->col[row] >= count) g->col[row] = count - 1;
    if (g->col[row] < 0)      g->col[row] = 0;

    if (g->col[row] < g->scroll[row])
        g->scroll[row] = g->col[row];

    if (g->col[row] >= g->scroll[row] + visible)
        g->scroll[row] = g->col[row] - visible + 1;

    if (g->scroll[row] > count - visible) g->scroll[row] = count - visible;
    if (g->scroll[row] < 0)               g->scroll[row] = 0;
}

void evo_grid_init(evo_grid *g, int rows)
{
    int i;

    if (rows < 1)              rows = 1;
    if (rows > EVO_GRID_ROWS)  rows = EVO_GRID_ROWS;

    g->row  = 0;
    g->rows = rows;
    g->settled_frames = 0;

    for (i = 0; i < EVO_GRID_ROWS; i++) {
        g->count[i]   = 0;
        g->col[i]     = 0;
        g->scroll[i]  = 0;
        g->visible[i] = 1;
    }
}

void evo_grid_set_row(evo_grid *g, int row, int count, int visible)
{
    if (row < 0 || row >= g->rows) return;

    g->count[row]   = count < 0 ? 0 : count;
    g->visible[row] = visible < 1 ? 1 : visible;

    grid_clamp_row(g, row);

    /*
     * If the cursor is parked on a row that has just become empty - the last
     * favourite was removed, say - move it somewhere it can act.
     */
    if (g->row == row && g->count[row] == 0)
        evo_grid_move_v(g, 1);
}

evo_move_result evo_grid_move_h(evo_grid *g, int delta)
{
    int row = g->row;
    int before;

    if (row < 0 || row >= g->rows) return EVO_MOVE_NONE;
    if (g->count[row] <= 0)        return EVO_MOVE_NONE;

    before = g->col[row];
    g->col[row] += delta;

    /*
     * Shelves clamp rather than wrap. A shelf is a strip you scan along; a
     * cursor that teleports from the last poster back to the first reads as
     * a glitch, not as navigation. Vertical movement between shelves does
     * wrap, because there are only a few rows and the mapping is obvious.
     */
    if (g->col[row] < 0)                g->col[row] = 0;
    if (g->col[row] >= g->count[row])   g->col[row] = g->count[row] - 1;

    grid_clamp_row(g, row);

    if (g->col[row] == before) return EVO_MOVE_BLOCKED;

    g->settled_frames = 0;
    return EVO_MOVE_OK;
}

evo_move_result evo_grid_move_v(evo_grid *g, int delta)
{
    int before = g->row;
    int step   = (delta >= 0) ? 1 : -1;
    int tried;
    int row    = g->row;

    if (g->rows <= 0) return EVO_MOVE_NONE;

    /* Walk in `step` until a non-empty row turns up, at most once round. */
    for (tried = 0; tried < g->rows; tried++) {
        row += step;

        if (row < 0)         row = g->rows - 1;
        if (row >= g->rows)  row = 0;

        if (g->count[row] > 0) {
            g->row = row;
            grid_clamp_row(g, row);

            if (g->row == before) return EVO_MOVE_BLOCKED;

            g->settled_frames = 0;
            return (step > 0 && g->row < before) ||
                   (step < 0 && g->row > before)
                       ? EVO_MOVE_WRAPPED : EVO_MOVE_OK;
        }
    }

    /* Every row is empty. */
    return EVO_MOVE_NONE;
}

int evo_grid_col(const evo_grid *g)
{
    if (g->row < 0 || g->row >= g->rows) return -1;
    if (g->count[g->row] <= 0)           return -1;
    return g->col[g->row];
}

int evo_grid_row_col(const evo_grid *g, int row)
{
    if (row < 0 || row >= g->rows) return -1;
    if (g->count[row] <= 0)        return -1;
    return g->col[row];
}

int evo_grid_row_scroll(const evo_grid *g, int row)
{
    if (row < 0 || row >= g->rows) return 0;
    return g->scroll[row];
}

int evo_grid_row_active(const evo_grid *g, int row)
{
    return g->row == row;
}
