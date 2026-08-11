#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "LcrDuckMatrix.h"
#include "SidechainClassifier.h"
#include "SidechainEnvelope.h"

namespace smart_ducking
{
// The L/C/R separation factor is adapted from Will Leskowitz's Apache-2.0
// "upmix" prototype (https://github.com/willleskowitz/upmix). This version is
// substantially modified: it shares Smart Ducking's JUCE STFT, uses
// time-smoothed complex coherence, produces an energy-normalised centre, and
// guarantees identity reconstruction with the matching LCR downmix.
class StereoSpectralLcrProcessor
{
public:
    static constexpr int displayBinCount = 384;

    struct Parameters
    {
        float amountDb = 10.0f;
        float lowFrequencyHz = 150.0f;
        float highFrequencyHz = 20000.0f;
        float weightFrequencyHz = 3000.0f;
        float weightGainDb = 6.0f;
        float weightQ = 0.8f;
        float activityThresholdDb = -30.0f;
        float attackMs = 50.0f;
        float holdMs = 150.0f;
        float releaseMs = 200.0f;
        float duckDepthDb = 0.0f;
        float crossoverHz = 92.0f;
        float crossoverStageMix = 1.0f;
        float widebandStageMix = 1.0f;
        float spectralStageMix = 1.0f;
        bool crossoverDelta = false;
        LcrDuckMatrix duckMatrix;
    };

    struct LcrSample
    {
        float left = 0.0f;
        float centre = 0.0f;
        float right = 0.0f;
        float leftSurround = 0.0f;
        float rightSurround = 0.0f;

        [[nodiscard]] float get (int channel) const noexcept
        {
            switch (channel)
            {
                case 0: return left;
                case 1: return centre;
                case 2: return right;
                case 3: return leftSurround;
                case 4: return rightSurround;
                default: return 0.0f;
            }
        }
    };

    void prepare (double newSampleRate)
    {
        sampleRate = juce::jmax (1.0, newSampleRate);
        baseOrder = sampleRate >= 80000.0 ? 11 : 10;

        for (int mode = 0; mode < numModes; ++mode)
        {
            const auto order = baseOrder + mode;
            ffts[static_cast<size_t> (mode)] = std::make_unique<juce::dsp::FFT> (order);
            const auto size = 1 << order;
            auto& window = windows[static_cast<size_t> (mode)];
            window.resize (static_cast<size_t> (size));
            juce::dsp::WindowingFunction<float>::fillWindowingTables (
                window.data(),
                static_cast<size_t> (size),
                juce::dsp::WindowingFunction<float>::hann,
                false);

            for (auto& value : window)
                value = std::sqrt (juce::jmax (0.0f, value));
        }

        const auto maximumSize = 1 << (baseOrder + numModes - 1);
        const auto maximumBins = maximumSize / 2 + 1;
        const auto maximumHop = maximumSize / economyOverlapFactor;

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            analysisInput[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumSize));
            inputHop[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumHop));
            fftData[static_cast<size_t> (channel)].resize (static_cast<size_t> (2 * maximumSize));
            sidechainAnalysisInput[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumSize));
            sidechainInputHop[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumHop));
            sidechainFftData[static_cast<size_t> (channel)].resize (static_cast<size_t> (2 * maximumSize));
        }

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            magnitudeDb[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumBins));
            processedMagnitudeDb[static_cast<size_t> (channel)].resize (
                static_cast<size_t> (maximumBins));
            rawReductionDb[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumBins));
            temporalReductionDb[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumBins));
            appliedSpectralReductionDb[static_cast<size_t> (channel)].resize (
                static_cast<size_t> (maximumBins));
        }

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            outputHop[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumHop));
            overlapBuffer[static_cast<size_t> (channel)].resize (static_cast<size_t> (maximumSize));
        }

        fftDataCentre.resize (static_cast<size_t> (2 * maximumSize));
        sidechainFftDataCentre.resize (static_cast<size_t> (2 * maximumSize));
        cachedWeightBellDb.resize (static_cast<size_t> (maximumBins));
        cachedEconomyHighWeight.resize (static_cast<size_t> (maximumBins));
        dryWindowGain.resize (static_cast<size_t> (maximumSize));

        lowBandDisplayFft = std::make_unique<juce::dsp::FFT> (lowBandDisplayFftOrder);
        lowBandDisplayWindow.resize (lowBandDisplayFftSize);
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            lowBandDisplayWindow.data(),
            lowBandDisplayFftSize,
            juce::dsp::WindowingFunction<float>::hann,
            false);

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            lowBandDisplayRing[static_cast<size_t> (channel)].resize (lowBandDisplayFftSize);
            lowBandDisplayFftData[static_cast<size_t> (channel)].resize (
                2 * lowBandDisplayFftSize);
            lowBandDisplayMagnitudeDb[static_cast<size_t> (channel)].resize (
                lowBandDisplayFftSize / 2 + 1);
        }

        for (auto& history : pceHistories)
            history.resize (maximumBins);

        for (auto& value : displayInputDb)
            value.store (-90.0f, std::memory_order_relaxed);

        for (auto& value : displayOutputDb)
            value.store (-90.0f, std::memory_order_relaxed);

        for (auto& value : displayReductionDb)
            value.store (0.0f, std::memory_order_relaxed);

        for (auto& value : displaySpectralMaskDb)
            value.store (0.0f, std::memory_order_relaxed);

        displayFrameCounter.store (0, std::memory_order_release);

        setResolutionIndex (1);
    }

    void reset() noexcept
    {
        for (auto& channel : analysisInput)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : sidechainAnalysisInput)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : inputHop)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : sidechainInputHop)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : outputHop)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : overlapBuffer)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : fftData)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : sidechainFftData)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : temporalReductionDb)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : appliedSpectralReductionDb)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : lowBandDisplayRing)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : lowBandDisplayFftData)
            std::fill (channel.begin(), channel.end(), 0.0f);

        for (auto& channel : lowBandDisplayMagnitudeDb)
            std::fill (channel.begin(), channel.end(), -160.0f);

        std::fill (fftDataCentre.begin(), fftDataCentre.end(), 0.0f);
        std::fill (sidechainFftDataCentre.begin(), sidechainFftDataCentre.end(), 0.0f);
        clearPceHistory();
        for (auto& envelope : voiceEnvelopes)
            envelope.reset();
        sidechainClassifier.reset();
        hopPosition = 0;
        lastMaximumReductionDb.fill (0.0f);
        lastWidebandReductionDb.fill (0.0f);
        lastLcrPeaks = {};
        processedFftFrameCount = 0;
        processedOutputFrameCount = 0;
        processedSidechainFftCount = 0;
        processedSidechainPceFrameCount = 0;
        visualisationFramesUntilUpdate.store (0, std::memory_order_relaxed);
        lowBandDisplayAccumulator.fill (0.0f);
        lowBandDisplayDecimationPosition = 0;
        lowBandDisplayWritePosition = 0;
        lowBandDisplaySamplesWritten = 0;
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticSpectralFrameCount.store (0, std::memory_order_relaxed);
        diagnosticSpectralFrameTicks.store (0, std::memory_order_relaxed);
        diagnosticDryVisualisationCount.store (0, std::memory_order_relaxed);
        diagnosticDryVisualisationTicks.store (0, std::memory_order_relaxed);
        diagnosticActiveVisualisationCount.store (0, std::memory_order_relaxed);
        diagnosticActiveVisualisationTicks.store (0, std::memory_order_relaxed);
        diagnosticFrameInputTicks.store (0, std::memory_order_relaxed);
        diagnosticClassificationTicks.store (0, std::memory_order_relaxed);
        diagnosticMainAnalysisTicks.store (0, std::memory_order_relaxed);
        diagnosticSidechainAnalysisTicks.store (0, std::memory_order_relaxed);
        diagnosticControlTicks.store (0, std::memory_order_relaxed);
        diagnosticMaskTicks.store (0, std::memory_order_relaxed);
        diagnosticWideGainTicks.store (0, std::memory_order_relaxed);
        diagnosticInverseTicks.store (0, std::memory_order_relaxed);
        diagnosticOlaTicks.store (0, std::memory_order_relaxed);
#endif
        lastSidechainMode = SidechainClassifier::Mode::silence;
        mainIsStereo = true;
        voiceIsStereo = true;
        heavyPathSleeping = true;
        heavyPathActive.store (false, std::memory_order_release);
        currentCrossoverStageMix = parameters.crossoverStageMix;
        currentWidebandStageMix = parameters.widebandStageMix;
        currentSpectralStageMix = parameters.spectralStageMix;
        currentCrossoverDeltaMix = parameters.crossoverDelta ? 1.0f : 0.0f;
    }

    void setResolutionIndex (int newResolutionIndex) noexcept
    {
        resolutionIndex = juce::jlimit (0, numModes - 1, newResolutionIndex);
        updateProcessingGeometry();
        reset();
    }

    void setEconomyMode (bool shouldUseEconomyMode) noexcept
    {
        if (economyMode == shouldUseEconomyMode)
            return;

        economyMode = shouldUseEconomyMode;
        updateProcessingGeometry();
        reset();
    }

    [[nodiscard]] bool isEconomyMode() const noexcept { return economyMode; }

    void updateProcessingGeometry() noexcept
    {
        fftSize = 1 << (baseOrder + resolutionIndex);
        hopSize = fftSize / (economyMode ? economyOverlapFactor : normalOverlapFactor);
        synthesisNormalisation = economyMode ? 1.0f : 0.5f;
        activeFft = ffts[static_cast<size_t> (resolutionIndex)].get();
        const auto& activeWindow = windows[static_cast<size_t> (resolutionIndex)];

        for (int sample = 0; sample < fftSize; ++sample)
        {
            const auto windowValue = activeWindow[static_cast<size_t> (sample)];
            dryWindowGain[static_cast<size_t> (sample)] =
                windowValue * windowValue * synthesisNormalisation;
        }

        const auto envelopeRate = sampleRate / static_cast<double> (hopSize);
        const auto frameDurationSeconds = static_cast<float> (hopSize / sampleRate);
        coherenceCoefficient = std::exp (-frameDurationSeconds / coherenceTimeSeconds);
        maskShapeAttackCoefficient = std::exp (
            -frameDurationSeconds / maskShapeAttackSeconds);
        maskShapeReleaseCoefficient = std::exp (
            -frameDurationSeconds / maskShapeReleaseSeconds);
        weightingCurveDirty = true;
        economyCrossoverCurveDirty = true;

        auto mainWindowSum = 0.0f;
        for (const auto value : windows[static_cast<size_t> (resolutionIndex)])
            mainWindowSum += value;

        auto lowBandWindowSum = 0.0f;
        for (const auto value : lowBandDisplayWindow)
            lowBandWindowSum += value;

        lowBandToMainFftScaleDb = juce::Decibels::gainToDecibels (
            mainWindowSum / juce::jmax (epsilon, lowBandWindowSum), -160.0f);
        lowBandAbsoluteFftOffsetDb = -juce::Decibels::gainToDecibels (
            0.5f * lowBandWindowSum, -160.0f);

        for (auto& envelope : voiceEnvelopes)
            envelope.prepare (envelopeRate);

        sidechainClassifier.prepare (envelopeRate);
    }

    void setParameters (Parameters newParameters) noexcept
    {
        const auto envelopeTimesChanged = newParameters.attackMs != parameters.attackMs
                                       || newParameters.holdMs != parameters.holdMs
                                       || newParameters.releaseMs != parameters.releaseMs;

        if (newParameters.weightFrequencyHz != parameters.weightFrequencyHz
            || newParameters.weightGainDb != parameters.weightGainDb
            || newParameters.weightQ != parameters.weightQ)
            weightingCurveDirty = true;

        if (newParameters.crossoverHz != parameters.crossoverHz)
            economyCrossoverCurveDirty = true;

        parameters = newParameters;

        if (envelopeTimesChanged)
            for (auto& envelope : voiceEnvelopes)
                envelope.setTimes (parameters.attackMs,
                                   parameters.holdMs,
                                   parameters.releaseMs);
    }

    [[nodiscard]] LcrSample processSample (float inputLeft,
                                           float inputRight,
                                           float sidechainLeft,
                                           float sidechainRight) noexcept
    {
        return processSample ({ inputLeft, 0.0f, inputRight, 0.0f, 0.0f },
                              { sidechainLeft, 0.0f, sidechainRight, 0.0f, 0.0f },
                              true,
                              true,
                              LcrSample {});
    }

    [[nodiscard]] LcrSample processSample (LcrSample input,
                                           LcrSample sidechain,
                                           bool newMainIsStereo,
                                           bool newVoiceIsStereo,
                                           LcrSample lowBandForVisualisation) noexcept
    {
        if (newMainIsStereo != mainIsStereo)
        {
            pceHistories[mainStreamIndex].clear();

            if (newMainIsStereo)
            {
                clearInactiveStereoChannels (analysisInput, inputHop);
                std::fill (overlapBuffer[leftSurroundIndex].begin(),
                           overlapBuffer[leftSurroundIndex].end(),
                           0.0f);
                std::fill (overlapBuffer[rightSurroundIndex].begin(),
                           overlapBuffer[rightSurroundIndex].end(),
                           0.0f);
                std::fill (outputHop[leftSurroundIndex].begin(),
                           outputHop[leftSurroundIndex].end(),
                           0.0f);
                std::fill (outputHop[rightSurroundIndex].begin(),
                           outputHop[rightSurroundIndex].end(),
                           0.0f);
            }
        }

        if (newVoiceIsStereo != voiceIsStereo)
        {
            pceHistories[sidechainStreamIndex].clear();
            sidechainClassifier.reset();
            lastSidechainMode = SidechainClassifier::Mode::silence;

            if (newVoiceIsStereo)
                clearInactiveStereoChannels (sidechainAnalysisInput, sidechainInputHop);
        }

        const LcrSample output {
            outputHop[leftIndex][static_cast<size_t> (hopPosition)],
            outputHop[centreIndex][static_cast<size_t> (hopPosition)],
            outputHop[rightIndex][static_cast<size_t> (hopPosition)],
            outputHop[leftSurroundIndex][static_cast<size_t> (hopPosition)],
            outputHop[rightSurroundIndex][static_cast<size_t> (hopPosition)]
        };

        writeHopSample (inputHop, input, newMainIsStereo);
        writeHopSample (sidechainInputHop, sidechain, newVoiceIsStereo);
        pushLowBandVisualisationSample (lowBandForVisualisation, newMainIsStereo);

        mainIsStereo = newMainIsStereo;
        voiceIsStereo = newVoiceIsStereo;

        if (++hopPosition >= hopSize)
        {
            hopPosition = 0;
            processFrame();
        }

        return output;
    }

    [[nodiscard]] int getLatencySamples() const noexcept { return fftSize; }
    [[nodiscard]] int getMaximumFftSize() const noexcept { return 1 << (baseOrder + numModes - 1); }
    [[nodiscard]] int getResolutionIndex() const noexcept { return resolutionIndex; }
    [[nodiscard]] float getLastMaximumReductionDb (int channel) const noexcept
    {
        return lastMaximumReductionDb[static_cast<size_t> (juce::jlimit (0, lcrChannels - 1, channel))];
    }
    [[nodiscard]] float getLastWidebandReductionDb (int channel) const noexcept
    {
        return lastWidebandReductionDb[static_cast<size_t> (juce::jlimit (0, lcrChannels - 1, channel))];
    }
    [[nodiscard]] LcrSample getLastLcrPeaks() const noexcept { return lastLcrPeaks; }
    [[nodiscard]] std::uint64_t getProcessedFftFrameCount() const noexcept
    {
        return processedFftFrameCount;
    }
    [[nodiscard]] std::uint64_t getProcessedOutputFrameCount() const noexcept
    {
        return processedOutputFrameCount;
    }
    [[nodiscard]] std::uint64_t getProcessedSidechainFftCount() const noexcept
    {
        return processedSidechainFftCount;
    }
    [[nodiscard]] std::uint64_t getProcessedSidechainPceFrameCount() const noexcept
    {
        return processedSidechainPceFrameCount;
    }
    [[nodiscard]] SidechainClassifier::Mode getSidechainMode() const noexcept
    {
        return lastSidechainMode;
    }

    [[nodiscard]] bool isHeavyPathActive() const noexcept
    {
        return heavyPathActive.load (std::memory_order_acquire);
    }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    [[nodiscard]] std::uint64_t getDiagnosticSpectralFrameCount() const noexcept
    {
        return diagnosticSpectralFrameCount.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticSpectralFrameTicks() const noexcept
    {
        return diagnosticSpectralFrameTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticDryVisualisationCount() const noexcept
    {
        return diagnosticDryVisualisationCount.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticDryVisualisationTicks() const noexcept
    {
        return diagnosticDryVisualisationTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticActiveVisualisationCount() const noexcept
    {
        return diagnosticActiveVisualisationCount.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticActiveVisualisationTicks() const noexcept
    {
        return diagnosticActiveVisualisationTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticFrameInputTicks() const noexcept
    {
        return diagnosticFrameInputTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticClassificationTicks() const noexcept
    {
        return diagnosticClassificationTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticMainAnalysisTicks() const noexcept
    {
        return diagnosticMainAnalysisTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticSidechainAnalysisTicks() const noexcept
    {
        return diagnosticSidechainAnalysisTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticControlTicks() const noexcept
    {
        return diagnosticControlTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticMaskTicks() const noexcept
    {
        return diagnosticMaskTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticWideGainTicks() const noexcept
    {
        return diagnosticWideGainTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticInverseTicks() const noexcept
    {
        return diagnosticInverseTicks.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDiagnosticOlaTicks() const noexcept
    {
        return diagnosticOlaTicks.load (std::memory_order_relaxed);
    }
#endif

    void setVisualisationEnabled (bool shouldBeEnabled) noexcept
    {
        visualisationEnabled.store (shouldBeEnabled, std::memory_order_release);
        visualisationFramesUntilUpdate.store (0, std::memory_order_relaxed);
    }

    void setVisualisationFrameRate (float framesPerSecond) noexcept
    {
        visualisationFrameRate.store (juce::jlimit (1.0f, 120.0f, framesPerSecond),
                                      std::memory_order_relaxed);
        visualisationFramesUntilUpdate.store (0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isVisualisationEnabled() const noexcept
    {
        return visualisationEnabled.load (std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t copyDisplayFrame (float* inputDbDestination,
                                                  float* outputDbDestination,
                                                  float* reductionDbDestination,
                                                  float* spectralMaskDbDestination,
                                                  int maximumBins) const noexcept
    {
        if (inputDbDestination == nullptr || outputDbDestination == nullptr
            || reductionDbDestination == nullptr || spectralMaskDbDestination == nullptr
            || maximumBins <= 0)
            return 0;

        const auto binsToCopy = juce::jmin (maximumBins, displayBinCount);

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = displayFrameCounter.load (std::memory_order_acquire);

            if ((before & 1u) != 0u)
                continue;

            for (int bin = 0; bin < binsToCopy; ++bin)
            {
                const auto index = static_cast<size_t> (bin);
                inputDbDestination[bin] = displayInputDb[index].load (
                    std::memory_order_relaxed);
                outputDbDestination[bin] = displayOutputDb[index].load (
                    std::memory_order_relaxed);
                reductionDbDestination[bin] = displayReductionDb[index].load (
                    std::memory_order_relaxed);
                spectralMaskDbDestination[bin] = displaySpectralMaskDb[index].load (
                    std::memory_order_relaxed);
            }

            const auto after = displayFrameCounter.load (std::memory_order_acquire);

            if (before == after)
                return after / 2u;
        }

        return 0;
    }

    static constexpr float centreDownmixGain = 0.7071067811865475f;

private:
    static void clearInactiveStereoChannels (
        std::array<std::vector<float>, 5>& history,
        std::array<std::vector<float>, 5>& hop) noexcept
    {
        constexpr std::array<size_t, 3> inactiveChannels {
            centreIndex, leftSurroundIndex, rightSurroundIndex
        };

        for (const auto channel : inactiveChannels)
        {
            std::fill (history[channel].begin(), history[channel].end(), 0.0f);
            std::fill (hop[channel].begin(), hop[channel].end(), 0.0f);
        }
    }

    void writeHopSample (std::array<std::vector<float>, 5>& destination,
                         const LcrSample& sample,
                         bool stereo) noexcept
    {
        const auto sanitise = [] (float value) noexcept
        {
            return std::isfinite (value) ? value : 0.0f;
        };
        const auto position = static_cast<size_t> (hopPosition);
        destination[leftIndex][position] = sanitise (sample.left);
        destination[rightIndex][position] = sanitise (sample.right);

        if (! stereo)
        {
            destination[centreIndex][position] = sanitise (sample.centre);
            destination[leftSurroundIndex][position] = sanitise (sample.leftSurround);
            destination[rightSurroundIndex][position] = sanitise (sample.rightSurround);
        }
    }

    void pushLowBandVisualisationSample (const LcrSample& sample, bool stereo) noexcept
    {
        if (economyMode || ! visualisationEnabled.load (std::memory_order_relaxed))
            return;

        lowBandDisplayAccumulator[leftIndex] += sample.left;
        lowBandDisplayAccumulator[rightIndex] += sample.right;

        if (! stereo)
        {
            lowBandDisplayAccumulator[centreIndex] += sample.centre;
            lowBandDisplayAccumulator[leftSurroundIndex] += sample.leftSurround;
            lowBandDisplayAccumulator[rightSurroundIndex] += sample.rightSurround;
        }

        if (++lowBandDisplayDecimationPosition < lowBandDisplayDecimationFactor)
            return;

        const auto inverseDecimation = 1.0f
                                     / static_cast<float> (lowBandDisplayDecimationFactor);
        const auto writePosition = static_cast<size_t> (lowBandDisplayWritePosition);

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            const auto channelIsActive = ! stereo
                                      || channel == static_cast<int> (leftIndex)
                                      || channel == static_cast<int> (rightIndex);
            lowBandDisplayRing[static_cast<size_t> (channel)][writePosition] =
                channelIsActive
                    ? lowBandDisplayAccumulator[static_cast<size_t> (channel)]
                          * inverseDecimation
                    : 0.0f;
        }

        lowBandDisplayAccumulator.fill (0.0f);
        lowBandDisplayDecimationPosition = 0;
        lowBandDisplayWritePosition = (lowBandDisplayWritePosition + 1)
                                    % lowBandDisplayFftSize;
        lowBandDisplaySamplesWritten = juce::jmin (lowBandDisplayFftSize,
                                                   lowBandDisplaySamplesWritten + 1);
    }

    void shiftAnalysisFrame (
        std::array<std::vector<float>, 5>& history,
        const std::array<std::vector<float>, 5>& hop,
        int retainedSamples,
        bool stereo) noexcept
    {
        const auto shiftChannel = [&] (size_t channel)
        {
            auto& input = history[channel];
            std::memmove (input.data(),
                          input.data() + hopSize,
                          static_cast<size_t> (retainedSamples) * sizeof (float));
            std::copy_n (hop[channel].begin(), hopSize, input.begin() + retainedSamples);
        };

        shiftChannel (leftIndex);
        shiftChannel (rightIndex);

        if (! stereo)
        {
            shiftChannel (centreIndex);
            shiftChannel (leftSurroundIndex);
            shiftChannel (rightSurroundIndex);
        }
    }

    struct PceHistory
    {
        void resize (int bins)
        {
            crossReal.resize (static_cast<size_t> (bins));
            crossImaginary.resize (static_cast<size_t> (bins));
            powerLeft.resize (static_cast<size_t> (bins));
            powerRight.resize (static_cast<size_t> (bins));
            rawCentreFactor.resize (static_cast<size_t> (bins));
        }

        void clear() noexcept
        {
            std::fill (crossReal.begin(), crossReal.end(), 0.0f);
            std::fill (crossImaginary.begin(), crossImaginary.end(), 0.0f);
            std::fill (powerLeft.begin(), powerLeft.end(), 0.0f);
            std::fill (powerRight.begin(), powerRight.end(), 0.0f);
        }

        std::vector<float> crossReal;
        std::vector<float> crossImaginary;
        std::vector<float> powerLeft;
        std::vector<float> powerRight;
        std::vector<float> rawCentreFactor;
    };

    [[nodiscard]] bool shouldUpdateVisualisationThisFrame() noexcept
    {
        if (! visualisationEnabled.load (std::memory_order_acquire))
            return false;

        const auto framesRemaining = visualisationFramesUntilUpdate.load (
            std::memory_order_relaxed);

        if (framesRemaining > 0)
        {
            visualisationFramesUntilUpdate.store (framesRemaining - 1,
                                                   std::memory_order_relaxed);
            return false;
        }

        const auto sourceFrameRate = static_cast<float> (sampleRate)
                                   / static_cast<float> (juce::jmax (1, hopSize));
        const auto targetFrameRate = visualisationFrameRate.load (std::memory_order_relaxed);
        const auto frameInterval = juce::jmax (
            1,
            static_cast<int> (std::ceil (sourceFrameRate / targetFrameRate)));
        visualisationFramesUntilUpdate.store (frameInterval - 1,
                                               std::memory_order_relaxed);
        return true;
    }

    void processFrame() noexcept
    {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        const auto diagnosticFrameStart = juce::Time::getHighResolutionTicks();
        auto diagnosticStageStart = diagnosticFrameStart;
#endif
        const auto retainedSamples = fftSize - hopSize;
        const auto smoothingStep = juce::jlimit (
            0.0f,
            1.0f,
            static_cast<float> (hopSize / sampleRate) / stageTransitionSeconds);
        const auto advanceStageMix = [smoothingStep] (float current, float target) noexcept
        {
            const auto difference = juce::jlimit (-smoothingStep,
                                                  smoothingStep,
                                                  target - current);
            return current + difference;
        };
        currentCrossoverStageMix = advanceStageMix (
            currentCrossoverStageMix,
            juce::jlimit (0.0f, 1.0f, parameters.crossoverStageMix));
        currentWidebandStageMix = advanceStageMix (
            currentWidebandStageMix,
            juce::jlimit (0.0f, 1.0f, parameters.widebandStageMix));
        currentSpectralStageMix = advanceStageMix (
            currentSpectralStageMix,
            juce::jlimit (0.0f, 1.0f, parameters.spectralStageMix));
        currentCrossoverDeltaMix = advanceStageMix (
            currentCrossoverDeltaMix,
            parameters.crossoverDelta ? 1.0f : 0.0f);

        shiftAnalysisFrame (analysisInput, inputHop, retainedSamples, mainIsStereo);
        shiftAnalysisFrame (sidechainAnalysisInput,
                            sidechainInputHop,
                            retainedSamples,
                            voiceIsStereo);

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        auto diagnosticStageEnd = juce::Time::getHighResolutionTicks();
        diagnosticFrameInputTicks.fetch_add (
            static_cast<std::uint64_t> (diagnosticStageEnd - diagnosticStageStart),
            std::memory_order_relaxed);
        diagnosticStageStart = diagnosticStageEnd;
#endif

        SidechainClassifier::Result classification;
        float maximumSidechainMagnitude = 0.0f;
        std::array<float, lcrChannels> sidechainChannelPeaks {};

        if (voiceIsStereo)
        {
            classification = sidechainClassifier.process (
                sidechainAnalysisInput[leftIndex].data(),
                sidechainAnalysisInput[rightIndex].data(),
                fftSize);
            maximumSidechainMagnitude = juce::jmax (classification.peakLeft,
                                                     classification.peakRight);

            if (classification.mode != lastSidechainMode)
                pceHistories[sidechainStreamIndex].clear();

            lastSidechainMode = classification.mode;
        }
        else
        {
            for (int channel = 0; channel < lcrChannels; ++channel)
                for (int sample = 0; sample < fftSize; ++sample)
                {
                    const auto magnitude = std::abs (
                        sidechainAnalysisInput[static_cast<size_t> (channel)]
                                              [static_cast<size_t> (sample)]);
                    sidechainChannelPeaks[static_cast<size_t> (channel)] = juce::jmax (
                        sidechainChannelPeaks[static_cast<size_t> (channel)], magnitude);
                }

            maximumSidechainMagnitude = *std::max_element (sidechainChannelPeaks.begin(),
                                                            sidechainChannelPeaks.end());

            const auto surroundMode = maximumSidechainMagnitude
                                            > SidechainClassifier::technicalSilenceFloor
                ? SidechainClassifier::Mode::surround
                : SidechainClassifier::Mode::silence;

            classification.mode = surroundMode;
            classification.instantaneousMode = surroundMode;

            if (surroundMode != lastSidechainMode)
                pceHistories[sidechainStreamIndex].clear();

            lastSidechainMode = surroundMode;
        }

        const auto sidechainSignalIsPresent = maximumSidechainMagnitude
                                            > SidechainClassifier::technicalSilenceFloor;
        const auto envelopeIsActive = std::any_of (voiceEnvelopes.begin(), voiceEnvelopes.end(), [] (const auto& envelope)
        {
            return envelope.getCurrentValue() > sleepControlThreshold;
        });
        const auto spectralReductionIsFinished = std::all_of (
            lastMaximumReductionDb.begin(),
            lastMaximumReductionDb.end(),
            [] (float reduction) { return reduction < sleepReductionThresholdDb; });
        const auto& window = windows[static_cast<size_t> (resolutionIndex)];
        const auto shouldUpdateVisualisation = shouldUpdateVisualisationThisFrame();

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticStageEnd = juce::Time::getHighResolutionTicks();
        diagnosticClassificationTicks.fetch_add (
            static_cast<std::uint64_t> (diagnosticStageEnd - diagnosticStageStart),
            std::memory_order_relaxed);
#endif

        const auto crossoverDeltaNeedsFft = economyMode
                                         && (parameters.crossoverDelta
                                             || currentCrossoverDeltaMix > 0.0f);
        const auto duckingStagesNeedFft = parameters.widebandStageMix > 0.0f
                                       || parameters.spectralStageMix > 0.0f
                                       || currentWidebandStageMix > 0.0f
                                       || currentSpectralStageMix > 0.0f;

        if ((! sidechainSignalIsPresent && ! envelopeIsActive
             && spectralReductionIsFinished && ! crossoverDeltaNeedsFft)
            || (! duckingStagesNeedFft && ! crossoverDeltaNeedsFft))
        {
            if (shouldUpdateVisualisation)
            {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
                const auto diagnosticDisplayStart = juce::Time::getHighResolutionTicks();
#endif
                updateDryDisplayFrame (window);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
                diagnosticDryVisualisationCount.fetch_add (1, std::memory_order_relaxed);
                diagnosticDryVisualisationTicks.fetch_add (
                    static_cast<std::uint64_t> (juce::Time::getHighResolutionTicks()
                                                - diagnosticDisplayStart),
                    std::memory_order_relaxed);
#endif
            }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            const auto diagnosticDryOlaStart = juce::Time::getHighResolutionTicks();
#endif
            addDryFrame (mainIsStereo);

            if (! heavyPathSleeping)
            {
                for (auto& channel : temporalReductionDb)
                    std::fill (channel.begin(), channel.begin() + fftSize / 2 + 1, 0.0f);

                lastMaximumReductionDb.fill (0.0f);
                lastWidebandReductionDb.fill (0.0f);
                clearPceHistory();
                heavyPathSleeping = true;
                heavyPathActive.store (false, std::memory_order_release);
            }

            finishOverlapAddFrame (retainedSamples);
            ++processedOutputFrameCount;
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticOlaTicks.fetch_add (
                static_cast<std::uint64_t> (juce::Time::getHighResolutionTicks()
                                            - diagnosticDryOlaStart),
                std::memory_order_relaxed);
            diagnosticSpectralFrameCount.fetch_add (1, std::memory_order_relaxed);
            diagnosticSpectralFrameTicks.fetch_add (
                static_cast<std::uint64_t> (juce::Time::getHighResolutionTicks()
                                            - diagnosticFrameStart),
                std::memory_order_relaxed);
#endif
            return;
        }

        const auto isWakingFromSleep = heavyPathSleeping;
        heavyPathSleeping = false;
        heavyPathActive.store (true, std::memory_order_release);

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticStageStart = juce::Time::getHighResolutionTicks();
#endif
        if (mainIsStereo)
        {
            performMainForwardFft (leftIndex, window);
            performMainForwardFft (rightIndex, window);
            calculateLcrSpectra (fftData[leftIndex],
                                 fftData[rightIndex],
                                 fftDataCentre,
                                 pceHistories[mainStreamIndex]);
        }
        else
        {
            for (int channel = 0; channel < lcrChannels; ++channel)
                performMainForwardFft (static_cast<size_t> (channel), window);
        }

        ++processedFftFrameCount;

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticStageEnd = juce::Time::getHighResolutionTicks();
        diagnosticMainAnalysisTicks.fetch_add (
            static_cast<std::uint64_t> (diagnosticStageEnd - diagnosticStageStart),
            std::memory_order_relaxed);
        diagnosticStageStart = diagnosticStageEnd;
#endif

        LcrDuckMatrix::Controls detectorTargets;

        if (sidechainSignalIsPresent)
        {
            switch (classification.mode)
            {
                case SidechainClassifier::Mode::leftOnly:
                    performSidechainForwardFft (leftIndex, window);
                    detectorTargets.left = calculateSidechainDetector (sidechainFftData[leftIndex]);
                    break;

                case SidechainClassifier::Mode::rightOnly:
                    performSidechainForwardFft (rightIndex, window);
                    detectorTargets.right = calculateSidechainDetector (
                        sidechainFftData[rightIndex]);
                    break;

                case SidechainClassifier::Mode::dualMono:
                    performSidechainForwardFft (leftIndex, window);
                    detectorTargets.centre = calculateSidechainDetector (
                        sidechainFftData[leftIndex],
                        1.0f / centreDownmixGain);
                    break;

                case SidechainClassifier::Mode::stereo:
                    performSidechainForwardFft (leftIndex, window);
                    performSidechainForwardFft (rightIndex, window);
                    calculateLcrSpectra (sidechainFftData[leftIndex],
                                         sidechainFftData[rightIndex],
                                         sidechainFftDataCentre,
                                         pceHistories[sidechainStreamIndex]);
                    ++processedSidechainPceFrameCount;
                    detectorTargets = {
                        calculateSidechainDetector (sidechainFftData[leftIndex]),
                        calculateSidechainDetector (sidechainFftDataCentre),
                        calculateSidechainDetector (sidechainFftData[rightIndex])
                    };
                    break;

                case SidechainClassifier::Mode::surround:
                    for (int channel = 0; channel < lcrChannels; ++channel)
                    {
                        if (sidechainChannelPeaks[static_cast<size_t> (channel)]
                            <= SidechainClassifier::technicalSilenceFloor)
                            continue;

                        performSidechainForwardFft (static_cast<size_t> (channel), window);
                        detectorTargets.set (
                            channel,
                            calculateSidechainDetector (
                                sidechainFftData[static_cast<size_t> (channel)]));
                    }
                    break;

                case SidechainClassifier::Mode::silence:
                    break;
            }
        }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticStageEnd = juce::Time::getHighResolutionTicks();
        diagnosticSidechainAnalysisTicks.fetch_add (
            static_cast<std::uint64_t> (diagnosticStageEnd - diagnosticStageStart),
            std::memory_order_relaxed);
        diagnosticStageStart = diagnosticStageEnd;
#endif

        LcrDuckMatrix::Controls voiceControls;

        for (int channel = 0; channel < lcrChannels; ++channel)
            voiceControls.set (
                channel,
                voiceEnvelopes[static_cast<size_t> (channel)].processSample (
                    detectorTargets.get (channel)));

        const auto targetControls = parameters.duckMatrix.apply (voiceControls);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticStageEnd = juce::Time::getHighResolutionTicks();
        diagnosticControlTicks.fetch_add (
            static_cast<std::uint64_t> (diagnosticStageEnd - diagnosticStageStart),
            std::memory_order_relaxed);
        std::uint64_t diagnosticActiveDisplayTicks = 0;
        std::uint64_t diagnosticMaskStageTicks = 0;
        std::uint64_t diagnosticWideGainStageTicks = 0;
        std::uint64_t diagnosticInverseStageTicks = 0;
#endif

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            const auto channelIsActive = ! mainIsStereo || channel <= static_cast<int> (rightIndex);

            if (! channelIsActive)
            {
                lastMaximumReductionDb[static_cast<size_t> (channel)] = 0.0f;
                lastWidebandReductionDb[static_cast<size_t> (channel)] = 0.0f;
                std::fill (appliedSpectralReductionDb[static_cast<size_t> (channel)].begin(),
                           appliedSpectralReductionDb[static_cast<size_t> (channel)].begin()
                               + fftSize / 2 + 1,
                           0.0f);
                continue;
            }

            auto& transform = mainIsStereo && channel == static_cast<int> (centreIndex)
                ? fftDataCentre
                : fftData[static_cast<size_t> (channel)];
            const auto frameControl = voiceControls.get (channel);
            lastWidebandReductionDb[static_cast<size_t> (channel)] =
                parameters.duckDepthDb * targetControls.get (channel)
                    * currentWidebandStageMix;
            const auto combineNormalSpectralAndWideGain = ! economyMode
                && parameters.amountDb > 0.0f
                && frameControl > 0.0f;
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticStageStart = juce::Time::getHighResolutionTicks();
#endif
            updateSpectralMask (channel,
                                transform,
                                frameControl,
                                currentSpectralStageMix,
                                isWakingFromSleep,
                                shouldUpdateVisualisation,
                                combineNormalSpectralAndWideGain,
                                lastWidebandReductionDb[static_cast<size_t> (channel)]);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticStageEnd = juce::Time::getHighResolutionTicks();
            diagnosticMaskStageTicks += static_cast<std::uint64_t> (
                diagnosticStageEnd - diagnosticStageStart);
            diagnosticStageStart = diagnosticStageEnd;
#endif
            if (economyMode)
            {
                if (lastMaximumReductionDb[static_cast<size_t> (channel)] > 0.0f
                    || lastWidebandReductionDb[static_cast<size_t> (channel)] > 0.0f
                    || currentCrossoverDeltaMix > 0.0f)
                    applyEconomyCrossoverGain (
                        channel,
                        transform,
                        lastWidebandReductionDb[static_cast<size_t> (channel)],
                        shouldUpdateVisualisation);
            }
            else if (! combineNormalSpectralAndWideGain
                     && lastWidebandReductionDb[static_cast<size_t> (channel)] > 0.0f)
                applyWidebandGain (transform,
                                   lastWidebandReductionDb[static_cast<size_t> (channel)]);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticStageEnd = juce::Time::getHighResolutionTicks();
            diagnosticWideGainStageTicks += static_cast<std::uint64_t> (
                diagnosticStageEnd - diagnosticStageStart);
#endif

            if (shouldUpdateVisualisation)
            {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
                const auto diagnosticCaptureStart = juce::Time::getHighResolutionTicks();
#endif
                const auto channelHasReduction =
                    lastMaximumReductionDb[static_cast<size_t> (channel)] > 0.0f
                    || lastWidebandReductionDb[static_cast<size_t> (channel)] > 0.0f;

                if (channelHasReduction)
                {
                    captureMagnitudeDb (transform,
                                        processedMagnitudeDb[static_cast<size_t> (channel)]);
                }
                else
                {
                    std::copy_n (magnitudeDb[static_cast<size_t> (channel)].begin(),
                                 fftSize / 2 + 1,
                                 processedMagnitudeDb[static_cast<size_t> (channel)].begin());
                }
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
                diagnosticActiveDisplayTicks += static_cast<std::uint64_t> (
                    juce::Time::getHighResolutionTicks() - diagnosticCaptureStart);
#endif
            }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticStageStart = juce::Time::getHighResolutionTicks();
#endif
            const auto channelHasReduction =
                lastMaximumReductionDb[static_cast<size_t> (channel)] > 0.0f
                || lastWidebandReductionDb[static_cast<size_t> (channel)] > 0.0f;

            if (! mainIsStereo && ! channelHasReduction)
                addDryChannel (static_cast<size_t> (channel));
            else
                inverseAndAccumulate (transform,
                                      overlapBuffer[static_cast<size_t> (channel)],
                                      window);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticInverseStageTicks += static_cast<std::uint64_t> (
                juce::Time::getHighResolutionTicks() - diagnosticStageStart);
#endif
        }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticMaskTicks.fetch_add (diagnosticMaskStageTicks, std::memory_order_relaxed);
        diagnosticWideGainTicks.fetch_add (diagnosticWideGainStageTicks,
                                            std::memory_order_relaxed);
        diagnosticInverseTicks.fetch_add (diagnosticInverseStageTicks,
                                           std::memory_order_relaxed);
#endif

        if (shouldUpdateVisualisation)
        {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            const auto diagnosticPublishStart = juce::Time::getHighResolutionTicks();
#endif
            publishDisplayFrame (mainIsStereo, true);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            diagnosticActiveDisplayTicks += static_cast<std::uint64_t> (
                juce::Time::getHighResolutionTicks() - diagnosticPublishStart);
            diagnosticActiveVisualisationCount.fetch_add (1, std::memory_order_relaxed);
            diagnosticActiveVisualisationTicks.fetch_add (diagnosticActiveDisplayTicks,
                                                           std::memory_order_relaxed);
#endif
        }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        const auto diagnosticOlaStart = juce::Time::getHighResolutionTicks();
#endif
        finishOverlapAddFrame (retainedSamples);
        ++processedOutputFrameCount;
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        diagnosticOlaTicks.fetch_add (
            static_cast<std::uint64_t> (juce::Time::getHighResolutionTicks()
                                        - diagnosticOlaStart),
            std::memory_order_relaxed);
        diagnosticSpectralFrameCount.fetch_add (1, std::memory_order_relaxed);
        diagnosticSpectralFrameTicks.fetch_add (
            static_cast<std::uint64_t> (juce::Time::getHighResolutionTicks()
                                        - diagnosticFrameStart),
            std::memory_order_relaxed);
#endif
    }

    void performMainForwardFft (size_t channel,
                                const std::vector<float>& window) noexcept
    {
        auto& transform = fftData[channel];
        juce::FloatVectorOperations::multiply (transform.data(),
                                               analysisInput[channel].data(),
                                               window.data(),
                                               fftSize);

        activeFft->performRealOnlyForwardTransform (transform.data(), true);
    }

    void performSidechainForwardFft (size_t channel,
                                     const std::vector<float>& window) noexcept
    {
        auto& transform = sidechainFftData[channel];
        juce::FloatVectorOperations::multiply (transform.data(),
                                               sidechainAnalysisInput[channel].data(),
                                               window.data(),
                                               fftSize);

        activeFft->performRealOnlyForwardTransform (transform.data(), true);
        ++processedSidechainFftCount;
    }

    [[nodiscard]] float calculateSidechainDetector (const std::vector<float>& transform,
                                                     float inputGain = 1.0f) noexcept
    {
        const auto bins = fftSize / 2 + 1;
        float magnitudeSum = std::abs (transform[0]);

        for (int bin = 1; bin < bins - 1; ++bin)
        {
            const auto real = transform[static_cast<size_t> (2 * bin)];
            const auto imaginary = transform[static_cast<size_t> (2 * bin + 1)];
            magnitudeSum += 2.0f * std::sqrt (real * real + imaginary * imaginary);
        }

        if (bins > 1)
            magnitudeSum += std::abs (transform[static_cast<size_t> (2 * (bins - 1))]);

        const auto peakEstimate = inputGain * magnitudeSum / static_cast<float> (fftSize);
        if (! std::isfinite (peakEstimate))
            return 0.0f;

        const auto levelDb = juce::Decibels::gainToDecibels (peakEstimate, -160.0f);
        return SidechainLevelMapper::mapLevelDb (levelDb,
                                                  detectorFloorDb,
                                                  parameters.activityThresholdDb);
    }

    void applyWidebandGain (std::vector<float>& transform, float reductionDb) const noexcept
    {
        const auto gain = juce::Decibels::decibelsToGain (-juce::jmax (0.0f, reductionDb));

        // JUCE's real-only inverse transform consumes only N/2 + 1 complex bins.
        // The rest of the 2N scratch buffer is workspace and is not audio data.
        const auto valuesUsedByInverse = fftSize + 2;

        juce::FloatVectorOperations::multiply (transform.data(), gain, valuesUsedByInverse);
    }

    void updateEconomyCrossoverCurveCache() noexcept
    {
        if (! economyCrossoverCurveDirty)
            return;

        const auto bins = fftSize / 2 + 1;
        const auto cutoff = juce::jmax (10.0f, parameters.crossoverHz);

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto frequency = static_cast<float> (bin)
                                 * static_cast<float> (sampleRate)
                                 / static_cast<float> (fftSize);

            if (frequency <= 0.0f)
            {
                cachedEconomyHighWeight[static_cast<size_t> (bin)] = 0.0f;
                continue;
            }

            // Magnitude-complementary LR8-style transition.  At the crossover
            // both branches are 0.5 (-6.02 dB), L + H is exactly one, and the
            // asymptotic high-pass slope is 48 dB/octave.
            const auto ratio = frequency / cutoff;
            const auto ratioSquared = ratio * ratio;
            const auto ratioFourth = ratioSquared * ratioSquared;
            const auto ratioEighth = ratioFourth * ratioFourth;
            cachedEconomyHighWeight[static_cast<size_t> (bin)] =
                ratioEighth / (1.0f + ratioEighth);
        }

        economyCrossoverCurveDirty = false;
    }

    void applyEconomyCrossoverGain (int channel,
                                    std::vector<float>& transform,
                                    float widebandReductionDb,
                                    bool updateDisplayMask) noexcept
    {
        updateEconomyCrossoverCurveCache();
        const auto bins = fftSize / 2 + 1;
        const auto widebandGain = juce::Decibels::decibelsToGain (
            -juce::jmax (0.0f, widebandReductionDb));
        auto& spectralReduction = appliedSpectralReductionDb[static_cast<size_t> (channel)];
        auto minimumEffectiveSpectralGain = 1.0f;

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto crossoverHighWeight = cachedEconomyHighWeight[static_cast<size_t> (bin)];
            const auto highWeight = 1.0f - currentCrossoverStageMix
                                  + currentCrossoverStageMix * crossoverHighWeight;
            const auto lowWeight = 1.0f - highWeight;
            const auto spectralGain = juce::Decibels::decibelsToGain (
                -juce::jmax (0.0f, spectralReduction[static_cast<size_t> (bin)]));
            const auto normalGain = lowWeight
                                  + highWeight * spectralGain * widebandGain;
            const auto crossoverDeltaGain = 1.0f - crossoverHighWeight;
            const auto combinedGain = normalGain
                                    + currentCrossoverDeltaMix
                                          * (crossoverDeltaGain - normalGain);
            const auto offset = static_cast<size_t> (2 * bin);
            transform[offset] *= combinedGain;
            transform[offset + 1] *= combinedGain;

            const auto effectiveSpectralGain = lowWeight + highWeight * spectralGain;
            minimumEffectiveSpectralGain = juce::jmin (minimumEffectiveSpectralGain,
                                                       effectiveSpectralGain);

            if (updateDisplayMask)
            {
                // Keep the amber UI mask frequency-selective: the red
                // wideband reduction remains represented by the W meter.
                const auto effectiveSpectralReduction = -juce::Decibels::gainToDecibels (
                    juce::jmax (epsilon, effectiveSpectralGain), -160.0f);
                spectralReduction[static_cast<size_t> (bin)] = effectiveSpectralReduction;
            }
        }

        lastMaximumReductionDb[static_cast<size_t> (channel)] =
            -juce::Decibels::gainToDecibels (
                juce::jmax (epsilon, minimumEffectiveSpectralGain), -160.0f);
    }

    void addDryFrame (bool stereo) noexcept
    {
        addDryChannel (leftIndex);
        addDryChannel (rightIndex);

        if (! stereo)
        {
            addDryChannel (centreIndex);
            addDryChannel (leftSurroundIndex);
            addDryChannel (rightSurroundIndex);
        }
    }

    void addDryChannel (size_t channel) noexcept
    {
        juce::FloatVectorOperations::addWithMultiply (
            overlapBuffer[channel].data(),
            analysisInput[channel].data(),
            dryWindowGain.data(),
            fftSize);
    }

    void inverseAndAccumulate (std::vector<float>& transform,
                               std::vector<float>& overlap,
                               const std::vector<float>& window) noexcept
    {
        activeFft->performRealOnlyInverseTransform (transform.data());
        juce::FloatVectorOperations::multiply (transform.data(), window.data(), fftSize);
        juce::FloatVectorOperations::multiply (transform.data(),
                                               synthesisNormalisation,
                                               fftSize);
        juce::FloatVectorOperations::add (overlap.data(), transform.data(), fftSize);
    }

    void finishOverlapAddFrame (int retainedSamples) noexcept
    {
        lastLcrPeaks = {};
        const auto captureOutputPeaks = visualisationEnabled.load (
            std::memory_order_relaxed);
        const auto channelsToFinish = mainIsStereo ? 3 : lcrChannels;

        for (int channel = 0; channel < channelsToFinish; ++channel)
        {
            auto& overlap = overlapBuffer[static_cast<size_t> (channel)];
            auto& output = outputHop[static_cast<size_t> (channel)];
            std::copy_n (overlap.begin(), hopSize, output.begin());

            if (captureOutputPeaks)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (
                    output.data(), hopSize);
                const auto magnitude = juce::jmax (std::abs (range.getStart()),
                                                   std::abs (range.getEnd()));

                switch (channel)
                {
                    case static_cast<int> (leftIndex):
                        lastLcrPeaks.left = juce::jmax (lastLcrPeaks.left, magnitude);
                        break;
                    case static_cast<int> (centreIndex):
                        lastLcrPeaks.centre = juce::jmax (lastLcrPeaks.centre, magnitude);
                        break;
                    case static_cast<int> (rightIndex):
                        lastLcrPeaks.right = juce::jmax (lastLcrPeaks.right, magnitude);
                        break;
                    case static_cast<int> (leftSurroundIndex):
                        lastLcrPeaks.leftSurround = juce::jmax (lastLcrPeaks.leftSurround, magnitude);
                        break;
                    case static_cast<int> (rightSurroundIndex):
                        lastLcrPeaks.rightSurround = juce::jmax (lastLcrPeaks.rightSurround, magnitude);
                        break;
                    default:
                        break;
                }
            }

            std::memmove (overlap.data(),
                          overlap.data() + hopSize,
                          static_cast<size_t> (retainedSamples) * sizeof (float));
            std::fill (overlap.begin() + retainedSamples, overlap.begin() + fftSize, 0.0f);
        }
    }

    void updateWeightingCurveCache() noexcept
    {
        if (! weightingCurveDirty)
            return;

        const auto bins = fftSize / 2 + 1;
        const auto sigmaOctaves = 1.0f
                                / juce::jmax (0.2f, 2.0f * parameters.weightQ);
        const auto weightFrequency = juce::jmax (1.0f, parameters.weightFrequencyHz);

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto frequency = static_cast<float> (bin)
                                 * static_cast<float> (sampleRate)
                                 / static_cast<float> (fftSize);
            const auto logDistance = std::log2 (juce::jmax (1.0f, frequency)
                                                / weightFrequency);
            cachedWeightBellDb[static_cast<size_t> (bin)] = parameters.weightGainDb
                * std::exp (-0.5f * (logDistance * logDistance)
                            / (sigmaOctaves * sigmaOctaves));
        }

        weightingCurveDirty = false;
    }

    void updateSpectralMask (int channel,
                             std::vector<float>& transform,
                             float frameControl,
                             float stageMix,
                             bool initialiseMaskImmediately,
                             bool captureInputSpectrum,
                             bool applyGainToTransform,
                             float additionalReductionDb) noexcept
    {
        const auto bins = fftSize / 2 + 1;
        auto& magnitudes = magnitudeDb[static_cast<size_t> (channel)];
        auto& rawReduction = rawReductionDb[static_cast<size_t> (channel)];
        auto& temporal = temporalReductionDb[static_cast<size_t> (channel)];
        auto& applied = appliedSpectralReductionDb[static_cast<size_t> (channel)];
        const auto amount = juce::jmax (0.0f, parameters.amountDb);
        stageMix = juce::jlimit (0.0f, 1.0f, stageMix);

        if (amount <= 0.0f || stageMix <= epsilon)
        {
            if (captureInputSpectrum)
                captureMagnitudeDb (transform, magnitudes);

            std::fill (temporal.begin(), temporal.begin() + bins, 0.0f);
            std::fill (applied.begin(), applied.begin() + bins, 0.0f);
            lastMaximumReductionDb[static_cast<size_t> (channel)] = 0.0f;
            return;
        }

        const auto lowFrequency = juce::jmax (0.0f, parameters.lowFrequencyHz);
        const auto highFrequency = juce::jmin (static_cast<float> (sampleRate * 0.5),
                                               parameters.highFrequencyHz);
        const auto firstAverageBin = juce::jmax (
            1,
            static_cast<int> (std::ceil (lowFrequency * static_cast<float> (fftSize)
                                         / static_cast<float> (sampleRate))));
        const auto lastAverageBin = juce::jmin (
            bins - 1,
            static_cast<int> (std::floor (highFrequency * static_cast<float> (fftSize)
                                          / static_cast<float> (sampleRate))));
        updateWeightingCurveCache();
        double globalPowerSum = 0.0;
        int globalPowerCount = 0;
        std::fill (rawReduction.begin(), rawReduction.begin() + bins, 0.0f);

        // Use one spectral baseline for the complete active Low-High range.
        // Average linear power rather than dB values: otherwise bins at the
        // display floor drag the baseline down and make almost the full band
        // hit maximum reduction.
        const auto firstMagnitudeBin = captureInputSpectrum ? 0 : firstAverageBin;
        const auto lastMagnitudeBin = captureInputSpectrum ? bins - 1 : lastAverageBin;

        if (firstMagnitudeBin <= lastMagnitudeBin)
        {
            for (int bin = firstMagnitudeBin; bin <= lastMagnitudeBin; ++bin)
            {
                const auto real = transform[static_cast<size_t> (2 * bin)];
                const auto imaginary = transform[static_cast<size_t> (2 * bin + 1)];
                const auto power = real * real + imaginary * imaginary;

                if (std::isfinite (power))
                {
                    magnitudes[static_cast<size_t> (bin)] =
                        juce::Decibels::gainToDecibels (std::sqrt (power), -160.0f);

                    if (bin >= firstAverageBin && bin <= lastAverageBin)
                    {
                        globalPowerSum += static_cast<double> (power);
                        ++globalPowerCount;
                    }
                }
                else
                {
                    magnitudes[static_cast<size_t> (bin)] = -160.0f;
                }
            }
        }

        const auto globalMeanPower = globalPowerCount > 0
            ? globalPowerSum / static_cast<double> (globalPowerCount)
            : 0.0;
        const auto globalBaselineDb = globalMeanPower > 0.0
            ? juce::Decibels::gainToDecibels (static_cast<float> (std::sqrt (globalMeanPower)),
                                              -160.0f)
            : -160.0f;
        // The voice envelope is applied after the structural mask has been smoothed.
        // This keeps bin-to-bin mask changes stable without adding a second attack or
        // release to the audible amount of spectral ducking.
        if (globalPowerCount > 0)
        {
            for (int bin = firstAverageBin; bin <= lastAverageBin; ++bin)
            {
                const auto prominenceDb = magnitudes[static_cast<size_t> (bin)]
                                        - globalBaselineDb;
                const auto drivenProminence = prominenceDb
                    + cachedWeightBellDb[static_cast<size_t> (bin)]
                    - prominenceThresholdDb;
                rawReduction[static_cast<size_t> (bin)] = juce::jlimit (
                    0.0f,
                    amount,
                    juce::jmax (0.0f, drivenProminence) * prominenceRatio);
            }
        }

        const auto voiceAmount = juce::jlimit (0.0f, 1.0f, frameControl);
        lastMaximumReductionDb[static_cast<size_t> (channel)] = 0.0f;

        for (int bin = 0; bin < bins; ++bin)
        {
            float target = 0.0f;

            if (bin == 0)
            {
                target = 0.5f * (rawReduction[0] + rawReduction[1]);
            }
            else if (bin == bins - 1)
            {
                target = 0.5f
                    * (rawReduction[static_cast<size_t> (bins - 2)]
                       + rawReduction[static_cast<size_t> (bins - 1)]);
            }
            else
            {
                target = rawReduction[static_cast<size_t> (bin - 1)];
                target += rawReduction[static_cast<size_t> (bin)];
                target += rawReduction[static_cast<size_t> (bin + 1)];
                target /= 3.0f;
            }

            auto& smoothed = temporal[static_cast<size_t> (bin)];
            if (initialiseMaskImmediately)
            {
                smoothed = target;
            }
            else
            {
                const auto coefficient = target > smoothed
                    ? maskShapeAttackCoefficient
                    : maskShapeReleaseCoefficient;
                smoothed = coefficient * smoothed + (1.0f - coefficient) * target;
            }

            const auto appliedReductionDb = smoothed * voiceAmount * stageMix;
            applied[static_cast<size_t> (bin)] = appliedReductionDb;
            lastMaximumReductionDb[static_cast<size_t> (channel)] = juce::jmax (
                lastMaximumReductionDb[static_cast<size_t> (channel)],
                appliedReductionDb);
            if (applyGainToTransform)
            {
                const auto combinedGain = juce::Decibels::decibelsToGain (
                    -(appliedReductionDb + juce::jmax (0.0f, additionalReductionDb)));
                transform[static_cast<size_t> (2 * bin)] *= combinedGain;
                transform[static_cast<size_t> (2 * bin + 1)] *= combinedGain;
            }
        }
    }

    void calculateLcrSpectra (std::vector<float>& leftTransform,
                              std::vector<float>& rightTransform,
                              std::vector<float>& centreTransform,
                              PceHistory& history) noexcept
    {
        const auto bins = fftSize / 2 + 1;
        for (int bin = 0; bin < bins; ++bin)
        {
            const auto offset = static_cast<size_t> (2 * bin);
            const auto leftReal = leftTransform[offset];
            const auto leftImaginary = leftTransform[offset + 1];
            const auto rightReal = rightTransform[offset];
            const auto rightImaginary = rightTransform[offset + 1];
            const auto crossReal = leftReal * rightReal + leftImaginary * rightImaginary;
            const auto crossImaginary = leftImaginary * rightReal - leftReal * rightImaginary;
            const auto powerLeft = leftReal * leftReal + leftImaginary * leftImaginary;
            const auto powerRight = rightReal * rightReal + rightImaginary * rightImaginary;
            auto& averagedCrossReal = history.crossReal[static_cast<size_t> (bin)];
            auto& averagedCrossImaginary = history.crossImaginary[static_cast<size_t> (bin)];
            auto& averagedPowerLeft = history.powerLeft[static_cast<size_t> (bin)];
            auto& averagedPowerRight = history.powerRight[static_cast<size_t> (bin)];
            averagedCrossReal = coherenceCoefficient * averagedCrossReal
                              + (1.0f - coherenceCoefficient) * crossReal;
            averagedCrossImaginary = coherenceCoefficient * averagedCrossImaginary
                                   + (1.0f - coherenceCoefficient) * crossImaginary;
            averagedPowerLeft = coherenceCoefficient * averagedPowerLeft
                              + (1.0f - coherenceCoefficient) * powerLeft;
            averagedPowerRight = coherenceCoefficient * averagedPowerRight
                               + (1.0f - coherenceCoefficient) * powerRight;
            const auto crossMagnitude = std::sqrt (averagedCrossReal * averagedCrossReal
                                                   + averagedCrossImaginary * averagedCrossImaginary);
            const auto coherence = juce::jlimit (
                0.0f,
                1.0f,
                crossMagnitude / std::sqrt (averagedPowerLeft * averagedPowerRight + epsilon));
            const auto phaseAgreement = juce::jlimit (
                0.0f,
                1.0f,
                averagedCrossReal / (crossMagnitude + epsilon));
            const auto magnitudeLeft = std::sqrt (powerLeft);
            const auto magnitudeRight = std::sqrt (powerRight);
            const auto balance = std::abs (magnitudeLeft - magnitudeRight)
                               / (magnitudeLeft + magnitudeRight + epsilon);
            const auto centreFactor = coherence * phaseAgreement * (1.0f - balance);
            history.rawCentreFactor[static_cast<size_t> (bin)] = std::isfinite (centreFactor)
                ? juce::jlimit (0.0f, 1.0f, centreFactor)
                : 0.0f;
        }

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto offset = static_cast<size_t> (2 * bin);
            float factor = 0.0f;

            if (bin == 0)
            {
                factor = 0.5f
                    * (history.rawCentreFactor[0] + history.rawCentreFactor[1]);
            }
            else if (bin == bins - 1)
            {
                factor = 0.5f
                    * (history.rawCentreFactor[static_cast<size_t> (bins - 2)]
                       + history.rawCentreFactor[static_cast<size_t> (bins - 1)]);
            }
            else
            {
                factor = history.rawCentreFactor[static_cast<size_t> (bin - 1)];
                factor += history.rawCentreFactor[static_cast<size_t> (bin)];
                factor += history.rawCentreFactor[static_cast<size_t> (bin + 1)];
                factor /= 3.0f;
            }

            const auto commonReal = 0.5f * factor
                                  * (leftTransform[offset] + rightTransform[offset]);
            const auto commonImaginary = 0.5f * factor
                                       * (leftTransform[offset + 1] + rightTransform[offset + 1]);
            leftTransform[offset] -= commonReal;
            leftTransform[offset + 1] -= commonImaginary;
            rightTransform[offset] -= commonReal;
            rightTransform[offset + 1] -= commonImaginary;
            centreTransform[offset] = commonReal / centreDownmixGain;
            centreTransform[offset + 1] = commonImaginary / centreDownmixGain;
        }
    }

    void clearPceHistory() noexcept
    {
        for (auto& history : pceHistories)
            history.clear();
    }

    void captureMagnitudeDb (const std::vector<float>& transform,
                             std::vector<float>& destination) const noexcept
    {
        const auto bins = fftSize / 2 + 1;

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto real = transform[static_cast<size_t> (2 * bin)];
            const auto imaginary = transform[static_cast<size_t> (2 * bin + 1)];
            destination[static_cast<size_t> (bin)] = juce::Decibels::gainToDecibels (
                std::sqrt (real * real + imaginary * imaginary),
                -160.0f);
        }
    }

    void updateLowBandDisplayFrame() noexcept
    {
        if (economyMode || lowBandDisplayFft == nullptr || lowBandDisplaySamplesWritten <= 0)
            return;

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            const auto channelIsDisplayed = ! mainIsStereo
                                          || channel == static_cast<int> (leftIndex)
                                          || channel == static_cast<int> (rightIndex);

            if (! channelIsDisplayed)
                continue;

            auto& transform = lowBandDisplayFftData[static_cast<size_t> (channel)];
            const auto& ring = lowBandDisplayRing[static_cast<size_t> (channel)];

            for (int sample = 0; sample < lowBandDisplayFftSize; ++sample)
            {
                const auto source = static_cast<size_t> (
                    (lowBandDisplayWritePosition + sample) % lowBandDisplayFftSize);
                const auto value = ring[source];
                transform[static_cast<size_t> (sample)] = value
                    * lowBandDisplayWindow[static_cast<size_t> (sample)];
            }

            std::fill (transform.begin() + lowBandDisplayFftSize,
                       transform.end(),
                       0.0f);
            lowBandDisplayFft->performRealOnlyForwardTransform (transform.data(), true);

            auto& magnitudes = lowBandDisplayMagnitudeDb[static_cast<size_t> (channel)];
            const auto bins = lowBandDisplayFftSize / 2 + 1;

            for (int bin = 0; bin < bins; ++bin)
            {
                const auto offset = static_cast<size_t> (2 * bin);
                const auto real = transform[offset];
                const auto imaginary = transform[offset + 1];
                const auto magnitude = juce::Decibels::gainToDecibels (
                    std::sqrt (real * real + imaginary * imaginary), -160.0f);
                magnitudes[static_cast<size_t> (bin)] = magnitude;
            }
        }
    }

    [[nodiscard]] static float addSpectrumLevelsDb (float firstDb, float secondDb) noexcept
    {
        if (firstDb <= -159.0f)
            return secondDb;
        if (secondDb <= -159.0f)
            return firstDb;

        const auto firstPower = juce::Decibels::decibelsToGain (firstDb);
        const auto secondPower = juce::Decibels::decibelsToGain (secondDb);
        return juce::Decibels::gainToDecibels (
            std::sqrt (firstPower * firstPower + secondPower * secondPower), -160.0f);
    }

    void updateDryDisplayFrame (const std::vector<float>& window) noexcept
    {
        const auto firstChannel = 0;
        const auto lastChannel = mainIsStereo ? 3 : lcrChannels;

        for (int channel = firstChannel; channel < lastChannel; ++channel)
        {
            if (mainIsStereo && channel == static_cast<int> (centreIndex))
                continue;

            performMainForwardFft (static_cast<size_t> (channel), window);
            captureMagnitudeDb (fftData[static_cast<size_t> (channel)],
                                magnitudeDb[static_cast<size_t> (channel)]);
            std::copy_n (magnitudeDb[static_cast<size_t> (channel)].begin(),
                         fftSize / 2 + 1,
                         processedMagnitudeDb[static_cast<size_t> (channel)].begin());
        }

        publishDisplayFrame (false, false);
    }

    void publishDisplayFrame (bool stereoCentreIsAvailable,
                              bool includeSpectralMask) noexcept
    {
        if (! visualisationEnabled.load (std::memory_order_acquire))
            return;

        updateLowBandDisplayFrame();

        displayFrameCounter.fetch_add (1, std::memory_order_acq_rel);

        const auto sourceBins = fftSize / 2 + 1;
        const auto normalisationDb = juce::Decibels::gainToDecibels (
            static_cast<float> (fftSize) * 0.5f,
            -160.0f);
        constexpr auto minimumDisplayFrequency = 20.0f;
        constexpr auto displayCalibrationDb = 18.0f;
        const auto nyquist = static_cast<float> (sampleRate * 0.5);
        const auto channelIsDisplayed = [this, stereoCentreIsAvailable] (int channel)
        {
            return ! mainIsStereo
                || channel == static_cast<int> (leftIndex)
                || channel == static_cast<int> (rightIndex)
                || (stereoCentreIsAvailable
                    && channel == static_cast<int> (centreIndex));
        };

        // JUCE FFT backends do not all use the same absolute scale. Calibrate
        // display-only FFT magnitudes against the peak of the corresponding
        // time-domain frame so the analyser remains visible on every backend.
        auto rawMagnitudeMaximum = -160.0f;
        auto timeDomainPeak = 0.0f;

        for (int channel = 0; channel < lcrChannels; ++channel)
        {
            if (! channelIsDisplayed (channel))
                continue;

            const auto& magnitudes = magnitudeDb[static_cast<size_t> (channel)];
            rawMagnitudeMaximum = juce::jmax (
                rawMagnitudeMaximum,
                *std::max_element (magnitudes.begin(),
                                   magnitudes.begin() + sourceBins));

            const auto& samples = analysisInput[static_cast<size_t> (channel)];
            for (int sample = 0; sample < fftSize; ++sample)
                timeDomainPeak = juce::jmax (
                    timeDomainPeak,
                    std::abs (samples[static_cast<size_t> (sample)]));
        }

        const auto fallbackDisplayOffsetDb = -normalisationDb + displayCalibrationDb;
        const auto displayOffsetDb = timeDomainPeak > epsilon
                                  && rawMagnitudeMaximum > -159.0f
            ? juce::Decibels::gainToDecibels (timeDomainPeak, -160.0f)
                  - rawMagnitudeMaximum
            : fallbackDisplayOffsetDb;

        for (int displayBin = 0; displayBin < displayBinCount; ++displayBin)
        {
            const auto normal = static_cast<float> (displayBin)
                              / static_cast<float> (displayBinCount - 1);
            const auto frequency = minimumDisplayFrequency
                                 * std::pow (nyquist / minimumDisplayFrequency, normal);
            const auto sourcePosition = juce::jlimit (
                0.0f,
                static_cast<float> (sourceBins - 1),
                frequency * static_cast<float> (fftSize) / static_cast<float> (sampleRate));
            const auto lowerBin = static_cast<int> (std::floor (sourcePosition));
            const auto upperBin = juce::jmin (sourceBins - 1, lowerBin + 1);
            const auto interpolation = sourcePosition - static_cast<float> (lowerBin);
            auto inputMaximum = -160.0f;
            auto outputMaximum = -160.0f;
            auto lowBandMaximum = -160.0f;
            auto spectralMaskMaximum = 0.0f;

            for (int channel = 0; channel < lcrChannels; ++channel)
            {
                if (! channelIsDisplayed (channel))
                    continue;

                const auto& inputMagnitudes = magnitudeDb[static_cast<size_t> (channel)];
                const auto& outputMagnitudes = processedMagnitudeDb[static_cast<size_t> (channel)];
                const auto& spectralMask = appliedSpectralReductionDb[static_cast<size_t> (channel)];
                const auto inputMagnitude = juce::jmap (
                    interpolation,
                    inputMagnitudes[static_cast<size_t> (lowerBin)],
                    inputMagnitudes[static_cast<size_t> (upperBin)]);
                const auto outputMagnitude = juce::jmap (
                    interpolation,
                    outputMagnitudes[static_cast<size_t> (lowerBin)],
                    outputMagnitudes[static_cast<size_t> (upperBin)]);
                inputMaximum = juce::jmax (inputMaximum, inputMagnitude);
                outputMaximum = juce::jmax (outputMaximum, outputMagnitude);

                if (! economyMode)
                {
                    const auto lowBandSampleRate = static_cast<float> (sampleRate)
                                                 / static_cast<float> (
                                                       lowBandDisplayDecimationFactor);
                    const auto lowBandNyquist = 0.5f * lowBandSampleRate;

                    if (frequency < lowBandNyquist)
                    {
                        const auto lowSourcePosition = juce::jlimit (
                            0.0f,
                            static_cast<float> (lowBandDisplayFftSize / 2),
                            frequency * static_cast<float> (lowBandDisplayFftSize)
                                / lowBandSampleRate);
                        const auto lowBin = static_cast<int> (std::floor (lowSourcePosition));
                        const auto highBin = juce::jmin (lowBandDisplayFftSize / 2,
                                                        lowBin + 1);
                        const auto lowInterpolation = lowSourcePosition
                                                    - static_cast<float> (lowBin);
                        const auto& lowMagnitudes = lowBandDisplayMagnitudeDb[
                            static_cast<size_t> (channel)];
                        const auto lowMagnitude = juce::jmap (
                            lowInterpolation,
                            lowMagnitudes[static_cast<size_t> (lowBin)],
                            lowMagnitudes[static_cast<size_t> (highBin)]);
                        lowBandMaximum = juce::jmax (lowBandMaximum, lowMagnitude);
                    }
                }

                if (includeSpectralMask)
                {
                    const auto maskValue = juce::jmap (
                        interpolation,
                        spectralMask[static_cast<size_t> (lowerBin)],
                        spectralMask[static_cast<size_t> (upperBin)]);
                    spectralMaskMaximum = juce::jmax (spectralMaskMaximum, maskValue);
                }
            }

            auto calibratedInput = inputMaximum + displayOffsetDb;
            auto calibratedOutput = outputMaximum + displayOffsetDb;

            if (! economyMode && lowBandMaximum > -159.0f)
            {
                // Express the decimated FFT in the main FFT's raw magnitude
                // scale, then apply the same per-frame display calibration.
                // Separate peak/max calibration for the short low-band FFT
                // under-read broadband bass by roughly 6 dB.
                const auto calibratedLowBand = lowBandMaximum
                    + (timeDomainPeak > epsilon && rawMagnitudeMaximum > -159.0f
                           ? lowBandToMainFftScaleDb + displayOffsetDb
                           : lowBandAbsoluteFftOffsetDb);
                calibratedInput = addSpectrumLevelsDb (calibratedInput, calibratedLowBand);
                calibratedOutput = addSpectrumLevelsDb (calibratedOutput, calibratedLowBand);
            }

            const auto input = juce::jlimit (-160.0f, 12.0f, calibratedInput);
            const auto output = juce::jlimit (-160.0f, 12.0f, calibratedOutput);
            const auto reduction = juce::jlimit (0.0f, 36.0f, input - output);
            const auto index = static_cast<size_t> (displayBin);
            displayInputDb[index].store (input, std::memory_order_relaxed);
            displayOutputDb[index].store (output, std::memory_order_relaxed);
            displayReductionDb[index].store (reduction, std::memory_order_relaxed);
            displaySpectralMaskDb[index].store (
                juce::jlimit (0.0f, 36.0f, spectralMaskMaximum),
                std::memory_order_relaxed);
        }

        displayFrameCounter.fetch_add (1, std::memory_order_release);
    }

    static constexpr int numModes = 3;
    static constexpr int normalOverlapFactor = 4;
    static constexpr int economyOverlapFactor = 2;
    static constexpr int lowBandDisplayDecimationFactor = 8;
    static constexpr int lowBandDisplayFftOrder = 9;
    static constexpr int lowBandDisplayFftSize = 1 << lowBandDisplayFftOrder;
    static constexpr int lcrChannels = 5;
    static constexpr size_t leftIndex = 0;
    static constexpr size_t centreIndex = 1;
    static constexpr size_t rightIndex = 2;
    static constexpr size_t leftSurroundIndex = 3;
    static constexpr size_t rightSurroundIndex = 4;
    static constexpr size_t mainStreamIndex = 0;
    static constexpr size_t sidechainStreamIndex = 1;
    static constexpr float prominenceThresholdDb = 4.0f;
    static constexpr float prominenceRatio = 1.5f;
    static constexpr float maskShapeAttackSeconds = 0.020f;
    static constexpr float maskShapeReleaseSeconds = 0.100f;
    static constexpr float coherenceTimeSeconds = 0.050f;
    static constexpr float sleepControlThreshold = 1.0e-5f;
    static constexpr float sleepReductionThresholdDb = 0.01f;
    static constexpr float stageTransitionSeconds = 0.030f;
    static constexpr float detectorFloorDb = -60.0f;
    static constexpr float epsilon = 1.0e-12f;

    double sampleRate = 48000.0;
    int baseOrder = 10;
    int resolutionIndex = 1;
    int fftSize = 2048;
    int hopSize = 512;
    int hopPosition = 0;
    float synthesisNormalisation = 0.5f;
    Parameters parameters;
    juce::dsp::FFT* activeFft = nullptr;
    std::array<std::unique_ptr<juce::dsp::FFT>, numModes> ffts;
    std::array<std::vector<float>, numModes> windows;
    std::array<std::vector<float>, lcrChannels> analysisInput;
    std::array<std::vector<float>, lcrChannels> inputHop;
    std::array<std::vector<float>, lcrChannels> fftData;
    std::array<std::vector<float>, lcrChannels> sidechainAnalysisInput;
    std::array<std::vector<float>, lcrChannels> sidechainInputHop;
    std::array<std::vector<float>, lcrChannels> sidechainFftData;
    std::array<std::vector<float>, lcrChannels> outputHop;
    std::array<std::vector<float>, lcrChannels> overlapBuffer;
    std::array<std::vector<float>, lcrChannels> magnitudeDb;
    std::array<std::vector<float>, lcrChannels> processedMagnitudeDb;
    std::array<std::vector<float>, lcrChannels> rawReductionDb;
    std::array<std::vector<float>, lcrChannels> temporalReductionDb;
    std::array<std::vector<float>, lcrChannels> appliedSpectralReductionDb;
    std::vector<float> fftDataCentre;
    std::vector<float> sidechainFftDataCentre;
    std::vector<float> cachedWeightBellDb;
    std::vector<float> cachedEconomyHighWeight;
    std::vector<float> dryWindowGain;
    std::unique_ptr<juce::dsp::FFT> lowBandDisplayFft;
    std::vector<float> lowBandDisplayWindow;
    std::array<std::vector<float>, lcrChannels> lowBandDisplayRing;
    std::array<std::vector<float>, lcrChannels> lowBandDisplayFftData;
    std::array<std::vector<float>, lcrChannels> lowBandDisplayMagnitudeDb;
    std::array<float, lcrChannels> lowBandDisplayAccumulator {};
    int lowBandDisplayDecimationPosition = 0;
    int lowBandDisplayWritePosition = 0;
    int lowBandDisplaySamplesWritten = 0;
    float lowBandToMainFftScaleDb = 0.0f;
    float lowBandAbsoluteFftOffsetDb = 0.0f;
    std::array<PceHistory, 2> pceHistories;
    SidechainClassifier sidechainClassifier;
    std::array<SidechainEnvelope, lcrChannels> voiceEnvelopes;
    std::array<float, lcrChannels> lastMaximumReductionDb {};
    std::array<float, lcrChannels> lastWidebandReductionDb {};
    LcrSample lastLcrPeaks;
    std::uint64_t processedFftFrameCount = 0;
    std::uint64_t processedOutputFrameCount = 0;
    std::uint64_t processedSidechainFftCount = 0;
    std::uint64_t processedSidechainPceFrameCount = 0;
    std::array<std::atomic<float>, displayBinCount> displayInputDb {};
    std::array<std::atomic<float>, displayBinCount> displayOutputDb {};
    std::array<std::atomic<float>, displayBinCount> displayReductionDb {};
    std::array<std::atomic<float>, displayBinCount> displaySpectralMaskDb {};
    std::atomic<std::uint64_t> displayFrameCounter { 0 };
    std::atomic<bool> visualisationEnabled { false };
    std::atomic<float> visualisationFrameRate { 30.0f };
    std::atomic<int> visualisationFramesUntilUpdate { 0 };
    std::atomic<bool> heavyPathActive { false };
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    std::atomic<std::uint64_t> diagnosticSpectralFrameCount { 0 };
    std::atomic<std::uint64_t> diagnosticSpectralFrameTicks { 0 };
    std::atomic<std::uint64_t> diagnosticDryVisualisationCount { 0 };
    std::atomic<std::uint64_t> diagnosticDryVisualisationTicks { 0 };
    std::atomic<std::uint64_t> diagnosticActiveVisualisationCount { 0 };
    std::atomic<std::uint64_t> diagnosticActiveVisualisationTicks { 0 };
    std::atomic<std::uint64_t> diagnosticFrameInputTicks { 0 };
    std::atomic<std::uint64_t> diagnosticClassificationTicks { 0 };
    std::atomic<std::uint64_t> diagnosticMainAnalysisTicks { 0 };
    std::atomic<std::uint64_t> diagnosticSidechainAnalysisTicks { 0 };
    std::atomic<std::uint64_t> diagnosticControlTicks { 0 };
    std::atomic<std::uint64_t> diagnosticMaskTicks { 0 };
    std::atomic<std::uint64_t> diagnosticWideGainTicks { 0 };
    std::atomic<std::uint64_t> diagnosticInverseTicks { 0 };
    std::atomic<std::uint64_t> diagnosticOlaTicks { 0 };
#endif
    SidechainClassifier::Mode lastSidechainMode = SidechainClassifier::Mode::silence;
    float coherenceCoefficient = 0.0f;
    float maskShapeAttackCoefficient = 0.0f;
    float maskShapeReleaseCoefficient = 0.0f;
    bool mainIsStereo = true;
    bool voiceIsStereo = true;
    bool heavyPathSleeping = true;
    float currentCrossoverStageMix = 1.0f;
    float currentWidebandStageMix = 1.0f;
    float currentSpectralStageMix = 1.0f;
    float currentCrossoverDeltaMix = 0.0f;
    bool weightingCurveDirty = true;
    bool economyMode = false;
    bool economyCrossoverCurveDirty = true;
};
} // namespace smart_ducking
