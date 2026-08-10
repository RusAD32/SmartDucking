#include "PluginProcessor.h"

#include "ParameterIds.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
constexpr int parameterVersion = 1;

juce::NormalisableRange<float> frequencyRange (float minimum, float maximum)
{
    return { minimum, maximum, 0.01f, 0.35f };
}

std::unique_ptr<juce::RangedAudioParameter> makeFloatParameter (
    const char* id,
    const char* name,
    juce::NormalisableRange<float> range,
    float defaultValue,
    const char* unit)
{
    auto attributes = juce::AudioParameterFloatAttributes {};

    if (unit != nullptr && unit[0] != '\0')
        attributes = attributes.withLabel (unit);

    return std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { id, parameterVersion }, name, range, defaultValue, attributes);
}

std::unique_ptr<juce::RangedAudioParameter> makeMatrixParameter (
    const char* id,
    const char* name,
    float defaultValue)
{
    return makeFloatParameter (id, name, { 0.0f, 1.0f, 0.01f }, defaultValue, "");
}
} // namespace

namespace smart_ducking
{
namespace
{
thread_local auto requestedPluginVariant = PluginVariant::stereo;
}

PluginVariant exchangeRequestedPluginVariant (PluginVariant replacement) noexcept
{
    const auto previous = requestedPluginVariant;
    requestedPluginVariant = replacement;
    return previous;
}

PluginVariant getRequestedPluginVariant() noexcept
{
    return requestedPluginVariant;
}
} // namespace smart_ducking

SmartDuckingAudioProcessor::BusesProperties
SmartDuckingAudioProcessor::makeBuses (smart_ducking::PluginVariant requestedVariant)
{
    const auto surround = juce::AudioChannelSet::create5point1();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto mainLayout = requestedVariant == smart_ducking::PluginVariant::surround
        ? surround
        : stereo;

    return BusesProperties()
        .withInput ("Main Input", mainLayout, true)
        .withInput ("Voice Sidechain", surround, true)
        .withOutput ("Output", mainLayout, true);
}

SmartDuckingAudioProcessor::SmartDuckingAudioProcessor (smart_ducking::PluginVariant newVariant)
    : AudioProcessor (makeBuses (newVariant)),
      variant (newVariant),
      parameters (*this, nullptr, "SmartDuckingState", createParameterLayout())
{
    using namespace smart_ducking::parameter_ids;

    duckDepthParameter = parameters.getRawParameterValue (duckDepthDb);
    crossoverParameter = parameters.getRawParameterValue (crossoverHz);
    attackParameter = parameters.getRawParameterValue (attackMs);
    holdParameter = parameters.getRawParameterValue (holdMs);
    releaseParameter = parameters.getRawParameterValue (releaseMs);
    lookaheadParameter = parameters.getRawParameterValue (lookaheadMs);
    activityThresholdParameter = parameters.getRawParameterValue (activityThresholdDb);
    spectralAmountParameter = parameters.getRawParameterValue (spectralAmountDb);
    spectralLowParameter = parameters.getRawParameterValue (spectralLowHz);
    spectralHighParameter = parameters.getRawParameterValue (spectralHighHz);
    weightFrequencyParameter = parameters.getRawParameterValue (weightFrequencyHz);
    weightGainParameter = parameters.getRawParameterValue (weightGainDb);
    weightQParameter = parameters.getRawParameterValue (weightQ);
    fftResolutionParameter = parameters.getRawParameterValue (fftResolution);
    processingModeParameter = parameters.getRawParameterValue (processingMode);
    voiceFormatParameter = parameters.getRawParameterValue (voiceFormat);
    vlcModeParameter = parameters.getRawParameterValue (vlcMode);
    downmixParameter = parameters.getRawParameterValue (downmix);
    dropLfeParameter = parameters.getRawParameterValue (dropLfe);
    matrixParameters = {
        parameters.getRawParameterValue (matrixLL), parameters.getRawParameterValue (matrixLC),
        parameters.getRawParameterValue (matrixLR), parameters.getRawParameterValue (matrixLLs),
        parameters.getRawParameterValue (matrixLRs), parameters.getRawParameterValue (matrixCL),
        parameters.getRawParameterValue (matrixCC), parameters.getRawParameterValue (matrixCR),
        parameters.getRawParameterValue (matrixCLs), parameters.getRawParameterValue (matrixCRs),
        parameters.getRawParameterValue (matrixRL), parameters.getRawParameterValue (matrixRC),
        parameters.getRawParameterValue (matrixRR), parameters.getRawParameterValue (matrixRLs),
        parameters.getRawParameterValue (matrixRRs), parameters.getRawParameterValue (matrixLsL),
        parameters.getRawParameterValue (matrixLsC), parameters.getRawParameterValue (matrixLsR),
        parameters.getRawParameterValue (matrixLsLs), parameters.getRawParameterValue (matrixLsRs),
        parameters.getRawParameterValue (matrixRsL), parameters.getRawParameterValue (matrixRsC),
        parameters.getRawParameterValue (matrixRsR), parameters.getRawParameterValue (matrixRsLs),
        parameters.getRawParameterValue (matrixRsRs)
    };

    jassert (duckDepthParameter != nullptr
             && crossoverParameter != nullptr
             && attackParameter != nullptr
             && holdParameter != nullptr
             && releaseParameter != nullptr
             && lookaheadParameter != nullptr
             && activityThresholdParameter != nullptr
             && spectralAmountParameter != nullptr
             && spectralLowParameter != nullptr
             && spectralHighParameter != nullptr
             && weightFrequencyParameter != nullptr
             && weightGainParameter != nullptr
             && weightQParameter != nullptr
             && fftResolutionParameter != nullptr
             && processingModeParameter != nullptr
             && voiceFormatParameter != nullptr
             && vlcModeParameter != nullptr
             && downmixParameter != nullptr
             && dropLfeParameter != nullptr);

    jassert (std::all_of (matrixParameters.begin(), matrixParameters.end(), [] (const auto* parameter)
    {
        return parameter != nullptr;
    }));

    parameters.addParameterListener (fftResolution, this);
    parameters.addParameterListener (processingMode, this);
    startTimerHz (20);
}

SmartDuckingAudioProcessor::~SmartDuckingAudioProcessor()
{
    using namespace smart_ducking::parameter_ids;
    stopTimer();
    parameters.removeParameterListener (fftResolution, this);
    parameters.removeParameterListener (processingMode, this);
}

const juce::String SmartDuckingAudioProcessor::getName() const
{
    return isSurroundVariant() ? "Smart Ducking (Surround)" : "Smart Ducking (Stereo)";
}

void SmartDuckingAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    processorPrepared.store (false, std::memory_order_release);
    currentSampleRate = juce::jmax (1.0, sampleRate);
    crossover.prepare (sampleRate, crossoverParameter->load());
    spectralLcrProcessor.prepare (sampleRate);
    const auto useNormalMode = processingModeParameter->load() >= 0.5f;
    const auto resolutionIndex = juce::jlimit (
        0, 2, juce::roundToInt (fftResolutionParameter->load()));
    currentEconomyMode.store (useNormalMode, std::memory_order_relaxed);
    currentResolutionIndex.store (resolutionIndex, std::memory_order_relaxed);
    spectralLcrProcessor.setEconomyMode (useNormalMode);
    spectralLcrProcessor.setResolutionIndex (resolutionIndex);

    const auto maximumDelay = spectralLcrProcessor.getMaximumFftSize();
    const juce::dsp::ProcessSpec delaySpec {
        sampleRate,
        static_cast<juce::uint32> (juce::jmax (1, maximumExpectedSamplesPerBlock)),
        6
    };
    lowBandDelay.setMaximumDelayInSamples (maximumDelay);
    lowBandDelay.prepare (delaySpec);
    const auto stftLatency = spectralLcrProcessor.getLatencySamples();
    lowBandDelay.setDelay (static_cast<float> (stftLatency));

    maximumLookaheadSamples = static_cast<int> (std::ceil (0.050 * currentSampleRate));
    const auto maximumLookaheadDelay = maximumLookaheadSamples + 2;
    mainLookaheadDelay.setMaximumDelayInSamples (maximumLookaheadDelay);
    mainLookaheadDelay.prepare (delaySpec);
    mainLookaheadDelay.setDelay (static_cast<float> (maximumLookaheadSamples));
    sidechainLookaheadDelay.setMaximumDelayInSamples (maximumLookaheadDelay);
    sidechainLookaheadDelay.prepare (delaySpec);
    const auto requestedLookaheadSamples = juce::jlimit (
        0.0f,
        static_cast<float> (maximumLookaheadSamples),
        static_cast<float> (0.001 * currentSampleRate * lookaheadParameter->load()));
    const auto initialSidechainDelay = static_cast<float> (maximumLookaheadSamples)
                                     - requestedLookaheadSamples;
    sidechainLookaheadDelay.setDelay (initialSidechainDelay);
    smoothedSidechainDelaySamples.reset (currentSampleRate, 0.020);
    smoothedSidechainDelaySamples.setCurrentAndTargetValue (initialSidechainDelay);

    const auto totalLatency = getTotalLatencySamples();
    setLatencySamples (totalLatency);
    configurationPending.store (false, std::memory_order_release);
    processorPrepared.store (true, std::memory_order_release);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    diagnosticLogger.recordPrepare (sampleRate,
                                    maximumExpectedSamplesPerBlock,
                                    resolutionIndex,
                                    totalLatency);
#endif
    mainInputPeakLeft.store (0.0f, std::memory_order_relaxed);
    mainInputPeakRight.store (0.0f, std::memory_order_relaxed);
    mainInputPeakCentre.store (0.0f, std::memory_order_relaxed);
    mainInputPeakLeftSurround.store (0.0f, std::memory_order_relaxed);
    mainInputPeakRightSurround.store (0.0f, std::memory_order_relaxed);
    outputPeakLeft.store (0.0f, std::memory_order_relaxed);
    outputPeakRight.store (0.0f, std::memory_order_relaxed);
    outputPeakCentre.store (0.0f, std::memory_order_relaxed);
    outputPeakLeftSurround.store (0.0f, std::memory_order_relaxed);
    outputPeakRightSurround.store (0.0f, std::memory_order_relaxed);
    sidechainPeakLeft.store (0.0f, std::memory_order_relaxed);
    sidechainPeakRight.store (0.0f, std::memory_order_relaxed);
    sidechainPeakCentre.store (0.0f, std::memory_order_relaxed);
    sidechainPeakLeftSurround.store (0.0f, std::memory_order_relaxed);
    sidechainPeakRightSurround.store (0.0f, std::memory_order_relaxed);
    gainReductionLeftDb.store (0.0f, std::memory_order_relaxed);
    gainReductionCentreDb.store (0.0f, std::memory_order_relaxed);
    gainReductionRightDb.store (0.0f, std::memory_order_relaxed);
    gainReductionLeftSurroundDb.store (0.0f, std::memory_order_relaxed);
    gainReductionRightSurroundDb.store (0.0f, std::memory_order_relaxed);
    spectralReductionLeftDb.store (0.0f, std::memory_order_relaxed);
    spectralReductionCentreDb.store (0.0f, std::memory_order_relaxed);
    spectralReductionRightDb.store (0.0f, std::memory_order_relaxed);
    spectralReductionLeftSurroundDb.store (0.0f, std::memory_order_relaxed);
    spectralReductionRightSurroundDb.store (0.0f, std::memory_order_relaxed);
    pceLeftPeak.store (0.0f, std::memory_order_relaxed);
    pceCentrePeak.store (0.0f, std::memory_order_relaxed);
    pceRightPeak.store (0.0f, std::memory_order_relaxed);
}

void SmartDuckingAudioProcessor::releaseResources()
{
    processorPrepared.store (false, std::memory_order_release);
}

void SmartDuckingAudioProcessor::parameterChanged (const juce::String&, float)
{
    configurationPending.store (true, std::memory_order_release);
}

void SmartDuckingAudioProcessor::updateTransportState()
{
    auto stateIsKnown = false;
    auto isPlaying = false;

    if (const auto* currentPlayHead = getPlayHead())
    {
        if (const auto position = currentPlayHead->getPosition())
        {
            stateIsKnown = true;
            isPlaying = position->getIsPlaying();
        }
    }

    transportStateKnown.store (stateIsKnown, std::memory_order_release);
    transportIsPlaying.store (isPlaying, std::memory_order_release);

}

void SmartDuckingAudioProcessor::timerCallback()
{
    constexpr auto quiescentTicksRequired = 4;
    const auto callbackCount = processBlockCounter.load (std::memory_order_acquire);

    if (callbackCount != lastTimerProcessBlockCounter)
    {
        lastTimerProcessBlockCounter = callbackCount;
        consecutiveQuiescentTimerTicks = 0;
    }
    else
    {
        consecutiveQuiescentTimerTicks = juce::jmin (
            quiescentTicksRequired, consecutiveQuiescentTimerTicks + 1);
    }

    if (! processorPrepared.load (std::memory_order_acquire)
        || ! configurationPending.load (std::memory_order_acquire))
        return;

    const auto transportStopped = transportStateKnown.load (std::memory_order_acquire)
                               && ! transportIsPlaying.load (std::memory_order_acquire);
    const auto audioCallbackIsQuiescent = consecutiveQuiescentTimerTicks
                                       >= quiescentTicksRequired;

    if (! transportStopped && ! audioCallbackIsQuiescent)
        return;

    suspendProcessing (true);

    {
        const juce::ScopedLock lock (getCallbackLock());
        applyRequestedDspConfiguration();
    }

    setLatencySamples (getTotalLatencySamples());
    suspendProcessing (false);
}

void SmartDuckingAudioProcessor::applyRequestedDspConfiguration()
{
    const auto requestedNormalMode = processingModeParameter->load() >= 0.5f;
    const auto requestedResolution = juce::jlimit (
        0, 2, juce::roundToInt (fftResolutionParameter->load()));
    const auto previousNormalMode = currentEconomyMode.load (std::memory_order_relaxed);
    const auto previousResolution = currentResolutionIndex.load (std::memory_order_relaxed);
    const auto modeChanged = requestedNormalMode != previousNormalMode;
    const auto resolutionChanged = requestedResolution != previousResolution;

    if (modeChanged)
        spectralLcrProcessor.setEconomyMode (requestedNormalMode);

    if (resolutionChanged)
        spectralLcrProcessor.setResolutionIndex (requestedResolution);

    if (modeChanged || resolutionChanged)
    {
        crossover.reset();
        lowBandDelay.reset();
        lowBandDelay.setDelay (static_cast<float> (spectralLcrProcessor.getLatencySamples()));
        currentEconomyMode.store (requestedNormalMode, std::memory_order_relaxed);
        currentResolutionIndex.store (requestedResolution, std::memory_order_relaxed);
    }

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    if (resolutionChanged)
        diagnosticLogger.recordResolutionChange (requestedResolution,
                                                   getTotalLatencySamples());
#endif

    const auto latestNormalMode = processingModeParameter->load() >= 0.5f;
    const auto latestResolution = juce::jlimit (
        0, 2, juce::roundToInt (fftResolutionParameter->load()));
    configurationPending.store (
        latestNormalMode != currentEconomyMode.load (std::memory_order_relaxed)
            || latestResolution != currentResolutionIndex.load (std::memory_order_relaxed),
        std::memory_order_release);
}

int SmartDuckingAudioProcessor::getTotalLatencySamples() const noexcept
{
    return spectralLcrProcessor.getLatencySamples() + maximumLookaheadSamples;
}

void SmartDuckingAudioProcessor::removeLegacyState (juce::ValueTree& state)
{
    constexpr auto legacySidechainPreGain = "sidechainPreGainDb";
    state.removeProperty (legacySidechainPreGain, nullptr);

    for (int childIndex = state.getNumChildren(); --childIndex >= 0;)
    {
        const auto child = state.getChild (childIndex);

        if (child.hasType ("PARAM")
            && child.getProperty ("id").toString() == legacySidechainPreGain)
            state.removeChild (childIndex, nullptr);
    }
}

bool SmartDuckingAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto surround = juce::AudioChannelSet::create5point1();
    const auto mainLayout = isSurroundVariant() ? surround : juce::AudioChannelSet::stereo();

    if (layouts.getMainInputChannelSet() != mainLayout
        || layouts.getMainOutputChannelSet() != mainLayout)
        return false;

    if (layouts.inputBuses.size() < 2)
        return false;

    return layouts.getChannelSet (true, 1) == surround;
}

void SmartDuckingAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto diagnosticProcessBlockStart = juce::Time::getHighResolutionTicks();
#endif
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear();
    processBlockCounter.fetch_add (1, std::memory_order_release);

    auto mainInput = getBusBuffer (buffer, true, 0);
    auto output = getBusBuffer (buffer, false, 0);
    const auto numSamples = buffer.getNumSamples();
    updateTransportState();
    const auto sidechainEnabled = getBusCount (true) >= 2 && getBus (true, 1)->isEnabled();
    auto sidechain = sidechainEnabled ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float> {};
    const auto mainIsStereo = ! isSurroundVariant();
    const auto voiceIsStereo = voiceFormatParameter->load() < 0.5f;
    const auto useVlcOrder = vlcModeParameter->load() >= 0.5f;
    const auto forceDownmix = downmixParameter->load() >= 0.5f;
    const auto dropLfe = dropLfeParameter->load() >= 0.5f;
    const auto visualisationActive = spectralLcrProcessor.isVisualisationEnabled();
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    constexpr auto diagnosticsActive = true;
#else
    constexpr auto diagnosticsActive = false;
#endif
    const auto collectVoicePeaks = visualisationActive || diagnosticsActive;

    std::array<const float*, 6> mainRead {};
    std::array<const float*, 6> voiceRead {};
    std::array<float*, 6> outputWrite {};

    for (int channel = 0; channel < 6; ++channel)
    {
        if (channel < mainInput.getNumChannels())
            mainRead[static_cast<size_t> (channel)] = mainInput.getReadPointer (channel);

        if (channel < sidechain.getNumChannels())
            voiceRead[static_cast<size_t> (channel)] = sidechain.getReadPointer (channel);

        if (channel < output.getNumChannels())
            outputWrite[static_cast<size_t> (channel)] = output.getWritePointer (channel);
    }

    crossover.setCutoffHz (crossoverParameter->load());
    const auto useNormalMode = currentEconomyMode.load (std::memory_order_relaxed);
    const auto requestedLookaheadSamples = juce::jlimit (
        0.0f,
        static_cast<float> (maximumLookaheadSamples),
        static_cast<float> (0.001 * currentSampleRate * lookaheadParameter->load()));
    smoothedSidechainDelaySamples.setTargetValue (
        static_cast<float> (maximumLookaheadSamples) - requestedLookaheadSamples);

    auto spectralParameters = smart_ducking::StereoSpectralLcrProcessor::Parameters {};
    spectralParameters.amountDb = spectralAmountParameter->load();
    spectralParameters.lowFrequencyHz = spectralLowParameter->load();
    spectralParameters.highFrequencyHz = spectralHighParameter->load();
    spectralParameters.weightFrequencyHz = weightFrequencyParameter->load();
    spectralParameters.weightGainDb = weightGainParameter->load();
    spectralParameters.weightQ = weightQParameter->load();
    spectralParameters.activityThresholdDb = activityThresholdParameter->load();
    spectralParameters.attackMs = attackParameter->load();
    spectralParameters.holdMs = holdParameter->load();
    spectralParameters.releaseMs = releaseParameter->load();
    spectralParameters.duckDepthDb = duckDepthParameter->load();
    spectralParameters.crossoverHz = crossoverParameter->load();

    for (size_t index = 0; index < spectralParameters.duckMatrix.coefficients.size(); ++index)
        spectralParameters.duckMatrix.coefficients[index] = matrixParameters[index]->load();

    spectralLcrProcessor.setParameters (spectralParameters);

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto activityThreshold = juce::Decibels::decibelsToGain (
        activityThresholdParameter->load());
#endif
    std::array<float, 5> maximumReductionDb {};
    std::array<float, 5> maximumSpectralDb {};
    std::array<float, 5> maximumMainInputPeaks {};
    std::array<float, 5> maximumVoiceInputPeaks {};
    std::array<float, 5> maximumOutputPeaks {};
    const auto updateReductionPeaks = [&]
    {
        for (int channel = 0; channel < 5; ++channel)
        {
            const auto index = static_cast<size_t> (channel);
            maximumReductionDb[index] = juce::jmax (
                maximumReductionDb[index],
                spectralLcrProcessor.getLastWidebandReductionDb (channel));
            maximumSpectralDb[index] = juce::jmax (
                maximumSpectralDb[index],
                spectralLcrProcessor.getLastMaximumReductionDb (channel));
        }
    };
    auto observedOutputFrame = spectralLcrProcessor.getProcessedOutputFrameCount();

    if (visualisationActive && numSamples > 0)
        updateReductionPeaks();
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    bool outputIsFinite = true;
#endif

    const auto readSample = [] (const std::array<const float*, 6>& channels,
                                int channel,
                                int sample) noexcept
    {
        const auto* data = channels[static_cast<size_t> (channel)];
        return data != nullptr ? data[sample] : 0.0f;
    };

    for (int sample = 0; sample < numSamples; ++sample)
    {
        std::array<float, 6> rawMain {};
        std::array<float, 6> rawVoice {};
        std::array<float, 6> delayedMain {};
        std::array<float, 6> delayedVoice {};
        sidechainLookaheadDelay.setDelay (smoothedSidechainDelaySamples.getNextValue());

        for (int channel = 0; channel < 6; ++channel)
        {
            const auto index = static_cast<size_t> (channel);
            rawMain[index] = readSample (mainRead, channel, sample);
            rawVoice[index] = readSample (voiceRead, channel, sample);
            mainLookaheadDelay.pushSample (channel, rawMain[index]);
            delayedMain[index] = mainLookaheadDelay.popSample (channel);
            sidechainLookaheadDelay.pushSample (channel, rawVoice[index]);
            delayedVoice[index] = sidechainLookaheadDelay.popSample (channel);
        }

        smart_ducking::StereoSpectralLcrProcessor::LcrSample mainCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample voiceCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample rawMainCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample rawVoiceCanonical;
        float mainLfe = 0.0f;

        const auto mapMain = [&] (const std::array<float, 6>& source,
                                  smart_ducking::StereoSpectralLcrProcessor::LcrSample& destination,
                                  float& lfe) noexcept
        {
            destination.left = source[0];
            destination.right = source[1];

            if (mainIsStereo)
                return;

            if (useVlcOrder)
            {
                destination.leftSurround = source[2];
                destination.rightSurround = source[3];
                destination.centre = source[4];
                lfe = source[5];
            }
            else
            {
                destination.centre = source[2];
                lfe = source[3];
                destination.leftSurround = source[4];
                destination.rightSurround = source[5];
            }
        };

        const auto mapVoice = [&] (const std::array<float, 6>& source,
                                   smart_ducking::StereoSpectralLcrProcessor::LcrSample& destination) noexcept
        {
            destination.left = source[0];
            destination.right = source[1];

            if (! voiceIsStereo)
            {
                destination.centre = source[2];
                destination.leftSurround = source[4];
                destination.rightSurround = source[5];
            }
        };

        float ignoredRawLfe = 0.0f;
        mapMain (delayedMain, mainCanonical, mainLfe);
        mapMain (rawMain, rawMainCanonical, ignoredRawLfe);
        mapVoice (delayedVoice, voiceCanonical);
        mapVoice (rawVoice, rawVoiceCanonical);

        const auto updatePeak = [] (float& maximum, float value) noexcept
        {
            maximum = juce::jmax (maximum, std::abs (value));
        };
        if (visualisationActive)
        {
            updatePeak (maximumMainInputPeaks[0], rawMainCanonical.left);
            updatePeak (maximumMainInputPeaks[1], rawMainCanonical.centre);
            updatePeak (maximumMainInputPeaks[2], rawMainCanonical.right);
            updatePeak (maximumMainInputPeaks[3], rawMainCanonical.leftSurround);
            updatePeak (maximumMainInputPeaks[4], rawMainCanonical.rightSurround);
        }

        if (collectVoicePeaks)
        {
            updatePeak (maximumVoiceInputPeaks[0], rawVoiceCanonical.left);
            updatePeak (maximumVoiceInputPeaks[1], rawVoiceCanonical.centre);
            updatePeak (maximumVoiceInputPeaks[2], rawVoiceCanonical.right);
            updatePeak (maximumVoiceInputPeaks[3], rawVoiceCanonical.leftSurround);
            updatePeak (maximumVoiceInputPeaks[4], rawVoiceCanonical.rightSurround);
        }

        smart_ducking::StereoSpectralLcrProcessor::LcrSample highInput;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample delayedLow;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample lowBandForVisualisation;

        const auto processCrossoverChannel = [&] (int channel, float input)
        {
            const auto bands = crossover.processSample (channel, input);
            lowBandDelay.pushSample (channel, bands.low);
            const auto low = lowBandDelay.popSample (channel);

            switch (channel)
            {
                case 0: highInput.left = bands.high; delayedLow.left = low;
                        lowBandForVisualisation.left = bands.low; break;
                case 1: highInput.centre = bands.high; delayedLow.centre = low;
                        lowBandForVisualisation.centre = bands.low; break;
                case 2: highInput.right = bands.high; delayedLow.right = low;
                        lowBandForVisualisation.right = bands.low; break;
                case 3: highInput.leftSurround = bands.high; delayedLow.leftSurround = low;
                        lowBandForVisualisation.leftSurround = bands.low; break;
                case 4: highInput.rightSurround = bands.high; delayedLow.rightSurround = low;
                        lowBandForVisualisation.rightSurround = bands.low; break;
                default: break;
            }
        };

        if (useNormalMode)
        {
            highInput = mainCanonical;
        }
        else if (mainIsStereo)
        {
            crossover.beginSample();
            processCrossoverChannel (0, mainCanonical.left);
            processCrossoverChannel (2, mainCanonical.right);
        }
        else
        {
            crossover.beginSample();
            for (int channel = 0; channel < 5; ++channel)
                processCrossoverChannel (channel, mainCanonical.get (channel));
        }

        auto delayedLfe = 0.0f;

        if (! mainIsStereo)
        {
            lowBandDelay.pushSample (5, mainLfe);
            delayedLfe = lowBandDelay.popSample (5);
        }
        const auto processedHigh = spectralLcrProcessor.processSample (
            highInput,
            voiceCanonical,
            mainIsStereo,
            voiceIsStereo,
            lowBandForVisualisation);

        const auto currentOutputFrame = spectralLcrProcessor.getProcessedOutputFrameCount();

        if (visualisationActive && currentOutputFrame != observedOutputFrame)
        {
            observedOutputFrame = currentOutputFrame;
            updateReductionPeaks();
        }

        smart_ducking::StereoSpectralLcrProcessor::LcrSample fullBand;

        if (mainIsStereo)
        {
            fullBand.left = (useNormalMode ? 0.0f : delayedLow.left) + processedHigh.left
                          + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                * processedHigh.centre;
            fullBand.right = (useNormalMode ? 0.0f : delayedLow.right) + processedHigh.right
                           + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                 * processedHigh.centre;
        }
        else
        {
            const auto lowMix = useNormalMode ? 0.0f : 1.0f;
            fullBand.left = lowMix * delayedLow.left + processedHigh.left;
            fullBand.centre = lowMix * delayedLow.centre + processedHigh.centre;
            fullBand.right = lowMix * delayedLow.right + processedHigh.right;
            fullBand.leftSurround = lowMix * delayedLow.leftSurround + processedHigh.leftSurround;
            fullBand.rightSurround = lowMix * delayedLow.rightSurround + processedHigh.rightSurround;
        }

        std::array<float, 6> outputValues {};

        if (mainIsStereo || forceDownmix)
        {
            const auto lfeContribution = (! mainIsStereo && ! dropLfe) ? 0.5f * delayedLfe : 0.0f;
            outputValues[0] = fullBand.left
                            + (mainIsStereo ? 0.0f
                                            : smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                                  * (fullBand.centre + fullBand.leftSurround))
                            + lfeContribution;
            outputValues[1] = fullBand.right
                            + (mainIsStereo ? 0.0f
                                            : smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                                  * (fullBand.centre + fullBand.rightSurround))
                            + lfeContribution;
        }
        else
        {
            outputValues = { fullBand.left,
                             fullBand.right,
                             fullBand.centre,
                             delayedLfe,
                             fullBand.leftSurround,
                             fullBand.rightSurround };
        }

        if (visualisationActive)
        {
            maximumOutputPeaks[0] = juce::jmax (maximumOutputPeaks[0],
                                                std::abs (outputValues[0]));
            maximumOutputPeaks[2] = juce::jmax (maximumOutputPeaks[2],
                                                std::abs (outputValues[1]));

            if (! mainIsStereo && ! forceDownmix)
            {
                maximumOutputPeaks[1] = juce::jmax (maximumOutputPeaks[1],
                                                    std::abs (outputValues[2]));
                maximumOutputPeaks[3] = juce::jmax (maximumOutputPeaks[3],
                                                    std::abs (outputValues[4]));
                maximumOutputPeaks[4] = juce::jmax (maximumOutputPeaks[4],
                                                    std::abs (outputValues[5]));
            }
        }

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
        {
            const auto finite = std::isfinite (outputValues[static_cast<size_t> (channel)]);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            outputIsFinite = outputIsFinite && finite;
#endif

            if (auto* destination = outputWrite[static_cast<size_t> (channel)])
                destination[sample] = finite ? outputValues[static_cast<size_t> (channel)] : 0.0f;
        }
    }

    if (visualisationActive)
    {
        gainReductionLeftDb.store (maximumReductionDb[0], std::memory_order_relaxed);
        gainReductionCentreDb.store (maximumReductionDb[1], std::memory_order_relaxed);
        gainReductionRightDb.store (maximumReductionDb[2], std::memory_order_relaxed);
        gainReductionLeftSurroundDb.store (maximumReductionDb[3], std::memory_order_relaxed);
        gainReductionRightSurroundDb.store (maximumReductionDb[4], std::memory_order_relaxed);
        spectralReductionLeftDb.store (maximumSpectralDb[0], std::memory_order_relaxed);
        spectralReductionCentreDb.store (maximumSpectralDb[1], std::memory_order_relaxed);
        spectralReductionRightDb.store (maximumSpectralDb[2], std::memory_order_relaxed);
        spectralReductionLeftSurroundDb.store (maximumSpectralDb[3], std::memory_order_relaxed);
        spectralReductionRightSurroundDb.store (maximumSpectralDb[4], std::memory_order_relaxed);
        mainInputPeakLeft.store (maximumMainInputPeaks[0], std::memory_order_relaxed);
        mainInputPeakCentre.store (maximumMainInputPeaks[1], std::memory_order_relaxed);
        mainInputPeakRight.store (maximumMainInputPeaks[2], std::memory_order_relaxed);
        mainInputPeakLeftSurround.store (maximumMainInputPeaks[3], std::memory_order_relaxed);
        mainInputPeakRightSurround.store (maximumMainInputPeaks[4], std::memory_order_relaxed);
        sidechainPeakLeft.store (maximumVoiceInputPeaks[0], std::memory_order_relaxed);
        sidechainPeakCentre.store (maximumVoiceInputPeaks[1], std::memory_order_relaxed);
        sidechainPeakRight.store (maximumVoiceInputPeaks[2], std::memory_order_relaxed);
        sidechainPeakLeftSurround.store (maximumVoiceInputPeaks[3], std::memory_order_relaxed);
        sidechainPeakRightSurround.store (maximumVoiceInputPeaks[4], std::memory_order_relaxed);
        outputPeakLeft.store (maximumOutputPeaks[0], std::memory_order_relaxed);
        outputPeakCentre.store (maximumOutputPeaks[1], std::memory_order_relaxed);
        outputPeakRight.store (maximumOutputPeaks[2], std::memory_order_relaxed);
        outputPeakLeftSurround.store (maximumOutputPeaks[3], std::memory_order_relaxed);
        outputPeakRightSurround.store (maximumOutputPeaks[4], std::memory_order_relaxed);
        const auto lcrPeaks = spectralLcrProcessor.getLastLcrPeaks();
        pceLeftPeak.store (lcrPeaks.left, std::memory_order_relaxed);
        pceCentrePeak.store (lcrPeaks.centre, std::memory_order_relaxed);
        pceRightPeak.store (lcrPeaks.right, std::memory_order_relaxed);
    }
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto maximumVoicePeak = *std::max_element (maximumVoiceInputPeaks.begin(),
                                                      maximumVoiceInputPeaks.end());

    diagnosticLogger.recordAudioBlock (numSamples,
                                       maximumVoicePeak >= activityThreshold,
                                       outputIsFinite,
                                       static_cast<int> (spectralLcrProcessor.getSidechainMode()),
                                       spectralLcrProcessor.getProcessedSidechainFftCount(),
                                       spectralLcrProcessor.getProcessedSidechainPceFrameCount(),
                                       spectralLcrProcessor.isVisualisationEnabled(),
                                       static_cast<std::uint64_t> (
                                           juce::Time::getHighResolutionTicks()
                                           - diagnosticProcessBlockStart),
                                       spectralLcrProcessor.getDiagnosticSpectralFrameCount(),
                                       spectralLcrProcessor.getDiagnosticSpectralFrameTicks(),
                                       spectralLcrProcessor.getDiagnosticDryVisualisationCount(),
                                       spectralLcrProcessor.getDiagnosticDryVisualisationTicks(),
                                       spectralLcrProcessor.getDiagnosticActiveVisualisationCount(),
                                       spectralLcrProcessor.getDiagnosticActiveVisualisationTicks());
    diagnosticLogger.recordSpectralStages (
        spectralLcrProcessor.getDiagnosticFrameInputTicks(),
        spectralLcrProcessor.getDiagnosticClassificationTicks(),
        spectralLcrProcessor.getDiagnosticMainAnalysisTicks(),
        spectralLcrProcessor.getDiagnosticSidechainAnalysisTicks(),
        spectralLcrProcessor.getDiagnosticControlTicks(),
        spectralLcrProcessor.getDiagnosticMaskTicks(),
        spectralLcrProcessor.getDiagnosticWideGainTicks(),
        spectralLcrProcessor.getDiagnosticInverseTicks(),
        spectralLcrProcessor.getDiagnosticOlaTicks());
#endif
}

juce::AudioProcessorEditor* SmartDuckingAudioProcessor::createEditor()
{
    return new SmartDuckingAudioProcessorEditor (*this);
}

void SmartDuckingAudioProcessor::getStateInformation (juce::MemoryBlock& destinationData)
{
    if (const auto stateXml = parameters.copyState().createXml())
        copyXmlToBinary (*stateXml, destinationData);
}

void SmartDuckingAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto stateXml = getXmlFromBinary (data, sizeInBytes);

    if (stateXml != nullptr && stateXml->hasTagName (parameters.state.getType()))
    {
        auto restoredState = juce::ValueTree::fromXml (*stateXml);
        removeLegacyState (restoredState);
        parameters.replaceState (restoredState);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
SmartDuckingAudioProcessor::createParameterLayout()
{
    using namespace smart_ducking::parameter_ids;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (makeFloatParameter (crossoverHz, "Crossover", frequencyRange (50.0f, 180.0f), 92.0f, "Hz"));
    layout.add (makeFloatParameter (duckDepthDb, "Duck Depth", { 0.0f, 18.0f, 0.01f }, 15.0f, "dB"));
    layout.add (makeFloatParameter (attackMs, "Attack", { 0.0f, 200.0f, 0.1f }, 50.0f, "ms"));
    layout.add (makeFloatParameter (holdMs, "Hold", { 0.0f, 500.0f, 0.1f }, 150.0f, "ms"));
    layout.add (makeFloatParameter (releaseMs, "Release", { 20.0f, 1000.0f, 0.1f, 0.45f }, 200.0f, "ms"));
    layout.add (makeFloatParameter (lookaheadMs, "Lookahead", { 0.0f, 50.0f, 0.1f }, 10.0f, "ms"));
    layout.add (makeFloatParameter (activityThresholdDb, "Voice Full", { -60.0f, 0.0f, 0.1f }, -30.0f, "dBFS"));
    layout.add (makeFloatParameter (spectralAmountDb, "Spectral Amount", { 0.0f, 18.0f, 0.01f }, 10.0f, "dB"));
    layout.add (makeFloatParameter (spectralLowHz, "Spectral Low", frequencyRange (100.0f, 2000.0f), 150.0f, "Hz"));
    layout.add (makeFloatParameter (spectralHighHz, "Spectral High", frequencyRange (4000.0f, 20000.0f), 20000.0f, "Hz"));
    layout.add (makeFloatParameter (weightFrequencyHz, "Weight Frequency", frequencyRange (500.0f, 8000.0f), 3000.0f, "Hz"));
    layout.add (makeFloatParameter (weightGainDb, "Weight Gain", { 0.0f, 12.0f, 0.01f }, 6.0f, "dB"));
    layout.add (makeFloatParameter (weightQ, "Weight Q", { 0.3f, 3.0f, 0.001f, 0.5f }, 0.8f, ""));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { fftResolution, parameterVersion },
        "FFT Resolution",
        juce::StringArray { "x1 Fast", "x2 Normal", "x4 Fine" },
        1));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { processingMode, parameterVersion },
        "Processing Mode",
        juce::StringArray { "QUALITY", "NORMAL" },
        1));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { voiceFormat, parameterVersion },
        "Voice Format",
        juce::StringArray { "Stereo", "Surround" },
        0));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { vlcMode, parameterVersion }, "VLC Channel Order", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { downmix, parameterVersion }, "Downmix", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { dropLfe, parameterVersion }, "Drop LFE", false));

    layout.add (makeMatrixParameter (matrixLL, "Matrix L to L", 1.00f));
    layout.add (makeMatrixParameter (matrixLC, "Matrix L to C", 0.25f));
    layout.add (makeMatrixParameter (matrixLR, "Matrix L to R", 0.00f));
    layout.add (makeMatrixParameter (matrixLLs, "Matrix L to Ls", 0.35f));
    layout.add (makeMatrixParameter (matrixLRs, "Matrix L to Rs", 0.00f));
    layout.add (makeMatrixParameter (matrixCL, "Matrix C to L", 0.10f));
    layout.add (makeMatrixParameter (matrixCC, "Matrix C to C", 1.00f));
    layout.add (makeMatrixParameter (matrixCR, "Matrix C to R", 0.10f));
    layout.add (makeMatrixParameter (matrixCLs, "Matrix C to Ls", 0.05f));
    layout.add (makeMatrixParameter (matrixCRs, "Matrix C to Rs", 0.05f));
    layout.add (makeMatrixParameter (matrixRL, "Matrix R to L", 0.00f));
    layout.add (makeMatrixParameter (matrixRC, "Matrix R to C", 0.25f));
    layout.add (makeMatrixParameter (matrixRR, "Matrix R to R", 1.00f));
    layout.add (makeMatrixParameter (matrixRLs, "Matrix R to Ls", 0.00f));
    layout.add (makeMatrixParameter (matrixRRs, "Matrix R to Rs", 0.35f));
    layout.add (makeMatrixParameter (matrixLsL, "Matrix Ls to L", 0.35f));
    layout.add (makeMatrixParameter (matrixLsC, "Matrix Ls to C", 0.00f));
    layout.add (makeMatrixParameter (matrixLsR, "Matrix Ls to R", 0.00f));
    layout.add (makeMatrixParameter (matrixLsLs, "Matrix Ls to Ls", 1.00f));
    layout.add (makeMatrixParameter (matrixLsRs, "Matrix Ls to Rs", 0.05f));
    layout.add (makeMatrixParameter (matrixRsL, "Matrix Rs to L", 0.00f));
    layout.add (makeMatrixParameter (matrixRsC, "Matrix Rs to C", 0.00f));
    layout.add (makeMatrixParameter (matrixRsR, "Matrix Rs to R", 0.35f));
    layout.add (makeMatrixParameter (matrixRsLs, "Matrix Rs to Ls", 0.05f));
    layout.add (makeMatrixParameter (matrixRsRs, "Matrix Rs to Rs", 1.00f));

    return layout;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SmartDuckingAudioProcessor (smart_ducking::getRequestedPluginVariant());
}
