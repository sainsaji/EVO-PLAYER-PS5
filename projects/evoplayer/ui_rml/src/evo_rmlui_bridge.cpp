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
    EvoPlaybackState state;
    state.title = title ? title : "";
    state.meta = metadata ? metadata : "";
    state.position_sec = position_sec;
    state.duration_sec = duration_sec;
    state.percentage = percentage;
    state.paused = (paused != 0);
    state.scrub_active = (scrub_active != 0);
    state.scrub_target = scrub_target;
    state.audio_track = audio_track ? audio_track : "";
    state.sub_track = sub_track ? sub_track : "";
    state.view_mode = view_mode;
    state.show_stats = (show_stats != 0);
    state.alpha = alpha;

    EvoRmlApp::Instance().UpdatePlaybackState(state);
}

void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderPlaybackOSD(framebuffer, width, height);
}

}
