#ifndef EVO_RMLUI_BRIDGE_H
#define EVO_RMLUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize RmlUi Retained Engine */
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
bool evo_rmlui_is_initialized(void);

/* Update and Render Netflix Playback OSD on top of video frame */
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
);

void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_BRIDGE_H */
