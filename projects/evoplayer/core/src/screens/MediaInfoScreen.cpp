#include "evo/screens/MediaInfoScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_recent.h"
#include "evo_favorites.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include "evo_vdec.h"
#include "evo_playback.h"
#include "evo_agc_runtime.h"
extern AVFormatContext *play_fmt;
extern evo_vdec *g_vdec;
extern int evo_audio_channels;
extern int prospero_subtitle_enabled;
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include <strings.h>

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
                                                   recent_file_count,
                                                   favorite_count,
                                                   playback->isActive());
        }
        return true;
    }

    if (pressed & (PadButtons::Circle | PadButtons::Cross)) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateBack(ScreenId::Player);
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

    char resBadge[32] = "";
    char hdrBadge[32] = "";
    char codecBadge[32] = "";
    char fpsBadge[32] = "";
    char colorHdr[64] = "BT.709 (SDR)";
    char decoderBadge[64] = "Software (FFmpeg)";
    char outputStr[64] = "";
    char channelsStr[64] = "";
    char rateStr[32] = "48 kHz";

    // Resolution badge
    if (meta.width >= 3840 || meta.height >= 2160) {
        std::snprintf(resBadge, sizeof(resBadge), "4K UHD");
    } else if (meta.width >= 1920 || meta.height >= 1080) {
        std::snprintf(resBadge, sizeof(resBadge), "1080p FHD");
    } else if (meta.width >= 1280 || meta.height >= 720) {
        std::snprintf(resBadge, sizeof(resBadge), "720p HD");
    } else if (meta.height > 0) {
        std::snprintf(resBadge, sizeof(resBadge), "%dp", meta.height);
    } else {
        std::snprintf(resBadge, sizeof(resBadge), "1080p");
    }

    // Video stream details from live decoder if active
    if (g_vdec) {
        int vc_trc = evo_vdec_ffmpeg_color_trc(g_vdec);
        int vc_pf = evo_vdec_ffmpeg_pix_fmt(g_vdec);
        bool is_10bit = (vc_pf == AV_PIX_FMT_YUV420P10LE ||
                         vc_pf == AV_PIX_FMT_YUV420P10BE ||
                         vc_pf == AV_PIX_FMT_YUV422P10LE ||
                         vc_pf == AV_PIX_FMT_YUV444P10LE);

        if (vc_trc == AVCOL_TRC_SMPTE2084) {
            std::snprintf(hdrBadge, sizeof(hdrBadge), "HDR10");
            std::snprintf(colorHdr, sizeof(colorHdr), "BT.2020 / ST 2084 (HDR10)");
        } else if (vc_trc == AVCOL_TRC_ARIB_STD_B67) {
            std::snprintf(hdrBadge, sizeof(hdrBadge), "HLG");
            std::snprintf(colorHdr, sizeof(colorHdr), "BT.2020 / ARIB (HLG)");
        } else if (vc_trc == AVCOL_TRC_BT2020_10) {
            std::snprintf(hdrBadge, sizeof(hdrBadge), "HDR");
            std::snprintf(colorHdr, sizeof(colorHdr), "BT.2020 (10-bit)");
        } else {
            std::snprintf(colorHdr, sizeof(colorHdr), is_10bit ? "BT.709 (10-bit SDR)" : "BT.709 (SDR)");
        }

        const char *cname = evo_vdec_ffmpeg_codec_name(g_vdec);
        if (!cname || !cname[0]) {
            cname = avcodec_get_name((enum AVCodecID)evo_vdec_codec_id(g_vdec));
        }
        if (cname && cname[0]) {
            if (strcasecmp(cname, "hevc") == 0 || strcasecmp(cname, "h265") == 0) {
                std::snprintf(codecBadge, sizeof(codecBadge), is_10bit ? "HEVC 10-BIT" : "HEVC");
            } else if (strcasecmp(cname, "h264") == 0 || strcasecmp(cname, "avc") == 0) {
                std::snprintf(codecBadge, sizeof(codecBadge), is_10bit ? "AVC 10-BIT" : "AVC / H.264");
            } else if (strcasecmp(cname, "av1") == 0) {
                std::snprintf(codecBadge, sizeof(codecBadge), is_10bit ? "AV1 10-BIT" : "AV1");
            } else if (strcasecmp(cname, "vp9") == 0) {
                std::snprintf(codecBadge, sizeof(codecBadge), is_10bit ? "VP9 10-BIT" : "VP9");
            } else {
                std::snprintf(codecBadge, sizeof(codecBadge), "%s", cname);
            }
        }
    }

    if (codecBadge[0] == '\0') {
        std::snprintf(codecBadge, sizeof(codecBadge), "%s", meta.videoCodec.empty() ? "Video" : meta.videoCodec.c_str());
    }

    double fps = evo_pb_video_fps();
    if (fps > 1.0) {
        std::snprintf(fpsBadge, sizeof(fpsBadge), "%d FPS", (int)std::round(fps));
    } else {
        std::snprintf(fpsBadge, sizeof(fpsBadge), "60 FPS");
    }

    // Decoder backend
    bool isNativeHw = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE);
    std::snprintf(decoderBadge, sizeof(decoderBadge), "%s",
                  isNativeHw ? "Hardware (sceVideodec2)" : "Software (FFmpeg)");

    // Audio channels
    std::snprintf(channelsStr, sizeof(channelsStr), "%d Channels (%s)",
                  evo_audio_channels,
                  evo_audio_channels == 6 ? "5.1 Surround" : (evo_audio_channels == 8 ? "7.1 Surround" : "Stereo"));

    // Display output status (derived from VideoOut query)
    bool displayHdr = evo_agc_runtime_is_display_hdr() != 0;
    std::snprintf(outputStr, sizeof(outputStr), "%dx%d (%s)",
                  DisplayWidth, DisplayHeight, displayHdr ? "HDR" : "SDR");

    params.res_badge = resBadge;
    params.hdr_badge = hdrBadge;
    params.codec_badge = codecBadge;
    params.fps_badge = fpsBadge;
    params.color_hdr = colorHdr;
    params.channels = channelsStr;
    params.sample_rate = rateStr;
    params.subtitles = prospero_subtitle_enabled ? "Active" : (meta.hasSubtitles ? "Available (Off)" : "None");
    params.output = outputStr;
    params.renderer = "Bare-Metal AGC (RDNA2 Direct)";
    params.decoder = decoderBadge;

    evo_rmlui_update_mediainfo(&params);
    evo_rmlui_render_mediainfo(framebuffer, width, height);
}

} // namespace evo
