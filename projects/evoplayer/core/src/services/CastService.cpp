#include "evo/services/CastService.hpp"
#include "evo/services/SettingsService.hpp"
#include "evo/Application.hpp"
#include "evo/Common.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo_cast_receiver.h"
#include "evo_yt_link.h"
#include "evo_browser_engine.h"
#include "evo_webui.h"
#include "evo_boot_log.h"
#include "evo_toast.h"
#include "evo_usb_remote.h"
#include "evo_hui.h"
#include "evo_thread.h"

#include <cstdlib>

extern "C" {
    int start_video_playback_at(const char *path, double resume_seconds);
    int start_video_playback_cast(const char *url, const char *title, double resume_seconds);
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

    /* Always register the callbacks: Settings can start the receiver later
     * (SettingsService::setCastReceiverEnabled), and a receiver started
     * without them silently ignored every cast until EVO was restarted. */
    evo_cast_receiver_set_callbacks(&CastService::onLaunchThunk, &CastService::onStopThunk, this);
    if (m_enabled) {
        evo_cast_receiver_start(m_deviceName.c_str(), m_port);
    }
}

void CastService::shutdown() {
    evo_cast_receiver_stop();
    joinResolve();
}

void* CastService::resolveThreadMain(void* arg) {
    auto* self = static_cast<CastService*>(arg);
    char streamUrl[4096] = {0};
    char ytTitle[256] = {0};
    int rc = -1;
    if (self->m_resolveFull) {
        rc = evo_yt_resolve_full(self->m_resolveVideoId.c_str(),
                                 streamUrl, sizeof(streamUrl), ytTitle, sizeof(ytTitle));
        if (rc != 0)
            evo_log("cast: full-length resolve failed for %s; trying the 60 s route",
                    self->m_resolveVideoId.c_str());
    }
    if (rc != 0) {
        streamUrl[0] = '\0';
        rc = evo_cast_resolve_youtube(self->m_resolveVideoId.c_str(),
                                      streamUrl, sizeof(streamUrl), ytTitle, sizeof(ytTitle));
    }
    self->m_resolveUrl = streamUrl;
    self->m_resolveTitle = ytTitle;
    self->m_resolveRc = (rc == 0 && streamUrl[0]) ? 0 : -1;
    self->m_resolveDone.store(true);
    return nullptr;
}

void CastService::startResolve(const char* videoId, const char* timeOffset, bool full) {
    m_resolveFull = full;
    m_resolveVideoId = videoId;
    m_resolveTimeOffset = timeOffset ? timeOffset : "";
    m_resolveUrl.clear();
    m_resolveTitle.clear();
    m_resolveRc = -1;
    m_resolveCancelled = false;
    m_resolveDone.store(false);
    if (evo_thread_create(&m_resolveThread, &CastService::resolveThreadMain, this) == 0) {
        m_resolveThreadValid = true;
    } else {
        evo_log("cast: could not start the resolve thread");
        toast("Cast", "Failed to resolve YouTube stream");
    }
}

bool CastService::playYouTubeLink(const char* link) {
    char vid[16];
    if (!link || evo_yt_link_video_id(link, vid, sizeof(vid)) != 0) return false;
    evo_log("cast: link entered -> %s", vid);
    onLaunchRequested(std::string("v=") + vid + "&full=1");
    return true;
}

void CastService::joinResolve() {
    if (m_resolveThreadValid) {
        pthread_join(m_resolveThread, nullptr);
        m_resolveThreadValid = false;
    }
}

/* Main thread, once the resolve thread has finished. */
void CastService::finishResolve() {
    joinResolve();
    const std::string& videoId = m_resolveVideoId;
    if (m_resolveCancelled) {
        evo_log("cast: resolve of %s finished after the phone stopped; dropped", videoId.c_str());
        return;
    }
    if (m_resolveRc == 0) {
        const std::string& title = m_resolveTitle;
        std::string msg = "Playing YouTube: " + (title.empty() ? videoId : title);
        toast("Cast", msg.c_str());
        double resumeSec = m_resolveTimeOffset.empty() ? 0.0 : std::atof(m_resolveTimeOffset.c_str());
        evo_log("cast: Playing native YouTube %s: %s (resume %.1fs)",
                videoId.c_str(), m_resolveUrl.c_str(), resumeSec);
        if (start_video_playback_cast(m_resolveUrl.c_str(), title.c_str(), resumeSec)) {
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::Player);
            }
            if (evo_webui_session_active()) {
                evo_webui_close();
                m_webActive = false;
            }
            return;
        }
        evo_log("cast: start_video_playback_cast failed");
        toast("Cast", "Playback start failed");
    } else {
        evo_log("cast: Native resolve failed for %s", videoId.c_str());
        toast("Cast", "Failed to resolve YouTube stream");
    }
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

    if (m_resolveThreadValid && m_resolveDone.load()) {
        finishResolve();
    }
    const bool resolving = m_resolveThreadValid;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pendingLaunch && !resolving) {
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
        if (m_resolveThreadValid) m_resolveCancelled = true;
        toast("Cast", "Phone disconnected");
        evo_cast_receiver_set_app_running(0);
        evo_webui_close();
        m_webActive = false;
        stop_video_playback();
    }
    updateBrowserCast(deltaMs, doStop);

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
        char full[8] = {0};

        evo_cast_parse_param(launchData.c_str(), "v", videoId, sizeof(videoId));
        evo_cast_parse_param(launchData.c_str(), "url", directUrl, sizeof(directUrl));
        evo_cast_parse_param(launchData.c_str(), "t", timeOffset, sizeof(timeOffset));
        evo_cast_parse_param(launchData.c_str(), "full", full, sizeof(full));

        if (full[0] == '1' && videoId[0]) {
            /* A link posted to /cast/url (or pasted): EVO's own player, full
             * length, whatever the native-player setting says. */
            toast("YouTube", "Resolving the link...");
            startResolve(videoId, timeOffset, true);
        } else if (directUrl[0]) {
            toast("Cast", "Playing cast stream...");
            evo_log("cast: Direct media play: %s", directUrl);
            if (start_video_playback_cast(directUrl, "", 0.0)) {
                if (auto sm = Application::getInstance().getScreenManager())
                    sm->navigateTo(ScreenId::Player);
            }
        } else {
            bool useNative = false;
            auto settings = Application::getInstance().getSettingsService();
            if (settings && settings->isCastNativePlayerEnabled()) {
                useNative = true;
            }

            if (useNative) {
                if (videoId[0]) {
                    toast("Cast", "Resolving YouTube stream...");
                    startResolve(videoId, timeOffset);
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

            /* pairingCode: EVO's own, so it can find the page's Lounge screen
             * and learn when the video ends (evo_cast_page_watch_start). */
            char pairingCode[64] = {0};
            evo_cast_new_pairing_code(pairingCode, sizeof(pairingCode));
            std::string query = launchData.empty() ? "" : (launchData + "&");
            std::string additionalUrl = "http://" + std::string(localIp[0] ? localIp : "127.0.0.1") +
                                       ":" + std::to_string(port) + "/apps/YouTube/dial_data";
            std::string tvUrl = "https://www.youtube.com/tv?" + query + "pairingCode=" + pairingCode +
                                "&theme=cl&additionalDataUrl=" + additionalUrl;

            if (evo_webui_session_active() || m_browserOpenPending) {
                /* The page does not follow EVO's Lounge session, so a second
                 * video from the phone cannot reach it; leave it be. */
                evo_log("cast: browser cast already open, ignoring %s", videoId[0] ? videoId : "(no video)");
                return;
            }
            evo_log("cast: Launching YouTube TV in %.1f s: %s", kBrowserHintMs / 1000.0, tvUrl.c_str());
            toast("YouTube in the browser",
                  "Closes when you stop or disconnect, or the video ends");
            m_browserUrl = tvUrl;
            m_browserVideoId = videoId;
            m_browserPairingCode = pairingCode;
            m_browserStartTime = timeOffset;
            m_browserOpenInMs = kBrowserHintMs;
            m_browserOpenPending = true;
        }
    }
}

void CastService::updateBrowserCast(double deltaMs, bool stopped) {
    const int ev = evo_cast_take_events();

    if (stopped) {
        /* doStop already closed the dialog. */
        if (m_browserOpenPending) evo_log("cast: browser cast cancelled before it opened");
        m_browserOpenPending = false;
        if (m_browserCast) evo_cast_page_watch_stop();
        m_browserCast = false;
        return;
    }

    if (m_browserOpenPending) {
        if (ev & EVO_CAST_EV_PHONE_LEFT) {
            evo_log("cast: phone left before the browser opened");
            m_browserOpenPending = false;
            evo_cast_receiver_set_app_running(0);
            return;
        }
        m_browserOpenInMs -= deltaMs;
        if (m_browserOpenInMs > 0.0) return;
        m_browserOpenPending = false;
        int rc = evo_webui_open_direct(m_browserUrl.c_str());
        evo_log("cast: evo_webui_open_direct returned %d", rc);
        if (rc == 0) {
            m_browserCast = true;
            m_browserCloseWanted = 0;
            evo_cast_page_watch_start(m_browserVideoId.c_str(), m_browserPairingCode.c_str(),
                                      m_browserStartTime.c_str());
        }
        return;
    }

    if (!m_browserCast) return;

    if (!evo_webui_session_active()) {
        /* Closed some other way (Triangle via evo-remote, the browser's own
         * close button). */
        evo_log("cast: browser cast closed");
        evo_cast_page_watch_stop();
        m_browserCast = false;
        return;
    }

    m_browserCloseWanted |= ev & (EVO_CAST_EV_PHONE_LEFT | EVO_CAST_EV_PAGE_ENDED);
    /* evo_webui_close() only acts once the dialog is up (P_RUN); an event
     * while it is still opening waits for that. */
    if (m_browserCloseWanted && evo_webui_active()) {
        const bool ended = (m_browserCloseWanted & EVO_CAST_EV_PAGE_ENDED) != 0;
        m_browserCloseWanted = 0;
        evo_log("cast: closing the browser cast (%s)", ended ? "video ended" : "phone disconnected");
        toast("Cast", ended ? "Video ended" : "Phone disconnected");
        evo_cast_receiver_set_app_running(0);
        evo_webui_close();
        evo_cast_page_watch_stop();
        m_browserCast = false;
    }
}

} // namespace evo
