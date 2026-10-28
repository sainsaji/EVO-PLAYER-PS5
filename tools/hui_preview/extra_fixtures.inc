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

/* The keyboard with the phone link up: a QR (a fixed pattern here) and phone: ip:port under it. */
static void fill_keyboard_phone_fixture(evo_keyboard_params_t& p)
{
    static unsigned char qr[29 * 29];
    for (int y = 0; y < 29; ++y)
        for (int x = 0; x < 29; ++x)
            qr[y * 29 + x] = (unsigned char)(((x * 7 + y * 13 + x * y) % 5) < 2);
    fill_keyboard_fixture(p);
    p.qr_size = 29;
    p.qr_modules = qr;
    p.phone_title = "phone: 192.168.0.12:9780";
}

/* The keyboard opened while every phone-link port is taken: no QR, a notice instead. */
static void fill_keyboard_noport_fixture(evo_keyboard_params_t& p)
{
    fill_keyboard_fixture(p);
    p.phone_title = "PHONE LINK OFF";
    p.phone_notice = "Ports 9780 to 9789 are all taken, so typing from a phone is off. Set a new port to turn it back on.";
    p.phone_kind = 0;
    p.phone_action = "Set port";
}

/* The digits-only pad for a port: three columns, backspace and Done beside the 0. */
static const evo_kb_key_t kPadR0[] = {{EVO_KB_KEY_CHAR, 1, "1", ""}, {EVO_KB_KEY_CHAR, 1, "2", ""}, {EVO_KB_KEY_CHAR, 1, "3", ""}};
static const evo_kb_key_t kPadR1[] = {{EVO_KB_KEY_CHAR, 1, "4", ""}, {EVO_KB_KEY_CHAR, 1, "5", ""}, {EVO_KB_KEY_CHAR, 1, "6", ""}};
static const evo_kb_key_t kPadR2[] = {{EVO_KB_KEY_CHAR, 1, "7", ""}, {EVO_KB_KEY_CHAR, 1, "8", ""}, {EVO_KB_KEY_CHAR, 1, "9", ""}};
static const evo_kb_key_t kPadR3[] = {{EVO_KB_KEY_BACKSPACE, 1, "", ""}, {EVO_KB_KEY_CHAR, 1, "0", ""}, {EVO_KB_KEY_DONE, 1, "", ""}};

/* The same keyboard after the button: typing a port, with the check result beside it. */
static void fill_keyboard_port_fixture(evo_keyboard_params_t& p, int free_port)
{
    fill_keyboard_fixture(p);
    for (int l = 0; l < EVO_KB_LAYERS; ++l)
    {
        p.key_rows[l][0] = kPadR0; p.key_row_len[l][0] = 3;
        p.key_rows[l][1] = kPadR1; p.key_row_len[l][1] = 3;
        p.key_rows[l][2] = kPadR2; p.key_row_len[l][2] = 3;
        p.key_rows[l][3] = kPadR3; p.key_row_len[l][3] = 3;
    }
    p.key_columns = 3;
    p.focus_row = 1;
    p.focus_col = 1;
    p.title = "PHONE & LOG PORT (1024-65535)";
    p.text = free_port ? "9800" : "9781";
    p.mode_label = "NUMBERS";
    p.len = 4;
    p.max_len = 5;
    p.clear_visible = 0;
    p.phone_title = free_port ? "PORT 9800 IS FREE" : "PORT 9781 IS TAKEN";
    p.phone_notice = free_port ? "Save it and the phone link and QR code come back at once."
                               : "Something else is using it. Type another port.";
    p.phone_kind = free_port ? 1 : 0;
    p.phone_action = nullptr;
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
    p.row_count = 16;
    for (int i = 0; i < 16; ++i)
    {
        p.rows[i].title = kTitles[i % 8];
        p.rows[i].subtitle = kSubs[i % 8];
        p.rows[i].initial = kInit[i % 8];
        p.rows[i].art_key = kTitles[i % 8];
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
    for (int i = 0; i < 16; ++i)
    {
        p.rows[i].title = kNames[i % 8];
        p.rows[i].subtitle = i == 4 ? "12 channels" : "Live";
        p.rows[i].initial = "C";
        p.rows[i].is_folder = i == 4;
        p.rows[i].is_live = i != 4;
        p.rows[i].played = 0;
        p.rows[i].progress = 0;
        p.rows[i].now = kNow[i % 8];
        p.rows[i].next = kNext[i % 8];
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

/* The media-server home as shelves (Continue Watching, Recently Added, a library). */
static void fill_shelves_fixture(evo_hui_provider_params_t& p, std::vector<uint32_t>& art, int focus_shelf, int focus_card)
{
    fill_provider_fixture(p, art, 0);
    p.shelves_active = 1;
    p.item_menu = 1;
    p.breadcrumb = "Jellyfin";
    p.shelf_total = 4;
    p.shelf_count = 4;
    p.shelf_focus_abs = focus_shelf;
    static const char* const kShelf[4] = {"Continue Watching", "Next Up", "Recently Added", "Movies"};
    static const char* const kTitles[10] = {"Masterplan", "Whalefall", "Street Fighter", "Tenzing", "Animals",
                                            "The Spiral", "Other Mommy", "Love of Your Life", "Big Buck Bunny", "See all"};
    for (int s = 0; s < 4; ++s)
    {
        evo_hui_shelf_t& sh = p.shelves[s];
        sh.title = kShelf[s];
        sh.abs_index = s;
        sh.total_cards = 24;
        sh.first_card = 0;
        sh.scroll_card = 0;
        sh.focus_card = s == focus_shelf ? focus_card : -1;
        sh.card_count = 10;
        for (int i = 0; i < 10; ++i)
        {
            evo_hui_provider_row_t& c = sh.cards[i];
            c.title = kTitles[(i + s) % 9];
            c.subtitle = "2026";
            c.initial = "M";
            c.art_key = kTitles[(i + s) % 9];
            if (i != 9 && i != 5)
            {
                c.art = art.data();
                c.art_w = 200;
                c.art_h = 300;
            }
            c.played = s == 2 && i == 1;
            c.progress = s == 0 && i < 4 ? 20 + i * 18 : 0;
            c.see_all = i == 9;
            if (i == 9)
                c.title = "See all";
        }
    }
    p.selected_title = "Street Fighter";
    p.selected_subtitle = "2026  -  7.1/10  -  R  -  Action";
    p.selected_resume = "Resume from 41:20";
}

/* The same shelves screen for the other providers: only the rows differ.
 * kind 0 Stremio (posters), 1 Plex (posters), 2 IPTV (channels + a poster row),
 * 3 Xtream (category cards). */
static void fill_provider_shelves_fixture(evo_hui_provider_params_t& p, std::vector<uint32_t>& art, int kind,
                                          int focus_shelf, int focus_card)
{
    fill_shelves_fixture(p, art, focus_shelf, focus_card);
    static const char* const kNames[4] = {"Stremio", "Plex", "IPTV", "Xtream Codes"};
    p.provider_name = kNames[kind];
    p.breadcrumb = kNames[kind];
    p.variant = kind == 2 ? EVO_HUI_PROVIDER_LIVETV : kind == 3 ? EVO_HUI_PROVIDER_XTREAM : EVO_HUI_PROVIDER_MEDIASERVER;
    p.item_menu = kind < 2 ? 1 : 0;
    p.options_hint = kind == 2 ? "Playlist & guide" : kind == 3 ? "Account" : kind == 0 ? "Add & manage addons" : "Edit server";
    if (kind == 0)
    {
        /* a wide image behind the shelves, and the title's logo top right */
        static std::vector<uint32_t> wide(320 * 180), logo(240 * 60);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
                wide[y * 320 + x] = 0xff000000u | (uint32_t(60 + y / 2) << 16) | (uint32_t(40 + x / 4) << 8) | 0x30u;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 240; ++x)
                logo[y * 240 + x] = (x > 8 && x < 232 && y > 14 && y < 46) ? 0xffffffffu : 0x00000000u;
        p.backdrop = wide.data();
        p.backdrop_w = 320;
        p.backdrop_h = 180;
        p.backdrop_key = "fixture-backdrop";
        p.logo = logo.data();
        p.logo_w = 240;
        p.logo_h = 60;
        p.logo_key = "fixture-logo";
        static const char* const kS[4] = {"Popular - Movies", "Popular - Series", "Trending", "Cinemeta - Top"};
        for (int s = 0; s < 4; ++s)
            p.shelves[s].title = kS[s];
        /* the Details block over the focused row */
        p.selected_title = "Love of Your Life";
        p.selected_subtitle = "2026  \xC2\xB7  1h 49m  \xC2\xB7  Drama, Romance";
        p.selected_rating = "7.8";
        p.selected_cert = "PG-13";
        p.selected_overview =
            "After years away, a woman returns to the small coastal town she grew up in and finds the summer she left "
            "behind is still waiting for her, along with the one person she hoped never to see again. What starts as "
            "a quiet visit turns into a season that changes everything.";
        p.selected_cast = "Margaret Qualley, Gabriel Basso, Catherine Keener";
        p.selected_resume = "";
        p.logo = nullptr;      /* this title has no logo: its name is drawn */
    }
    else if (kind == 1)
    {
        static const char* const kS[4] = {"Movies", "TV Shows", "Anime", "Documentaries"};
        for (int s = 0; s < 4; ++s)
            p.shelves[s].title = kS[s];
    }
    else if (kind == 2)
    {
        static const char* const kS[4] = {"FAVORITES", "SPORTS", "NEWS", "MOVIES"};
        static const char* const kC[10] = {"ESPN", "Sky Sports", "BBC One", "CNN", "Eurosport",
                                           "Fox News", "HBO", "Discovery", "Nat Geo", "See all"};
        static const char* const kNow[10] = {"SportsCenter", "Premier League", "The News at Ten", "Situation Room",
                                             "Tour de France", "The Five", "Dune", "Gold Rush", "Wild Africa", ""};
        for (int s = 0; s < 4; ++s)
        {
            evo_hui_shelf_t& sh = p.shelves[s];
            sh.title = kS[s];
            const bool channels = s < 3;               /* the last shelf is a poster row: side by side */
            for (int i = 0; channels && i < 10; ++i)
            {
                evo_hui_provider_row_t& c = sh.cards[i];
                c.title = kC[(i + s) % 9];
                c.subtitle = "";
                c.is_live = i != 9;
                c.now = i != 9 ? kNow[(i + s) % 9] : "";
                c.progress = 0;
                c.played = 0;
                c.art = i % 3 == 2 ? nullptr : art.data();
                c.art_w = 200;
                c.art_h = 300;
            }
        }
        p.selected_title = "ESPN";
        p.selected_subtitle = "Now: SportsCenter   Next: NFL Live";
        p.selected_resume = "";
    }
    else
    {
        static const char* const kS[3] = {"LIVE TV", "MOVIES", "SERIES"};
        for (int s = 0; s < 3; ++s)
        {
            evo_hui_shelf_t& sh = p.shelves[s];
            sh.title = kS[s];
            for (int i = 0; i < 10; ++i)
            {
                evo_hui_provider_row_t& c = sh.cards[i];
                c.is_folder = i != 9;
                c.art = nullptr;
                c.subtitle = "";
                c.progress = 0;
                c.played = 0;
            }
        }
        p.shelf_count = 3;
        p.shelf_total = 3;
        p.selected_title = "Sports";
        p.selected_subtitle = "Category";
        p.selected_resume = "";
    }
}

static void fill_settings_pv_extra_fixture(evo_rmlui_settings_params_t &p, const std::string &kind)
{
    if (kind == "resume")
    {
        build_preview_section(p, 0, "VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "AUTO", 0, 0, "", {"AUTO", "HARDWARE", "SOFTWARE"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, EVO_RMLUI_PREVIEW_RESUME, "Picks a film up where you stopped instead of from the start.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN", "../icons/icon_aspect.png", "FIT", 0, 0, "", {"FIT", "FILL", "STRETCH"}, 0, -1, false}
        });
    }
    else if (kind == "resume_off")
    {
        build_preview_section(p, 0, "VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "AUTO", 0, 0, "", {"AUTO", "HARDWARE", "SOFTWARE"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 0, EVO_RMLUI_PREVIEW_RESUME, "Picks a film up where you stopped instead of from the start.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN", "../icons/icon_aspect.png", "FIT", 0, 0, "", {"FIT", "FILL", "STRETCH"}, 0, -1, false}
        });
    }
    else if (kind == "decoder")
    {
        build_preview_section(p, 0, "VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "HARDWARE", 0, EVO_RMLUI_PREVIEW_DECODER, "Hardware decoding is smoother and cooler. Software is for odd formats.", {"AUTO", "HARDWARE", "SOFTWARE"}, 1, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "UPSCALING", "SHARPEN VIDEO SMALLER THAN THE SCREEN", "../icons/icon_sparkles.png", "AI", 0, 0, "", {"OFF", "SHARP", "AI"}, 2, -1, false}
        });
    }
    else if (kind == "decoder_sw")
    {
        build_preview_section(p, 0, "VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "SOFTWARE", 0, EVO_RMLUI_PREVIEW_DECODER, "Hardware decoding is smoother and cooler. Software is for odd formats.", {"AUTO", "HARDWARE", "SOFTWARE"}, 2, 2, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "UPSCALING", "SHARPEN VIDEO SMALLER THAN THE SCREEN", "../icons/icon_sparkles.png", "AI", 0, 0, "", {"OFF", "SHARP", "AI"}, 2, -1, false}
        });
    }
    else if (kind == "nav_sounds")
    {
        build_preview_section(p, 1, "AUDIO", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "STEREO", 0, 0, "", {"STEREO", "SURROUND 5.1"}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS", "../icons/icon_listener.png", "", 1, EVO_RMLUI_PREVIEW_NAV_SOUNDS, "A soft click when you move and press buttons.", {}, 0, -1, false}
        });
    }
    else if (kind == "nav_sounds_off")
    {
        build_preview_section(p, 1, "AUDIO", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "STEREO", 0, 0, "", {"STEREO", "SURROUND 5.1"}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS", "../icons/icon_listener.png", "", 0, EVO_RMLUI_PREVIEW_NAV_SOUNDS, "A soft click when you move and press buttons.", {}, 0, -1, false}
        });
    }
    else if (kind == "folders_first")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "MIDNIGHT", 0, 0, "", {"MIDNIGHT", "DARK", "AMBER", "TEAL"}, 0, -1, true},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, EVO_RMLUI_PREVIEW_FOLDERS_FIRST, "Lists folders above files in the USB browser.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, 0, "", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false}
        });
    }
    else if (kind == "folders_first_off")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "MIDNIGHT", 0, 0, "", {"MIDNIGHT", "DARK", "AMBER", "TEAL"}, 0, -1, true},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 0, EVO_RMLUI_PREVIEW_FOLDERS_FIRST, "Lists folders above files in the USB browser.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, 0, "", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false}
        });
    }
    else if (kind == "keyboard")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, EVO_RMLUI_PREVIEW_KEYBOARD, "The virtual keyboard is drawn by EVO. The native one is the PS5 system keyboard.", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, 0, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "keyboard_ps5")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "NATIVE PS5 IME", 0, EVO_RMLUI_PREVIEW_KEYBOARD, "The virtual keyboard is drawn by EVO. The native one is the PS5 system keyboard.", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 1, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "ask_stream")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, 0, "", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 1, EVO_RMLUI_PREVIEW_ASK_STREAM, "Lets you choose the format and quality each time you open a live channel.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "COMPATIBILITY REPORT", "WRITES A CODEC REPORT TO USB0", "../icons/icon_report.png", "RUN", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "ask_stream_off")
    {
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, 0, "", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 0, EVO_RMLUI_PREVIEW_ASK_STREAM, "Lets you choose the format and quality each time you open a live channel.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "COMPATIBILITY REPORT", "WRITES A CODEC REPORT TO USB0", "../icons/icon_report.png", "RUN", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "port_free" || kind == "port_busy" || kind == "port_fallback" || kind == "port_none")
    {
        const int state = (kind == "port_free") ? 0 : (kind == "port_busy") ? 1 : (kind == "port_fallback") ? 2 : 3;
        const char *badge = (state == 3) ? "NONE" : (state == 0) ? "9800" : "9780";
        const char *link = (state == 0) ? "http://192.168.0.12:9800/input"
                         : (state == 1) ? "http://192.168.0.12:9780/input"
                         : (state == 2) ? "http://192.168.0.12:9783/input"
                                        : "";
        const char *status = (state == 0) ? "Listening on 9800"
                           : (state == 1) ? "9800 is in use, kept 9780"
                           : (state == 2) ? "9780 is busy, using 9783"
                                          : "Ports 9780 to 9789 are all taken, so the phone keyboard and live log are off. Please set a new port.";
        build_preview_section(p, 3, "INTERFACE", "THEMES, CONTROLLER & SORTING", {
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, 0, "", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "PHONE & LOG PORT", "PHONE LINK & LIVE LOG", "../icons/icon_keyboard.png", badge, 0, EVO_RMLUI_PREVIEW_PORT, "EVO checks the port is free before it saves it. If it is busy at start-up, EVO tries the next nine and shows the one it got.", {link, status}, state, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "debug_overlay")
    {
        build_preview_section(p, 4, "SYSTEM & DIAGNOSTICS", "DIAGNOSTICS & SYSTEM MANAGEMENT", {
            {EVO_RMLUI_ROW_ACTION, "DEVELOPER TOOLS", "SYSTEM DIAGNOSTICS & PERFORMANCE STATS", "../icons/icon_developer_tools.png", "OPEN", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "DEBUG OVERLAY", "SHOWS LIVE PERFORMANCE NUMBERS ON SCREEN", "../icons/icon_activity.png", "", 1, EVO_RMLUI_PREVIEW_DEBUG_OVERLAY, "Shows live performance numbers on screen.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER", "../icons/icon_power.png", "QUIT", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "debug_overlay_off")
    {
        build_preview_section(p, 4, "SYSTEM & DIAGNOSTICS", "DIAGNOSTICS & SYSTEM MANAGEMENT", {
            {EVO_RMLUI_ROW_ACTION, "DEVELOPER TOOLS", "SYSTEM DIAGNOSTICS & PERFORMANCE STATS", "../icons/icon_developer_tools.png", "OPEN", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "DEBUG OVERLAY", "SHOWS LIVE PERFORMANCE NUMBERS ON SCREEN", "../icons/icon_activity.png", "", 0, EVO_RMLUI_PREVIEW_DEBUG_OVERLAY, "Shows live performance numbers on screen.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER", "../icons/icon_power.png", "QUIT", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "motion_off")
    {
        build_preview_section(p, 5, "EXPERIMENTAL", "UNFINISHED WORK - EXPECT ARTEFACTS", {
            {EVO_RMLUI_ROW_TOGGLE, "MATCH VIDEO FRAME RATE", "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ", "../icons/icon_gauge.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "MOTION SMOOTHING", "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION", "../icons/icon_activity.png", "", 0, EVO_RMLUI_PREVIEW_MOTION, "Pans and sports look fluid. Films can look too smooth, and fast motion can show haloes.", {"OFF", "LOW", "HIGH"}, 0, 0, false},
            {EVO_RMLUI_ROW_VALUE, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK", "../icons/icon_sparkles.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false}
        });
    }
    else if (kind == "refresh_60")
    {
        build_preview_section(p, 0, "VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "AI NETWORK", "BIGGER IS SHARPER BUT HEAVIER - MAXIMUM IS FOR PS5 PRO", "../icons/icon_brain.png", "LARGE", 0, 0, "", {"AUTO", "STANDARD", "LARGE", "MAXIMUM (PRO)"}, 2, -1, false},
            {EVO_RMLUI_ROW_VALUE, "120 HZ OUTPUT", "SMOOTHER 24 FPS - THE TV GOES BLACK BRIEFLY ON EACH SWITCH", "../icons/icon_gauge.png", "", 0, EVO_RMLUI_PREVIEW_REFRESH, "24 fps film plays more evenly on a 120 Hz output. The TV goes black briefly on each switch.", {"OFF", "AUTO (ALWAYS 120 HZ)", "PLAYBACK ONLY"}, 0, 0, false},
            {EVO_RMLUI_ROW_VALUE, "HDR OUTPUT", "HDR10 FOR HDR VIDEO - THE TV GOES BLACK BRIEFLY AT START & END", "../icons/icon_sun.png", "AUTO", 0, 0, "", {"OFF (TONE-MAP TO SDR)", "AUTO (MATCH CONTENT)"}, 1, -1, false}
        });
    }
    else if (kind == "match_rate_off")
    {
        build_preview_section(p, 5, "EXPERIMENTAL", "UNFINISHED WORK - EXPECT ARTEFACTS", {
            {EVO_RMLUI_ROW_VALUE, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK", "../icons/icon_sparkles.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "MATCH VIDEO FRAME RATE", "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ", "../icons/icon_gauge.png", "", 0, EVO_RMLUI_PREVIEW_MATCH_RATE, "Outputs the video's own rate (24p or 50 Hz) so motion has no judder.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "MOTION SMOOTHING", "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION", "../icons/icon_activity.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false}
        });
    }
    else if (kind == "channels_stereo")
    {
        build_preview_section(p, 1, "AUDIO", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_TOGGLE, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI", "../icons/icon_volume.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "STEREO (2.0)", 0, EVO_RMLUI_PREVIEW_CHANNELS, "Stereo mixes 5.1 and 7.1 down to two speakers or headphones.", {"AUTO", "STEREO (2.0)"}, 1, 1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "lightbar_off")
    {
        build_preview_section(p, 3, "INTERFACE & STORAGE", "THEMES, CONTROLS & BROWSING", {
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "MIDNIGHT", 0, 0, "", {"MIDNIGHT", "CARBON", "EMBER", "AURORA"}, 0, -1, true},
            {EVO_RMLUI_ROW_TOGGLE, "CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT", "../icons/icon_gamepad.png", "", 0, EVO_RMLUI_PREVIEW_LIGHTBAR, "The DualSense light follows the theme accent.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_font_standard")
    {
        build_preview_section(p, 2, "SUBTITLES", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "STANDARD", 0, EVO_RMLUI_PREVIEW_SUB_FONT, "Picks the typeface for on-screen subtitles.", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 0, 0, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false}
        });
    }
    else if (kind == "sub_font_bold")
    {
        build_preview_section(p, 2, "SUBTITLES", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "BOLD", 0, EVO_RMLUI_PREVIEW_SUB_FONT, "Picks the typeface for on-screen subtitles.", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 2, 2, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false}
        });
    }
    else if (kind == "sub_font_condensed")
    {
        build_preview_section(p, 2, "SUBTITLES", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "CONDENSED", 0, EVO_RMLUI_PREVIEW_SUB_FONT, "Picks the typeface for on-screen subtitles.", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 3, 3, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false}
        });
    }
}
