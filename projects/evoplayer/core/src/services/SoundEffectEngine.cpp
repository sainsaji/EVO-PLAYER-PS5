#include "evo/services/SoundEffectEngine.hpp"

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);
}

#include "evo_rmlui_bundle.h"

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

    /* Recorded cues from ps5-homebrew-ui; two focus takes alternate so a held
     * D-pad does not sound like a machine gun. */
    loadSample("hui/sfx/focus_01.wav", &m_move[0]);
    loadSample("hui/sfx/focus_02.wav", &m_move[1]);
    loadSample("hui/sfx/select_01.wav", &m_confirm);
    loadSample("hui/sfx/back_01.wav", &m_back);
    loadSample("hui/sfx/toggle_01.wav", &m_toggle);

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
            case SoundEffect::Move: {
                const std::vector<int16_t>& take = m_move[m_moveAlternate];
                m_moveAlternate ^= 1;
                if (!take.empty()) renderSample(m_audioHandle, take, 0.8);
                else renderTone(m_audioHandle, 1180.0, 1180.0, 26, 0.30);
                break;
            }
            case SoundEffect::Confirm:
                if (!m_confirm.empty()) renderSample(m_audioHandle, m_confirm, 0.9);
                else renderTone(m_audioHandle, 760.0, 1520.0, 70, 0.42);
                break;
            case SoundEffect::Back:
                if (!m_back.empty()) renderSample(m_audioHandle, m_back, 0.9);
                else renderTone(m_audioHandle, 760.0, 380.0, 70, 0.38);
                break;
            case SoundEffect::Toggle:
                if (!m_toggle.empty()) renderSample(m_audioHandle, m_toggle, 0.9);
                else renderTone(m_audioHandle, 980.0, 1360.0, 45, 0.36);
                break;
            default:
                break;
        }
    }
}

/* Minimal RIFF reader for the kit's cues: PCM, 16-bit, mono or stereo, 48 kHz
 * (stereo is folded to mono; the port duplicates it back out). Anything else
 * is refused and that effect keeps its synthesised tone. */
bool SoundEffectEngine::loadSample(const char* key, std::vector<int16_t>* out) {
    out->clear();
    const EvoRmlBundleFile* f = evo_rmlui_bundle_find(key);
    if (!f || f->size < 12 || std::memcmp(f->data, "RIFF", 4) != 0 ||
        std::memcmp(f->data + 8, "WAVE", 4) != 0)
        return false;
    auto u16 = [&](size_t at) { return static_cast<unsigned>(f->data[at] | (f->data[at + 1] << 8)); };
    auto u32 = [&](size_t at) { return static_cast<size_t>(u16(at) | (u16(at + 2) << 16)); };
    unsigned channels = 0, rate = 0, bits = 0, format = 0;
    size_t at = 12;
    while (at + 8 <= f->size) {
        const size_t len = u32(at + 4);
        const size_t body = at + 8;
        if (body + len > f->size) break;
        if (std::memcmp(f->data + at, "fmt ", 4) == 0 && len >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = static_cast<unsigned>(u32(body + 4));
            bits = u16(body + 14);
        } else if (std::memcmp(f->data + at, "data", 4) == 0) {
            if (format != 1 || bits != 16 || rate != SampleRate || (channels != 1 && channels != 2))
                return false;
            const size_t frames = len / (2 * channels);
            out->resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                int sum = 0;
                for (unsigned c = 0; c < channels; ++c)
                    sum += static_cast<int16_t>(u16(body + (i * channels + c) * 2));
                (*out)[i] = static_cast<int16_t>(sum / static_cast<int>(channels));
            }
            return true;
        }
        at = body + len + (len & 1);
    }
    return false;
}

void SoundEffectEngine::renderSample(int handle, const std::vector<int16_t>& samples, double gain) {
    int16_t buffer[AudioGrain * 2];
    for (size_t pos = 0; pos < samples.size(); pos += AudioGrain) {
        for (int i = 0; i < AudioGrain; ++i) {
            const size_t s = pos + static_cast<size_t>(i);
            const int16_t v = s < samples.size()
                ? static_cast<int16_t>(static_cast<double>(samples[s]) * gain) : 0;
            buffer[i * 2 + 0] = v;
            buffer[i * 2 + 1] = v;
        }
        if (sceAudioOutOutput(handle, buffer) < 0)
            return;
    }
    std::memset(buffer, 0, sizeof(buffer));
    sceAudioOutOutput(handle, buffer);
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
