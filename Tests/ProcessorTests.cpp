#include <cmath>
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <iostream>
#include <vector>

#include "ParameterIds.h"
#include "PluginProcessor.h"
#include "CrossoverMagnitude.h"
#include "LcrDuckMatrix.h"
#include "LinkwitzRileyCrossover.h"
#include "PceHistory.h"
#include "PowerOfTwoDelayBuffer.h"
#include "RealFft.h"
#include "SidechainClassifier.h"
#include "SidechainEnvelope.h"
#include "SpectralFrameSmoothers.h"
#include "SpectralSuppressor.h"
#include "SpectralVectorMath.h"
#include "UserPresetStore.h"

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool approximatelyEqual (float left, float right, float tolerance = 1.0e-6f)
{
    return std::abs (left - right) <= tolerance;
}

bool exactlyEqual (float left, float right)
{
    return std::bit_cast<std::uint32_t> (left)
        == std::bit_cast<std::uint32_t> (right);
}

void setParameter (SmartDuckingAudioProcessor& processor, const char* id, float rawValue)
{
    auto* parameter = processor.getParameterState().getParameter (id);
    expect (parameter != nullptr, id);

    if (parameter != nullptr)
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (rawValue));
}

struct ToneResult
{
    double leftRatio = 0.0;
    double rightRatio = 0.0;
};

struct SpatialToneResult
{
    std::array<double, 6> outputRatios {};
};

struct ProcessorImpulseResult
{
    int latency = 0;
    float maximumBeforeLatency = 0.0f;
    float sampleAtLatency = 0.0f;
};

struct TestPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position;
        position.setIsPlaying (isPlaying);
        return position;
    }

    bool isPlaying = false;
};

ToneResult processTone (SmartDuckingAudioProcessor& processor,
                        double frequencyHz,
                        float sidechainLeft,
                        float sidechainRight,
                        float mainLeft = 1.0f,
                        float mainRight = 1.0f)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int totalSamples = 24576;
    constexpr int warmupSamples = 8192;
    constexpr float amplitude = 0.5f;

    processor.prepareToPlay (sampleRate, blockSize);
    const auto voiceOffset = processor.getChannelCountOfBus (true, 0);
    juce::AudioBuffer<float> buffer (voiceOffset + 6, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    const auto phaseIncrement = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;
    double inputEnergy = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;

    for (int processed = 0; processed < totalSamples; processed += blockSize)
    {
        buffer.clear();

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = amplitude * static_cast<float> (std::sin (phase));
            phase += phaseIncrement;

            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            buffer.setSample (0, sample, mainLeft * value);
            buffer.setSample (1, sample, mainRight * value);
            buffer.setSample (voiceOffset, sample, sidechainLeft);
            buffer.setSample (voiceOffset + 1, sample, sidechainRight);
        }

        processor.processBlock (buffer, midi);

        for (int sample = 0; sample < blockSize; ++sample)
        {
            if (processed + sample < warmupSamples)
                continue;

            const auto input = amplitude * std::sin (
                phase - phaseIncrement * static_cast<double> (blockSize - sample));
            inputEnergy += input * input;
            leftEnergy += static_cast<double> (buffer.getSample (0, sample))
                        * static_cast<double> (buffer.getSample (0, sample));
            rightEnergy += static_cast<double> (buffer.getSample (1, sample))
                         * static_cast<double> (buffer.getSample (1, sample));
        }
    }

    return {
        std::sqrt (leftEnergy / inputEnergy),
        std::sqrt (rightEnergy / inputEnergy)
    };
}

double processToneAfterMonitorSwitch (smart_ducking::MonitorMode monitorMode,
                                      double frequencyHz,
                                      float slopeIndex = 2.0f,
                                      float crossoverHz = 92.0f)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int preSwitchSamples = 12288;
    constexpr int postSwitchSamples = 16384;
    constexpr int measurementStart = 8192;
    constexpr float amplitude = 0.5f;
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::crossoverSlope, slopeIndex);
    setParameter (processor, smart_ducking::parameter_ids::crossoverHz, crossoverHz);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
    processor.prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> buffer (8, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    const auto phaseIncrement = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;

    const auto fillAndProcess = [&]
    {
        buffer.clear();
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = amplitude * static_cast<float> (std::sin (phase));
            phase += phaseIncrement;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
        }
        processor.processBlock (buffer, midi);
    };

    for (int processed = 0; processed < preSwitchSamples; processed += blockSize)
        fillAndProcess();

    setParameter (processor,
                  smart_ducking::parameter_ids::monitorMode,
                  static_cast<float> (monitorMode));
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;

    for (int processed = 0; processed < postSwitchSamples; processed += blockSize)
    {
        fillAndProcess();

        if (processed < measurementStart)
            continue;

        for (int sample = 0; sample < blockSize; ++sample)
        {
            inputEnergy += static_cast<double> (amplitude) * amplitude * 0.5;
            const auto output = static_cast<double> (buffer.getSample (0, sample));
            outputEnergy += output * output;
        }
    }

    return std::sqrt (outputEnergy / inputEnergy);
}

std::vector<float> renderNormalCrossoverMonitor (
    smart_ducking::MonitorMode monitorMode,
    float slopeIndex = 2.0f)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int totalSamples = 32768;
    constexpr int warmupSamples = 8192;
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor,
                  smart_ducking::parameter_ids::monitorMode,
                  static_cast<float> (monitorMode));
    setParameter (processor, smart_ducking::parameter_ids::crossoverHz, 750.0f);
    setParameter (processor, smart_ducking::parameter_ids::crossoverSlope, slopeIndex);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (8, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (totalSamples - warmupSamples);

    for (int processed = 0; processed < totalSamples; processed += blockSize)
    {
        buffer.clear();

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = processed + sample;
            const auto phase = juce::MathConstants<double>::twoPi
                             * static_cast<double> (absoluteSample) / sampleRate;
            const auto impulse = absoluteSample % 997 == 0 ? 0.18f : 0.0f;
            const auto value = 0.18f * static_cast<float> (std::sin (73.0 * phase))
                             + 0.14f * static_cast<float> (std::sin (750.0 * phase))
                             + 0.11f * static_cast<float> (std::sin (6341.0 * phase))
                             + impulse;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value * 0.63f);
        }

        processor.processBlock (buffer, midi);

        if (processed >= warmupSamples)
            output.insert (output.end(),
                           buffer.getReadPointer (0),
                           buffer.getReadPointer (0) + blockSize);
    }

    return output;
}

struct CrossoverToneResult
{
    double lowRatio = 0.0;
    double highRatio = 0.0;
    double sumRatio = 0.0;
};

CrossoverToneResult measureQualityCrossover (float slopeDbPerOctave,
                                              double frequencyHz)
{
    constexpr double sampleRate = 48000.0;
    constexpr float cutoffHz = 92.0f;
    constexpr int totalSamples = 96000;
    constexpr int warmupSamples = 24000;
    smart_ducking::LinkwitzRileyCrossover crossover;
    crossover.prepare (sampleRate, cutoffHz, slopeDbPerOctave);
    double inputEnergy = 0.0;
    double lowEnergy = 0.0;
    double highEnergy = 0.0;
    double sumEnergy = 0.0;
    auto phase = 0.0;
    const auto phaseIncrement = juce::MathConstants<double>::twoPi
                              * frequencyHz / sampleRate;

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        const auto input = static_cast<float> (std::sin (phase));
        phase += phaseIncrement;

        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;

        crossover.beginSample();
        const auto bands = crossover.processSample (0, input);

        if (sample < warmupSamples)
            continue;

        inputEnergy += static_cast<double> (input) * input;
        lowEnergy += static_cast<double> (bands.low) * bands.low;
        highEnergy += static_cast<double> (bands.high) * bands.high;
        const auto sum = static_cast<double> (bands.low + bands.high);
        sumEnergy += sum * sum;
    }

    return {
        std::sqrt (lowEnergy / inputEnergy),
        std::sqrt (highEnergy / inputEnergy),
        std::sqrt (sumEnergy / inputEnergy)
    };
}

SpatialToneResult processSpatialTone (SmartDuckingAudioProcessor& processor,
                                      const std::array<float, 6>& mainChannels,
                                      const std::array<float, 6>& voiceChannels = {},
                                      double frequencyHz = 1000.0)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int totalSamples = 16384;
    constexpr int warmupSamples = 8192;
    constexpr float amplitude = 0.5f;

    processor.prepareToPlay (sampleRate, blockSize);
    const auto mainChannelCount = processor.getChannelCountOfBus (true, 0);
    const auto outputChannelCount = processor.getChannelCountOfBus (false, 0);
    const auto voiceOffset = mainChannelCount;
    juce::AudioBuffer<float> buffer (voiceOffset + 6, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    const auto phaseIncrement = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;
    double inputEnergy = 0.0;
    std::array<double, 6> outputEnergy {};

    for (int processed = 0; processed < totalSamples; processed += blockSize)
    {
        buffer.clear();

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = amplitude * static_cast<float> (std::sin (phase));
            phase += phaseIncrement;

            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            for (int channel = 0; channel < 6; ++channel)
            {
                if (channel < mainChannelCount)
                    buffer.setSample (channel, sample,
                                      mainChannels[static_cast<size_t> (channel)] * value);

                buffer.setSample (channel + voiceOffset, sample,
                                  voiceChannels[static_cast<size_t> (channel)]);
            }
        }

        processor.processBlock (buffer, midi);

        for (int sample = 0; sample < blockSize; ++sample)
        {
            if (processed + sample < warmupSamples)
                continue;

            const auto input = amplitude * std::sin (
                phase - phaseIncrement * static_cast<double> (blockSize - sample));
            inputEnergy += input * input;

            for (int channel = 0; channel < outputChannelCount; ++channel)
            {
                const auto output = static_cast<double> (buffer.getSample (channel, sample));
                outputEnergy[static_cast<size_t> (channel)] += output * output;
            }
        }
    }

    SpatialToneResult result;

    for (size_t channel = 0; channel < result.outputRatios.size(); ++channel)
        result.outputRatios[channel] = std::sqrt (outputEnergy[channel] / inputEnergy);

    return result;
}

ProcessorImpulseResult processImpulseWithLookahead (float lookaheadMs,
                                                     double sampleRate = 48000.0,
                                                     bool globalBypass = false)
{
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::lookaheadMs, lookaheadMs);
    setParameter (processor, smart_ducking::parameter_ids::globalBypass,
                  globalBypass ? 1.0f : 0.0f);
    processor.prepareToPlay (sampleRate, 1);

    ProcessorImpulseResult result;
    result.latency = processor.getLatencySamples();
    juce::AudioBuffer<float> buffer (8, 1);
    juce::MidiBuffer midi;

    for (int sample = 0; sample <= result.latency; ++sample)
    {
        buffer.clear();
        buffer.setSample (0, 0, sample == 0 ? 1.0f : 0.0f);
        processor.processBlock (buffer, midi);
        const auto output = buffer.getSample (0, 0);

        if (sample < result.latency)
            result.maximumBeforeLatency = juce::jmax (result.maximumBeforeLatency,
                                                       std::abs (output));
        else
            result.sampleAtLatency = output;
    }

    return result;
}

std::array<std::vector<float>, 2> renderDeterministicSignal (
    const std::vector<int>& blockPattern,
    bool nonRealtime = false)
{
    constexpr double sampleRate = 48000.0;
    constexpr int totalSamples = 16384;
    const auto maximumBlock = *std::max_element (blockPattern.begin(), blockPattern.end());
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::attackMs, 5.0f);
    setParameter (processor, smart_ducking::parameter_ids::holdMs, 20.0f);
    setParameter (processor, smart_ducking::parameter_ids::releaseMs, 80.0f);
    processor.prepareToPlay (sampleRate, maximumBlock);
    processor.setNonRealtime (nonRealtime);
    juce::MidiBuffer midi;
    std::array<std::vector<float>, 2> output;
    for (auto& channel : output)
        channel.resize (totalSamples);

    int processed = 0;
    size_t patternIndex = 0;
    while (processed < totalSamples)
    {
        const auto blockSize = juce::jmin (
            blockPattern[patternIndex++ % blockPattern.size()],
            totalSamples - processed);
        juce::AudioBuffer<float> buffer (8, blockSize);
        buffer.clear();

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = processed + sample;
            const auto phase = juce::MathConstants<double>::twoPi
                             * 997.0 * static_cast<double> (absoluteSample) / sampleRate;
            const auto input = 0.25f * static_cast<float> (std::sin (phase));
            buffer.setSample (0, sample, input);
            buffer.setSample (1, sample, 0.7f * input);
            const auto voice = absoluteSample >= 2048 && absoluteSample < 12288 ? 0.4f : 0.0f;
            buffer.setSample (2, sample, voice);
            buffer.setSample (3, sample, 0.8f * voice);
        }

        processor.processBlock (buffer, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < blockSize; ++sample)
                output[static_cast<size_t> (channel)]
                      [static_cast<size_t> (processed + sample)] = buffer.getSample (channel, sample);

        processed += blockSize;
    }

    return output;
}

int findWidebandOnsetSample (float lookaheadMs)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    constexpr int voiceStartSample = 4096;
    constexpr int totalSamples = 12288;
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::fftResolution, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::attackMs, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::holdMs, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 15.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::lookaheadMs, lookaheadMs);
    processor.prepareToPlay (sampleRate, blockSize);
    processor.notifyEditorOpened();

    juce::AudioBuffer<float> buffer (8, blockSize);
    juce::MidiBuffer midi;
    auto onset = -1;

    for (int processed = 0; processed < totalSamples && onset < 0; processed += blockSize)
    {
        buffer.clear();

        for (int sample = 0; sample < blockSize; ++sample)
        {
            buffer.setSample (0, sample, 0.25f);
            buffer.setSample (1, sample, 0.25f);

            if (processed + sample >= voiceStartSample)
            {
                buffer.setSample (2, sample, 1.0f);
                buffer.setSample (3, sample, 1.0f);
            }
        }

        processor.processBlock (buffer, midi);

        if (processor.getGainReductionCentreDb() > 5.0f)
            onset = processed + blockSize;
    }

    processor.notifyEditorClosed();
    return onset;
}

float maximumAbsoluteDifference (const std::vector<float>& left,
                                 const std::vector<float>& right)
{
    auto maximum = 0.0f;
    for (size_t index = 0; index < left.size(); ++index)
        maximum = std::max (maximum, std::abs (left[index] - right[index]));
    return maximum;
}

void testSpectralVectorMathBackend()
{
    namespace svm = smart_ducking::spectral_vector_math;
    expect (svm::getBackendSimdWidth (svm::Backend::vector) > 1,
            "xsimd vector-math backend selects a real SIMD architecture");

    auto powersMatch = true;
    auto smoothingMatches = true;
    auto normalGainMatches = true;
    auto scalarTailsMatchExactly = true;

    for (const auto bins : { 513, 1025, 2049, 4097 })
    {
        std::vector<float> powers (static_cast<size_t> (bins));
        std::vector<float> scalarDb (static_cast<size_t> (bins));
        std::vector<float> vectorDb (static_cast<size_t> (bins));

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto phase = static_cast<float> (bin) * 0.019f;
            powers[static_cast<size_t> (bin)] = bin % 97 == 0
                ? 0.0f
                : 1.0e-12f + 0.75f * std::abs (std::sin (phase));
        }

        svm::powersToDecibels (
            powers.data(), scalarDb.data(), bins, -160.0f, svm::Backend::scalar);
        svm::powersToDecibels (
            powers.data(), vectorDb.data(), bins, -160.0f, svm::Backend::vector);
        powersMatch = powersMatch
                   && maximumAbsoluteDifference (scalarDb, vectorDb) <= 1.0e-5f;
        scalarTailsMatchExactly = scalarTailsMatchExactly
                               && exactlyEqual (scalarDb.back(), vectorDb.back());

        std::vector<float> targets (static_cast<size_t> (bins));
        std::vector<float> scalarTemporal (static_cast<size_t> (bins));
        std::vector<float> vectorTemporal (static_cast<size_t> (bins));
        std::vector<float> scalarApplied (static_cast<size_t> (bins));
        std::vector<float> vectorApplied (static_cast<size_t> (bins));
        std::vector<float> scalarTransform (static_cast<size_t> (2 * bins));
        std::vector<float> vectorTransform (static_cast<size_t> (2 * bins));

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto index = static_cast<size_t> (bin);
            targets[index] = 12.0f * std::abs (std::sin (0.013f * bin));
            scalarTemporal[index] = 9.0f * std::abs (std::cos (0.017f * bin));
            vectorTemporal[index] = scalarTemporal[index];
            scalarTransform[2 * index] = 0.1f + 0.8f * std::sin (0.011f * bin);
            scalarTransform[2 * index + 1] = -0.2f + 0.7f * std::cos (0.007f * bin);
        }
        vectorTransform = scalarTransform;

        const svm::MaskSmoothingParameters smoothingParameters {
            0.73f, 0.91f, 0.67f, 3.25f, false
        };
        const auto scalarMaximum = svm::smoothMaskAndApplyGain (
            targets.data(),
            scalarTemporal.data(),
            scalarApplied.data(),
            scalarTransform.data(),
            bins,
            smoothingParameters,
            svm::Backend::scalar);
        const auto vectorMaximum = svm::smoothMaskAndApplyGain (
            targets.data(),
            vectorTemporal.data(),
            vectorApplied.data(),
            vectorTransform.data(),
            bins,
            smoothingParameters,
            svm::Backend::vector);
        smoothingMatches = smoothingMatches
                        && std::abs (scalarMaximum - vectorMaximum) <= 2.0e-6f
                        && maximumAbsoluteDifference (scalarTemporal, vectorTemporal)
                               <= 2.0e-6f
                        && maximumAbsoluteDifference (scalarApplied, vectorApplied)
                               <= 2.0e-6f
                        && maximumAbsoluteDifference (scalarTransform, vectorTransform)
                               <= 2.0e-6f;
        scalarTailsMatchExactly = scalarTailsMatchExactly
                               && exactlyEqual (scalarApplied.back(), vectorApplied.back())
                               && exactlyEqual (scalarTransform.back(), vectorTransform.back());

        std::vector<float> crossoverWeights (static_cast<size_t> (bins));
        std::vector<float> scalarReduction (static_cast<size_t> (bins));
        std::vector<float> vectorReduction (static_cast<size_t> (bins));
        scalarTransform.resize (static_cast<size_t> (2 * bins));

        for (int bin = 0; bin < bins; ++bin)
        {
            const auto index = static_cast<size_t> (bin);
            crossoverWeights[index] = static_cast<float> (bin)
                                    / static_cast<float> (bins - 1);
            scalarReduction[index] = 16.0f * std::abs (std::sin (0.021f * bin));
            scalarTransform[2 * index] = 0.15f + 0.6f * std::sin (0.005f * bin);
            scalarTransform[2 * index + 1] = -0.1f + 0.5f * std::cos (0.009f * bin);
        }
        vectorReduction = scalarReduction;
        vectorTransform = scalarTransform;

        const svm::NormalCrossoverParameters normalParameters {
            0.42f, 0.83f, 1.0e-12f, true
        };
        const auto scalarMinimum = svm::applyNormalCrossoverGain (
            crossoverWeights.data(),
            scalarReduction.data(),
            scalarTransform.data(),
            bins,
            normalParameters,
            svm::Backend::scalar);
        const auto vectorMinimum = svm::applyNormalCrossoverGain (
            crossoverWeights.data(),
            vectorReduction.data(),
            vectorTransform.data(),
            bins,
            normalParameters,
            svm::Backend::vector);
        normalGainMatches = normalGainMatches
                         && std::abs (scalarMinimum - vectorMinimum) <= 2.0e-6f
                         && maximumAbsoluteDifference (scalarReduction, vectorReduction)
                                <= 1.0e-5f
                         && maximumAbsoluteDifference (scalarTransform, vectorTransform)
                                <= 2.0e-6f;
        scalarTailsMatchExactly = scalarTailsMatchExactly
                               && exactlyEqual (scalarReduction.back(), vectorReduction.back())
                               && exactlyEqual (scalarTransform.back(), vectorTransform.back());
    }

    expect (powersMatch, "xsimd magnitude dB conversion nulls against scalar");
    expect (smoothingMatches, "xsimd spectral smoothing/gain nulls against scalar");
    expect (normalGainMatches, "xsimd NORMAL crossover/gain nulls against scalar");
    expect (scalarTailsMatchExactly,
            "xsimd kernels leave the final real-FFT bin on the bit-exact scalar tail");

    auto endToEndMaximumError = 0.0f;
    double endToEndErrorEnergy = 0.0;
    double endToEndReferenceEnergy = 0.0;

    for (const auto economyMode : { false, true })
    {
        for (int resolution = 0; resolution < 3; ++resolution)
        {
            smart_ducking::StereoSpectralLcrProcessor scalarProcessor;
            smart_ducking::StereoSpectralLcrProcessor vectorProcessor;
            scalarProcessor.setVectorMathBackendForTests (svm::Backend::scalar);
            vectorProcessor.setVectorMathBackendForTests (svm::Backend::vector);
            scalarProcessor.prepare (48000.0);
            vectorProcessor.prepare (48000.0);
            scalarProcessor.setEconomyMode (economyMode);
            vectorProcessor.setEconomyMode (economyMode);
            scalarProcessor.setResolutionIndex (resolution);
            vectorProcessor.setResolutionIndex (resolution);
            auto parameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
            parameters.attackMs = 0.0f;
            parameters.holdMs = 0.0f;
            parameters.releaseMs = 70.0f;
            parameters.duckDepthDb = 7.0f;
            parameters.amountDb = 14.0f;
            scalarProcessor.setParameters (parameters);
            vectorProcessor.setParameters (parameters);

            const auto samples = scalarProcessor.getLatencySamples() * 3;
            for (int sample = 0; sample < samples; ++sample)
            {
                const auto mainLeft = 0.31f * static_cast<float> (
                    std::sin (0.019 * static_cast<double> (sample)));
                const auto mainRight = 0.27f * static_cast<float> (
                    std::cos (0.023 * static_cast<double> (sample)));
                const auto voice = sample < samples * 2 / 3
                    ? 0.42f * static_cast<float> (
                        std::sin (0.031 * static_cast<double> (sample)))
                    : 0.0f;
                const auto scalar = scalarProcessor.processSample (
                    mainLeft, mainRight, voice, 0.73f * voice);
                const auto vector = vectorProcessor.processSample (
                    mainLeft, mainRight, voice, 0.73f * voice);

                for (int channel = 0; channel < 5; ++channel)
                {
                    const auto reference = scalar.get (channel);
                    const auto error = std::abs (vector.get (channel) - reference);
                    endToEndMaximumError = std::max (endToEndMaximumError, error);
                    endToEndErrorEnergy += static_cast<double> (error) * error;
                    endToEndReferenceEnergy += static_cast<double> (reference) * reference;
                }
            }
        }
    }

    const auto relativeRmsError = std::sqrt (
        endToEndErrorEnergy / std::max (1.0e-30, endToEndReferenceEnergy));
    expect (endToEndMaximumError <= 2.0e-5f && relativeRmsError <= 2.0e-5,
            "scalar and xsimd spectral processors null end-to-end in every mode/resolution");
}

void testRealFftBackend()
{
#if SMART_DUCKING_USE_PFFFT
    expect (smart_ducking::RealFft::getBackendSimdWidth() > 1,
            "PFFFT backend uses a SIMD implementation on supported release targets");
#endif

    auto spectraMatch = true;
    auto inverseMatches = true;

    for (int order = 9; order <= 13; ++order)
    {
        const auto size = 1 << order;
        smart_ducking::RealFft backend (order);
        juce::dsp::FFT referenceFft (order);
        std::vector<float> backendData (static_cast<size_t> (2 * size), 0.0f);
        std::vector<float> referenceData (static_cast<size_t> (2 * size), 0.0f);

        for (int sample = 0; sample < size; ++sample)
        {
            const auto position = static_cast<float> (sample);
            const auto value = 0.37f * std::sin (0.013f * position)
                             + 0.19f * std::cos (0.071f * position)
                             + (sample == 17 ? 0.5f : 0.0f);
            backendData[static_cast<size_t> (sample)] = value;
            referenceData[static_cast<size_t> (sample)] = value;
        }

        backend.forward (backendData.data());
        referenceFft.performRealOnlyForwardTransform (referenceData.data(), true);

        double spectralErrorSquared = 0.0;
        double spectralReferenceSquared = 0.0;
        auto maximumSpectralError = 0.0f;
        auto maximumSpectralReference = 0.0f;

        for (int value = 0; value <= size + 1; ++value)
        {
            const auto actual = backendData[static_cast<size_t> (value)];
            const auto expected = referenceData[static_cast<size_t> (value)];
            const auto error = std::abs (actual - expected);
            spectralErrorSquared += static_cast<double> (error) * error;
            spectralReferenceSquared += static_cast<double> (expected) * expected;
            maximumSpectralError = std::max (maximumSpectralError, error);
            maximumSpectralReference = std::max (maximumSpectralReference,
                                                 std::abs (expected));
        }

        const auto relativeSpectralRms = std::sqrt (
            spectralErrorSquared / std::max (1.0e-20, spectralReferenceSquared));
        spectraMatch = spectraMatch
                    && relativeSpectralRms < 2.0e-5
                    && maximumSpectralError
                           < 2.0e-4f * std::max (1.0f, maximumSpectralReference);

        for (int bin = 0; bin <= size / 2; ++bin)
        {
            const auto gain = 0.25f + 0.75f * static_cast<float> (bin)
                                        / static_cast<float> (size / 2);
            const auto offset = static_cast<size_t> (2 * bin);
            backendData[offset] *= gain;
            backendData[offset + 1] *= gain;
            referenceData[offset] *= gain;
            referenceData[offset + 1] *= gain;
        }

        backend.inverse (backendData.data());
        referenceFft.performRealOnlyInverseTransform (referenceData.data());

        double inverseErrorSquared = 0.0;
        double inverseReferenceSquared = 0.0;
        auto maximumInverseError = 0.0f;
        auto maximumInverseReference = 0.0f;

        for (int sample = 0; sample < size; ++sample)
        {
            const auto actual = backendData[static_cast<size_t> (sample)];
            const auto expected = referenceData[static_cast<size_t> (sample)];
            const auto error = std::abs (actual - expected);
            inverseErrorSquared += static_cast<double> (error) * error;
            inverseReferenceSquared += static_cast<double> (expected) * expected;
            maximumInverseError = std::max (maximumInverseError, error);
            maximumInverseReference = std::max (maximumInverseReference,
                                                std::abs (expected));
        }

        const auto relativeInverseRms = std::sqrt (
            inverseErrorSquared / std::max (1.0e-20, inverseReferenceSquared));
        inverseMatches = inverseMatches
                      && relativeInverseRms < 2.0e-5
                      && maximumInverseError
                             < 2.0e-4f * std::max (1.0f, maximumInverseReference);
    }

    expect (spectraMatch,
            "selected real FFT backend matches JUCE positive-frequency spectra");
    expect (inverseMatches,
            "selected real FFT backend matches JUCE after spectral-bin processing");
}

#if ! SMART_DUCKING_ENABLE_DIAGNOSTICS && SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH
void testSteadyActiveBlockPath (smart_ducking::PluginVariant variant,
                                bool normalMode,
                                bool downmix)
{
    constexpr double sampleRate = 48000.0;
    constexpr int maximumBlockSize = 512;
    SmartDuckingAudioProcessor blockProcessor (variant);
    SmartDuckingAudioProcessor sampleProcessor (variant);

    const auto configure = [normalMode, downmix] (auto& processor)
    {
        setParameter (processor,
                      smart_ducking::parameter_ids::processingMode,
                      normalMode ? 1.0f : 0.0f);
        setParameter (processor, smart_ducking::parameter_ids::fftResolution, 0.0f);
        setParameter (processor, smart_ducking::parameter_ids::crossoverHz, 73.0f);

        if (processor.isSurroundVariant())
        {
            setParameter (processor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
            setParameter (processor, smart_ducking::parameter_ids::mainChannelOrder, 1.0f);
            setParameter (processor, smart_ducking::parameter_ids::voiceChannelOrder, 2.0f);
            setParameter (processor, smart_ducking::parameter_ids::outputChannelOrder, 3.0f);
            setParameter (processor,
                          smart_ducking::parameter_ids::downmix,
                          downmix ? 1.0f : 0.0f);
        }
    };

    configure (blockProcessor);
    configure (sampleProcessor);
    blockProcessor.prepareToPlay (sampleRate, maximumBlockSize);
    sampleProcessor.prepareToPlay (sampleRate, maximumBlockSize);
    sampleProcessor.setSteadyActiveBlockPathEnabledForTesting (false);

    const auto mainChannels = blockProcessor.getChannelCountOfBus (true, 0);
    constexpr std::array<int, 7> blockSizes { 1, 7, 31, 64, 127, 255, 512 };
    juce::MidiBuffer blockMidi;
    juce::MidiBuffer sampleMidi;
    auto absoluteSample = 0;
    auto maximumError = 0.0f;
    double errorEnergy = 0.0;
    double referenceEnergy = 0.0;

    const auto processComparisonBlock = [&] (int numSamples)
    {
        juce::AudioBuffer<float> source (mainChannels + 6, numSamples);
        juce::AudioBuffer<float> blockBuffer (mainChannels + 6, numSamples);
        juce::AudioBuffer<float> sampleBuffer (mainChannels + 6, numSamples);
        source.clear();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto position = static_cast<double> (absoluteSample + sample);

            for (int channel = 0; channel < mainChannels; ++channel)
            {
                const auto value = 0.19f * static_cast<float> (std::sin (
                    0.017 * position + 0.31 * static_cast<double> (channel)))
                    + ((absoluteSample + sample + 37 * channel) % 911 == 0
                           ? 0.13f : 0.0f);
                source.setSample (channel, sample, value);
            }

            for (int channel = 0; channel < 6; ++channel)
            {
                const auto value = 0.23f * static_cast<float> (std::cos (
                    0.029 * position + 0.23 * static_cast<double> (channel)));
                source.setSample (mainChannels + channel, sample, value);
            }
        }

        blockBuffer.makeCopyOf (source, true);
        sampleBuffer.makeCopyOf (source, true);
        blockProcessor.processBlock (blockBuffer, blockMidi);
        sampleProcessor.processBlock (sampleBuffer, sampleMidi);

        for (int channel = 0; channel < mainChannels; ++channel)
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto actual = blockBuffer.getSample (channel, sample);
                const auto reference = sampleBuffer.getSample (channel, sample);
                const auto error = actual - reference;
                maximumError = std::max (maximumError, std::abs (error));
                errorEnergy += static_cast<double> (error) * error;
                referenceEnergy += static_cast<double> (reference) * reference;
            }

        absoluteSample += numSamples;
    };

    for (int block = 0; block < 28; ++block)
        processComparisonBlock (blockSizes[static_cast<size_t> (block)
                                           % blockSizes.size()]);

    const auto steadyBlocksBeforeAutomation = blockProcessor.getSteadyActiveBlockCount();
    expect (steadyBlocksBeforeAutomation > 0,
            "steady active block path engages for stable active processing");
    processComparisonBlock (maximumBlockSize + 1);
    expect (blockProcessor.getSteadyActiveBlockCount() == steadyBlocksBeforeAutomation,
            "blocks larger than the prepared scratch capacity use the sample fallback");

    if (normalMode)
    {
        setParameter (blockProcessor, smart_ducking::parameter_ids::lookaheadMs, 19.0f);
        setParameter (sampleProcessor, smart_ducking::parameter_ids::lookaheadMs, 19.0f);
    }
    else
    {
        setParameter (blockProcessor, smart_ducking::parameter_ids::crossoverHz, 118.0f);
        setParameter (sampleProcessor, smart_ducking::parameter_ids::crossoverHz, 118.0f);
    }

    processComparisonBlock (127);
    expect (blockProcessor.getSteadyActiveBlockCount() == steadyBlocksBeforeAutomation,
            normalMode
                ? "lookahead ramps fall back to sample processing for the whole block"
                : "crossover ramps fall back to sample processing for the whole block");

    for (int block = 0; block < 28; ++block)
        processComparisonBlock (128);

    expect (blockProcessor.getSteadyActiveBlockCount() > steadyBlocksBeforeAutomation,
            "steady active block path resumes after parameter ramps settle");

    if (variant == smart_ducking::PluginVariant::surround)
    {
        const auto steadyBlocksBeforeRouting = blockProcessor.getSteadyActiveBlockCount();
        setParameter (blockProcessor,
                      smart_ducking::parameter_ids::mainChannelOrder,
                      2.0f);
        setParameter (sampleProcessor,
                      smart_ducking::parameter_ids::mainChannelOrder,
                      2.0f);
        processComparisonBlock (64);
        expect (blockProcessor.getSteadyActiveBlockCount() == steadyBlocksBeforeRouting,
                "channel-routing transitions use the sample fallback");
    }

    const auto relativeRmsError = std::sqrt (
        errorEnergy / std::max (1.0e-30, referenceEnergy));
    expect (maximumError <= 1.0e-6f && relativeRmsError <= 2.0e-7,
            "steady block and generic active paths null across variable blocks and automation");
}
#endif
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuceGui;
    testRealFftBackend();
    testSpectralVectorMathBackend();

    {
        smart_ducking::PowerOfTwoDelayBuffer delay;
        constexpr auto maximumDelay = 37;
        delay.prepare (2, maximumDelay);
        expect (delay.getAllocatedSamples() == 128,
                "fixed delay trades bounded extra memory for power-of-two wrapping");

        std::array<float, 96> reference {};
        auto samplePathMatches = true;
        for (int sample = 0; sample < 96; ++sample)
        {
            const auto input = static_cast<float> (sample + 1);
            reference[static_cast<size_t> (sample)] = input;
            const auto taps = delay.processSample (0, input, 7, 31);
            const auto expectedFirst = sample >= 7
                ? reference[static_cast<size_t> (sample - 7)] : 0.0f;
            const auto expectedSecond = sample >= 31
                ? reference[static_cast<size_t> (sample - 31)] : 0.0f;
            samplePathMatches = samplePathMatches
                && approximatelyEqual (taps.first, expectedFirst)
                && approximatelyEqual (taps.second, expectedSecond);
        }
        expect (samplePathMatches,
                "fixed delay returns exact integer taps across ring wrapping");

        delay.reset();
        constexpr std::array<int, 6> blockSizes { 1, 5, 17, 31, 43, 63 };
        std::array<float, 63> input {};
        std::array<float, 63> output {};
        auto sourceIndex = 0;
        auto blockPathMatches = true;
        for (const auto blockSize : blockSizes)
        {
            for (int sample = 0; sample < blockSize; ++sample)
                input[static_cast<size_t> (sample)] = static_cast<float> (++sourceIndex);

            delay.processBlock (1, input.data(), output.data(), blockSize, 23);
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto absoluteSample = sourceIndex - blockSize + sample;
                const auto expected = absoluteSample >= 23
                    ? static_cast<float> (absoluteSample - 23 + 1) : 0.0f;
                blockPathMatches = blockPathMatches
                    && approximatelyEqual (output[static_cast<size_t> (sample)], expected);
            }
        }
        expect (blockPathMatches,
                "fixed delay block processing matches the sample timeline for variable blocks");

        smart_ducking::PowerOfTwoDelayBuffer sampleTwoTapDelay;
        smart_ducking::PowerOfTwoDelayBuffer blockTwoTapDelay;
        sampleTwoTapDelay.prepare (1, maximumDelay);
        blockTwoTapDelay.prepare (1, maximumDelay);
        std::array<float, 63> firstOutput {};
        std::array<float, 63> secondOutput {};
        auto twoTapBlockMatches = true;
        for (const auto firstDelay : { 0, 7 })
        {
            sampleTwoTapDelay.reset();
            blockTwoTapDelay.reset();
            sourceIndex = 0;

            for (const auto blockSize : blockSizes)
            {
                for (int sample = 0; sample < blockSize; ++sample)
                    input[static_cast<size_t> (sample)] = static_cast<float> (++sourceIndex)
                        * (sample % 2 == 0 ? 0.75f : -0.5f);

                blockTwoTapDelay.processBlockTwoTap (0,
                                                     input.data(),
                                                     firstOutput.data(),
                                                     secondOutput.data(),
                                                     blockSize,
                                                     firstDelay,
                                                     31);

                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto expected = sampleTwoTapDelay.processSample (
                        0, input[static_cast<size_t> (sample)], firstDelay, 31);
                    twoTapBlockMatches = twoTapBlockMatches
                        && exactlyEqual (firstOutput[static_cast<size_t> (sample)],
                                         expected.first)
                        && exactlyEqual (secondOutput[static_cast<size_t> (sample)],
                                         expected.second);
                }
            }
        }

        expect (twoTapBlockMatches,
                "two-tap block delay is sample-exact for zero/nonzero taps and variable blocks");
    }

    for (const auto slopeDbPerOctave : { 24.0f, 36.0f, 48.0f, 60.0f, 72.0f })
        for (const auto stereo : { true, false })
        {
            constexpr int totalSamples = 1537;
            constexpr std::array<int, 7> crossoverBlockSizes {
                1, 5, 17, 63, 128, 255, 511
            };
            smart_ducking::LinkwitzRileyCrossover sampleCrossover;
            smart_ducking::LinkwitzRileyCrossover blockCrossover;
            sampleCrossover.prepare (48000.0, 730.0f, slopeDbPerOctave);
            blockCrossover.prepare (48000.0, 730.0f, slopeDbPerOctave);
            std::array<std::vector<float>, 5> input;
            std::array<std::vector<float>, 5> sampleLow;
            std::array<std::vector<float>, 5> sampleHigh;
            std::array<std::vector<float>, 5> blockLow;
            std::array<std::vector<float>, 5> blockHigh;

            for (int channel = 0; channel < 5; ++channel)
            {
                input[static_cast<size_t> (channel)].resize (totalSamples);
                sampleLow[static_cast<size_t> (channel)].resize (totalSamples);
                sampleHigh[static_cast<size_t> (channel)].resize (totalSamples);
                blockLow[static_cast<size_t> (channel)].resize (totalSamples);
                blockHigh[static_cast<size_t> (channel)].resize (totalSamples);

                for (int sample = 0; sample < totalSamples; ++sample)
                    input[static_cast<size_t> (channel)][static_cast<size_t> (sample)] =
                        0.31f * static_cast<float> (std::sin (
                            0.023 * static_cast<double> (sample)
                            + 0.41 * static_cast<double> (channel)));
            }

            auto processed = 0;
            auto blockIndex = size_t { 0 };

            while (processed < totalSamples)
            {
                const auto blockSize = juce::jmin (
                    totalSamples - processed,
                    crossoverBlockSizes[blockIndex++ % crossoverBlockSizes.size()]);
                std::array<const float*, 5> inputPointers {};
                std::array<float*, 5> lowPointers {};
                std::array<float*, 5> highPointers {};

                for (int channel = 0; channel < 5; ++channel)
                {
                    if (stereo && channel != 0 && channel != 2)
                        continue;

                    const auto index = static_cast<size_t> (channel);
                    inputPointers[index] = input[index].data() + processed;
                    lowPointers[index] = blockLow[index].data() + processed;
                    highPointers[index] = blockHigh[index].data() + processed;
                }

                blockCrossover.processSteadyBlock (inputPointers,
                                                   lowPointers,
                                                   highPointers,
                                                   blockSize,
                                                   stereo);

                for (int sample = 0; sample < blockSize; ++sample)
                {
                    sampleCrossover.beginSample();
                    for (int channel = 0; channel < 5; ++channel)
                    {
                        if (stereo && channel != 0 && channel != 2)
                            continue;

                        const auto index = static_cast<size_t> (channel);
                        const auto bands = sampleCrossover.processSample (
                            channel, input[index][static_cast<size_t> (processed + sample)]);
                        sampleLow[index][static_cast<size_t> (processed + sample)] = bands.low;
                        sampleHigh[index][static_cast<size_t> (processed + sample)] = bands.high;
                    }
                }

                processed += blockSize;
            }

            auto crossoverBlockMatches = true;
            auto maximumCrossoverBlockError = 0.0f;
            for (int channel = 0; channel < 5; ++channel)
            {
                if (stereo && channel != 0 && channel != 2)
                    continue;

                for (int sample = 0; sample < totalSamples; ++sample)
                {
                    const auto index = static_cast<size_t> (channel);
                    const auto sampleIndex = static_cast<size_t> (sample);
                    crossoverBlockMatches = crossoverBlockMatches
                        && std::isfinite (blockLow[index][sampleIndex])
                        && std::isfinite (blockHigh[index][sampleIndex]);
                    maximumCrossoverBlockError = std::max ({
                        maximumCrossoverBlockError,
                        std::abs (blockLow[index][sampleIndex]
                                  - sampleLow[index][sampleIndex]),
                        std::abs (blockHigh[index][sampleIndex]
                                  - sampleHigh[index][sampleIndex])
                    });
                }
            }
            crossoverBlockMatches = crossoverBlockMatches
                                 && maximumCrossoverBlockError <= 1.0e-7f;

            expect (crossoverBlockMatches,
                    "SIMD Linkwitz-Riley block kernel nulls against the sample path");
            blockCrossover.setCutoffHz (1180.0f);
            expect (! blockCrossover.canProcessSteadyBlock(),
                    "Linkwitz-Riley block kernel rejects cutoff automation");

            smart_ducking::LinkwitzRileyCrossover slopeTransitionCrossover;
            slopeTransitionCrossover.prepare (48000.0, 730.0f, slopeDbPerOctave);
            slopeTransitionCrossover.setSlopeDbPerOctave (
                slopeDbPerOctave < 72.0f ? 72.0f : 24.0f);
            expect (! slopeTransitionCrossover.canProcessSteadyBlock(),
                    "Linkwitz-Riley block kernel rejects slope transitions");
        }

#if ! SMART_DUCKING_ENABLE_DIAGNOSTICS && SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH
    testSteadyActiveBlockPath (smart_ducking::PluginVariant::stereo, false, false);
    testSteadyActiveBlockPath (smart_ducking::PluginVariant::stereo, true, false);
    testSteadyActiveBlockPath (smart_ducking::PluginVariant::surround, false, false);
    testSteadyActiveBlockPath (smart_ducking::PluginVariant::surround, true, true);
#endif

    SmartDuckingAudioProcessor processor;
    SmartDuckingAudioProcessor surroundProcessor (smart_ducking::PluginVariant::surround);

    for (const auto slopeDbPerOctave : { 24.0f, 36.0f, 48.0f, 60.0f, 72.0f })
    {
        const auto lowAtCutoff = smart_ducking::crossoverLowMagnitude (
            100.0f, 100.0f, slopeDbPerOctave);
        const auto highAtCutoff = smart_ducking::crossoverHighMagnitude (
            100.0f, 100.0f, slopeDbPerOctave);
        expect (approximatelyEqual (lowAtCutoff, 0.5f)
                    && approximatelyEqual (highAtCutoff, 0.5f),
                "every displayed crossover slope intersects at complementary -6.02 dB");

        for (const auto frequency : { 20.0f, 43.5f, 100.0f, 229.7f, 20000.0f })
        {
            const auto low = smart_ducking::crossoverLowMagnitude (
                frequency, 100.0f, slopeDbPerOctave);
            const auto high = smart_ducking::crossoverHighMagnitude (
                frequency, 100.0f, slopeDbPerOctave);
            expect (approximatelyEqual (low + high, 1.0f),
                    "displayed crossover branches stay magnitude-complementary across the plot");
        }
    }

    for (const auto slopeDbPerOctave : { 24.0f, 36.0f, 48.0f })
    {
        const auto formerTemplateEdgeHz = 100.0f * std::pow (2.0f, 1.2f);
        const auto lowAtFormerEdge = smart_ducking::crossoverLowMagnitude (
            formerTemplateEdgeHz, 100.0f, slopeDbPerOctave);
        expect (lowAtFormerEdge
                    > juce::Decibels::decibelsToGain (-60.0f),
                "shallower crossover curves are not truncated at the former template edge");
    }

    expect (processor.getName() == "Smart Ducking (Stereo)", "stereo plugin name");
    expect (surroundProcessor.getName() == "Smart Ducking (Surround)", "surround plugin name");
    expect (approximatelyEqual (static_cast<float> (processor.getTailLengthSeconds()), 2.0f),
            "finite two-second tail is reported to the host");
    expect (processor.getBusCount (true) == 2, "main plus sidechain input buses");
    expect (processor.getBusCount (false) == 1, "one output bus");
    expect (processor.getChannelCountOfBus (true, 0) == 2, "stereo variant Main is stereo");
    expect (processor.getChannelCountOfBus (true, 1) == 6, "stereo variant Voice is physical 5.1");
    expect (processor.getChannelCountOfBus (false, 0) == 2, "stereo variant output is stereo");
    expect (surroundProcessor.getChannelCountOfBus (true, 0) == 6,
            "surround variant Main is 5.1");
    expect (surroundProcessor.getChannelCountOfBus (true, 1) == 6,
            "surround variant Voice is physical 5.1");
    expect (surroundProcessor.getChannelCountOfBus (false, 0) == 6,
            "surround variant output is 5.1");

    auto monoSidechainLayout = processor.getBusesLayout();
    monoSidechainLayout.inputBuses.set (1, juce::AudioChannelSet::mono());
    expect (! processor.isBusesLayoutSupported (monoSidechainLayout), "physical mono sidechain layout is rejected");

    auto invalidMainLayout = processor.getBusesLayout();
    invalidMainLayout.inputBuses.set (0, juce::AudioChannelSet::mono());
    expect (! processor.isBusesLayoutSupported (invalidMainLayout), "non-stereo Main is rejected by Stereo");

    auto stereoLayoutOnSurround = surroundProcessor.getBusesLayout();
    stereoLayoutOnSurround.inputBuses.set (0, juce::AudioChannelSet::stereo());
    stereoLayoutOnSurround.outputBuses.set (0, juce::AudioChannelSet::stereo());
    expect (! surroundProcessor.isBusesLayoutSupported (stereoLayoutOnSurround),
            "Stereo Main/Output is rejected by Surround");

    const auto& state = processor.getParameterState();
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::crossoverHz), 92.0f),
            "crossover default");
    if (const auto* crossoverSlope = state.getParameter (
            smart_ducking::parameter_ids::crossoverSlope))
    {
        expect (crossoverSlope->getAllValueStrings()
                    == juce::StringArray { "24 dB/oct", "36 dB/oct", "48 dB/oct",
                                           "60 dB/oct", "72 dB/oct" }
                    && approximatelyEqual (
                        *state.getRawParameterValue (
                            smart_ducking::parameter_ids::crossoverSlope),
                        2.0f),
                "Crossover Slope exposes five choices and defaults to 48 dB/oct");
    }
    else
    {
        expect (false, "Crossover Slope parameter exists");
    }
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::duckDepthDb), 10.0f),
            "duck depth default");
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::matrixLC), 0.25f),
            "L to C matrix default");
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::matrixCL), 0.10f),
            "C to L matrix default");
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::matrixCR), 0.10f),
            "C to R matrix default");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::activityThresholdDb),
                -30.0f),
            "voice full threshold default");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::lookaheadMs),
                10.0f),
            "lookahead default");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::processingMode),
                1.0f),
            "NORMAL processing mode default");
    if (const auto* weightGain = state.getParameter (
            smart_ducking::parameter_ids::weightGainDb))
    {
        expect (approximatelyEqual (weightGain->convertFrom0to1 (0.0f), -12.0f)
                    && approximatelyEqual (weightGain->convertFrom0to1 (1.0f), 12.0f)
                    && approximatelyEqual (*state.getRawParameterValue (
                                                smart_ducking::parameter_ids::weightGainDb),
                                            6.0f),
                "Weight Gain spans negative and positive 12 dB around zero");
    }
    else
    {
        expect (false, "Weight Gain parameter exists");
    }
    if (const auto* weightFrequency = state.getParameter (
            smart_ducking::parameter_ids::weightFrequencyHz))
    {
        expect (approximatelyEqual (weightFrequency->convertFrom0to1 (0.0f), 500.0f, 0.1f)
                    && approximatelyEqual (weightFrequency->convertFrom0to1 (1.0f),
                                            20000.0f,
                                            0.1f)
                    && approximatelyEqual (*state.getRawParameterValue (
                                                smart_ducking::parameter_ids::weightFrequencyHz),
                                            3000.0f,
                                            0.1f),
                "Weight Frequency spans 500 Hz to the 20 kHz analyser limit");
    }
    else
    {
        expect (false, "Weight Frequency parameter exists");
    }
    expect (approximatelyEqual (
                smart_ducking::StereoSpectralLcrProcessor::getDisplayMaximumFrequency(),
                20000.0f),
            "analyser always ends at 20 kHz");
    expect (state.getParameter (smart_ducking::parameter_ids::mainFormat) == nullptr,
            "Main Format is fixed by plugin variant rather than exposed as a parameter");
    expect (state.getParameter ("sidechainPreGainDb") == nullptr,
            "legacy Sidechain Pre-Gain is no longer exposed to the host");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::crossoverBypass),
                0.0f)
                && approximatelyEqual (
                    *state.getRawParameterValue (smart_ducking::parameter_ids::widebandBypass),
                    0.0f)
                && approximatelyEqual (
                    *state.getRawParameterValue (smart_ducking::parameter_ids::spectralBypass),
                    0.0f),
            "all stage bypass parameters default to off");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::globalBypass),
                0.0f)
                && processor.getBypassParameter()
                       == state.getParameter (smart_ducking::parameter_ids::globalBypass),
            "global bypass defaults off and is exposed as the host bypass parameter");
    expect (approximatelyEqual (
                *state.getRawParameterValue (smart_ducking::parameter_ids::monitorMode),
                0.0f),
            "Monitor Mode defaults to Normal");

    if (const auto* processingMode = state.getParameter (
            smart_ducking::parameter_ids::processingMode))
    {
        const auto choices = processingMode->getAllValueStrings();
        expect (choices.size() == 2 && choices[0] == "QUALITY" && choices[1] == "NORMAL",
                "host Processing Mode choices match the custom editor names");
    }
    else
    {
        expect (false, "Processing Mode parameter exists");
    }

    if (const auto* monitorMode = state.getParameter (
            smart_ducking::parameter_ids::monitorMode))
    {
        const auto choices = monitorMode->getAllValueStrings();
        expect (choices == juce::StringArray { "Normal", "Crossover Low",
                                               "Crossover High", "Wideband Solo",
                                               "Wideband Delta", "Spectral Solo",
                                               "Spectral Delta" },
                "host Monitor Mode exposes complementary Crossover Low/High choices");
    }
    else
    {
        expect (false, "Monitor Mode parameter exists");
    }

    for (const auto* channelOrderId : {
             smart_ducking::parameter_ids::mainChannelOrder,
             smart_ducking::parameter_ids::voiceChannelOrder,
             smart_ducking::parameter_ids::outputChannelOrder })
    {
        if (const auto* channelOrder = state.getParameter (channelOrderId))
        {
            expect (channelOrder->getAllValueStrings()
                        == juce::StringArray { "ITU / SMPTE", "REAPER / VLC",
                                               "Film", "DTS / MPEG-D" }
                        && approximatelyEqual (
                            *state.getRawParameterValue (channelOrderId),
                            0.0f),
                    "5.1 channel-order parameters expose four formats and default to ITU / SMPTE");
        }
        else
        {
            expect (false, "5.1 channel-order parameter exists");
        }
    }
    expect (smart_ducking::getMeterChannelsForOrder (
                smart_ducking::SurroundChannelOrder::ituSmpte)
                == std::array<int, 5> { 0, 2, 1, 3, 4 }
                && smart_ducking::getMeterChannelsForOrder (
                    smart_ducking::SurroundChannelOrder::reaperVlc)
                       == std::array<int, 5> { 0, 2, 3, 4, 1 }
                && smart_ducking::getMeterChannelsForOrder (
                    smart_ducking::SurroundChannelOrder::film)
                       == std::array<int, 5> { 0, 1, 2, 3, 4 }
                && smart_ducking::getMeterChannelsForOrder (
                    smart_ducking::SurroundChannelOrder::dtsMpegD)
                       == std::array<int, 5> { 1, 0, 2, 3, 4 },
            "surround meter columns and labels follow each selected physical channel order");

    {
        SmartDuckingAudioProcessor presetProcessor;
        setParameter (presetProcessor, smart_ducking::parameter_ids::globalBypass, 1.0f);
        expect (presetProcessor.getNumPrograms() == 4,
                "four factory programs are exposed to the host");
        expect (presetProcessor.getProgramName (0) == "Default"
                    && presetProcessor.getProgramName (1) == "Gentle Dialogue"
                    && presetProcessor.getProgramName (2) == "Voice Focus"
                    && presetProcessor.getProgramName (3) == "Fast Announcer",
                "factory program names are stable");
        presetProcessor.setCurrentProgram (1);
        expect (presetProcessor.getCurrentProgram() == 1
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::duckDepthDb),
                        8.0f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::releaseMs),
                        320.0f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::globalBypass),
                        1.0f),
                "factory programs apply documented values without changing global bypass");
        presetProcessor.setCurrentProgram (3);
        expect (approximatelyEqual (
                    *presetProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::attackMs),
                    10.0f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::lookaheadMs),
                        20.0f),
                "Fast Announcer factory program applies its documented values");

        setParameter (presetProcessor, smart_ducking::parameter_ids::duckDepthDb, 6.5f);
        setParameter (presetProcessor, smart_ducking::parameter_ids::widebandBypass, 1.0f);
        setParameter (presetProcessor,
                      smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverHigh));
        const auto userPresetState = presetProcessor.createCurrentPresetState();
        expect (userPresetState.hasType ("SmartDuckingPreset")
                    && ! userPresetState.hasProperty (
                        smart_ducking::parameter_ids::globalBypass)
                    && ! userPresetState.hasProperty (
                        smart_ducking::parameter_ids::monitorMode),
                "user preset snapshots omit global bypass and transient monitoring");

        setParameter (presetProcessor, smart_ducking::parameter_ids::duckDepthDb, 17.0f);
        setParameter (presetProcessor, smart_ducking::parameter_ids::widebandBypass, 0.0f);
        expect (presetProcessor.applyPresetState (userPresetState)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::duckDepthDb),
                        6.5f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::widebandBypass),
                        1.0f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::monitorMode),
                        static_cast<float> (smart_ducking::MonitorMode::normal))
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::globalBypass),
                        1.0f)
                    && presetProcessor.doesPresetStateMatch (userPresetState),
                "user preset recall restores processing, resets Monitor Mode, and preserves global bypass");

        juce::ValueTree partialPreset ("SmartDuckingPreset");
        partialPreset.setProperty (smart_ducking::parameter_ids::duckDepthDb, 4.0f, nullptr);
        setParameter (presetProcessor, smart_ducking::parameter_ids::releaseMs, 900.0f);
        expect (presetProcessor.applyPresetState (partialPreset)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::duckDepthDb),
                        4.0f)
                    && approximatelyEqual (
                        *presetProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::releaseMs),
                        200.0f)
                    && presetProcessor.doesPresetStateMatch (partialPreset),
                "older partial presets use current defaults for parameters they do not contain");
    }

    {
        const auto temporaryPresetDirectory =
            juce::File::getCurrentWorkingDirectory()
                .getNonexistentChildFile ("smart-ducking-preset-tests", {}, true);
        const auto directoryResult = temporaryPresetDirectory.createDirectory();
        expect (directoryResult.wasOk(), "temporary user-preset directory is created");

        smart_ducking::UserPresetStore store (temporaryPresetDirectory);
        SmartDuckingAudioProcessor presetProcessor;
        setParameter (presetProcessor, smart_ducking::parameter_ids::duckDepthDb, 7.25f);
        const auto alphaState = presetProcessor.createCurrentPresetState();
        expect (store.save ("Zulu", alphaState).wasOk()
                    && store.save ("Alpha", alphaState).wasOk(),
                "user presets are written atomically");
        const auto corruptPreset = temporaryPresetDirectory.getChildFile (
            "corrupt.smartduckingpreset");
        expect (corruptPreset.replaceWithText ("not xml"),
                "corrupt preset fixture is written");

        auto loadedPresets = store.loadAll();
        expect (loadedPresets.size() == 2
                    && loadedPresets[0].name == "Alpha"
                    && loadedPresets[1].name == "Zulu"
                    && approximatelyEqual (static_cast<float> (
                        loadedPresets[0].state.getProperty (
                            smart_ducking::parameter_ids::duckDepthDb)),
                        7.25f),
                "user preset discovery ignores corrupt files and sorts valid names");

        setParameter (presetProcessor, smart_ducking::parameter_ids::duckDepthDb, 9.5f);
        expect (store.save ("Alpha", presetProcessor.createCurrentPresetState()).wasOk(),
                "saving an existing user preset replaces it");
        loadedPresets = store.loadAll();
        expect (loadedPresets.size() == 2
                    && approximatelyEqual (static_cast<float> (
                        loadedPresets[0].state.getProperty (
                            smart_ducking::parameter_ids::duckDepthDb)),
                        9.5f),
                "replaced user preset contains the latest settings");

        const auto removed = loadedPresets.size() > 1 && store.remove (loadedPresets[1]);
        expect (removed && store.loadAll().size() == 1,
                "a selected user preset can be deleted");
        expect (temporaryPresetDirectory.deleteRecursively(),
                "temporary user-preset directory is cleaned up");
    }

    {
        SmartDuckingAudioProcessor comparisonProcessor;
        setParameter (comparisonProcessor, smart_ducking::parameter_ids::globalBypass, 1.0f);
        setParameter (comparisonProcessor, smart_ducking::parameter_ids::duckDepthDb, 6.0f);
        comparisonProcessor.switchComparisonSlot (1);
        expect (comparisonProcessor.getComparisonSlot() == 1
                    && approximatelyEqual (
                        *comparisonProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::duckDepthDb),
                        10.0f),
                "switching to B stores A and recalls the initial B snapshot");
        setParameter (comparisonProcessor, smart_ducking::parameter_ids::duckDepthDb, 12.0f);
        comparisonProcessor.switchComparisonSlot (0);
        expect (approximatelyEqual (
                    *comparisonProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::duckDepthDb),
                    6.0f),
                "A/B comparison recalls A");
        comparisonProcessor.switchComparisonSlot (1);
        expect (approximatelyEqual (
                    *comparisonProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::duckDepthDb),
                    12.0f)
                    && approximatelyEqual (
                        *comparisonProcessor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::globalBypass),
                        1.0f),
                "A/B comparison recalls B without changing global bypass");

        juce::MemoryBlock comparisonState;
        comparisonProcessor.getStateInformation (comparisonState);
        SmartDuckingAudioProcessor restoredComparison;
        restoredComparison.setStateInformation (comparisonState.getData(),
                                                 static_cast<int> (comparisonState.getSize()));
        restoredComparison.switchComparisonSlot (0);
        const auto restoredA = restoredComparison.getParameterState().getRawParameterValue (
            smart_ducking::parameter_ids::duckDepthDb)->load();
        restoredComparison.switchComparisonSlot (1);
        const auto restoredB = restoredComparison.getParameterState().getRawParameterValue (
            smart_ducking::parameter_ids::duckDepthDb)->load();
        expect (approximatelyEqual (restoredA, 6.0f)
                    && approximatelyEqual (restoredB, 12.0f),
                "both A/B snapshots survive project-state round-trip");
    }

    {
        const auto configureWideband = [] (SmartDuckingAudioProcessor& stageProcessor)
        {
            setParameter (stageProcessor, smart_ducking::parameter_ids::processingMode, 1.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::duckDepthDb, 12.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        };

        SmartDuckingAudioProcessor wet;
        configureWideband (wet);
        const auto wetResult = processTone (wet, 1000.0, 1.0f, 1.0f);

        SmartDuckingAudioProcessor bypassed;
        configureWideband (bypassed);
        setParameter (bypassed, smart_ducking::parameter_ids::widebandBypass, 1.0f);
        const auto bypassedResult = processTone (bypassed, 1000.0, 1.0f, 1.0f);
        expect (wetResult.leftRatio < 0.55 && bypassedResult.leftRatio > 0.95,
                "Wideband Bypass disables broadband attenuation");

        SmartDuckingAudioProcessor globallyBypassed;
        configureWideband (globallyBypassed);
        setParameter (globallyBypassed, smart_ducking::parameter_ids::globalBypass, 1.0f);
        const auto globallyBypassedResult = processTone (
            globallyBypassed, 1000.0, 1.0f, 1.0f);
        expect (globallyBypassedResult.leftRatio > 0.95
                    && globallyBypassedResult.rightRatio > 0.95,
                "global bypass returns the latency-aligned dry Main signal");

        SmartDuckingAudioProcessor allStagesBypassed;
        configureWideband (allStagesBypassed);
        setParameter (allStagesBypassed, smart_ducking::parameter_ids::crossoverBypass, 1.0f);
        setParameter (allStagesBypassed, smart_ducking::parameter_ids::widebandBypass, 1.0f);
        setParameter (allStagesBypassed, smart_ducking::parameter_ids::spectralBypass, 1.0f);
        const auto allStagesBypassedResult = processTone (
            allStagesBypassed, 1000.0, 1.0f, 1.0f);
        expect (allStagesBypassedResult.leftRatio > 0.95
                    && allStagesBypassed.getProcessedFftFrameCount() == 0,
                "bypassing every ducking stage skips the FFT path");

        SmartDuckingAudioProcessor solo;
        configureWideband (solo);
        setParameter (solo, smart_ducking::parameter_ids::widebandBypass, 1.0f);
        setParameter (solo, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::widebandSolo));
        const auto soloResult = processTone (solo, 1000.0, 1.0f, 1.0f);
        expect (soloResult.leftRatio < 0.55,
                "Wideband Solo overrides bypass and bypasses the other stages");

        SmartDuckingAudioProcessor delta;
        configureWideband (delta);
        setParameter (delta, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::widebandDelta));
        const auto deltaResult = processTone (delta, 1000.0, 1.0f, 1.0f);
        expect (deltaResult.leftRatio > 0.35 && deltaResult.leftRatio < 0.9,
                "Wideband Delta returns the signal removed by the wideband stage");

        constexpr int bypassBlockSize = 64;
        SmartDuckingAudioProcessor smoothGlobalBypass;
        configureWideband (smoothGlobalBypass);
        smoothGlobalBypass.prepareToPlay (48000.0, bypassBlockSize);
        juce::AudioBuffer<float> bypassBuffer (8, bypassBlockSize);
        juce::MidiBuffer bypassMidi;
        auto previousOutput = 0.0f;

        for (int block = 0; block < 200; ++block)
        {
            bypassBuffer.clear();
            for (int sample = 0; sample < bypassBlockSize; ++sample)
            {
                bypassBuffer.setSample (0, sample, 0.25f);
                bypassBuffer.setSample (1, sample, 0.25f);
                bypassBuffer.setSample (2, sample, 1.0f);
                bypassBuffer.setSample (3, sample, 1.0f);
            }

            smoothGlobalBypass.processBlock (bypassBuffer, bypassMidi);
            previousOutput = bypassBuffer.getSample (0, bypassBlockSize - 1);
        }

        expect (! smoothGlobalBypass.isGlobalBypassDspSleeping()
                    && smoothGlobalBypass.isHeavyPathActive(),
                "active voice keeps the ducking DSP awake before global bypass");
        setParameter (smoothGlobalBypass, smart_ducking::parameter_ids::globalBypass, 1.0f);
        auto maximumBypassStep = 0.0f;
        for (int block = 0; block < 32; ++block)
        {
            bypassBuffer.clear();
            for (int sample = 0; sample < bypassBlockSize; ++sample)
            {
                bypassBuffer.setSample (0, sample, 0.25f);
                bypassBuffer.setSample (1, sample, 0.25f);
                bypassBuffer.setSample (2, sample, 1.0f);
                bypassBuffer.setSample (3, sample, 1.0f);
            }

            smoothGlobalBypass.processBlock (bypassBuffer, bypassMidi);
            for (int sample = 0; sample < bypassBlockSize; ++sample)
            {
                const auto currentOutput = bypassBuffer.getSample (0, sample);
                maximumBypassStep = juce::jmax (maximumBypassStep,
                                                std::abs (currentOutput - previousOutput));
                previousOutput = currentOutput;
            }
        }

        expect (maximumBypassStep < 0.01f && previousOutput > 0.249f,
                "global bypass crossfades to the dry path without a discontinuity");
        expect (smoothGlobalBypass.isGlobalBypassDspSleeping()
                    && ! smoothGlobalBypass.isHeavyPathActive(),
                "settled global bypass suspends the crossover, detector, FFT and matrix path");

        setParameter (smoothGlobalBypass, smart_ducking::parameter_ids::globalBypass, 0.0f);
        const auto wakeBlocks = (smoothGlobalBypass.getLatencySamples()
                                 + static_cast<int> (0.030 * 48000.0))
                              / bypassBlockSize
                              + 4;
        for (int block = 0; block < wakeBlocks; ++block)
        {
            bypassBuffer.clear();
            for (int sample = 0; sample < bypassBlockSize; ++sample)
            {
                bypassBuffer.setSample (0, sample, 0.25f);
                bypassBuffer.setSample (1, sample, 0.25f);
                bypassBuffer.setSample (2, sample, 1.0f);
                bypassBuffer.setSample (3, sample, 1.0f);
            }
            smoothGlobalBypass.processBlock (bypassBuffer, bypassMidi);
        }
        expect (! smoothGlobalBypass.isGlobalBypassDspSleeping()
                    && smoothGlobalBypass.isHeavyPathActive(),
                "leaving global bypass warms the latency-aligned wet path before fading it in");
    }

    {
        const auto configureLowProtection = [] (SmartDuckingAudioProcessor& stageProcessor)
        {
            setParameter (stageProcessor, smart_ducking::parameter_ids::processingMode, 1.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::duckDepthDb, 12.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        };

        SmartDuckingAudioProcessor protectedLow;
        configureLowProtection (protectedLow);
        const auto protectedResult = processTone (protectedLow, 40.0, 1.0f, 1.0f);

        SmartDuckingAudioProcessor crossoverBypassed;
        configureLowProtection (crossoverBypassed);
        setParameter (crossoverBypassed,
                      smart_ducking::parameter_ids::crossoverBypass,
                      1.0f);
        const auto bypassedResult = processTone (crossoverBypassed, 40.0, 1.0f, 1.0f);
        expect (protectedResult.leftRatio > 0.9 && bypassedResult.leftRatio < 0.55,
                "Crossover Bypass disables low-band protection");

        SmartDuckingAudioProcessor crossoverHigh;
        configureLowProtection (crossoverHigh);
        setParameter (crossoverHigh,
                      smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverHigh));
        const auto highResult = processTone (crossoverHigh, 1000.0, 1.0f, 1.0f);
        expect (highResult.leftRatio > 0.95,
                "Crossover High bypasses Wideband and Spectral while retaining alignment");
    }

    {
        const auto configureSpectral = [] (SmartDuckingAudioProcessor& stageProcessor)
        {
            setParameter (stageProcessor, smart_ducking::parameter_ids::processingMode, 1.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
            setParameter (stageProcessor, smart_ducking::parameter_ids::spectralAmountDb, 12.0f);
        };

        SmartDuckingAudioProcessor wet;
        configureSpectral (wet);
        const auto wetResult = processTone (wet, 3000.0, 1.0f, 1.0f);

        SmartDuckingAudioProcessor bypassed;
        configureSpectral (bypassed);
        setParameter (bypassed, smart_ducking::parameter_ids::spectralBypass, 1.0f);
        const auto bypassedResult = processTone (bypassed, 3000.0, 1.0f, 1.0f);
        expect (wetResult.leftRatio < bypassedResult.leftRatio - 0.1
                    && bypassedResult.leftRatio > 0.95,
                "Spectral Bypass disables spectral attenuation");

        SmartDuckingAudioProcessor delta;
        configureSpectral (delta);
        setParameter (delta, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::spectralDelta));
        const auto deltaResult = processTone (delta, 3000.0, 1.0f, 1.0f);
        expect (deltaResult.leftRatio > 0.05
                    && approximatelyEqual (static_cast<float> (
                                                wetResult.leftRatio
                                                + deltaResult.leftRatio),
                                            1.0f,
                                            0.12f),
                "Spectral Delta returns the signal removed by the spectral stage");
    }

    for (const auto processingMode : { 0.0f, 1.0f })
    {
        SmartDuckingAudioProcessor lowMonitor;
        setParameter (lowMonitor, smart_ducking::parameter_ids::processingMode, processingMode);
        setParameter (lowMonitor, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverLow));
        setParameter (lowMonitor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (lowMonitor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto lowPass = processTone (lowMonitor, 40.0, 0.0f, 0.0f);
        const auto lowReject = processTone (lowMonitor, 1000.0, 0.0f, 0.0f);

        SmartDuckingAudioProcessor highMonitor;
        setParameter (highMonitor, smart_ducking::parameter_ids::processingMode, processingMode);
        setParameter (highMonitor, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverHigh));
        setParameter (highMonitor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (highMonitor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto highReject = processTone (highMonitor, 40.0, 0.0f, 0.0f);
        const auto highPass = processTone (highMonitor, 1000.0, 0.0f, 0.0f);
        expect (lowPass.leftRatio > 0.9 && lowReject.leftRatio < 0.05,
                processingMode < 0.5f
                    ? "QUALITY Crossover Low monitors only the protected low band"
                    : "NORMAL Crossover Low monitors the complementary FFT low band");
        expect (highReject.leftRatio < 0.05 && highPass.leftRatio > 0.9,
                processingMode < 0.5f
                    ? "QUALITY Crossover High monitors only the processed high band"
                    : "NORMAL Crossover High monitors the complementary FFT high band");
        expect (lowMonitor.getLatencySamples() == highMonitor.getLatencySamples(),
                "monitoring does not change PDC");
    }


    {
        const auto liveLowPass = processToneAfterMonitorSwitch (
            smart_ducking::MonitorMode::crossoverLow, 40.0);
        const auto liveLowReject = processToneAfterMonitorSwitch (
            smart_ducking::MonitorMode::crossoverLow, 1000.0);
        const auto liveHighReject = processToneAfterMonitorSwitch (
            smart_ducking::MonitorMode::crossoverHigh, 40.0);
        const auto liveHighPass = processToneAfterMonitorSwitch (
            smart_ducking::MonitorMode::crossoverHigh, 1000.0);
        expect (liveLowPass > 0.9 && liveLowReject < 0.05,
                "NORMAL live switch to Crossover Low outputs only the FFT low branch");
        expect (liveHighReject < 0.05 && liveHighPass > 0.9,
                "NORMAL live switch to Crossover High outputs only the FFT high branch");
    }

    for (int slopeIndex = 0; slopeIndex < 5; ++slopeIndex)
    {
        const auto fullBand = renderNormalCrossoverMonitor (
            smart_ducking::MonitorMode::normal,
            static_cast<float> (slopeIndex));
        const auto lowBand = renderNormalCrossoverMonitor (
            smart_ducking::MonitorMode::crossoverLow,
            static_cast<float> (slopeIndex));
        const auto highBand = renderNormalCrossoverMonitor (
            smart_ducking::MonitorMode::crossoverHigh,
            static_cast<float> (slopeIndex));
        expect (fullBand.size() == lowBand.size() && lowBand.size() == highBand.size(),
                "NORMAL crossover monitor renders matching signal lengths");

        double referenceEnergy = 0.0;
        double errorEnergy = 0.0;
        float maximumError = 0.0f;

        for (size_t sample = 0; sample < fullBand.size(); ++sample)
        {
            const auto error = lowBand[sample] + highBand[sample] - fullBand[sample];
            referenceEnergy += static_cast<double> (fullBand[sample]) * fullBand[sample];
            errorEnergy += static_cast<double> (error) * error;
            maximumError = juce::jmax (maximumError, std::abs (error));
        }

        const auto relativeRmsError = std::sqrt (errorEnergy / referenceEnergy);
        expect (relativeRmsError < 1.0e-4 && maximumError < 1.0e-4f,
                "every NORMAL slope keeps FFT Low plus High exactly complementary");
    }

    for (const auto slopeDbPerOctave : { 24.0f, 36.0f, 48.0f, 60.0f, 72.0f })
    {
        constexpr auto cutoffHz = 92.0;
        const auto order = static_cast<int> (slopeDbPerOctave / 6.0f);
        const auto atCutoff = measureQualityCrossover (slopeDbPerOctave, cutoffHz);
        const auto oneOctaveAbove = measureQualityCrossover (
            slopeDbPerOctave, 2.0 * cutoffHz);
        const auto warpedRatio = std::tan (juce::MathConstants<double>::pi
                                           * 2.0 * cutoffHz / 48000.0)
                               / std::tan (juce::MathConstants<double>::pi
                                           * cutoffHz / 48000.0);
        const auto expectedLowAbove = 1.0 / (1.0 + std::pow (warpedRatio, order));

        expect (std::abs (atCutoff.lowRatio - 0.5) < 0.001
                    && std::abs (atCutoff.highRatio - 0.5) < 0.001,
                "every QUALITY slope crosses at complementary -6.02 dB branches");
        expect (std::abs (atCutoff.sumRatio - 1.0) < 0.001
                    && std::abs (oneOctaveAbove.sumRatio - 1.0) < 0.001,
                "every QUALITY Linkwitz-Riley slope reconstructs a flat all-pass sum");
        expect (std::abs (oneOctaveAbove.lowRatio - expectedLowAbove) < 0.001,
                "every QUALITY crossover option has its requested asymptotic slope");

        SmartDuckingAudioProcessor qualityProcessor;
        setParameter (qualityProcessor,
                      smart_ducking::parameter_ids::processingMode,
                      0.0f);
        setParameter (qualityProcessor,
                      smart_ducking::parameter_ids::crossoverSlope,
                      (slopeDbPerOctave - 24.0f) / 12.0f);
        setParameter (qualityProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (qualityProcessor,
                      smart_ducking::parameter_ids::spectralAmountDb,
                      0.0f);
        const auto qualityRoundTrip = processTone (
            qualityProcessor, cutoffHz, 0.0f, 0.0f);
        expect (std::abs (qualityRoundTrip.leftRatio - 1.0) < 0.004
                    && std::abs (qualityRoundTrip.rightRatio - 1.0) < 0.004,
                "every QUALITY slope remains flat through the complete processor path");

        constexpr auto normalCutoffHz = 93.75f;
        const auto normalLowAbove = processToneAfterMonitorSwitch (
            smart_ducking::MonitorMode::crossoverLow,
            2.0 * normalCutoffHz,
            (slopeDbPerOctave - 24.0f) / 12.0f,
            normalCutoffHz);
        const auto expectedNormalLowAbove = 1.0 / (1.0 + std::pow (2.0, order));
        expect (std::abs (normalLowAbove - expectedNormalLowAbove) < 0.004,
                "every NORMAL crossover option applies its selected cached FFT slope");
    }

    {
        for (const auto monitorMode : { smart_ducking::MonitorMode::crossoverLow,
                                        smart_ducking::MonitorMode::crossoverHigh })
        {
            SmartDuckingAudioProcessor crossoverMonitorSurround (
                smart_ducking::PluginVariant::surround);
            setParameter (crossoverMonitorSurround,
                          smart_ducking::parameter_ids::monitorMode,
                          static_cast<float> (monitorMode));
            setParameter (crossoverMonitorSurround,
                          smart_ducking::parameter_ids::duckDepthDb,
                          0.0f);
            setParameter (crossoverMonitorSurround,
                          smart_ducking::parameter_ids::spectralAmountDb,
                          0.0f);
            const auto lfeOnly = processSpatialTone (
                crossoverMonitorSurround,
                { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
                {},
                40.0);
            expect (*std::max_element (lfeOnly.outputRatios.begin(),
                                       lfeOnly.outputRatios.end()) < 0.001,
                    "Crossover Low and High exclude LFE from monitor output");
        }
    }
    {
        const auto zeroLookaheadImpulse = processImpulseWithLookahead (0.0f);
        const auto defaultLookaheadImpulse = processImpulseWithLookahead (10.0f);
        const auto maximumLookaheadImpulse = processImpulseWithLookahead (50.0f);
        const auto fortyFourOneImpulse = processImpulseWithLookahead (10.0f, 44100.0);
        const auto eightyEightTwoImpulse = processImpulseWithLookahead (10.0f, 88200.0);
        const auto ninetySixImpulse = processImpulseWithLookahead (10.0f, 96000.0);
        const auto bypassImpulse = processImpulseWithLookahead (10.0f, 48000.0, true);
        constexpr int expectedLatency = 2048 + 2400;

        expect (zeroLookaheadImpulse.latency == expectedLatency
                    && defaultLookaheadImpulse.latency == expectedLatency
                    && maximumLookaheadImpulse.latency == expectedLatency,
                "Lookahead uses a fixed 50 ms PDC reserve at 48 kHz");
        expect (bypassImpulse.latency == expectedLatency
                    && bypassImpulse.maximumBeforeLatency < 1.0e-6f
                    && approximatelyEqual (bypassImpulse.sampleAtLatency, 1.0f, 1.0e-6f),
                "global bypass preserves the exact reported-latency impulse path");
        expect (fortyFourOneImpulse.latency == 2048 + 2205
                    && eightyEightTwoImpulse.latency == 4096 + 4410
                    && ninetySixImpulse.latency == 4096 + 4800,
                "reported PDC follows STFT geometry plus 50 ms at 44.1, 88.2 and 96 kHz");
        expect (zeroLookaheadImpulse.maximumBeforeLatency < 1.0e-6f
                    && defaultLookaheadImpulse.maximumBeforeLatency < 1.0e-6f
                    && maximumLookaheadImpulse.maximumBeforeLatency < 1.0e-6f,
                "processor emits no Main signal before fixed reported latency");
        expect (approximatelyEqual (zeroLookaheadImpulse.sampleAtLatency, 1.0f, 0.002f)
                    && approximatelyEqual (defaultLookaheadImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f)
                    && approximatelyEqual (maximumLookaheadImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f),
                "fixed Lookahead reserve reconstructs an impulse at reported latency");
        expect (fortyFourOneImpulse.maximumBeforeLatency < 1.0e-6f
                    && eightyEightTwoImpulse.maximumBeforeLatency < 1.0e-6f
                    && ninetySixImpulse.maximumBeforeLatency < 1.0e-6f
                    && approximatelyEqual (fortyFourOneImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f)
                    && approximatelyEqual (eightyEightTwoImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f)
                    && approximatelyEqual (ninetySixImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f),
                "fixed Lookahead reserve reconstructs correctly at 44.1, 88.2 and 96 kHz");

        const auto zeroLookaheadOnset = findWidebandOnsetSample (0.0f);
        const auto maximumLookaheadOnset = findWidebandOnsetSample (50.0f);
        expect (zeroLookaheadOnset > 0 && maximumLookaheadOnset > 0,
                "wideband onset is detected at both Lookahead extremes");
        expect (std::abs ((zeroLookaheadOnset - maximumLookaheadOnset) - 2400) <= 512,
                "50 ms Lookahead advances the detector by approximately 2400 samples");
    }

    {
        SmartDuckingAudioProcessor configurationProcessor;
        TestPlayHead playHead;
        playHead.isPlaying = true;
        configurationProcessor.setPlayHead (&playHead);
        configurationProcessor.prepareToPlay (48000.0, 64);
        juce::AudioBuffer<float> buffer (8, 64);
        juce::MidiBuffer midi;
        buffer.clear();
        configurationProcessor.processBlock (buffer, midi);
        const auto initialLatency = configurationProcessor.getLatencySamples();

        setParameter (configurationProcessor, smart_ducking::parameter_ids::fftResolution, 0.0f);
        setParameter (configurationProcessor, smart_ducking::parameter_ids::processingMode, 0.0f);
        buffer.clear();
        configurationProcessor.processBlock (buffer, midi);
        expect (configurationProcessor.hasPendingDspConfiguration(),
                "FFT and Processing Mode changes remain pending while transport is playing");
        expect (configurationProcessor.getActiveFftResolutionIndex() == 1
                    && configurationProcessor.isActiveNormalMode()
                    && configurationProcessor.getLatencySamples() == initialLatency,
                "audio callback does not reset DSP geometry or PDC during playback");

        configurationProcessor.prepareToPlay (48000.0, 64);
        expect (! configurationProcessor.hasPendingDspConfiguration()
                    && configurationProcessor.getActiveFftResolutionIndex() == 0
                    && ! configurationProcessor.isActiveNormalMode(),
                "prepareToPlay safely applies a pending DSP configuration");
        expect (configurationProcessor.getLatencySamples() == 1024 + 2400,
                "safe x1 configuration reports STFT plus fixed Lookahead reserve");
        configurationProcessor.setPlayHead (nullptr);
    }

    for (int orderIndex = 0;
         orderIndex < smart_ducking::surroundChannelOrderCount;
         ++orderIndex)
    {
        const auto order = static_cast<smart_ducking::SurroundChannelOrder> (orderIndex);
        const auto& physicalToCanonical =
            smart_ducking::getPhysicalToCanonicalChannels (order);
        SmartDuckingAudioProcessor routingProcessor (smart_ducking::PluginVariant::surround);
        setParameter (routingProcessor,
                      smart_ducking::parameter_ids::mainChannelOrder,
                      static_cast<float> (orderIndex));
        setParameter (routingProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (routingProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (routingProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

        for (int input = 0; input < 6; ++input)
        {
            std::array<float, 6> main {};
            main[static_cast<size_t> (input)] = 1.0f;
            const auto routed = processSpatialTone (routingProcessor, main);

            for (int output = 0; output < 6; ++output)
                expect (approximatelyEqual (
                            static_cast<float> (
                                routed.outputRatios[static_cast<size_t> (output)]),
                            output == physicalToCanonical[static_cast<size_t> (input)]
                                ? 1.0f : 0.0f,
                            0.004f),
                        "all Main 5.1 channel orders map physical inputs to canonical processing roles");
        }

        SmartDuckingAudioProcessor outputOrderProcessor (
            smart_ducking::PluginVariant::surround);
        setParameter (outputOrderProcessor,
                      smart_ducking::parameter_ids::outputChannelOrder,
                      static_cast<float> (orderIndex));
        setParameter (outputOrderProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (outputOrderProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (outputOrderProcessor,
                      smart_ducking::parameter_ids::spectralAmountDb,
                      0.0f);

        for (int canonicalInput = 0; canonicalInput < 6; ++canonicalInput)
        {
            std::array<float, 6> main {};
            main[static_cast<size_t> (canonicalInput)] = 1.0f;
            const auto routed = processSpatialTone (outputOrderProcessor, main);

            for (int physicalOutput = 0; physicalOutput < 6; ++physicalOutput)
                expect (approximatelyEqual (
                            static_cast<float> (routed.outputRatios[
                                static_cast<size_t> (physicalOutput)]),
                            physicalToCanonical[static_cast<size_t> (physicalOutput)]
                                    == canonicalInput
                                ? 1.0f : 0.0f,
                            0.004f),
                        "all Output 5.1 channel orders map canonical roles to physical outputs");
        }

        SmartDuckingAudioProcessor voiceOrderProcessor (
            smart_ducking::PluginVariant::surround);
        setParameter (voiceOrderProcessor,
                      smart_ducking::parameter_ids::voiceFormat,
                      1.0f);
        setParameter (voiceOrderProcessor,
                      smart_ducking::parameter_ids::voiceChannelOrder,
                      static_cast<float> (orderIndex));
        voiceOrderProcessor.notifyEditorOpened();
        constexpr std::array<int, 6> canonicalToMeter { 0, 2, 1, -1, 3, 4 };

        for (int physicalVoice = 0; physicalVoice < 6; ++physicalVoice)
        {
            std::array<float, 6> voice {};
            voice[static_cast<size_t> (physicalVoice)] = 1.0f;
            static_cast<void> (processSpatialTone (
                voiceOrderProcessor, { 1, 0, 0, 0, 0, 0 }, voice));
            const std::array<float, 5> voicePeaks {
                voiceOrderProcessor.getSidechainPeakLeft(),
                voiceOrderProcessor.getSidechainPeakCentre(),
                voiceOrderProcessor.getSidechainPeakRight(),
                voiceOrderProcessor.getSidechainPeakLeftSurround(),
                voiceOrderProcessor.getSidechainPeakRightSurround()
            };
            const auto canonicalVoice =
                physicalToCanonical[static_cast<size_t> (physicalVoice)];
            const auto expectedMeter =
                canonicalToMeter[static_cast<size_t> (canonicalVoice)];

            for (int meter = 0; meter < 5; ++meter)
                expect (approximatelyEqual (voicePeaks[static_cast<size_t> (meter)],
                                            meter == expectedMeter ? 1.0f : 0.0f,
                                            0.001f),
                        "all Voice SC 5.1 channel orders map physical inputs to canonical detectors");
        }
    }

    {
        SmartDuckingAudioProcessor downmixProcessor (smart_ducking::PluginVariant::surround);
        setParameter (downmixProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (downmixProcessor, smart_ducking::parameter_ids::downmix, 1.0f);
        setParameter (downmixProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (downmixProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

        const auto centre = processSpatialTone (downmixProcessor, { 0, 0, 1, 0, 0, 0 });
        expect (approximatelyEqual (static_cast<float> (centre.outputRatios[0]), 0.70710678f, 0.004f)
                    && approximatelyEqual (static_cast<float> (centre.outputRatios[1]), 0.70710678f, 0.004f),
                "5.1 downmix sends centre to L/R at -3 dB");

        const auto leftSurround = processSpatialTone (downmixProcessor, { 0, 0, 0, 0, 1, 0 });
        expect (approximatelyEqual (static_cast<float> (leftSurround.outputRatios[0]), 0.70710678f, 0.004f)
                    && leftSurround.outputRatios[1] < 0.004,
                "5.1 downmix sends Ls only to L at -3 dB");

        const auto rightSurround = processSpatialTone (downmixProcessor, { 0, 0, 0, 0, 0, 1 });
        expect (rightSurround.outputRatios[0] < 0.004
                    && approximatelyEqual (static_cast<float> (rightSurround.outputRatios[1]),
                                            0.70710678f,
                                            0.004f),
                "5.1 downmix sends Rs only to R at -3 dB");

        const auto lfe = processSpatialTone (downmixProcessor, { 0, 0, 0, 1, 0, 0 });
        expect (approximatelyEqual (static_cast<float> (lfe.outputRatios[0]), 0.5f, 0.004f)
                    && approximatelyEqual (static_cast<float> (lfe.outputRatios[1]), 0.5f, 0.004f),
                "5.1 downmix includes LFE in L/R at -6 dB");
        expect (lfe.outputRatios[2] < 0.004 && lfe.outputRatios[3] < 0.004
                    && lfe.outputRatios[4] < 0.004 && lfe.outputRatios[5] < 0.004,
                "forced stereo downmix clears non-L/R output channels");

        setParameter (downmixProcessor, smart_ducking::parameter_ids::dropLfe, 1.0f);
        const auto droppedLfe = processSpatialTone (downmixProcessor, { 0, 0, 0, 1, 0, 0 });
        expect (droppedLfe.outputRatios[0] < 0.004 && droppedLfe.outputRatios[1] < 0.004,
                "Drop LFE removes LFE from forced stereo downmix");

        SmartDuckingAudioProcessor globallyBypassedRouting (
            smart_ducking::PluginVariant::surround);
        setParameter (globallyBypassedRouting,
                      smart_ducking::parameter_ids::mainChannelOrder,
                      1.0f);
        setParameter (globallyBypassedRouting, smart_ducking::parameter_ids::downmix, 1.0f);
        setParameter (globallyBypassedRouting, smart_ducking::parameter_ids::dropLfe, 1.0f);
        setParameter (globallyBypassedRouting,
                      smart_ducking::parameter_ids::globalBypass,
                      1.0f);
        const auto bypassedCentre = processSpatialTone (
            globallyBypassedRouting, { 0, 0, 0, 0, 1, 0 });
        expect (approximatelyEqual (
                    static_cast<float> (bypassedCentre.outputRatios[0]),
                    0.70710678f,
                    0.004f)
                    && approximatelyEqual (
                        static_cast<float> (bypassedCentre.outputRatios[1]),
                        0.70710678f,
                        0.004f),
                "global bypass preserves Main channel remapping and stereo Downmix");
        const auto bypassedLfe = processSpatialTone (
            globallyBypassedRouting, { 0, 0, 0, 0, 0, 1 });
        expect (*std::max_element (bypassedLfe.outputRatios.begin(),
                                   bypassedLfe.outputRatios.end()) < 0.004,
                "global bypass preserves Drop LFE");

        SmartDuckingAudioProcessor bypassedOutputOrder (
            smart_ducking::PluginVariant::surround);
        setParameter (bypassedOutputOrder,
                      smart_ducking::parameter_ids::outputChannelOrder,
                      2.0f);
        setParameter (bypassedOutputOrder,
                      smart_ducking::parameter_ids::globalBypass,
                      1.0f);
        const auto bypassedFilmCentre = processSpatialTone (
            bypassedOutputOrder, { 0, 0, 1, 0, 0, 0 });
        expect (approximatelyEqual (
                    static_cast<float> (bypassedFilmCentre.outputRatios[1]),
                    1.0f,
                    0.004f),
                "global bypass preserves the configured surround Output channel order");
    }

    {
        constexpr double sampleRate = 48000.0;
        constexpr int blockSize = 64;
        constexpr int warmupBlocks = 160;
        constexpr int settleBlocks = 32;
        juce::MidiBuffer midi;

        const auto processConstant = [&] (SmartDuckingAudioProcessor& routingProcessor,
                                          int inputChannel,
                                          int numBlocks,
                                          auto&& inspectFirstBlock)
        {
            juce::AudioBuffer<float> buffer (12, blockSize);

            for (int block = 0; block < numBlocks; ++block)
            {
                buffer.clear();
                for (int sample = 0; sample < blockSize; ++sample)
                    buffer.setSample (inputChannel, sample, 1.0f);

                routingProcessor.processBlock (buffer, midi);
                if (block == 0)
                    inspectFirstBlock (buffer);
            }

            return buffer;
        };

        SmartDuckingAudioProcessor smoothDownmix (smart_ducking::PluginVariant::surround);
        setParameter (smoothDownmix, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (smoothDownmix, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        smoothDownmix.prepareToPlay (sampleRate, blockSize);
        static_cast<void> (processConstant (smoothDownmix, 2, warmupBlocks,
                                            [] (const auto&) {}));
        setParameter (smoothDownmix, smart_ducking::parameter_ids::downmix, 1.0f);
        const auto downmixSettled = processConstant (
            smoothDownmix, 2, settleBlocks,
            [&] (const auto& firstBlock)
            {
                expect (firstBlock.getSample (0, 0) > 0.0f
                            && firstBlock.getSample (0, 0) < 0.02f
                            && firstBlock.getSample (2, 0) > 0.98f,
                        "Downmix transition starts from the current surround route");
            });
        expect (downmixSettled.getSample (0, blockSize - 1) > 0.70f
                    && std::abs (downmixSettled.getSample (2, blockSize - 1)) < 0.01f,
                "Downmix transition reaches the stereo route without a one-sample switch");

        SmartDuckingAudioProcessor smoothDropLfe (smart_ducking::PluginVariant::surround);
        setParameter (smoothDropLfe, smart_ducking::parameter_ids::downmix, 1.0f);
        setParameter (smoothDropLfe, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (smoothDropLfe, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        smoothDropLfe.prepareToPlay (sampleRate, blockSize);
        static_cast<void> (processConstant (smoothDropLfe, 3, warmupBlocks,
                                            [] (const auto&) {}));
        setParameter (smoothDropLfe, smart_ducking::parameter_ids::dropLfe, 1.0f);
        const auto lfeSettled = processConstant (
            smoothDropLfe, 3, settleBlocks,
            [&] (const auto& firstBlock)
            {
                expect (firstBlock.getSample (0, 0) > 0.48f,
                        "Drop LFE transition starts from the current LFE contribution");
            });
        expect (std::abs (lfeSettled.getSample (0, blockSize - 1)) < 0.01f,
                "Drop LFE transition reaches silence without a one-sample switch");

        SmartDuckingAudioProcessor smoothMainOrder (smart_ducking::PluginVariant::surround);
        setParameter (smoothMainOrder, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (smoothMainOrder, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        smoothMainOrder.prepareToPlay (sampleRate, blockSize);
        static_cast<void> (processConstant (smoothMainOrder, 2, warmupBlocks,
                                            [] (const auto&) {}));
        setParameter (smoothMainOrder,
                      smart_ducking::parameter_ids::mainChannelOrder,
                      1.0f);
        const auto reaperOrderSettled = processConstant (
            smoothMainOrder, 2, settleBlocks + 64,
            [&] (const auto& firstBlock)
            {
                expect (firstBlock.getSample (2, 0) > 0.98f
                            && std::abs (firstBlock.getSample (4, 0)) < 0.01f,
                        "channel-order transition preserves the latency-aligned current mapping initially");
            });
        expect (std::abs (reaperOrderSettled.getSample (2, blockSize - 1)) < 0.01f
                    && reaperOrderSettled.getSample (4, blockSize - 1) > 0.99f,
                "channel-order transition reaches the remapped channel without a one-sample switch");
    }

    {
        SmartDuckingAudioProcessor lfeProcessor (smart_ducking::PluginVariant::surround);
        setParameter (lfeProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (lfeProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (lfeProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
        setParameter (lfeProcessor, smart_ducking::parameter_ids::spectralAmountDb, 10.0f);

        const auto voiceLfeOnly = processSpatialTone (lfeProcessor,
                                                      { 1, 0, 0, 0, 0, 0 },
                                                      { 0, 0, 0, 1, 0, 0 },
                                                      3000.0);
        expect (approximatelyEqual (static_cast<float> (voiceLfeOnly.outputRatios[0]), 1.0f, 0.004f),
                "Voice LFE is excluded from wideband and spectral detectors");

        const auto mainLfeOnly = processSpatialTone (lfeProcessor,
                                                     { 0, 0, 0, 1, 0, 0 },
                                                     { 1, 0, 0, 0, 0, 0 },
                                                     3000.0);
        expect (approximatelyEqual (static_cast<float> (mainLfeOnly.outputRatios[3]), 1.0f, 0.004f),
                "Main LFE is latency-aligned but never processed");
    }

    {
        SmartDuckingAudioProcessor matrixProcessor (smart_ducking::PluginVariant::surround);
        setParameter (matrixProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (matrixProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (matrixProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
        setParameter (matrixProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
        setParameter (matrixProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

        const auto directLs = processSpatialTone (matrixProcessor,
                                                  { 0, 0, 0, 0, 1, 0 },
                                                  { 0, 0, 0, 0, 1, 0 });
        expect (approximatelyEqual (static_cast<float> (directLs.outputRatios[4]),
                                    juce::Decibels::decibelsToGain (-10.0f),
                                    0.025f),
                "Voice Ls applies full wideband ducking to Main Ls");

        const auto neighbourLs = processSpatialTone (matrixProcessor,
                                                     { 0, 0, 0, 0, 1, 0 },
                                                     { 1, 0, 0, 0, 0, 0 });
        expect (approximatelyEqual (static_cast<float> (neighbourLs.outputRatios[4]),
                                    juce::Decibels::decibelsToGain (-3.5f),
                                    0.025f),
                "Voice L applies the 35 percent neighbour coefficient to Main Ls");
    }

    {
        SmartDuckingAudioProcessor stereoVoiceProcessor (smart_ducking::PluginVariant::surround);
        setParameter (stereoVoiceProcessor, smart_ducking::parameter_ids::voiceFormat, 0.0f);
        setParameter (stereoVoiceProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (stereoVoiceProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
        setParameter (stereoVoiceProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
        setParameter (stereoVoiceProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto stereoVoiceToSurroundMain = processSpatialTone (stereoVoiceProcessor,
                                                                  { 0, 0, 1, 0, 0, 0 },
                                                                  { 1, 1, 0, 0, 0, 0 });
        expect (approximatelyEqual (
                    static_cast<float> (stereoVoiceToSurroundMain.outputRatios[2]),
                    juce::Decibels::decibelsToGain (-10.0f),
                    0.025f),
                "Stereo Voice PCE centre ducks a 5.1 Main centre");

        SmartDuckingAudioProcessor surroundVoiceProcessor (smart_ducking::PluginVariant::stereo);
        setParameter (surroundVoiceProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (surroundVoiceProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (surroundVoiceProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
        setParameter (surroundVoiceProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
        setParameter (surroundVoiceProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto surroundVoiceToStereoMain = processSpatialTone (surroundVoiceProcessor,
                                                                  { 1, 0, 0, 0, 0, 0 },
                                                                  { 0, 0, 0, 0, 1, 0 });
        expect (approximatelyEqual (
                    static_cast<float> (surroundVoiceToStereoMain.outputRatios[0]),
                    juce::Decibels::decibelsToGain (-3.5f),
                    0.025f),
                "5.1 Voice Ls matrix detector ducks a stereo Main hard-left component");
    }

    {
        SmartDuckingAudioProcessor spectralSurroundProcessor (smart_ducking::PluginVariant::surround);
        setParameter (spectralSurroundProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (spectralSurroundProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (spectralSurroundProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
        setParameter (spectralSurroundProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (spectralSurroundProcessor, smart_ducking::parameter_ids::spectralAmountDb, 10.0f);
        const auto matchingLs = processSpatialTone (spectralSurroundProcessor,
                                                    { 0, 0, 0, 0, 1, 0 },
                                                    { 0, 0, 0, 0, 1, 0 },
                                                    3000.0);
        expect (matchingLs.outputRatios[4] < 0.75,
                "spectral ducking directly suppresses the matching 5.1 Ls component");

        const auto nonMatchingL = processSpatialTone (spectralSurroundProcessor,
                                                      { 0, 0, 0, 0, 1, 0 },
                                                      { 1, 0, 0, 0, 0, 0 },
                                                      3000.0);
        expect (approximatelyEqual (static_cast<float> (nonMatchingL.outputRatios[4]), 1.0f, 0.004f),
                "spectral ducking does not apply neighbour matrix routing in 5.1");
    }

    smart_ducking::LcrDuckMatrix matrix;
    const auto combinedMatrixControls = matrix.apply ({ 0.8f, 0.7f, 0.6f });
    expect (approximatelyEqual (combinedMatrixControls.left, 0.8f),
            "matrix uses maximum detector contribution for original L");
    expect (approximatelyEqual (combinedMatrixControls.centre, 0.7f),
            "matrix uses maximum detector contribution for original C");
    expect (approximatelyEqual (combinedMatrixControls.right, 0.6f),
            "matrix uses maximum detector contribution for original R");

    for (const auto& [rows, columns] : std::array<std::pair<int, int>, 4> {
             std::pair { 3, 3 }, std::pair { 3, 5 },
             std::pair { 5, 3 }, std::pair { 5, 5 } })
    {
        auto mappingsAreValid = true;

        for (int voice = 0; voice < rows; ++voice)
            for (int main = 0; main < columns; ++main)
            {
                const auto mirrored = smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (
                    voice, main);
                const auto mirroredVoice = static_cast<int> (mirrored / 5);
                const auto mirroredMain = static_cast<int> (mirrored % 5);
                const auto roundTrip = smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (
                    mirroredVoice, mirroredMain);
                mappingsAreValid = mappingsAreValid
                    && mirroredVoice < rows
                    && mirroredMain < columns
                    && roundTrip == static_cast<size_t> (voice * 5 + main);
            }

        expect (mappingsAreValid,
                "L/R symmetry mapping stays within every active matrix shape and is reversible");
    }

    expect (smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (1, 2) == 5
                && smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (0, 1) == 11
                && smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (0, 3) == 14
                && smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (3, 2) == 20,
            "L/R symmetry follows spatial reflection rather than matrix diagonal reflection");

    {
        smart_ducking::StereoSpectralLcrProcessor smoothedMatrixProcessor;
        smoothedMatrixProcessor.prepare (48000.0);
        smoothedMatrixProcessor.setEconomyMode (true);
        auto matrixParameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
        matrixParameters.amountDb = 0.0f;
        matrixParameters.duckDepthDb = 10.0f;
        matrixParameters.attackMs = 0.0f;
        matrixParameters.holdMs = 0.0f;
        matrixParameters.duckMatrix.coefficients.fill (1.0f);
        smoothedMatrixProcessor.setParameters (matrixParameters);

        for (int sample = 0; sample < 12288; ++sample)
            static_cast<void> (smoothedMatrixProcessor.processSample (0.25f,
                                                                      0.25f,
                                                                      1.0f,
                                                                      1.0f));

        const auto beforeAutomation = smoothedMatrixProcessor.getLastWidebandReductionDb (0);
        matrixParameters.duckMatrix.coefficients.fill (0.0f);
        smoothedMatrixProcessor.setParameters (matrixParameters);

        for (int sample = 0; sample < 1024; ++sample)
            static_cast<void> (smoothedMatrixProcessor.processSample (0.25f,
                                                                      0.25f,
                                                                      1.0f,
                                                                      1.0f));

        const auto duringAutomation = smoothedMatrixProcessor.getLastWidebandReductionDb (0);

        for (int sample = 0; sample < 16384; ++sample)
            static_cast<void> (smoothedMatrixProcessor.processSample (0.25f,
                                                                      0.25f,
                                                                      1.0f,
                                                                      1.0f));

        expect (beforeAutomation > 8.0f
                    && duringAutomation > 0.1f
                    && duringAutomation < beforeAutomation
                    && smoothedMatrixProcessor.getLastWidebandReductionDb (0) < 0.1f,
                "matrix automation smooths final maximum-aggregated controls at frame rate");
    }

    {
        smart_ducking::LinearFrameSmoother linearSmoother;
        linearSmoother.reset (0.0f);
        const auto first = linearSmoother.process (1.0f, 0.01f, 0.03f);
        const auto second = linearSmoother.process (1.0f, 0.01f, 0.03f);
        const auto third = linearSmoother.process (1.0f, 0.01f, 0.03f);
        expect (approximatelyEqual (first, 1.0f / 3.0f)
                    && approximatelyEqual (second, 2.0f / 3.0f)
                    && approximatelyEqual (third, 1.0f),
                "extracted frame smoother completes a bounded linear transition");

        smart_ducking::SpatialControlSmoother spatialSmoother;
        smart_ducking::LcrDuckMatrix::Controls target;
        target.left = 1.0f;
        target.rightSurround = 0.5f;
        const auto spatialFirst = spatialSmoother.process (target, 0.01f, 0.035f);
        const auto spatialSecond = spatialSmoother.process (target, 0.01f, 0.035f);
        expect (spatialFirst.left > 0.0f && spatialFirst.left < spatialSecond.left
                    && spatialSecond.left < 1.0f
                    && approximatelyEqual (spatialFirst.rightSurround,
                                           0.5f * spatialFirst.left),
                "extracted spatial smoother advances all five controls coherently");

        smart_ducking::PceHistory history;
        history.resize (8);
        history.crossReal[3] = 1.0f;
        history.crossImaginary[4] = -1.0f;
        history.powerLeft[5] = 2.0f;
        history.powerRight[6] = 3.0f;
        history.clear();
        expect (history.crossReal.size() == 8
                    && std::all_of (history.crossReal.begin(), history.crossReal.end(),
                                    [] (float value) { return std::abs (value) < 1.0e-12f; })
                    && std::all_of (history.crossImaginary.begin(), history.crossImaginary.end(),
                                    [] (float value) { return std::abs (value) < 1.0e-12f; })
                    && std::all_of (history.powerLeft.begin(), history.powerLeft.end(),
                                    [] (float value) { return std::abs (value) < 1.0e-12f; })
                    && std::all_of (history.powerRight.begin(), history.powerRight.end(),
                                    [] (float value) { return std::abs (value) < 1.0e-12f; }),
                "extracted PCE history resizes and clears its persistent state");
    }

    smart_ducking::SidechainEnvelope holdEnvelope;
    holdEnvelope.prepare (1000.0);
    holdEnvelope.setTimes (0.0f, 3.0f, 100.0f);
    expect (approximatelyEqual (holdEnvelope.processSample (1.0f), 1.0f), "zero attack is immediate");
    expect (approximatelyEqual (holdEnvelope.processSample (0.0f), 1.0f), "hold sample one");
    expect (approximatelyEqual (holdEnvelope.processSample (0.0f), 1.0f), "hold sample two");
    expect (approximatelyEqual (holdEnvelope.processSample (0.0f), 1.0f), "hold sample three");
    expect (holdEnvelope.processSample (0.0f) < 1.0f, "release starts after hold");

    expect (approximatelyEqual (smart_ducking::SidechainLevelMapper::mapLevelDb (-61.0f, -60.0f, -30.0f),
                                0.0f),
            "voice below the lower boundary does not trigger ducking");
    expect (approximatelyEqual (smart_ducking::SidechainLevelMapper::mapLevelDb (-45.0f, -60.0f, -30.0f),
                                0.5f),
            "voice between boundaries proportionally controls ducking");
    expect (approximatelyEqual (smart_ducking::SidechainLevelMapper::mapLevelDb (-29.0f, -60.0f, -30.0f),
                                 1.0f),
            "voice above the upper boundary gives full ducking");

    using SidechainMode = smart_ducking::SidechainClassifier::Mode;
    std::array<float, 64> classifierLeft {};
    std::array<float, 64> classifierRight {};
    smart_ducking::SidechainClassifier classifier;
    classifier.prepare (100.0);

    auto classification = classifier.process (classifierLeft.data(),
                                               classifierRight.data(),
                                               static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::silence, "classifier recognises silence");

    classifierLeft.fill (0.5f);
    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::leftOnly,
            "classifier immediately routes a left-only signal after silence");

    classifier.reset();
    classifierLeft.fill (0.0f);
    classifierRight.fill (0.5f);
    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::rightOnly,
            "classifier immediately routes a right-only signal after silence");

    classifier.reset();
    classifierLeft.fill (0.5f);
    classifierRight.fill (0.5f);
    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::dualMono,
            "classifier immediately routes exact dual mono to centre after silence");

    classifier.reset();
    classifierRight.fill (0.25f);
    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::stereo,
            "perfect correlation with unequal channel levels is not treated as dual mono");

    classifier.reset();
    for (size_t sample = 0; sample < classifierLeft.size(); ++sample)
    {
        classifierLeft[sample] = (sample % 2 == 0) ? 0.5f : -0.5f;
        classifierRight[sample] = (sample % 3 == 0) ? 0.5f : -0.5f;
    }

    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::stereo,
            "classifier keeps decorrelated two-channel voice in stereo PCE mode");

    classifierRight = classifierLeft;
    for (int frame = 0; frame < 9; ++frame)
    {
        classification = classifier.process (classifierLeft.data(),
                                              classifierRight.data(),
                                              static_cast<int> (classifierLeft.size()));
        expect (classification.mode == SidechainMode::stereo,
                "fast route requires its stability hold");
    }

    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::dualMono,
            "stable dual mono enters the fast route after 100 ms");

    for (size_t sample = 0; sample < classifierRight.size(); ++sample)
        classifierRight[sample] = (sample % 3 == 0) ? 0.5f : -0.5f;

    classification = classifier.process (classifierLeft.data(),
                                          classifierRight.data(),
                                          static_cast<int> (classifierLeft.size()));
    expect (classification.mode == SidechainMode::stereo,
            "decorrelation exits a fast route immediately");

    // Meter and spectrum capture are intentionally disabled when no editor is
    // open. Enable the same visualisation path that the editor activates so
    // the following meter assertions test current runtime behaviour.
    processor.notifyEditorOpened();
    setParameter (processor, smart_ducking::parameter_ids::attackMs, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::holdMs, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
    setParameter (processor, smart_ducking::parameter_ids::crossoverHz, 92.0f);

    const auto tenDecibelGain = juce::Decibels::decibelsToGain (-10.0f);
    const auto twoPointFiveDecibelGain = juce::Decibels::decibelsToGain (-2.5f);

    const auto dryHigh = processTone (processor, 1000.0, 0.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (dryHigh.leftRatio), 1.0f, 0.002f),
            "default QUALITY crossover reconstruction is flat at one kilohertz left");
    expect (approximatelyEqual (static_cast<float> (dryHigh.rightRatio), 1.0f, 0.002f),
            "default QUALITY crossover reconstruction is flat at one kilohertz right");
    expect (approximatelyEqual (processor.getSidechainPeakLeft(), 0.0f), "silent left sidechain meter");
    expect (approximatelyEqual (processor.getSidechainPeakRight(), 0.0f), "silent right sidechain meter");

    const auto dryAtCrossover = processTone (processor, 92.0, 0.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (dryAtCrossover.leftRatio), 1.0f, 0.003f),
            "default QUALITY crossover reconstruction is flat at crossover");

    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

    const auto duckedAtCrossover = processTone (processor, 92.0, 1.0f, 0.0f);
    const auto expectedCrossoverDuck = 0.5f * (1.0f + twoPointFiveDecibelGain);
    expect (approximatelyEqual (static_cast<float> (duckedAtCrossover.leftRatio),
                                expectedCrossoverDuck,
                                0.005f),
            "left voice applies its 0.25 matrix weight to the centre above crossover");

    const auto duckedHigh = processTone (processor, 1000.0, 1.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (duckedHigh.leftRatio), twoPointFiveDecibelGain, 0.003f),
            "left voice ducks a phantom-centre main by its 0.25 centre weight");
    expect (approximatelyEqual (static_cast<float> (duckedHigh.rightRatio), twoPointFiveDecibelGain, 0.003f),
            "centre-component ducking is symmetrically downmixed");

    const auto centreVoiceOnCentreMain = processTone (processor, 1000.0, 1.0f, 1.0f);
    expect (approximatelyEqual (static_cast<float> (centreVoiceOnCentreMain.leftRatio), tenDecibelGain, 0.003f)
                && approximatelyEqual (static_cast<float> (centreVoiceOnCentreMain.rightRatio),
                                       tenDecibelGain,
                                       0.003f),
            "centre voice ducks the extracted main centre by the full depth");

    const auto oneDecibelGain = juce::Decibels::decibelsToGain (-1.0f);
    const auto centreVoiceOnHardLeftMain = processTone (processor, 1000.0, 1.0f, 1.0f, 1.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (centreVoiceOnHardLeftMain.leftRatio),
                                oneDecibelGain,
                                0.003f),
            "centre voice applies its 0.10 neighbour weight to a hard-left main");
    expect (centreVoiceOnHardLeftMain.rightRatio < 0.003,
            "LCR ducking does not create right-channel spill from a hard-left main");

    const auto leftVoiceOnHardLeftMain = processTone (processor, 1000.0, 1.0f, 0.0f, 1.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (leftVoiceOnHardLeftMain.leftRatio),
                                tenDecibelGain,
                                0.003f),
            "left voice applies full ducking to the extracted main left");

    const auto leftVoiceOnHardRightMain = processTone (processor, 1000.0, 1.0f, 0.0f, 0.0f, 1.0f);
    expect (approximatelyEqual (static_cast<float> (leftVoiceOnHardRightMain.rightRatio), 1.0f, 0.003f),
            "left voice leaves the extracted main right unchanged at zero matrix weight");

    const auto dryProtectedLow = processTone (processor, 20.0, 0.0f, 0.0f);
    const auto protectedLow = processTone (processor, 20.0, 1.0f, 1.0f);
    expect (approximatelyEqual (static_cast<float> (protectedLow.leftRatio),
                                static_cast<float> (dryProtectedLow.leftRatio),
                                0.005f),
            "default NORMAL mode protects the twenty hertz low band on left");
    expect (approximatelyEqual (static_cast<float> (protectedLow.rightRatio),
                                static_cast<float> (dryProtectedLow.rightRatio),
                                0.005f),
            "default NORMAL mode protects the twenty hertz low band on right");

    {
        SmartDuckingAudioProcessor normalProcessor;
        setParameter (normalProcessor, smart_ducking::parameter_ids::processingMode, 1.0f);
        setParameter (normalProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
        setParameter (normalProcessor, smart_ducking::parameter_ids::holdMs, 0.0f);
        setParameter (normalProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
        setParameter (normalProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

        const auto normalDryHigh = processTone (normalProcessor, 1000.0, 0.0f, 0.0f);
        expect (approximatelyEqual (static_cast<float> (normalDryHigh.leftRatio), 1.0f, 0.003f)
                    && approximatelyEqual (static_cast<float> (normalDryHigh.rightRatio),
                                           1.0f,
                                           0.003f),
                "NORMAL reconstructs dry high-band stereo without level change");

        const auto normalDuckedHigh = processTone (normalProcessor, 1000.0, 1.0f, 1.0f);
        expect (approximatelyEqual (static_cast<float> (normalDuckedHigh.leftRatio),
                                    tenDecibelGain,
                                    0.006f)
                    && approximatelyEqual (static_cast<float> (normalDuckedHigh.rightRatio),
                                           tenDecibelGain,
                                           0.006f),
                "NORMAL applies the full centre wideband depth above crossover");

        const auto normalDryLow = processTone (normalProcessor, 20.0, 0.0f, 0.0f);
        const auto normalProtectedLow = processTone (normalProcessor, 20.0, 1.0f, 1.0f);
        const auto normalLowIsProtected = approximatelyEqual (
                                              static_cast<float> (normalProtectedLow.leftRatio),
                                              static_cast<float> (normalDryLow.leftRatio),
                                              0.003f)
                                       && approximatelyEqual (
                                              static_cast<float> (normalProtectedLow.rightRatio),
                                              static_cast<float> (normalDryLow.rightRatio),
                                              0.003f);
        if (! normalLowIsProtected)
            std::cerr << "NORMAL dry/protected-low ratios: " << normalDryLow.leftRatio
                      << "/" << normalProtectedLow.leftRatio << ", "
                      << normalDryLow.rightRatio << "/" << normalProtectedLow.rightRatio << '\n';
        expect (normalLowIsProtected,
                "NORMAL protects the low band from wideband ducking");

        setParameter (normalProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (normalProcessor, smart_ducking::parameter_ids::spectralAmountDb, 10.0f);
        const auto normalSpectral = processTone (normalProcessor, 3000.0, 1.0f, 1.0f);
        expect (normalSpectral.leftRatio < 0.75 && normalSpectral.rightRatio < 0.75,
                "NORMAL applies direct spectral suppression above crossover");
    }

    expect (processor.getMainInputPeakLeft() > 0.49f, "left main meter");
    expect (processor.getMainInputPeakRight() > 0.49f, "right main meter");
    expect (approximatelyEqual (processor.getSidechainPeakLeft(), 1.0f), "left sidechain meter");
    expect (approximatelyEqual (processor.getSidechainPeakRight(), 1.0f), "right sidechain meter");
    expect (approximatelyEqual (processor.getGainReductionLeftDb(), 1.0f, 1.0e-4f), "left GR meter");
    expect (approximatelyEqual (processor.getGainReductionCentreDb(), 10.0f, 1.0e-4f), "centre GR meter");
    expect (approximatelyEqual (processor.getGainReductionRightDb(), 1.0f, 1.0e-4f), "right GR meter");
    expect (processor.getLatencySamples() == 2048 + 2400,
            "x2 processor reports STFT plus fixed 50 ms Lookahead reserve at 48 kHz");

    for (int economyMode = 0; economyMode < 2; ++economyMode)
    {
        for (int resolution = 0; resolution < 3; ++resolution)
        {
            smart_ducking::StereoSpectralLcrProcessor suppressor;
            suppressor.prepare (48000.0);
            suppressor.setEconomyMode (economyMode != 0);
            suppressor.setResolutionIndex (resolution);
            auto spectralParameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
            spectralParameters.amountDb = 0.0f;
            suppressor.setParameters (spectralParameters);

            const auto latency = 1024 << resolution;
            float maximumBeforeLatency = 0.0f;
            float reconstructedImpulse = 0.0f;

            for (int sample = 0; sample <= latency; ++sample)
            {
                const auto output = suppressor.processSample (sample == 0 ? 1.0f : 0.0f,
                                                              0.0f,
                                                              0.0f,
                                                              0.0f);
                const auto outputSample = output.left
                                        + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                              * output.centre;

                if (sample < latency)
                    maximumBeforeLatency = juce::jmax (maximumBeforeLatency,
                                                       std::abs (outputSample));
                else
                    reconstructedImpulse = outputSample;
            }

            expect (maximumBeforeLatency < 1.0e-6f,
                    "both processing modes emit no signal before reported latency");
            expect (approximatelyEqual (reconstructedImpulse, 1.0f, 0.002f),
                    "both processing modes reconstruct an impulse at reported latency");
            expect (suppressor.getProcessedFftFrameCount() == 0,
                    "Spectral Amount zero bypasses all FFT work in both processing modes");
        }
    }

    smart_ducking::StereoSpectralLcrProcessor sleepingSuppressor;
    sleepingSuppressor.prepare (48000.0);
    const auto sleepingLatency = sleepingSuppressor.getLatencySamples();

    for (int sample = 0; sample < sleepingLatency * 2; ++sample)
        static_cast<void> (sleepingSuppressor.processSample (0.25f, 0.25f, 0.0f, 0.0f));

    expect (sleepingSuppressor.getProcessedFftFrameCount() == 0,
            "silent sidechain keeps the heavy spectral path asleep");

    for (int sample = 0; sample < sleepingLatency / 2; ++sample)
        static_cast<void> (sleepingSuppressor.processSample (0.25f, 0.25f, 1.0f, 1.0f));

    expect (sleepingSuppressor.getProcessedFftFrameCount() > 0,
            "voice activity wakes the heavy spectral path");

    {
        smart_ducking::StereoSpectralLcrProcessor sampleSuppressor;
        smart_ducking::StereoSpectralLcrProcessor blockSuppressor;
        sampleSuppressor.prepare (48000.0);
        blockSuppressor.prepare (48000.0);
        constexpr std::array<int, 8> blockSizes { 1, 7, 63, 128, 511, 17, 1024, 257 };
        constexpr auto maximumBlockSize = 1024;
        std::array<std::array<float, maximumBlockSize>, 5> input {};
        std::array<std::array<float, maximumBlockSize>, 5> blockOutput {};
        std::array<const float*, 5> inputPointers {};
        std::array<float*, 5> outputPointers {};
        auto phase = 0.0;
        auto blockPathMatches = true;

        for (const auto blockSamples : blockSizes)
        {
            for (size_t channel = 0; channel < input.size(); ++channel)
            {
                inputPointers[channel] = input[channel].data();
                outputPointers[channel] = blockOutput[channel].data();

                for (int sample = 0; sample < blockSamples; ++sample)
                {
                    input[channel][static_cast<size_t> (sample)] = 0.25f
                        * static_cast<float> (std::sin (
                            phase + 0.17 * static_cast<double> (channel)));
                    phase += 0.013;
                }
            }

            blockSuppressor.processSilentSidechainBlock (inputPointers,
                                                          outputPointers,
                                                          blockSamples,
                                                          false);

            for (int sample = 0; sample < blockSamples; ++sample)
            {
                const smart_ducking::StereoSpectralLcrProcessor::LcrSample main {
                    input[0][static_cast<size_t> (sample)],
                    input[1][static_cast<size_t> (sample)],
                    input[2][static_cast<size_t> (sample)],
                    input[3][static_cast<size_t> (sample)],
                    input[4][static_cast<size_t> (sample)]
                };
                const auto reference = sampleSuppressor.processSample (
                    main, {}, false, false, {});

                for (int channel = 0; channel < 5; ++channel)
                    blockPathMatches = blockPathMatches
                        && approximatelyEqual (
                            blockOutput[static_cast<size_t> (channel)]
                                       [static_cast<size_t> (sample)],
                            reference.get (channel));
            }
        }

        expect (blockPathMatches,
                "silent-sidechain block path exactly matches sample-path latency and routing");

        auto wakeMatches = true;
        const auto wakeSamples = sampleSuppressor.getLatencySamples() * 2;
        for (int sample = 0; sample < wakeSamples; ++sample)
        {
            const auto mainValue = 0.2f * static_cast<float> (std::sin (0.019 * sample));
            const auto voiceValue = sample < wakeSamples / 2 ? 0.4f : 0.0f;
            const smart_ducking::StereoSpectralLcrProcessor::LcrSample main {
                mainValue, mainValue * 0.9f, mainValue * 0.8f,
                mainValue * 0.7f, mainValue * 0.6f
            };
            const smart_ducking::StereoSpectralLcrProcessor::LcrSample voice {
                voiceValue, voiceValue, voiceValue, voiceValue, voiceValue
            };
            const auto reference = sampleSuppressor.processSample (
                main, voice, false, false, {});
            const auto afterBlockSleep = blockSuppressor.processSample (
                main, voice, false, false, {});

            for (int channel = 0; channel < 5; ++channel)
                wakeMatches = wakeMatches
                    && approximatelyEqual (reference.get (channel),
                                           afterBlockSleep.get (channel),
                                           2.0e-5f);
        }

        expect (wakeMatches
                    && blockSuppressor.getProcessedFftFrameCount()
                         == sampleSuppressor.getProcessedFftFrameCount(),
                "silent-sidechain block path preserves sample-exact wake behaviour");
    }

    {
        constexpr auto blockSize = 127;
        constexpr auto silentBlocks = 80;
        constexpr auto voiceBlocks = 96;
        constexpr auto trailingBlocks = 112;
        SmartDuckingAudioProcessor blockPathProcessor (
            smart_ducking::PluginVariant::surround);
        SmartDuckingAudioProcessor samplePathProcessor (
            smart_ducking::PluginVariant::surround);
        setParameter (blockPathProcessor,
                      smart_ducking::parameter_ids::voiceFormat,
                      1.0f);
        setParameter (samplePathProcessor,
                      smart_ducking::parameter_ids::voiceFormat,
                      1.0f);
        blockPathProcessor.prepareToPlay (48000.0, blockSize);
        samplePathProcessor.prepareToPlay (48000.0, blockSize);
        // Keeping realtime visualisation enabled forces the established sample
        // path without changing its audio result.
        samplePathProcessor.notifyEditorOpened();
        juce::AudioBuffer<float> blockBuffer (12, blockSize);
        juce::AudioBuffer<float> sampleBuffer (12, blockSize);
        juce::MidiBuffer blockMidi;
        juce::MidiBuffer sampleMidi;
        auto processorPathsMatch = true;
        auto maximumProcessorPathError = 0.0f;
        auto maximumErrorBlock = -1;
        auto maximumErrorChannel = -1;
        auto maximumErrorSample = -1;
        auto phase = 0.0;

        for (int block = 0; block < silentBlocks + voiceBlocks + trailingBlocks; ++block)
        {
            blockBuffer.clear();
            const auto voiceActive = block >= silentBlocks
                                  && block < silentBlocks + voiceBlocks;

            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto signal = 0.2f * static_cast<float> (std::sin (phase));
                phase += 0.021;

                for (int channel = 0; channel < 6; ++channel)
                    blockBuffer.setSample (
                        channel,
                        sample,
                        signal * (1.0f - 0.07f * static_cast<float> (channel)));

                if (voiceActive)
                    for (int channel = 0; channel < 6; ++channel)
                        blockBuffer.setSample (6 + channel,
                                               sample,
                                               0.35f - 0.02f * channel);
            }

            sampleBuffer.makeCopyOf (blockBuffer, true);
            blockPathProcessor.processBlock (blockBuffer, blockMidi);
            samplePathProcessor.processBlock (sampleBuffer, sampleMidi);

            for (int channel = 0; channel < 6; ++channel)
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto error = std::abs (
                        blockBuffer.getSample (channel, sample)
                        - sampleBuffer.getSample (channel, sample));
                    processorPathsMatch = processorPathsMatch && error <= 3.0e-5f;
                    if (error > maximumProcessorPathError)
                    {
                        maximumProcessorPathError = error;
                        maximumErrorBlock = block;
                        maximumErrorChannel = channel;
                        maximumErrorSample = sample;
                    }
                }
        }

        expect (blockPathProcessor.getStaticSilentSidechainBlockCount() > 0,
                "static 5.1 processing enters the silent-sidechain block path");
        expect (blockPathProcessor.getProcessedFftFrameCount() > 0,
                "Voice wakes FFT processing after the block sleep path");
        if (! processorPathsMatch)
            std::cerr << "Block/sample path maximum error: "
                      << maximumProcessorPathError << " at block "
                      << maximumErrorBlock << ", channel "
                      << maximumErrorChannel << ", sample "
                      << maximumErrorSample << '\n';
        expect (processorPathsMatch,
                "block sleep, Voice wake, and return to sleep match the sample path");
    }

    {
        constexpr auto blockSize = 127;
        constexpr auto silentBlocks = 80;
        constexpr auto voiceBlocks = 96;
        constexpr auto trailingBlocks = 112;
        SmartDuckingAudioProcessor blockPathProcessor (
            smart_ducking::PluginVariant::stereo);
        SmartDuckingAudioProcessor samplePathProcessor (
            smart_ducking::PluginVariant::stereo);
        setParameter (blockPathProcessor,
                      smart_ducking::parameter_ids::voiceFormat,
                      1.0f);
        setParameter (samplePathProcessor,
                      smart_ducking::parameter_ids::voiceFormat,
                      1.0f);
        blockPathProcessor.prepareToPlay (48000.0, blockSize);
        samplePathProcessor.prepareToPlay (48000.0, blockSize);
        samplePathProcessor.notifyEditorOpened();
        juce::AudioBuffer<float> blockBuffer (8, blockSize);
        juce::AudioBuffer<float> sampleBuffer (8, blockSize);
        juce::MidiBuffer blockMidi;
        juce::MidiBuffer sampleMidi;
        auto processorPathsMatch = true;
        auto maximumProcessorPathError = 0.0f;
        auto phase = 0.0;

        for (int block = 0; block < silentBlocks + voiceBlocks + trailingBlocks; ++block)
        {
            blockBuffer.clear();
            const auto voiceActive = block >= silentBlocks
                                  && block < silentBlocks + voiceBlocks;

            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto signal = 0.2f * static_cast<float> (std::sin (phase));
                phase += 0.021;
                blockBuffer.setSample (0, sample, signal);
                blockBuffer.setSample (1, sample, 0.83f * signal);

                if (voiceActive)
                    for (int channel = 0; channel < 6; ++channel)
                        blockBuffer.setSample (2 + channel,
                                               sample,
                                               0.35f - 0.02f * channel);
            }

            sampleBuffer.makeCopyOf (blockBuffer, true);
            blockPathProcessor.processBlock (blockBuffer, blockMidi);
            samplePathProcessor.processBlock (sampleBuffer, sampleMidi);

            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto error = std::abs (
                        blockBuffer.getSample (channel, sample)
                        - sampleBuffer.getSample (channel, sample));
                    processorPathsMatch = processorPathsMatch && error <= 3.0e-5f;
                    maximumProcessorPathError = juce::jmax (maximumProcessorPathError,
                                                            error);
                }
        }

        expect (blockPathProcessor.getStaticSilentSidechainBlockCount() > 0,
                "static stereo processing with surround Voice enters block sleep");
        expect (blockPathProcessor.getProcessedFftFrameCount() > 0,
                "surround Voice wakes stereo FFT processing after block sleep");
        if (! processorPathsMatch)
            std::cerr << "Stereo block/sample path maximum error: "
                      << maximumProcessorPathError << '\n';
        expect (processorPathsMatch,
                "stereo block sleep, surround Voice wake, and return match sample path");
    }

    {
        SmartDuckingAudioProcessor offlineProcessor (
            smart_ducking::PluginVariant::surround);
        constexpr int offlineBlockSize = 128;
        offlineProcessor.prepareToPlay (48000.0, offlineBlockSize);
        offlineProcessor.notifyEditorOpened();
        offlineProcessor.setNonRealtime (true);
        expect (offlineProcessor.isOfflineRenderingActive()
                    && ! offlineProcessor.isSpectrumVisualisationActive(),
                "offline rendering state reaches the UI before its first audio block");
        juce::AudioBuffer<float> offlineBuffer (12, offlineBlockSize);
        juce::MidiBuffer offlineMidi;
        offlineBuffer.clear();
        offlineProcessor.processBlock (offlineBuffer, offlineMidi);
        expect (offlineProcessor.isOfflineRenderingActive()
                    && ! offlineProcessor.isSpectrumVisualisationActive(),
                "offline rendering suppresses spectrum and meter production");
        expect (offlineProcessor.getStaticSilentSidechainBlockCount() > 0,
                "static 5.1 offline render uses the silent-sidechain block path");

        offlineProcessor.setNonRealtime (false);
        expect (! offlineProcessor.isOfflineRenderingActive(),
                "offline rendering state clears without requiring another audio block");
        offlineProcessor.processBlock (offlineBuffer, offlineMidi);
        expect (! offlineProcessor.isOfflineRenderingActive()
                    && offlineProcessor.isSpectrumVisualisationActive(),
                "realtime processing restores editor visualisation after rendering");
    }

    smart_ducking::StereoSpectralLcrProcessor leftFastPathSuppressor;
    leftFastPathSuppressor.prepare (48000.0);

    for (int sample = 0; sample < leftFastPathSuppressor.getLatencySamples() * 2; ++sample)
        static_cast<void> (leftFastPathSuppressor.processSample (0.25f, 0.25f, 1.0f, 0.0f));

    expect (leftFastPathSuppressor.getSidechainMode() == SidechainMode::leftOnly,
            "one-sided Voice uses the matching direct LCR route");
    expect (leftFastPathSuppressor.getProcessedSidechainFftCount()
                == leftFastPathSuppressor.getProcessedFftFrameCount(),
            "one-sided Voice performs one forward FFT per active Main frame");
    expect (leftFastPathSuppressor.getProcessedSidechainPceFrameCount() == 0,
            "one-sided Voice bypasses sidechain PCE");

    smart_ducking::StereoSpectralLcrProcessor dualMonoFastPathSuppressor;
    dualMonoFastPathSuppressor.prepare (48000.0);

    for (int sample = 0; sample < dualMonoFastPathSuppressor.getLatencySamples() * 2; ++sample)
        static_cast<void> (dualMonoFastPathSuppressor.processSample (0.25f, 0.25f, 1.0f, 1.0f));

    expect (dualMonoFastPathSuppressor.getSidechainMode() == SidechainMode::dualMono,
            "dual-mono Voice uses the direct centre route");
    expect (dualMonoFastPathSuppressor.getProcessedSidechainFftCount()
                == dualMonoFastPathSuppressor.getProcessedFftFrameCount(),
            "dual-mono Voice performs one forward FFT per active Main frame");
    expect (dualMonoFastPathSuppressor.getProcessedSidechainPceFrameCount() == 0,
            "dual-mono Voice bypasses sidechain PCE");

    smart_ducking::StereoSpectralLcrProcessor stereoSidechainSuppressor;
    stereoSidechainSuppressor.prepare (48000.0);
    double stereoVoiceLeftPhase = 0.0;
    double stereoVoiceRightPhase = 0.0;

    for (int sample = 0; sample < stereoSidechainSuppressor.getLatencySamples() * 2; ++sample)
    {
        const auto voiceLeft = static_cast<float> (std::sin (stereoVoiceLeftPhase));
        const auto voiceRight = static_cast<float> (std::sin (stereoVoiceRightPhase));
        stereoVoiceLeftPhase += juce::MathConstants<double>::twoPi * 700.0 / 48000.0;
        stereoVoiceRightPhase += juce::MathConstants<double>::twoPi * 1300.0 / 48000.0;
        static_cast<void> (stereoSidechainSuppressor.processSample (0.25f,
                                                                    0.25f,
                                                                    voiceLeft,
                                                                    voiceRight));
    }

    expect (stereoSidechainSuppressor.getSidechainMode() == SidechainMode::stereo,
            "decorrelated Voice retains stereo PCE");
    expect (stereoSidechainSuppressor.getProcessedSidechainFftCount()
                == 2 * stereoSidechainSuppressor.getProcessedSidechainPceFrameCount(),
            "stereo Voice performs two forward FFTs for each sidechain PCE frame");
    expect (stereoSidechainSuppressor.getProcessedSidechainPceFrameCount()
                == stereoSidechainSuppressor.getProcessedFftFrameCount(),
            "stereo Voice runs one sidechain PCE per active Main frame");

    smart_ducking::StereoSpectralLcrProcessor weightingCacheSuppressor;
    weightingCacheSuppressor.prepare (48000.0);
    auto weightingCacheParameters =
        smart_ducking::StereoSpectralLcrProcessor::Parameters {};
    weightingCacheParameters.attackMs = 0.0f;
    weightingCacheSuppressor.setParameters (weightingCacheParameters);
    const auto processWeightingFrames = [&weightingCacheSuppressor] (int samples)
    {
        for (int sample = 0; sample < samples; ++sample)
            static_cast<void> (weightingCacheSuppressor.processSample (
                0.25f, 0.20f, 1.0f, 0.0f));
    };
    processWeightingFrames (weightingCacheSuppressor.getLatencySamples() * 2);
    const auto initialWeightingUpdates = weightingCacheSuppressor.getWeightingCurveUpdateCount();
    weightingCacheSuppressor.setParameters (weightingCacheParameters);
    processWeightingFrames (weightingCacheSuppressor.getLatencySamples());
    expect (initialWeightingUpdates == 1
                && weightingCacheSuppressor.getWeightingCurveUpdateCount()
                       == initialWeightingUpdates,
            "unchanged Weighting EQ parameters reuse the cached FFT curve");
    weightingCacheParameters.weightGainDb += 1.0f;
    weightingCacheSuppressor.setParameters (weightingCacheParameters);
    processWeightingFrames (weightingCacheSuppressor.getLatencySamples());
    expect (weightingCacheSuppressor.getWeightingCurveUpdateCount()
                == initialWeightingUpdates + 1,
            "changing Weighting EQ invalidates its FFT curve exactly once");

    smart_ducking::StereoSpectralLcrProcessor immediateMaskSuppressor;
    immediateMaskSuppressor.prepare (48000.0);
    auto immediateMaskParameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
    immediateMaskParameters.attackMs = 0.0f;
    immediateMaskParameters.holdMs = 0.0f;
    immediateMaskParameters.duckDepthDb = 0.0f;
    immediateMaskParameters.amountDb = 10.0f;
    immediateMaskSuppressor.setParameters (immediateMaskParameters);

    for (int sample = 0; sample < immediateMaskSuppressor.getLatencySamples(); ++sample)
    {
        const auto input = std::sin (juce::MathConstants<double>::twoPi * 3000.0
                                     * static_cast<double> (sample) / 48000.0);
        static_cast<void> (immediateMaskSuppressor.processSample (static_cast<float> (input),
                                                                  0.0f,
                                                                  1.0f,
                                                                  0.0f));
    }

    expect (immediateMaskSuppressor.getLastMaximumReductionDb (0) > 9.0f,
            "a waking spectral mask adds no second attack after the shared voice envelope");

    smart_ducking::StereoSpectralLcrProcessor pceProcessor;
    pceProcessor.prepare (48000.0);
    auto pceParameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
    pceParameters.amountDb = 0.0f;
    pceProcessor.setParameters (pceParameters);
    const auto pceLatency = pceProcessor.getLatencySamples();
    float pceBeforeLatency = 0.0f;
    smart_ducking::StereoSpectralLcrProcessor::LcrSample pceImpulse;

    for (int sample = 0; sample <= pceLatency; ++sample)
    {
        const auto input = sample == 0 ? 1.0f : 0.0f;
        const auto output = pceProcessor.processSample (input, input, 1.0f, 1.0f);
        const auto reconstructedLeft = output.left
                                     + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                           * output.centre;
        const auto reconstructedRight = output.right
                                      + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                            * output.centre;

        if (sample < pceLatency)
            pceBeforeLatency = juce::jmax (pceBeforeLatency,
                                          juce::jmax (std::abs (reconstructedLeft),
                                                      std::abs (reconstructedRight)));
        else
            pceImpulse = output;
    }

    expect (pceBeforeLatency < 1.0e-6f, "active PCE emits no signal before reported latency");
    expect (std::abs (pceImpulse.left) < 0.002f && std::abs (pceImpulse.right) < 0.002f,
            "dual-mono impulse is removed from PCE side residuals");
    expect (approximatelyEqual (
                pceImpulse.centre,
                1.0f / smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain,
                0.003f),
            "dual-mono impulse is routed to the energy-normalised centre");
    expect (approximatelyEqual (
                pceImpulse.left
                    + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain * pceImpulse.centre,
                1.0f,
                0.003f),
            "LCR downmix exactly reconstructs the dual-mono impulse");

    pceProcessor.reset();
    smart_ducking::StereoSpectralLcrProcessor::LcrSample hardLeftImpulse;

    for (int sample = 0; sample <= pceLatency; ++sample)
        hardLeftImpulse = pceProcessor.processSample (sample == 0 ? 1.0f : 0.0f,
                                                      0.0f,
                                                      1.0f,
                                                      1.0f);

    expect (approximatelyEqual (hardLeftImpulse.left, 1.0f, 0.003f),
            "hard-left impulse remains in PCE left");
    expect (std::abs (hardLeftImpulse.centre) < 0.002f
                && std::abs (hardLeftImpulse.right) < 0.002f,
            "hard-left impulse does not leak into PCE centre or right");

    pceProcessor.reset();
    smart_ducking::StereoSpectralLcrProcessor::LcrSample antiPhaseImpulse;

    for (int sample = 0; sample <= pceLatency; ++sample)
        antiPhaseImpulse = pceProcessor.processSample (sample == 0 ? 1.0f : 0.0f,
                                                       sample == 0 ? -1.0f : 0.0f,
                                                       1.0f,
                                                       1.0f);

    expect (std::abs (antiPhaseImpulse.centre) < 0.002f,
            "anti-phase impulse is rejected by PCE centre");
    expect (approximatelyEqual (antiPhaseImpulse.left, 1.0f, 0.003f)
                && approximatelyEqual (antiPhaseImpulse.right, -1.0f, 0.003f),
            "anti-phase impulse remains in the side residuals");

    pceProcessor.reset();
    constexpr int pceToneSamples = 24576;
    constexpr int pceToneWarmup = 8192;
    constexpr float pceToneAmplitude = 0.25f;
    double pceTonePhase = 0.0;
    const auto pceToneIncrement = juce::MathConstants<double>::twoPi * 1000.0 / 48000.0;
    double pceSideEnergy = 0.0;
    double pceCentreEnergy = 0.0;
    double pceInputEnergy = 0.0;

    for (int sample = 0; sample < pceToneSamples; ++sample)
    {
        const auto input = pceToneAmplitude * static_cast<float> (std::sin (pceTonePhase));
        pceTonePhase += pceToneIncrement;
        const auto output = pceProcessor.processSample (input, input, 1.0f, 1.0f);

        if (sample >= pceToneWarmup)
        {
            pceSideEnergy += static_cast<double> (output.left) * output.left
                           + static_cast<double> (output.right) * output.right;
            pceCentreEnergy += static_cast<double> (output.centre) * output.centre;
            pceInputEnergy += static_cast<double> (input) * input;
        }
    }

    expect (std::sqrt (pceSideEnergy / pceInputEnergy) < 0.03,
            "correlated dual-mono tone is removed from PCE sides");
    expect (approximatelyEqual (static_cast<float> (std::sqrt (pceCentreEnergy / pceInputEnergy)),
                                1.0f / smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain,
                                0.03f),
            "correlated dual-mono tone is routed to PCE centre");

    pceProcessor.reset();
    double decorrelatedLeftPhase = 0.0;
    double decorrelatedRightPhase = 0.0;
    const auto decorrelatedLeftIncrement = juce::MathConstants<double>::twoPi * 997.0 / 48000.0;
    const auto decorrelatedRightIncrement = juce::MathConstants<double>::twoPi * 1733.0 / 48000.0;
    double decorrelatedCentreEnergy = 0.0;
    double decorrelatedInputEnergy = 0.0;

    for (int sample = 0; sample < pceToneSamples; ++sample)
    {
        const auto inputLeft = pceToneAmplitude * static_cast<float> (std::sin (decorrelatedLeftPhase));
        const auto inputRight = pceToneAmplitude * static_cast<float> (std::sin (decorrelatedRightPhase));
        decorrelatedLeftPhase += decorrelatedLeftIncrement;
        decorrelatedRightPhase += decorrelatedRightIncrement;
        const auto output = pceProcessor.processSample (inputLeft, inputRight, 1.0f, 1.0f);

        if (sample >= pceToneWarmup)
        {
            decorrelatedCentreEnergy += static_cast<double> (output.centre) * output.centre;
            decorrelatedInputEnergy += static_cast<double> (inputLeft) * inputLeft
                                     + static_cast<double> (inputRight) * inputRight;
        }
    }

    expect (std::sqrt (decorrelatedCentreEnergy / decorrelatedInputEnergy) < 0.05,
            "different-frequency stereo tones are rejected by PCE centre");

    const std::array<int, 7> stressBlockSizes { 1, 17, 127, 256, 512, 2048, 4096 };

    for (const auto stressBlockSize : stressBlockSizes)
    {
        for (int resolution = 0; resolution < 3; ++resolution)
        {
            SmartDuckingAudioProcessor stressProcessor;
            setParameter (stressProcessor, smart_ducking::parameter_ids::attackMs, 0.0f);
            setParameter (stressProcessor, smart_ducking::parameter_ids::duckDepthDb, 10.0f);
            setParameter (stressProcessor, smart_ducking::parameter_ids::spectralAmountDb, 10.0f);
            setParameter (stressProcessor,
                          smart_ducking::parameter_ids::fftResolution,
                          static_cast<float> (resolution));
            stressProcessor.prepareToPlay (48000.0, stressBlockSize);
            const auto stressVoiceOffset = stressProcessor.getChannelCountOfBus (true, 0);
            juce::AudioBuffer<float> stressBuffer (stressVoiceOffset + 6, stressBlockSize);
            juce::MidiBuffer stressMidi;
            double stressPhase = 0.0;
            const auto stressPhaseIncrement = juce::MathConstants<double>::twoPi * 1234.0 / 48000.0;
            const auto stressSamples = (1024 << resolution) * 3;
            bool stressOutputIsFinite = true;

            for (int processed = 0; processed < stressSamples; processed += stressBlockSize)
            {
                stressBuffer.clear();
                const auto voiceActive = (processed / juce::jmax (1, 1024 << resolution)) % 2 == 0;

                for (int sample = 0; sample < stressBlockSize; ++sample)
                {
                    const auto value = 0.25f * static_cast<float> (std::sin (stressPhase));
                    stressPhase += stressPhaseIncrement;
                    stressBuffer.setSample (0, sample, value);
                    stressBuffer.setSample (1, sample, 0.7f * value);
                    stressBuffer.setSample (stressVoiceOffset,
                                            sample,
                                            voiceActive ? 0.2f : 0.0f);
                    stressBuffer.setSample (stressVoiceOffset + 1,
                                            sample,
                                            voiceActive ? 0.15f : 0.0f);
                }

                stressProcessor.processBlock (stressBuffer, stressMidi);

                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < stressBlockSize; ++sample)
                        stressOutputIsFinite = stressOutputIsFinite
                                            && std::isfinite (stressBuffer.getSample (channel, sample));
            }

            expect (stressOutputIsFinite, "PCE stress output remains finite across host block sizes");
        }
    }

    {
        const auto fixedBlocks = renderDeterministicSignal ({ 128 });
        const auto variableBlocks = renderDeterministicSignal (
            { 1, 17, 63, 128, 257, 511, 31 }, true);
        auto maximumDifference = 0.0f;

        for (size_t channel = 0; channel < fixedBlocks.size(); ++channel)
            for (size_t sample = 0; sample < fixedBlocks[channel].size(); ++sample)
                maximumDifference = std::max (
                    maximumDifference,
                    std::abs (fixedBlocks[channel][sample] - variableBlocks[channel][sample]));

        expect (maximumDifference < 1.0e-5f,
                "realtime and offline-style variable block renders are numerically equivalent");
    }

    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 10.0f);
    const auto spectrallyDucked = processTone (processor, 3000.0, 1.0f, 0.0f, 1.0f, 0.0f);
    expect (spectrallyDucked.leftRatio < 0.75,
            "variant A suppresses a prominent extracted-left component for left voice");
    expect (spectrallyDucked.rightRatio < 0.003,
            "left LCR spectral suppression does not create right-channel spill");
    expect (processor.getSpectralReductionLeftDb() > 3.0f, "left spectral GR meter");
    expect (processor.getSpectralReductionRightDb() < 0.01f, "silent right detector leaves spectral GR at zero");

    const auto leftVoiceOnSpectralCentre = processTone (processor, 3000.0, 1.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (leftVoiceOnSpectralCentre.leftRatio), 1.0f, 0.004f)
                && approximatelyEqual (static_cast<float> (leftVoiceOnSpectralCentre.rightRatio), 1.0f, 0.004f),
            "left voice does not spectrally suppress an extracted phantom-centre main");

    const auto centreVoiceOnSpectralCentre = processTone (processor, 3000.0, 1.0f, 1.0f);
    expect (centreVoiceOnSpectralCentre.leftRatio < 0.75
                && centreVoiceOnSpectralCentre.rightRatio < 0.75,
            "centre voice spectrally suppresses the extracted main centre before downmix");

    processor.notifyEditorClosed();

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    expect (editor != nullptr, "custom editor creation");

    if (editor != nullptr)
    {
        expect (editor->getWidth() == 1000, "custom editor default width");
        expect (editor->getHeight() == 650, "custom editor default height");

        struct VisibleControls
        {
            int sliders = 0;
            int comboBoxes = 0;
            int toggleButtons = 0;
            int linearSliders = 0;
            int tabbedComponents = 0;
            bool haveBounds = true;
        };

        const auto isEffectivelyVisible = [] (const juce::Component& candidate)
        {
            for (auto* component = &candidate; component != nullptr;
                 component = component->getParentComponent())
            {
                if (dynamic_cast<const juce::AudioProcessorEditor*> (component) != nullptr)
                    break;

                if (! component->isVisible())
                    return false;
            }

            return true;
        };

        const auto inspectVisibleControls = [&] (juce::Component& root)
        {
            VisibleControls result;
            std::function<void (juce::Component&)> inspect;
            inspect = [&] (juce::Component& component)
            {
                if (isEffectivelyVisible (component))
                {
                    if (auto* slider = dynamic_cast<juce::Slider*> (&component))
                    {
                        ++result.sliders;
                        const auto style = slider->getSliderStyle();
                        result.linearSliders += style == juce::Slider::LinearHorizontal
                                                    || style == juce::Slider::LinearBar ? 1 : 0;
                        result.haveBounds = result.haveBounds
                                         && ! component.getBounds().isEmpty();
                    }

                    if (dynamic_cast<juce::ComboBox*> (&component) != nullptr)
                    {
                        ++result.comboBoxes;
                        result.haveBounds = result.haveBounds
                                         && ! component.getBounds().isEmpty();
                    }

                    if (dynamic_cast<juce::ToggleButton*> (&component) != nullptr)
                    {
                        ++result.toggleButtons;
                        result.haveBounds = result.haveBounds
                                         && ! component.getBounds().isEmpty();
                    }

                    if (dynamic_cast<juce::TabbedComponent*> (&component) != nullptr)
                        ++result.tabbedComponents;
                }

                for (int child = 0; child < component.getNumChildComponents(); ++child)
                    inspect (*component.getChildComponent (child));
            };
            inspect (root);
            return result;
        };

        const auto findTextButton = [] (juce::Component& root,
                                        const juce::String& text) -> juce::TextButton*
        {
            std::function<juce::TextButton* (juce::Component&)> find;
            find = [&] (juce::Component& component) -> juce::TextButton*
            {
                if (auto* button = dynamic_cast<juce::TextButton*> (&component);
                    button != nullptr && button->getButtonText() == text)
                    return button;

                for (int child = 0; child < component.getNumChildComponents(); ++child)
                    if (auto* result = find (*component.getChildComponent (child)))
                        return result;

                return nullptr;
            };
            return find (root);
        };

        const auto findComponentWithId = [] (juce::Component& root,
                                             const juce::String& id) -> juce::Component*
        {
            std::function<juce::Component* (juce::Component&)> find;
            find = [&] (juce::Component& component) -> juce::Component*
            {
                if (component.getComponentID() == id)
                    return &component;

                for (int child = 0; child < component.getNumChildComponents(); ++child)
                    if (auto* result = find (*component.getChildComponent (child)))
                        return result;

                return nullptr;
            };
            return find (root);
        };

        const auto activate = [&] (const char* buttonText)
        {
            auto* button = findTextButton (*editor, buttonText);
            expect (button != nullptr, buttonText);

            if (button != nullptr && button->onClick)
                button->onClick();
        };

        auto visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 7 && visible.comboBoxes == 4,
                "Dynamics exposes seven controls plus preset, processing, FFT, and Voice selectors");
        expect (visible.toggleButtons == 0,
                "Stereo Dynamics hides Surround-only output controls");
        expect (visible.linearSliders == 0, "Dynamics uses rotary controls");
        expect (visible.tabbedComponents == 0,
                "redesigned editor uses lightweight Dynamics/Routing pages rather than TabbedComponent");
        expect (visible.haveBounds, "all visible Dynamics controls have a non-empty layout");
        expect (findTextButton (*editor, "SIMPLE") == nullptr
                    && findTextButton (*editor, "ADVANCED") == nullptr
                    && findTextButton (*editor, "SPECTRUM") == nullptr,
                "obsolete Simple/Advanced and Spectrum navigation is absent");
        const auto* globalBypassButton = dynamic_cast<juce::TextButton*> (
            findComponentWithId (*editor, "globalBypassButton"));
        const auto* globalBypassPanel = findComponentWithId (
            *editor, "globalBypassPanel");
        expect (globalBypassButton != nullptr
                    && globalBypassButton->getClickingTogglesState()
                    && ! globalBypassButton->getBounds().isEmpty()
                    && globalBypassButton->getParentComponent() != nullptr
                    && globalBypassButton->getParentComponent()->getComponentID()
                           == "globalBypassPanel"
                    && globalBypassPanel != nullptr
                    && globalBypassPanel->getParentComponent() != nullptr
                    && dynamic_cast<juce::AudioProcessorEditor*> (
                           globalBypassPanel->getParentComponent()) != nullptr,
                "second row contains global bypass in a separate sibling mini-panel");
        const auto* presetSelector = dynamic_cast<juce::ComboBox*> (
            findComponentWithId (*editor, "presetChoice"));
        const auto* comparisonA = findTextButton (*editor, "A");
        const auto* comparisonB = findTextButton (*editor, "B");
        const auto* voiceFormatSelector = findComponentWithId (
            *editor, "voiceFormatChoice");
        expect (presetSelector != nullptr
                    && presetSelector->getText().isNotEmpty()
                    && ! presetSelector->getBounds().isEmpty()
                    && comparisonA != nullptr
                    && presetSelector->getRight() < comparisonA->getX()
                    && presetSelector->getX() == 326
                    && comparisonA->getX() - presetSelector->getRight() == 8
                    && comparisonB != nullptr
                    && comparisonB->getX() - comparisonA->getRight() == 3,
                "preset selector and A/B form a stable left-aligned group after the branding");
        expect (comparisonB != nullptr && voiceFormatSelector != nullptr
                    && voiceFormatSelector->getX() - comparisonB->getRight() >= 14,
                "left-aligned A/B comparison remains separated from the right header controls");
        expect (findTextButton (*editor, "LOW") != nullptr
                    && findTextButton (*editor, "HIGH") != nullptr
                    && findComponentWithId (*editor, "stageBypassButton0") != nullptr,
                "Crossover strip exposes icon bypass plus LOW and HIGH monitors");
        for (int stage = 0; stage < 3; ++stage)
        {
            const auto* label = findComponentWithId (
                *editor, "stageLabel" + juce::String (stage));
            const auto* bypass = findComponentWithId (
                *editor, "stageBypassButton" + juce::String (stage));
            expect (label != nullptr && bypass != nullptr
                        && bypass->getX() - label->getRight() == 10,
                    "each stage label has the same compact gap before its controls");
        }
        const auto* spectralBypass = findComponentWithId (
            *editor, "stageBypassButton2");
        const auto* stagePanel = findComponentWithId (*editor, "stageControlPanel");
        expect (globalBypassPanel != nullptr && globalBypassButton != nullptr
                    && spectralBypass != nullptr && stagePanel != nullptr
                    && globalBypassPanel->getX() - stagePanel->getRight() == 8
                    && globalBypassPanel->getY() == stagePanel->getY()
                    && globalBypassPanel->getWidth() == 50
                    && globalBypassPanel->getHeight() == stagePanel->getHeight()
                    && globalBypassButton->getBounds()
                           == globalBypassPanel->getLocalBounds()
                                  .withSizeKeepingCentre (34, 32),
                "stage card and outlined global-bypass card are separate sibling groups");

        // This event emulates the tiny smooth deltas produced by a macOS
        // trackpad.  Asking JUCE for a desktop mouse source from a headless
        // Windows console test is not portable; parameter choices and DSP for
        // every slope are covered above on all platforms.
#if JUCE_MAC
        if (auto* spectrum = findComponentWithId (*editor, "spectrumPanel"))
        {
            juce::Image spectrumSnapshot (juce::Image::RGB,
                                          spectrum->getWidth(),
                                          spectrum->getHeight(),
                                          true);
            juce::Graphics spectrumGraphics (spectrumSnapshot);
            spectrum->paintEntireComponent (spectrumGraphics, true);
            auto plot = spectrum->getLocalBounds().toFloat().reduced (1.0f)
                           .reduced (12.0f, 10.0f)
                           .withTrimmedLeft (30.0f)
                           .withTrimmedRight (30.0f);
            const auto crossoverX = plot.getX() + static_cast<float> (
                std::log (92.0f / 20.0f) / std::log (20000.0f / 20.0f))
                    * plot.getWidth();
            // The entire vertical drag stripe must also accept wheel input,
            // not only the small circular handle or its text label.
            const auto position = juce::Point<float> {
                crossoverX, plot.getBottom() - 70.0f
            };
            const auto eventTime = juce::Time::getCurrentTime();
            const auto mouseEvent = juce::MouseEvent (
                juce::Desktop::getInstance().getMainMouseSource(),
                position,
                juce::ModifierKeys {},
                0.0f,
                0.0f,
                0.0f,
                0.0f,
                0.0f,
                spectrum,
                spectrum,
                eventTime,
                position,
                eventTime,
                1,
                false);
            const auto trackpadWheel = juce::MouseWheelDetails {
                0.0f, 0.001f, false, true, false
            };
            spectrum->mouseWheelMove (mouseEvent, trackpadWheel);
            expect (approximatelyEqual (
                        *processor.getParameterState().getRawParameterValue (
                            smart_ducking::parameter_ids::crossoverSlope),
                        3.0f),
                    "a tiny smooth macOS trackpad delta changes Crossover Slope anywhere in its drag stripe");
            setParameter (processor, smart_ducking::parameter_ids::crossoverSlope, 2.0f);
        }
        else
        {
            expect (false, "spectrum panel is available for trackpad interaction testing");
        }
#endif

        activate ("ROUTING");
        visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 9 && visible.comboBoxes == 2,
                "Stereo Routing exposes the active 3 by 3 matrix plus preset and Voice selectors");
        expect (visible.linearSliders == 9, "Routing matrix uses linear sliders");
        expect (visible.haveBounds, "all visible Routing controls have a non-empty layout");

        if (auto* symmetry = findTextButton (*editor, "L/R LINK"))
        {
            symmetry->setToggleState (true, juce::dontSendNotification);

            if (symmetry->onClick)
                symmetry->onClick();
        }
        else
        {
            expect (false, "Routing exposes the L/R symmetry edit link");
        }

        activate ("DYNAMICS");
        juce::Image snapshot (juce::Image::RGB, editor->getWidth(), editor->getHeight(), true);
        juce::Graphics snapshotGraphics (snapshot);
        editor->paintEntireComponent (snapshotGraphics, true);
        expect (snapshot.getPixelAt (10, 10).getAlpha() > 0, "custom editor paints off-screen");
        const auto logoPixel = snapshot.getPixelAt (20, 35);
        expect (logoPixel.getRed() > 220
                    && logoPixel.getGreen() > 220
                    && logoPixel.getBlue() > 220,
                "embedded SVG RusAD logo paints in the editor header");
        activate ("ROUTING");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> surroundEditor (surroundProcessor.createEditor());
        surroundEditor->setSize (960, 650);
        int visibleFormatComboCount = 0;
        int visibleSurroundToggleCount = 0;
        juce::ToggleButton* downmixButton = nullptr;
        juce::ToggleButton* dropLfeButton = nullptr;
        juce::ComboBox* surroundPresetSelector = nullptr;
        juce::ComboBox* voiceFormatSelector = nullptr;
        std::function<void (juce::Component&)> inspectSurroundEditor;
        inspectSurroundEditor = [&] (juce::Component& component)
        {
            const auto visible = [&]
            {
                for (auto* candidate = &component; candidate != nullptr;
                     candidate = candidate->getParentComponent())
                {
                    if (dynamic_cast<juce::AudioProcessorEditor*> (candidate) != nullptr)
                        break;

                    if (! candidate->isVisible())
                        return false;
                }

                return true;
            }();

            if (visible)
                if (auto* comboBox = dynamic_cast<juce::ComboBox*> (&component))
                {
                    ++visibleFormatComboCount;

                    if (component.getComponentID() == "presetChoice")
                        surroundPresetSelector = comboBox;
                    else if (component.getComponentID() == "voiceFormatChoice")
                        voiceFormatSelector = comboBox;
                }

            if (visible)
            {
                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (&component))
                {
                    ++visibleSurroundToggleCount;

                    if (toggle->getButtonText() == "DOWNMIX")
                        downmixButton = toggle;
                    else if (toggle->getButtonText() == "DROP LFE")
                        dropLfeButton = toggle;
                }
            }

            for (int child = 0; child < component.getNumChildComponents(); ++child)
                inspectSurroundEditor (*component.getChildComponent (child));
        };
        inspectSurroundEditor (*surroundEditor);
        expect (visibleFormatComboCount == 4,
                "Surround Dynamics exposes preset, two processing selectors, and Voice Format without Main Format");
        expect (surroundPresetSelector != nullptr
                    && surroundPresetSelector->getX() == 326,
                "minimum-width Surround header anchors presets directly after full branding");
        expect (voiceFormatSelector != nullptr
                    && voiceFormatSelector->getProperties()["comboContextLabel"]
                           .toString() == "VOICE"
                    && voiceFormatSelector->getHeight() == 36,
                "Voice Format selector carries its compact context label inside the control");
        expect (visibleSurroundToggleCount == 2,
                "Surround header exposes Downmix and Drop LFE; VLC order moved to the meter menu");
        expect (downmixButton != nullptr && dropLfeButton != nullptr
                    && downmixButton->getY() == dropLfeButton->getY()
                    && downmixButton->getWidth() == 86
                    && dropLfeButton->getWidth() == 88
                    && downmixButton->getHeight() == 34
                    && dropLfeButton->getHeight() == 34
                    && downmixButton->getRight() < dropLfeButton->getX(),
                "Surround Downmix and Drop LFE retain their horizontal header layout");
        expect (downmixButton != nullptr && downmixButton->isEnabled(),
                "Surround Downmix control is clickable");
        expect (dropLfeButton != nullptr && ! dropLfeButton->isEnabled(),
                "Drop LFE remains disabled until Downmix is active");
        expect (surroundProcessor.getOutputMeterChannelCount() == 5,
                "Surround Output meter defaults to five spatial channels");
        setParameter (surroundProcessor, smart_ducking::parameter_ids::downmix, 1.0f);
        expect (surroundProcessor.getOutputMeterChannelCount() == 2,
                "Surround Output meter switches to L/R during forced downmix");
    }

    constexpr float savedWeightFrequency = 4200.0f;
    setParameter (processor, smart_ducking::parameter_ids::weightFrequencyHz, savedWeightFrequency);
    setParameter (processor, smart_ducking::parameter_ids::crossoverSlope, 4.0f);
    setParameter (processor, smart_ducking::parameter_ids::widebandBypass, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::globalBypass, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::mainChannelOrder, 2.0f);
    setParameter (processor, smart_ducking::parameter_ids::voiceChannelOrder, 3.0f);
    setParameter (processor, smart_ducking::parameter_ids::outputChannelOrder, 1.0f);
    setParameter (processor,
                  smart_ducking::parameter_ids::monitorMode,
                  static_cast<float> (smart_ducking::MonitorMode::spectralDelta));

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    SmartDuckingAudioProcessor restoredProcessor;
    restoredProcessor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));
    expect (approximatelyEqual (
                *restoredProcessor.getParameterState().getRawParameterValue (
                    smart_ducking::parameter_ids::weightFrequencyHz),
                savedWeightFrequency,
                0.1f),
            "parameter state round-trip");
    expect (approximatelyEqual (
                *restoredProcessor.getParameterState().getRawParameterValue (
                    smart_ducking::parameter_ids::crossoverSlope),
                4.0f),
            "Crossover Slope survives project-state round-trip");
    expect (approximatelyEqual (
                *restoredProcessor.getParameterState().getRawParameterValue (
                    smart_ducking::parameter_ids::widebandBypass),
                1.0f)
                && approximatelyEqual (
                    *restoredProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::monitorMode),
                    static_cast<float> (smart_ducking::MonitorMode::spectralDelta))
                && approximatelyEqual (
                    *restoredProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::globalBypass),
                    1.0f),
            "global/stage bypass and Monitor Mode survive state round-trip");
    expect (approximatelyEqual (
                *restoredProcessor.getParameterState().getRawParameterValue (
                    smart_ducking::parameter_ids::mainChannelOrder),
                2.0f)
                && approximatelyEqual (
                    *restoredProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::voiceChannelOrder),
                    3.0f)
                && approximatelyEqual (
                    *restoredProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::outputChannelOrder),
                    1.0f),
            "all three 5.1 channel orders survive project-state round-trip");
    expect (static_cast<int> (restoredProcessor.getParameterState().state.getProperty (
                "uiPage", -1)) == 1
                && ! restoredProcessor.getParameterState().state.hasProperty ("uiEditorMode")
                && ! restoredProcessor.getParameterState().state.hasProperty ("uiAdvancedPage")
                && static_cast<bool> (restoredProcessor.getParameterState().state.getProperty (
                    "uiMatrixSymmetryLink", false)),
            "Dynamics/Routing page and symmetry-link UI state survive state round-trip");

    {
        auto legacyState = processor.getParameterState().copyState();
        for (int childIndex = legacyState.getNumChildren(); --childIndex >= 0;)
            if (legacyState.getChild (childIndex).getProperty ("id").toString()
                == smart_ducking::parameter_ids::crossoverSlope)
                legacyState.removeChild (childIndex, nullptr);

        juce::MemoryBlock legacyStateData;
        if (const auto legacyXml = legacyState.createXml())
            juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyStateData);

        SmartDuckingAudioProcessor migratedProcessor;
        migratedProcessor.setStateInformation (legacyStateData.getData(),
                                                static_cast<int> (legacyStateData.getSize()));
        expect (approximatelyEqual (
                    *migratedProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::crossoverSlope),
                    2.0f),
                "projects saved before Crossover Slope load at 48 dB/oct");
    }

    {
        auto legacyState = processor.getParameterState().copyState();
        juce::ValueTree legacyParameter { "PARAM" };
        legacyParameter.setProperty ("id", "sidechainPreGainDb", nullptr);
        legacyParameter.setProperty ("value", 30.0f, nullptr);
        legacyState.appendChild (legacyParameter, nullptr);
        juce::MemoryBlock legacyStateData;

        if (const auto legacyXml = legacyState.createXml())
            juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyStateData);

        SmartDuckingAudioProcessor migratedProcessor;
        migratedProcessor.setStateInformation (legacyStateData.getData(),
                                                static_cast<int> (legacyStateData.getSize()));
        const auto migratedState = migratedProcessor.getParameterState().copyState();
        auto retainedLegacyParameter = false;

        for (const auto& child : migratedState)
            retainedLegacyParameter = retainedLegacyParameter
                                   || child.getProperty ("id").toString()
                                          == "sidechainPreGainDb";

        expect (! retainedLegacyParameter,
                "state migration removes the obsolete Sidechain Pre-Gain child");
        expect (approximatelyEqual (
                    *migratedProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::weightFrequencyHz),
                    savedWeightFrequency,
                    0.1f),
                "legacy state migration preserves current parameter values");
    }

    {
        auto legacyState = processor.getParameterState().copyState();
        auto foundLegacyVlcMode = false;

        for (auto child : legacyState)
        {
            if (child.getProperty ("id").toString()
                == smart_ducking::parameter_ids::mainChannelOrder)
            {
                child.setProperty ("value", 1.0f, nullptr);
                foundLegacyVlcMode = true;
                break;
            }
        }

        expect (foundLegacyVlcMode,
                "legacy APVTS state contains the retained VLC parameter ID");
        juce::MemoryBlock legacyStateData;
        if (const auto legacyXml = legacyState.createXml())
            juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyStateData);

        SmartDuckingAudioProcessor migratedProcessor;
        migratedProcessor.setStateInformation (legacyStateData.getData(),
                                                static_cast<int> (legacyStateData.getSize()));
        expect (approximatelyEqual (
                    *migratedProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::mainChannelOrder),
                    1.0f),
                "legacy VLC On state migrates to the REAPER / VLC channel-order choice");
    }

    {
        const auto loadLegacyProcessingMode = [&] (float legacyModeIndex)
        {
            auto legacyState = processor.getParameterState().copyState();
            auto foundProcessingMode = false;

            for (auto child : legacyState)
            {
                if (child.getProperty ("id").toString()
                    == smart_ducking::parameter_ids::processingMode)
                {
                    child.setProperty ("value", legacyModeIndex, nullptr);
                    foundProcessingMode = true;
                    break;
                }
            }

            expect (foundProcessingMode,
                    "legacy APVTS state contains the Processing Mode parameter");

            juce::MemoryBlock legacyStateData;

            if (const auto legacyXml = legacyState.createXml())
                juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyStateData);

            SmartDuckingAudioProcessor migratedProcessor;
            migratedProcessor.setStateInformation (legacyStateData.getData(),
                                                    static_cast<int> (legacyStateData.getSize()));
            return migratedProcessor.getParameterState().getRawParameterValue (
                smart_ducking::parameter_ids::processingMode)->load();
        };

        expect (approximatelyEqual (loadLegacyProcessingMode (0.0f), 0.0f),
                "legacy Normal index loads as QUALITY");
        expect (approximatelyEqual (loadLegacyProcessingMode (1.0f), 1.0f),
                "legacy Economy index loads as NORMAL");
    }

    if (failures == 0)
    {
        std::cout << "Smart Ducking crossover prototype tests passed\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed\n";
    return 1;
}
