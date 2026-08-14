/*
 * tools/uiview.c - render the player's UI on the host, without a console.
 *
 * This is not a mock-up. It links the real drawing code - evo_ui's SDF
 * primitives, evo_chrome, evo_widgets, evo_screens - against the real font
 * atlas and the real generated icons, and paints into a plain 1920x1080 BGRA
 * buffer exactly as the player does. What comes out is what the console
 * draws, modulo the tile swizzle, which changes where pixels live in memory
 * and not what they look like.
 *
 * That is possible only because the UI layer was built with no dependency on
 * the decoder, VideoOut or the pad: screens take a plain model struct and
 * draw through a vtable. The vtable is the one thing this file has to supply
 * itself.
 *
 *   ./tools/uiview.sh --all              every screen to output/uiview/
 *   ./tools/uiview.sh browse --sel 2
 *   ./tools/uiview.sh launch --theme EMBER
 *
 * Output is a BMP so that tools/shot.py can convert it, which means host
 * renders and console captures go through exactly the same image path and
 * can be compared directly.
 */

#include "evo_chrome.h"
#include "evo_draw.h"
#include "evo_focus.h"
#include "evo_metrics.h"
#include "evo_nav.h"
#include "evo_screens.h"
#include "evo_theme.h"
#include "evo_ui.h"
#include "evo_widgets.h"
#include "evo_keyboard.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The real assets. Pure data - no PS5 types anywhere in them. */
/* The real release-notes table, not a fixture - so a render cannot disagree
 * with what the console shows. */
#include "evo_changelog.h"
#include "assets/renderer_reset_assets.h"
#include "assets/evo_icons.h"
#include "assets/evo_font_punct.h"

/* After both atlases - it reads RR_* and EVO_PUNCT_* directly. */
#include "evo_font.h"
#include "evo_textreader.h"
#include "evo_feedback.h"

void evo_feedback(evo_feedback_kind kind) { (void)kind; }
void evo_feedback_for_move(int moved) { (void)moved; }

/* The reader measures with the same tables it draws with. */
static int reader_measure(const char *s, int len, int face)
{
    int w = 0;
    for (int i = 0; i < len; i++)
        w += evo_font_advance(s[i], face);
    return w;
}

#define W EVO_SCREEN_W
#define H EVO_SCREEN_H

static uint32_t g_fb[W * H];

/* ===========================================================================
 * The draw vtable.
 *
 * A deliberate copy of the rr_* renderers in main.c. Sharing them would mean
 * moving them into their own translation unit, and main.c pulls the same ~1MB
 * of asset headers for other reasons - so the shared unit would either
 * duplicate that data in the ELF or force a wider refactor of a 20k-line file
 * whose video path is load bearing.
 *
 * The copy is kept honest by comparison, not by discipline: render the launch
 * screen here and diff it against a console capture of the same screen. If
 * they match, the copy is faithful. See tools/uiview.sh --verify.
 * ======================================================================== */

static uint32_t blend(uint32_t dst, uint32_t src)
{
    return evo_ui_blend(dst, src, (src >> 24) & 0xFF);
}

/*
 * Text drawing, shared with the player through evo_font.h.
 *
 * This used to carry its own rr_idx linear scan and its own four-way face
 * switch, transcribed from main.c. Both now come from the shared resolver, so
 * the mock and the player cannot disagree about what a character looks like -
 * which matters more now that there are two atlases to pick between.
 */
static void rr_text(uint32_t *fb, int x, int y, const char *t,
                    uint32_t colour, int face)
{
    int cx = x;
    unsigned ca = (colour >> 24) & 255;

    for (const char *q = t; *q; q++) {
        evo_font_glyph g;

        if (!evo_font_find(*q, face, &g)) { cx += EVO_FONT_MISS_ADV; continue; }

        for (int yy = 0; yy < g.sh; yy++) {
            int fy = y + yy;
            if ((unsigned)fy >= H) continue;
            for (int xx = 0; xx < g.sw; xx++) {
                int fx = cx + xx;
                unsigned ma, a;
                if ((unsigned)fx >= W) continue;
                ma = g.atlas[(g.sy + yy) * g.atlas_w + (g.sx + xx)];
                if (ma <= EVO_FONT_INK_MIN) continue;
                a = (ma * ca) / 255;
                fb[fy * W + fx] = blend(fb[fy * W + fx],
                                        (a << 24) | (colour & 0x00FFFFFF));
            }
        }
        cx += g.adv;
    }
}

static int rr_text_w(const char *t, int face)
{
    return evo_font_text_width(t, face);
}

static void img_tint(uint32_t *fb, int x, int y, int w, int h,
                     const unsigned int *img, uint32_t colour)
{
    unsigned ca = (colour >> 24) & 255;
    if (!ca) return;

    for (int yy = 0; yy < h; yy++) {
        int fy = y + yy;
        if ((unsigned)fy >= H) continue;
        for (int xx = 0; xx < w; xx++) {
            int fx = x + xx;
            unsigned sa;
            if ((unsigned)fx >= W) continue;
            sa = (img[yy * w + xx] >> 24) & 255;
            if (!sa) continue;
            fb[fy * W + fx] = blend(fb[fy * W + fx],
                                    (((sa * ca) / 255) << 24) |
                                    (colour & 0x00FFFFFFu));
        }
    }
}

/*
 * Index -> icon, through the table tools/gen_icons.py emits.
 *
 * The player has the identical function. Both used to spell out a switch with
 * one arm per icon, and nothing tied the copies together - the comment in
 * evo_draw.h says this file is "kept honest by comparison, not by discipline".
 * Indexing a generated table is the discipline.
 */
static void rr_icon_tinted(uint32_t *fb, int x, int y, int idx, uint32_t tint)
{
    if ((unsigned)idx >= EVO_ICON_TABLE_COUNT)
        return;
    img_tint(fb, x, y, EVO_ICON_TABLE[idx].w, EVO_ICON_TABLE[idx].h,
             EVO_ICON_TABLE[idx].px, tint);
}

static void rr_icon(uint32_t *fb, int x, int y, int idx)
{
    rr_icon_tinted(fb, x, y, idx, evo_theme_current()->accent);
}

static void rr_control_tinted(uint32_t *fb, int x, int y, int t_idx, uint32_t t)
{
    if ((unsigned)t_idx >= EVO_CTRL_TABLE_COUNT)
        return;
    img_tint(fb, x, y, EVO_CTRL_TABLE[t_idx].w, EVO_CTRL_TABLE[t_idx].h,
             EVO_CTRL_TABLE[t_idx].px, t);
}

static void rr_control(uint32_t *fb, int x, int y, int idx)
{
    rr_control_tinted(fb, x, y, idx, evo_theme_current()->accent);
}

static const evo_draw_vtable VTABLE = {
    rr_text, rr_text_w, rr_icon, rr_icon_tinted, rr_control, rr_control_tinted
};

/* ===========================================================================
 * BMP output
 *
 * Same 24-bit bottom-up layout the player's EVO_AUTOSHOT writes, so
 * tools/shot.py reads host renders and console captures identically - which
 * is what makes `shot.py diff` between them meaningful.
 * ======================================================================== */

static int write_bmp(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    int   stride = W * 3;
    int   pad    = (4 - (stride % 4)) % 4;
    uint32_t data = (uint32_t)((stride + pad) * H);
    uint8_t  hdr[54];
    uint8_t *row;

    if (!f) return -1;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2)  = 54 + data;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = W;
    *(int32_t  *)(hdr + 22) = H;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = data;
    fwrite(hdr, 1, sizeof(hdr), f);

    row = calloc(1, (size_t)stride + pad);
    if (!row) { fclose(f); return -1; }

    for (int y = H - 1; y >= 0; y--) {          /* bottom-up */
        for (int x = 0; x < W; x++) {
            uint32_t p = fb[y * W + x];         /* 0xAABBGGRR */
            row[x * 3 + 0] = (uint8_t)((p >> 16) & 0xFF);   /* B */
            row[x * 3 + 1] = (uint8_t)((p >> 8)  & 0xFF);   /* G */
            row[x * 3 + 2] = (uint8_t)( p        & 0xFF);   /* R */
        }
        fwrite(row, 1, (size_t)stride + pad, f);
    }

    free(row);
    fclose(f);
    return 0;
}

/* ===========================================================================
 * Fixtures
 *
 * Deliberately awkward: long names that must ellipsise, a mix of resumed and
 * unwatched items, a folder among files. Tidy fixtures make a layout look
 * better than it is.
 * ======================================================================== */

/*
 * 320x180, the size main.c caches cover art at (PROSPERO_COVER_W/_H). The
 * fixture used to be 128x72, which magnified into every surface it was drawn
 * on and so could not show what a real cover looks like on a tile.
 */
#define ART_W 320
#define ART_H 180
static uint32_t g_art[ART_W * ART_H];

/*
 * The hero gets its own, larger buffer, because main.c gives it one:
 * EVO_HERO_ART_W/_H are 960x540 precisely so a backdrop drawn 1740px wide is
 * under a 2x upscale. Feeding the hero a tile-sized fixture made the mock
 * render a 5.4x magnification and show stair-stepping that the console never
 * produces - a picture that lies about the thing it exists to check.
 */
#define HERO_W 960
#define HERO_H 540
static uint32_t g_hero[HERO_W * HERO_H];

/*
 * A gradient with a true circle inscribed in it, at whatever size is asked
 * for.
 *
 * The gradient alone told us nothing about aspect: covers are 16:9 and the
 * tiles they fill are closer to 5:3, so anything that stretches rather than
 * crops squashes them ~7% horizontally - invisible on a gradient, obvious on
 * a circle, and obvious on a face. It is a ring rather than a disc because a
 * disc hides the crop; a ring shows both where the edges went and whether it
 * is still round.
 */
static void fill_art(uint32_t *buf, int w, int h)
{
    const int cx = w / 2, cy = h / 2;
    const int rad   = h / 2 - h / 22;
    const int thick = h / 36 + 1;

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r = 40 + (x * 160) / w;
            int g = 30 + (y * 120) / h;
            int b = 90 + ((x + y) * 90) / (w + h);
            int dx = x - cx, dy = y - cy;
            int d  = dx * dx + dy * dy;

            if (d <= rad * rad && d >= (rad - thick) * (rad - thick)) {
                r = 235; g = 240; b = 245;
            }

            buf[y * w + x] = 0xFF000000u | ((uint32_t)b << 16) |
                             ((uint32_t)g << 8) | (uint32_t)r;
        }
}

static void make_art(void)
{
    fill_art(g_art,  ART_W,  ART_H);
    fill_art(g_hero, HERO_W, HERO_H);
}

static void render_launch(int sel_row, int sel_col)
{
    static const char *titles[] = {
        "leftbehind-bts", "ps-studios-intro", "DTS 5.1",
        "Dolby TrueHD 7.1", "Dolby Atmos objects", "AAC 5.1", "FLAC 7.1"
    };
    static const char *details[] = {
        "7M 58S LEFT", "0M 01S LEFT", "0M 01S LEFT",
        "0M 09S LEFT", "0M 42S LEFT", "0M 40S LEFT", "12M 03S LEFT"
    };
    evo_launch_item items[7];
    evo_launch_model m;
    evo_grid g;

    memset(items, 0, sizeof(items));
    for (int i = 0; i < 7; i++) {
        items[i].title    = titles[i];
        items[i].detail   = details[i];
        items[i].progress = (i * 137) % 1000;
        /*
         * Art on some tiles and not others, on purpose. Cover art resolves at
         * one file per frame, so a shelf genuinely does sit half-filled for
         * the first few frames after it appears - and the icon fallback has to
         * hold its own next to a poster rather than only being seen alone.
         */
        if (i == 0 || i == 1 || i == 2 || i == 4) {
            items[i].art.pixels = g_art;
            items[i].art.w = ART_W;
            items[i].art.h = ART_H;
        }
    }

    memset(&m, 0, sizeof(m));
    m.has_resume    = 1;
    m.hero_title    = "leftbehind-bts";
    m.hero_detail   = "7M 58S LEFT";
    m.hero_action   = "RESUME";
    m.hero_progress = 240;
    m.hero_art.pixels = g_hero;
    m.hero_art.w = HERO_W;
    m.hero_art.h = HERO_H;
    m.recent        = items;
    m.recent_count  = 7;
    m.clock         = "18:23";
    m.theme_name    = evo_theme_name(evo_theme_index());

    evo_grid_init(&g, EVO_LAUNCH_ROWS);
    evo_screen_launch_sync(&g, &m);
    g.row = sel_row;
    if (sel_col > 0) for (int i = 0; i < sel_col; i++) evo_grid_move_h(&g, 1);

    /* Seed the glide so the still shows the settled state, not a frame
     * mid-travel with a stray ring in it. */
    g.glide_ready = 0;
    evo_grid_tick(&g, EVO_BLEED_X, EVO_TILE_PITCH, 0);

    evo_screen_launch(g_fb, &m, &g);
}

static void render_browse(int sel, int rail_focused, int rail_sel, int empty)
{
    static const char *names[] = {
        "cin-end-credits-t1x.mp4", "A Very Long Documentary Title 2160p HDR.mkv",
        "files.json", "grounded-bts.mp4", "left-behind_ps4-credits.mp4",
        "leftbehind-bts.mp4", "ps-studios-intro.mp4", "Season 01",
        "trailer-4k-hdr.mkv", "LPCM 7.1.wav"
    };
    static const char *dets[] = {
        "VIDEO  -  342.42 MB", "VIDEO  -  8.10 GB", "FILE  -  356.00 B",
        "VIDEO  -  1.19 GB", "VIDEO  -  210.48 MB", "VIDEO  -  928.51 MB",
        "VIDEO  -  14.12 MB", "FOLDER", "VIDEO  -  4.402 GB",
        "AUDIO  -  102.11 MB"
    };
    static const evo_file_kind kinds[] = {
        EVO_FILE_VIDEO, EVO_FILE_VIDEO, EVO_FILE_OTHER, EVO_FILE_VIDEO,
        EVO_FILE_VIDEO, EVO_FILE_VIDEO, EVO_FILE_VIDEO, EVO_FILE_FOLDER,
        EVO_FILE_VIDEO, EVO_FILE_AUDIO
    };
    const int N = 10;
    evo_browser_entry entries[10];
    evo_browser_inspect ins;
    evo_browser_model m;
    evo_focus f;

    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < N; i++) {
        entries[i].name     = names[i];
        entries[i].detail   = dets[i];
        entries[i].kind     = kinds[i];
        entries[i].favorite = (i == 1 || i == 5);
        entries[i].progress = (i == 5) ? 620 : -1;
    }

    memset(&ins, 0, sizeof(ins));
    ins.name        = names[sel < N ? sel : 0];
    ins.kind        = "VIDEO";
    ins.extension   = "MP4";
    ins.container   = "mov mp4 m4a 3gp 3g2 mj2";
    ins.size        = "342.42 MB";
    ins.duration    = "21M 07S";
    ins.resolution  = "2560 x 1440";
    ins.video_codec = "h264";
    ins.audio_codec = "aac";
    ins.subtitles   = "NO";
    ins.preview.pixels = g_art;
    ins.preview.w = ART_W;
    ins.preview.h = ART_H;
    ins.preview_badge  = "21M 07S";

    memset(&m, 0, sizeof(m));
    m.path        = "/usb0/homebrew/PPSA03396-app0/build/ps5/main/movie1";
    m.entries     = entries;
    m.first       = 0;
    m.entry_count = N;
    m.count       = empty ? 0 : N;
    m.inspect     = empty ? NULL : &ins;

    evo_focus_init(&f, m.count, evo_screen_browser_capacity(), 1);
    evo_focus_set(&f, sel);
    evo_focus_tick(&f, EVO_CONTENT_Y, EVO_ROW_PITCH, 0);

    evo_screen_browser(g_fb, &m, &f, rail_focused, rail_sel);
}

static void render_list(const char *which, int sel, int rail_focused,
                        int rail_sel, int empty)
{
    /* Sized for the longest list, which is the changelog - every other
     * fixture here is eight rows or fewer. */
    evo_list_entry e[EVO_CHANGELOG_COUNT > 8 ? EVO_CHANGELOG_COUNT : 8];
    evo_list_model m;
    evo_focus f;
    int n = 0;

    memset(e, 0, sizeof(e));
    memset(&m, 0, sizeof(m));

    if (!strcmp(which, "settings")) {
        static const char *t[] = {
            "PLAYBACK & VIDEO",
            "SUBTITLES",
            "INTERFACE & CONTROLS",
            "SYSTEM & DIAGNOSTICS"
        };
        static const char *d[] = {
            "PROFILE, ASPECT RATIO & RESUME",
            "AUTO-DETECT & DEFAULT SIZING",
            "THEMES, SOUNDS, LIGHTBAR & SORTING",
            "DEVELOPER TOOLS & MEDIA TILE"
        };
        static const int ic[] = {
            EVO_IC_RESUME,
            EVO_IC_SUBTITLES,
            EVO_IC_PALETTE,
            EVO_IC_TOOLS
        };
        n = 4;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="SETTINGS"; m.subtitle="APPLICATION & PLAYBACK PREFERENCES";
        m.section=EVO_SECTION_SETTINGS;
    } else if (!strcmp(which, "settings_playback")) {
        static const char *t[] = { "PLAYBACK PROFILE","DEFAULT ASPECT RATIO","RESUME PLAYBACK","SURROUND SOUND TEST" };
        static const char *d[] = { "Performance","FIT","ON","5.1 & 7.1 SPEAKER CHANNEL VERIFICATION" };
        static const int ic[] = { EVO_IC_SETTINGS, EVO_IC_ASPECT, EVO_IC_RESUME, EVO_IC_RESUME };
        n = 4;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="PLAYBACK & VIDEO"; m.subtitle="SETTINGS  -  PROFILES, ASPECT RATIO & RESUME";
        m.section=EVO_SECTION_SETTINGS;
    } else if (!strcmp(which, "settings_subtitles")) {
        static const char *t[] = { "AUTO-DETECT SUBTITLES","DEFAULT SUBTITLE SIZE" };
        static const char *d[] = { "ON","MEDIUM" };
        static const int ic[] = { EVO_IC_SUBTITLES, EVO_IC_SUBTITLES };
        n = 2;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="SUBTITLES"; m.subtitle="SETTINGS  -  DETECTION & DEFAULT SIZING";
        m.section=EVO_SECTION_SETTINGS;
    } else if (!strcmp(which, "settings_interface")) {
        static const char *t[] = { "THEME","NAVIGATION SOUNDS","CONTROLLER LIGHTBAR","FOLDERS FIRST","KEYBOARD INPUT" };
        static char theme_val[64];
        static const char *d[5];
        static const int ic[] = { EVO_IC_PALETTE, EVO_IC_SUBTITLES, EVO_IC_PALETTE, EVO_IC_FOLDER, EVO_IC_SETTINGS };
        static uint32_t sw[3];
        const evo_theme *cur = evo_theme_current();

        snprintf(theme_val, sizeof(theme_val), "%s  -  %d OF %d",
                 evo_theme_name(evo_theme_index()),
                 evo_theme_index() + 1, evo_theme_count());
        d[0] = theme_val; d[1] = "ON"; d[2] = "THEME ACCENT"; d[3] = "ON"; d[4] = "NATIVE PS5 IME";
        n = 5;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        sw[0] = cur->accent; sw[1] = cur->surface; sw[2] = cur->bg_top;
        e[0].swatches = sw; e[0].swatch_count = 3;
        m.title="INTERFACE & CONTROLS"; m.subtitle="SETTINGS  -  THEMES, SOUNDS & CONTROLS";
        m.section=EVO_SECTION_SETTINGS;

    } else if (!strcmp(which, "settings_system")) {
        static const char *t[] = { "COMPATIBILITY REPORT","DEBUG OVERLAY","REMOVE HOME TILE" };
        static const char *d[] = { "WRITES A CODEC REPORT TO USB0","OFF","REMOVES MEDIA TILE" };
        static const int ic[] = { EVO_IC_TOOLS, EVO_IC_ASPECT, EVO_IC_TRASH };
        n = 3;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="SYSTEM & DIAGNOSTICS"; m.subtitle="SETTINGS  -  DIAGNOSTICS & SYSTEM MANAGEMENT";
        m.section=EVO_SECTION_SETTINGS;
    } else if (!strcmp(which, "tools")) {
        static const char *t[] = { "COMPATIBILITY REPORT","DEBUG OVERLAY",
            "NAVIGATION SOUNDS","LIGHTBAR" };
        static const char *d[] = { "WRITES A CODEC REPORT TO USB0","OFF",
            "ON","THEME ACCENT" };
        static const int ic[] = { EVO_IC_TOOLS,EVO_IC_ASPECT,EVO_IC_SUBTITLES,
            EVO_IC_PALETTE };
        n = 4;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="DEVELOPER TOOLS"; m.subtitle="DIAGNOSTICS & SYSTEM REPORTS";
        m.section=EVO_SECTION_SETTINGS;
    } else if (!strcmp(which, "emby_setup")) {
        static const char *t[] = { "SERVER HOST","SERVER PORT","ACCOUNT USERNAME",
            "CONNECTION STATUS","EXPLORE MEDIA LIBRARIES" };
        static const char *d[] = { "192.168.0.11  (PRESS X TO EDIT)",
            "8096  (PRESS X TO EDIT)",
            "bin  (PRESS X TO EDIT)",
            "CONNECTED (PRESS X TO DISCONNECT)",
            "BROWSE MOVIES, TV SHOWS & COLLECTIONS" };
        static const int ic[] = { EVO_IC_SETTINGS,EVO_IC_TOOLS,EVO_IC_RESUME,
            EVO_IC_SUBTITLES,EVO_IC_FOLDER };
        n = 5;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="EMBY MEDIA SERVER"; m.subtitle="STREAMING ADDON CONFIGURATION";
        m.section=EVO_SECTION_EMBY;
    } else if (!strcmp(which, "emby_browse")) {
        static const char *t[] = { "Movies","TV Shows","Anime Collection","Music Videos" };
        static const char *d[] = { "LIBRARY - movies","LIBRARY - tvshows","LIBRARY - anime","LIBRARY - musicvideos" };
        n = 4;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=EVO_IC_FOLDER;
            e[i].chevron=1; e[i].progress=-1;
        }
        m.title="EMBY BROWSER"; m.subtitle="EMBY  -  ROOT LIBRARIES";
        m.section=EVO_SECTION_EMBY;
        m.empty_title="NO ITEMS FOUND";
        m.empty_hint="PRESS CIRCLE TO GO BACK";
        m.empty_icon=EVO_IC_FOLDER;
    } else if (!strcmp(which, "recent")) {
        static const char *t[] = { "leftbehind-bts", ("A Very Long Documentary "
            "Title That Must Ellipsise"), "DTS 5.1", "Dolby TrueHD 7.1", "AAC 5.1" };
        static const char *d[] = { "7M 58S LEFT","1H 12M LEFT","0M 01S LEFT",
            "0M 09S LEFT","0M 40S LEFT" };
        n = 5;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=EVO_IC_RECENT;
            e[i].progress=(i*211)%1000;
        }
        m.title="RECENT"; m.subtitle="PICK UP WHERE YOU LEFT OFF";
        m.section=EVO_SECTION_RECENT;
        m.empty_title="NOTHING PLAYED YET";
        m.empty_hint="FILES YOU OPEN WILL APPEAR HERE";
        m.empty_icon=EVO_IC_RECENT;
    } else if (!strcmp(which, "favorites")) {
        static const char *t[] = { "leftbehind-bts","trailer-4k-hdr" };
        static const char *d[] = { "15M 28S","2M 11S" };
        n = 2;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=EVO_IC_FAVORITE;
            e[i].progress=-1;
        }
        m.title="FAVORITES"; m.subtitle="MEDIA YOU SAVED FOR LATER";
        m.section=EVO_SECTION_FAVORITES;
        m.empty_title="NO FAVORITES YET";
        m.empty_hint="PRESS TRIANGLE ON A FILE IN THE BROWSER";
        m.empty_icon=EVO_IC_FAVORITE;
    } else if (!strcmp(which, "profile")) {
        static const char *t[] = { "BALANCED","PERFORMANCE","COMPATIBILITY",
            "DEBUG" };
        static const char *d[] = {
            "Even trade between smoothness and format coverage",
            "ACTIVE",
            "Widest format support - try this when a file will not play",
            "Verbose diagnostics and on-screen counters" };
        n = 4;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=EVO_IC_SETTINGS;
            e[i].chevron=0; e[i].progress=-1;
        }
        e[1].info = 1;                    /* the active profile states a fact */
        m.title="PLAYBACK PROFILE";
        m.subtitle="HOW AGGRESSIVELY THE DECODER IS TUNED";
        m.section=EVO_SECTION_SETTINGS;   /* a child of settings */
    } else if (!strcmp(which, "changelog")) {
        for (int i = 0; i < EVO_CHANGELOG_COUNT; i++) {
            const evo_changelog_row *r = &EVO_CHANGELOG[i];
            switch (r->kind) {
                case EVO_CL_VERSION: e[i].title="VERSION";
                                     e[i].icon=EVO_IC_ABOUT;  break;
                case EVO_CL_NEW:     e[i].title="NEW";
                                     e[i].icon=EVO_IC_RESUME; break;
                case EVO_CL_FIXED:   e[i].title="FIXED";
                                     e[i].icon=EVO_IC_TOOLS;  break;
                default:             e[i].title="REMOVED";
                                     e[i].icon=EVO_IC_TRASH;  break;
            }
            e[i].detail=r->text; e[i].progress=-1; e[i].info=1;
        }
        n = EVO_CHANGELOG_COUNT;
        m.title="CHANGELOG"; m.subtitle="WHAT CHANGED IN EACH RELEASE";
        m.section=EVO_SECTION_ABOUT;   /* a child of about */
    } else { /* about */
        static const char *t[] = { "VERSION","EVO PLAYER","BUILT ON","THEMES",
            "SCREENSHOTS","CHANGELOG" };
        static const char *d[] = { EVO_PLAYER_VERSION,
            "MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW",
            "FFMPEG AND THE PS5 PAYLOAD SDK",
            "4 AVAILABLE - DROP .THEME FILES ON USB0",
            "PRESS L3 OR R3 IN ANY MENU",
            "WHAT CHANGED IN EACH RELEASE" };
        static const int ic[] = { EVO_IC_ABOUT,EVO_IC_RESUME,EVO_IC_TOOLS,
            EVO_IC_PALETTE,EVO_IC_ASPECT,EVO_IC_RECENT };
        n = 6;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].progress=-1; e[i].info=1;
        }
        /* The changelog row is the only one you can open, so it keeps the
         * chevron and the action styling. */
        e[5].info = 0; e[5].chevron = 1;
        m.title="ABOUT"; m.subtitle="CREDITS AND PROJECT INFO";
        m.section=EVO_SECTION_ABOUT;
    }

    if (empty) n = 0;
    m.entries = e;
    m.count   = n;

    evo_focus_init(&f, n, evo_screen_list_capacity(), 1);
    evo_focus_set(&f, sel);
    evo_focus_tick(&f, EVO_CONTENT_Y, EVO_ROW_PITCH, 0);

    evo_screen_list(g_fb, &m, &f, rail_focused, rail_sel,
                    EVO_HINTS_LIST, EVO_HINTS_LIST_N);
}

/* ------------------------------------------------------------------------ */

/*
 * The modal screens draw OVER whatever is in the framebuffer - during
 * playback that is the last decoded video frame. Filling the buffer with the
 * stand-in artwork first is what makes their scrim judgeable; against an
 * empty buffer a dialog looks like it is on a plain background and you cannot
 * tell whether the dimming works.
 */
static void fill_fake_video(void)
{
    /* From the hero-sized buffer, not the tile-sized one: this fills the whole
     * 1920x1080 frame, and a 320px source blown up six times is chunky enough
     * to be mistaken for a fault in the scrim it sits under. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int sx = (x * HERO_W) / W;
            int sy = (y * HERO_H) / H;
            g_fb[y * W + x] = g_hero[sy * HERO_W + sx];
        }
}

static void render_resume(void)
{
    static const evo_dialog_action a[2] = {
        { EVO_GLYPH_CROSS, "RESUME" }, { EVO_GLYPH_CIRCLE, "START OVER" }
    };
    evo_dialog_model m;

    fill_fake_video();
    memset(&m, 0, sizeof(m));
    m.eyebrow = "RESUME PLAYBACK";
    m.title   = "leftbehind-bts";
    m.detail  = "STOPPED AT 12M 04S OF 21M 07S";
    m.progress = 572;
    m.actions = a; m.action_count = 2;
    m.art.pixels = g_art; m.art.w = ART_W; m.art.h = ART_H;
    evo_screen_dialog(g_fb, &m);
}

static void render_finished(void)
{
    static const evo_dialog_action a[3] = {
        { EVO_GLYPH_CROSS, "REPLAY" }, { EVO_GLYPH_TRIANGLE, "NEXT" },
        { EVO_GLYPH_CIRCLE, "BACK" }
    };
    evo_dialog_model m;

    fill_fake_video();
    memset(&m, 0, sizeof(m));
    m.eyebrow = "FINISHED";
    m.title   = "leftbehind-bts";
    m.detail  = "PLAYBACK REACHED THE END OF THE FILE";
    m.progress = 1000;
    m.actions = a; m.action_count = 3;
    m.art.pixels = g_art; m.art.w = ART_W; m.art.h = ART_H;
    evo_screen_dialog(g_fb, &m);
}

/*
 * The stop-playback prompt.
 *
 * The action order is the thing worth looking at here: KEEP WATCHING is
 * first, so it is the primary chip and CIRCLE - the button that opened the
 * prompt - is the one that dismisses it. See draw_exit_confirm_screen() in
 * main.c for why that inverts the usual reading of the two buttons.
 */
static void render_exitconfirm(void)
{
    static const evo_dialog_action a[2] = {
        { EVO_GLYPH_CIRCLE, "KEEP WATCHING" },
        { EVO_GLYPH_CROSS,  "STOP"          }
    };
    evo_dialog_model m;

    fill_fake_video();
    memset(&m, 0, sizeof(m));
    m.eyebrow  = "STOP PLAYBACK";
    m.title    = "leftbehind-bts";
    m.detail   = "AT 12M 04S OF 21M 07S  -  RESUMES FROM HERE";
    m.progress = 572;
    m.actions = a; m.action_count = 2;
    m.art.pixels = g_art; m.art.w = ART_W; m.art.h = ART_H;
    evo_screen_dialog(g_fb, &m);
}

static void render_mediainfo(void)
{
    static const evo_prop props[8] = {
        { "CONTAINER",  "mov mp4 m4a 3gp 3g2 mj2" },
        { "SIZE",       "928.51 MB"               },
        { "LENGTH",     "21M 07S"                 },
        { "RESOLUTION", "3840 x 2160"             },
        { "VIDEO",      "hevc"                    },
        { "AUDIO",      "eac3"                    },
        { "OUTPUT",     "8 CH  -  48000 HZ"       },
        { "SUBTITLES",  "YES"                     }
    };
    evo_info_model m;

    memset(&m, 0, sizeof(m));
    m.title      = "MEDIA INFO";
    m.subtitle   = "leftbehind-bts";
    m.props      = props;
    m.prop_count = 8;
    m.art_badge  = "21M 07S";
    m.art.pixels = g_art; m.art.w = ART_W; m.art.h = ART_H;
    evo_screen_info(g_fb, &m);
}

/* Toasts are an overlay, so they are rendered on top of a real screen -
 * against an empty buffer you cannot tell whether the panel is opaque enough
 * to read over content, which is the only thing worth checking. */
static void render_toast(evo_toast_kind kind)
{
    evo_toast t;

    render_list("settings", 6, 0, 1, 0);

    memset(&t, 0, sizeof(t));
    t.alpha = 255;
    t.slide = 0;
    t.kind  = kind;

    switch (kind) {
        case EVO_TOAST_ERROR:
            t.title = "PLAYBACK FAILED"; t.message = "No decoder for this stream"; break;
        case EVO_TOAST_OK:
            t.title = "FAVORITES"; t.message = "Added leftbehind-bts"; break;
        default:
            t.title = "THEME";
            t.message = evo_theme_name(evo_theme_index()); break;
    }

    evo_widget_toast(g_fb, &t);
}

static void render_surround_studio(int sel, int rail, int rail_sel)
{
    const evo_theme *th = evo_theme_current();
    evo_page page;
    memset(&page, 0, sizeof(page));
    page.title = "SURROUND SOUND STUDIO";
    page.subtitle = "CALIBRATION  -  7.1 SPEAKER SYSTEM (8 CHANNELS)";
    page.section = EVO_SECTION_SETTINGS;
    page.rail_focused = rail;
    page.rail_index = rail_sel;

    static const evo_hint hints[] = {
        { EVO_GLYPH_CROSS,    "TEST / ACTIVATE" },
        { EVO_GLYPH_SQUARE,   "5.1 / 7.1 MODE" },
        { EVO_GLYPH_TRIANGLE, "SILENCE" },
        { EVO_GLYPH_DPAD,     "NAVIGATE" },
        { EVO_GLYPH_CIRCLE,   "BACK" }
    };

    evo_chrome_begin(g_fb, &page);

    int mon_x = 130, mon_y = 160, mon_w = 460, mon_h = 220;
    evo_ui_round_rect(g_fb, mon_x, mon_y, mon_w, mon_h, 18,
                      th->surface_alt, th->bg_bottom,
                      th->border, 1,
                      0x44000000, 16);

    evo_text(g_fb, mon_x + 24, mon_y + 18, "SPEAKER CALIBRATION MONITOR", th->accent, EVO_FACE_SMALL);
    evo_text(g_fb, mon_x + 24, mon_y + 48, "FRONT LEFT (FL)", th->accent, EVO_FACE_MENU);
    evo_text(g_fb, mon_x + 24, mon_y + 92, "TONE FREQ: 330.0 HZ (E4)", th->text_secondary, EVO_FACE_SMALL);
    evo_text(g_fb, mon_x + 24, mon_y + 124, "PS5 AUDIO OUT: S16_8CH (CH 0)", th->text_muted, EVO_FACE_SMALL);
    evo_text(g_fb, mon_x + 24, mon_y + 158, "STATUS: [ ACTIVE NOW ]", th->accent, EVO_FACE_SMALL);

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
        int is_sel = (!rail && sel == i);
        int cur_y = act_y + i * 80;
        uint32_t fill_top = is_sel ? th->surface_sel : th->surface;
        uint32_t fill_bot = is_sel ? th->surface_sel_alt : th->surface_alt;
        uint32_t border = is_sel ? th->border_sel : th->border;

        evo_ui_round_rect(g_fb, mon_x, cur_y, mon_w, 72, 14,
                          fill_top, fill_bot, border, is_sel ? 2 : 1,
                          is_sel ? 0x66000000 : 0x22000000, is_sel ? 14 : 6);

        if (is_sel) {
            evo_ui_round_rect(g_fb, mon_x + 3, cur_y + 14, 5, 44, 3,
                              th->accent, th->accent, th->accent, 0, 0, 0);
        }

        evo_text(g_fb, mon_x + 24, cur_y + 12, actions[i].label, is_sel ? th->text_primary : th->text_secondary, EVO_FACE_MENU);
        evo_text(g_fb, mon_x + 24, cur_y + 44, (i == 3) ? "CURRENT: 7.1 SURROUND" : actions[i].sub, is_sel ? th->accent : th->text_muted, EVO_FACE_SMALL);
    }

    int stage_x = 610, stage_y = 160, stage_w = 1180, stage_h = 760;
    evo_ui_round_rect(g_fb, stage_x, stage_y, stage_w, stage_h, 24,
                      th->surface_alt, th->bg_top,
                      th->border, 1,
                      0x55000000, 20);

    int scr_w = 420, scr_h = 14;
    int scr_x = stage_x + (stage_w - scr_w) / 2;
    int scr_y = stage_y + 24;
    evo_ui_round_rect(g_fb, scr_x, scr_y, scr_w, scr_h, 7,
                      th->accent, th->accent_soft, th->accent, 1,
                      0x4419d8ff, 10);
    evo_text(g_fb, scr_x + (scr_w - evo_text_w("FRONT DISPLAY / TV SCREEN", EVO_FACE_SMALL)) / 2,
             scr_y + 22, "FRONT DISPLAY / TV SCREEN", th->text_muted, EVO_FACE_SMALL);

    int couch_cx = stage_x + stage_w / 2;
    int couch_cy = stage_y + 390;

    evo_ui_circle(g_fb, couch_cx, couch_cy, 120, 0x14FFFFFF);
    evo_ui_circle(g_fb, couch_cx, couch_cy, 230, 0x0BFFFFFF);
    evo_ui_circle(g_fb, couch_cx, couch_cy, 340, 0x06FFFFFF);

    int couch_w = 130, couch_h = 66;
    evo_ui_round_rect(g_fb, couch_cx - couch_w / 2, couch_cy - couch_h / 2, couch_w, couch_h, 14,
                      th->surface, th->surface_alt, th->border, 1, 0x44000000, 8);
    evo_text(g_fb, couch_cx - evo_text_w("LISTENER", EVO_FACE_MENU) / 2,
             couch_cy - 20, "LISTENER", th->text_primary, EVO_FACE_MENU);
    evo_text(g_fb, couch_cx - evo_text_w("SWEET SPOT", EVO_FACE_SMALL) / 2,
             couch_cy + 12, "SWEET SPOT", th->accent, EVO_FACE_SMALL);

    static const struct {
        const char *name;
        const char *label;
        double hz;
        int dx;
        int dy;
        int w;
        int h;
        int is_act;
    } spk[8] = {
        { "CENTER",       "FC",   554.0,  -75, -260, 150, 82, 0 },
        { "SUBWOOFER",   "LFE",   55.0,   85, -260, 150, 82, 0 },
        { "FRONT LEFT",   "FL",  330.0, -460, -180, 150, 82, 1 },
        { "FRONT RIGHT",  "FR",  440.0,  310, -180, 150, 82, 0 },
        { "SIDE LEFT",    "SL", 1109.0, -510,   10, 150, 82, 0 },
        { "SIDE RIGHT",   "SR", 1319.0,  360,   10, 150, 82, 0 },
        { "BACK LEFT",    "BL",  659.0, -380,  200, 150, 82, 0 },
        { "BACK RIGHT",   "BR",  880.0,  230,  200, 150, 82, 0 }
    };

    int is_51 = 0; /* Test 7.1 layout with all 8 speakers */

    for (int i = 0; i < 8; i++) {
        if (is_51 && (!strcmp(spk[i].label, "SL") || !strcmp(spk[i].label, "SR"))) {
            continue;
        }
        int sx = couch_cx + spk[i].dx;
        int sy = couch_cy + spk[i].dy;
        int sw = spk[i].w;
        int sh = spk[i].h;
        int is_act = spk[i].is_act;
        int is_sel = (!rail && sel == (i + 5));

        if (is_act) {
            evo_ui_circle(g_fb, sx + sw / 2, sy + sh / 2, 75, 0x3319D8FF);
            evo_ui_circle(g_fb, sx + sw / 2, sy + sh / 2, 105, 0x1A19D8FF);
        }

        uint32_t fill_t = is_act ? 0xDD004466 : (is_sel ? th->surface_sel : th->surface);
        uint32_t fill_b = is_act ? 0xDD002233 : (is_sel ? th->surface_sel_alt : th->surface_alt);
        uint32_t border = is_act ? 0xFF00E5FF : (is_sel ? th->border_sel : th->border);

        evo_ui_round_rect(g_fb, sx, sy, sw, sh, 16,
                          fill_t, fill_b, border, (is_act || is_sel) ? 2 : 1,
                          is_act ? 0x8819D8FF : (is_sel ? 0x66000000 : 0x22000000),
                          is_act ? 18 : (is_sel ? 12 : 6));

        if (is_sel) {
            evo_ui_round_rect(g_fb, sx + 4, sy + 14, 5, sh - 28, 3,
                              th->accent, th->accent, th->accent, 0, 0, 0);
        }

        uint32_t txt_c = is_act ? 0xFF00FFFF : (is_sel ? th->text_primary : th->text_secondary);
        evo_text(g_fb, sx + 20, sy + 14, spk[i].label, txt_c, EVO_FACE_MENU);

        char hz_str[32];
        if (is_act) {
            snprintf(hz_str, sizeof(hz_str), "%.0f Hz [ON]", spk[i].hz);
        } else {
            snprintf(hz_str, sizeof(hz_str), "%.0f Hz", spk[i].hz);
        }
        evo_text(g_fb, sx + 20, sy + 46, hz_str,
                 is_act ? th->accent : th->text_muted,
                 EVO_FACE_SMALL);
    }

    evo_chrome_end(g_fb, &page, hints, 5);
}

/*
 * Subtitle picker.
 *
 * Modelled on the file that prompted it: thirty-odd tracks, two of them
 * English, and the one flagged "default" holding two cues. The fixture keeps
 * that shape because it is the case the screen exists to make legible - a
 * fixture of three tidy tracks would demonstrate nothing.
 */
static void render_picker(int sel, int size_face)
{
    static const struct { const char *label; const char *detail; int weak; }
    tracks[] = {
        { "SUBTITLES OFF",  "",           0 },
        { "ENGLISH",        "SIGNS ONLY", 1 },
        { "ENGLISH SDH",    "1001 CUES",  0 },
        { "ARABIC",         "998 CUES",   0 },
        { "CZECH",          "994 CUES",   0 },
        { "DANISH",         "1002 CUES",  0 },
        { "GERMAN",         "999 CUES",   0 },
        { "GREEK",          "1000 CUES",  0 },
        { "SPANISH",        "997 CUES",   0 },
        { "SPANISH 2",      "997 CUES",   0 },
        { "FINNISH",        "995 CUES",   0 },
        { "FRENCH",         "1003 CUES",  0 }
    };
    const int count = (int)(sizeof(tracks) / sizeof(tracks[0]));
    const int cap   = evo_screen_picker_capacity();

    evo_picker_entry rows[16];
    evo_picker_model m;
    evo_focus f;
    int first, shown, i;
    static char title_buf[64];
    const char *size_str = (size_face == EVO_FACE_SUB)   ? "SMALL" :
                           (size_face == EVO_FACE_TITLE) ? "LARGE" : "MEDIUM";

    fill_fake_video();

    memset(&f, 0, sizeof(f));
    f.visible = cap;
    evo_focus_set_count(&f, count);
    evo_focus_set(&f, sel);

    first = f.scroll;
    shown = count - first;
    if (shown > cap) shown = cap;
    if (shown > (int)(sizeof(rows) / sizeof(rows[0])))
        shown = (int)(sizeof(rows) / sizeof(rows[0]));

    for (i = 0; i < shown; i++) {
        rows[i].label   = tracks[first + i].label;
        rows[i].detail  = tracks[first + i].detail;
        rows[i].weak    = tracks[first + i].weak;
        rows[i].current = (first + i == 2);   /* SDH is what is playing */
    }

    snprintf(title_buf, sizeof(title_buf), "SELECT TRACK  -  SIZE: %s", size_str);

    memset(&m, 0, sizeof(m));
    m.eyebrow      = "SUBTITLES";
    m.title        = title_buf;
    m.entries      = rows;
    m.first        = first;
    m.entry_count  = shown;
    m.count        = count;
    m.preview_face = size_face;
    m.preview_text = NULL;

    evo_screen_picker(g_fb, &m, &f);
}

static void render_subtitles(int face)
{
    fill_fake_video();

    const char *line1 = "Welcome to EVO Player on PlayStation 5";
    const char *line2 = (face == EVO_FACE_SUB)   ? "Subtitle size: SMALL (EVO_FACE_SUB) with adaptive outline and shadow" :
                        (face == EVO_FACE_TITLE) ? "Subtitle size: LARGE (EVO_FACE_TITLE)" :
                        "Subtitle size: MEDIUM (EVO_FACE_MENU) default size";

    const char *lines[2] = { line1, line2 };
    int line_count = 2;

    int bottom_y = 910;
    int line_spacing = (face == EVO_FACE_SUB)   ? 56 :
                       (face == EVO_FACE_TITLE) ? 96 : 76;
    int first_y = bottom_y - (line_count - 1) * line_spacing;

    for (int i = 0; i < line_count; i++) {
        const char *line = lines[i];
        int tw = evo_font_text_width(line, face);
        int x = (W - tw) / 2;
        int y = first_y + i * line_spacing;

        /* Soft lower shadow */
        rr_text(g_fb, x + 2, y + 3, line, 0xBE000000, face);

        /* Thin outline around letters */
        rr_text(g_fb, x - 2, y, line, 0xF5000000, face);
        rr_text(g_fb, x + 2, y, line, 0xF5000000, face);
        rr_text(g_fb, x, y - 2, line, 0xF5000000, face);
        rr_text(g_fb, x, y + 2, line, 0xF5000000, face);

        /* Main text */
        rr_text(g_fb, x, y, line, 0xFFFAFCFF, face);
    }
}


/*
 * The text reader, on a fixture that is deliberately awkward: curly quotes and
 * an em dash that the loader has to fold, a line long enough to wrap several
 * times, and enough punctuation to show whether the generated glyphs sit on
 * the same baseline as the letters. A fixture of clean uppercase prose would
 * prove nothing - that already worked.
 */
static int g_reader_face = EVO_FACE_SUB;

static void render_reader(int sel)
{
    static const char SAMPLE[] =
        "The Adventures of the Speckled Band\n"
        "\n"
        "On glancing over my notes of the seventy odd cases in which I have "
        "during the last eight years studied the methods of my friend Sherlock "
        "Holmes, I find many tragic, some comic, a large number merely strange, "
        "but none commonplace.\n"
        "\n"
        "\"Very sorry to knock you up, Watson,\" said he, \"but it's the common "
        "lot this morning. Mrs. Hudson has been knocked up, she retorted upon "
        "me - and I on you.\"\n"
        "\n"
        "  * A list item, indented.\n"
        "  * Another - with a dash, an ellipsis... and a query?\n"
        "\n"
        "Costs: $4.50 (approx. 60%) & rising; see notes [1], {2}, <3>.\n"
        "Path: /mnt/usb0/books/holmes.txt  |  ~4.2 KB  |  100% loaded\n";

    evo_text_doc doc;
    evo_reader_model m;
    const char *win[EVO_READER_MAX_VISIBLE];
    char buf[EVO_READER_MAX_VISIBLE][512];
    char badge[64], sub[128];
    int cap, shown, i, page_w;

    /* The loader reads a file, so the fixture is written to one. That keeps
     * the host path identical to the console path - including the decode. */
    {
        const char *tmp = "/tmp/uiview_reader_fixture.txt";
        FILE *f = fopen(tmp, "wb");
        if (f) { fwrite(SAMPLE, 1, sizeof(SAMPLE) - 1, f); fclose(f); }
        if (evo_text_load(&doc, tmp) != EVO_TEXT_OK) {
            memset(&m, 0, sizeof(m));
            m.title = "READER"; m.notice = "COULD NOT LOAD THE FIXTURE";
            evo_screen_reader(g_fb, &m, 0, 0, EVO_HINTS_LIST, EVO_HINTS_LIST_N);
            return;
        }
    }

    doc.face = g_reader_face;
    cap = evo_screen_reader_capacity(doc.face);

    /* The screen owns its own column width - asking it is the only way the
     * wrap and the draw cannot disagree. */
    page_w = evo_screen_reader_wrap_w();
    evo_text_wrap(&doc, page_w, reader_measure);

    evo_text_scroll(&doc, sel, cap);

    shown = doc.line_count - doc.top;
    if (shown > cap) shown = cap;
    if (shown < 0) shown = 0;

    for (i = 0; i < shown; i++) {
        int n = doc.lines[doc.top + i].len;
        if (n > (int)sizeof(buf[0]) - 1) n = (int)sizeof(buf[0]) - 1;
        memcpy(buf[i], doc.lines[doc.top + i].begin, (size_t)n);
        buf[i][n] = 0;
        win[i] = buf[i];
    }

    snprintf(badge, sizeof(badge), "LINE %d OF %d",
             doc.line_count ? doc.lines[doc.top].source_line : 0,
             doc.source_lines);
    snprintf(sub, sizeof(sub), "%zu BYTES", doc.file_bytes);

    memset(&m, 0, sizeof(m));
    m.title        = doc.title;
    m.subtitle     = sub;
    m.badge        = badge;
    m.lines        = win;
    m.line_count   = shown;
    m.face         = doc.face;
    m.line_pitch   = evo_screen_reader_pitch(doc.face);
    m.progress     = evo_text_progress(&doc, cap);
    m.visible_frac = doc.line_count ? (double)cap / (double)doc.line_count : 1.0;

    {
        static const evo_hint hints[4] = {
            { EVO_GLYPH_DPAD,   "SCROLL" },
            { EVO_GLYPH_LSTICK, "PAGE" },
            { EVO_GLYPH_TRIANGLE, "SIZE" },
            { EVO_GLYPH_CIRCLE, "BACK" }
        };
        evo_screen_reader(g_fb, &m, 0, 0, hints, 4);
    }

    evo_text_free(&doc);
}

static void render_keyboard(void)
{
    render_list("emby_setup", 0, 0, 0, 0);
    evo_keyboard_open("ENTER SERVER HOST IP OR DOMAIN", "192.168.0.11", 64, NULL, NULL);
    evo_screen_keyboard(g_fb);
}

static const char *SCREENS[] = { "launch","browse","recent","favorites",
                                 "settings","settings_playback","settings_surround",
                                 "settings_subtitles",
                                 "settings_interface","settings_system",
                                 "profile","tools","about",
                                 "changelog","emby_setup","emby_browse",
                                 "keyboard",
                                 "resume","finished","mediainfo","exitconfirm",
                                 "picker","picker_small","picker_medium","picker_large",
                                 "subs_small","subs_medium","subs_large",
                                 "reader",
                                 "toast","toastok","toasterror", NULL };

static void render_one(const char *screen, int sel, int rail, int rail_sel,
                       int empty, int row)
{
    memset(g_fb, 0, sizeof(g_fb));

    if      (!strcmp(screen, "launch"))        render_launch(row, sel);
    else if (!strcmp(screen, "browse"))        render_browse(sel, rail, rail_sel, empty);
    else if (!strcmp(screen, "resume"))        render_resume();
    else if (!strcmp(screen, "finished"))      render_finished();
    else if (!strcmp(screen, "mediainfo"))     render_mediainfo();
    else if (!strcmp(screen, "exitconfirm"))   render_exitconfirm();
    else if (!strcmp(screen, "picker"))        render_picker(sel, EVO_FACE_MENU);
    else if (!strcmp(screen, "picker_small"))  render_picker(sel, EVO_FACE_SUB);
    else if (!strcmp(screen, "picker_medium")) render_picker(sel, EVO_FACE_MENU);
    else if (!strcmp(screen, "picker_large"))  render_picker(sel, EVO_FACE_TITLE);
    else if (!strcmp(screen, "subs_small"))    render_subtitles(EVO_FACE_SUB);
    else if (!strcmp(screen, "subs_medium"))   render_subtitles(EVO_FACE_MENU);
    else if (!strcmp(screen, "subs_large"))    render_subtitles(EVO_FACE_TITLE);
    else if (!strcmp(screen, "reader"))        render_reader(sel);
    else if (!strcmp(screen, "keyboard"))      render_keyboard();
    else if (!strcmp(screen, "settings_surround")) render_surround_studio(sel, rail, rail_sel);
    else if (!strcmp(screen, "toast"))         render_toast(EVO_TOAST_INFO);
    else if (!strcmp(screen, "toastok"))       render_toast(EVO_TOAST_OK);
    else if (!strcmp(screen, "toasterror"))    render_toast(EVO_TOAST_ERROR);
    else                                       render_list(screen, sel, rail, rail_sel, empty);
}

int main(int argc, char **argv)
{
    const char *screen = NULL;
    const char *out    = NULL;
    const char *theme  = NULL;
    int sel = 0, rail = 0, rail_sel = 1, empty = 0, row = 0, all = 0;

    /* Before any text is drawn. Single-threaded here, so a plain call is all
     * the "once" this program needs - see evo_font.h. */
    evo_font_build_tables();

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--all"))      all = 1;
        else if (!strcmp(argv[i], "--sel"))      sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--row"))      row = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rail"))     rail = 1;
        else if (!strcmp(argv[i], "--rail-sel")) rail_sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--empty"))    empty = 1;
        else if (!strcmp(argv[i], "--theme"))    theme = argv[++i];
        else if (!strcmp(argv[i], "--face"))     g_reader_face = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o"))         out = argv[++i];
        else if (argv[i][0] != '-')              screen = argv[i];
    }

    evo_draw_bind(&VTABLE);
    evo_theme_init();
    if (theme) evo_theme_set_by_name(theme);
    make_art();

    printf("theme: %s\n", evo_theme_name(evo_theme_index()));

    if (all) {
        char path[256];
        for (int i = 0; SCREENS[i]; i++) {
            render_one(SCREENS[i], sel, rail, rail_sel, empty, row);
            snprintf(path, sizeof(path), "%s/%s.bmp",
                     out ? out : ".", SCREENS[i]);
            if (write_bmp(path, g_fb) != 0) {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            printf("  %s\n", path);
        }
        return 0;
    }

    if (!screen) {
        fprintf(stderr, "usage: uiview <screen|--all> [--sel N] [--row N] "
                        "[--rail] [--rail-sel N] [--empty] [--theme NAME] "
                        "-o OUT\nscreens:");
        for (int i = 0; SCREENS[i]; i++) fprintf(stderr, " %s", SCREENS[i]);
        fprintf(stderr, "\n");
        return 2;
    }

    render_one(screen, sel, rail, rail_sel, empty, row);

    {
        /* -o takes either a file or a directory, because the wrapper always
         * passes the output directory and --all needs one name per screen. */
        char path[512];
        const char *o = out ? out : ".";
        size_t n = strlen(o);

        if (n > 4 && !strcmp(o + n - 4, ".bmp"))
            snprintf(path, sizeof(path), "%s", o);
        else
            snprintf(path, sizeof(path), "%s/%s.bmp", o, screen);

        if (write_bmp(path, g_fb) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }

        printf("  %s\n", path);
    }

    return 0;
}
