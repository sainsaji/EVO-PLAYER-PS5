#include "evo_rmlui_bridge.h"
#include "evo_rmlui_app.h"

extern "C" {

bool evo_rmlui_init(int screen_width, int screen_height) {
    return EvoRmlApp::Instance().Initialize(screen_width, screen_height);
}

void evo_rmlui_shutdown(void) {
    EvoRmlApp::Instance().Shutdown();
}

bool evo_rmlui_is_initialized(void) {
    return EvoRmlApp::Instance().IsInitialized();
}

void evo_rmlui_update_changelog(const evo_rmlui_changelog_params_t* p) {
    if (!p) return;
    EvoChangelogState state;
    state.title = p->title ? p->title : "CHANGELOG";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.rail_focused = (p->rail_focused != 0);
    state.release_total = p->release_total;
    state.cursor_index = p->cursor_index;

    for (int i = 0; i < p->release_count && i < EVO_RMLUI_CL_RELEASES; i++) {
        EvoChangelogRelease r;
        r.version = p->releases[i].version ? p->releases[i].version : "";
        r.tagline = p->releases[i].tagline ? p->releases[i].tagline : "";
        r.date = p->releases[i].date ? p->releases[i].date : "";
        r.is_focused = (p->releases[i].is_focused != 0);
        state.releases.push_back(r);
    }

    state.detail_version = p->detail_version ? p->detail_version : "";
    state.detail_tagline = p->detail_tagline ? p->detail_tagline : "";
    state.item_total = p->item_total;

    for (int i = 0; i < p->item_count && i < EVO_RMLUI_CL_ITEMS; i++) {
        state.items.emplace_back(
            p->items[i].kind ? p->items[i].kind : "",
            p->items[i].text ? p->items[i].text : "");
    }

    EvoRmlApp::Instance().UpdateChangelogState(state);
}

void evo_rmlui_render_changelog(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderChangelog(framebuffer, width, height);
}

void evo_rmlui_update_reader(const evo_rmlui_reader_params_t* p) {
    if (!p) return;
    EvoReaderState state;
    state.title = p->title ? p->title : "";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.badge = p->badge ? p->badge : "";
    state.rail_focused = (p->rail_focused != 0);
    state.face = p->face;
    state.progress = p->progress;
    state.visible_frac = p->visible_frac;
    state.notice = p->notice ? p->notice : "";
    state.footnote = p->footnote ? p->footnote : "";

    for (int i = 0; i < p->line_count && i < EVO_RMLUI_READER_LINES; i++) {
        state.lines.push_back(p->lines[i] ? p->lines[i] : "");
    }

    EvoRmlApp::Instance().UpdateReaderState(state);
}

void evo_rmlui_render_reader(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderReader(framebuffer, width, height);
}

void evo_rmlui_update_surround(const evo_rmlui_surround_params_t* p) {
    if (!p) return;
    EvoSurroundState state;
    state.rail_focused = (p->rail_focused != 0);
    state.is_51_layout = (p->is_51_layout != 0);
    state.selected_item = p->selected_item;
    state.active_channel = p->active_channel;
    state.surround_mode = p->surround_mode;

    for (int i = 0; i < p->speaker_count && i < EVO_RMLUI_SURROUND_SPEAKERS; i++) {
        EvoSurroundSpeaker s;
        s.name = p->speakers[i].name ? p->speakers[i].name : "";
        s.label = p->speakers[i].label ? p->speakers[i].label : "";
        s.hz = p->speakers[i].hz;
        s.dx = p->speakers[i].dx;
        s.dy = p->speakers[i].dy;
        s.ch = p->speakers[i].ch;
        s.item_idx = p->speakers[i].item_idx;
        s.hidden = (p->speakers[i].hidden != 0);
        state.speakers.push_back(s);
    }

    EvoRmlApp::Instance().UpdateSurroundState(state);
}

void evo_rmlui_render_surround(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderSurround(framebuffer, width, height);
}

void evo_rmlui_update_browser(const evo_rmlui_browser_params_t* p) {
    if (!p) return;
    EvoBrowserState state;
    state.path = p->path ? p->path : "";
    state.title = p->title ? p->title : "";
    state.at_root = (p->at_root != 0);
    state.rail_focused = (p->rail_focused != 0);
    state.total_count = p->total_count;
    state.cursor_index = p->cursor_index;

    for (int i = 0; i < p->row_count && i < EVO_RMLUI_BROWSER_ROWS; i++) {
        EvoBrowserRow r;
        r.name = p->rows[i].name ? p->rows[i].name : "";
        r.detail = p->rows[i].detail ? p->rows[i].detail : "";
        r.icon_path = p->rows[i].icon_path ? p->rows[i].icon_path : "";
        r.badge = p->rows[i].badge ? p->rows[i].badge : "";
        r.progress = p->rows[i].progress;
        r.is_favorite = (p->rows[i].is_favorite != 0);
        r.is_focused = (p->rows[i].is_focused != 0);
        state.rows.push_back(r);
    }

    state.is_empty = (p->is_empty != 0);
    state.empty_title = p->empty_title ? p->empty_title : "";
    state.empty_hint = p->empty_hint ? p->empty_hint : "";

    state.ins_name = p->ins_name ? p->ins_name : "";
    state.ins_kind = p->ins_kind ? p->ins_kind : "";
    state.ins_ext = p->ins_ext ? p->ins_ext : "";
    state.ins_probing = (p->ins_probing != 0);
    state.ins_preview_badge = p->ins_preview_badge ? p->ins_preview_badge : "";
    state.ins_preview = p->ins_preview;
    state.ins_preview_w = p->ins_preview_w;
    state.ins_preview_h = p->ins_preview_h;

    for (int i = 0; i < p->ins_prop_count && i < EVO_RMLUI_BROWSER_PROPS; i++) {
        state.ins_props.emplace_back(
            p->ins_props[i].key ? p->ins_props[i].key : "",
            p->ins_props[i].value ? p->ins_props[i].value : "");
    }

    EvoRmlApp::Instance().UpdateBrowserState(state);
}

void evo_rmlui_render_browser(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderBrowser(framebuffer, width, height);
}

void evo_rmlui_set_version(const char* version) {
    EvoRmlApp::Instance().SetVersion(version ? version : "");
}

void evo_rmlui_update_list(const evo_rmlui_list_params_t* p) {
    if (!p) return;
    EvoListState state;
    state.title = p->title ? p->title : "";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.section = p->section;
    state.rail_focused = (p->rail_focused != 0);
    state.total_count = p->total_count;
    state.cursor_index = p->cursor_index;

    for (int i = 0; i < p->row_count && i < EVO_RMLUI_LIST_ROWS; i++) {
        EvoListRow r;
        r.title = p->rows[i].title ? p->rows[i].title : "";
        r.detail = p->rows[i].detail ? p->rows[i].detail : "";
        r.icon_path = p->rows[i].icon_path ? p->rows[i].icon_path : "";
        r.badge = p->rows[i].badge ? p->rows[i].badge : "";
        r.progress = p->rows[i].progress;
        r.has_chevron = (p->rows[i].has_chevron != 0);
        r.is_focused = (p->rows[i].is_focused != 0);
        state.rows.push_back(r);
    }

    state.is_empty = (p->is_empty != 0);
    state.empty_title = p->empty_title ? p->empty_title : "";
    state.empty_hint = p->empty_hint ? p->empty_hint : "";
    state.empty_icon = p->empty_icon ? p->empty_icon : "";

    for (int i = 0; i < p->hint_count && i < 4; i++) {
        EvoListHint h;
        h.glyph_path = p->hints[i].glyph_path ? p->hints[i].glyph_path : "";
        h.label = p->hints[i].label ? p->hints[i].label : "";
        state.hints.push_back(h);
    }

    EvoRmlApp::Instance().UpdateListState(state);
}

void evo_rmlui_render_list(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderList(framebuffer, width, height);
}

static EvoLaunchTile evo_rmlui_tile_from(const evo_rmlui_launch_tile_t& in) {
    EvoLaunchTile t;
    t.title = in.title ? in.title : "";
    t.detail = in.detail ? in.detail : "";
    t.icon_path = in.icon_path ? in.icon_path : "";
    t.progress = in.progress;
    t.art = in.art;
    t.art_w = in.art_w;
    t.art_h = in.art_h;
    t.is_focused = (in.is_focused != 0);
    return t;
}

void evo_rmlui_update_launch(const evo_rmlui_launch_params_t* p) {
    if (!p) return;
    EvoLaunchState state;
    state.app_name = p->app_name ? p->app_name : "EVO PLAYER";
    state.version = p->version ? p->version : "";
    state.clock = p->clock ? p->clock : "";
    state.theme_name = p->theme_name ? p->theme_name : "";

    state.hero_eyebrow = p->hero_eyebrow ? p->hero_eyebrow : "";
    state.hero_title = p->hero_title ? p->hero_title : "";
    state.hero_detail = p->hero_detail ? p->hero_detail : "";
    state.hero_action = p->hero_action ? p->hero_action : "";
    state.hero_progress = p->hero_progress;
    state.hero_art = p->hero_art;
    state.hero_art_w = p->hero_art_w;
    state.hero_art_h = p->hero_art_h;
    state.hero_focused = (p->hero_focused != 0);

    state.recent_total = p->recent_total;
    state.recent_cursor = p->recent_cursor;
    for (int i = 0; i < p->recent_visible && i < EVO_RMLUI_TILES; i++)
        state.recent.push_back(evo_rmlui_tile_from(p->recent[i]));

    for (int i = 0; i < p->library_visible && i < EVO_RMLUI_TILES; i++)
        state.library.push_back(evo_rmlui_tile_from(p->library[i]));

    EvoRmlApp::Instance().UpdateLaunchState(state);
}

void evo_rmlui_render_launch(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderLaunch(framebuffer, width, height);
}

void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* p) {
    if (!p) return;
    EvoPlaybackState state;
    state.title = p->title ? p->title : "";
    state.meta = p->metadata ? p->metadata : "";
    state.res_badge = p->res_badge ? p->res_badge : "";
    state.hdr_badge = p->hdr_badge ? p->hdr_badge : "";
    state.codec_badge = p->codec_badge ? p->codec_badge : "";
    state.fps_badge = p->fps_badge ? p->fps_badge : "";
    state.audio_badge = p->audio_badge ? p->audio_badge : "";
    state.position_sec = p->position_sec;
    state.duration_sec = p->duration_sec;
    state.percentage = p->percentage;
    state.paused = (p->paused != 0);
    state.scrub_active = (p->scrub_active != 0);
    state.scrub_target = p->scrub_target;
    state.audio_track = p->audio_track ? p->audio_track : "";
    state.sub_track = p->sub_track ? p->sub_track : "";
    state.view_mode = p->view_mode;
    state.show_stats = (p->show_stats != 0);
    state.alpha = p->alpha;

    EvoRmlApp::Instance().UpdatePlaybackState(state);
}

void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderPlaybackOSD(framebuffer, width, height);
}

void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* p) {
    if (!p) return;
    EvoDialogState state;
    state.eyebrow = p->eyebrow ? p->eyebrow : "";
    state.title = p->title ? p->title : "";
    state.detail = p->detail ? p->detail : "";
    state.progress_pct = p->progress_pct;

    for (int i = 0; i < p->action_count && i < 3; i++) {
        EvoDialogAction act;
        act.icon_path = p->actions[i].icon_path ? p->actions[i].icon_path : "../icons/btn_cross.png";
        act.label = p->actions[i].label ? p->actions[i].label : "";
        act.is_primary = (p->actions[i].is_primary != 0);
        state.actions.push_back(act);
    }

    EvoRmlApp::Instance().UpdateDialogState(state);
}

void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderDialog(framebuffer, width, height);
}

void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* p) {
    if (!p) return;
    EvoSettingsState state;
    state.title = p->title ? p->title : "SETTINGS";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.counter = p->counter ? p->counter : "";
    state.rail_active_idx = p->rail_active_idx;
    state.rail_focused = (p->rail_focused != 0);

    for (int i = 0; i < p->row_count && i < 6; i++) {
        EvoSettingsRow row;
        row.title = p->rows[i].title ? p->rows[i].title : "";
        row.detail = p->rows[i].detail ? p->rows[i].detail : "";
        row.icon_path = p->rows[i].icon_path ? p->rows[i].icon_path : "../icons/icon_settings.png";
        row.badge = p->rows[i].badge ? p->rows[i].badge : "";
        row.has_chevron = (p->rows[i].has_chevron != 0);
        row.is_focused = (p->rows[i].is_focused != 0);
        state.rows.push_back(row);
    }

    EvoRmlApp::Instance().UpdateSettingsState(state);
}

void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderSettings(framebuffer, width, height);
}

void evo_rmlui_update_about(const evo_rmlui_about_params_t* p) {
    if (!p) return;
    EvoAboutState state;
    state.app_name = p->app_name ? p->app_name : "EVO PLAYER PRO";
    state.version = p->version ? p->version : "";
    state.build_tag = p->build_tag ? p->build_tag : "PS5 HOMEBREW";
    state.tagline = p->tagline ? p->tagline : "";
    state.themes_info = p->themes_info ? p->themes_info : "";
    state.action_focused = (p->action_focused != 0);

    EvoRmlApp::Instance().UpdateAboutState(state);
}

void evo_rmlui_render_about(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderAbout(framebuffer, width, height);
}

void evo_rmlui_update_subtitles(const evo_rmlui_subtitles_params_t* p) {
    if (!p) return;
    EvoSubtitlesState state;
    state.eyebrow = p->eyebrow ? p->eyebrow : "SUBTITLES & CLOSED CAPTIONS";
    state.title = p->title ? p->title : "SELECT SUBTITLE TRACK";
    state.size_str = p->size_str ? p->size_str : "MEDIUM";
    state.preview_text = p->preview_text ? p->preview_text : "";
    state.preview_face = p->preview_face;

    for (int i = 0; i < p->track_count && i < 6; i++) {
        EvoSubtitlesTrack trk;
        trk.label = p->tracks[i].label ? p->tracks[i].label : "";
        trk.detail = p->tracks[i].detail ? p->tracks[i].detail : "";
        trk.is_current = (p->tracks[i].is_current != 0);
        trk.is_focused = (p->tracks[i].is_focused != 0);
        state.tracks.push_back(trk);
    }

    EvoRmlApp::Instance().UpdateSubtitlesState(state);
}

void evo_rmlui_render_subtitles(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderSubtitles(framebuffer, width, height);
}

void evo_rmlui_update_mediainfo(const evo_rmlui_mediainfo_params_t* p) {
    if (!p) return;
    EvoMediaInfoState state;
    state.title = p->title ? p->title : "";
    state.path = p->path ? p->path : "";
    state.res_badge = p->res_badge ? p->res_badge : "";
    state.hdr_badge = p->hdr_badge ? p->hdr_badge : "";
    state.codec_badge = p->codec_badge ? p->codec_badge : "";
    state.fps_badge = p->fps_badge ? p->fps_badge : "";
    state.container = p->container ? p->container : "";
    state.file_size = p->file_size ? p->file_size : "";
    state.duration = p->duration ? p->duration : "";
    state.video_codec = p->video_codec ? p->video_codec : "";
    state.resolution = p->resolution ? p->resolution : "";
    state.color_hdr = p->color_hdr ? p->color_hdr : "";
    state.audio_codec = p->audio_codec ? p->audio_codec : "";
    state.channels = p->channels ? p->channels : "";
    state.sample_rate = p->sample_rate ? p->sample_rate : "";
    state.subtitles = p->subtitles ? p->subtitles : "";
    state.output = p->output ? p->output : "";
    state.renderer = p->renderer ? p->renderer : "";

    EvoRmlApp::Instance().UpdateMediaInfoState(state);
}

void evo_rmlui_render_mediainfo(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderMediaInfo(framebuffer, width, height);
}

void evo_rmlui_set_theme(const evo_rmlui_theme_t* t) {
    if (!t) return;
    EvoThemeColors colors;
    colors.name = t->name ? t->name : "MIDNIGHT";
    colors.bg_top = t->bg_top;
    colors.bg_bottom = t->bg_bottom;
    colors.surface = t->surface;
    colors.surface_sel = t->surface_sel;
    colors.border = t->border;
    colors.border_sel = t->border_sel;
    colors.accent = t->accent;
    colors.accent_soft = t->accent_soft;
    colors.accent_alt = t->accent_alt;
    colors.text_primary = t->text_primary;
    colors.text_secondary = t->text_secondary;
    colors.text_muted = t->text_muted;
    EvoRmlApp::Instance().SetTheme(colors);
}

void evo_rmlui_update_nav(const evo_rmlui_nav_params_t* p) {
    if (!p) return;
    EvoNavState state;
    state.active_section = p->active_section;
    state.rail_focused   = (p->rail_focused != 0);
    state.cursor_index   = p->cursor_index;
    state.visible        = (p->visible != 0);
    EvoRmlApp::Instance().UpdateNavState(state);
}

int evo_rmlui_agc_geo_active(void) {
    return EvoRmlApp::Instance().AgcGeoActive() ? 1 : 0;
}

int evo_rmlui_agc_geo_present(int vout_handle, uint32_t buf_idx, void* gpu_target,
                              int target_linear, uint32_t out_w, uint32_t out_h,
                              int64_t flip_marker) {
    return EvoRmlApp::Instance().AgcGeoPresent(vout_handle, buf_idx, gpu_target,
                                               target_linear, out_w, out_h, flip_marker);
}

} // extern "C"

