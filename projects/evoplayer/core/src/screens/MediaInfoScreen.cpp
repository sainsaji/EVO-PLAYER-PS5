#include "evo/screens/MediaInfoScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_recent.h"
#include "evo_favorites.h"

extern "C" {
#include <libavformat/avformat.h>
extern AVFormatContext *play_fmt;
}

#include <cstdio>
#include <cstring>

namespace evo {

MediaInfoScreen::MediaInfoScreen()
    : StatefulScreen("MediaInfoScreen") {
}

void MediaInfoScreen::onEnter() {
    StatefulScreen::onEnter();
}

void MediaInfoScreen::onExit() {
    StatefulScreen::onExit();
}

bool MediaInfoScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Square) {
        // Export compatibility report to USB
        auto metaService = Application::getInstance().getMediaMetadataService();
        auto playback = Application::getInstance().getPlaybackController();
        auto settings = Application::getInstance().getSettingsService();
        if (metaService && playback && settings) {
            auto meta = metaService->extractMetadataFromFormat(play_fmt, playback->getCurrentFilePath());
            metaService->exportCompatibilityReport("/mnt/usb0/evo_compat_report.txt",
                                                   meta,
                                                   playback->getPositionSeconds(),
                                                   settings->getProfile(),
                                                   recent_file_count,
                                                   favorite_count,
                                                   playback->isActive());
        }
        return true;
    }

    if (pressed & (PadButtons::Circle | PadButtons::Cross)) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::Player);
        }
        return true;
    }

    return false;
}

void MediaInfoScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void MediaInfoScreen::render(uint32_t* framebuffer, int width, int height) {
    auto metaService = Application::getInstance().getMediaMetadataService();
    auto playback = Application::getInstance().getPlaybackController();
    if (!metaService || !playback) return;

    auto meta = metaService->extractMetadataFromFormat(play_fmt, playback->getCurrentFilePath());

    evo_rmlui_mediainfo_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = meta.title.c_str();
    params.path = meta.filePath.c_str();
    params.container = meta.container.c_str();
    params.video_codec = meta.videoCodec.c_str();
    params.audio_codec = meta.audioCodec.c_str();

    char resBuf[32];
    std::snprintf(resBuf, sizeof(resBuf), "%d x %d", meta.width, meta.height);
    params.resolution = resBuf;

    std::string durStr = metaService->formatDuration(meta.durationSeconds);
    params.duration = durStr.c_str();

    std::string sizeStr = metaService->formatFileSize(meta.fileSizeBytes);
    params.file_size = sizeStr.c_str();

    params.res_badge = "1080p";
    params.hdr_badge = "";
    params.codec_badge = meta.videoCodec.c_str();
    params.fps_badge = "60 FPS";
    params.color_hdr = "SDR BT.709";
    params.channels = "STEREO";
    params.sample_rate = "48 kHz";
    params.subtitles = "None";
    params.output = "HDMI 1080p60";
    params.renderer = "GL Quad";
    params.decoder = "Hardware (sceVideodec2)";

    evo_rmlui_update_mediainfo(&params);
    evo_rmlui_render_mediainfo(framebuffer, width, height);
}

} // namespace evo
