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

}
