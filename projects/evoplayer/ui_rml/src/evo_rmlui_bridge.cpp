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

void evo_rmlui_update_playback_osd(
    const char* title,
    const char* metadata,
    double position_sec,
    double duration_sec,
    double percentage,
    int paused,
    int scrub_active,
    double scrub_target,
    const char* audio_track,
    const char* sub_track,
    int view_mode,
    int show_stats,
    int alpha
) {
    evo_playback_osd_params_t p;
    p.title = title;
    p.metadata = metadata;
    p.res_badge = "";
    p.hdr_badge = "";
    p.codec_badge = "";
    p.fps_badge = "";
    p.audio_badge = "";
    p.position_sec = position_sec;
    p.duration_sec = duration_sec;
    p.percentage = percentage;
    p.paused = paused;
    p.scrub_active = scrub_active;
    p.scrub_target = scrub_target;
    p.audio_track = audio_track;
    p.sub_track = sub_track;
    p.view_mode = view_mode;
    p.show_stats = show_stats;
    p.alpha = alpha;

    evo_rmlui_update_playback_params(&p);
}

void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderPlaybackOSD(framebuffer, width, height);
}

}
