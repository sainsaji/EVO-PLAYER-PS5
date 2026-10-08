/*
 * evo_hui.h - EVO's ps5-homebrew-ui screens, as seen by the RmlUi bridge.
 *
 * The bridge (ui_rml/src/evo_rmlui_bridge.cpp) hands every update to these
 * first. A render call returns 1 when the kit drew the screen into the
 * current AGC frame, 0 when the caller should fall back to the RmlUi screen
 * (kit not built, ui_sdf.pipe missing, fonts failed, or the screen is not
 * ported yet). Main thread only.
 *
 * Kill switch: /mnt/usb0/evo_no_hui keeps every screen on RmlUi.
 */
#ifndef EVO_HUI_H
#define EVO_HUI_H

#include "evo_rmlui_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

void evo_hui_shutdown(void);

/* 1 while a kit screen is up and animating: the device loop must keep
 * drawing frames. */
int  evo_hui_wants_frame(void);
/* Once per presented frame. */
void evo_hui_end_frame(void);

void evo_hui_set_theme(const evo_rmlui_theme_t* theme);
void evo_hui_update_nav(const evo_rmlui_nav_params_t* params);

/* Dev remote (#115): the kit screen on display and its focused item, or NULL
 * when no kit screen drew last frame. `*screen_doc` is the screen's name
 * ("hui_home"); the return value is one JSON object in the shape
 * evo_rmlui_devstate.cpp writes for an RmlUi element. Static storage. */
const char* evo_hui_dev_focus_json(const char** screen_doc);

void evo_hui_update_launch(const evo_rmlui_launch_params_t* params);
int  evo_hui_render_launch(int width, int height);

void evo_hui_update_list(const evo_rmlui_list_params_t* params);
int  evo_hui_render_list(int width, int height);

void evo_hui_update_browser(const evo_rmlui_browser_params_t* params);
int  evo_hui_render_browser(int width, int height);

void evo_hui_update_settings(const evo_rmlui_settings_params_t* params);
int  evo_hui_render_settings(int width, int height);

/* Player OSD. The render returns 0 (keep RmlUi's OSD) for a frame whose
 * captions hold characters the kit's fonts lack. */
void evo_hui_update_playback(const evo_playback_osd_params_t* params);
void evo_hui_update_perf_hud(const evo_perf_hud_t* hud);
int  evo_hui_render_playback_osd(int width, int height);

void evo_hui_update_subtitles(const evo_rmlui_subtitles_params_t* params);
int  evo_hui_render_subtitles(int width, int height);
void evo_hui_update_mediainfo(const evo_rmlui_mediainfo_params_t* params);
int  evo_hui_render_mediainfo(int width, int height);

void evo_hui_update_about(const evo_rmlui_about_params_t* params);
int  evo_hui_render_about(int width, int height);
void evo_hui_update_changelog(const evo_rmlui_changelog_params_t* params);
int  evo_hui_render_changelog(int width, int height);
int  evo_hui_render_closed(int width, int height);

/* Overlays, drawn over whatever screen this frame already has. */
void evo_hui_update_dialog(const evo_rmlui_dialog_params_t* params);
int  evo_hui_render_dialog(int width, int height);
void evo_hui_update_toast(const evo_rmlui_toast_params_t* params);
int  evo_hui_render_toast(int width, int height);

void evo_hui_update_reader(const evo_rmlui_reader_params_t* params);
int  evo_hui_render_reader(int width, int height);
void evo_hui_update_image(const evo_rmlui_image_params_t* params);
int  evo_hui_render_image(int width, int height);
void evo_hui_update_surround(const evo_rmlui_surround_params_t* params);
int  evo_hui_render_surround(int width, int height);
void evo_hui_update_provider(const evo_hui_provider_params_t* params);
int  evo_hui_render_provider(int width, int height);
/* The virtual keyboard: an overlay that draws only while it is visible. */
void evo_hui_update_keyboard(const evo_keyboard_params_t* params);
int  evo_hui_render_keyboard(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* EVO_HUI_H */
