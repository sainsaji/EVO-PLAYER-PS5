#pragma once
#include <RmlUi/Core.h>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "evo_rmlui_render.h"
#include "evo_rmlui_system.h"

struct EvoPlaybackState {
    std::string title;
    std::string meta;
    std::string res_badge;
    std::string hdr_badge;
    std::string codec_badge;
    std::string fps_badge;
    std::string audio_badge;
    double position_sec = 0.0;
    double duration_sec = 0.0;
    double percentage = 0.0;
    bool paused = false;
    bool scrub_active = false;
    double scrub_target = 0.0;
    std::string audio_track;
    std::string sub_track;
    int view_mode = 0; // 0=FIT, 1=FILL, 2=STRETCH
    bool show_stats = false;
    int alpha = 255;
};

struct EvoDialogAction {
    std::string icon_path;
    std::string label;
    bool is_primary = false;
};

struct EvoDialogState {
    std::string eyebrow;
    std::string title;
    std::string detail;
    double progress_pct = -1.0; // 0.0 to 1.0, or -1.0 to hide
    std::vector<EvoDialogAction> actions;
};

struct EvoSettingsRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    bool has_chevron = true;
    bool is_focused = false;
};

struct EvoSettingsState {
    std::string title;
    std::string subtitle;
    std::string counter;
    int rail_active_idx = 5;
    bool rail_focused = false;
    std::vector<EvoSettingsRow> rows;
};

struct EvoSubtitlesTrack {
    std::string label;
    std::string detail;
    bool is_current = false;
    bool is_focused = false;
};

struct EvoSubtitlesState {
    std::string eyebrow;
    std::string title;
    std::string size_str;
    std::string preview_text;
    int preview_face = 1; // 0=small, 1=medium, 2=large
    std::vector<EvoSubtitlesTrack> tracks;
};

struct EvoMediaInfoState {
    std::string title;
    std::string path;
    std::string res_badge;
    std::string hdr_badge;
    std::string codec_badge;
    std::string fps_badge;
    std::string container;
    std::string file_size;
    std::string duration;
    std::string video_codec;
    std::string resolution;
    std::string color_hdr;
    std::string audio_codec;
    std::string channels;
    std::string sample_rate;
    std::string subtitles;
    std::string output;
    std::string renderer;
};

struct EvoLaunchTile {
    std::string title;
    std::string detail;
    std::string icon_path;
    int progress = -1;
    const uint32_t* art = nullptr;
    int art_w = 0;
    int art_h = 0;
    bool is_focused = false;
};

struct EvoLaunchState {
    std::string app_name = "EVO PLAYER";
    std::string version;
    std::string clock;
    std::string theme_name;

    std::string hero_eyebrow;
    std::string hero_title;
    std::string hero_detail;
    std::string hero_action;
    int hero_progress = -1;
    const uint32_t* hero_art = nullptr;
    int hero_art_w = 0;
    int hero_art_h = 0;
    bool hero_focused = false;

    int recent_total = 0;
    int recent_cursor = -1;
    std::vector<EvoLaunchTile> recent;
    std::vector<EvoLaunchTile> library;
};

struct EvoListRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    int progress = -1;
    bool has_chevron = false;
    bool is_focused = false;
};

struct EvoListHint {
    std::string glyph_path;
    std::string label;
};

struct EvoListState {
    std::string title;
    std::string subtitle;
    int section = 0;
    bool rail_focused = false;

    int total_count = 0;
    int cursor_index = -1;
    std::vector<EvoListRow> rows;

    bool is_empty = false;
    std::string empty_title;
    std::string empty_hint;
    std::string empty_icon;

    std::vector<EvoListHint> hints;
};

struct EvoBrowserRow {
    std::string name;
    std::string detail;
    std::string icon_path;
    std::string badge;
    int progress = -1;
    bool is_favorite = false;
    bool is_focused = false;
};

struct EvoBrowserState {
    std::string path;
    bool at_root = true;
    bool rail_focused = false;

    int total_count = 0;
    int cursor_index = -1;
    std::vector<EvoBrowserRow> rows;

    bool is_empty = false;
    std::string empty_title;
    std::string empty_hint;

    std::string ins_name;
    std::string ins_kind;
    std::string ins_ext;
    bool ins_probing = false;
    std::string ins_preview_badge;
    const uint32_t* ins_preview = nullptr;
    int ins_preview_w = 0;
    int ins_preview_h = 0;
    std::vector<std::pair<std::string, std::string>> ins_props;
};

struct EvoChangelogRelease {
    std::string version;
    std::string tagline;
    std::string date;
    bool is_focused = false;
};

struct EvoChangelogState {
    std::string title;
    std::string subtitle;
    bool rail_focused = false;

    int release_total = 0;
    int cursor_index = 0;
    std::vector<EvoChangelogRelease> releases;

    std::string detail_version;
    std::string detail_tagline;
    int item_total = 0;
    std::vector<std::pair<std::string, std::string>> items;
};

struct EvoThemeColors {
    /*
     * Packed 0xAABBGGRR, the same order EVO_RGBA() produces in evo_theme.c —
     * these are read back byte-for-byte by to_hex_rgb(). They used to be
     * written 0xAARRGGBB, which rendered MIDNIGHT's cyan accent as gold in
     * every frame drawn before main.c pushed the real theme, and in every
     * host uiview render, which never pushes one at all.
     */
    std::string name = "MIDNIGHT";
    uint32_t bg_top = 0xFF160B06;
    uint32_t bg_bottom = 0xFF090402;
    uint32_t surface = 0xEB2E1B12;
    uint32_t surface_sel = 0xF54C2E1B;
    uint32_t border = 0xAA553B2A;
    uint32_t border_sel = 0xDCFFCD00;
    uint32_t accent = 0xFFFFCD00;
    uint32_t accent_soft = 0x3CFFA800;
    uint32_t accent_alt = 0xFFFF5C7A;
    uint32_t text_primary = 0xFFFFF3EC;
    uint32_t text_secondary = 0xFFCCB29F;
    uint32_t text_muted = 0xFF8C715E;
};

struct EvoNavState {
    int active_section = 5; /* 0=HOME 1=BROWSE 2=RECENT 3=FAV 4=EMBY 5=SETTINGS 6=ABOUT */
    int rail_focused = 0;   /* 0=collapsed icon strip, 1=expanded labelled panel */
    int cursor_index = 5;   /* which item has cursor when expanded */
    int visible = 1;        /* 1=show the nav rail, 0=hide (full-screen OSD etc.) */
};

class EvoRmlApp {

public:
    static EvoRmlApp& Instance();

    bool Initialize(int width, int height);
    void Shutdown();

    void SetTheme(const EvoThemeColors& theme);
    void SetVersion(const std::string& v) { m_version = v; }
    const EvoThemeColors& GetTheme() const { return m_theme; }

    void UpdateChangelogState(const EvoChangelogState& state);
    void RenderChangelog(uint32_t* framebuffer, int width, int height);

    void UpdateBrowserState(const EvoBrowserState& state);
    void RenderBrowser(uint32_t* framebuffer, int width, int height);

    void UpdateListState(const EvoListState& state);
    void RenderList(uint32_t* framebuffer, int width, int height);

    void UpdateLaunchState(const EvoLaunchState& state);
    void RenderLaunch(uint32_t* framebuffer, int width, int height);

    void UpdatePlaybackState(const EvoPlaybackState& state);
    void RenderPlaybackOSD(uint32_t* framebuffer, int width, int height);

    void UpdateDialogState(const EvoDialogState& state);
    void RenderDialog(uint32_t* framebuffer, int width, int height);

    void UpdateSettingsState(const EvoSettingsState& state);
    void RenderSettings(uint32_t* framebuffer, int width, int height);

    void UpdateSubtitlesState(const EvoSubtitlesState& state);
    void RenderSubtitles(uint32_t* framebuffer, int width, int height);

    void UpdateMediaInfoState(const EvoMediaInfoState& state);
    void RenderMediaInfo(uint32_t* framebuffer, int width, int height);

    /* Sidebar navigation rail — call UpdateNavState before any Render* call */
    void UpdateNavState(const EvoNavState& state);

    bool IsInitialized() const { return m_initialized; }

private:
    EvoRmlApp();
    ~EvoRmlApp();

    bool m_initialized = false;
    int m_width = 1920;
    int m_height = 1080;

    std::unique_ptr<EvoSystemInterface> m_system;
    std::unique_ptr<EvoRenderInterface> m_render;
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_launch_doc = nullptr;
    Rml::ElementDocument* m_list_doc = nullptr;
    Rml::ElementDocument* m_browser_doc = nullptr;
    Rml::ElementDocument* m_changelog_doc = nullptr;
    Rml::ElementDocument* m_playback_doc = nullptr;
    Rml::ElementDocument* m_dialog_doc = nullptr;
    Rml::ElementDocument* m_settings_doc = nullptr;
    Rml::ElementDocument* m_subtitles_doc = nullptr;
    Rml::ElementDocument* m_mediainfo_doc = nullptr;
    Rml::ElementDocument* m_nav_doc = nullptr;

    EvoThemeColors m_theme;
    std::string m_version;
    EvoLaunchState m_last_launch;
    EvoListState m_last_list;
    EvoBrowserState m_last_browser;
    EvoChangelogState m_last_changelog;
    EvoPlaybackState m_last_state;

    /*
     * RmlUi caches textures by source string, so a poster whose pixels change
     * has to change name too or the stale frame is served forever. Each art
     * slot (0 = hero, 1..6 = the recent shelf) carries a generation that bumps
     * when its pixels do, and the previous generation is released on the way.
     */
    /* Rows the list document actually contains. Must stay in step with
     * EVO_RMLUI_LIST_ROWS in evo_rmlui_bridge.h and with list.rml. */
    static const int kListRows = 9;

    /* Must stay in step with EVO_RMLUI_BROWSER_ROWS / _PROPS and browser.rml. */
    static const int kBrowserRows = 12;
    static const int kBrowserProps = 9;

    /* Must stay in step with EVO_RMLUI_CL_* and changelog.rml. */
    static const int kChangelogReleases = 8;
    static const int kChangelogItems = 14;

    /* 0 = hero, 1..6 = the recent shelf, 7 = the browser preview. */
    static const int kBrowserArtSlot = 7;
    static const int kArtSlots = 1 + 6 + 1;
    int m_art_generation[kArtSlots] = {};
    const uint32_t* m_art_last_ptr[kArtSlots] = {};
    int m_art_last_dims[kArtSlots][2] = {};
    /* The cover cache reuses buffers, so an unchanged pointer does not mean
     * unchanged pixels. The item's title rides along as the content tag. */
    std::string m_art_last_tag[kArtSlots];
    std::string m_art_source[kArtSlots];
    std::string ArtSource(int slot, const uint32_t* pixels, int w, int h,
                          const std::string& tag);
    EvoDialogState m_last_dialog;
    EvoSettingsState m_last_settings;
    EvoSubtitlesState m_last_subtitles;
    EvoMediaInfoState m_last_mediainfo;
    EvoNavState m_last_nav;
};

