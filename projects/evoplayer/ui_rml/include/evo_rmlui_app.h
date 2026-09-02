#pragma once
#include <RmlUi/Core.h>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <unordered_map>
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

    bool operator==(const EvoPlaybackState& o) const {
        return title == o.title && meta == o.meta && res_badge == o.res_badge &&
               hdr_badge == o.hdr_badge && codec_badge == o.codec_badge &&
               fps_badge == o.fps_badge && audio_badge == o.audio_badge &&
               position_sec == o.position_sec && duration_sec == o.duration_sec &&
               percentage == o.percentage && paused == o.paused &&
               scrub_active == o.scrub_active && scrub_target == o.scrub_target &&
               audio_track == o.audio_track && sub_track == o.sub_track &&
               view_mode == o.view_mode && show_stats == o.show_stats && alpha == o.alpha;
    }
    bool operator!=(const EvoPlaybackState& o) const { return !(*this == o); }
};

struct EvoDialogAction {
    std::string icon_path;
    std::string label;
    bool is_primary = false;

    bool operator==(const EvoDialogAction& o) const {
        return icon_path == o.icon_path && label == o.label && is_primary == o.is_primary;
    }
    bool operator!=(const EvoDialogAction& o) const { return !(*this == o); }
};

struct EvoDialogState {
    std::string eyebrow;
    std::string title;
    std::string detail;
    double progress_pct = -1.0; // 0.0 to 1.0, or -1.0 to hide
    std::vector<EvoDialogAction> actions;

    bool operator==(const EvoDialogState& o) const {
        return eyebrow == o.eyebrow && title == o.title && detail == o.detail &&
               progress_pct == o.progress_pct && actions == o.actions;
    }
    bool operator!=(const EvoDialogState& o) const { return !(*this == o); }
};

struct EvoSettingsRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    bool has_chevron = true;
    bool is_focused = false;

    bool operator==(const EvoSettingsRow& o) const {
        return title == o.title && detail == o.detail && icon_path == o.icon_path &&
               badge == o.badge && has_chevron == o.has_chevron && is_focused == o.is_focused;
    }
    bool operator!=(const EvoSettingsRow& o) const { return !(*this == o); }
};

struct EvoSettingsState {
    std::string title;
    std::string subtitle;
    std::string counter;
    int rail_active_idx = 5;
    bool rail_focused = false;
    std::vector<EvoSettingsRow> rows;

    bool operator==(const EvoSettingsState& o) const {
        return title == o.title && subtitle == o.subtitle && counter == o.counter &&
               rail_active_idx == o.rail_active_idx && rail_focused == o.rail_focused &&
               rows == o.rows;
    }
    bool operator!=(const EvoSettingsState& o) const { return !(*this == o); }
};

struct EvoSubtitlesTrack {
    std::string label;
    std::string detail;
    bool is_current = false;
    bool is_focused = false;

    bool operator==(const EvoSubtitlesTrack& o) const {
        return label == o.label && detail == o.detail &&
               is_current == o.is_current && is_focused == o.is_focused;
    }
    bool operator!=(const EvoSubtitlesTrack& o) const { return !(*this == o); }
};

struct EvoSubtitlesState {
    std::string eyebrow;
    std::string title;
    std::string size_str;
    std::string preview_text;
    int preview_face = 1; // 0=small, 1=medium, 2=large
    std::vector<EvoSubtitlesTrack> tracks;

    bool operator==(const EvoSubtitlesState& o) const {
        return eyebrow == o.eyebrow && title == o.title && size_str == o.size_str &&
               preview_text == o.preview_text && preview_face == o.preview_face &&
               tracks == o.tracks;
    }
    bool operator!=(const EvoSubtitlesState& o) const { return !(*this == o); }
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

    bool operator==(const EvoMediaInfoState& o) const {
        return title == o.title && path == o.path && res_badge == o.res_badge &&
               hdr_badge == o.hdr_badge && codec_badge == o.codec_badge &&
               fps_badge == o.fps_badge && container == o.container &&
               file_size == o.file_size && duration == o.duration &&
               video_codec == o.video_codec && resolution == o.resolution &&
               color_hdr == o.color_hdr && audio_codec == o.audio_codec &&
               channels == o.channels && sample_rate == o.sample_rate &&
               subtitles == o.subtitles && output == o.output && renderer == o.renderer;
    }
    bool operator!=(const EvoMediaInfoState& o) const { return !(*this == o); }
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

    bool operator==(const EvoLaunchTile& o) const {
        return title == o.title && detail == o.detail && icon_path == o.icon_path &&
               progress == o.progress && art == o.art && art_w == o.art_w &&
               art_h == o.art_h && is_focused == o.is_focused;
    }
    bool operator!=(const EvoLaunchTile& o) const { return !(*this == o); }
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

    bool operator==(const EvoLaunchState& o) const {
        return app_name == o.app_name && version == o.version && clock == o.clock &&
               theme_name == o.theme_name && hero_eyebrow == o.hero_eyebrow &&
               hero_title == o.hero_title && hero_detail == o.hero_detail &&
               hero_action == o.hero_action && hero_progress == o.hero_progress &&
               hero_art == o.hero_art && hero_art_w == o.hero_art_w &&
               hero_art_h == o.hero_art_h && hero_focused == o.hero_focused &&
               recent_total == o.recent_total && recent_cursor == o.recent_cursor &&
               recent == o.recent && library == o.library;
    }
    bool operator!=(const EvoLaunchState& o) const { return !(*this == o); }
};

struct EvoListRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    int progress = -1;
    bool has_chevron = false;
    bool is_focused = false;

    bool operator==(const EvoListRow& o) const {
        return title == o.title && detail == o.detail && icon_path == o.icon_path &&
               badge == o.badge && progress == o.progress &&
               has_chevron == o.has_chevron && is_focused == o.is_focused;
    }
    bool operator!=(const EvoListRow& o) const { return !(*this == o); }
};

struct EvoListHint {
    std::string glyph_path;
    std::string label;

    bool operator==(const EvoListHint& o) const {
        return glyph_path == o.glyph_path && label == o.label;
    }
    bool operator!=(const EvoListHint& o) const { return !(*this == o); }
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

    bool operator==(const EvoListState& o) const {
        return title == o.title && subtitle == o.subtitle && section == o.section &&
               rail_focused == o.rail_focused && total_count == o.total_count &&
               cursor_index == o.cursor_index && rows == o.rows &&
               is_empty == o.is_empty && empty_title == o.empty_title &&
               empty_hint == o.empty_hint && empty_icon == o.empty_icon && hints == o.hints;
    }
    bool operator!=(const EvoListState& o) const { return !(*this == o); }
};

struct EvoBrowserRow {
    std::string name;
    std::string detail;
    std::string icon_path;
    std::string badge;
    int progress = -1;
    bool is_favorite = false;
    bool is_focused = false;

    bool operator==(const EvoBrowserRow& o) const {
        return name == o.name && detail == o.detail && icon_path == o.icon_path &&
               badge == o.badge && progress == o.progress &&
               is_favorite == o.is_favorite && is_focused == o.is_focused;
    }
    bool operator!=(const EvoBrowserRow& o) const { return !(*this == o); }
};

struct EvoBrowserState {
    std::string path;
    std::string title;
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

    bool operator==(const EvoBrowserState& o) const {
        return path == o.path && title == o.title && at_root == o.at_root && rail_focused == o.rail_focused &&
               total_count == o.total_count && cursor_index == o.cursor_index &&
               rows == o.rows && is_empty == o.is_empty && empty_title == o.empty_title &&
               empty_hint == o.empty_hint && ins_name == o.ins_name &&
               ins_kind == o.ins_kind && ins_ext == o.ins_ext &&
               ins_probing == o.ins_probing && ins_preview_badge == o.ins_preview_badge &&
               ins_preview == o.ins_preview && ins_preview_w == o.ins_preview_w &&
               ins_preview_h == o.ins_preview_h && ins_props == o.ins_props;
    }
    bool operator!=(const EvoBrowserState& o) const { return !(*this == o); }
};

struct EvoChangelogRelease {
    std::string version;
    std::string tagline;
    std::string date;
    bool is_focused = false;

    bool operator==(const EvoChangelogRelease& o) const {
        return version == o.version && tagline == o.tagline &&
               date == o.date && is_focused == o.is_focused;
    }
    bool operator!=(const EvoChangelogRelease& o) const { return !(*this == o); }
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

    bool operator==(const EvoChangelogState& o) const {
        return title == o.title && subtitle == o.subtitle && rail_focused == o.rail_focused &&
               release_total == o.release_total && cursor_index == o.cursor_index &&
               releases == o.releases && detail_version == o.detail_version &&
               detail_tagline == o.detail_tagline && item_total == o.item_total &&
               items == o.items;
    }
    bool operator!=(const EvoChangelogState& o) const { return !(*this == o); }
};

struct EvoReaderState {
    std::string title;
    std::string subtitle;
    std::string badge;
    bool rail_focused = false;

    std::vector<std::string> lines;
    int face = 0;

    double progress = 0.0;
    double visible_frac = 1.0;

    std::string notice;
    std::string footnote;

    bool operator==(const EvoReaderState& o) const {
        return title == o.title && subtitle == o.subtitle && badge == o.badge &&
               rail_focused == o.rail_focused && lines == o.lines && face == o.face &&
               progress == o.progress && visible_frac == o.visible_frac &&
               notice == o.notice && footnote == o.footnote;
    }
    bool operator!=(const EvoReaderState& o) const { return !(*this == o); }
};

struct EvoSurroundSpeaker {
    std::string name;
    std::string label;
    double hz = 0.0;
    int dx = 0;
    int dy = 0;
    int ch = -1;
    int item_idx = -1;
    bool hidden = false;

    bool operator==(const EvoSurroundSpeaker& o) const {
        return name == o.name && label == o.label && hz == o.hz && dx == o.dx &&
               dy == o.dy && ch == o.ch && item_idx == o.item_idx && hidden == o.hidden;
    }
    bool operator!=(const EvoSurroundSpeaker& o) const { return !(*this == o); }
};

struct EvoSurroundState {
    bool rail_focused = false;
    bool is_51_layout = false;
    int selected_item = 0;
    int active_channel = -1;
    int surround_mode = 0;
    std::vector<EvoSurroundSpeaker> speakers;

    bool operator==(const EvoSurroundState& o) const {
        return rail_focused == o.rail_focused && is_51_layout == o.is_51_layout &&
               selected_item == o.selected_item && active_channel == o.active_channel &&
               surround_mode == o.surround_mode && speakers == o.speakers;
    }
    bool operator!=(const EvoSurroundState& o) const { return !(*this == o); }
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

    bool operator==(const EvoNavState& o) const {
        return active_section == o.active_section && rail_focused == o.rail_focused &&
               cursor_index == o.cursor_index && visible == o.visible;
    }
    bool operator!=(const EvoNavState& o) const { return !(*this == o); }
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

    void UpdateReaderState(const EvoReaderState& state);
    void RenderReader(uint32_t* framebuffer, int width, int height);

    void UpdateSurroundState(const EvoSurroundState& state);
    void RenderSurround(uint32_t* framebuffer, int width, int height);

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
    Rml::ElementDocument* m_reader_doc = nullptr;
    Rml::ElementDocument* m_surround_doc = nullptr;
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
    EvoReaderState m_last_reader;
    EvoSurroundState m_last_surround;
    EvoPlaybackState m_last_state;

    /*
     * Bumped on every SetTheme call. Each Update*State call skips its whole
     * body - all the SetProperty/SetInnerRML work - when the incoming state
     * struct equals the one from last frame AND the theme has not moved
     * since that frame was processed. Without the generation check, a screen
     * sitting idle (state unchanged) would miss a live theme switch until
     * something else about it also happened to change.
     *
     * This exists because RmlUi's SetProperty always marks its target dirty
     * regardless of whether the value actually changed, forcing a full
     * geometry/decorator rebuild - so calling it on unchanged state every
     * single frame (the previous behavior here) measured 6-7 fps on the
     * launch screen on real hardware, slow enough that the pad is polled too
     * rarely to reliably catch a normal button tap.
     */
    int m_theme_generation = 0;
    int m_theme_gen_launch = -1;
    int m_theme_gen_list = -1;
    int m_theme_gen_browser = -1;
    int m_theme_gen_changelog = -1;
    int m_theme_gen_reader = -1;
    int m_theme_gen_surround = -1;
    int m_theme_gen_playback = -1;
    int m_theme_gen_dialog = -1;
    int m_theme_gen_settings = -1;
    int m_theme_gen_subtitles = -1;
    int m_theme_gen_mediainfo = -1;
    int m_theme_gen_nav = -1;

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
    static const int kReaderLines = 64;
    static const int kSurroundSpeakers = 8;

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

    /*
     * RmlUi's ElementImage::OnPropertyChange marks geometry dirty on ANY
     * image-color SetProperty call, regardless of whether the value actually
     * changed - SetProperty itself never diffs against the current value.
     * Icon tinting is recomputed every frame (nav rail runs every screen,
     * every frame, via evo_sync_rmlui_nav), so without this cache every icon
     * rebuilds its geometry every frame even when its colour is unchanged
     * from the last one - the cost that made the navbar miss D-pad presses.
     */
    std::unordered_map<Rml::Element*, std::string> m_image_color_cache;
    void SetImageColor(Rml::Element* el, const std::string& color);

    /*
     * State-diffing in Update*State only avoids RE-STYLING unchanged
     * elements; it does nothing about the cost of Context::Render() itself,
     * which walks and CPU-rasterizes every visible element every time it is
     * called regardless of whether anything is dirty - measured at 6-7 fps
     * on the launch screen with state-diffing already in place and skipping
     * ~97% of Update calls (host profile: Context::Update 0.1 ms, the whole
     * ~120 ms frame is the render-interface raster). That render pass is what
     * needs skipping on an unchanged frame.
     *
     * Step 1 of docs/evo-pro/gpu-rendering-plan.md: the full-screen menu
     * documents (launch, list, browser, settings, changelog, reader,
     * surround) are opaque, cover the whole 1080p frame, and - the RCSS
     * carries no transitions or animations - never change between input
     * events. Each rasterizes into m_surface ONLY when m_frame_dirty (a real
     * state / theme / nav change set it), the visible screen switched, or the
     * surface was (re)sized; every frame then blits m_surface into the
     * caller's rotating VideoOut buffer (~0.5 ms memcpy). The blitted pixels
     * are exactly what Context::Render() last produced, so parity holds.
     *
     * Overlay screens that compose over live video (playback OSD, dialog,
     * subtitle picker, media info) are NOT cached - they keep rendering
     * straight into the framebuffer every frame, unchanged.
     */
    bool m_frame_dirty = true;
    std::vector<uint32_t> m_surface;
    int m_surface_w = 0;
    int m_surface_h = 0;
    int m_cached_screen = -1;
    void RenderCachedScreen(int screen_id, uint32_t* framebuffer, int width, int height);
};

