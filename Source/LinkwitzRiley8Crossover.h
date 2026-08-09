#pragma once

#include <array>
#include <cmath>
#include <utility>

#include <juce_audio_basics/juce_audio_basics.h>

namespace smart_ducking
{
class LinkwitzRiley8Crossover
{
public:
    struct Bands
    {
        float low = 0.0f;
        float high = 0.0f;
    };

    void prepare (double newSampleRate, float initialCutoffHz) noexcept
    {
        sampleRate = juce::jmax (1.0, newSampleRate);
        smoothedCutoff.reset (sampleRate, smoothingTimeSeconds);
        smoothedCutoff.setCurrentAndTargetValue (clampCutoff (initialCutoffHz));
        reset();
        updateCoefficients (smoothedCutoff.getCurrentValue());
        coefficientUpdateCountdown = coefficientUpdateIntervalSamples;
        coefficientsDirty = false;
    }

    void reset() noexcept
    {
        for (auto& channel : lowStates)
            for (auto& state : channel)
                state.reset();

        for (auto& channel : highStates)
            for (auto& state : channel)
                state.reset();
    }

    void setCutoffHz (float cutoffHz) noexcept
    {
        const auto clamped = clampCutoff (cutoffHz);

        if (std::abs (clamped - smoothedCutoff.getTargetValue()) > 1.0e-4f)
            smoothedCutoff.setTargetValue (clamped);
    }

    void beginSample() noexcept
    {
        const auto wasSmoothing = smoothedCutoff.isSmoothing();
        const auto cutoff = smoothedCutoff.getNextValue();
        const auto reachedTarget = wasSmoothing && ! smoothedCutoff.isSmoothing();

        if (coefficientsDirty
            || reachedTarget
            || (wasSmoothing && coefficientUpdateCountdown <= 0))
        {
            updateCoefficients (cutoff);
            coefficientUpdateCountdown = coefficientUpdateIntervalSamples;
            coefficientsDirty = false;
        }

        if (coefficientUpdateCountdown > 0)
            --coefficientUpdateCountdown;
    }

    [[nodiscard]] Bands processSample (int channel, float input) noexcept
    {
        jassert (juce::isPositiveAndBelow (channel, numChannels));

        auto low = input;
        auto high = input;

        for (int stage = 0; stage < numStages; ++stage)
        {
            low = lowStates[static_cast<size_t> (channel)][static_cast<size_t> (stage)]
                      .process (low, lowCoefficients[static_cast<size_t> (stage)]);
            high = highStates[static_cast<size_t> (channel)][static_cast<size_t> (stage)]
                       .process (high, highCoefficients[static_cast<size_t> (stage)]);
        }

        return { low, high };
    }

    [[nodiscard]] float getCurrentCutoffHz() const noexcept
    {
        return smoothedCutoff.getCurrentValue();
    }

private:
    struct Coefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct State
    {
        void reset() noexcept
        {
            z1 = 0.0;
            z2 = 0.0;
        }

        [[nodiscard]] float process (float input, const Coefficients& coefficients) noexcept
        {
            const auto output = coefficients.b0 * static_cast<double> (input) + z1;
            z1 = coefficients.b1 * static_cast<double> (input) - coefficients.a1 * output + z2;
            z2 = coefficients.b2 * static_cast<double> (input) - coefficients.a2 * output;
            return static_cast<float> (output);
        }

        double z1 = 0.0;
        double z2 = 0.0;
    };

    [[nodiscard]] float clampCutoff (float cutoffHz) const noexcept
    {
        return juce::jlimit (10.0f,
                             static_cast<float> (sampleRate * 0.45),
                             cutoffHz);
    }

    static std::pair<Coefficients, Coefficients> makeLowAndHighPass (
        double currentSampleRate,
        float cutoffHz,
        double q) noexcept
    {
        const auto omega = juce::MathConstants<double>::twoPi
                         * static_cast<double> (cutoffHz)
                         / currentSampleRate;
        const auto cosine = std::cos (omega);
        const auto sine = std::sin (omega);
        const auto alpha = sine / (2.0 * q);
        const auto inverseA0 = 1.0 / (1.0 + alpha);

        Coefficients low;
        low.b0 = 0.5 * (1.0 - cosine) * inverseA0;
        low.b1 = (1.0 - cosine) * inverseA0;
        low.b2 = low.b0;
        low.a1 = -2.0 * cosine * inverseA0;
        low.a2 = (1.0 - alpha) * inverseA0;

        Coefficients high;
        high.b0 = 0.5 * (1.0 + cosine) * inverseA0;
        high.b1 = -(1.0 + cosine) * inverseA0;
        high.b2 = high.b0;
        high.a1 = low.a1;
        high.a2 = low.a2;

        return { low, high };
    }

    void updateCoefficients (float cutoffHz) noexcept
    {
        for (int stage = 0; stage < numStages; ++stage)
        {
            const auto [low, high] = makeLowAndHighPass (
                sampleRate,
                cutoffHz,
                butterworthQ[static_cast<size_t> (stage)]);
            lowCoefficients[static_cast<size_t> (stage)] = low;
            highCoefficients[static_cast<size_t> (stage)] = high;
        }
    }

    static constexpr int numChannels = 5;
    static constexpr int numStages = 4;
    static constexpr int coefficientUpdateIntervalSamples = 16;
    static constexpr double smoothingTimeSeconds = 0.05;
    static constexpr std::array<double, numStages> butterworthQ {
        0.541196100146197,
        1.306562964876377,
        0.541196100146197,
        1.306562964876377
    };

    double sampleRate = 48000.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedCutoff;
    int coefficientUpdateCountdown = 0;
    bool coefficientsDirty = true;
    std::array<Coefficients, numStages> lowCoefficients;
    std::array<Coefficients, numStages> highCoefficients;
    std::array<std::array<State, numStages>, numChannels> lowStates;
    std::array<std::array<State, numStages>, numChannels> highStates;
};
} // namespace smart_ducking
