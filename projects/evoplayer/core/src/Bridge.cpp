#include "evo_jailbreak.h"
#include "evo/Application.hpp"
#include "evo/Common.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/screens/TextReaderScreen.hpp"
#include "evo/screens/ImageViewerScreen.hpp"
#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_vdec.h"
#include "evo_toast.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_theme.h"
#include "evo_subtitle.h"
#include "evo_rmlui_bridge.h"

namespace evo {
/* Startup default; evo::Application::initialize() replaces these with what
 * the VideoOut layer reports the panel is running at. */
int DisplayWidth  = 1920;
int DisplayHeight = 1080;
}  // namespace evo

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sys/time.h>

// Global playback and display state
pp_playback g_pp_pb;
int g_ps5_user_id = 0;
int screen = 0;
/* Whether the VideoOut surface is registered in an HDR format. Set by main()
 * before the AGC runtime comes up; lived in the deleted GL context stub. */
int g_ps5_video_out_hdr = 0;

int player_paused = 0;
double media_duration_sec = 0.0;
double resume_base_offset_seconds = 0.0;
double requested_resume_seek_pos = 0.0;
char current_media_path[768] = {0};
int evo_audio_channels = 2;

AVFormatContext *play_fmt = nullptr;
AVCodecContext *audio_ctx = nullptr;
evo_vdec *g_vdec = nullptr;
int g_vdec_force_ffmpeg = 0;
AVPacket *video_pending_pkt = nullptr;

int perf_render_fps = 0;
int perf_decode_fps = 0;
int perf_render_frames = 0;
int perf_decode_frames = 0;
double g_gl_present_ms = 0.0;
int show_debug_overlay = 0;

int current_profile = 0;
int playback_profile = 1;
int video_packet_cap = 96;
int audio_packet_cap = 96;
int decoder_thread_count = 4;
int video_view_mode = 1;
long long controls_last_used_ms = 0;

int dbg_read_fail = 0;
int dbg_video_packets = 0;
int dbg_video_frames = 0;
int dbg_video_thread_alive = 0;
int dbg_swaps = 0;
long long dbg_last_pts = 0;
int g_first_frame_bc_done = 0;
struct SwsContext *play_sws = nullptr;

pp_aspect_mode prospero_view_mode_to_aspect(void) {
    if (video_view_mode == 1)
        return PP_ASPECT_FILL;
    if (video_view_mode == 2)
        return PP_ASPECT_STRETCH;
    return PP_ASPECT_FIT;
}

int prospero_get_initial_user_id(void) {
    return g_ps5_user_id;
}


long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000);
}

long long perf_now_ms(void) {
    return now_ms();
}

int str_contains_ci(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    if (!*needle) return 1;
    size_t needle_len = strlen(needle);
    for (; *s; s++) {
        if (strncasecmp(s, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

void format_time_mmss(char *out, size_t out_size, double sec) {
    if (!out || out_size == 0) return;
    if (sec < 0.0) sec = 0.0;
    int s = (int)sec;
    int m = s / 60;
    s %= 60;
    snprintf(out, out_size, "%02d:%02d", m, s);
}

void clean_media_title(const char *path, char *line1, size_t line1_sz, char *line2, size_t line2_sz) {
    if (!path) return;
    std::string title, cat;
    if (auto meta = evo::Application::getInstance().getMediaMetadataService()) {
        meta->cleanMediaTitle(path, title, cat);
    }
    if (line1 && line1_sz > 0) {
        snprintf(line1, line1_sz, "%s", title.c_str());
    }
    if (line2 && line2_sz > 0) {
        snprintf(line2, line2_sz, "%s", cat.c_str());
    }
}


double prospero_player_position(void) {
    return resume_base_offset_seconds + prospero_media_clock_seconds();
}

double evo_player_position_s(void) {
    return prospero_player_position();
}

/*
 * Non-zero on success. Every caller tests `if (!start_video_playback(...))` as
 * "it failed", so returning 0 for success - as this shim did - made the
 * subtitle track switch report TRACK CHANGE FAILED on every successful switch
 * and skip re-enabling subtitles afterwards.
 */
int start_video_playback_at(const char *path, double resume_seconds) {
    if (auto pb = evo::Application::getInstance().getPlaybackController()) {
        return pb->startPlayback(path ? path : "", resume_seconds) ? 1 : 0;
    }
    return 0;
}

int start_video_playback(const char *path) {
    return start_video_playback_at(path, 0.0);
}

void stop_video_playback(void) {
    if (auto pb = evo::Application::getInstance().getPlaybackController()) {
        pb->stopPlayback();
    }
}

/*
 * The dev remote's two entry points (evo_usb_remote.c). Both mirror what a
 * controller does, NOT just the playback call underneath it - the legacy
 * main.c versions pushed the player screen on open and returned to the browser
 * on stop, and the C++ shims that replaced them kept only the
 * start/stop_video_playback() half.
 *
 * The consequence was invisible off-console and total on it: `evo-remote.sh
 * play` opened and demuxed the file (evo_status showed a real dur=) while EVO
 * sat on whatever screen it was already on, so nothing ever appeared, pos
 * never advanced and every clip in a sweep recorded as stalled.
 */
void evo_stop_media_playback(void) {
    stop_video_playback();
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        const evo::ScreenId cur = sm->getCurrentScreenId();
        if (cur == evo::ScreenId::Player || cur == evo::ScreenId::PlaybackFinished)
            sm->navigateTo(evo::ScreenId::UsbBrowser);
    }
}

/*
 * Direct navigation for the dev remote. Driving the UI with synthetic button
 * presses works for anything with a fixed path, but the storage browser's
 * focus model is not observable from outside the app - so "go to Internal
 * Storage" is expressed here as what it means, not as a guess at which keys
 * would achieve it.
 */
void evo_remote_goto_screen(int screen_id) {
    if (auto sm = evo::Application::getInstance().getScreenManager())
        sm->navigateTo(static_cast<evo::ScreenId>(screen_id));
}

void evo_remote_browser_source(int sidebar_index) {
    auto sm = evo::Application::getInstance().getScreenManager();
    if (!sm)
        return;
    sm->navigateTo(evo::ScreenId::UsbBrowser);
    if (auto *bs = dynamic_cast<evo::BrowserScreen *>(
            sm->getScreen(evo::ScreenId::UsbBrowser)))
        bs->setSource(sidebar_index);
}

void evo_remote_open_image(const char *path) {
    auto sm = evo::Application::getInstance().getScreenManager();
    if (!sm || !path || !path[0])
        return;
    if (auto *v = dynamic_cast<evo::ImageViewerScreen *>(
            sm->getScreen(evo::ScreenId::ImageViewer)))
        v->openImage(path);
    sm->navigateTo(evo::ScreenId::ImageViewer);
}

void evo_remote_open_text(const char *path) {
    auto sm = evo::Application::getInstance().getScreenManager();
    if (!sm || !path || !path[0])
        return;
    if (auto *r = dynamic_cast<evo::TextReaderScreen *>(
            sm->getScreen(evo::ScreenId::TextReader)))
        r->openFile(path);
    sm->navigateTo(evo::ScreenId::TextReader);
}

void evo_open_media_path(const char *path) {
    if (!path || !path[0])
        return;
    /* Same order as BrowserScreen::activate() - start, then show. */
    start_video_playback(path);
    if (auto sm = evo::Application::getInstance().getScreenManager())
        sm->navigateTo(evo::ScreenId::Player);
}

void save_resume_position(void) {
    if (auto pb = evo::Application::getInstance().getPlaybackController()) {
        pb->saveResumePosition();
    }
}

double load_resume_position(const char *path) {
    if (auto pb = evo::Application::getInstance().getPlaybackController()) {
        return pb->loadResumePosition(path ? path : "");
    }
    return 0.0;
}

void prospero_settings_save(void) {
    if (auto s = evo::Application::getInstance().getSettingsService()) {
        s->saveSettings();
    }
}

void prospero_settings_load(void) {
    if (auto s = evo::Application::getInstance().getSettingsService()) {
        s->loadSettings();
    }
}

void evo_sync_rmlui_theme(void) {
    if (auto s = evo::Application::getInstance().getSettingsService()) {
        s->syncThemeToRmlUi();
    }
}

void evo_sfx_play(int kind) {
    if (auto sfx = evo::Application::getInstance().getSoundEffectEngine()) {
        sfx->playSound(static_cast<evo::SoundEffect>(kind));
    }
}

/* The DEBUG OVERLAY setting drives both FPS readouts: the player pill during
 * playback and the rail pill everywhere else. It used to drive neither -
 * show_debug_overlay was never assigned and the debug document's update entry
 * point was never called, so the setting was a dead end. */
int evo_fps_counter_enabled(void) {
    if (auto s = evo::Application::getInstance().getSettingsService())
        return s->isDebugOverlayEnabled() ? 1 : 0;
    return 0;
}

void evo_sync_rmlui_nav(int section, int rail_focused, int rail_index, int visible) {
    if (!evo_rmlui_is_initialized()) return;
    evo_rmlui_nav_params_t nav;
    nav.active_section = section;
    nav.rail_focused   = rail_focused;
    nav.cursor_index   = rail_focused ? rail_index : section;
    nav.visible        = visible;
    nav.fps            = perf_render_fps;
    nav.show_fps       = evo_fps_counter_enabled();
    /* Read here rather than threaded through evo_sync_rmlui_nav's signature and
     * every caller: the banner is a property of the process, not of whichever
     * screen happens to be syncing the rail. Stubs to "open" off-device. */
    nav.storage_locked = evo_jailbreak_is_open() ? 0 : 1;
    evo_rmlui_update_nav(&nav);
}

void draw_video_frame_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h) {
    (void)x; (void)y; (void)max_w; (void)max_h;
    // When GL quad is active, drawing to fb is bypassed or handled via GL quad blit
    if (!fb) return;
}

// C Screen router adapters
void draw_menu_linear(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_usb_browser(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_player_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_settings_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_recent_files_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_favorites_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_settings_playback_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_settings_subtitles_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_settings_interface_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_settings_system_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_surround_test_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_developer_tools_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_about_support_screen(uint32_t *fb) {
    draw_developer_tools_screen(fb);
}

void draw_changelog_screen(uint32_t *fb) {
    draw_developer_tools_screen(fb);
}

void draw_resume_prompt(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_media_info_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_text_reader_screen(uint32_t *fb) {
    if (auto sm = evo::Application::getInstance().getScreenManager()) {
        sm->render(fb, evo::DisplayWidth, evo::DisplayHeight);
    }
}

void draw_profile_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_theme_select_screen(uint32_t *fb) {
    draw_settings_screen(fb);
}

void draw_emby_setup_screen(uint32_t *fb) {
    draw_developer_tools_screen(fb);
}

void draw_emby_browse_screen(uint32_t *fb) {
    draw_developer_tools_screen(fb);
}

} // extern "C"
