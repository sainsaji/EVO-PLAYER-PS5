#include "evo/services/SurroundTestService.hpp"

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);
}

#include <cmath>
#include <cstring>
#include <unistd.h>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace evo {

SurroundTestService::SurroundTestService() = default;

SurroundTestService::~SurroundTestService() {
    stop();
}

bool SurroundTestService::start() {
    if (m_running.load()) {
        return true;
    }

    sceAudioOutInit();

    // Open 8-channel AudioOut port (PS5 S16_8CH, param=2)
    m_audioHandle = sceAudioOutOpen(0xFF, 0, 0, AudioGrain, SampleRate, 2 /* S16_8CH */);
    if (m_audioHandle < 1) {
        m_audioHandle = -1;
        return false;
    }

    m_running.store(true);
    if (pthread_create(&m_thread, nullptr, AudioThreadEntry, this) != 0) {
        m_running.store(false);
        sceAudioOutClose(m_audioHandle);
        m_audioHandle = -1;
        return false;
    }

    return true;
}

void SurroundTestService::stop() {
    if (m_running.load()) {
        m_running.store(false);
        m_active.store(false);
        m_currentChannel.store(-1);
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

void SurroundTestService::triggerTone(bool is51Layout, int channelIndex) {
    m_is51Layout = is51Layout;
    m_currentChannel.store(channelIndex);

    if (!m_running.load()) {
        if (!start()) {
            return;
        }
    }

    int mask = 0;
    if (channelIndex >= 0 && channelIndex < 8) {
        mask = (1 << channelIndex);
    } else if (channelIndex == 8) {
        // All channels
        mask = is51Layout ? 0x3F : 0xFF;
    }

    m_pendingMask.store(mask);
    m_active.store(mask != 0);
}

void* SurroundTestService::AudioThreadEntry(void* arg) {
    auto* self = static_cast<SurroundTestService*>(arg);
    self->AudioLoop();
    return nullptr;
}

void SurroundTestService::AudioLoop() {
    while (m_running.load()) {
        int mask = m_pendingMask.exchange(0);
        if (mask == 0) {
            usleep(10000);
            continue;
        }

        if (m_audioHandle < 1) {
            continue;
        }

        double frequency = (mask & (1 << 3)) ? 60.0 : 440.0; // LFE uses 60Hz, others 440Hz
        int blocks = (SampleRate * 1200 / 1000) / AudioGrain; // ~1.2 seconds duration

        emitTone(m_audioHandle, mask, frequency, blocks, AudioGrain);
        m_active.store(false);
    }
}

void SurroundTestService::emitTone(int handle, int channelMask, double frequencyHz, int blocks, int grain) {
    std::vector<int16_t> buffer(grain * ChannelCount, 0);
    double phase = 0.0;
    double phaseInc = 2.0 * M_PI * frequencyHz / static_cast<double>(SampleRate);
    int totalSamples = blocks * grain;
    int currentSample = 0;

    for (int b = 0; b < blocks && m_running.load(); ++b) {
        for (int i = 0; i < grain; ++i, ++currentSample) {
            double envelope = 1.0;
            // 20ms attack ramp
            int attackSamples = SampleRate * 20 / 1000;
            if (currentSample < attackSamples) {
                envelope = static_cast<double>(currentSample) / static_cast<double>(attackSamples);
            }
            // 50ms decay ramp
            int decayStart = totalSamples - (SampleRate * 50 / 1000);
            if (currentSample > decayStart) {
                envelope = static_cast<double>(totalSamples - currentSample) / static_cast<double>(SampleRate * 50 / 1000);
            }

            int16_t sampleValue = static_cast<int16_t>(std::sin(phase) * envelope * 20000.0);
            phase += phaseInc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;

            for (int ch = 0; ch < ChannelCount; ++ch) {
                if (channelMask & (1 << ch)) {
                    buffer[i * ChannelCount + ch] = sampleValue;
                } else {
                    buffer[i * ChannelCount + ch] = 0;
                }
            }
        }

        if (sceAudioOutOutput(handle, buffer.data()) < 0) {
            return;
        }
    }

    // Output silence
    std::fill(buffer.begin(), buffer.end(), 0);
    sceAudioOutOutput(handle, buffer.data());
}

} // namespace evo
