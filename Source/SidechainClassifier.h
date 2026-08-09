#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace smart_ducking
{
class SidechainClassifier
{
public:
    enum class Mode
    {
        silence,
        leftOnly,
        rightOnly,
        dualMono,
        stereo,
        surround
    };

    struct Result
    {
        Mode mode = Mode::silence;
        Mode instantaneousMode = Mode::silence;
        bool modeChanged = false;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        float dualMonoDifference = 0.0f;
    };

    void prepare (double frameRate) noexcept
    {
        const auto safeFrameRate = std::max (1.0, frameRate);
        fastPathHoldFrames = std::max (1, static_cast<int> (std::ceil (fastPathHoldSeconds
                                                                      * safeFrameRate)));
        reset();
    }

    void reset() noexcept
    {
        currentMode = Mode::silence;
        candidateMode = Mode::silence;
        candidateFrames = 0;
    }

    [[nodiscard]] Result process (const float* left,
                                  const float* right,
                                  int numSamples) noexcept
    {
        auto result = analyse (left, right, numSamples);
        const auto previousMode = currentMode;
        updateStableMode (result.instantaneousMode);
        result.mode = currentMode;
        result.modeChanged = currentMode != previousMode;
        return result;
    }

    [[nodiscard]] Mode getCurrentMode() const noexcept { return currentMode; }

    static constexpr float technicalSilenceFloor = 1.0e-8f;
    static constexpr double fastPathHoldSeconds = 0.100;

private:
    [[nodiscard]] static Result analyse (const float* left,
                                         const float* right,
                                         int numSamples) noexcept
    {
        Result result;

        if (left == nullptr || right == nullptr || numSamples <= 0)
            return result;

        double energyLeft = 0.0;
        double energyRight = 0.0;
        double differenceEnergy = 0.0;
        double crossEnergy = 0.0;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto leftSample = std::isfinite (left[sample]) ? left[sample] : 0.0f;
            const auto rightSample = std::isfinite (right[sample]) ? right[sample] : 0.0f;
            const auto difference = static_cast<double> (leftSample) - rightSample;
            result.peakLeft = std::max (result.peakLeft, std::abs (leftSample));
            result.peakRight = std::max (result.peakRight, std::abs (rightSample));
            energyLeft += static_cast<double> (leftSample) * leftSample;
            energyRight += static_cast<double> (rightSample) * rightSample;
            differenceEnergy += difference * difference;
            crossEnergy += static_cast<double> (leftSample) * rightSample;
        }

        const auto leftIsSilent = result.peakLeft <= technicalSilenceFloor;
        const auto rightIsSilent = result.peakRight <= technicalSilenceFloor;

        if (leftIsSilent && rightIsSilent)
        {
            result.instantaneousMode = Mode::silence;
            return result;
        }

        if (! leftIsSilent && rightIsSilent)
        {
            result.instantaneousMode = Mode::leftOnly;
            return result;
        }

        if (leftIsSilent && ! rightIsSilent)
        {
            result.instantaneousMode = Mode::rightOnly;
            return result;
        }

        constexpr double epsilon = 1.0e-30;
        const auto totalEnergy = energyLeft + energyRight;
        result.dualMonoDifference = static_cast<float> (differenceEnergy / (totalEnergy + epsilon));
        const auto rmsLeft = std::sqrt (energyLeft / static_cast<double> (numSamples));
        const auto rmsRight = std::sqrt (energyRight / static_cast<double> (numSamples));
        const auto levelBalance = std::abs (rmsLeft - rmsRight) / (rmsLeft + rmsRight + epsilon);
        const auto correlation = crossEnergy / std::sqrt (energyLeft * energyRight + epsilon);
        const auto isDualMono = result.dualMonoDifference <= dualMonoDifferenceThreshold
                             && levelBalance <= dualMonoBalanceThreshold
                             && correlation >= dualMonoCorrelationThreshold;
        result.instantaneousMode = isDualMono ? Mode::dualMono : Mode::stereo;
        return result;
    }

    void updateStableMode (Mode instantaneousMode) noexcept
    {
        if (instantaneousMode == currentMode)
        {
            candidateMode = currentMode;
            candidateFrames = 0;
            return;
        }

        // Silence can start a known route immediately. Stereo is also entered
        // immediately so a newly decorrelated or panned voice is never kept in
        // an incorrect fast path.
        if (currentMode == Mode::silence
            || instantaneousMode == Mode::silence
            || instantaneousMode == Mode::stereo)
        {
            currentMode = instantaneousMode;
            candidateMode = currentMode;
            candidateFrames = 0;
            return;
        }

        // A transition between two direct routes first falls back to the safe
        // stereo path, then earns the new fast route after the normal hold.
        if (currentMode != Mode::stereo)
        {
            currentMode = Mode::stereo;
            candidateMode = instantaneousMode;
            candidateFrames = 0;
            return;
        }

        if (candidateMode != instantaneousMode)
        {
            candidateMode = instantaneousMode;
            candidateFrames = 1;
            return;
        }

        if (++candidateFrames >= fastPathHoldFrames)
        {
            currentMode = candidateMode;
            candidateFrames = 0;
        }
    }

    static constexpr float dualMonoDifferenceThreshold = 1.0e-6f;
    static constexpr double dualMonoBalanceThreshold = 0.002;
    static constexpr double dualMonoCorrelationThreshold = 0.9999;

    Mode currentMode = Mode::silence;
    Mode candidateMode = Mode::silence;
    int candidateFrames = 0;
    int fastPathHoldFrames = 1;
};
} // namespace smart_ducking
