#pragma once

#include <cmath>
#include <algorithm>

namespace evo {
namespace animation {

/**
 * @brief Easing curve types matching SharpProspero.Animation.Ease.
 */
enum class Ease {
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    InSine,
    OutSine,
    InOutSine,
    OutBack,
    OutBounce
};

/**
 * @brief Evaluates easing curves over progress t in [0, 1].
 *
 * Direct C++ port of SharpProspero.Animation.Easing. Zero static init dependencies.
 */
class Easing {
private:
    static constexpr float BackC1 = 1.70158f;
    static constexpr float BackC3 = BackC1 + 1.0f;
    static constexpr float BounceN1 = 7.5625f;
    static constexpr float BounceD1 = 2.75f;
    static constexpr float Pi = 3.14159265358979323846f;

    static inline float Sq(float x) { return x * x; }
    static inline float Cube(float x) { return x * x * x; }

    static inline float OutBounce(float t) {
        if (t < 1.0f / BounceD1) {
            return BounceN1 * t * t;
        }
        if (t < 2.0f / BounceD1) {
            t -= 1.5f / BounceD1;
            return (BounceN1 * t * t) + 0.75f;
        }
        if (t < 2.5f / BounceD1) {
            t -= 2.25f / BounceD1;
            return (BounceN1 * t * t) + 0.9375f;
        }
        t -= 2.625f / BounceD1;
        return (BounceN1 * t * t) + 0.984375f;
    }

public:
    static inline float Apply(Ease ease, float t) {
        t = std::max(0.0f, std::min(1.0f, t));
        switch (ease) {
            case Ease::Linear:
                return t;
            case Ease::InQuad:
                return t * t;
            case Ease::OutQuad:
                return t * (2.0f - t);
            case Ease::InOutQuad:
                return (t < 0.5f) ? (2.0f * t * t) : (1.0f - (Sq(-2.0f * t + 2.0f) / 2.0f));
            case Ease::InCubic:
                return t * t * t;
            case Ease::OutCubic:
                return 1.0f - Cube(1.0f - t);
            case Ease::InOutCubic:
                return (t < 0.5f) ? (4.0f * t * t * t) : (1.0f - (Cube(-2.0f * t + 2.0f) / 2.0f));
            case Ease::InSine:
                return 1.0f - std::cos(t * (Pi / 2.0f));
            case Ease::OutSine:
                return std::sin(t * (Pi / 2.0f));
            case Ease::InOutSine:
                return -(std::cos(Pi * t) - 1.0f) / 2.0f;
            case Ease::OutBack:
                return 1.0f + (BackC3 * Cube(t - 1.0f)) + (BackC1 * Sq(t - 1.0f));
            case Ease::OutBounce:
                return OutBounce(t);
            default:
                return t;
        }
    }

    static inline float Interpolate(float from, float to, float t, Ease ease = Ease::Linear) {
        return from + ((to - from) * Apply(ease, t));
    }
};

} // namespace animation
} // namespace evo
