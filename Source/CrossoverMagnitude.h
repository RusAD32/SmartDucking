#pragma once

#include <cmath>

#include <juce_core/juce_core.h>

namespace smart_ducking
{
[[nodiscard]] inline float crossoverHighMagnitude (float frequencyHz,
                                                    float cutoffHz,
                                                    float slopeDbPerOctave) noexcept
{
    if (frequencyHz <= 0.0f)
        return 0.0f;

    const auto cutoff = juce::jmax (10.0f, cutoffHz);
    const auto order = juce::jlimit (
        4, 12, juce::roundToInt (slopeDbPerOctave / 6.0f));
    const auto ratio = frequencyHz / cutoff;
    const auto smallerRatio = ratio >= 1.0f ? 1.0f / ratio : ratio;
    const auto power = std::pow (smallerRatio, static_cast<float> (order));

    return ratio >= 1.0f ? 1.0f / (1.0f + power)
                         : power / (1.0f + power);
}

[[nodiscard]] inline float crossoverLowMagnitude (float frequencyHz,
                                                   float cutoffHz,
                                                   float slopeDbPerOctave) noexcept
{
    return 1.0f - crossoverHighMagnitude (frequencyHz,
                                          cutoffHz,
                                          slopeDbPerOctave);
}
} // namespace smart_ducking
