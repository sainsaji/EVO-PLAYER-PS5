#include "evo/services/SoundEffectEngine.hpp"

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);
}

#include <cmath>
#include <cstring>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace evo {

SoundEffectEngine::SoundEffectEngine() = default;

SoundEffectEngine::~SoundEffectEngine() {
    shutdown();
}

bool SoundEffectEngine::initialize() {
    if (m_running.load()) {
        return true;
    }

    sceAudioOutInit();

    // Open dedicated AudioOut port for UI sound effects (Stereo S16, 48kHz)
    m_audioHandle = sceAudioOutOpen(0xFF, 0, 0, AudioGrain, SampleRate, 1 /* S16_STEREO */);
    if (m_audioHandle < 1) {
        m_audioHandle = -1;
        return false;
    }

    m_running.store(true);
    if (pthread_create(&m_thread, nullptr, WorkerThreadEntry, this) != 0) {
        m_running.store(false);
        sceAudioOutClose(m_audioHandle);
        m_audioHandle = -1;
        return false;
    }

    return true;
}

void SoundEffectEngine::shutdown() {
    if (m_running.load()) {
        m_running.store(false);
        if (m_thread != 0) {
            pthread_join(m_thread, nullptr);
            m_thread = 0;
        }
    }

    if (m_audioHandle >= 1) {
        sceAudioOutClose(m_audioHandle);
        m_audioHandle = -1;
    }
}

void SoundEffectEngine::playSound(SoundEffect effect) {
    if (!m_enabled || m_audioHandle < 1 || effect == SoundEffect::None) {
        return;
    }
    m_pendingEffect.store(static_cast<int>(effect));
}

void* SoundEffectEngine::WorkerThreadEntry(void* arg) {
    auto* self = static_cast<SoundEffectEngine*>(arg);
    self->WorkerLoop();
    return nullptr;
}

void SoundEffectEngine::WorkerLoop() {
    while (m_running.load()) {
        int effect = m_pendingEffect.exchange(0);
        if (effect == 0) {
            usleep(2000);
            continue;
        }

        if (m_audioHandle < 1) {
            continue;
        }

        switch (static_cast<SoundEffect>(effect)) {
            case SoundEffect::Move:
                renderTone(m_audioHandle, 1180.0, 1180.0, 26, 0.30);
                break;
            case SoundEffect::Confirm:
                renderTone(m_audioHandle, 760.0, 1520.0, 70, 0.42);
                break;
            case SoundEffect::Back:
                renderTone(m_audioHandle, 760.0, 380.0, 70, 0.38);
                break;
            case SoundEffect::Toggle:
                renderTone(m_audioHandle, 980.0, 1360.0, 45, 0.36);
                break;
            default:
                break;
        }
    }
}

void SoundEffectEngine::renderTone(int handle, double startFreq, double endFreq, int durationMs, double gain) {
    int totalSamples = (SampleRate * durationMs) / 1000;
    int blocks = (totalSamples + AudioGrain - 1) / AudioGrain;
    int16_t buffer[AudioGrain * 2];
    double phase = 0.0;
    int sampleIndex = 0;

    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < AudioGrain; ++i, ++sampleIndex) {
            double t = static_cast<double>(sampleIndex) / static_cast<double>(totalSamples);
            if (t > 1.0) t = 1.0;

            double freq = startFreq + (endFreq - startFreq) * t;
            double envelope = std::exp(-4.5 * t) * (1.0 - t);

            // 2 ms attack ramp to prevent audible clicks
            int attackSamples = SampleRate / 500;
            if (sampleIndex < attackSamples) {
                envelope *= static_cast<double>(sampleIndex) / static_cast<double>(attackSamples);
            }

            int16_t sample = static_cast<int16_t>(std::sin(phase) * envelope * gain * 26000.0);
            phase += 2.0 * M_PI * freq / static_cast<double>(SampleRate);
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;

            buffer[i * 2 + 0] = sample;
            buffer[i * 2 + 1] = sample;
        }

        if (sceAudioOutOutput(handle, buffer) < 0) {
            return;
        }
    }

    // Output one silent grain to prevent buffer tail repetition
    std::memset(buffer, 0, sizeof(buffer));
    sceAudioOutOutput(handle, buffer);
}

} // namespace evo
