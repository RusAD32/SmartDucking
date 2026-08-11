#pragma once

#include <algorithm>
#include <cmath>

#include "LcrDuckMatrix.h"

namespace smart_ducking
{
class LinearFrameSmoother
{
public:
    void reset (float value = 0.0f) noexcept { current = value; }

    [[nodiscard]] float process (float target,
                                 float frameDurationSeconds,
                                 float transitionSeconds) noexcept
    {
        const auto maximumStep = std::clamp (
            frameDurationSeconds / std::max (transitionSeconds, 1.0e-6f),
            0.0f,
            1.0f);
        current += std::clamp (target - current, -maximumStep, maximumStep);
        return current;
    }

    [[nodiscard]] float getCurrentValue() const noexcept { return current; }

private:
    float current = 0.0f;
};

class SpatialControlSmoother
{
public:
    void reset (LcrDuckMatrix::Controls value = {}) noexcept { current = value; }

    [[nodiscard]] LcrDuckMatrix::Controls process (
        LcrDuckMatrix::Controls target,
        float frameDurationSeconds,
        float transitionSeconds) noexcept
    {
        const auto coefficient = 1.0f - std::exp (
            -frameDurationSeconds / std::max (transitionSeconds, 1.0e-6f));

        for (int channel = 0; channel < LcrDuckMatrix::numChannels; ++channel)
        {
            const auto value = current.get (channel);
            current.set (channel,
                         value + coefficient * (target.get (channel) - value));
        }

        return current;
    }

    [[nodiscard]] LcrDuckMatrix::Controls getCurrentValue() const noexcept
    {
        return current;
    }

private:
    LcrDuckMatrix::Controls current;
};
} // namespace smart_ducking
