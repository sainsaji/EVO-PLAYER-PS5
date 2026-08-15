#ifndef EVO_RMLUI_BRIDGE_H
#define EVO_RMLUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* title;
    const char* metadata;
    const char* res_badge;
    const char* hdr_badge;
    const char* codec_badge;
    const char* fps_badge;
    const char* audio_badge;
    double position_sec;
    double duration_sec;
    double percentage;
    int paused;
    int scrub_active;
    double scrub_target;
    const char* audio_track;
    const char* sub_track;
    int view_mode;
    int show_stats;
    int alpha;
} evo_playback_osd_params_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* detail;
    double progress_pct; // 0.0 to 1.0, or -1.0
    int action_count;
    struct {
        const char* icon_path;
        const char* label;
        int is_primary;
    } actions[3];
} evo_rmlui_dialog_params_t;

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int has_chevron;
    int is_focused;
} evo_rmlui_settings_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    const char* counter;
    int rail_active_idx;
    int rail_focused;
    int row_count;
    evo_rmlui_settings_row_t rows[8];
} evo_rmlui_settings_params_t;

typedef struct {
    const char* label;
    const char* detail;
    int is_current;
    int is_focused;
} evo_rmlui_subtitles_track_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* size_str;
    const char* preview_text;
    int preview_face; // 0=small, 1=medium, 2=large
    int track_count;
    evo_rmlui_subtitles_track_t tracks[8];
} evo_rmlui_subtitles_params_t;

/* Initialize RmlUi Retained Engine */
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
bool evo_rmlui_is_initialized(void);

/* Playback OSD API */
void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* params);
void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height);

/* Confirmation & Modal Dialog API */
void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* params);
void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height);

/* Settings API */
void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* params);
void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height);

/* Subtitles Track Selection Modal API */
void evo_rmlui_update_subtitles(const evo_rmlui_subtitles_params_t* params);
void evo_rmlui_render_subtitles(uint32_t* framebuffer, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_BRIDGE_H */
