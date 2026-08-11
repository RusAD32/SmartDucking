#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>

namespace smart_ducking
{
class SidechainLevelMapper
{
public:
    [[nodiscard]] static float mapLevelDb (float levelDb,
                                           float lowerThresholdDb,
                                           float fullScaleThresholdDb) noexcept
    {
        const auto upper = std::max (lowerThresholdDb + 0.1f, fullScaleThresholdDb);
        return std::clamp ((levelDb - lowerThresholdDb) / (upper - lowerThresholdDb), 0.0f, 1.0f);
    }
};

class SidechainEnvelope
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = std::max (1.0, newSampleRate);
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        envelope = 0.0f;
        holdSamplesRemaining = 0;
    }

    void setTimes (float newAttackMs, float newHoldMs, float newReleaseMs) noexcept
    {
        const auto clampedAttackMs = std::max (0.0f, newAttackMs);
        const auto clampedHoldMs = std::max (0.0f, newHoldMs);
        const auto clampedReleaseMs = std::max (0.0f, newReleaseMs);

        const auto unchanged = [] (float left, float right) noexcept
        {
            return std::bit_cast<std::uint32_t> (left)
                == std::bit_cast<std::uint32_t> (right);
        };

        if (unchanged (clampedAttackMs, attackMs)
            && unchanged (clampedHoldMs, holdMs)
            && unchanged (clampedReleaseMs, releaseMs))
            return;

        attackMs = clampedAttackMs;
        holdMs = clampedHoldMs;
        releaseMs = clampedReleaseMs;
        updateCoefficients();
    }

    [[nodiscard]] float processSample (float detectorValue) noexcept
    {
        const auto target = std::clamp (detectorValue, 0.0f, 1.0f);

        if (target >= envelope)
        {
            envelope = attackCoefficient * envelope + (1.0f - attackCoefficient) * target;
            holdSamplesRemaining = holdSamples;
        }
        else if (holdSamplesRemaining > 0)
        {
            --holdSamplesRemaining;
        }
        else
        {
            envelope = releaseCoefficient * envelope + (1.0f - releaseCoefficient) * target;

            if (envelope < 1.0e-8f)
                envelope = 0.0f;
        }

        return envelope;
    }

    [[nodiscard]] float getCurrentValue() const noexcept { return envelope; }

private:
    static float timeToCoefficient (float timeMs, double currentSampleRate) noexcept
    {
        if (timeMs <= 0.0f)
            return 0.0f;

        const auto timeSamples = 0.001 * static_cast<double> (timeMs) * currentSampleRate;
        return static_cast<float> (std::exp (-1.0 / timeSamples));
    }

    void updateCoefficients() noexcept
    {
        attackCoefficient = timeToCoefficient (attackMs, sampleRate);
        releaseCoefficient = timeToCoefficient (releaseMs, sampleRate);
        holdSamples = static_cast<int> (std::round (0.001 * static_cast<double> (holdMs) * sampleRate));
    }

    double sampleRate = 48000.0;
    float attackMs = 50.0f;
    float holdMs = 150.0f;
    float releaseMs = 200.0f;
    float attackCoefficient = 0.0f;
    float releaseCoefficient = 0.0f;
    float envelope = 0.0f;
    int holdSamples = 0;
    int holdSamplesRemaining = 0;
};
} // namespace smart_ducking
