#include "evo/screens/PlayerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_subtitle.h"
#include "evo_vdec.h"
#include "evo_perf_monitor.h"
#include "evo_toast.h"
#include "evo_feedback.h"
#include "evo_boot_log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sys/time.h>

extern evo_vdec *g_vdec;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern int perf_render_fps;
extern int perf_decode_fps;
extern int detected_audio_rate;
extern int evo_audio_channels;
extern char current_media_path[768];
extern double resume_base_offset_seconds;
void draw_video_frame_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h);
}

#include <cstdio>
#include <cstring>
#include <cmath>

namespace evo {

static uint64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

PlayerScreen::PlayerScreen()
    : StatefulScreen("PlayerScreen")
    , m_playerFsm(PlayerScreenState::NormalPlayback, "PlayerPresenterFSM")
{
    initPlayerStateMachine();
}

void PlayerScreen::initPlayerStateMachine() {
    m_playerFsm
        .addState(PlayerScreenState::NormalPlayback, "NormalPlayback")
        .addState(PlayerScreenState::OsdVisible, "OsdVisible")
        .addState(PlayerScreenState::Scrubbing, "Scrubbing")
        .addState(PlayerScreenState::StatsOverlay, "StatsOverlay");

    m_playerFsm
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::HideOsd, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::Scrubbing, PlayerScreenEvent::EndScrub, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ToggleStats, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible);
}

void PlayerScreen::toggleStatsForNerds() {
    m_showStatsForNerds = !m_showStatsForNerds;
    m_playerFsm.postEvent(PlayerScreenEvent::ToggleStats);
}

/*
 * The clock subtitles are timed against: the raw audio clock when a real audio
 * track is open, otherwise the video clock, plus the resume offset - never the
 * OSD position, which reports the scrub target while the user is scrubbing and
 * would drag every cue to the scrub head instead of the picture on screen.
 */
double PlayerScreen::subtitleClockSeconds() const {
    double clock = (audio_ctx && audio_handle >= 1) ? (double)audio_clock_seconds
                                                    : evo_pb_video_clock_s();
    double pos = resume_base_offset_seconds + clock;
    return (pos < 0.0) ? 0.0 : pos;
}

bool PlayerScreen::hasActiveOverlay() const {
    if (m_osdVisibilityAlpha > 0 || m_showStatsForNerds) {
        return true;
    }
    if (prospero_subtitle_enabled) {
        auto playback = Application::getInstance().getPlaybackController();
        if (playback && !playback->isMusicMode()) {
            double subPos = subtitleClockSeconds() - (static_cast<double>(prospero_subtitle_delay_ms) / 1000.0);
            if (subPos < 0.0) subPos = 0.0;
            if (prospero_subtitle_use_external) {
                const ProsperoSubtitleCue* cue = prospero_subtitle_active_cue(subPos);
                if (cue && cue->text[0]) return true;
            } else {
                char activeSubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
                prospero_embedded_subtitle_text_at(subPos, activeSubText, sizeof(activeSubText));
                if (activeSubText[0]) return true;
            }
        }
    }
    return false;
}

void PlayerScreen::onEnter() {
    StatefulScreen::onEnter();
    m_osdVisibilityAlpha = 255;
    m_controlsLastUsedMs = NowMs();
    m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
}

void PlayerScreen::onExit() {
    StatefulScreen::onExit();
}

bool PlayerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)released;
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback) return false;

    if (pressed) {
        m_controlsLastUsedMs = NowMs();
    }

    // Scrub hold update
    playback->updateScrubHold(held);

    if (pressed & PadButtons::Cross) {
        // Toggle play / pause
        playback->togglePause();
        toast("PLAYBACK", playback->isPaused() ? "PAUSED" : "RESUMED");
        return true;
    }
    if (pressed & PadButtons::Circle) {
        // Stop playback confirmation prompt
        if (screenMgr) {
            screenMgr->navigateTo(ScreenId::ExitConfirm);
        }
        return true;
    }
    if (pressed & PadButtons::Options) {
        toggleStatsForNerds();
        toast("STATS", m_showStatsForNerds ? "ON" : "OFF");
        return true;
    }
    if (pressed & PadButtons::Square) {
        if (screenMgr) {
            screenMgr->navigateTo(ScreenId::MediaInfo);
        }
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        /* Aspect ratio, as it has always been on this screen. */
        playback->cycleViewMode();
        toast("VIEW MODE", playback->getViewMode() == ViewMode::Fit ? "FIT" :
                           playback->getViewMode() == ViewMode::Fill ? "FILL" : "STRETCH");
        return true;
    }
    if (pressed & PadButtons::L1) {
        playback->jumpChapter(-1);
        return true;
    }
    if (pressed & PadButtons::R1) {
        playback->jumpChapter(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        playback->moveScrub(-10.0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        playback->moveScrub(10.0);
        return true;
    }
    /*
     * Up toggles subtitles, Down opens the picker - the legacy binding. The
     * picker rather than a next-track cycle because cycling reopens the file
     * on every step, which on a disc rip with thirty tracks means thirty
     * reopens to reach the one you want.
     */
    if (pressed & PadButtons::Up) {
        if (!playback->isScrubbing()) {
            prospero_subtitle_toggle();
            toast("SUBTITLES", prospero_subtitle_enabled ? "ON" : "OFF");
        }
        return true;
    }
    if (pressed & PadButtons::Down) {
        if (!playback->isScrubbing() && screenMgr) {
            screenMgr->navigateTo(ScreenId::SubtitlePicker);
        }
        return true;
    }
    if (pressed & PadButtons::R2) {
        /* Audio track picker. R2 because L2/R3 are the legacy subtitle-delay
         * nudge and R2 is otherwise unbound on this screen. */
        if (!playback->isScrubbing() && screenMgr) {
            screenMgr->navigateTo(ScreenId::AudioTrackPicker);
        }
        return true;
    }

    return false;
}

void PlayerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_playerFsm.update(deltaMs);

    auto playback = Application::getInstance().getPlaybackController();
    if (!playback) return;

    playback->tickScrubAutoCommit();

    uint64_t now = NowMs();
    bool wantsControls = playback->isMusicMode() ||
                         ((now - m_controlsLastUsedMs < 3500) || playback->isPaused() || m_showStatsForNerds) ||
                         playback->isScrubbing();

    if (playback->isScrubbing()) {
        m_playerFsm.postEvent(PlayerScreenEvent::StartScrub);
    } else if (wantsControls) {
        m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
    } else {
        m_playerFsm.postEvent(PlayerScreenEvent::HideOsd);
    }

    // Smooth OSD fade in/out
    if (wantsControls) {
        if (m_osdVisibilityAlpha < 255) {
            m_osdVisibilityAlpha = std::min(255, m_osdVisibilityAlpha + 42);
        }
    } else {
        if (m_osdVisibilityAlpha > 0) {
            m_osdVisibilityAlpha = std::max(0, m_osdVisibilityAlpha - 28);
        }
    }
}

void PlayerScreen::feedPerformanceHud() {
    static char l_video[192], l_audio[192], l_subs[128];
    static char l_perf[160], l_queues[160], l_clocks[160];

    char vcodec[48] = "NONE", acodec[48] = "NONE", alang[24] = "und";
    int vw = 0, vh = 0, ach = 0, arate = detected_audio_rate;

    if (play_fmt && video_stream_index >= 0 && video_stream_index < (int)play_fmt->nb_streams) {
        AVCodecParameters *cp = play_fmt->streams[video_stream_index]->codecpar;
        if (cp) {
            std::snprintf(vcodec, sizeof(vcodec), "%s", avcodec_get_name(cp->codec_id));
            vw = cp->width;
            vh = cp->height;
        }
    }
    if (play_fmt && audio_stream_index >= 0 && audio_stream_index < (int)play_fmt->nb_streams) {
        AVStream *st = play_fmt->streams[audio_stream_index];
        if (st && st->codecpar) {
            std::snprintf(acodec, sizeof(acodec), "%s", avcodec_get_name(st->codecpar->codec_id));
            ach = st->codecpar->ch_layout.nb_channels;
            arate = st->codecpar->sample_rate;
            const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", nullptr, 0);
            if (lang && lang->value && lang->value[0]) {
                std::snprintf(alang, sizeof(alang), "%s", lang->value);
            }
        }
    }

    const char *backend = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE)
                              ? "Hardware (sceVideodec2)" : "Software (FFmpeg)";

    std::snprintf(l_video, sizeof(l_video), "VIDEO  %s  %dx%d  %.3f fps  (%s)",
                  vcodec, vw, vh, evo_pb_video_fps(), backend);
    std::snprintf(l_audio, sizeof(l_audio), "AUDIO  %s  %s  %dch  %d Hz",
                  acodec, alang, ach, arate);
    std::snprintf(l_subs, sizeof(l_subs), "SUBS  %s  /  %s",
                  prospero_subtitle_enabled ? "ON" : "OFF",
                  prospero_subtitle_use_external ? "EXTERNAL"
                      : (prospero_embedded_subtitle_stream_index >= 0 ? "EMBEDDED" : "NONE"));
    std::snprintf(l_perf, sizeof(l_perf), "RENDER %d fps  /  DECODE %d fps",
                  perf_render_fps, perf_decode_fps);
    std::snprintf(l_queues, sizeof(l_queues), "QUEUES  video %d/96  audio %d/96  pcm %d",
                  packet_queue_count(&video_packet_queue),
                  packet_queue_count(&audio_packet_queue),
                  audio_queue_count);
    std::snprintf(l_clocks, sizeof(l_clocks), "CLOCKS  video %.2f  audio %.2f  delta %.3f",
                  evo_pb_video_clock_s(), audio_clock_seconds,
                  evo_pb_video_clock_s() - audio_clock_seconds);

    evo_perf_snapshot_t s;
    evo_perf_monitor_get(&s);

    evo_perf_hud_t hud;
    std::memset(&hud, 0, sizeof(hud));
    hud.line_video = l_video;
    hud.line_audio = l_audio;
    hud.line_subs = l_subs;
    hud.line_perf = l_perf;
    hud.line_queues = l_queues;
    hud.line_clocks = l_clocks;
    hud.hist_len = EVO_PERF_HIST;
    hud.gpu_hist = s.gpu_hist;
    hud.ram_hist = s.ram_hist;
    hud.cpu_hist = s.cpu_hist;
    hud.gpu_pct = s.gpu_pct;
    hud.gpu_peak_pct = s.gpu_peak_pct;
    hud.ram_mb = s.ram_mb;
    hud.ram_peak_mb = s.ram_peak_mb;
    hud.ram_total_mb = s.ram_total_mb;
    hud.cpu_pct = s.cpu_pct;
    hud.cpu_peak_pct = s.cpu_peak_pct;

    evo_rmlui_update_perf_hud(&hud);
}

void PlayerScreen::render(uint32_t* framebuffer, int width, int height) {
    auto playback = Application::getInstance().getPlaybackController();
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!playback) return;

    static int s_player_screen_log = 5;
    if (s_player_screen_log > 0) {
        s_player_screen_log--;
        evo_boot_log("PlayerScreen::render fb=%p %dx%d alpha=%d rml_init=%d",
                     (void*)framebuffer, width, height, m_osdVisibilityAlpha,
                     evo_rmlui_is_initialized());
        evo_boot_log_flush();
    }

    if (!playback->isMusicMode()) {
        draw_video_frame_to_fb(framebuffer, 0, 0, width, height);
    } else {
        std::fill_n(framebuffer, width * height, 0xFF000000);
    }

    char activeSubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
    if (prospero_subtitle_enabled && !playback->isMusicMode()) {
        double subPos = subtitleClockSeconds() - (static_cast<double>(prospero_subtitle_delay_ms) / 1000.0);
        if (subPos < 0.0) subPos = 0.0;

        if (prospero_subtitle_use_external) {
            const ProsperoSubtitleCue* cue = prospero_subtitle_active_cue(subPos);
            if (cue && cue->text[0]) {
                std::snprintf(activeSubText, sizeof(activeSubText), "%s", cue->text);
            }
        } else {
            prospero_embedded_subtitle_text_at(subPos, activeSubText, sizeof(activeSubText));
        }
    }

    if (m_osdVisibilityAlpha > 0 && evo_rmlui_is_initialized()) {
        std::string titleStr, metaStr;
        if (metaService) {
            metaService->cleanMediaTitle(playback->getCurrentFilePath(), titleStr, metaStr);
        }

        evo_playback_osd_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.title = titleStr.empty() ? "Video Playback" : titleStr.c_str();
        p.metadata = metaStr.c_str();
        /*
         * Derived from the open stream, not assumed. These were hardcoded to
         * "1080p FHD" / "H.264 / AVC" / "60 FPS", so every file reported the
         * same thing whatever it actually was.
         */
        static char resBuf[32], codecBuf[32], fpsBuf[16];
        const char* hdrBadge = "";
        resBuf[0] = codecBuf[0] = fpsBuf[0] = '\0';

        if (play_fmt && video_stream_index >= 0 &&
            video_stream_index < static_cast<int>(play_fmt->nb_streams)) {
            AVCodecParameters* cp = play_fmt->streams[video_stream_index]->codecpar;
            if (cp) {
                int vh = cp->height, vw = cp->width;
                if (vh >= 2000)      std::snprintf(resBuf, sizeof(resBuf), "4K UHD");
                else if (vh >= 1400) std::snprintf(resBuf, sizeof(resBuf), "1440p QHD");
                else if (vh >= 1000) std::snprintf(resBuf, sizeof(resBuf), "1080p FHD");
                else if (vh >= 700)  std::snprintf(resBuf, sizeof(resBuf), "720p HD");
                else if (vh > 0)     std::snprintf(resBuf, sizeof(resBuf), "%dx%d", vw, vh);

                const char* cn = avcodec_get_name(cp->codec_id);
                switch (cp->codec_id) {
                    case AV_CODEC_ID_H264: cn = "H.264 / AVC";  break;
                    case AV_CODEC_ID_HEVC: cn = "H.265 / HEVC"; break;
                    case AV_CODEC_ID_VP9:  cn = "VP9";          break;
                    case AV_CODEC_ID_AV1:  cn = "AV1";          break;
                    default: break;
                }
                std::snprintf(codecBuf, sizeof(codecBuf), "%s", cn);

                if (cp->color_trc == AVCOL_TRC_SMPTE2084)     hdrBadge = "HDR10";
                else if (cp->color_trc == AVCOL_TRC_ARIB_STD_B67) hdrBadge = "HLG";
            }
        }

        double fps = evo_pb_video_fps();
        if (fps > 1.0)
            std::snprintf(fpsBuf, sizeof(fpsBuf), "%d FPS", static_cast<int>(fps + 0.5));

        p.res_badge   = resBuf[0]   ? resBuf   : "";
        p.hdr_badge   = hdrBadge;
        p.codec_badge = codecBuf[0] ? codecBuf : "";
        p.fps_badge   = fpsBuf[0]   ? fpsBuf   : "";
        p.audio_badge = (evo_audio_channels == 8) ? "7.1 CH" : "STEREO";
        p.decoder_badge = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE) ? "Hardware" : "Software";
        p.position_sec = playback->getPositionSeconds();
        p.duration_sec = playback->getDurationSeconds();
        p.percentage = playback->getPercentage();
        p.paused = playback->isPaused() ? 1 : 0;
        p.scrub_active = playback->isScrubbing() ? 1 : 0;
        p.scrub_target = playback->getScrubTargetSeconds();
        p.view_mode = static_cast<int>(playback->getViewMode());
        p.show_stats = m_showStatsForNerds ? 1 : 0;
        p.alpha = m_osdVisibilityAlpha;
        p.subtitle_text = activeSubText[0] ? activeSubText : nullptr;
        p.subtitle_face = prospero_subtitle_face;
        p.subtitle_raised = 1;
        p.chrome_hidden = 0;
        p.fps = perf_render_fps;
        p.music_mode = playback->isMusicMode() ? 1 : 0;
        p.music_codec = "AUDIO";

        evo_rmlui_update_playback_params(&p);
        if (m_showStatsForNerds) {
            feedPerformanceHud();
        }
        evo_rmlui_render_playback_osd(framebuffer, width, height);
    } else if (activeSubText[0] && !playback->isMusicMode() && evo_rmlui_is_initialized()) {
        evo_playback_osd_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.title = "Video Playback";
        p.subtitle_text = activeSubText;
        p.subtitle_face = prospero_subtitle_face;
        p.subtitle_raised = 0;
        p.chrome_hidden = 1;
        p.alpha = 255;
        p.fps = perf_render_fps;

        evo_rmlui_update_playback_params(&p);
        evo_rmlui_render_playback_osd(framebuffer, width, height);
    }
}

} // namespace evo
