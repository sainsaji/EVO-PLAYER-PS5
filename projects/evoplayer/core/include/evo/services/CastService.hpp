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

private:
    static void onLaunchThunk(const char* launchData, void* userdata);
    static void onStopThunk(void* userdata);
    static void* resolveThreadMain(void* arg);
    void startResolve(const char* videoId, const char* timeOffset);
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
    bool m_resolveCancelled = false;   // phone sent stop while resolving
};

} // namespace evo

#endif // EVO_CAST_SERVICE_HPP
