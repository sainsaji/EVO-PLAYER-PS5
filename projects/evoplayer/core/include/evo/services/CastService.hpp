#ifndef EVO_CAST_SERVICE_HPP
#define EVO_CAST_SERVICE_HPP

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <pthread.h>

namespace evo {

class CastService {
public:
    CastService();
    ~CastService();

    void initialize();
    void shutdown();
    void update(double deltaMs);

    bool isRunning() const;
    int getPort() const;
    const std::string& getDeviceName() const;
    void setDeviceName(const std::string& name);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void onLaunchRequested(const std::string& launchData);
    void onStopRequested();

    /* Play a YouTube link (any form evo_yt_link_video_id accepts) in EVO's
     * player, full length. Main thread. False if the text holds no video id. */
    bool playYouTubeLink(const char* link);

private:
    static void onLaunchThunk(const char* launchData, void* userdata);
    static void onStopThunk(void* userdata);
    static void* resolveThreadMain(void* arg);
    void startResolve(const char* videoId, const char* timeOffset, bool full = false);
    void finishResolve();
    void joinResolve();

    bool m_enabled = true;
    int m_port = 8765;
    std::string m_deviceName = "EVO Player";

    std::mutex m_mutex;
    bool m_pendingLaunch = false;
    std::string m_pendingLaunchData;
    bool m_pendingStop = false;
    bool m_webActive = false;

    /* Browser cast (cast-to-evo-player off): youtube.com/tv in Sony's browser
     * dialog. The pad cannot get back to EVO from there, so EVO closes it when
     * the phone stops or disconnects or the video ends, and shows a hint
     * first (the dialog opens after kBrowserHintMs). */
    void updateBrowserCast(double deltaMs, bool stopped);
    static constexpr double kBrowserHintMs = 3500.0;
    bool m_browserOpenPending = false;
    double m_browserOpenInMs = 0.0;
    std::string m_browserUrl;
    std::string m_browserVideoId;
    std::string m_browserPairingCode;
    std::string m_browserStartTime;
    bool m_browserCast = false;      // EVO opened the dialog for a cast
    int m_browserCloseWanted = 0;    // EVO_CAST_EV_* seen, waiting for the dialog to be up

    /* YouTube stream resolution runs on its own thread: it is several HTTPS
     * round trips and used to block the frame loop for seconds. */
    pthread_t m_resolveThread{};
    bool m_resolveThreadValid = false;
    std::atomic<bool> m_resolveDone{false};
    std::string m_resolveVideoId;
    std::string m_resolveTimeOffset;
    std::string m_resolveUrl;
    std::string m_resolveTitle;
    int m_resolveRc = -1;
    bool m_resolveFull = false;        // a pasted/posted link: full-length route first
    bool m_resolveCancelled = false;   // phone sent stop while resolving
    /* Resolved: the stream is opened a frame later, so the "Opening the
     * video..." toast is drawn before the open blocks the frame loop (~4 s). */
    bool m_startPending = false;
    int m_startDelayFrames = 0;
    void startResolvedPlayback();
};

} // namespace evo

#endif // EVO_CAST_SERVICE_HPP
