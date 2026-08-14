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
    /*
     * The logo, at its native 72px, with the wordmark beside it. It replaced
     * two concentric circles - see draw_app_mark() in evo_chrome.c for why.
     *
     * The icon starts at the bleed margin rather than being centred on it, so
     * the mark shares its left edge with the hero and both shelves; a 72px
     * glyph centred where the old 24px circle was would have hung 14px into
     * the title-safe margin.
     */
    int text_x = x + EVO_RAIL_ICON + 20;

    evo_ui_circle(fb, x + EVO_RAIL_ICON / 2, cy, 32,
                  with_alpha(th->accent_soft, 70));
    evo_icon_tinted(fb, x, cy - EVO_RAIL_ICON / 2, EVO_IC_LOGO, th->accent);

    evo_text(fb, text_x, evo_text_y_centred(cy - 26, 32, EVO_FACE_MENU),
             "EVO PLAYER", th->text_primary, EVO_FACE_MENU);

#ifdef EVO_PLAYER_VERSION
    evo_text(fb, text_x, evo_text_y_centred(cy + 6, 26, EVO_FACE_SMALL),
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
        evo_ui_vgrad_over(fb, art_x, y + h - 140, art_w, 138,
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
        /*
         * Chip geometry, spelled out because guessing it put the glyph and
         * the label on top of each other: 14 left pad, a 48px controller
         * glyph, 14 of gap, the label, 18 right pad.
         */
        const int pad_l = 14, glyph_w = 48, gap = 14, pad_r = 18;
        const int text_x_off = pad_l + glyph_w + gap;
        int cw = text_x_off + evo_text_w(m->hero_action, EVO_FACE_SUB) + pad_r;
        int ch = 52;
        int cy = y + h - 76;

        evo_ui_round_rect(fb, text_x, cy, cw, ch, ch / 2,
                          selected ? th->accent : with_alpha(th->surface, 240),
                          selected ? th->accent : with_alpha(th->surface_alt, 240),
                          selected ? th->accent : th->border, th->border_px,
                          with_alpha(th->shadow, 0), 0);

        /* On the selected chip the fill IS the accent, so the glyph has to
         * flip to the same dark colour as the label or it disappears. */
        evo_glyph_tinted(fb, text_x + pad_l, cy + (ch - glyph_w) / 2,
                         EVO_GLYPH_CROSS,
                         selected ? th->bg_bottom : th->accent);

        evo_text(fb, text_x + text_x_off,
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
            /*
             * Full bleed. Covers used to be cached at 80x80, which over a
             * 274px tile is a 3x upscale that reads as a broken image, so
             * they were inset as a thumbnail in the icon position instead.
             * They are cached at 320x180 now - larger than the tile draws
             * them - so the tile can be the poster it was always meant to be.
             */
            tile.art_inset = 0;
            tile.icon      = (tile.art ? -1 : EVO_IC_RECENT);
        }

        evo_widget_tile(fb, x, tiles_y, EVO_TILE_W, EVO_TILE_H, &tile);
    }

    /*
     * A selection ring that slides, drawn only while the glide is in flight.
     *
     * The tile's own selected chrome switches instantly - fill, icon tint and
     * bloom - because those are states, not positions. What was missing was
     * any sense of travel: the cursor teleported between tiles while every
     * list in the app glided. Once the glide settles this draws nothing, so
     * the resting appearance is unchanged and the ring never doubles up on
     * the tile's own border.
     */
    if (active && g->glide_ready && g->glide_fp != g->glide_target_fp) {
        const evo_theme *th = evo_theme_current();
        int gx = evo_grid_glide_x(g);

        evo_ui_round_rect(fb, gx, tiles_y, EVO_TILE_W, EVO_TILE_H, th->radius,
                          with_alpha(th->accent, 0), with_alpha(th->accent, 0),
                          th->accent, th->border_px + 1,
                          with_alpha(th->shadow, 0), 0);
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
        /*
         * Placeholder bars where the probed fields will land, rather than
         * only a line of text. The panel otherwise appears to have finished
         * loading with half its rows missing, and the eye reads absence as an
         * answer - "this file has no codecs" - instead of as "still working".
         */
        int i;
        int sy = py + 10;

        for (i = 0; i < 4; i++) {
            int keyw = 90 + (i * 23) % 40;
            int valw = 150 + (i * 61) % 130;

            evo_ui_round_rect(fb, x, sy + 8, keyw, 10, 5,
                              with_alpha(th->text_muted, 55),
                              with_alpha(th->text_muted, 55),
                              with_alpha(th->border, 0), 0,
                              with_alpha(th->shadow, 0), 0);
            evo_ui_round_rect(fb, x + 150, sy + 8, valw, 10, 5,
                              with_alpha(th->text_muted, 40),
                              with_alpha(th->text_muted, 40),
                              with_alpha(th->border, 0), 0,
                              with_alpha(th->shadow, 0), 0);
            sy += 34;
        }

        evo_text(fb, x, sy + 10, "READING MEDIA",
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
        row.marquee_phase = row.selected ? f->settled_ms : 0;

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
        row.info          = m->entries[index].info;
        row.swatches      = m->entries[index].swatches;
        row.swatch_count  = m->entries[index].swatch_count;
        row.selected      = (index == f->index);
        row.badge_icon    = -1;
        row.marquee_phase = row.selected ? f->settled_ms : 0;

        evo_widget_row(fb, x, EVO_CONTENT_Y + i * EVO_ROW_PITCH,
                       w, EVO_ROW_H, &row);
    }

    evo_widget_scrollbar(fb, x + w + 10, EVO_CONTENT_Y, EVO_CONTENT_H,
                         evo_focus_scroll_permille(f), f->visible, m->count);

    evo_chrome_end(fb, &page, hints, hint_count);
}

/* ==========================================================================
 * Changelog Master-Detail screen
 * ========================================================================== */

void evo_screen_changelog(uint32_t *fb, const evo_changelog_model *m,
                          const evo_focus *f, int rail_focused, int rail_index,
                          const evo_hint *hints, int hint_count)
{
    const evo_theme *th = evo_theme_current();
    evo_page page;
    char badge[32];
    int list_x = EVO_CONTENT_X;   /* 152 */
    int list_w = 420;
    int row_h = 88;
    int row_pitch = 98;
    int visible_rows = 6;
    int i;

    memset(&page, 0, sizeof(page));
    page.title        = "CHANGELOG";
    page.subtitle     = "WHAT CHANGED IN EACH RELEASE";
    page.section      = EVO_SECTION_ABOUT;
    page.rail_focused = rail_focused;
    page.rail_index   = rail_index;

    if (m && m->release_count > 0) {
        snprintf(badge, sizeof(badge), "RELEASE %d OF %d", (f ? f->index : 0) + 1, m->release_count);
        page.badge = badge;
    }

    evo_chrome_begin(fb, &page);

    if (!m || m->release_count == 0) {
        evo_widget_empty(fb, list_x, EVO_CONTENT_Y, EVO_CONTENT_W - 24, EVO_CONTENT_H,
                         "NO RELEASES FOUND", "PRESS CIRCLE TO GO BACK", EVO_IC_ABOUT);
        evo_chrome_end(fb, &page, hints, hint_count);
        return;
    }

    /* 1. Left column: Release version cards */
    for (i = 0; i < f->visible && (f->scroll + i) < m->release_count; i++) {
        int index = f->scroll + i;
        int y = EVO_CONTENT_Y + i * row_pitch;
        int is_sel = (index == f->index);
        int title_y, sub_y;

        evo_ui_card(fb, list_x, y, list_w, row_h, is_sel);

        const evo_changelog_release *rel = &m->releases[index];
        int icon_idx = (index == 0) ? EVO_IC_LOGO : EVO_IC_ABOUT;
        evo_icon_tinted(fb, list_x + EVO_ROW_PAD_X,
                        y + (row_h - EVO_ROW_ICON) / 2 - 8,
                        icon_idx,
                        is_sel ? th->accent : with_alpha(th->text_muted, 180));

        evo_text_y_stacked(y, row_h, EVO_FACE_MENU, EVO_FACE_SMALL, 6,
                           &title_y, &sub_y);

        char ver_title[48];
        snprintf(ver_title, sizeof(ver_title), "VERSION %s", rel->version);
        evo_text(fb, list_x + EVO_ROW_TEXT_X, title_y, ver_title,
                 is_sel ? th->accent : th->text_primary, EVO_FACE_MENU);

        char ver_sub[64];
        snprintf(ver_sub, sizeof(ver_sub), "%d CHANGES  -  %s", rel->item_count, rel->date);
        evo_text_fit(fb, list_x + EVO_ROW_TEXT_X, sub_y, list_w - EVO_ROW_TEXT_X - 44, ver_sub,
                     th->text_muted, EVO_FACE_SMALL);

        if (is_sel) {
            evo_icon_tinted(fb, list_x + list_w - 44, y + (row_h - EVO_ROW_ICON) / 2 - 8,
                            EVO_IC_CHEVRON, th->accent);
        }
    }

    if (m->release_count > visible_rows) {
        evo_widget_scrollbar(fb, list_x + list_w + 8, EVO_CONTENT_Y,
                             EVO_CONTENT_H, evo_focus_scroll_permille(f),
                             visible_rows, m->release_count);
    }

    /* 2. Vertical separator rule */
    evo_ui_vline(fb, list_x + list_w + 22, EVO_CONTENT_Y, EVO_CONTENT_H,
                 with_alpha(th->border, 90));

    /* 3. Right column: Release Notes Inspector Panel */
    int panel_x = list_x + list_w + 40;
    int panel_w = EVO_CONTENT_R - panel_x;
    int panel_y = EVO_CONTENT_Y;
    int panel_h = EVO_CONTENT_H;

    evo_ui_card(fb, panel_x, panel_y, panel_w, panel_h, 0);

    int cur_idx = (f && f->index >= 0 && f->index < m->release_count) ? f->index : 0;
    const evo_changelog_release *cur = &m->releases[cur_idx];

    int inset_x = panel_x + 36;
    int inset_w = panel_w - 72;
    int hy = panel_y + 24;

    /* Pill badge for version tag */
    int tag_w = 150;
    int tag_h = 32;
    char tag_str[32];
    snprintf(tag_str, sizeof(tag_str), "RELEASE %s", cur->version);
    evo_widget_badge(fb, inset_x, hy, tag_w, tag_h, tag_str,
                     with_alpha(th->accent_soft, 80), th->accent,
                     th->accent, EVO_FACE_SMALL);

    /* Stats right */
    char stats_str[64];
    snprintf(stats_str, sizeof(stats_str), "%d HIGHLIGHTS  -  %s", cur->item_count, cur->date);
    int stats_w = evo_text_w(stats_str, EVO_FACE_SMALL);
    evo_text_right(fb, inset_x + inset_w, hy + 7, stats_str, th->text_muted, EVO_FACE_SMALL);

    /* Tagline */
    int max_tagline_w = inset_w - tag_w - stats_w - 40;
    if (max_tagline_w < 120) max_tagline_w = 120;
    evo_text_fit(fb, inset_x + tag_w + 20, hy + 2, max_tagline_w,
                 cur->tagline, th->text_primary, EVO_FACE_MENU);

    /* Divider */
    evo_ui_hline(fb, inset_x, hy + 46, inset_w, with_alpha(th->border, 110));

    /* Items */
    int item_y = hy + 62;
    int item_pitch = 54;
    int max_items = 11;

    for (int j = 0; j < cur->item_count && j < max_items; j++) {
        const evo_changelog_item *it = &cur->items[j];

        /* Standardized Categorical Badge */
        const char *badge_text = "NEW";
        int badge_cat = EVO_BADGE_ACCENT;

        if (it->kind == EVO_CL_FIXED) {
            badge_text = "FIXED";
            badge_cat  = EVO_BADGE_SUCCESS;
        } else if (it->kind == EVO_CL_IMPROVED) {
            badge_text = "IMPROVED";
            badge_cat  = EVO_BADGE_WARNING;
        } else if (it->kind == EVO_CL_REMOVED) {
            badge_text = "REMOVED";
            badge_cat  = EVO_BADGE_DANGER;
        }

        int pw = 94;
        int ph = 26;
        evo_widget_category_badge(fb, inset_x, item_y + 3, pw, ph, badge_cat, badge_text);

        /* Item text */
        evo_text_fit(fb, inset_x + pw + 16, item_y + 3, inset_w - pw - 20,
                     it->text, th->text_primary, EVO_FACE_SUB);

        /* Subtle item row separator */
        if (j < cur->item_count - 1 && j < max_items - 1) {
            evo_ui_hline(fb, inset_x, item_y + 41, inset_w, with_alpha(th->border, 35));
        }

        item_y += item_pitch;
    }

    evo_chrome_end(fb, &page, hints, hint_count);
}

/* ==========================================================================
 * Surround Sound Studio Screen
 * ========================================================================== */

void evo_screen_surround_test(uint32_t *fb, const evo_surround_test_model *m,
                              int rail_focused, int rail_index,
                              const evo_hint *hints, int hint_count)
{
    const evo_theme *th = evo_theme_current();
    evo_page page;
    memset(&page, 0, sizeof(page));
    page.title = "SURROUND SOUND STUDIO";
    page.subtitle = (m && m->is_51_layout)
        ? "CALIBRATION  -  5.1 SPEAKER SYSTEM (6 CHANNELS)"
        : "CALIBRATION  -  7.1 SPEAKER SYSTEM (8 CHANNELS)";
    page.section = EVO_SECTION_SETTINGS;
    page.rail_focused = rail_focused;
    page.rail_index = rail_index;

    evo_chrome_begin(fb, &page);

    int mon_x = 130, mon_y = 160, mon_w = 460, mon_h = 220;

    /* 1. Left Telemetry Monitor Panel */
    evo_stat_card card;
    memset(&card, 0, sizeof(card));
    card.header_label = "SPEAKER CALIBRATION MONITOR";

    int active_spk = m ? m->active_channel : -1;
    int sel_item   = m ? m->selected_item : 0;
    int display_spk = -1;

    if (active_spk >= 0) {
        display_spk = active_spk;
    } else if (sel_item >= 5 && sel_item <= 12) {
        display_spk = (sel_item == 5 ? 0 :
                       sel_item == 6 ? 2 :
                       sel_item == 7 ? 1 :
                       sel_item == 8 ? 3 :
                       sel_item == 9 ? 6 :
                       sel_item == 10 ? 7 :
                       sel_item == 11 ? 4 : 5);
    }

    char title_buf[64], hz_buf[64], ch_buf[64], stat_buf[64];
    if (display_spk >= 0 && m && display_spk < m->speaker_count) {
        const evo_surround_speaker_info *spk = &m->speakers[display_spk];
        snprintf(title_buf, sizeof(title_buf), "%s (%s)", spk->name, spk->label);
        snprintf(hz_buf, sizeof(hz_buf), "TONE FREQ: %.1f HZ", spk->hz);
        snprintf(ch_buf, sizeof(ch_buf), "PS5 AUDIO OUT: S16_8CH (CH %d)", display_spk);
        snprintf(stat_buf, sizeof(stat_buf), active_spk >= 0 ? "STATUS: [ ACTIVE NOW ]" : "STATUS: [ READY / STANDBY ]");
        card.is_active = (active_spk >= 0);
    } else {
        snprintf(title_buf, sizeof(title_buf), "%s",
                 sel_item == 0 ? "5.1 AUTO SEQUENCE" :
                 sel_item == 1 ? "7.1 AUTO SEQUENCE" :
                 sel_item == 2 ? "360 ROTATION SWEEP" :
                 sel_item == 3 ? (m && m->is_51_layout ? "LAYOUT: 5.1 CHANNELS" : "LAYOUT: 7.1 CHANNELS") : "SILENCE / STOP");
        snprintf(hz_buf, sizeof(hz_buf), "MODE: %s",
                 sel_item == 0 ? "CALIBRATION SWEEP 5.1" :
                 sel_item == 1 ? "CALIBRATION SWEEP 7.1" :
                 sel_item == 2 ? "PERIMETER CIRCLE" :
                 sel_item == 3 ? "TOGGLE SPEAKER COUNT" : "STOP AUDIO STREAM");
        snprintf(ch_buf, sizeof(ch_buf), "PS5 AUDIO OUT: 48 kHz / 16-BIT");
        snprintf(stat_buf, sizeof(stat_buf), "%s", (m && m->surround_mode != 0) ? "STATUS: [ RUNNING TEST ]" : "STATUS: [ IDLE ]");
        card.is_active = 0;
    }

    card.title       = title_buf;
    card.line1       = hz_buf;
    card.line2       = ch_buf;
    card.status_text = stat_buf;
    evo_widget_stat_card(fb, mon_x, mon_y, mon_w, mon_h, &card);

    /* 2. Action Buttons */
    static const struct {
        const char *label;
        const char *sub;
    } actions[5] = {
        { "AUTO TEST 5.1", "6-CHANNEL CALIBRATION" },
        { "AUTO TEST 7.1", "8-CHANNEL CALIBRATION" },
        { "360 ROTATION SWEEP", "CIRCULAR PERIMETER PAN" },
        { "SPEAKER LAYOUT", "SWITCH 5.1 / 7.1" },
        { "SILENCE / STOP", "STOP ALL OUTPUT" }
    };

    int act_y = 400;
    for (int i = 0; i < 5; i++) {
        int is_sel = (!rail_focused && sel_item == i);
        int cur_y = act_y + i * 80;
        uint32_t fill_top = is_sel ? th->surface_sel : th->surface;
        uint32_t fill_bot = is_sel ? th->surface_sel_alt : th->surface_alt;
        uint32_t border   = is_sel ? th->border_sel : th->border;

        evo_ui_round_rect(fb, mon_x, cur_y, mon_w, 72, 14,
                          fill_top, fill_bot, border, is_sel ? 2 : 1,
                          is_sel ? 0x66000000 : 0x22000000, is_sel ? 14 : 6);

        if (is_sel) {
            evo_ui_round_rect(fb, mon_x + 3, cur_y + 14, 5, 44, 3,
                              th->accent, th->accent, th->accent, 0, 0, 0);
        }

        evo_text(fb, mon_x + 24, cur_y + 12, actions[i].label, is_sel ? th->text_primary : th->text_secondary, EVO_FACE_MENU);
        evo_text(fb, mon_x + 24, cur_y + 44, (i == 3) ? (m && m->is_51_layout ? "CURRENT: 5.1 SURROUND" : "CURRENT: 7.1 SURROUND") : actions[i].sub, is_sel ? th->accent : th->text_muted, EVO_FACE_SMALL);
    }

    /* 3. Stage & Visualizer */
    int stage_x = 610, stage_y = 160, stage_w = 1180, stage_h = 760;
    evo_ui_round_rect(fb, stage_x, stage_y, stage_w, stage_h, 24,
                      th->surface_alt, th->bg_top,
                      th->border, 1,
                      0x55000000, 20);

    int scr_w = 420, scr_h = 14;
    int scr_x = stage_x + (stage_w - scr_w) / 2;
    int scr_y = stage_y + 24;
    evo_ui_round_rect(fb, scr_x, scr_y, scr_w, scr_h, 7,
                      th->accent, th->accent_soft, th->accent, 1,
                      0x4419d8ff, 10);
    evo_text(fb, scr_x + (scr_w - evo_text_w("FRONT DISPLAY / TV SCREEN", EVO_FACE_SMALL)) / 2,
             scr_y + 22, "FRONT DISPLAY / TV SCREEN", th->text_muted, EVO_FACE_SMALL);

    int couch_cx = stage_x + stage_w / 2;
    int couch_cy = stage_y + 390;

    evo_ui_circle(fb, couch_cx, couch_cy, 120, 0x14FFFFFF);
    evo_ui_circle(fb, couch_cx, couch_cy, 230, 0x0BFFFFFF);
    evo_ui_circle(fb, couch_cx, couch_cy, 340, 0x06FFFFFF);

    int couch_w = 130, couch_h = 66;
    evo_ui_round_rect(fb, couch_cx - couch_w / 2, couch_cy - couch_h / 2, couch_w, couch_h, 14,
                      th->surface, th->surface_alt, th->border, 1, 0x44000000, 8);
    evo_text(fb, couch_cx - evo_text_w("LISTENER", EVO_FACE_MENU) / 2,
             couch_cy - 20, "LISTENER", th->text_primary, EVO_FACE_MENU);
    evo_text(fb, couch_cx - evo_text_w("SWEET SPOT", EVO_FACE_SMALL) / 2,
             couch_cy + 12, "SWEET SPOT", th->accent, EVO_FACE_SMALL);

    if (m && m->speakers) {
        for (int i = 0; i < m->speaker_count; i++) {
            int ch = m->speakers[i].ch;
            if (m->is_51_layout && (ch == 6 || ch == 7)) {
                continue; /* Hide side surround speakers in 5.1 */
            }
            int sx = couch_cx + m->speakers[i].dx;
            int sy = couch_cy + m->speakers[i].dy;
            int sw = m->speakers[i].w;
            int sh = m->speakers[i].h;
            int is_act = (active_spk == ch);
            int is_sel = (!rail_focused && sel_item == m->speakers[i].item_idx);

            char hz_str[32];
            if (is_act) {
                snprintf(hz_str, sizeof(hz_str), "%.0f Hz [ON]", m->speakers[i].hz);
            } else {
                snprintf(hz_str, sizeof(hz_str), "%.0f Hz", m->speakers[i].hz);
            }

            evo_speaker_node node;
            node.label       = m->speakers[i].label;
            node.sub         = hz_str;
            node.is_active   = is_act;
            node.is_selected = is_sel;

            evo_widget_speaker_node(fb, sx, sy, sw, sh, &node);
        }
    }

    evo_chrome_end(fb, &page, hints, hint_count);
}

/* ==========================================================================
 * Modal dialog
 * ========================================================================== */

void evo_screen_dialog(uint32_t *fb, const evo_dialog_model *m)
{
    const evo_theme *th = evo_theme_current();
    const int w  = 1120;
    /*
     * Sized to its contents. At 520 the panel had 180px of nothing between
     * the progress bar and the buttons, which reads as a layout that failed
     * to fill rather than as breathing room.
     */
    const int h  = 430;
    const int x  = (EVO_SCREEN_W - w) / 2;
    const int y  = (EVO_SCREEN_H - h) / 2;
    int       pad = 56;
    int       art_w = 0;
    int       ty, dy;
    int       i, ax;

    if (!m) return;

    /*
     * Scrim the whole screen rather than clearing it. A dialog over playback
     * has the last decoded frame behind it, and blanking that makes the
     * prompt feel like the video was thrown away rather than paused.
     */
    evo_ui_vgrad_over(fb, 0, 0, EVO_SCREEN_W, EVO_SCREEN_H,
                      with_alpha(th->scrim, 205), with_alpha(th->scrim, 225));

    evo_ui_round_rect(fb, x, y, w, h, th->radius + 4,
                      with_alpha(th->surface, 255),
                      with_alpha(th->surface_alt, 255),
                      th->border_sel, th->border_px + 1,
                      th->shadow, th->shadow_px + 8);

    /* Artwork on the left, when there is any - it identifies the file faster
     * than its name does. */
    if (m->art.pixels && m->art.w > 0 && m->art.h > 0) {
        art_w = 372;
        evo_widget_preview(fb, x + pad, y + pad, art_w, art_w * 9 / 16,
                           m->art.pixels, m->art.w, m->art.h, NULL, -1);
        art_w += 40;
    }

    if (m->eyebrow && *m->eyebrow)
        evo_text(fb, x + pad + art_w, y + pad, m->eyebrow,
                 th->accent, EVO_FACE_SMALL);

    evo_text_y_stacked(y + pad + 30, 118, EVO_FACE_TITLE, EVO_FACE_SUB, 14,
                       &ty, &dy);

    evo_text_fit(fb, x + pad + art_w, ty, w - pad * 2 - art_w,
                 m->title ? m->title : "", th->text_primary, EVO_FACE_TITLE);

    if (m->detail && *m->detail)
        evo_text_fit(fb, x + pad + art_w, dy, w - pad * 2 - art_w,
                     m->detail, th->text_secondary, EVO_FACE_SUB);

    if (m->progress >= 0)
        evo_widget_progress(fb, x + pad + art_w, y + pad + 168,
                            w - pad * 2 - art_w, 6, m->progress);

    /*
     * Actions along the bottom, measured rather than on a fixed pitch - the
     * same rule the footer hints follow, and for the same reason.
     */
    ax = x + pad;
    int max_actions_r = x + w - pad;

    for (i = 0; i < m->action_count && m->actions; i++) {
        int lw = evo_text_w(m->actions[i].label, EVO_FACE_SUB);
        int cw = EVO_FOOTER_GLYPH + EVO_FOOTER_GLYPH_GAP + lw + 40;
        int cy = y + h - pad - 56;
        int primary = (i == 0);

        if (ax + cw > max_actions_r) {
            cw = max_actions_r - ax;
            if (cw < 50) break;
        }

        evo_ui_round_rect(fb, ax, cy, cw, 56, 28,
                          primary ? th->accent : with_alpha(th->surface, 255),
                          primary ? th->accent : with_alpha(th->surface_alt, 255),
                          primary ? th->accent : th->border, th->border_px,
                          with_alpha(th->shadow, 0), 0);

        evo_glyph_tinted(fb, ax + 18, cy + (56 - EVO_FOOTER_GLYPH) / 2,
                         m->actions[i].glyph,
                         primary ? th->bg_bottom : th->accent);

        int max_label_w = cw - (18 + EVO_FOOTER_GLYPH + EVO_FOOTER_GLYPH_GAP + 18);
        if (max_label_w > 0) {
            evo_text_fit(fb, ax + 18 + EVO_FOOTER_GLYPH + EVO_FOOTER_GLYPH_GAP,
                         evo_text_y_centred(cy, 56, EVO_FACE_SUB),
                         max_label_w,
                         m->actions[i].label,
                         primary ? th->bg_bottom : th->text_primary, EVO_FACE_SUB);
        }

        ax += cw + 20;
    }
}

/* ==========================================================================
 * Media info
 * ========================================================================== */

void evo_screen_info(uint32_t *fb, const evo_info_model *m)
{
    evo_page page;
    int      col_w;
    int      half;

    if (!m) return;

    memset(&page, 0, sizeof(page));
    page.title    = m->title;
    page.subtitle = m->subtitle;
    page.section  = EVO_SECTION_NONE;
    page.no_rail  = 1;   /* modal: reached from playback, not from the rail */

    evo_chrome_begin(fb, &page);

    /* Preview on the right, properties filling the rest in two columns. The
     * browser's inspector is one narrow column because it has to share the
     * page with a file list; here there is no list to share with. */
    if (m->art.pixels && m->art.w > 0 && m->art.h > 0) {
        evo_widget_preview(fb, EVO_INSPECT_X, EVO_CONTENT_Y,
                           EVO_PREVIEW_W, EVO_PREVIEW_H,
                           m->art.pixels, m->art.w, m->art.h,
                           m->art_badge, EVO_IC_RESUME);
    }

    col_w = (EVO_INSPECT_X - EVO_INSPECT_GAP - EVO_BLEED_X) / 2 - 24;
    half  = (m->prop_count + 1) / 2;

    evo_widget_props(fb, EVO_BLEED_X, EVO_CONTENT_Y, col_w,
                     m->props, half);

    if (m->prop_count > half)
        evo_widget_props(fb, EVO_BLEED_X + col_w + 48, EVO_CONTENT_Y, col_w,
                         m->props + half, m->prop_count - half);

    {
        const evo_hint hints[1] = { { EVO_GLYPH_CIRCLE, "BACK" } };
        evo_chrome_end(fb, &page, hints, 1);
    }
}

/* ==========================================================================
 * Overlay picker
 * ========================================================================== */

#define EVO_PICKER_W       920
#define EVO_PICKER_PAD      48
#define EVO_PICKER_ROW_H    62
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

void evo_screen_picker(uint32_t *fb, const evo_picker_model *m,
                       const evo_focus *f)
{
    const evo_theme *th  = evo_theme_current();
    const int cap = evo_screen_picker_capacity();
    int shown, h, x, y, row_y, i;
    const evo_hint hints[3] = {
        { EVO_GLYPH_CROSS,    "SELECT" },
        { EVO_GLYPH_TRIANGLE, "SIZE"   },
        { EVO_GLYPH_CIRCLE,   "BACK"   }
    };

    if (!m || !f) return;

    shown = m->entry_count < cap ? m->entry_count : cap;
    if (shown < 0) shown = 0;

    h = EVO_PICKER_HEAD + shown * EVO_PICKER_PITCH + EVO_PICKER_FOOT;
    x = (EVO_SCREEN_W - EVO_PICKER_W) / 2;
    if (m->preview_face >= 0) {
        y = 110;
    } else {
        y = (EVO_SCREEN_H - h) / 2;
    }

    /* Same reasoning as the dialog: scrim, never clear. */
    evo_ui_vgrad_over(fb, 0, 0, EVO_SCREEN_W, EVO_SCREEN_H,
                      with_alpha(th->scrim, 205), with_alpha(th->scrim, 225));

    evo_ui_round_rect(fb, x, y, EVO_PICKER_W, h, th->radius + 4,
                      with_alpha(th->surface, 255),
                      with_alpha(th->surface_alt, 255),
                      th->border_sel, th->border_px + 1,
                      th->shadow, th->shadow_px + 8);

    if (m->eyebrow && *m->eyebrow)
        evo_text(fb, x + EVO_PICKER_PAD, y + 40, m->eyebrow,
                 th->accent, EVO_FACE_SMALL);

    if (m->title && *m->title)
        evo_text_fit(fb, x + EVO_PICKER_PAD, y + 68,
                     EVO_PICKER_W - EVO_PICKER_PAD * 2, m->title,
                     th->text_primary, EVO_FACE_MENU);

    row_y = y + EVO_PICKER_HEAD;

    for (i = 0; i < shown; i++) {
        const evo_picker_entry *e = &m->entries[i];
        int abs_index = m->first + i;
        int selected  = (abs_index == f->index);
        int rx        = x + EVO_PICKER_PAD;
        int rw        = EVO_PICKER_W - EVO_PICKER_PAD * 2 - 16;
        int ry        = row_y + i * EVO_PICKER_PITCH;
        uint32_t label_col;
        int text_x    = rx + 22;

        evo_ui_card(fb, rx, ry, rw, EVO_PICKER_ROW_H, selected);

        /*
         * The active track is flagged with an accent bar at the row's leading
         * edge rather than an icon: the icon set draws at 72px native, which
         * does not fit a 62px row, and "which one am I on" versus "which one
         * is playing" should not be the same glance across 900px of row.
         */
        if (e->current) {
            evo_ui_round_rect(fb, rx + 16,
                              ry + (EVO_PICKER_ROW_H - 28) / 2, 5, 28, 2,
                              th->accent, th->accent, th->accent, 0,
                              with_alpha(th->shadow, 0), 0);
            text_x = rx + 16 + 5 + 16;
        }

        /*
         * Selection is signalled by the card underneath, the same way every
         * other list in the app does it. Inverting the text as well made the
         * selected row read as the dimmest one on screen.
         */
        label_col = e->weak ? th->text_muted : th->text_primary;

        if (e->detail && *e->detail) {
            int dw = evo_text_w(e->detail, EVO_FACE_SMALL);

            evo_text(fb, rx + rw - 22 - dw,
                     evo_text_y_centred(ry, EVO_PICKER_ROW_H, EVO_FACE_SMALL),
                     e->detail,
                     selected ? th->text_secondary : th->text_muted,
                     EVO_FACE_SMALL);

            evo_text_fit(fb, text_x,
                         evo_text_y_centred(ry, EVO_PICKER_ROW_H,
                                            EVO_FACE_SUB),
                         rw - (text_x - rx) - dw - 44,
                         e->label ? e->label : "", label_col, EVO_FACE_SUB);
        } else {
            evo_text_fit(fb, text_x,
                         evo_text_y_centred(ry, EVO_PICKER_ROW_H,
                                            EVO_FACE_SUB),
                         rw - (text_x - rx) - 22,
                         e->label ? e->label : "", label_col, EVO_FACE_SUB);
        }
    }

    if (m->count > cap)
        evo_widget_scrollbar(fb, x + EVO_PICKER_W - EVO_PICKER_PAD + 12,
                             row_y, shown * EVO_PICKER_PITCH - 8,
                             evo_focus_scroll_permille(f), cap, m->count);

    /*
     * Hints drawn here rather than through the chrome footer: the footer is
     * pinned to the bottom of the screen, and these belong to the panel.
     */
    {
        int hx = x + EVO_PICKER_PAD;
        int hy = y + h - EVO_PICKER_FOOT + 16;
        int n;

        for (n = 0; n < 3; n++) {
            evo_glyph_tinted(fb, hx, hy, hints[n].glyph, th->text_muted);
            hx += EVO_FOOTER_GLYPH + EVO_FOOTER_GLYPH_GAP;

            evo_text(fb, hx,
                     evo_text_y_centred(hy, EVO_FOOTER_GLYPH,
                                        EVO_FACE_SMALL),
                     hints[n].label, th->text_muted, EVO_FACE_SMALL);

            hx += evo_text_w(hints[n].label, EVO_FACE_SMALL) + 34;
        }
    }

    /*
     * Subtitle preview below the picker card so changes in size are immediately
     * visible with authentic subtitle outline and shadow styling.
     */
    if (m->preview_face >= 0) {
        int face = m->preview_face;
        int line_spacing = (face == EVO_FACE_SUB)   ? 56 :
                           (face == EVO_FACE_TITLE) ? 96 : 76;

        const char *line1 = "Welcome to EVO Player on PlayStation 5";
        const char *line2 = (face == EVO_FACE_SUB)   ? "Subtitle size: SMALL" :
                            (face == EVO_FACE_TITLE) ? "Subtitle size: LARGE" :
                                                       "Subtitle size: MEDIUM";
        const char *lines[2];
        int line_count = 2;

        if (m->preview_text && *m->preview_text) {
            lines[0] = m->preview_text;
            line_count = 1;
        } else {
            lines[0] = line1;
            lines[1] = line2;
        }

        /* Subtle preview eyebrow */
        evo_text_centre(fb, EVO_SCREEN_W / 2, y + h + 24,
                        "SUBTITLE PREVIEW", th->text_muted, EVO_FACE_SMALL);

        /* Center lines vertically in the remaining space below the label */
        int preview_area_top = y + h + 54;
        int preview_area_bot = EVO_SCREEN_H - 40;
        int center_y = (preview_area_top + preview_area_bot) / 2;
        int first_y = center_y - ((line_count - 1) * line_spacing) / 2;

        for (int li = 0; li < line_count; li++) {
            const char *line = lines[li];
            int tw = evo_text_w(line, (evo_face)face);
            int lx = (EVO_SCREEN_W - tw) / 2;
            if (lx < 0) lx = 0;
            int ly = first_y + li * line_spacing;

            /* Soft lower shadow */
            evo_text(fb, lx + 2, ly + 3, line, 0xBE000000, (evo_face)face);

            /* Thin outline around letters */
            evo_text(fb, lx - 2, ly,     line, 0xF5000000, (evo_face)face);
            evo_text(fb, lx + 2, ly,     line, 0xF5000000, (evo_face)face);
            evo_text(fb, lx,     ly - 2, line, 0xF5000000, (evo_face)face);
            evo_text(fb, lx,     ly + 2, line, 0xF5000000, (evo_face)face);

            /* Main text */
            evo_text(fb, lx,     ly,     line, 0xFFFAFCFF, (evo_face)face);
        }
    }
}

/* ==========================================================================
 * Text reader
 *
 * The page is a column of already-wrapped lines and a scrollbar. Everything
 * interesting - encoding, wrapping, where the view is - happened in
 * media/evo_textreader.c before this was called; drawing a reader is genuinely
 * this simple once the text is laid out, and keeping it that way is why the
 * wrap lives on the other side of a measuring callback.
 *
 * Line pitch is the face's own line box plus a little air. Reading a wall of
 * text on a television at two metres is the case the leading has to serve, so
 * it is looser than a list row, where the box itself does the separating.
 * ======================================================================== */

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

/* Must match the column evo_screen_reader() draws into, below. */
#define EVO_READER_SCROLLBAR_GUTTER 34

int evo_screen_reader_wrap_w(void)
{
    evo_page page;

    memset(&page, 0, sizeof(page));
    page.section = EVO_SECTION_BROWSER;

    return evo_chrome_content_r(&page) - evo_chrome_content_x(&page)
           - EVO_READER_SCROLLBAR_GUTTER;
}

void evo_screen_reader(uint32_t *fb, const evo_reader_model *m,
                       int rail_focused, int rail_index,
                       const evo_hint *hints, int hint_count)
{
    const evo_theme *th = evo_theme_current();
    evo_page page;
    int x, right, y, pitch, i;

    if (!fb || !m) return;

    memset(&page, 0, sizeof(page));
    page.title        = m->title;
    page.subtitle     = m->subtitle;
    page.badge        = m->badge;
    page.section      = EVO_SECTION_BROWSER;   /* it was opened from there */
    page.rail_focused = rail_focused;
    page.rail_index   = rail_index;

    evo_chrome_begin(fb, &page);

    x     = evo_chrome_content_x(&page);
    right = evo_chrome_content_r(&page);
    y     = EVO_CONTENT_Y;
    pitch = m->line_pitch > 0 ? m->line_pitch : evo_screen_reader_pitch(m->face);

    if (m->notice) {
        /* An error, or an empty file. Said in the middle of the page rather
         * than as a toast: the page has nothing else on it, and a toast would
         * be gone before the reader worked out why the screen was blank. */
        int tw = evo_text_w(m->notice, EVO_FACE_MENU);
        evo_text(fb, x + (right - x - tw) / 2,
                 EVO_CONTENT_Y + (EVO_CONTENT_B - EVO_CONTENT_Y) / 2 - 24,
                 m->notice, th->text_secondary, EVO_FACE_MENU);
    } else {
        /* The scrollbar occupies the right edge, so text stops short of it. */
        int text_r = right - EVO_READER_SCROLLBAR_GUTTER;

        for (i = 0; i < m->line_count && m->lines; i++) {
            const char *s = m->lines[i];

            if (y + reader_face_h(m->face) > EVO_CONTENT_B)
                break;
            if (s && *s)
                evo_text_fit(fb, x, y, text_r - x, s,
                             th->text_primary, (evo_face)m->face);
            y += pitch;
        }

        /*
         * Thumb size encodes how much of the document is on screen, which is
         * the only thing that tells you whether "half way" means five more
         * pages or five hundred. evo_widget_scrollbar takes counts, so the
         * fractions are turned back into them.
         */
        {
            int total   = 1000;
            int visible = (int)(m->visible_frac * 1000.0 + 0.5);
            int permille = (int)(m->progress * 1000.0 + 0.5);

            if (visible < 20)   visible = 20;     /* stays grabbable */
            if (visible > total) visible = total;
            if (permille < 0)   permille = 0;
            if (permille > 1000) permille = 1000;

            if (visible < total)
                evo_widget_scrollbar(fb, right - 12, EVO_CONTENT_Y,
                                     EVO_CONTENT_B - EVO_CONTENT_Y,
                                     permille, visible, total);
        }
    }

    if (m->footnote)
        evo_text(fb, x, EVO_CONTENT_B + 6, m->footnote,
                 th->text_muted, EVO_FACE_SMALL);

    evo_chrome_end(fb, &page, hints, hint_count);
}
