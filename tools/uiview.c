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

static int rr_idx(char ch)
{
    for (int i = 0; i < RR_COUNT; i++)
        if (RR_CHARS[i] == ch) return i;
    return -1;
}

static void rr_text(uint32_t *fb, int x, int y, const char *t,
                    uint32_t colour, int face)
{
    int cx = x;
    unsigned ca = (colour >> 24) & 255;

    for (const char *q = t; *q; q++) {
        int id = rr_idx(*q);
        int sx, sy, sw, sh, adv;

        if (id < 0) { cx += 12; continue; }

        if (face == 3) { sx=RR_TITLE_X[id]; sy=RR_TITLE_Y[id]; sw=RR_TITLE_W[id]; sh=RR_TITLE_H[id]; adv=RR_TITLE_ADV[id]; }
        else if (face == 2) { sx=RR_MENU_X[id]; sy=RR_MENU_Y[id]; sw=RR_MENU_W[id]; sh=RR_MENU_H[id]; adv=RR_MENU_ADV[id]; }
        else if (face == 1) { sx=RR_SUB_X[id]; sy=RR_SUB_Y[id]; sw=RR_SUB_W[id]; sh=RR_SUB_H[id]; adv=RR_SUB_ADV[id]; }
        else { sx=RR_SMALL_X[id]; sy=RR_SMALL_Y[id]; sw=RR_SMALL_W[id]; sh=RR_SMALL_H[id]; adv=RR_SMALL_ADV[id]; }

        for (int yy = 0; yy < sh; yy++) {
            int fy = y + yy;
            if ((unsigned)fy >= H) continue;
            for (int xx = 0; xx < sw; xx++) {
                int fx = cx + xx;
                unsigned ma, a;
                if ((unsigned)fx >= W) continue;
                ma = RR_FONT[(sy + yy) * RR_FONT_W + (sx + xx)];
                if (ma <= 18) continue;
                a = (ma * ca) / 255;
                fb[fy * W + fx] = blend(fb[fy * W + fx],
                                        (a << 24) | (colour & 0x00FFFFFF));
            }
        }
        cx += adv;
    }
}

static int rr_text_w(const char *t, int face)
{
    int w = 0;
    if (!t) return 0;
    for (const char *q = t; *q; q++) {
        int id = rr_idx(*q);
        if (id < 0) { w += 12; continue; }
        if (face == 3)      w += RR_TITLE_ADV[id];
        else if (face == 2) w += RR_MENU_ADV[id];
        else if (face == 1) w += RR_SUB_ADV[id];
        else                w += RR_SMALL_ADV[id];
    }
    return w;
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

static void rr_icon_tinted(uint32_t *fb, int x, int y, int idx, uint32_t tint)
{
    switch (idx) {
    case 0:  img_tint(fb,x,y,EVO_ICON_BROWSE_USB_W,EVO_ICON_BROWSE_USB_H,EVO_ICON_BROWSE_USB,tint); break;
    case 1:  img_tint(fb,x,y,EVO_ICON_RECENT_FILES_W,EVO_ICON_RECENT_FILES_H,EVO_ICON_RECENT_FILES,tint); break;
    case 2:  img_tint(fb,x,y,EVO_ICON_FAVORITES_W,EVO_ICON_FAVORITES_H,EVO_ICON_FAVORITES,tint); break;
    case 3:  img_tint(fb,x,y,EVO_ICON_SETTINGS_W,EVO_ICON_SETTINGS_H,EVO_ICON_SETTINGS,tint); break;
    case 4:  img_tint(fb,x,y,EVO_ICON_DEVELOPER_TOOLS_W,EVO_ICON_DEVELOPER_TOOLS_H,EVO_ICON_DEVELOPER_TOOLS,tint); break;
    case 5:  img_tint(fb,x,y,EVO_ICON_ABOUT_SUPPORT_W,EVO_ICON_ABOUT_SUPPORT_H,EVO_ICON_ABOUT_SUPPORT,tint); break;
    case 6:  img_tint(fb,x,y,EVO_ICON_CHEVRON_W,EVO_ICON_CHEVRON_H,EVO_ICON_CHEVRON,tint); break;
    case 7:  img_tint(fb,x,y,EVO_ICON_RESUME_W,EVO_ICON_RESUME_H,EVO_ICON_RESUME,tint); break;
    case 8:  img_tint(fb,x,y,EVO_ICON_ASPECT_W,EVO_ICON_ASPECT_H,EVO_ICON_ASPECT,tint); break;
    case 9:  img_tint(fb,x,y,EVO_ICON_SUBTITLES_W,EVO_ICON_SUBTITLES_H,EVO_ICON_SUBTITLES,tint); break;
    case 10: img_tint(fb,x,y,EVO_ICON_PALETTE_W,EVO_ICON_PALETTE_H,EVO_ICON_PALETTE,tint); break;
    case 11: img_tint(fb,x,y,EVO_ICON_FOLDER_W,EVO_ICON_FOLDER_H,EVO_ICON_FOLDER,tint); break;
    case 12: img_tint(fb,x,y,EVO_ICON_TRASH_W,EVO_ICON_TRASH_H,EVO_ICON_TRASH,tint); break;
    case 13: img_tint(fb,x,y,EVO_ICON_HOME_W,EVO_ICON_HOME_H,EVO_ICON_HOME,tint); break;
    default: break;
    }
}

static void rr_icon(uint32_t *fb, int x, int y, int idx)
{
    rr_icon_tinted(fb, x, y, idx, evo_theme_current()->accent);
}

static void rr_control_tinted(uint32_t *fb, int x, int y, int idx, uint32_t t)
{
    switch (idx) {
    case 0: img_tint(fb,x,y,EVO_CTRL_X_W,EVO_CTRL_X_H,EVO_CTRL_X,t); break;
    case 1: img_tint(fb,x,y,EVO_CTRL_DPAD_W,EVO_CTRL_DPAD_H,EVO_CTRL_DPAD,t); break;
    case 2: img_tint(fb,x,y,EVO_CTRL_LEFT_STICK_W,EVO_CTRL_LEFT_STICK_H,EVO_CTRL_LEFT_STICK,t); break;
    case 3: img_tint(fb,x,y,EVO_CTRL_RIGHT_STICK_W,EVO_CTRL_RIGHT_STICK_H,EVO_CTRL_RIGHT_STICK,t); break;
    case 4: img_tint(fb,x,y,EVO_CTRL_CIRCLE_W,EVO_CTRL_CIRCLE_H,EVO_CTRL_CIRCLE,t); break;
    case 5: img_tint(fb,x,y,EVO_CTRL_TRIANGLE_W,EVO_CTRL_TRIANGLE_H,EVO_CTRL_TRIANGLE,t); break;
    case 6: img_tint(fb,x,y,EVO_CTRL_SQUARE_W,EVO_CTRL_SQUARE_H,EVO_CTRL_SQUARE,t); break;
    default: break;
    }
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

#define ART_W 128
#define ART_H 72
static uint32_t g_art[ART_W * ART_H];

static void make_art(void)
{
    for (int y = 0; y < ART_H; y++)
        for (int x = 0; x < ART_W; x++) {
            int r = 40 + (x * 160) / ART_W;
            int g = 30 + (y * 120) / ART_H;
            int b = 90 + ((x + y) * 90) / (ART_W + ART_H);
            g_art[y * ART_W + x] = 0xFF000000u | ((uint32_t)b << 16) |
                                   ((uint32_t)g << 8) | (uint32_t)r;
        }
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
        if (i == 0) { items[i].art.pixels = g_art;
                      items[i].art.w = ART_W; items[i].art.h = ART_H; }
    }

    memset(&m, 0, sizeof(m));
    m.has_resume    = 1;
    m.hero_title    = "leftbehind-bts";
    m.hero_detail   = "7M 58S LEFT";
    m.hero_action   = "RESUME";
    m.hero_progress = 240;
    m.hero_art.pixels = g_art;
    m.hero_art.w = ART_W;
    m.hero_art.h = ART_H;
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
        static const char *t[] = { "PLAYBACK PROFILE","RESUME PLAYBACK",
            "DEFAULT ASPECT RATIO","AUTO-DETECT SUBTITLES","DEVELOPER MODE",
            "FOLDERS FIRST","THEME","REMOVE HOME TILE" };
        static char theme_val[64];
        static const char *d[8];
        static const int ic[] = { EVO_IC_SETTINGS,EVO_IC_RESUME,EVO_IC_ASPECT,
            EVO_IC_SUBTITLES,EVO_IC_TOOLS,EVO_IC_FOLDER,EVO_IC_PALETTE,EVO_IC_TRASH };
        static uint32_t sw[3];
        const evo_theme *cur = evo_theme_current();

        /* Report the theme actually being rendered. A hardcoded "CARBON"
         * here rendered under MIDNIGHT, which makes the fixture lie about
         * the one row whose job is to name the theme. */
        snprintf(theme_val, sizeof(theme_val), "%s  -  %d OF %d",
                 evo_theme_name(evo_theme_index()),
                 evo_theme_index() + 1, evo_theme_count());

        d[0]="Performance"; d[1]="ON"; d[2]="FIT"; d[3]="ON";
        d[4]="OFF"; d[5]="ON"; d[6]=theme_val; d[7]="REMOVES MEDIA TILE";

        n = 8;
        for (int i = 0; i < n; i++) {
            e[i].title=t[i]; e[i].detail=d[i]; e[i].icon=ic[i];
            e[i].chevron=1; e[i].progress=-1;
        }

        /* Exercise the THEME row's swatches, as the real settings screen
         * does - a fixture that skips them cannot show they render. */
        sw[0] = cur->accent; sw[1] = cur->surface; sw[2] = cur->bg_top;
        e[6].swatches = sw; e[6].swatch_count = 3;

        m.title="SETTINGS"; m.subtitle="PLAYBACK AND APPLICATION PREFERENCES";
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
        m.title="TOOLS"; m.subtitle="DIAGNOSTICS AND CONTROLLER FEEDBACK";
        m.section=EVO_SECTION_TOOLS;
    } else if (!strcmp(which, "recent")) {
        static const char *t[] = { "leftbehind-bts","A Very Long Documentary "
            "Title That Must Ellipsise","DTS 5.1","Dolby TrueHD 7.1","AAC 5.1" };
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
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int sx = (x * ART_W) / W;
            int sy = (y * ART_H) / H;
            g_fb[y * W + x] = g_art[sy * ART_W + sx];
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


/*
 * Subtitle picker.
 *
 * Modelled on the file that prompted it: thirty-odd tracks, two of them
 * English, and the one flagged "default" holding two cues. The fixture keeps
 * that shape because it is the case the screen exists to make legible - a
 * fixture of three tidy tracks would demonstrate nothing.
 */
static void render_picker(int sel)
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

    memset(&m, 0, sizeof(m));
    m.eyebrow     = "SUBTITLES";
    m.title       = "SELECT A TRACK";
    m.entries     = rows;
    m.first       = first;
    m.entry_count = shown;
    m.count       = count;

    evo_screen_picker(g_fb, &m, &f);
}

static const char *SCREENS[] = { "launch","browse","recent","favorites",
                                 "settings","profile","tools","about",
                                 "changelog",
                                 "resume","finished","mediainfo",
                                 "picker",
                                 "toast","toastok","toasterror", NULL };

static void render_one(const char *screen, int sel, int rail, int rail_sel,
                       int empty, int row)
{
    memset(g_fb, 0, sizeof(g_fb));

    if      (!strcmp(screen, "launch"))    render_launch(row, sel);
    else if (!strcmp(screen, "browse"))    render_browse(sel, rail, rail_sel, empty);
    else if (!strcmp(screen, "resume"))    render_resume();
    else if (!strcmp(screen, "finished"))  render_finished();
    else if (!strcmp(screen, "mediainfo")) render_mediainfo();
    else if (!strcmp(screen, "picker"))    render_picker(sel);
    else if (!strcmp(screen, "toast"))      render_toast(EVO_TOAST_INFO);
    else if (!strcmp(screen, "toastok"))    render_toast(EVO_TOAST_OK);
    else if (!strcmp(screen, "toasterror")) render_toast(EVO_TOAST_ERROR);
    else                                   render_list(screen, sel, rail, rail_sel, empty);
}

int main(int argc, char **argv)
{
    const char *screen = NULL;
    const char *out    = NULL;
    const char *theme  = NULL;
    int sel = 0, rail = 0, rail_sel = 1, empty = 0, row = 0, all = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--all"))      all = 1;
        else if (!strcmp(argv[i], "--sel"))      sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--row"))      row = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rail"))     rail = 1;
        else if (!strcmp(argv[i], "--rail-sel")) rail_sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--empty"))    empty = 1;
        else if (!strcmp(argv[i], "--theme"))    theme = argv[++i];
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
