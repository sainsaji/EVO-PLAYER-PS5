#ifndef EVO_SOUND_EFFECT_ENGINE_HPP
#define EVO_SOUND_EFFECT_ENGINE_HPP

#include "evo/interfaces/ISoundEffectEngine.hpp"
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace evo {

class SoundEffectEngine : public ISoundEffectEngine {
public:
    SoundEffectEngine();
    ~SoundEffectEngine() override;

    bool initialize() override;
    void shutdown() override;
    void playSound(SoundEffect effect) override;
    void setEnabled(bool enabled) override { m_enabled = enabled; }
    bool isEnabled() const override { return m_enabled; }

private:
    static void* WorkerThreadEntry(void* arg);
    void WorkerLoop();
    void renderTone(int handle, double startFreq, double endFreq, int durationMs, double gain);
    /* ps5-homebrew-ui's "glass" set (assets/hui/sfx), mono S16 48 kHz.
     * Empty when the bundle lacks one: that effect falls back to its tone. */
    bool loadSample(const char* key, std::vector<int16_t>* out);
    void renderSample(int handle, const std::vector<int16_t>& samples, double gain);

    std::vector<int16_t> m_move[2];
    std::vector<int16_t> m_confirm;
    std::vector<int16_t> m_back;
    std::vector<int16_t> m_toggle;
    int m_moveAlternate = 0;

    static constexpr int AudioGrain = 256;
    static constexpr int SampleRate = 48000;

    int m_audioHandle = -1;
    pthread_t m_thread = 0;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_pendingEffect{0};
    bool m_enabled = true;
};

} // namespace evo

#endif // EVO_SOUND_EFFECT_ENGINE_HPP
