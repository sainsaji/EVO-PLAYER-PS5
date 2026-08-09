#include "evo_screens.h"
#include "evo_chrome.h"
#include "evo_draw.h"
#include "evo_metrics.h"
#include "evo_theme.h"
#include "evo_ui.h"
#include "evo_widgets.h"

#include <stdio.h>
#include <string.h>

static uint32_t with_alpha(uint32_t c, int a)
{
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

/* ==========================================================================
 * Launch screen
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

static void draw_launch_header(uint32_t *fb, const evo_launch_model *m)
{
    const evo_theme *th = evo_theme_current();
    int x  = EVO_BLEED_X;
    int cy = 88;

    evo_ui_circle(fb, x + 22, cy, 24, with_alpha(th->accent_soft, 80));
    evo_ui_circle(fb, x + 22, cy, 14, th->accent);

    evo_text(fb, x + 60, evo_text_y_centred(cy - 26, 32, EVO_FACE_MENU),
             "EVO PLAYER", th->text_primary, EVO_FACE_MENU);

#ifdef EVO_PLAYER_VERSION
    evo_text(fb, x + 60, evo_text_y_centred(cy + 6, 26, EVO_FACE_SMALL),
             "VERSION " EVO_PLAYER_VERSION, th->text_muted, EVO_FACE_SMALL);
#endif

    /* Right side: theme name and clock, the two things worth glancing at. */
    {
        int rx = EVO_BLEED_R;

        if (m && m->clock && *m->clock) {
            evo_text_right(fb, rx, evo_text_y_centred(cy - 26, 32,
                                                      EVO_FACE_MENU),
                           m->clock, th->text_primary, EVO_FACE_MENU);
        }

        if (m && m->theme_name && *m->theme_name) {
            evo_text_right(fb, rx, evo_text_y_centred(cy + 6, 26,
                                                      EVO_FACE_SMALL),
                           m->theme_name, th->text_muted, EVO_FACE_SMALL);
        }
    }
}

static void draw_hero(uint32_t *fb, const evo_launch_model *m, int selected)
{
    const evo_theme *th = evo_theme_current();
    const int x = EVO_BLEED_X;
    const int y = EVO_HERO_Y;
    const int w = EVO_BLEED_W;
    const int h = EVO_HERO_H;
    int text_x  = x + 56;
    int title_y, detail_y;

    evo_ui_card(fb, x, y, w, h, selected);

    if (m && m->hero_art.pixels && m->hero_art.w > 0 && m->hero_art.h > 0) {
        /*
         * Artwork spans the whole card and fades out towards the left, so the
         * copy sits on flat surface colour while the frame still reads as a
         * backdrop rather than as a panel stuck to one side. An art region
         * that starts partway across leaves a hard vertical seam down the
         * middle of the hero, which is exactly what it looked like.
         */
        const int art_x = x + 2;
        const int art_w = w - 4;
        const int art_h = h - 4;

        /* Cover-crop rather than stretch. The source is 16:9 and the hero is
         * roughly 5.5:1, so scaling to fit the box makes faces visibly wide.
         * Scale on the wider ratio and take the middle band of the source. */
        const int sw = m->hero_art.w;
        const int sh = m->hero_art.h;

        /* Fixed point 16.16 source-pixels-per-dest-pixel, using the axis that
         * needs the most magnification so neither axis is left short. */
        int step_x = (sw << 16) / art_w;
        int step_y = (sh << 16) / art_h;
        int step   = step_x < step_y ? step_x : step_y;

        int span_x = (art_w * step) >> 16;
        int span_y = (art_h * step) >> 16;
        int src_x0 = (sw - span_x) / 2;
        int src_y0 = (sh - span_y) / 2;

        const int fade_from = (art_w * 28) / 100;
        const int fade_to   = (art_w * 60) / 100;

        int i;

        if (src_x0 < 0) src_x0 = 0;
        if (src_y0 < 0) src_y0 = 0;

        for (i = 0; i < art_w; i++) {
            int alpha;
            int sx = src_x0 + ((i * step) >> 16);
            int j;

            if (i <= fade_from)    alpha = 0;
            else if (i >= fade_to) alpha = 255;
            else                   alpha = ((i - fade_from) * 255) /
                                           (fade_to - fade_from);

            if (alpha == 0) continue;
            if (sx >= sw) sx = sw - 1;

            for (j = 0; j < art_h; j++) {
                int sy = src_y0 + ((j * step) >> 16);
                int fx = art_x + i;
                int fy = y + 2 + j;

                if (sy >= sh) sy = sh - 1;
                if ((unsigned)fx >= EVO_SCREEN_W) continue;
                if ((unsigned)fy >= EVO_SCREEN_H) continue;

                fb[fy * EVO_SCREEN_W + fx] =
                    evo_ui_blend(fb[fy * EVO_SCREEN_W + fx],
                                 m->hero_art.pixels[sy * sw + sx], alpha);
            }
        }

        /* Bottom scrim across the full card, so the chip and the progress bar
         * sit on a consistent ground and the band has no visible left edge. */
        evo_ui_vgrad(fb, art_x, y + h - 140, art_w, 138,
                     with_alpha(th->scrim, 0), with_alpha(th->scrim, 215));

        if (selected)
            evo_ui_round_rect(fb, x, y, w, h, th->radius,
                              with_alpha(th->surface_sel, 0),
                              with_alpha(th->surface_sel, 0),
                              th->border_sel, th->border_px + 1,
                              with_alpha(th->shadow, 0), 0);
    }

    if (!m) return;

    evo_text(fb, text_x, y + 46,
             m->has_resume ? "CONTINUE WATCHING" : "WELCOME",
             th->accent, EVO_FACE_SMALL);

    evo_text_y_stacked(y + 84, 120, EVO_FACE_TITLE, EVO_FACE_SUB, 16,
                       &title_y, &detail_y);

    evo_text_fit(fb, text_x, title_y, w / 2 - 40,
                 m->hero_title ? m->hero_title : "EVO PLAYER",
                 th->text_primary, EVO_FACE_TITLE);

    if (m->hero_detail && *m->hero_detail)
        evo_text_fit(fb, text_x, detail_y, w / 2 - 40, m->hero_detail,
                     th->text_secondary, EVO_FACE_SUB);

    if (m->hero_progress >= 0)
        evo_widget_progress(fb, text_x, y + h - 96, w / 3, 6,
                            m->hero_progress);

    /* Call-to-action chip. Explicit rather than implied, because the hero is
     * the one target on this page whose action is not obvious from its
     * label. */
    if (m->hero_action && *m->hero_action) {
        int cw = evo_text_w(m->hero_action, EVO_FACE_SUB) + 92;
        int ch = 52;
        int cy = y + h - 76;

        evo_ui_round_rect(fb, text_x, cy, cw, ch, ch / 2,
                          selected ? th->accent : with_alpha(th->surface, 240),
                          selected ? th->accent : with_alpha(th->surface_alt, 240),
                          selected ? th->accent : th->border, th->border_px,
                          with_alpha(th->shadow, 0), 0);

        evo_glyph(fb, text_x + 14, cy + (ch - 40) / 2 - 2, EVO_GLYPH_CROSS);

        evo_text(fb, text_x + 62,
                 evo_text_y_centred(cy, ch, EVO_FACE_SUB),
                 m->hero_action,
                 selected ? th->bg_bottom : th->text_primary,
                 EVO_FACE_SUB);
    }
}

static void draw_shelf(uint32_t *fb, int label_y, int tiles_y,
                       const char *label,
                       const evo_grid *g, int row,
                       const evo_launch_item *items, int count,
                       int use_sections)
{
    const evo_theme *th = evo_theme_current();
    int scroll = evo_grid_row_scroll(g, row);
    int cursor = evo_grid_row_col(g, row);
    int active = evo_grid_row_active(g, row);
    int i;

    evo_widget_shelf_label(fb, EVO_BLEED_X, label_y, label);

    /* "n of m" on the right when the shelf scrolls, so it is clear there is
     * more off the edge than the six tiles on screen. */
    if (count > EVO_TILES_VISIBLE && cursor >= 0) {
        char pos[32];
        snprintf(pos, sizeof(pos), "%d OF %d", cursor + 1, count);
        evo_text_right(fb, EVO_BLEED_R, label_y, pos,
                       th->text_muted, EVO_FACE_SMALL);
    }

    for (i = 0; i < EVO_TILES_VISIBLE; i++) {
        int index = scroll + i;
        int x     = EVO_BLEED_X + i * EVO_TILE_PITCH;
        evo_tile  tile;

        if (index >= count) break;

        memset(&tile, 0, sizeof(tile));
        tile.icon     = -1;
        tile.progress = -1;
        tile.selected = (active && index == cursor);

        if (use_sections) {
            const evo_section_info *info =
                evo_section_get(library_section(index));

            tile.title  = info->label;
            tile.detail = info->blurb;
            tile.icon   = info->icon;
        } else {
            tile.title    = items[index].title;
            tile.detail   = items[index].detail;
            tile.progress = items[index].progress;
            tile.art      = items[index].art.pixels;
            tile.art_w    = items[index].art.w;
            tile.art_h    = items[index].art.h;
            /* Inset: covers are cached at 80x80, and stretched over a 274px
             * tile they are a 3x upscale that reads as a broken image. */
            tile.art_inset = 1;
            tile.icon      = (tile.art ? -1 : EVO_IC_RECENT);
        }

        evo_widget_tile(fb, x, tiles_y, EVO_TILE_W, EVO_TILE_H, &tile);
    }
}

void evo_screen_launch(uint32_t *fb, const evo_launch_model *m,
                       const evo_grid *g)
{
    evo_page page;
    int recent = m ? m->recent_count : 0;

    if (recent > EVO_LAUNCH_RECENT_MAX) recent = EVO_LAUNCH_RECENT_MAX;

    memset(&page, 0, sizeof(page));
    page.no_rail = 1;
    page.section = EVO_SECTION_HOME;

    evo_ui_background(fb);
    draw_launch_header(fb, m);

    draw_hero(fb, m, evo_grid_row_active(g, EVO_LAUNCH_ROW_HERO));

    if (recent > 0) {
        draw_shelf(fb, EVO_SHELF1_LABEL_Y, EVO_SHELF1_Y, "JUMP BACK IN",
                   g, EVO_LAUNCH_ROW_RECENT, m->recent, recent, 0);

        draw_shelf(fb, EVO_SHELF2_LABEL_Y, EVO_SHELF2_Y, "LIBRARY",
                   g, EVO_LAUNCH_ROW_LIBRARY, 0, EVO_SECTION_COUNT - 1, 1);
    } else {
        /*
         * With no history the second shelf moves up into the first slot
         * rather than leaving a labelled gap. The grid still calls it row 2,
         * so navigation is unaffected.
         */
        draw_shelf(fb, EVO_SHELF1_LABEL_Y, EVO_SHELF1_Y, "LIBRARY",
                   g, EVO_LAUNCH_ROW_LIBRARY, 0, EVO_SECTION_COUNT - 1, 1);
    }

    {
        const evo_hint hints[3] = {
            { EVO_GLYPH_CROSS, "SELECT" },
            { EVO_GLYPH_DPAD,  "MOVE"   },
            { EVO_GLYPH_LSTICK, "SCREENSHOT" }
        };
        evo_chrome_end(fb, &page, hints, 3);
    }
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
 * File browser
 * ========================================================================== */

static int browser_icon(evo_file_kind kind)
{
    switch (kind) {
        case EVO_FILE_FOLDER:   return EVO_IC_FOLDER;
        case EVO_FILE_VIDEO:    return EVO_IC_RESUME;
        case EVO_FILE_AUDIO:    return EVO_IC_RECENT;
        case EVO_FILE_IMAGE:    return EVO_IC_ASPECT;
        case EVO_FILE_SUBTITLE: return EVO_IC_SUBTITLES;
        default:                return EVO_IC_ABOUT;
    }
}

int evo_screen_browser_capacity(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    return evo_chrome_row_capacity(&page);
}

static void draw_inspector(uint32_t *fb, const evo_browser_inspect *ins)
{
    const evo_theme *th = evo_theme_current();
    const int x = EVO_INSPECT_X;
    const int y = EVO_CONTENT_Y;
    int py;

    if (!ins) {
        evo_widget_preview(fb, x, y, EVO_PREVIEW_W, EVO_PREVIEW_H,
                           0, 0, 0, 0, EVO_IC_FOLDER);
        return;
    }

    evo_widget_preview(fb, x, y, EVO_PREVIEW_W, EVO_PREVIEW_H,
                       ins->preview.pixels, ins->preview.w, ins->preview.h,
                       ins->preview_badge, EVO_IC_USB);

    py = y + EVO_PREVIEW_H + 20;

    /* Filename above the table: it is the one field that can be long enough
     * to need the full panel width. */
    if (ins->name && *ins->name) {
        evo_text_fit(fb, x, py, EVO_INSPECT_W, ins->name,
                     th->text_primary, EVO_FACE_MENU);
        py += 42;
        evo_ui_hline(fb, x, py, EVO_INSPECT_W, with_alpha(th->border, 130));
        py += 14;
    }

    {
        /*
         * Fixed order, so the panel does not reshuffle as fields arrive from
         * the background probe. Empty values are skipped by the widget.
         */
        const evo_prop props[] = {
            { "TYPE",       ins->kind        },
            { "FORMAT",     ins->extension   },
            { "CONTAINER",  ins->container   },
            { "SIZE",       ins->size        },
            { "LENGTH",     ins->duration    },
            { "RESOLUTION", ins->resolution  },
            { "VIDEO",      ins->video_codec },
            { "AUDIO",      ins->audio_codec },
            { "SUBTITLES",  ins->subtitles   }
        };

        py = evo_widget_props(fb, x, py, EVO_INSPECT_W, props,
                              (int)(sizeof(props) / sizeof(props[0])));
    }

    if (ins->probing) {
        evo_text(fb, x, py + 12, "READING MEDIA...",
                 with_alpha(th->text_muted, 190), EVO_FACE_SMALL);
    }
}

void evo_screen_browser(uint32_t *fb, const evo_browser_model *m,
                        const evo_focus *f, int rail_focused, int rail_index)
{
    const evo_theme *th = evo_theme_current();
    evo_page page;
    char     badge[32];
    int      i;
    int      list_x = EVO_CONTENT_X;
    int      list_w = EVO_LIST_W;

    memset(&page, 0, sizeof(page));
    page.title        = "BROWSE";
    page.subtitle     = m ? m->path : "";
    page.section      = EVO_SECTION_BROWSER;
    page.rail_focused = rail_focused;
    page.rail_index   = rail_index;

    if (m && m->count > 0) {
        snprintf(badge, sizeof(badge), "%d OF %d", f->index + 1, m->count);
        page.badge = badge;
    } else {
        page.badge = "EMPTY";
    }

    evo_chrome_begin(fb, &page);

    if (!m || m->count == 0) {
        evo_widget_empty(fb, list_x, EVO_CONTENT_Y, list_w, EVO_CONTENT_H,
                         "THIS FOLDER IS EMPTY",
                         m && m->at_root ? "CONNECT A USB DRIVE WITH MEDIA"
                                         : "PRESS CIRCLE TO GO BACK",
                         EVO_IC_USB);
        draw_inspector(fb, 0);
        evo_chrome_end(fb, &page, EVO_HINTS_BROWSE, EVO_HINTS_BROWSE_N);
        return;
    }

    /* Resting cards first, then the glided highlight, then all content on
     * top - so the highlight never paints over a row's text mid-glide. */
    for (i = 0; i < f->visible; i++) {
        int index = f->scroll + i;
        if (index >= m->count) break;

        evo_ui_card(fb, list_x, EVO_CONTENT_Y + i * EVO_ROW_PITCH,
                    list_w, EVO_ROW_H, 0);
    }

    evo_widget_row_highlight(fb, list_x, evo_focus_glide_y(f),
                             list_w, EVO_ROW_H);

    for (i = 0; i < f->visible; i++) {
        int index = f->scroll + i;
        int slot  = index - m->first;
        evo_row row;
        const evo_browser_entry *e;

        if (index >= m->count) break;
        if (slot < 0 || slot >= m->entry_count) continue;

        e = &m->entries[slot];

        memset(&row, 0, sizeof(row));
        row.title      = e->name;
        row.detail     = e->detail;
        row.icon       = browser_icon(e->kind);
        row.selected   = (index == f->index);
        row.chevron    = (e->kind == EVO_FILE_FOLDER);
        row.badge_icon = e->favorite ? EVO_IC_FAVORITE : -1;
        row.progress   = e->progress;
        row.marquee_phase = row.selected ? f->settled_frames : 0;

        evo_widget_row(fb, list_x, EVO_CONTENT_Y + i * EVO_ROW_PITCH,
                       list_w, EVO_ROW_H, &row);
    }

    evo_widget_scrollbar(fb, list_x + list_w + 12, EVO_CONTENT_Y,
                         EVO_CONTENT_H, evo_focus_scroll_permille(f),
                         f->visible, m->count);

    /* A hairline between the list and the inspector: without it the two
     * columns read as one wide, ragged block. */
    evo_ui_vline(fb, EVO_INSPECT_X - EVO_INSPECT_GAP / 2, EVO_CONTENT_Y,
                 EVO_CONTENT_H, with_alpha(th->border, 110));

    draw_inspector(fb, m->inspect);

    evo_chrome_end(fb, &page, EVO_HINTS_BROWSE, EVO_HINTS_BROWSE_N);
}

/* ==========================================================================
 * Generic list page
 * ========================================================================== */

int evo_screen_list_capacity(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    return evo_chrome_row_capacity(&page);
}

void evo_screen_list(uint32_t *fb, const evo_list_model *m,
                     const evo_focus *f, int rail_focused, int rail_index,
                     const evo_hint *hints, int hint_count)
{
    evo_page page;
    char     badge[32];
    int      x = EVO_CONTENT_X;
    /*
     * Reserve the scrollbar gutter inside the content area. At full width the
     * bar landed at x=1868, past the title-safe inset and effectively
     * invisible on a TV - so a list that scrolled gave no sign that it did.
     */
    int      w = EVO_CONTENT_W - 24;
    int      i;

    memset(&page, 0, sizeof(page));
    page.title        = m->title;
    page.subtitle     = m->subtitle;
    page.section      = m->section;
    page.rail_focused = rail_focused;
    page.rail_index   = rail_index;

    if (m->count > 0) {
        snprintf(badge, sizeof(badge), "%d OF %d", f->index + 1, m->count);
        page.badge = badge;
    }

    evo_chrome_begin(fb, &page);

    if (m->count == 0) {
        evo_widget_empty(fb, x, EVO_CONTENT_Y, w, EVO_CONTENT_H,
                         m->empty_title, m->empty_hint, m->empty_icon);
        evo_chrome_end(fb, &page, hints, hint_count);
        return;
    }

    for (i = 0; i < f->visible; i++) {
        int index = f->scroll + i;
        if (index >= m->count) break;

        evo_ui_card(fb, x, EVO_CONTENT_Y + i * EVO_ROW_PITCH,
                    w, EVO_ROW_H, 0);
    }

    evo_widget_row_highlight(fb, x, evo_focus_glide_y(f), w, EVO_ROW_H);

    for (i = 0; i < f->visible; i++) {
        int index = f->scroll + i;
        evo_row row;

        if (index >= m->count) break;

        memset(&row, 0, sizeof(row));
        row.title         = m->entries[index].title;
        row.detail        = m->entries[index].detail;
        row.icon          = m->entries[index].icon;
        row.chevron       = m->entries[index].chevron;
        row.progress      = m->entries[index].progress;
        row.selected      = (index == f->index);
        row.badge_icon    = -1;
        row.marquee_phase = row.selected ? f->settled_frames : 0;

        evo_widget_row(fb, x, EVO_CONTENT_Y + i * EVO_ROW_PITCH,
                       w, EVO_ROW_H, &row);
    }

    evo_widget_scrollbar(fb, x + w + 10, EVO_CONTENT_Y, EVO_CONTENT_H,
                         evo_focus_scroll_permille(f), f->visible, m->count);

    evo_chrome_end(fb, &page, hints, hint_count);
}
