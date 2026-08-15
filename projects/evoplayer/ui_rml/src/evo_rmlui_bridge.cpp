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
        act.icon_path = p->actions[i].icon_path ? p->actions[i].icon_path : "icons/btn_cross.png";
        act.label = p->actions[i].label ? p->actions[i].label : "";
        act.is_primary = (p->actions[i].is_primary != 0);
        state.actions.push_back(act);
    }

    EvoRmlApp::Instance().UpdateDialogState(state);
}

void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderDialog(framebuffer, width, height);
}

}
