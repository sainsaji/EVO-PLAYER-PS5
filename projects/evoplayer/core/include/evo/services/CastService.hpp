#ifndef EVO_CAST_SERVICE_HPP
#define EVO_CAST_SERVICE_HPP

#include <string>
#include <memory>
#include <mutex>

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

    bool m_enabled = true;
    int m_port = 8765;
    std::string m_deviceName = "EVO Player";

    std::mutex m_mutex;
    bool m_pendingLaunch = false;
    std::string m_pendingLaunchData;
    bool m_pendingStop = false;
    bool m_webActive = false;
};

} // namespace evo

#endif // EVO_CAST_SERVICE_HPP
