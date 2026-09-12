#ifndef EVO_SURROUND_TEST_SERVICE_HPP
#define EVO_SURROUND_TEST_SERVICE_HPP

#include "evo/interfaces/ISurroundTestService.hpp"
#include <pthread.h>
#include <atomic>

namespace evo {

class SurroundTestService : public ISurroundTestService {
public:
    SurroundTestService();
    ~SurroundTestService() override;

    bool start() override;
    void stop() override;
    void triggerTone(bool is51Layout, int channelIndex) override;
    bool isActive() const override { return m_active.load(); }
    int getCurrentChannel() const override { return m_currentChannel.load(); }
    bool is51Layout() const override { return m_is51Layout; }
    void set51Layout(bool is51) override { m_is51Layout = is51; }

private:
    static void* AudioThreadEntry(void* arg);
    void AudioLoop();
    void emitTone(int handle, int channelMask, double frequencyHz, int blocks, int grain);

    static constexpr int AudioGrain = 512;
    static constexpr int SampleRate = 48000;
    static constexpr int ChannelCount = 8;

    int m_audioHandle = -1;
    pthread_t m_thread = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    std::atomic<int> m_currentChannel{-1};
    std::atomic<int> m_pendingMask{0};
    bool m_is51Layout = false;
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SERVICE_HPP
