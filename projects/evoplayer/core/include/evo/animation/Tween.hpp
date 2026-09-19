#pragma once

#include "evo/animation/Easing.hpp"
#include <cmath>

namespace evo {
namespace animation {

/**
 * @brief Tween end behaviour matching SharpProspero.Animation.TweenMode.
 */
enum class TweenMode {
    Once,
    Loop,
    PingPong
};

/**
 * @brief Smoothly transitions a float value from start to end along an easing curve.
 *
 * Direct C++ port of SharpProspero.Animation.Tween.
 * Supports Once (clamps at end), Loop (repeats), and PingPong (runs forward and backward indefinitely).
 */
class Tween {
private:
    float m_from = 0.0f;
    float m_to = 1.0f;
    float m_duration = 1.0f;
    float m_elapsed = 0.0f;
    Ease m_ease = Ease::Linear;
    TweenMode m_mode = TweenMode::Once;

    inline float positionPhase() const {
        if (m_duration <= 0.0f) return 1.0f;
        switch (m_mode) {
            case TweenMode::Loop: {
                float rem = std::fmod(m_elapsed, m_duration);
                return rem / m_duration;
            }
            case TweenMode::PingPong: {
                float cycle = std::fmod(m_elapsed / m_duration, 2.0f);
                return (cycle <= 1.0f) ? cycle : (2.0f - cycle);
            }
            case TweenMode::Once:
            default: {
                float clamped = (m_elapsed >= m_duration) ? m_duration : m_elapsed;
                return clamped / m_duration;
            }
        }
    }

public:
    Tween() = default;

    Tween(float from, float to, float durationSeconds, Ease ease = Ease::Linear, TweenMode mode = TweenMode::Once)
        : m_from(from)
        , m_to(to)
        , m_duration((durationSeconds > 0.0001f) ? durationSeconds : 0.0001f)
        , m_elapsed(0.0f)
        , m_ease(ease)
        , m_mode(mode)
    {}

    float getFrom() const { return m_from; }
    float getTo() const { return m_to; }
    Ease getEase() const { return m_ease; }
    TweenMode getMode() const { return m_mode; }
    float getDuration() const { return m_duration; }
    float getElapsed() const { return m_elapsed; }

    float getProgress() const {
        return (m_duration <= 0.0f) ? 1.0f : positionPhase();
    }

    float getValue() const {
        return m_from + ((m_to - m_from) * Easing::Apply(m_ease, getProgress()));
    }

    bool isComplete() const {
        return (m_mode == TweenMode::Once) && (m_elapsed >= m_duration);
    }

    bool isRunning() const {
        return !isComplete();
    }

    float update(float deltaSeconds) {
        if (deltaSeconds > 0.0f) {
            m_elapsed += deltaSeconds;

            if (m_mode == TweenMode::Once) {
                if (m_elapsed > m_duration) {
                    m_elapsed = m_duration;
                }
            } else {
                float period = (m_mode == TweenMode::PingPong) ? (2.0f * m_duration) : m_duration;
                if (m_elapsed >= period) {
                    m_elapsed = std::fmod(m_elapsed, period);
                }
            }
        }
        return getValue();
    }

    void restart() {
        m_elapsed = 0.0f;
    }

    void reset(float from, float to, float durationSeconds, Ease ease = Ease::Linear, TweenMode mode = TweenMode::Once) {
        m_from = from;
        m_to = to;
        m_duration = (durationSeconds > 0.0001f) ? durationSeconds : 0.0001f;
        m_elapsed = 0.0f;
        m_ease = ease;
        m_mode = mode;
    }
};

} // namespace animation
} // namespace evo
