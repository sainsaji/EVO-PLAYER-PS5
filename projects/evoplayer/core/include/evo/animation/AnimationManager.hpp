#pragma once

#include "evo/animation/Tween.hpp"
#include <vector>
#include <functional>
#include <algorithm>

namespace evo {
namespace animation {

/**
 * @brief Global and screen-level manager for UI animations, transitions, and frame pacing.
 *
 * Coordinates tween updates and signals the AGC GPU render loop when frames need to be
 * actively rendered at 60 FPS.
 */
class AnimationManager {
private:
    bool m_continuous = false;
    double m_transitionTimerMs = 0.0;
    std::vector<Tween> m_activeTweens;

    AnimationManager() = default;

public:
    static AnimationManager& getInstance() {
        static AnimationManager instance;
        return instance;
    }

    void setContinuousAnimation(bool enabled) {
        m_continuous = enabled;
    }

    bool isContinuousAnimationEnabled() const {
        return m_continuous;
    }

    void triggerTransition(double durationMs = 350.0) {
        if (durationMs > m_transitionTimerMs) {
            m_transitionTimerMs = durationMs;
        }
    }

    bool isTransitionActive() const {
        return m_transitionTimerMs > 0.0;
    }

    void addTween(const Tween& tween) {
        m_activeTweens.push_back(tween);
    }

    void clearTweens() {
        m_activeTweens.clear();
    }

    bool hasActiveAnimations() const {
        if (m_continuous || m_transitionTimerMs > 0.0) {
            return true;
        }
        for (const auto& tw : m_activeTweens) {
            if (tw.isRunning()) return true;
        }
        return false;
    }

    void update(double deltaMs) {
        if (m_transitionTimerMs > 0.0) {
            m_transitionTimerMs -= deltaMs;
            if (m_transitionTimerMs < 0.0) {
                m_transitionTimerMs = 0.0;
            }
        }

        float deltaSec = static_cast<float>(deltaMs / 1000.0);
        for (auto& tw : m_activeTweens) {
            tw.update(deltaSec);
        }

        // Prune completed once-tweens
        m_activeTweens.erase(
            std::remove_if(m_activeTweens.begin(), m_activeTweens.end(),
                           [](const Tween& tw) { return tw.isComplete(); }),
            m_activeTweens.end()
        );
    }
};

} // namespace animation
} // namespace evo
