#include "evo_rmlui_bridge.h"
#include <vector>
#include <cstdint>

static void fill_reader_fixture(evo_rmlui_reader_params_t& p)
{
    p.title = "README.md";
    p.subtitle = "evoplayer / docs";
    p.badge = "TEXT";
    p.rail_focused = 0;
    
    static const char* const kLines[] = {
        "# EVO Player",
        "",
        "Welcome to EVO Player, the ultimate media player for your homebrew console.",
        "",
        "## Features",
        "- Hardware decoding for maximum performance",
        "- 4K HDR support where available",
        "- Native user interface built on ps5-homebrew-ui",
        "- Subtitle rendering",
        "- FTP server integration",
        "",
        "## Controls",
        "Use the directional pad to navigate menus. Press Cross to select,",
        "and Circle to go back.",
        "",
        "During playback:",
        "- L2 / R2: Rewind / Fast Forward",
        "- L1 / R1: Previous / Next Chapter",
        "- Square: Audio and Subtitle options",
        "- Triangle: Media Info",
        "",
        "## Installation",
        "Place your media files in the USB drive or internal storage.",
        "The app will automatically discover them.",
        "",
        "## Known Issues",
        "- Some esoteric audio codecs might require software fallback.",
        "- Network streaming performance depends on your Wi-Fi.",
        "",
        "Enjoy your media!"
    };
    
    p.line_count = 30;
    for (int i = 0; i < 30; ++i) {
        p.lines[i] = kLines[i];
    }
    
    p.face = 2;
    p.progress = 0.3;
    p.visible_frac = 0.4;
    p.notice = "";
    p.footnote = "FIRST 10KB OF 45KB";
}

static void fill_image_fixture(evo_rmlui_image_params_t& p, std::vector<uint32_t>& storage)
{
    p.title = "sample_image.jpg";
    p.w = 640;
    p.h = 360;
    p.loaded = 1;
    
    storage.resize(640 * 360);
    for (int y = 0; y < 360; ++y) {
        for (int x = 0; x < 640; ++x) {
            uint8_t r = (x * 255) / 640;
            uint8_t g = (y * 255) / 360;
            uint8_t b = 128;
            uint8_t a = 255;
            storage[y * 640 + x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
    
    p.pixels = storage.data();
}

static void fill_keyboard_fixture(evo_keyboard_params_t& p)
{
    p.visible = 1;
    p.native_only = 0;
    p.title = "SEARCH";
    p.text = "blade";
    p.mode_label = "LOWERCASE";
    
    p.rows[0] = "1234567890";
    p.rows[1] = "qwertyuiop";
    p.rows[2] = "asdfghjkl-";
    p.rows[3] = "zxcvbnm,._";
    
    p.action_labels[0] = "SHIFT";
    p.action_labels[1] = "SPACE";
    p.action_labels[2] = "DELETE";
    p.action_labels[3] = "CANCEL";
    p.action_labels[4] = "DONE";
    p.action_labels[5] = "";
    
    p.focus_row = 1;
    p.focus_col = 3;
    p.show_caret = 1;
    p.len = 5;
    p.max_len = 64;
}

/* Surround Sound Studio: mode 0 stage, 1 spatial orb, 2 calibration. */
static void fill_surround_fixture(evo_rmlui_surround_params_t& p, int mode)
{
    p = evo_rmlui_surround_params_t{};
    static const char* const kNames[8] = {"FRONT LEFT", "FRONT RIGHT", "CENTER", "SUBWOOFER",
                                          "BACK LEFT", "BACK RIGHT", "SIDE LEFT", "SIDE RIGHT"};
    static const char* const kLabels[8] = {"FL", "FR", "C", "LFE", "BL", "BR", "SL", "SR"};
    static const int kDx[8] = {-190, 190, 0, -300, -200, 200, -320, 320};
    static const int kDy[8] = {-260, -260, -290, -180, 220, 220, -10, -10};
    static const int kOrder[8] = {0, 2, 1, 7, 6, 4, 5, 3};
    p.is_51_layout = 0;
    p.speaker_count = 8;
    for (int i = 0; i < 8; ++i)
    {
        p.speakers[i] = {kNames[i], kLabels[i], i == 3 ? 60.0 : 440.0, kDx[i], kDy[i], i, 8 + i, 0};
        p.order[i] = kOrder[i];
    }
    p.order_count = 8;
    p.px_per_m = 100.0f;
    p.anim_time = 1.3f;
    p.active_channel = -1;
    p.nearest_channel = -1;
    p.selected_item = 2;
    p.cal_message = "";
    p.source_dbfs = -18.0f;
    p.distance_m = 1.8f;
    if (mode == 0)
    {
        p.selected_item = 8;
        p.active_channel = 0;
        p.surround_mode = 1;
        p.proximity[0] = 1.0f;
        p.proximity[2] = 0.35f;
        p.nearest_channel = 0;
    }
    else if (mode == 1)
    {
        p.view_mode = EVO_SURROUND_VIEW_ORB;
        p.orb_active = 1;
        p.tone_follow = 1;
        p.field_playing = 1;
        p.orb_x = -120.0f;
        p.orb_y = -150.0f;
        p.orb_z_m = 0.6f;
        p.flight_mode = 1;
        p.nearest_channel = 0;
        p.azimuth_deg = 322.0f;
        p.elevation_deg = 18.0f;
        for (int i = 0; i < 8; ++i)
            p.proximity[i] = i == 0 ? 0.82f : (i == 2 ? 0.55f : 0.12f * i / 8.0f);
        p.trail_count = 5;
        for (int i = 0; i < 5; ++i)
        {
            p.trail_x[i] = p.orb_x + 14.0f * (i + 1);
            p.trail_y[i] = p.orb_y + 9.0f * (i + 1);
        }
    }
    else
    {
        p.view_mode = EVO_SURROUND_VIEW_CALIBRATION;
        p.cal_phase = EVO_SURROUND_CAL_MEASURING;
        p.cal_step = 3;
        p.cal_total = 8;
        p.cal_channel = 2;
        p.cal_mic_level = 0.55f;
        p.cal_noise_db = -62.0f;
        p.cal_message = "Measuring...";
        for (int i = 0; i < 8; ++i)
        {
            p.cal_detected[i] = 1;
            p.cal_trim_db[i] = -1.5f + i * 0.4f;
            p.cal_path_m[i] = 0.1f * i;
            p.cal_delay_ms[i] = 0.3f * i;
        }
    }
}

/* Media-server poster page (Emby / Jellyfin / Stremio addons). */
static void fill_provider_fixture(evo_hui_provider_params_t& p, std::vector<uint32_t>& art, int focus)
{
    p = evo_hui_provider_params_t{};
    const int w = 200, h = 300;
    art.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const uint32_t r = 40 + x * 150 / w, g = 70 + y * 120 / h, b = 160;
            art[static_cast<size_t>(y) * w + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    static const char* const kTitles[8] = {"Big Buck Bunny", "Sintel", "Tears of Steel", "Elephants Dream",
                                           "Movies", "Cosmos Laundromat", "Spring", "Caminandes"};
    static const char* const kSubs[8] = {"2008 \xC2\xB7 10 min", "2010 \xC2\xB7 15 min", "2012 \xC2\xB7 12 min", "2006",
                                         "Library", "2015", "2019 \xC2\xB7 8 min", "2016"};
    static const char* const kInit[8] = {"B", "S", "T", "E", "M", "C", "S", "C"};
    p.provider_name = "Jellyfin";
    p.breadcrumb = "Jellyfin / Movies";
    p.status = "";
    p.query = "";
    p.page_info = "PAGE 1 OF 3";
    p.has_multiple_pages = 1;
    p.count = 22;
    p.row_count = 8;
    for (int i = 0; i < 8; ++i)
    {
        p.rows[i].title = kTitles[i];
        p.rows[i].subtitle = kSubs[i];
        p.rows[i].initial = kInit[i];
        p.rows[i].art_key = kTitles[i];
        if (i < 5 && i != 4)
        {
            p.rows[i].art = art.data();
            p.rows[i].art_w = w;
            p.rows[i].art_h = h;
        }
        p.rows[i].is_folder = i == 4;
        p.rows[i].played = i == 1;
        p.rows[i].progress = i == 2 ? 42 : 0;
    }
    p.focus = focus;
    p.has_selected = 1;
    p.selected_title = kTitles[focus];
    p.selected_subtitle = kSubs[focus];
    p.selected_initial = kInit[focus];
    p.selected_overview = "A large and lovable rabbit deals with three tiny bullies, led by a flying squirrel, who "
                          "are determined to ruin his day. Peaceful by nature, Big Buck Bunny finds his patience "
                          "tested as the situation escalates.";
    p.selected_resume = focus == 2 ? "Resume from 5:10" : "";
    p.selected_progress = focus == 2 ? 42 : 0;
    p.selected_art_key = kTitles[focus];
    p.selected_art = (focus < 5 && focus != 4) ? art.data() : nullptr;
    p.selected_art_w = w;
    p.selected_art_h = h;
    p.selected_is_folder = focus == 4;
    p.selected_played = focus == 1;
}

/* The channel screens: variant 1 Live TV, 2 Xtream. mode 0 channels, 1 setup, 2 options panel. */
static void fill_channels_fixture(evo_hui_provider_params_t& p, std::vector<uint32_t>& art, int variant, int mode)
{
    fill_provider_fixture(p, art, 1);
    p.variant = variant;
    p.provider_name = variant == EVO_HUI_PROVIDER_XTREAM ? "Xtream Codes" : "IPTV";
    p.breadcrumb = variant == EVO_HUI_PROVIDER_XTREAM ? "Xtream Codes / Sports" : "IPTV / Sports";
    static const char* const kNames[8] = {"BBC One HD", "Sky Sports Main Event", "ESPN", "Eurosport 1",
                                          "Documentaries", "CNN International", "Al Jazeera", "France 24"};
    static const char* const kNow[8] = {"The One Show", "Premier League: Arsenal v Chelsea", "SportsCenter", "", "", "Quest Means Business", "", "Journal"};
    static const char* const kNext[8] = {"EastEnders", "Super Sunday", "", "", "", "Newsroom", "", ""};
    for (int i = 0; i < 8; ++i)
    {
        p.rows[i].title = kNames[i];
        p.rows[i].subtitle = i == 4 ? "12 channels" : "Live";
        p.rows[i].initial = "C";
        p.rows[i].is_folder = i == 4;
        p.rows[i].is_live = i != 4;
        p.rows[i].played = 0;
        p.rows[i].progress = 0;
        p.rows[i].now = kNow[i];
        p.rows[i].next = kNext[i];
        p.rows[i].art = (i == 0 || i == 2) ? art.data() : nullptr;
    }
    p.focus = 1;
    p.selected_title = kNames[1];
    p.selected_subtitle = "Sports";
    p.selected_is_folder = 0;
    p.selected_num = "CH 2";
    p.selected_tech = "LIVE HLS STREAM \xE2\x80\xA2 1080p \xE2\x80\xA2 60 FPS";
    p.selected_now = kNow[1];
    p.selected_next = kNext[1];
    p.selected_art = nullptr;
    p.epg_status = "Live Broadcast";
    p.is_folder_level = 0;
    if (mode == 1)
    {
        p.empty = 1;
        p.count = 0;
        p.row_count = 0;
        p.has_selected = 0;
        p.focus = -1;
        p.setup_configured = 1;
        p.setup_account = "Signed in to up.kiwi";
        p.setup_focus = 1;
    }
    else if (mode == 2)
    {
        p.panel_open = 1;
        p.panel_crumb = "OPTIONS \xE2\x80\xBA ";
        p.panel_eyebrow = "LIVE TV";
        p.panel_title = "Playlist & guide";
        p.panel_sub = "1,204 channels from my-playlist.m3u";
        p.panel_note_b = "TIP";
        p.panel_note = "Pin a guide to keep it across reloads.";
        p.panel_accept = "SELECT";
        p.panel_back = "CLOSE";
        static const char* const kT[4] = {"Change playlist", "Channel guide", "Refresh now", "Remove playlist"};
        static const char* const kD[4] = {"my-playlist.m3u", "epg.xml.gz (in use)", "Last updated 3 hours ago", "Forget this source"};
        p.panel_row_count = 4;
        for (int i = 0; i < 4; ++i)
        {
            p.panel_rows[i].title = kT[i];
            p.panel_rows[i].detail = kD[i];
            p.panel_rows[i].icon = "/assets/icons/icon_tv.png";
            p.panel_rows[i].chevron = i < 2;
            p.panel_rows[i].badge = i == 1 ? "ON" : "";
            p.panel_rows[i].warn = i == 3;
            p.panel_rows[i].focused = i == 1;
        }
    }
}
