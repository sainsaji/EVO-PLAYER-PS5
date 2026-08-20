#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include "../projects/evoplayer/ui_rml/include/evo_rmlui_bridge.h"

static void save_bmp_24(const char* filename, const uint32_t* fb, int width, int height) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return;

    int row_padded = (width * 3 + 3) & (~3);
    int image_size = row_padded * height;
    int file_size = 54 + image_size;

    uint8_t header[54] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        (uint8_t)(width), (uint8_t)(width >> 8), (uint8_t)(width >> 16), (uint8_t)(width >> 24),
        (uint8_t)(height), (uint8_t)(height >> 8), (uint8_t)(height >> 16), (uint8_t)(height >> 24),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0x13, 0x0B, 0, 0,
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    file.write((const char*)header, 54);

    std::vector<uint8_t> row_buf(row_padded, 0);
    for (int y = height - 1; y >= 0; y--) {
        const uint32_t* src_row = &fb[y * width];
        for (int x = 0; x < width; x++) {
            uint32_t pixel = src_row[x];
            uint8_t r = pixel & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = (pixel >> 16) & 0xFF;
            row_buf[x * 3 + 0] = b;
            row_buf[x * 3 + 1] = g;
            row_buf[x * 3 + 2] = r;
        }
        file.write((const char*)row_buf.data(), row_padded);
    }
}

/* ------------------------------------------------------------------
 * Launch / home screen
 *
 * The posters the console shows are decoded video frames, so there is no
 * file to point at here. These stand-ins are generated at the same
 * dimensions the player caches covers and the hero still at, and go through
 * the same raw-BGRA path, so what this renders is the real code path and
 * not an <img src> that only works on the host.
 * ------------------------------------------------------------------ */

static uint32_t evo_demo_bgra(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static std::vector<uint32_t> make_demo_art(int w, int h, int seed) {
    std::vector<uint32_t> px((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float fx = (float)x / (float)w;
            float fy = (float)y / (float)h;
            /* A soft diagonal wash plus a vignette — enough structure to see
             * the cover-crop and the scrims doing their job. */
            float d = 0.55f * fx + 0.45f * (1.0f - fy);
            float vig = 1.0f - 0.45f * ((fx - 0.5f) * (fx - 0.5f) +
                                        (fy - 0.5f) * (fy - 0.5f)) * 4.0f;
            if (vig < 0.0f) vig = 0.0f;

            int base_r = (seed * 53) % 90 + 20;
            int base_g = (seed * 97) % 70 + 25;
            int base_b = (seed * 31) % 110 + 60;

            int r = (int)((base_r + d * 150.0f) * vig);
            int g = (int)((base_g + d * 120.0f) * vig);
            int b = (int)((base_b + d * 170.0f) * vig);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            px[(size_t)y * w + x] = evo_demo_bgra(r, g, b);
        }
    }
    return px;
}

struct DemoRecent {
    const char* title;
    const char* detail;
    int progress;
};

static void render_launch_screens(std::vector<uint32_t>& fb, int width, int height) {
    /* Cover cache is 320x180; the hero still is 560 wide. */
    std::vector<uint32_t> hero_art = make_demo_art(560, 315, 3);
    std::vector<std::vector<uint32_t>> covers;
    for (int i = 0; i < 6; i++) covers.push_back(make_demo_art(320, 180, 5 + i * 7));

    static const DemoRecent recents[6] = {
        { "Blade Runner 2049",            "1H 42M LEFT",      412 },
        { "Dune Part Two",                "58M LEFT",         735 },
        { "The Grand Budapest Hotel",     "MKV - 4.2 GB",      -1 },
        { "Arrival",                      "22M LEFT",         880 },
        { "Interstellar 2160p HDR10",     "2H 04M LEFT",      160 },
        { "Dolby Atmos Demo Disc 2024",   "MKV - 12.8 GB",     -1 },
    };

    static const char* lib_titles[6] = {
        "BROWSE", "RECENT", "FAVORITES", "EMBY", "SETTINGS", "ABOUT"
    };
    static const char* lib_details[6] = {
        "Videos and folders on USB storage",
        "Pick up where you left off",
        "Media you saved for later",
        "Emby and media server streaming",
        "Playback profiles and preferences",
        "Credits and project info"
    };
    static const char* lib_icons[6] = {
        "projects/evoplayer/assets/icons/icon_browse_usb.png",
        "projects/evoplayer/assets/icons/icon_recent_files.png",
        "projects/evoplayer/assets/icons/icon_favorites.png",
        "projects/evoplayer/assets/icons/icon_emby.png",
        "projects/evoplayer/assets/icons/icon_settings.png",
        "projects/evoplayer/assets/icons/icon_about_support.png"
    };

    /*
     * row: 0 = hero, 1 = the recent shelf, 2 = the library shelf.
     * col: which tile in that shelf carries the cursor.
     */
    auto build = [&](evo_rmlui_launch_params_t& p, int row, int col,
                     bool with_recent) {
        memset(&p, 0, sizeof(p));
        p.app_name = "EVO PLAYER";
        p.version = "VERSION 0.7.0";
        p.clock = "21:48";
        p.theme_name = "MIDNIGHT";

        if (with_recent) {
            p.hero_eyebrow = "CONTINUE WATCHING";
            p.hero_title = "Blade Runner 2049";
            p.hero_detail = "1H 42M LEFT  -  HEVC MAIN 10  -  HDR10  -  DTS-HD MA 5.1";
            p.hero_action = "RESUME";
            p.hero_progress = 412;
            p.hero_art = hero_art.data();
            p.hero_art_w = 560;
            p.hero_art_h = 315;
        } else {
            p.hero_eyebrow = "WELCOME";
            p.hero_title = "EVO PLAYER";
            p.hero_detail = "Play video and audio from USB storage";
            p.hero_action = "BROWSE USB";
            p.hero_progress = -1;
        }
        p.hero_focused = (row == 0) ? 1 : 0;

        if (with_recent) {
            p.recent_total = 9;
            p.recent_visible = 6;
            p.recent_cursor = (row == 1) ? col : -1;
            for (int i = 0; i < 6; i++) {
                p.recent[i].title = recents[i].title;
                p.recent[i].detail = recents[i].detail;
                p.recent[i].progress = recents[i].progress;
                p.recent[i].icon_path =
                    "projects/evoplayer/assets/icons/icon_recent_files.png";
                p.recent[i].art = covers[i].data();
                p.recent[i].art_w = 320;
                p.recent[i].art_h = 180;
                p.recent[i].is_focused = (row == 1 && i == col) ? 1 : 0;
            }
        }

        p.library_visible = 6;
        for (int i = 0; i < 6; i++) {
            p.library[i].title = lib_titles[i];
            p.library[i].detail = lib_details[i];
            p.library[i].icon_path = lib_icons[i];
            p.library[i].progress = -1;
            p.library[i].is_focused = (row == 2 && i == col) ? 1 : 0;
        }
    };

    evo_rmlui_nav_params_t nav;
    memset(&nav, 0, sizeof(nav));
    nav.active_section = 0;   /* HOME */
    nav.cursor_index = 0;
    nav.visible = 1;

    struct Shot { const char* name; int row; int col; bool recent; int rail; };
    static const Shot shots[] = {
        { "rml_launch_hero",     0, 0, true,  0 },
        { "rml_launch_recent",   1, 2, true,  0 },
        { "rml_launch_library",  2, 4, true,  0 },
        { "rml_launch_empty",    2, 0, false, 0 },
        { "rml_launch_rail",     0, 0, true,  1 },
    };

    for (const Shot& s : shots) {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);

        nav.rail_focused = s.rail;
        nav.cursor_index = s.rail ? 1 : 0;
        evo_rmlui_update_nav(&nav);

        evo_rmlui_launch_params_t p;
        build(p, s.row, s.col, s.recent);
        evo_rmlui_update_launch(&p);
        evo_rmlui_render_launch(fb.data(), width, height);

        std::string out = std::string("output/uiview/") + s.name + ".bmp";
        save_bmp_24(out.c_str(), fb.data(), width, height);
    }

    /* Hand the rail back the way the settings shots below expect to find it:
     * SETTINGS lit, collapsed. Nav state is sticky across renders, so leaving
     * it expanded here drew the overlay across every screen that follows. */
    nav.active_section = 5;
    nav.cursor_index = 5;
    nav.rail_focused = 0;
    evo_rmlui_update_nav(&nav);
}

/* ------------------------------------------------------------------
 * List, browser and changelog screens
 * ------------------------------------------------------------------ */

static void set_nav(int section, int rail_focused) {
    evo_rmlui_nav_params_t nav;
    memset(&nav, 0, sizeof(nav));
    nav.active_section = section;
    nav.cursor_index = rail_focused ? section : section;
    nav.rail_focused = rail_focused;
    nav.visible = 1;
    evo_rmlui_update_nav(&nav);
}

static void render_list_screens(std::vector<uint32_t>& fb, int width, int height) {
    const char* ico_recent = "projects/evoplayer/assets/icons/icon_recent_files.png";
    const char* ico_fav    = "projects/evoplayer/assets/icons/icon_favorites.png";
    const char* ico_set    = "projects/evoplayer/assets/icons/icon_settings.png";
    const char* ico_folder = "projects/evoplayer/assets/icons/icon_folder.png";
    const char* ico_res    = "projects/evoplayer/assets/icons/icon_resume.png";

    auto hints3 = [](evo_rmlui_list_params_t& p, const char* a, const char* b, const char* c) {
        p.hints[0].glyph_path = "projects/evoplayer/assets/icons/btn_cross.png";
        p.hints[0].label = a;
        p.hints[1].glyph_path = "projects/evoplayer/assets/icons/btn_circle.png";
        p.hints[1].label = b;
        p.hints[2].glyph_path = "projects/evoplayer/assets/icons/btn_dpad.png";
        p.hints[2].label = c;
        p.hint_count = 3;
    };

    /* --- RECENT --- */
    {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);
        set_nav(2, 0);
        evo_rmlui_list_params_t p;
        memset(&p, 0, sizeof(p));
        p.title = "RECENT";
        p.subtitle = "PICK UP WHERE YOU LEFT OFF";
        p.section = 2;
        p.total_count = 12;
        p.cursor_index = 1;
        hints3(p, "PLAY", "BACK", "MOVE");

        static const char* t[9] = {
            "Blade Runner 2049", "Dune Part Two", "The Grand Budapest Hotel",
            "Arrival", "Interstellar 2160p HDR10", "Dolby Atmos Demo Disc 2024",
            "Sicario", "Whiplash", "The Social Network"
        };
        static const char* d[9] = {
            "1H 42M LEFT", "58M LEFT", "MKV - 4.2 GB", "22M LEFT",
            "2H 04M LEFT", "MKV - 12.8 GB", "41M LEFT", "13M LEFT", "1H 06M LEFT"
        };
        static const int pr[9] = { 412, 735, -1, 880, 160, -1, 560, 905, 330 };

        for (int i = 0; i < 7; i++) {
            p.rows[i].title = t[i];
            p.rows[i].detail = d[i];
            p.rows[i].icon_path = ico_recent;
            p.rows[i].progress = pr[i];
            p.rows[i].is_focused = (i == 1);
            p.row_count++;
        }
        evo_rmlui_update_list(&p);
        evo_rmlui_render_list(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_recent.bmp", fb.data(), width, height);
    }

    /* --- FAVORITES, empty --- */
    {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);
        set_nav(3, 0);
        evo_rmlui_list_params_t p;
        memset(&p, 0, sizeof(p));
        p.title = "FAVORITES";
        p.subtitle = "MEDIA YOU SAVED FOR LATER";
        p.section = 3;
        p.cursor_index = -1;
        p.is_empty = 1;
        p.empty_title = "NO FAVORITES YET";
        p.empty_hint = "PRESS TRIANGLE ON A FILE IN THE BROWSER";
        p.empty_icon = ico_fav;
        hints3(p, "PLAY", "BACK", "REMOVE");
        evo_rmlui_update_list(&p);
        evo_rmlui_render_list(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_favorites_empty.bmp", fb.data(), width, height);
    }

    /* --- FAVORITES, populated --- */
    {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);
        set_nav(3, 0);
        evo_rmlui_list_params_t p;
        memset(&p, 0, sizeof(p));
        p.title = "FAVORITES";
        p.subtitle = "MEDIA YOU SAVED FOR LATER";
        p.section = 3;
        p.total_count = 4;
        p.cursor_index = 0;
        hints3(p, "PLAY", "BACK", "REMOVE");
        static const char* t[4] = {
            "Blade Runner 2049", "Dolby Atmos Demo Disc 2024",
            "Planet Earth II - Islands", "Whiplash"
        };
        static const char* d[4] = { "2H 44M", "18M", "50M", "1H 46M" };
        for (int i = 0; i < 4; i++) {
            p.rows[i].title = t[i];
            p.rows[i].detail = d[i];
            p.rows[i].icon_path = ico_fav;
            p.rows[i].progress = -1;
            p.rows[i].is_focused = (i == 0);
            p.row_count++;
        }
        evo_rmlui_update_list(&p);
        evo_rmlui_render_list(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_favorites.bmp", fb.data(), width, height);
    }

    /* --- EMBY SETUP --- */
    {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);
        set_nav(4, 0);
        evo_rmlui_list_params_t p;
        memset(&p, 0, sizeof(p));
        p.title = "EMBY MEDIA SERVER";
        p.subtitle = "STREAMING ADDON CONFIGURATION";
        p.section = 4;
        p.total_count = 6;
        p.cursor_index = 4;
        hints3(p, "SELECT", "BACK", "MOVE");
        static const char* t[6] = {
            "SERVER HOST", "SERVER PORT", "ACCOUNT USERNAME",
            "ACCOUNT PASSWORD", "CONNECTION STATUS", "EXPLORE MEDIA LIBRARIES"
        };
        static const char* d[6] = {
            "192.168.0.24  (PRESS X TO EDIT)", "8096  (PRESS X TO EDIT)",
            "sain  (PRESS X TO EDIT)", "********  (PRESS X TO EDIT)",
            "CONNECTED (PRESS X TO DISCONNECT)",
            "BROWSE MOVIES, TV SHOWS & COLLECTIONS"
        };
        for (int i = 0; i < 6; i++) {
            p.rows[i].title = t[i];
            p.rows[i].detail = d[i];
            p.rows[i].icon_path = (i == 5) ? ico_folder : ico_set;
            p.rows[i].progress = -1;
            p.rows[i].has_chevron = 1;
            p.rows[i].is_focused = (i == 4);
            p.row_count++;
        }
        p.rows[4].badge = "ONLINE";
        evo_rmlui_update_list(&p);
        evo_rmlui_render_list(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_emby_setup.bmp", fb.data(), width, height);
    }

    /* --- EMBY BROWSE --- */
    {
        std::fill(fb.begin(), fb.end(), 0xFF0E0906);
        set_nav(4, 0);
        evo_rmlui_list_params_t p;
        memset(&p, 0, sizeof(p));
        p.title = "EMBY BROWSER";
        p.subtitle = "EMBY  -  ROOT LIBRARIES";
        p.section = 4;
        p.total_count = 7;
        p.cursor_index = 2;
        hints3(p, "OPEN", "BACK", "MOVE");
        static const char* t[7] = {
            "Movies", "TV Shows", "Documentaries", "Music Videos",
            "4K HDR Collection", "Kids", "Home Videos"
        };
        static const char* d[7] = {
            "482 ITEMS", "96 SERIES", "134 ITEMS", "58 ITEMS",
            "71 ITEMS", "203 ITEMS", "19 ITEMS"
        };
        for (int i = 0; i < 7; i++) {
            p.rows[i].title = t[i];
            p.rows[i].detail = d[i];
            p.rows[i].icon_path = ico_folder;
            p.rows[i].progress = -1;
            p.rows[i].has_chevron = 1;
            p.rows[i].is_focused = (i == 2);
            p.row_count++;
        }
        evo_rmlui_update_list(&p);
        evo_rmlui_render_list(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_emby_browse.bmp", fb.data(), width, height);
    }

    (void)ico_res;
}

static void render_browser_screen(std::vector<uint32_t>& fb, int width, int height) {
    std::vector<uint32_t> preview = make_demo_art(560, 315, 11);

    std::fill(fb.begin(), fb.end(), 0xFF0E0906);
    set_nav(1, 0);

    evo_rmlui_browser_params_t p;
    memset(&p, 0, sizeof(p));
    p.path = "/usb0/Movies";
    p.at_root = 0;
    p.total_count = 37;
    p.cursor_index = 3;

    struct Row { const char* n; const char* d; const char* icon; const char* badge; int prog; int fav; };
    static const Row rows[12] = {
        { "..",                          "PARENT FOLDER",        "projects/evoplayer/assets/icons/icon_folder.png",   "DIR",   -1, 0 },
        { "4K HDR",                      "12 ITEMS",             "projects/evoplayer/assets/icons/icon_folder.png",   "DIR",   -1, 0 },
        { "Documentaries",               "31 ITEMS",             "projects/evoplayer/assets/icons/icon_folder.png",   "DIR",   -1, 0 },
        { "Blade Runner 2049.mkv",       "MKV - 24.8 GB - 2H 44M", "projects/evoplayer/assets/icons/icon_resume.png", NULL,   412, 1 },
        { "Dune Part Two.mkv",           "MKV - 31.2 GB - 2H 46M", "projects/evoplayer/assets/icons/icon_resume.png", NULL,   735, 0 },
        { "Arrival.mp4",                 "MP4 - 8.1 GB - 1H 56M",  "projects/evoplayer/assets/icons/icon_resume.png", NULL,   880, 1 },
        { "Interstellar 2160p HDR10.mkv","MKV - 46.0 GB - 2H 49M", "projects/evoplayer/assets/icons/icon_resume.png", NULL,   160, 0 },
        { "Sicario.mkv",                 "MKV - 18.4 GB - 2H 01M", "projects/evoplayer/assets/icons/icon_resume.png", NULL,    -1, 0 },
        { "Whiplash.mkv",                "MKV - 11.7 GB - 1H 46M", "projects/evoplayer/assets/icons/icon_resume.png", NULL,   905, 0 },
        { "Atmos Demo.m4a",              "M4A - 92 MB - 4M 18S",   "projects/evoplayer/assets/icons/icon_subtitles.png", "AUDIO", -1, 0 },
        { "Soundtrack.flac",             "FLAC - 412 MB - 58M",    "projects/evoplayer/assets/icons/icon_subtitles.png", "AUDIO", -1, 0 },
        { "notes.txt",                   "TXT - 2 KB",             "projects/evoplayer/assets/icons/icon_about_support.png", NULL, -1, 0 },
    };

    for (int i = 0; i < 12; i++) {
        p.rows[i].name = rows[i].n;
        p.rows[i].detail = rows[i].d;
        p.rows[i].icon_path = rows[i].icon;
        p.rows[i].badge = rows[i].badge;
        p.rows[i].progress = rows[i].prog;
        p.rows[i].is_favorite = rows[i].fav;
        p.rows[i].is_focused = (i == 3);
        p.row_count++;
    }

    p.ins_name = "Blade Runner 2049.mkv";
    p.ins_kind = "VIDEO";
    p.ins_ext = "MKV";
    p.ins_preview_badge = "2H 44M";
    p.ins_preview = preview.data();
    p.ins_preview_w = 560;
    p.ins_preview_h = 315;

    struct KV { const char* k; const char* v; };
    static const KV props[7] = {
        { "SIZE",       "24.8 GB" },
        { "CONTAINER",  "MATROSKA" },
        { "DURATION",   "2H 44M" },
        { "RESOLUTION", "3840 x 2160" },
        { "VIDEO",      "HEVC MAIN 10" },
        { "AUDIO",      "DTS-HD MA 5.1" },
        { "SUBTITLES",  "3" },
    };
    for (int i = 0; i < 7; i++) {
        p.ins_props[i].key = props[i].k;
        p.ins_props[i].value = props[i].v;
        p.ins_prop_count++;
    }

    evo_rmlui_update_browser(&p);
    evo_rmlui_render_browser(fb.data(), width, height);
    save_bmp_24("output/uiview/rml_browser.bmp", fb.data(), width, height);

    /* Empty folder, at the root — no BACK hint, no inspector content. */
    std::fill(fb.begin(), fb.end(), 0xFF0E0906);
    evo_rmlui_browser_params_t e;
    memset(&e, 0, sizeof(e));
    e.path = "/usb0";
    e.at_root = 1;
    e.cursor_index = -1;
    e.is_empty = 1;
    e.empty_title = "NOTHING HERE";
    e.empty_hint = "CONNECT A USB DRIVE WITH MEDIA ON IT";
    e.ins_name = "";
    evo_rmlui_update_browser(&e);
    evo_rmlui_render_browser(fb.data(), width, height);
    save_bmp_24("output/uiview/rml_browser_empty.bmp", fb.data(), width, height);
}

/* ------------------------------------------------------------------
 * Playback OSD, paused
 *
 * Nav is left visible from render_browser_screen() just above, matching
 * how the real app reaches this screen (browse -> open a file -> pause).
 * RenderPlaybackOSD must hide the nav rail itself instead of inheriting
 * whatever the previous screen left it as - it forgot to once, and the
 * rail bled through onto the player only while paused, since the OSD draw
 * call is skipped entirely during steady playback.
 * ------------------------------------------------------------------ */
static void render_playback_screen(std::vector<uint32_t>& fb, int width, int height) {
    std::fill(fb.begin(), fb.end(), 0xFF06090E);

    evo_playback_osd_params_t p;
    memset(&p, 0, sizeof(p));
    p.title = "Blade Runner 2049";
    p.metadata = "1H 42M LEFT";
    p.res_badge = "4K UHD";
    p.hdr_badge = "HDR10";
    p.codec_badge = "HEVC 10-BIT";
    p.fps_badge = "24 FPS";
    p.audio_badge = "";
    p.position_sec = 1234.0;
    p.duration_sec = 9780.0;
    p.percentage = p.position_sec / p.duration_sec;
    p.paused = 1;
    p.scrub_active = 0;
    p.scrub_target = 0.0;
    p.audio_track = "DTS-HD MA 5.1";
    p.sub_track = "English";
    p.view_mode = 0;
    p.show_stats = 0;
    p.alpha = 255;

    evo_rmlui_update_playback_params(&p);
    evo_rmlui_render_playback_osd(fb.data(), width, height);
    save_bmp_24("output/uiview/rml_playback_paused.bmp", fb.data(), width, height);
}

static void render_changelog_screen(std::vector<uint32_t>& fb, int width, int height) {
    std::fill(fb.begin(), fb.end(), 0xFF0E0906);
    set_nav(6, 0);

    evo_rmlui_changelog_params_t p;
    memset(&p, 0, sizeof(p));
    p.title = "CHANGELOG";
    p.subtitle = "WHAT CHANGED IN EACH RELEASE";
    p.release_total = 5;
    p.cursor_index = 0;

    struct Rel { const char* v; const char* t; const char* d; };
    static const Rel rel[5] = {
        { "0.7.1", "RMLUI LAUNCH, BROWSER & LIST SCREENS", "AUGUST 2026" },
        { "0.7.0", "EMBY PASSWORD AUTH & REFINED UI",      "AUGUST 2026" },
        { "0.6.0", "NATIVE RMLUI PLAYBACK OSD",            "AUGUST 2026" },
        { "0.5.0", "MEDIA HOME TILE & THEMING",            "JULY 2026"   },
        { "0.4.0", "HARDWARE DECODE PIPELINE",             "JULY 2026"   },
    };
    for (int i = 0; i < 5; i++) {
        p.releases[i].version = rel[i].v;
        p.releases[i].tagline = rel[i].t;
        p.releases[i].date = rel[i].d;
        p.releases[i].is_focused = (i == 0);
        p.release_count++;
    }

    p.detail_version = "0.7.1";
    p.detail_tagline = "RMLUI LAUNCH, BROWSER & LIST SCREENS";
    p.item_total = 9;

    struct It { const char* k; const char* t; };
    static const It items[9] = {
        { "NEW",      "RETAINED-MODE LAUNCH SCREEN WITH HERO, RECENT SHELF AND LIBRARY TILES" },
        { "NEW",      "USB BROWSER REBUILT AS A VIRTUALISED TWELVE-ROW LIST WITH LIVE INSPECTOR" },
        { "NEW",      "SHARED LIST DOCUMENT SERVING RECENT, FAVORITES AND BOTH EMBY SCREENS" },
        { "NEW",      "MASTER-DETAIL CHANGELOG VIEWER" },
        { "FIXED",    "BROWSER OPENED IN THE LAST PLAYED FOLDER INSTEAD OF THE USB ROOT" },
        { "FIXED",    "NAVIGATION RAIL WAS UNREACHABLE FROM THE HOME SCREEN" },
        { "FIXED",    "THEME COLOURS WERE BYTE-SWAPPED BEFORE THE FIRST THEME SYNC" },
        { "FIXED",    "GRADIENT DECORATORS FLATTENED TO THEIR START COLOUR" },
        { "IMPROVED", "RUNTIME ARTWORK NOW REACHES THE DOM WITHOUT A FILE ON DISK" },
    };
    for (int i = 0; i < 9; i++) {
        p.items[i].kind = items[i].k;
        p.items[i].text = items[i].t;
        p.item_count++;
    }

    evo_rmlui_update_changelog(&p);
    evo_rmlui_render_changelog(fb.data(), width, height);
    save_bmp_24("output/uiview/rml_changelog.bmp", fb.data(), width, height);
}

int main(int argc, char** argv) {
    const int width = 1920;
    const int height = 1080;
    std::vector<uint32_t> fb(width * height, 0xFF06090E);

    if (!evo_rmlui_init(width, height)) {
        std::cerr << "Failed to initialize RmlUi playback engine!" << std::endl;
        return 1;
    }

    render_launch_screens(fb, width, height);
    render_list_screens(fb, width, height);
    render_browser_screen(fb, width, height);
    render_playback_screen(fb, width, height);
    render_changelog_screen(fb, width, height);
    set_nav(5, 0);

    // 1. Settings Main Hub
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "SETTINGS";
        set.subtitle = "APPLICATION & PLAYBACK PREFERENCES";
        set.counter = "1 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "PLAYBACK & VIDEO";
        set.rows[0].detail = "PROFILE, ASPECT RATIO & RESUME";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "SUBTITLES";
        set.rows[1].detail = "AUTO-DETECT & DEFAULT SIZING";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "INTERFACE & CONTROLS";
        set.rows[2].detail = "THEMES, SOUNDS, LIGHTBAR & SORTING";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "SYSTEM & DIAGNOSTICS";
        set.rows[3].detail = "DEVELOPER TOOLS & MEDIA TILE";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[3].has_chevron = 1;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_main.bmp", fb.data(), width, height);
    }

    // 2. Playback & Video Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "PLAYBACK & VIDEO";
        set.subtitle = "SETTINGS  -  PROFILES, ASPECT RATIO & RESUME";
        set.counter = "1 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "PLAYBACK PROFILE";
        set.rows[0].detail = "SELECT ENGINE PROFILE";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[0].badge = "Performance";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "DEFAULT ASPECT RATIO";
        set.rows[1].detail = "FIT, FILL OR STRETCH";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
        set.rows[1].badge = "FIT";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "RESUME PLAYBACK";
        set.rows[2].detail = "REMEMBER PLAYBACK POSITION";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[2].badge = "ON";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "SURROUND SOUND TEST";
        set.rows[3].detail = "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 1;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_playback.bmp", fb.data(), width, height);
    }

    // 3. Subtitles Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "SUBTITLES";
        set.subtitle = "SETTINGS  -  AUTO-DETECT & DEFAULT SIZING";
        set.counter = "1 OF 2";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 2;

        set.rows[0].title = "AUTO-DETECT SUBTITLES";
        set.rows[0].detail = "LOAD EXTERNAL .SRT AND EMBEDDED STREAMS";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set.rows[0].badge = "ON";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "DEFAULT SUBTITLE SIZE";
        set.rows[1].detail = "MEDIUM (RECOMMENDED FOR 4K TVS)";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
        set.rows[1].badge = "MEDIUM";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_subtitles.bmp", fb.data(), width, height);
    }

    // 4. Interface & Controls Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "INTERFACE & CONTROLS";
        set.subtitle = "SETTINGS  -  THEMES, SOUNDS, LIGHTBAR & SORTING";
        set.counter = "1 OF 5";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 5;

        set.rows[0].title = "THEME";
        set.rows[0].detail = "MIDNIGHT OBSIDIAN";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[0].badge = "ACTIVE";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "NAVIGATION SOUNDS";
        set.rows[1].detail = "PLAY AUDIO CLICKS ON INPUT";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[1].badge = "ON";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "CONTROLLER LIGHTBAR";
        set.rows[2].detail = "SYNC DUALSENSE ACCENT WITH THEME";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[2].badge = "THEME";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "FOLDER SORTING";
        set.rows[3].detail = "NAME ASCENDING (A-Z)";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_browse_usb.png";
        set.rows[3].badge = "A-Z";
        set.rows[3].has_chevron = 1;
        set.rows[3].is_focused = 0;

        set.rows[4].title = "ON-SCREEN KEYBOARD";
        set.rows[4].detail = "SYSTEM OS KEYBOARD FOR SEARCH";
        set.rows[4].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[4].badge = "ENABLED";
        set.rows[4].has_chevron = 1;
        set.rows[4].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_interface.bmp", fb.data(), width, height);
    }

    // 5. System & Diagnostics Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "SYSTEM & DIAGNOSTICS";
        set.subtitle = "SETTINGS  -  DEVELOPER TOOLS & MEDIA TILE";
        set.counter = "1 OF 3";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 3;

        set.rows[0].title = "DEVELOPER TOOLS";
        set.rows[0].detail = "COMPATIBILITY REPORT, DEBUG HUD & HARDWARE TESTS";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[0].badge = "OPEN";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "RESET APPLICATION PREFERENCES";
        set.rows[1].detail = "RESTORE DEFAULT SETTINGS";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "RESET";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "MEDIA HOME TILE";
        set.rows[2].detail = "REMOVE QUICK LAUNCH TILE FROM PS5 HOME SCREEN";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_home.png";
        set.rows[2].badge = "INSTALLED";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_system.bmp", fb.data(), width, height);
    }

    // 6. Playback Profile Picker
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "PLAYBACK PROFILE";
        set.subtitle = "HOW AGGRESSIVELY THE DECODER IS TUNED";
        set.counter = "2 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "BALANCED";
        set.rows[0].detail = "EVEN TRADE BETWEEN SMOOTHNESS AND FORMAT COVERAGE";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[0].badge = "";
        set.rows[0].has_chevron = 0;
        set.rows[0].is_focused = 0;

        set.rows[1].title = "PERFORMANCE";
        set.rows[1].detail = "FEWEST DROPPED FRAMES - BEST FOR HIGH BITRATE VIDEO";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "ACTIVE";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 1;

        set.rows[2].title = "COMPATIBILITY";
        set.rows[2].detail = "WIDEST FORMAT SUPPORT - TRY THIS WHEN A FILE WILL NOT PLAY";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[2].badge = "";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "DEBUG";
        set.rows[3].detail = "VERBOSE DIAGNOSTICS AND ON-SCREEN COUNTERS";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_profile.bmp", fb.data(), width, height);
    }

    // 7. Developer Tools Screen
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "DEVELOPER TOOLS";
        set.subtitle = "DIAGNOSTICS & SYSTEM REPORTS";
        set.counter = "1 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "COMPATIBILITY REPORT";
        set.rows[0].detail = "WRITES A CODEC REPORT TO USB0";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[0].badge = "RUN";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "DEBUG OVERLAY";
        set.rows[1].detail = "ON-SCREEN REALTIME PERFORMANCE STATS";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "OFF";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "NAVIGATION SOUNDS";
        set.rows[2].detail = "PLAY AUDIO CLICKS ON CONTROLLER INPUT";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set.rows[2].badge = "ON";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "LIGHTBAR ACCENTS";
        set.rows[3].detail = "SYNC DUALSENSE LIGHTBAR WITH THEME";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[3].badge = "THEME";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_devtools.bmp", fb.data(), width, height);
    }

    // 8. About & Support Screen
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "ABOUT & SUPPORT";
        set.subtitle = "CREDITS, ENGINE & PROJECT INFO";
        set.counter = "1 OF 6";
        set.rail_active_idx = 6;
        set.rail_focused = 0;
        set.row_count = 6;

        set.rows[0].title = "VERSION";
        set.rows[0].detail = "v0.7.0 HOMEBREW EDITION";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_about_support.png";
        set.rows[0].badge = "v0.7.0";
        set.rows[0].has_chevron = 0;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "EVO PLAYER PRO";
        set.rows[1].detail = "CINEMATIC MEDIA PLAYER FOR PS5 HOMEBREW";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[1].badge = "";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "BUILT ON";
        set.rows[2].detail = "FFMPEG, RMLUI & PS5 PAYLOAD SDK";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[2].badge = "";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "THEMES";
        set.rows[3].detail = "8 AVAILABLE - DROP .THEME FILES ON USB0";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        set.rows[4].title = "SCREENSHOTS";
        set.rows[4].detail = "PRESS L3 OR R3 IN ANY MENU";
        set.rows[4].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
        set.rows[4].badge = "";
        set.rows[4].has_chevron = 0;
        set.rows[4].is_focused = 0;

        set.rows[5].title = "CHANGELOG";
        set.rows[5].detail = "WHAT CHANGED IN EACH RELEASE";
        set.rows[5].icon_path = "projects/evoplayer/assets/icons/icon_recent_files.png";
        set.rows[5].badge = "VIEW";
        set.rows[5].has_chevron = 1;
        set.rows[5].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_about.bmp", fb.data(), width, height);
    }

    // 9. Color Themes Selection Screen
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "COLOR THEMES";
        set.subtitle = "INTERFACE PALETTES & DUALSENSE LIGHTBAR SYNC";
        set.counter = "1 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "MIDNIGHT";
        set.rows[0].detail = "OBSIDIAN BACKDROP WITH SAPPHIRE BLUE ACCENTS";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[0].badge = "ACTIVE";
        set.rows[0].has_chevron = 0;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "CARBON";
        set.rows[1].detail = "MONOCHROME SLATE WITH PURE WHITE & ORANGE ACCENTS";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[1].badge = "";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "EMBER";
        set.rows[2].detail = "WARM CINEMA AMBER WITH GOLD ACCENTS";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[2].badge = "";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "AURORA";
        set.rows[3].detail = "DEEP EMERALD TEAL WITH MINT AURORA GLOW";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_theme.bmp", fb.data(), width, height);
    }

    // 10. Centralized Theming Showcase (Carbon, Ember, Aurora)
    {
        // Carbon Theme
        evo_rmlui_theme_t carbon_th;
        memset(&carbon_th, 0, sizeof(carbon_th));
        carbon_th.name = "CARBON";
        carbon_th.accent = 0xFFF2F2F5;     // Pure Silver/White
        carbon_th.border_sel = 0xFFF2F2F5;
        carbon_th.surface = 0xEB1E1E24;
        carbon_th.surface_sel = 0xF52E2E38;
        carbon_th.border = 0xAA3A3A48;
        evo_rmlui_set_theme(&carbon_th);

        evo_rmlui_settings_params_t set_c;
        memset(&set_c, 0, sizeof(set_c));
        set_c.title = "SETTINGS (CARBON THEME)";
        set_c.subtitle = "MONOCHROME SLATE PALETTE";
        set_c.counter = "1 OF 4";
        set_c.rail_active_idx = 5;
        set_c.row_count = 4;
        set_c.rows[0].title = "PLAYBACK & VIDEO";
        set_c.rows[0].detail = "PROFILE, ASPECT RATIO & RESUME";
        set_c.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set_c.rows[0].has_chevron = 1;
        set_c.rows[0].is_focused = 1;
        set_c.rows[1].title = "SUBTITLES";
        set_c.rows[1].detail = "AUTO-DETECT & DEFAULT SIZING";
        set_c.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set_c.rows[1].has_chevron = 1;
        set_c.rows[2].title = "INTERFACE & CONTROLS";
        set_c.rows[2].detail = "THEMES, SOUNDS, LIGHTBAR & SORTING";
        set_c.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set_c.rows[2].has_chevron = 1;
        set_c.rows[3].title = "SYSTEM & DIAGNOSTICS";
        set_c.rows[3].detail = "DEVELOPER TOOLS & MEDIA TILE";
        set_c.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set_c.rows[3].has_chevron = 1;

        std::fill(fb.begin(), fb.end(), 0xFF0E0E12);
        evo_rmlui_update_settings(&set_c);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_theme_carbon.bmp", fb.data(), width, height);

        // Ember Theme
        evo_rmlui_theme_t ember_th;
        memset(&ember_th, 0, sizeof(ember_th));
        ember_th.name = "EMBER";
        ember_th.accent = 0xFF28A5FF;     // Amber Gold (#ffa528 in BGR/RGBA) -> R=0xff, G=0xa5, B=0x28
        ember_th.accent = 0x0028A5FF;
        ember_th.accent = (0x28 << 16) | (0xa5 << 8) | 0xff; // R=0xff, G=0xa5, B=0x28 -> 0x0028a5ff
        ember_th.border_sel = ember_th.accent;
        ember_th.surface = 0xEB121824;
        ember_th.surface_sel = 0xF51A263D;
        ember_th.border = 0xAA2D3D55;
        evo_rmlui_set_theme(&ember_th);

        set_c.title = "SETTINGS (EMBER THEME)";
        set_c.subtitle = "WARM CINEMA AMBER PALETTE";
        std::fill(fb.begin(), fb.end(), 0xFF080C14);
        evo_rmlui_update_settings(&set_c);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_theme_ember.bmp", fb.data(), width, height);

        // Aurora Theme
        evo_rmlui_theme_t aurora_th;
        memset(&aurora_th, 0, sizeof(aurora_th));
        aurora_th.name = "AURORA";
        aurora_th.accent = (0xc0 << 16) | (0xf5 << 8) | 0x3d; // R=0x3d, G=0xf5, B=0xc0
        aurora_th.border_sel = aurora_th.accent;
        aurora_th.surface = 0xEB22240E;
        aurora_th.surface_sel = 0xF5373B16;
        aurora_th.border = 0xAA454A20;
        evo_rmlui_set_theme(&aurora_th);

        set_c.title = "SETTINGS (AURORA THEME)";
        set_c.subtitle = "MINT & EMERALD TEAL PALETTE";
        std::fill(fb.begin(), fb.end(), 0xFF0B1410);
        evo_rmlui_update_settings(&set_c);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_theme_aurora.bmp", fb.data(), width, height);

        // Reset to Midnight Theme
        evo_rmlui_theme_t midnight_th;
        memset(&midnight_th, 0, sizeof(midnight_th));
        midnight_th.name = "MIDNIGHT";
        midnight_th.accent = (0xf8 << 16) | (0xbd << 8) | 0x38; // Electric blue #38bdf8
        midnight_th.border_sel = midnight_th.accent;
        midnight_th.surface = 0xEB1E1B12;
        midnight_th.surface_sel = 0xF5362014;
        midnight_th.border = 0xAA3B2A18;
        evo_rmlui_set_theme(&midnight_th);
    }

    std::cout << "Rendered all settings screens successfully" << std::endl;
    return 0;
}
