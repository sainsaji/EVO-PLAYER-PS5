#include "evo/services/CastService.hpp"
#include "evo/services/SettingsService.hpp"
#include "evo/Application.hpp"
#include "evo/Common.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo_cast_receiver.h"
#include "evo_browser_engine.h"
#include "evo_webui.h"
#include "evo_boot_log.h"
#include "evo_toast.h"
#include "evo_usb_remote.h"
#include "evo_hui.h"

extern "C" {
    int start_video_playback_at(const char *path, double resume_seconds);
    int start_video_playback_source(const char *url, const char *title, double resume_seconds);
    void stop_video_playback(void);
}

namespace evo {

CastService::CastService() = default;

CastService::~CastService() {
    shutdown();
}

void CastService::initialize() {
    auto& app = Application::getInstance();
    auto settings = app.getSettingsService();
    if (settings) {
        m_enabled = settings->isCastReceiverEnabled();
        m_port = settings->getCastPort();
        m_deviceName = settings->getCastDeviceName();
    }

    if (m_enabled) {
        evo_cast_receiver_set_callbacks(&CastService::onLaunchThunk, &CastService::onStopThunk, this);
        evo_cast_receiver_start(m_deviceName.c_str(), m_port);
    }
}

void CastService::shutdown() {
    evo_cast_receiver_stop();
}

bool CastService::isRunning() const {
    return evo_cast_receiver_is_running() != 0;
}

int CastService::getPort() const {
    return evo_cast_receiver_port();
}

const std::string& CastService::getDeviceName() const {
    return m_deviceName;
}

void CastService::setDeviceName(const std::string& name) {
    m_deviceName = name;
    evo_cast_receiver_set_friendly_name(name.c_str());
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->setCastDeviceName(name);
        settings->saveSettings();
    }
}

void CastService::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled) {
        evo_cast_receiver_set_callbacks(&CastService::onLaunchThunk, &CastService::onStopThunk, this);
        evo_cast_receiver_start(m_deviceName.c_str(), m_port);
    } else {
        evo_cast_receiver_stop();
    }
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->setCastReceiverEnabled(enabled);
        settings->saveSettings();
    }
}

void CastService::onLaunchThunk(const char* launchData, void* userdata) {
    if (userdata) {
        auto* self = static_cast<CastService*>(userdata);
        self->onLaunchRequested(launchData ? launchData : "");
    }
}

void CastService::onStopThunk(void* userdata) {
    if (userdata) {
        auto* self = static_cast<CastService*>(userdata);
        self->onStopRequested();
    }
}

void CastService::onLaunchRequested(const std::string& launchData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingLaunch = true;
    m_pendingLaunchData = launchData;
}

void CastService::onStopRequested() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingStop = true;
}

void CastService::update(double deltaMs) {
    (void)deltaMs;
    bool doLaunch = false;
    std::string launchData;
    bool doStop = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pendingLaunch) {
            doLaunch = true;
            launchData = m_pendingLaunchData;
            m_pendingLaunch = false;
            m_pendingLaunchData.clear();
        }
        if (m_pendingStop) {
            doStop = true;
            m_pendingStop = false;
        }
    }

    if (doStop) {
        toast("Cast", "Phone disconnected");
        evo_cast_receiver_set_app_running(0);
        evo_webui_close();
        m_webActive = false;
        stop_video_playback();
    }

    if (m_webActive && !evo_webui_session_active()) {
        m_webActive = false;
        evo_cast_receiver_set_app_running(0);
    } else if (evo_webui_session_active()) {
        m_webActive = true;
    }

    if (doLaunch) {
        char videoId[64] = {0};
        char directUrl[512] = {0};
        char timeOffset[32] = {0};

        evo_cast_parse_param(launchData.c_str(), "v", videoId, sizeof(videoId));
        evo_cast_parse_param(launchData.c_str(), "url", directUrl, sizeof(directUrl));
        evo_cast_parse_param(launchData.c_str(), "t", timeOffset, sizeof(timeOffset));

        if (directUrl[0]) {
            toast("Cast", "Playing cast stream...");
            evo_log("cast: Direct media play: %s", directUrl);
            evo_open_media_path(directUrl);
        } else {
            bool useNative = false;
            auto settings = Application::getInstance().getSettingsService();
            if (settings && settings->isCastNativePlayerEnabled()) {
                useNative = true;
            }

            if (useNative) {
                if (videoId[0]) {
                    toast("Cast", "Resolving YouTube stream...");
                    char streamUrl[4096] = {0};
                    char ytTitle[256] = {0};
                    int rrc = evo_cast_resolve_youtube(videoId, streamUrl, sizeof(streamUrl), ytTitle, sizeof(ytTitle));
                    if (rrc == 0 && streamUrl[0]) {
                        std::string msg = "Playing YouTube: " + std::string(ytTitle[0] ? ytTitle : videoId);
                        toast("Cast", msg.c_str());
                        double resumeSec = 0.0;
                        if (timeOffset[0]) {
                            resumeSec = std::atof(timeOffset);
                        }
                        evo_log("cast: Playing native YouTube %s: %s (resume %.1fs)", videoId, streamUrl, resumeSec);
                        if (start_video_playback_source(streamUrl, ytTitle, resumeSec)) {
                            if (auto sm = Application::getInstance().getScreenManager()) {
                                sm->navigateTo(ScreenId::Player);
                            }
                            if (evo_webui_session_active()) {
                                evo_webui_close();
                                m_webActive = false;
                            }
                            return;
                        }
                        evo_log("cast: start_video_playback_source failed");
                        toast("Cast", "Playback start failed");
                    } else {
                        evo_log("cast: Native resolve failed for %s", videoId);
                        toast("Cast", "Failed to resolve YouTube stream");
                    }
                } else {
                    toast("Cast", "YouTube connected from phone");
                    evo_log("cast: Phone connected, awaiting video selection from Lounge");
                }
                // When native player toggle is enabled, NEVER launch WebKit browser!
                return;
            }

            char localIp[32] = {0};
            evo_cast_receiver_get_local_ip(localIp, sizeof(localIp));
            int port = evo_cast_receiver_port();

            std::string query = launchData.empty() ? "" : (launchData + "&");
            std::string additionalUrl = "http://" + std::string(localIp[0] ? localIp : "127.0.0.1") +
                                       ":" + std::to_string(port) + "/apps/YouTube/dial_data";
            std::string tvUrl = "https://www.youtube.com/tv?" + query + "additionalDataUrl=" + additionalUrl;

            if (videoId[0]) {
                std::string msg = "Casting YouTube: " + std::string(videoId);
                toast("Cast", msg.c_str());
                evo_log("cast: Launching YouTube video %s: %s", videoId, tvUrl.c_str());
            } else {
                toast("Cast", "Casting YouTube from phone...");
                evo_log("cast: Launching YouTube TV: %s", tvUrl.c_str());
            }

            int rc = evo_webui_open_direct(tvUrl.c_str());
            evo_log("cast: evo_webui_open_direct returned %d", rc);
        }
    }
}

} // namespace evo
