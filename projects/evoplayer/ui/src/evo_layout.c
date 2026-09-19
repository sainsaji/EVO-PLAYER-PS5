/*
 * evo_layout — the geometry helpers that outlived the immediate-mode screen
 * renderer.
 *
 * When the RmlUi migration retired ui/src/evo_screens.c and ui/src/evo_chrome.c
 * (#44), a handful of pure layout functions still had callers on BOTH paths:
 * main.c fills the RmlUi params from `evo_*_model` structs and needs the same
 * capacity / wrap-width / grid-sync maths the SDF screens used, so the cursor
 * and the DOM stay in step. Those functions live here now; nothing in this
 * file draws.
 */
#include "evo_chrome.h"
#include "evo_screens.h"
#include "evo_draw.h"
#include "evo_metrics.h"

#include <string.h>

/* ==========================================================================
 * Standard footer hint sets (were in evo_chrome.c)
 * ========================================================================== */

const evo_hint EVO_HINTS_LIST[3] = {
    { EVO_GLYPH_CROSS,  "SELECT" },
    { EVO_GLYPH_CIRCLE, "BACK"   },
    { EVO_GLYPH_DPAD,   "MOVE"   }
};
const int EVO_HINTS_LIST_N = 3;

/* ==========================================================================
 * Content bounds (were in evo_chrome.c)
 * ========================================================================== */

int evo_chrome_content_x(const evo_page *p)
{
    return (p && p->no_rail) ? EVO_BLEED_X : EVO_CONTENT_X;
}

int evo_chrome_content_r(const evo_page *p)
{
    return (p && p->no_rail) ? EVO_BLEED_R : EVO_CONTENT_R;
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

/* ==========================================================================
 * Side navigation step (was in evo_chrome.c)
 * ========================================================================== */

int evo_sidenav_step(int index, int delta)
{
    int n = EVO_SECTION_COUNT;

    index = (index + delta) % n;
    if (index < 0) index += n;

    return index;
}

/* ==========================================================================
 * Launch grid sync + activation (were in evo_screens.c)
 * ========================================================================== */

void evo_screen_launch_sync(evo_grid *g, const evo_launch_model *m)
{
    int recent = m ? m->recent_count : 0;

    if (recent > EVO_LAUNCH_RECENT_MAX) recent = EVO_LAUNCH_RECENT_MAX;

    /* The hero is a single wide target, so it is a one-item row. */
    evo_grid_set_row(g, EVO_LAUNCH_ROW_HERO,    1,      1);
    evo_grid_set_row(g, EVO_LAUNCH_ROW_RECENT,  recent, EVO_TILES_VISIBLE);
    evo_grid_set_row(g, EVO_LAUNCH_ROW_LIBRARY, EVO_SECTION_COUNT - 1,
                     EVO_TILES_VISIBLE);
}

/*
 * The library shelf lists every section except HOME - offering "go home" on
 * the home screen is noise, and it would push the shelf to seven tiles and
 * off the six-tile grid the whole page is built on.
 */
static evo_section library_section(int col)
{
    return (evo_section)(col + 1);
}

evo_screen_id evo_screen_launch_activate(const evo_grid *g,
                                         const evo_launch_model *m,
                                         int *out_recent_index,
                                         int *out_play_hero)
{
    int col = evo_grid_col(g);

    if (out_recent_index) *out_recent_index = -1;
    if (out_play_hero)    *out_play_hero    = 0;

    switch (g->row) {
        case EVO_LAUNCH_ROW_HERO:
            /* With nothing to resume the hero sends you to the browser,
             * which is what its label promises in that state. */
            if (m && m->has_resume) {
                if (out_play_hero) *out_play_hero = 1;
                return EVO_SCREEN_LAUNCH;
            }
            return EVO_SCREEN_BROWSER;

        case EVO_LAUNCH_ROW_RECENT:
            if (col >= 0 && out_recent_index) *out_recent_index = col;
            return EVO_SCREEN_LAUNCH;

        case EVO_LAUNCH_ROW_LIBRARY:
            if (col < 0) return EVO_SCREEN_LAUNCH;
            return evo_section_get(library_section(col))->screen;

        default:
            return EVO_SCREEN_LAUNCH;
    }
}

/* ==========================================================================
 * Row-capacity helpers (were in evo_screens.c)
 * ========================================================================== */

int evo_screen_browser_capacity(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    return evo_chrome_row_capacity(&page);
}

int evo_screen_list_capacity(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    return evo_chrome_row_capacity(&page);
}

/* ==========================================================================
 * Subtitle picker capacity (were in evo_screens.c)
 * ========================================================================== */

#define EVO_PICKER_PITCH    70
#define EVO_PICKER_HEAD    132   /* eyebrow + title above the first row */
#define EVO_PICKER_FOOT     84   /* hint strip below the last row       */

int evo_screen_picker_capacity(void)
{
    /*
     * Derived from the tallest panel that still leaves the subtitle preview
     * visible below it, so size switching is immediately apparent.
     */
    int max_body = EVO_SCREEN_H - 360 - EVO_PICKER_HEAD - EVO_PICKER_FOOT;
    int rows     = max_body / EVO_PICKER_PITCH;

    if (rows < 3) rows = 3;
    if (rows > 7) rows = 7;

    return rows;
}

/* ==========================================================================
 * Text reader geometry (were in evo_screens.c)
 * ========================================================================== */

/* Cell heights of the four faces, from the atlas (see tools/measure_font.py).
 * Pitch adds leading on top; the ratio is what makes long text readable
 * rather than dense. */
static int reader_face_h(int face)
{
    switch (face) {
    case EVO_FACE_TITLE: return 57;
    case EVO_FACE_MENU:  return 46;
    case EVO_FACE_SUB:   return 29;
    default:             return 25;
    }
}

int evo_screen_reader_pitch(int face)
{
    return reader_face_h(face) + (face >= EVO_FACE_MENU ? 12 : 9);
}

int evo_screen_reader_capacity(int face)
{
    int n = (EVO_CONTENT_B - EVO_CONTENT_Y) / evo_screen_reader_pitch(face);

    if (n < 1) n = 1;
    if (n > EVO_READER_MAX_VISIBLE) n = EVO_READER_MAX_VISIBLE;
    return n;
}

/* Must match the column the reader draws into. */
#define EVO_READER_SCROLLBAR_GUTTER 34

int evo_screen_reader_wrap_w(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    page.section = EVO_SECTION_BROWSER;

    return evo_chrome_content_r(&page) - evo_chrome_content_x(&page)
           - EVO_READER_SCROLLBAR_GUTTER;
}
