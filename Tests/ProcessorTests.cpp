#include <cmath>
#include <array>
#include <functional>
#include <iostream>

#include "ParameterIds.h"
#include "PluginProcessor.h"
#include "LcrDuckMatrix.h"
#include "SidechainClassifier.h"
#include "SidechainEnvelope.h"
#include "SpectralSuppressor.h"

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
                                                     double sampleRate = 48000.0)
{
    SmartDuckingAudioProcessor processor;
    setParameter (processor, smart_ducking::parameter_ids::processingMode, 1.0f);
    setParameter (processor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
    setParameter (processor, smart_ducking::parameter_ids::lookaheadMs, lookaheadMs);
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
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuceGui;
    SmartDuckingAudioProcessor processor;
    SmartDuckingAudioProcessor surroundProcessor (smart_ducking::PluginVariant::surround);

    expect (processor.getName() == "Smart Ducking (Stereo)", "stereo plugin name");
    expect (surroundProcessor.getName() == "Smart Ducking (Surround)", "surround plugin name");
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
    expect (approximatelyEqual (*state.getRawParameterValue (smart_ducking::parameter_ids::duckDepthDb), 15.0f),
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
        expect (choices == juce::StringArray { "Normal", "Crossover Solo",
                                               "Crossover Delta", "Wideband Solo",
                                               "Wideband Delta", "Spectral Solo",
                                               "Spectral Delta" },
                "host Monitor Mode exposes one unambiguous Solo/Delta choice");
    }
    else
    {
        expect (false, "Monitor Mode parameter exists");
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

        SmartDuckingAudioProcessor crossoverSolo;
        configureLowProtection (crossoverSolo);
        setParameter (crossoverSolo,
                      smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverSolo));
        const auto soloResult = processTone (crossoverSolo, 1000.0, 1.0f, 1.0f);
        expect (soloResult.leftRatio > 0.95,
                "Crossover Solo bypasses Wideband and Spectral while retaining alignment");
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
                      static_cast<float> (smart_ducking::MonitorMode::crossoverDelta));
        setParameter (lowMonitor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (lowMonitor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto low = processTone (lowMonitor, 40.0, 0.0f, 0.0f);

        SmartDuckingAudioProcessor highMonitor;
        setParameter (highMonitor, smart_ducking::parameter_ids::processingMode, processingMode);
        setParameter (highMonitor, smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverDelta));
        setParameter (highMonitor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (highMonitor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const auto high = processTone (highMonitor, 1000.0, 0.0f, 0.0f);
        expect (low.leftRatio > 0.9 && high.leftRatio < 0.05,
                processingMode < 0.5f
                    ? "QUALITY Crossover Delta monitors only the protected low band"
                    : "NORMAL Crossover Delta monitors the complementary FFT low band");
        expect (lowMonitor.getLatencySamples() == highMonitor.getLatencySamples(),
                "monitoring does not change PDC");
    }

    {
        SmartDuckingAudioProcessor crossoverDeltaSurround (
            smart_ducking::PluginVariant::surround);
        setParameter (crossoverDeltaSurround,
                      smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverDelta));
        setParameter (crossoverDeltaSurround, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (crossoverDeltaSurround,
                      smart_ducking::parameter_ids::spectralAmountDb,
                      0.0f);
        const auto lfeOnly = processSpatialTone (crossoverDeltaSurround,
                                                 { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
                                                 {},
                                                 40.0);
        expect (*std::max_element (lfeOnly.outputRatios.begin(),
                                   lfeOnly.outputRatios.end()) < 0.001,
                "Crossover Delta excludes LFE from the monitor output");
    }
    {
        const auto zeroLookaheadImpulse = processImpulseWithLookahead (0.0f);
        const auto defaultLookaheadImpulse = processImpulseWithLookahead (10.0f);
        const auto maximumLookaheadImpulse = processImpulseWithLookahead (50.0f);
        const auto fortyFourOneImpulse = processImpulseWithLookahead (10.0f, 44100.0);
        const auto ninetySixImpulse = processImpulseWithLookahead (10.0f, 96000.0);
        constexpr int expectedLatency = 2048 + 2400;

        expect (zeroLookaheadImpulse.latency == expectedLatency
                    && defaultLookaheadImpulse.latency == expectedLatency
                    && maximumLookaheadImpulse.latency == expectedLatency,
                "Lookahead uses a fixed 50 ms PDC reserve at 48 kHz");
        expect (fortyFourOneImpulse.latency == 2048 + 2205
                    && ninetySixImpulse.latency == 4096 + 4800,
                "reported PDC follows STFT geometry plus 50 ms at 44.1 and 96 kHz");
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
                    && ninetySixImpulse.maximumBeforeLatency < 1.0e-6f
                    && approximatelyEqual (fortyFourOneImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f)
                    && approximatelyEqual (ninetySixImpulse.sampleAtLatency,
                                           1.0f,
                                           0.002f),
                "fixed Lookahead reserve reconstructs correctly at 44.1 and 96 kHz");

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

    {
        SmartDuckingAudioProcessor routingProcessor (smart_ducking::PluginVariant::surround);
        setParameter (routingProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (routingProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (routingProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);

        for (int channel = 0; channel < 6; ++channel)
        {
            std::array<float, 6> main {};
            main[static_cast<size_t> (channel)] = 1.0f;
            const auto routed = processSpatialTone (routingProcessor, main);

            for (int output = 0; output < 6; ++output)
                expect (approximatelyEqual (static_cast<float> (routed.outputRatios[static_cast<size_t> (output)]),
                                            output == channel ? 1.0f : 0.0f,
                                            0.004f),
                        "standard 5.1 routing preserves the canonical channel order");
        }
    }

    {
        SmartDuckingAudioProcessor vlcProcessor (smart_ducking::PluginVariant::surround);
        setParameter (vlcProcessor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
        setParameter (vlcProcessor, smart_ducking::parameter_ids::vlcMode, 1.0f);
        setParameter (vlcProcessor, smart_ducking::parameter_ids::duckDepthDb, 0.0f);
        setParameter (vlcProcessor, smart_ducking::parameter_ids::spectralAmountDb, 0.0f);
        const std::array<int, 6> vlcInputToCanonicalOutput { 0, 1, 4, 5, 2, 3 };

        for (int input = 0; input < 6; ++input)
        {
            std::array<float, 6> main {};
            main[static_cast<size_t> (input)] = 1.0f;
            const auto routed = processSpatialTone (vlcProcessor, main);

            for (int output = 0; output < 6; ++output)
                expect (approximatelyEqual (static_cast<float> (routed.outputRatios[static_cast<size_t> (output)]),
                                            output == vlcInputToCanonicalOutput[static_cast<size_t> (input)]
                                                ? 1.0f : 0.0f,
                                            0.004f),
                        "VLC mode remaps Main input to canonical 5.1 output");
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
            "LR8 reconstruction is flat at one kilohertz left");
    expect (approximatelyEqual (static_cast<float> (dryHigh.rightRatio), 1.0f, 0.002f),
            "LR8 reconstruction is flat at one kilohertz right");
    expect (approximatelyEqual (processor.getSidechainPeakLeft(), 0.0f), "silent left sidechain meter");
    expect (approximatelyEqual (processor.getSidechainPeakRight(), 0.0f), "silent right sidechain meter");

    const auto dryAtCrossover = processTone (processor, 92.0, 0.0f, 0.0f);
    expect (approximatelyEqual (static_cast<float> (dryAtCrossover.leftRatio), 1.0f, 0.003f),
            "LR8 reconstruction is flat at crossover");

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

        const auto activate = [&] (const char* buttonText)
        {
            auto* button = findTextButton (*editor, buttonText);
            expect (button != nullptr, buttonText);

            if (button != nullptr && button->onClick)
                button->onClick();
        };

        auto visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 6 && visible.comboBoxes == 1,
                "Simple mode exposes six essential controls and Voice Format");
        expect (visible.toggleButtons == 0,
                "Stereo Simple mode hides Surround-only output controls");
        expect (visible.linearSliders == 0, "Simple mode uses rotary controls");
        expect (visible.tabbedComponents == 0,
                "redesigned editor uses Simple/Advanced pages rather than TabbedComponent");
        expect (visible.haveBounds, "all visible Simple controls have a non-empty layout");

        activate ("ADVANCED");
        visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 6 && visible.comboBoxes == 1,
                "Advanced Dynamics exposes six controls including Lookahead and Voice Format");
        expect (visible.linearSliders == 0, "Dynamics uses rotary controls");

        activate ("SPECTRUM");
        visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 4 && visible.comboBoxes == 3,
                "Advanced Spectrum exposes four controls, two selectors, and Voice Format");
        expect (visible.linearSliders == 0, "Spectrum uses rotary controls");

        activate ("ROUTING");
        visible = inspectVisibleControls (*editor);
        expect (visible.sliders == 9 && visible.comboBoxes == 1,
                "Stereo Routing exposes the active 3 by 3 matrix and Voice Format");
        expect (visible.linearSliders == 9, "Routing matrix uses linear sliders");
        expect (visible.haveBounds, "all visible Advanced controls have a non-empty layout");

        activate ("SIMPLE");
        juce::Image snapshot (juce::Image::RGB, editor->getWidth(), editor->getHeight(), true);
        juce::Graphics snapshotGraphics (snapshot);
        editor->paintEntireComponent (snapshotGraphics, true);
        expect (snapshot.getPixelAt (10, 10).getAlpha() > 0, "custom editor paints off-screen");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> surroundEditor (surroundProcessor.createEditor());
        int visibleFormatComboCount = 0;
        int visibleSurroundToggleCount = 0;
        juce::ToggleButton* downmixButton = nullptr;
        juce::ToggleButton* dropLfeButton = nullptr;
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

            if (visible && dynamic_cast<juce::ComboBox*> (&component) != nullptr)
                ++visibleFormatComboCount;

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
        expect (visibleFormatComboCount == 1,
                "Surround Simple mode exposes Voice Format without Main Format");
        expect (visibleSurroundToggleCount == 2,
                "Surround header exposes Downmix and Drop LFE; VLC order moved to the meter menu");
        expect (downmixButton != nullptr && downmixButton->isEnabled(),
                "Surround Downmix control is clickable");
        expect (dropLfeButton != nullptr && ! dropLfeButton->isEnabled(),
                "Drop LFE remains disabled until Downmix is active");
    }

    constexpr float savedWeightFrequency = 4200.0f;
    setParameter (processor, smart_ducking::parameter_ids::weightFrequencyHz, savedWeightFrequency);
    setParameter (processor, smart_ducking::parameter_ids::widebandBypass, 1.0f);
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
                    smart_ducking::parameter_ids::widebandBypass),
                1.0f)
                && approximatelyEqual (
                    *restoredProcessor.getParameterState().getRawParameterValue (
                        smart_ducking::parameter_ids::monitorMode),
                    static_cast<float> (smart_ducking::MonitorMode::spectralDelta)),
            "stage bypass and Monitor Mode survive state round-trip");

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
