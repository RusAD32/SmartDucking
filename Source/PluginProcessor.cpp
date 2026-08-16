#include "PluginProcessor.h"

#include "FactoryPresets.h"
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

std::array<float, 6> makeOutputSample (
    const std::array<float, 6>& canonical,
    bool mainIsStereo,
    const smart_ducking::ChannelOrderMix& outputOrder,
    float downmixMix,
    float dropLfeMix) noexcept
{
    if (mainIsStereo)
        return { canonical[0], canonical[1], 0.0f, 0.0f, 0.0f, 0.0f };

    const auto surround = smart_ducking::applySurroundOutputOrder (canonical, outputOrder);

    if (downmixMix <= 0.0f)
        return surround;

    std::array<float, 6> stereo {};
    const auto lfeContribution = 0.5f * canonical[3] * (1.0f - dropLfeMix);
    stereo[0] = canonical[0]
              + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                    * (canonical[2] + canonical[4])
              + lfeContribution;
    stereo[1] = canonical[1]
              + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                    * (canonical[2] + canonical[5])
              + lfeContribution;

    if (downmixMix >= 1.0f)
        return stereo;

    std::array<float, 6> result {};
    for (size_t channel = 0; channel < result.size(); ++channel)
        result[channel] = surround[channel]
                        + downmixMix * (stereo[channel] - surround[channel]);

    return result;
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
    crossoverSlopeParameter = parameters.getRawParameterValue (crossoverSlope);
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
    mainChannelOrderParameter = parameters.getRawParameterValue (mainChannelOrder);
    voiceChannelOrderParameter = parameters.getRawParameterValue (voiceChannelOrder);
    outputChannelOrderParameter = parameters.getRawParameterValue (outputChannelOrder);
    downmixParameter = parameters.getRawParameterValue (downmix);
    dropLfeParameter = parameters.getRawParameterValue (dropLfe);
    globalBypassParameter = parameters.getRawParameterValue (globalBypass);
    globalBypassHostParameter = parameters.getParameter (globalBypass);
    crossoverBypassParameter = parameters.getRawParameterValue (crossoverBypass);
    widebandBypassParameter = parameters.getRawParameterValue (widebandBypass);
    spectralBypassParameter = parameters.getRawParameterValue (spectralBypass);
    monitorModeParameter = parameters.getRawParameterValue (monitorMode);
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
             && crossoverSlopeParameter != nullptr
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
             && mainChannelOrderParameter != nullptr
             && voiceChannelOrderParameter != nullptr
             && outputChannelOrderParameter != nullptr
             && downmixParameter != nullptr
             && dropLfeParameter != nullptr
             && globalBypassParameter != nullptr
             && globalBypassHostParameter != nullptr
             && crossoverBypassParameter != nullptr
             && widebandBypassParameter != nullptr
             && spectralBypassParameter != nullptr
             && monitorModeParameter != nullptr);

    jassert (std::all_of (matrixParameters.begin(), matrixParameters.end(), [] (const auto* parameter)
    {
        return parameter != nullptr;
    }));

    parameters.addParameterListener (fftResolution, this);
    parameters.addParameterListener (processingMode, this);
    initialiseComparisonSlots();
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

int SmartDuckingAudioProcessor::getNumPrograms()
{
    return static_cast<int> (smart_ducking::factoryPresets.size());
}

int SmartDuckingAudioProcessor::getCurrentProgram()
{
    return currentFactoryProgram.load (std::memory_order_relaxed);
}

void SmartDuckingAudioProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    applyPresetState (createFactoryPresetState (index));
    currentFactoryProgram.store (index, std::memory_order_relaxed);
    factoryProgramChangeCount.fetch_add (1, std::memory_order_relaxed);
}

const juce::String SmartDuckingAudioProcessor::getProgramName (int index)
{
    return index >= 0 && index < getNumPrograms()
        ? smart_ducking::factoryPresets[static_cast<size_t> (index)].name
        : juce::String {};
}

juce::ValueTree SmartDuckingAudioProcessor::createCurrentPresetState() const
{
    juce::ValueTree presetState ("SmartDuckingPreset");

    for (auto* parameter : getParameters())
    {
        const auto* parameterWithId =
            dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter);
        const auto* rangedParameter = dynamic_cast<const juce::RangedAudioParameter*> (parameter);
        if (parameterWithId == nullptr || rangedParameter == nullptr
            || isGlobalBypassParameter (parameter)
            || parameterWithId->paramID == smart_ducking::parameter_ids::monitorMode)
            continue;

        presetState.setProperty (parameterWithId->paramID,
                                 rangedParameter->convertFrom0to1 (parameter->getValue()),
                                 nullptr);
    }

    return presetState;
}

juce::ValueTree SmartDuckingAudioProcessor::createFactoryPresetState (int index) const
{
    index = juce::jlimit (
        0, static_cast<int> (smart_ducking::factoryPresets.size()) - 1, index);
    juce::ValueTree presetState ("SmartDuckingPreset");

    for (auto* parameter : getParameters())
    {
        const auto* parameterWithId =
            dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter);
        const auto* rangedParameter = dynamic_cast<const juce::RangedAudioParameter*> (parameter);
        if (parameterWithId == nullptr || rangedParameter == nullptr
            || isGlobalBypassParameter (parameter)
            || parameterWithId->paramID == smart_ducking::parameter_ids::monitorMode)
            continue;

        presetState.setProperty (parameterWithId->paramID,
                                 rangedParameter->convertFrom0to1 (parameter->getDefaultValue()),
                                 nullptr);
    }

    for (const auto& value : smart_ducking::factoryPresets[static_cast<size_t> (index)].values)
        presetState.setProperty (value.parameterId, value.rawValue, nullptr);

    return presetState;
}

bool SmartDuckingAudioProcessor::applyPresetState (const juce::ValueTree& presetState)
{
    if (! presetState.isValid() || ! presetState.hasType ("SmartDuckingPreset"))
        return false;

    for (auto* parameter : getParameters())
    {
        auto* parameterWithId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter);
        auto* rangedParameter = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        if (parameterWithId == nullptr || rangedParameter == nullptr
            || isGlobalBypassParameter (parameter))
            continue;

        auto normalisedValue = parameter->getDefaultValue();
        if (parameterWithId->paramID != smart_ducking::parameter_ids::monitorMode
            && presetState.hasProperty (parameterWithId->paramID))
        {
            const auto rawValue = static_cast<float> (
                presetState.getProperty (parameterWithId->paramID));
            normalisedValue = rangedParameter->convertTo0to1 (rawValue);
        }

        parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalisedValue));
    }

    captureComparisonSlot (currentComparisonSlot.load (std::memory_order_relaxed));
    return true;
}

bool SmartDuckingAudioProcessor::doesPresetStateMatch (
    const juce::ValueTree& presetState) const
{
    if (! presetState.isValid() || ! presetState.hasType ("SmartDuckingPreset"))
        return false;

    for (auto* parameter : getParameters())
    {
        const auto* parameterWithId =
            dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter);
        const auto* rangedParameter = dynamic_cast<const juce::RangedAudioParameter*> (parameter);
        if (parameterWithId == nullptr || rangedParameter == nullptr
            || isGlobalBypassParameter (parameter)
            || parameterWithId->paramID == smart_ducking::parameter_ids::monitorMode)
            continue;

        auto expectedValue = parameter->getDefaultValue();
        if (presetState.hasProperty (parameterWithId->paramID))
            expectedValue = rangedParameter->convertTo0to1 (static_cast<float> (
                presetState.getProperty (parameterWithId->paramID)));

        if (std::abs (parameter->getValue() - expectedValue) > 1.0e-5f)
            return false;
    }

    return true;
}

bool SmartDuckingAudioProcessor::isGlobalBypassParameter (
    const juce::AudioProcessorParameter* parameter) const noexcept
{
    return parameter != nullptr && parameter == globalBypassHostParameter;
}

void SmartDuckingAudioProcessor::initialiseComparisonSlots()
{
    const auto parameterCount = getParameters().size();
    for (auto& slot : comparisonSlots)
        slot.resize (static_cast<size_t> (parameterCount));

    captureComparisonSlot (0);
    captureComparisonSlot (1);
}

void SmartDuckingAudioProcessor::captureComparisonSlot (int slot)
{
    if (slot < 0 || slot >= static_cast<int> (comparisonSlots.size()))
        return;

    auto& snapshot = comparisonSlots[static_cast<size_t> (slot)];
    const auto& processorParameters = getParameters();

    if (snapshot.size() != static_cast<size_t> (processorParameters.size()))
        snapshot.resize (static_cast<size_t> (processorParameters.size()));

    for (int index = 0; index < processorParameters.size(); ++index)
        snapshot[static_cast<size_t> (index)] = processorParameters[index] != nullptr
            ? processorParameters[index]->getValue() : 0.0f;
}

void SmartDuckingAudioProcessor::restoreComparisonSlot (int slot)
{
    if (slot < 0 || slot >= static_cast<int> (comparisonSlots.size()))
        return;

    const auto& snapshot = comparisonSlots[static_cast<size_t> (slot)];
    const auto& processorParameters = getParameters();
    const auto count = juce::jmin (static_cast<int> (snapshot.size()),
                                   processorParameters.size());

    for (int index = 0; index < count; ++index)
        if (processorParameters[index] != nullptr
            && ! isGlobalBypassParameter (processorParameters[index]))
            processorParameters[index]->setValueNotifyingHost (
                snapshot[static_cast<size_t> (index)]);
}

void SmartDuckingAudioProcessor::switchComparisonSlot (int slot)
{
    slot = juce::jlimit (0, static_cast<int> (comparisonSlots.size()) - 1, slot);
    const auto previous = currentComparisonSlot.load (std::memory_order_relaxed);

    if (slot == previous)
        return;

    captureComparisonSlot (previous);
    restoreComparisonSlot (slot);
    currentComparisonSlot.store (slot, std::memory_order_relaxed);
}

void SmartDuckingAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    processorPrepared.store (false, std::memory_order_release);
    audioCallbackQuiescent.store (false, std::memory_order_release);
    currentSampleRate = juce::jmax (1.0, sampleRate);
    crossover.prepare (sampleRate,
                       crossoverParameter->load(),
                       24.0f + 12.0f * crossoverSlopeParameter->load());
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
    const auto stftLatency = spectralLcrProcessor.getLatencySamples();
    maximumLookaheadSamples = static_cast<int> (std::ceil (0.050 * currentSampleRate));
    currentStftLatencySamples = stftLatency;
    lowBandDelay.prepare (6, maximumDelay);
    highBandDelay.prepare (6, maximumDelay);
    mainLatencyDelay.prepare (6, maximumLookaheadSamples + maximumDelay);
    consecutiveSilentSidechainSamples = maximumLookaheadSamples
                                      + spectralLcrProcessor.getLatencySamples();
    staticSilentSidechainBlockCount = 0;
    const auto maximumLookaheadDelay = maximumLookaheadSamples + 2;
    sidechainLookaheadDelay.setMaximumDelayInSamples (maximumLookaheadDelay);
    sidechainLookaheadDelay.prepare (delaySpec);
    const auto requestedLookaheadSamples = juce::jlimit (
        0.0f,
        static_cast<float> (maximumLookaheadSamples),
        static_cast<float> (0.001 * currentSampleRate * lookaheadParameter->load()));
    const auto initialSidechainDelay = static_cast<float> (maximumLookaheadSamples)
                                     - requestedLookaheadSamples;
    sidechainLookaheadDelay.setDelay (initialSidechainDelay);
    staticSleepLookaheadBuffer.setSize (5,
                                        juce::jmax (1, maximumExpectedSamplesPerBlock),
                                        false,
                                        false,
                                        true);
    staticSleepSpatialOutputBuffer.setSize (5,
                                             juce::jmax (1, maximumExpectedSamplesPerBlock),
                                             false,
                                             false,
                                             true);
    staticSleepLfeBuffer.setSize (1,
                                  juce::jmax (1, maximumExpectedSamplesPerBlock),
                                  false,
                                  false,
                                  true);
    staticSleepSanitisedInputBuffer.setSize (6,
                                              juce::jmax (1, maximumExpectedSamplesPerBlock),
                                              false,
                                              false,
                                              true);
    smoothedSidechainDelaySamples.reset (currentSampleRate, 0.020);
    smoothedSidechainDelaySamples.setCurrentAndTargetValue (initialSidechainDelay);

    const auto initialMonitorMode = static_cast<smart_ducking::MonitorMode> (juce::jlimit (
        0, 6, juce::roundToInt (monitorModeParameter->load())));
    const auto initialCrossoverEnabled = initialMonitorMode == smart_ducking::MonitorMode::normal
        ? crossoverBypassParameter->load() < 0.5f
        : initialMonitorMode == smart_ducking::MonitorMode::crossoverLow
            || initialMonitorMode == smart_ducking::MonitorMode::crossoverHigh;
    const auto initialDelta = initialMonitorMode == smart_ducking::MonitorMode::widebandDelta
                           || initialMonitorMode == smart_ducking::MonitorMode::spectralDelta;
    const auto initialCrossoverLow = initialMonitorMode
        == smart_ducking::MonitorMode::crossoverLow;
    const auto initialCrossoverHigh = initialMonitorMode
        == smart_ducking::MonitorMode::crossoverHigh;
    const auto initialCrossoverMonitor = initialCrossoverLow || initialCrossoverHigh;
    const auto initialExternalCrossover = ! useNormalMode
                                       && (initialCrossoverEnabled || initialCrossoverMonitor);
    smoothedQualityCrossoverMix.reset (currentSampleRate, 0.030);
    smoothedQualityCrossoverMix.setCurrentAndTargetValue (
        ! useNormalMode && initialCrossoverEnabled ? 1.0f : 0.0f);
    smoothedDeltaMonitorMix.reset (currentSampleRate, 0.030);
    smoothedDeltaMonitorMix.setCurrentAndTargetValue (initialDelta ? 1.0f : 0.0f);
    smoothedQualityCrossoverLowMix.reset (currentSampleRate, 0.030);
    smoothedQualityCrossoverLowMix.setCurrentAndTargetValue (
        ! useNormalMode && initialCrossoverLow ? 1.0f : 0.0f);
    smoothedQualityCrossoverHighMix.reset (currentSampleRate, 0.030);
    smoothedQualityCrossoverHighMix.setCurrentAndTargetValue (
        ! useNormalMode && initialCrossoverHigh ? 1.0f : 0.0f);
    smoothedCrossoverMonitorLfeMute.reset (currentSampleRate, 0.030);
    smoothedCrossoverMonitorLfeMute.setCurrentAndTargetValue (
        initialCrossoverMonitor ? 1.0f : 0.0f);
    crossoverPathSleeping = ! initialExternalCrossover;
    crossoverWarmupSamplesRemaining = 0;
    smoothedGlobalBypassMix.reset (currentSampleRate, 0.030);
    const auto initiallyGloballyBypassed = globalBypassParameter->load() >= 0.5f;
    smoothedGlobalBypassMix.setCurrentAndTargetValue (initiallyGloballyBypassed ? 1.0f
                                                                               : 0.0f);
    globalBypassState = initiallyGloballyBypassed ? GlobalBypassState::bypassed
                                                   : GlobalBypassState::active;
    globalBypassWarmupSamplesRemaining = 0;
    globalBypassDspSleeping.store (initiallyGloballyBypassed, std::memory_order_relaxed);
    const auto initialiseRoutingSmoother = [this] (auto& smoother, bool enabled)
    {
        smoother.reset (currentSampleRate, 0.030);
        smoother.setCurrentAndTargetValue (enabled ? 1.0f : 0.0f);
    };
    initialiseRoutingSmoother (smoothedVoiceSurroundMix,
                              voiceFormatParameter->load() >= 0.5f);
    smoothedMainChannelOrder.prepare (currentSampleRate,
        smart_ducking::sanitiseSurroundChannelOrder (mainChannelOrderParameter->load()));
    smoothedVoiceChannelOrder.prepare (currentSampleRate,
        smart_ducking::sanitiseSurroundChannelOrder (voiceChannelOrderParameter->load()));
    smoothedOutputChannelOrder.prepare (currentSampleRate,
        smart_ducking::sanitiseSurroundChannelOrder (outputChannelOrderParameter->load()));
    initialiseRoutingSmoother (smoothedDownmixMix, downmixParameter->load() >= 0.5f);
    initialiseRoutingSmoother (smoothedDropLfeMix, dropLfeParameter->load() >= 0.5f);

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

void SmartDuckingAudioProcessor::setNonRealtime (bool shouldBeNonRealtime) noexcept
{
    juce::AudioProcessor::setNonRealtime (shouldBeNonRealtime);
    spectralLcrProcessor.setVisualisationEnabled (
        editorVisualisationRequested.load (std::memory_order_acquire)
            && ! shouldBeNonRealtime);
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

    audioCallbackQuiescent.store (
        consecutiveQuiescentTimerTicks >= quiescentTicksRequired,
        std::memory_order_release);

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
        currentStftLatencySamples = spectralLcrProcessor.getLatencySamples();
        lowBandDelay.reset();
        highBandDelay.reset();
        mainLatencyDelay.reset();
        currentEconomyMode.store (requestedNormalMode, std::memory_order_relaxed);
        currentResolutionIndex.store (requestedResolution, std::memory_order_relaxed);
        // The STFT history has been reset, but the variable sidechain delay may
        // still contain old samples.  Drain it before the block sleep path is
        // allowed to freeze its read/write positions.
        consecutiveSilentSidechainSamples = 0;
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

void SmartDuckingAudioProcessor::processStaticSilentSidechainBlock (
    juce::AudioBuffer<float>& mainInput,
    juce::AudioBuffer<float>& output,
    const smart_ducking::ChannelOrderMix& mainOrder,
    const smart_ducking::ChannelOrderMix& outputOrder,
    float downmixMix,
    float dropLfeMix) noexcept
{
    const auto numSamples = output.getNumSamples();
    const auto lookaheadCapacity = staticSleepLookaheadBuffer.getNumSamples();

    if (! isSurroundVariant())
    {
        auto processed = 0;

        while (processed < numSamples)
        {
            const auto blockSamples = juce::jmin (numSamples - processed,
                                                 lookaheadCapacity);
            std::array<const float*, 5> delayedMain {};
            std::array<float*, 5> delayedOutput {};

            for (int channel = 0; channel < 2; ++channel)
            {
                const auto spatialChannel = channel == 0 ? 0 : 2;
                const auto* source = mainInput.getReadPointer (channel, processed);
                auto* sanitisedInput = staticSleepSanitisedInputBuffer.getWritePointer (
                    channel);
                auto* lookaheadDelayed = staticSleepLookaheadBuffer.getWritePointer (
                    spatialChannel);

                for (int sample = 0; sample < blockSamples; ++sample)
                    sanitisedInput[sample] = std::isfinite (source[sample])
                        ? source[sample] : 0.0f;

                mainLatencyDelay.processBlock (channel,
                                                sanitisedInput,
                                                lookaheadDelayed,
                                                blockSamples,
                                                maximumLookaheadSamples);
                delayedMain[static_cast<size_t> (spatialChannel)] = lookaheadDelayed;
                delayedOutput[static_cast<size_t> (spatialChannel)] =
                    output.getWritePointer (channel, processed);
            }

            spectralLcrProcessor.processSilentSidechainBlock (delayedMain,
                                                              delayedOutput,
                                                              blockSamples,
                                                              true);
            processed += blockSamples;
        }

        globalBypassDspSleeping.store (false, std::memory_order_relaxed);
        ++staticSilentSidechainBlockCount;
        return;
    }

    const auto& inputMap = smart_ducking::getPhysicalToCanonicalChannels (mainOrder.from);
    const auto& outputMap = smart_ducking::getPhysicalToCanonicalChannels (outputOrder.from);
    const auto lfeCapacity = staticSleepLfeBuffer.getNumSamples();
    auto processed = 0;

    while (processed < numSamples)
    {
        const auto blockSamples = juce::jmin (numSamples - processed,
                                             lookaheadCapacity,
                                             lfeCapacity);
        std::array<const float*, 5> delayedMain {};
        std::array<float*, 5> delayedSpatial {};

        for (int physicalChannel = 0; physicalChannel < 6; ++physicalChannel)
        {
            const auto canonicalChannel = inputMap[static_cast<size_t> (physicalChannel)];
            const auto* source = mainInput.getReadPointer (physicalChannel, processed);

            if (canonicalChannel == 3)
            {
                auto* delayedLfe = staticSleepLfeBuffer.getWritePointer (0);
                auto* sanitisedInput = staticSleepSanitisedInputBuffer.getWritePointer (
                    physicalChannel);

                for (int sample = 0; sample < blockSamples; ++sample)
                    sanitisedInput[sample] = std::isfinite (source[sample])
                        ? source[sample] : 0.0f;

                mainLatencyDelay.processBlock (physicalChannel,
                                                sanitisedInput,
                                                delayedLfe,
                                                blockSamples,
                                                maximumLookaheadSamples
                                                    + currentStftLatencySamples);

                continue;
            }

            const auto spatialChannel = canonicalChannel == 0 ? 0
                                      : canonicalChannel == 2 ? 1
                                      : canonicalChannel == 1 ? 2
                                      : canonicalChannel == 4 ? 3 : 4;
            auto* lookaheadDelayed = staticSleepLookaheadBuffer.getWritePointer (
                spatialChannel);
            auto* sanitisedInput = staticSleepSanitisedInputBuffer.getWritePointer (
                physicalChannel);

            for (int sample = 0; sample < blockSamples; ++sample)
                sanitisedInput[sample] = std::isfinite (source[sample])
                    ? source[sample] : 0.0f;

            mainLatencyDelay.processBlock (physicalChannel,
                                            sanitisedInput,
                                            lookaheadDelayed,
                                            blockSamples,
                                            maximumLookaheadSamples);

            delayedMain[static_cast<size_t> (spatialChannel)] = lookaheadDelayed;
        }

        if (downmixMix <= 0.0f)
        {
            for (int spatialChannel = 0; spatialChannel < 5; ++spatialChannel)
            {
                constexpr std::array<int, 5> spatialToCanonical { 0, 2, 1, 4, 5 };
                const auto canonicalChannel = spatialToCanonical[static_cast<size_t> (
                    spatialChannel)];
                const auto physicalOutputChannel = static_cast<int> (std::distance (
                    outputMap.begin(),
                    std::find (outputMap.begin(), outputMap.end(), canonicalChannel)));
                delayedSpatial[static_cast<size_t> (spatialChannel)] =
                    output.getWritePointer (physicalOutputChannel, processed);
            }
        }
        else
        {
            for (int spatialChannel = 0; spatialChannel < 5; ++spatialChannel)
                delayedSpatial[static_cast<size_t> (spatialChannel)] =
                    staticSleepSpatialOutputBuffer.getWritePointer (spatialChannel);
        }

        spectralLcrProcessor.processSilentSidechainBlock (delayedMain,
                                                          delayedSpatial,
                                                          blockSamples,
                                                          false);

        if (downmixMix > 0.0f)
        {
            const auto* left = delayedSpatial[0];
            const auto* centre = delayedSpatial[1];
            const auto* right = delayedSpatial[2];
            const auto* leftSurround = delayedSpatial[3];
            const auto* rightSurround = delayedSpatial[4];
            const auto* lfe = staticSleepLfeBuffer.getReadPointer (0);
            auto* outputLeft = output.getWritePointer (0, processed);
            auto* outputRight = output.getWritePointer (1, processed);
            const auto lfeGain = 0.5f * (1.0f - dropLfeMix);

            for (int sample = 0; sample < blockSamples; ++sample)
            {
                const auto stereoLeft = left[sample]
                    + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                          * (centre[sample] + leftSurround[sample])
                    + lfeGain * lfe[sample];
                const auto stereoRight = right[sample]
                    + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                          * (centre[sample] + rightSurround[sample])
                    + lfeGain * lfe[sample];
                outputLeft[sample] = left[sample]
                    + downmixMix * (stereoLeft - left[sample]);
                outputRight[sample] = right[sample]
                    + downmixMix * (stereoRight - right[sample]);
            }

            for (int channel = 2; channel < output.getNumChannels(); ++channel)
                juce::FloatVectorOperations::multiply (
                    output.getWritePointer (channel, processed),
                    1.0f - downmixMix,
                    blockSamples);
        }
        else
        {
            const auto* lfe = staticSleepLfeBuffer.getReadPointer (0);
            const auto lfePhysicalChannel = static_cast<int> (std::distance (
                outputMap.begin(),
                std::find (outputMap.begin(), outputMap.end(), 3)));
            std::copy_n (lfe,
                         blockSamples,
                         output.getWritePointer (lfePhysicalChannel, processed));
        }

        processed += blockSamples;
    }

    globalBypassDspSleeping.store (false, std::memory_order_relaxed);
    ++staticSilentSidechainBlockCount;
}

void SmartDuckingAudioProcessor::processSteadyGlobalBypass (
    juce::AudioBuffer<float>& mainInput,
    juce::AudioBuffer<float>& output,
    bool visualisationActive) noexcept
{
    const auto mainIsStereo = ! isSurroundVariant();
    const auto forceDownmix = downmixParameter->load() >= 0.5f;
    const auto numSamples = output.getNumSamples();
    std::array<const float*, 6> mainRead {};
    std::array<float*, 6> outputWrite {};
    std::array<float, 5> maximumMainInputPeaks {};
    std::array<float, 5> maximumOutputPeaks {};

    for (int channel = 0; channel < 6; ++channel)
    {
        if (channel < mainInput.getNumChannels())
            mainRead[static_cast<size_t> (channel)] = mainInput.getReadPointer (channel);

        if (channel < output.getNumChannels())
            outputWrite[static_cast<size_t> (channel)] = output.getWritePointer (channel);
    }

    const auto readSample = [] (const std::array<const float*, 6>& channels,
                                int channel,
                                int sample) noexcept
    {
        const auto* data = channels[static_cast<size_t> (channel)];
        return data != nullptr ? data[sample] : 0.0f;
    };

    const auto updatePeak = [] (float& maximum, float value) noexcept
    {
        maximum = juce::jmax (maximum, std::abs (value));
    };
    const auto mainOrderIsConstant = ! smoothedMainChannelOrder.isSmoothing();
    const auto mainOrderConstant = smoothedMainChannelOrder.getCurrent();
    const auto outputOrderIsConstant = ! smoothedOutputChannelOrder.isSmoothing();
    const auto outputOrderConstant = smoothedOutputChannelOrder.getCurrent();
    const auto downmixMixIsConstant = mainIsStereo || ! smoothedDownmixMix.isSmoothing();
    const auto downmixMixConstant = mainIsStereo ? 1.0f : smoothedDownmixMix.getCurrentValue();
    const auto dropLfeMixIsConstant = ! smoothedDropLfeMix.isSmoothing();
    const auto dropLfeMixConstant = smoothedDropLfeMix.getCurrentValue();

    for (int sample = 0; sample < numSamples; ++sample)
    {
        std::array<float, 6> rawMain {};
        std::array<float, 6> delayedMain {};

        const auto mainChannelsToProcess = mainIsStereo ? 2 : 6;
        for (int channel = 0; channel < mainChannelsToProcess; ++channel)
        {
            const auto index = static_cast<size_t> (channel);
            rawMain[index] = readSample (mainRead, channel, sample);
            delayedMain[index] = mainLatencyDelay.processSample (
                channel,
                rawMain[index],
                maximumLookaheadSamples + currentStftLatencySamples);
        }

        const auto mainOrderMix = mainOrderIsConstant
            ? mainOrderConstant : smoothedMainChannelOrder.getNext();
        const auto outputOrderMix = outputOrderIsConstant
            ? outputOrderConstant : smoothedOutputChannelOrder.getNext();
        const auto downmixMix = downmixMixIsConstant
            ? downmixMixConstant : smoothedDownmixMix.getNextValue();
        const auto dropLfeMix = dropLfeMixIsConstant
            ? dropLfeMixConstant : smoothedDropLfeMix.getNextValue();
        auto rawCanonical = rawMain;
        auto delayedCanonical = delayedMain;

        if (! mainIsStereo)
        {
            rawCanonical = smart_ducking::applySurroundInputOrder (rawMain, mainOrderMix);
            delayedCanonical = smart_ducking::applySurroundInputOrder (delayedMain,
                                                                       mainOrderMix);
        }

        const auto outputValues = makeOutputSample (delayedCanonical,
                                                    mainIsStereo,
                                                    outputOrderMix,
                                                    downmixMix,
                                                    dropLfeMix);

        if (visualisationActive)
        {
            updatePeak (maximumMainInputPeaks[0], rawCanonical[0]);
            updatePeak (maximumMainInputPeaks[2], rawCanonical[1]);
            updatePeak (maximumOutputPeaks[0],
                        forceDownmix ? outputValues[0] : delayedCanonical[0]);
            updatePeak (maximumOutputPeaks[2],
                        forceDownmix ? outputValues[1] : delayedCanonical[1]);

            if (! mainIsStereo)
            {
                updatePeak (maximumMainInputPeaks[1], rawCanonical[2]);
                updatePeak (maximumMainInputPeaks[3], rawCanonical[4]);
                updatePeak (maximumMainInputPeaks[4], rawCanonical[5]);

                if (! forceDownmix)
                {
                    updatePeak (maximumOutputPeaks[1], delayedCanonical[2]);
                    updatePeak (maximumOutputPeaks[3], delayedCanonical[4]);
                    updatePeak (maximumOutputPeaks[4], delayedCanonical[5]);
                }
            }
        }

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            if (auto* destination = outputWrite[static_cast<size_t> (channel)])
            {
                const auto value = outputValues[static_cast<size_t> (channel)];
                destination[sample] = std::isfinite (value) ? value : 0.0f;
            }
    }

    if (! visualisationActive)
        return;

    mainInputPeakLeft.store (maximumMainInputPeaks[0], std::memory_order_relaxed);
    mainInputPeakCentre.store (maximumMainInputPeaks[1], std::memory_order_relaxed);
    mainInputPeakRight.store (maximumMainInputPeaks[2], std::memory_order_relaxed);
    mainInputPeakLeftSurround.store (maximumMainInputPeaks[3], std::memory_order_relaxed);
    mainInputPeakRightSurround.store (maximumMainInputPeaks[4], std::memory_order_relaxed);
    outputPeakLeft.store (maximumOutputPeaks[0], std::memory_order_relaxed);
    outputPeakCentre.store (maximumOutputPeaks[1], std::memory_order_relaxed);
    outputPeakRight.store (maximumOutputPeaks[2], std::memory_order_relaxed);
    outputPeakLeftSurround.store (maximumOutputPeaks[3], std::memory_order_relaxed);
    outputPeakRightSurround.store (maximumOutputPeaks[4], std::memory_order_relaxed);
    sidechainPeakLeft.store (0.0f, std::memory_order_relaxed);
    sidechainPeakCentre.store (0.0f, std::memory_order_relaxed);
    sidechainPeakRight.store (0.0f, std::memory_order_relaxed);
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
    processBlockCounter.fetch_add (1, std::memory_order_relaxed);
    const auto offlineRendering = isNonRealtime();
    spectralLcrProcessor.setVisualisationEnabled (
        editorVisualisationRequested.load (std::memory_order_acquire)
            && ! offlineRendering);

    auto mainInput = getBusBuffer (buffer, true, 0);
    auto output = getBusBuffer (buffer, false, 0);
    const auto numSamples = buffer.getNumSamples();
    // The playhead is consulted only while a mode/resolution rebuild is
    // pending. Querying it on every ordinary audio block asks the host for
    // transport state that the DSP does not otherwise use.
    if (configurationPending.load (std::memory_order_acquire))
        updateTransportState();
    const auto sidechainEnabled = getBusCount (true) >= 2 && getBus (true, 1)->isEnabled();
    auto sidechain = sidechainEnabled ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float> {};
    const auto mainIsStereo = ! isSurroundVariant();
    const auto voiceIsStereo = voiceFormatParameter->load() < 0.5f;
    const auto forceDownmix = downmixParameter->load() >= 0.5f;
    const auto useNormalMode = currentEconomyMode.load (std::memory_order_relaxed);
    const auto globalBypassRequested = globalBypassParameter->load() >= 0.5f;

    if (globalBypassRequested)
    {
        if (globalBypassState == GlobalBypassState::active
            || globalBypassState == GlobalBypassState::fadingToWet)
            globalBypassState = GlobalBypassState::fadingToBypass;
        else if (globalBypassState == GlobalBypassState::warmingWet)
            globalBypassState = GlobalBypassState::bypassed;
    }
    else if (globalBypassState == GlobalBypassState::bypassed)
    {
        globalBypassState = GlobalBypassState::warmingWet;
        globalBypassWarmupSamplesRemaining = getTotalLatencySamples();
    }
    else if (globalBypassState == GlobalBypassState::fadingToBypass)
    {
        globalBypassState = GlobalBypassState::fadingToWet;
    }

    globalBypassDspSleeping.store (
        globalBypassState == GlobalBypassState::bypassed,
        std::memory_order_relaxed);
    const auto monitorMode = static_cast<smart_ducking::MonitorMode> (juce::jlimit (
        0, 6, juce::roundToInt (monitorModeParameter->load())));
    auto crossoverEnabled = crossoverBypassParameter->load() < 0.5f;
    auto widebandEnabled = widebandBypassParameter->load() < 0.5f;
    auto spectralEnabled = spectralBypassParameter->load() < 0.5f;

    if (monitorMode != smart_ducking::MonitorMode::normal)
    {
        crossoverEnabled = monitorMode == smart_ducking::MonitorMode::crossoverLow
                        || monitorMode == smart_ducking::MonitorMode::crossoverHigh;
        widebandEnabled = monitorMode == smart_ducking::MonitorMode::widebandSolo
                      || monitorMode == smart_ducking::MonitorMode::widebandDelta;
        spectralEnabled = monitorMode == smart_ducking::MonitorMode::spectralSolo
                       || monitorMode == smart_ducking::MonitorMode::spectralDelta;
    }

    const auto crossoverMonitorRequested =
        monitorMode == smart_ducking::MonitorMode::crossoverLow
        || monitorMode == smart_ducking::MonitorMode::crossoverHigh;
    const auto externalCrossoverRequested = ! useNormalMode && crossoverEnabled;

    if (externalCrossoverRequested && crossoverPathSleeping)
    {
        crossover.reset();
        crossoverPathSleeping = false;
        crossoverWarmupSamplesRemaining = spectralLcrProcessor.getLatencySamples();
    }
    else if (! externalCrossoverRequested && crossoverWarmupSamplesRemaining > 0)
    {
        crossoverWarmupSamplesRemaining = 0;
        crossoverPathSleeping = true;
    }

    const auto externalCrossoverReady = externalCrossoverRequested
                                     && crossoverWarmupSamplesRemaining == 0;
    smoothedQualityCrossoverMix.setTargetValue (
        externalCrossoverReady && ! useNormalMode && crossoverEnabled ? 1.0f : 0.0f);
    smoothedDeltaMonitorMix.setTargetValue (
        monitorMode == smart_ducking::MonitorMode::widebandDelta
            || monitorMode == smart_ducking::MonitorMode::spectralDelta
            ? 1.0f : 0.0f);
    smoothedQualityCrossoverLowMix.setTargetValue (
        externalCrossoverReady
            && ! useNormalMode
            && monitorMode == smart_ducking::MonitorMode::crossoverLow
            ? 1.0f : 0.0f);
    smoothedQualityCrossoverHighMix.setTargetValue (
        externalCrossoverReady
            && ! useNormalMode
            && monitorMode == smart_ducking::MonitorMode::crossoverHigh
            ? 1.0f : 0.0f);
    smoothedCrossoverMonitorLfeMute.setTargetValue (
        crossoverMonitorRequested && (useNormalMode || externalCrossoverReady) ? 1.0f : 0.0f);
    smoothedGlobalBypassMix.setTargetValue (
        globalBypassState == GlobalBypassState::active
            || globalBypassState == GlobalBypassState::fadingToWet ? 0.0f : 1.0f);
    smoothedVoiceSurroundMix.setTargetValue (voiceFormatParameter->load() >= 0.5f
                                                 ? 1.0f : 0.0f);
    smoothedMainChannelOrder.setTarget (
        smart_ducking::sanitiseSurroundChannelOrder (mainChannelOrderParameter->load()));
    smoothedVoiceChannelOrder.setTarget (
        smart_ducking::sanitiseSurroundChannelOrder (voiceChannelOrderParameter->load()));
    smoothedOutputChannelOrder.setTarget (
        smart_ducking::sanitiseSurroundChannelOrder (outputChannelOrderParameter->load()));
    smoothedDownmixMix.setTargetValue (forceDownmix ? 1.0f : 0.0f);
    smoothedDropLfeMix.setTargetValue (dropLfeParameter->load() >= 0.5f ? 1.0f : 0.0f);
    const auto visualisationActive = spectralLcrProcessor.isVisualisationEnabled();
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    constexpr auto diagnosticsActive = true;
#else
    constexpr auto diagnosticsActive = false;
#endif
    const auto collectVoicePeaks = visualisationActive || diagnosticsActive;

    if (globalBypassState == GlobalBypassState::bypassed)
    {
        // The sidechain delay does not advance in steady bypass, so require a
        // fresh silent drain period before using the sleeping block path after
        // the wet path is re-enabled.
        consecutiveSilentSidechainSamples = 0;
        processSteadyGlobalBypass (mainInput,
                                   output,
                                   visualisationActive);
        return;
    }

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
    crossover.setSlopeDbPerOctave (
        24.0f + 12.0f * crossoverSlopeParameter->load());
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
    spectralParameters.crossoverSlopeDbPerOctave =
        24.0f + 12.0f * crossoverSlopeParameter->load();
    spectralParameters.crossoverStageMix = crossoverEnabled ? 1.0f : 0.0f;
    spectralParameters.widebandStageMix = widebandEnabled ? 1.0f : 0.0f;
    spectralParameters.spectralStageMix = spectralEnabled ? 1.0f : 0.0f;
    spectralParameters.crossoverLowMonitorMix = useNormalMode
        && monitorMode == smart_ducking::MonitorMode::crossoverLow ? 1.0f : 0.0f;
    spectralParameters.crossoverHighMonitorMix = useNormalMode
        && monitorMode == smart_ducking::MonitorMode::crossoverHigh ? 1.0f : 0.0f;
    for (size_t index = 0; index < spectralParameters.duckMatrix.coefficients.size(); ++index)
        spectralParameters.duckMatrix.coefficients[index] = matrixParameters[index]->load();

    spectralLcrProcessor.setParameters (spectralParameters);

    const auto staticSilentSidechainCandidate = useNormalMode
        && ! visualisationActive
        && ! diagnosticsActive
        && globalBypassState == GlobalBypassState::active
        && crossoverPathSleeping
        && monitorMode == smart_ducking::MonitorMode::normal
        && spectralLcrProcessor.canUseSilentSidechainBlockPath()
        && ! smoothedSidechainDelaySamples.isSmoothing()
        && ! smoothedQualityCrossoverMix.isSmoothing()
        && ! smoothedDeltaMonitorMix.isSmoothing()
        && ! smoothedQualityCrossoverLowMix.isSmoothing()
        && ! smoothedQualityCrossoverHighMix.isSmoothing()
        && ! smoothedCrossoverMonitorLfeMute.isSmoothing()
        && ! smoothedVoiceSurroundMix.isSmoothing()
        && ! smoothedMainChannelOrder.isSmoothing()
        && ! smoothedVoiceChannelOrder.isSmoothing()
        && ! smoothedOutputChannelOrder.isSmoothing()
        && ! smoothedDownmixMix.isSmoothing()
        && ! smoothedDropLfeMix.isSmoothing()
        && ! smoothedGlobalBypassMix.isSmoothing()
        && smoothedGlobalBypassMix.getCurrentValue() <= 0.0f;

    auto staticSilentSidechainPath = false;

    if (staticSilentSidechainCandidate)
    {
        auto sidechainBlockIsSilent = true;
        if (sidechainEnabled)
            for (int channel = 0; channel < sidechain.getNumChannels(); ++channel)
                if (sidechain.getMagnitude (channel, 0, numSamples)
                    > smart_ducking::SidechainClassifier::technicalSilenceFloor)
                {
                    sidechainBlockIsSilent = false;
                    break;
                }

        const auto sidechainSilenceDrainSamples = maximumLookaheadSamples
                                                + spectralLcrProcessor.getLatencySamples();
        const auto sidechainDelayAndHistoryWereSilent =
            consecutiveSilentSidechainSamples >= sidechainSilenceDrainSamples;
        staticSilentSidechainPath = sidechainBlockIsSilent
                                  && sidechainDelayAndHistoryWereSilent;

        if (sidechainBlockIsSilent)
            consecutiveSilentSidechainSamples = juce::jmin (
                sidechainSilenceDrainSamples,
                consecutiveSilentSidechainSamples + numSamples);
        else
            consecutiveSilentSidechainSamples = 0;
    }
    else
    {
        // The ordinary path advances the sidechain delay.  Starting the
        // counter again avoids assuming that its contents are silent after an
        // active or transitional block.
        consecutiveSilentSidechainSamples = 0;
    }

    if (staticSilentSidechainPath)
    {
        processStaticSilentSidechainBlock (mainInput,
                                           output,
                                           smoothedMainChannelOrder.getCurrent(),
                                           smoothedOutputChannelOrder.getCurrent(),
                                           smoothedDownmixMix.getCurrentValue(),
                                           smoothedDropLfeMix.getCurrentValue());
        return;
    }

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
    const auto voiceSurroundMixIsConstant = ! smoothedVoiceSurroundMix.isSmoothing();
    const auto voiceSurroundMixConstant = smoothedVoiceSurroundMix.getCurrentValue();
    const auto deltaMixIsConstant = ! smoothedDeltaMonitorMix.isSmoothing();
    const auto deltaMixConstant = smoothedDeltaMonitorMix.getCurrentValue();
    const auto crossoverLowMixIsConstant = ! smoothedQualityCrossoverLowMix.isSmoothing();
    const auto crossoverLowMixConstant = smoothedQualityCrossoverLowMix.getCurrentValue();
    const auto crossoverHighMixIsConstant = ! smoothedQualityCrossoverHighMix.isSmoothing();
    const auto crossoverHighMixConstant = smoothedQualityCrossoverHighMix.getCurrentValue();
    const auto globalBypassMixIsConstant = ! smoothedGlobalBypassMix.isSmoothing();
    const auto globalBypassMixConstant = smoothedGlobalBypassMix.getCurrentValue();
    const auto sidechainDelayIsConstant = ! smoothedSidechainDelaySamples.isSmoothing();
    const auto sidechainDelayConstant = smoothedSidechainDelaySamples.getCurrentValue();
    const auto qualityCrossoverMixIsConstant = ! smoothedQualityCrossoverMix.isSmoothing();
    const auto qualityCrossoverMixConstant = smoothedQualityCrossoverMix.getCurrentValue();
    const auto crossoverLfeMuteIsConstant = ! smoothedCrossoverMonitorLfeMute.isSmoothing();
    const auto crossoverLfeMuteConstant = smoothedCrossoverMonitorLfeMute.getCurrentValue();
    const auto mainOrderIsConstant = ! smoothedMainChannelOrder.isSmoothing();
    const auto mainOrderConstant = smoothedMainChannelOrder.getCurrent();
    const auto voiceOrderIsConstant = ! smoothedVoiceChannelOrder.isSmoothing();
    const auto voiceOrderConstant = smoothedVoiceChannelOrder.getCurrent();
    const auto outputOrderIsConstant = ! smoothedOutputChannelOrder.isSmoothing();
    const auto outputOrderConstant = smoothedOutputChannelOrder.getCurrent();
    const auto downmixMixIsConstant = mainIsStereo || ! smoothedDownmixMix.isSmoothing();
    const auto downmixMixConstant = mainIsStereo ? 1.0f : smoothedDownmixMix.getCurrentValue();
    const auto dropLfeMixIsConstant = ! smoothedDropLfeMix.isSmoothing();
    const auto dropLfeMixConstant = smoothedDropLfeMix.getCurrentValue();

    if (sidechainDelayIsConstant)
        sidechainLookaheadDelay.setDelay (sidechainDelayConstant);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        std::array<float, 6> rawMain {};
        std::array<float, 6> rawVoice {};
        std::array<float, 6> delayedMain {};
        std::array<float, 6> delayedVoice {};
        std::array<float, 6> delayedBypassMain {};
        const auto voiceSurroundMix = voiceSurroundMixIsConstant
            ? voiceSurroundMixConstant : smoothedVoiceSurroundMix.getNextValue();
        const auto deltaMix = deltaMixIsConstant
            ? deltaMixConstant : smoothedDeltaMonitorMix.getNextValue();
        const auto crossoverLowMix = crossoverLowMixIsConstant
            ? crossoverLowMixConstant : smoothedQualityCrossoverLowMix.getNextValue();
        const auto crossoverHighMix = crossoverHighMixIsConstant
            ? crossoverHighMixConstant : smoothedQualityCrossoverHighMix.getNextValue();
        const auto crossoverMonitorMixSum = crossoverLowMix + crossoverHighMix;
        const auto crossoverMonitorMix = juce::jlimit (
            0.0f, 1.0f, crossoverMonitorMixSum);
        const auto globalBypassMix = globalBypassMixIsConstant
            ? globalBypassMixConstant : smoothedGlobalBypassMix.getNextValue();
        if (! sidechainDelayIsConstant)
            sidechainLookaheadDelay.setDelay (smoothedSidechainDelaySamples.getNextValue());

        const auto mainChannelsToProcess = mainIsStereo ? 2 : 6;
        for (int channel = 0; channel < mainChannelsToProcess; ++channel)
        {
            const auto index = static_cast<size_t> (channel);
            rawMain[index] = readSample (mainRead, channel, sample);
            const auto delayed = mainLatencyDelay.processSample (
                channel,
                rawMain[index],
                maximumLookaheadSamples,
                maximumLookaheadSamples + currentStftLatencySamples);
            delayedMain[index] = delayed.first;
            delayedBypassMain[index] = delayed.second;
        }

        const auto voiceChannelsToProcess = ! voiceIsStereo || voiceSurroundMix > 0.0f ? 6 : 2;
        for (int channel = 0; channel < voiceChannelsToProcess; ++channel)
        {
            const auto index = static_cast<size_t> (channel);
            rawVoice[index] = readSample (voiceRead, channel, sample);
            sidechainLookaheadDelay.pushSample (channel, rawVoice[index]);
            delayedVoice[index] = sidechainLookaheadDelay.popSample (channel);
        }

        smart_ducking::StereoSpectralLcrProcessor::LcrSample mainCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample voiceCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample rawMainCanonical;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample rawVoiceCanonical;
        float mainLfe = 0.0f;
        float bypassMainLfe = 0.0f;
        const auto mainOrderMix = mainOrderIsConstant
            ? mainOrderConstant : smoothedMainChannelOrder.getNext();
        const auto voiceOrderMix = voiceOrderIsConstant
            ? voiceOrderConstant : smoothedVoiceChannelOrder.getNext();
        const auto outputOrderMix = outputOrderIsConstant
            ? outputOrderConstant : smoothedOutputChannelOrder.getNext();
        const auto downmixMix = downmixMixIsConstant
            ? downmixMixConstant : smoothedDownmixMix.getNextValue();
        const auto dropLfeMix = dropLfeMixIsConstant
            ? dropLfeMixConstant : smoothedDropLfeMix.getNextValue();

        const auto mapMain = [&] (const std::array<float, 6>& source,
                                  smart_ducking::StereoSpectralLcrProcessor::LcrSample& destination,
                                  float& lfe) noexcept
        {
            if (mainIsStereo)
            {
                destination.left = source[0];
                destination.right = source[1];
                return;
            }

            const auto canonical = smart_ducking::applySurroundInputOrder (source,
                                                                           mainOrderMix);
            destination.left = canonical[0];
            destination.right = canonical[1];
            destination.centre = canonical[2];
            lfe = canonical[3];
            destination.leftSurround = canonical[4];
            destination.rightSurround = canonical[5];
        };

        const auto mapVoice = [&] (const std::array<float, 6>& source,
                                   smart_ducking::StereoSpectralLcrProcessor::LcrSample& destination) noexcept
        {
            if (voiceSurroundMix <= 0.0f)
            {
                destination.left = source[0];
                destination.right = source[1];
                return;
            }

            const auto canonical = smart_ducking::applySurroundInputOrder (source,
                                                                           voiceOrderMix);
            destination.left = source[0] + voiceSurroundMix * (canonical[0] - source[0]);
            destination.right = source[1] + voiceSurroundMix * (canonical[1] - source[1]);
            destination.centre = voiceSurroundMix * canonical[2];
            destination.leftSurround = voiceSurroundMix * canonical[4];
            destination.rightSurround = voiceSurroundMix * canonical[5];
        };

        float ignoredRawLfe = 0.0f;
        mapMain (delayedMain, mainCanonical, mainLfe);
        smart_ducking::StereoSpectralLcrProcessor::LcrSample bypassMainCanonical;
        mapVoice (delayedVoice, voiceCanonical);

        if (visualisationActive)
            mapMain (rawMain, rawMainCanonical, ignoredRawLfe);
        if (collectVoicePeaks)
            mapVoice (rawVoice, rawVoiceCanonical);
        // The second tap is also the aligned LFE path, so keep its routing
        // current even while global bypass is inactive.
        mapMain (delayedBypassMain, bypassMainCanonical, bypassMainLfe);

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
        smart_ducking::StereoSpectralLcrProcessor::LcrSample delayedHigh;
        smart_ducking::StereoSpectralLcrProcessor::LcrSample lowBandForVisualisation;
        const auto qualityCrossoverMix = qualityCrossoverMixIsConstant
            ? qualityCrossoverMixConstant : smoothedQualityCrossoverMix.getNextValue();

        const auto processCrossoverChannel = [&] (int channel, float input)
        {
            const auto bands = crossover.processSample (channel, input);
            const auto low = lowBandDelay.processSample (
                channel, bands.low, currentStftLatencySamples);
            const auto high = highBandDelay.processSample (
                channel, bands.high, currentStftLatencySamples);

            switch (channel)
            {
                case 0: highInput.left = bands.high; delayedLow.left = low; delayedHigh.left = high;
                        lowBandForVisualisation.left = bands.low; break;
                case 1: highInput.centre = bands.high; delayedLow.centre = low; delayedHigh.centre = high;
                        lowBandForVisualisation.centre = bands.low; break;
                case 2: highInput.right = bands.high; delayedLow.right = low; delayedHigh.right = high;
                        lowBandForVisualisation.right = bands.low; break;
                case 3: highInput.leftSurround = bands.high; delayedLow.leftSurround = low;
                        delayedHigh.leftSurround = high;
                        lowBandForVisualisation.leftSurround = bands.low; break;
                case 4: highInput.rightSurround = bands.high; delayedLow.rightSurround = low;
                        delayedHigh.rightSurround = high;
                        lowBandForVisualisation.rightSurround = bands.low; break;
                default: break;
            }
        };

        if (! crossoverPathSleeping)
        {
            crossover.beginSample();

            if (mainIsStereo)
            {
                processCrossoverChannel (0, mainCanonical.left);
                processCrossoverChannel (2, mainCanonical.right);
            }
            else
            {
                for (int channel = 0; channel < 5; ++channel)
                    processCrossoverChannel (channel, mainCanonical.get (channel));
            }

            if (crossoverWarmupSamplesRemaining > 0
                && --crossoverWarmupSamplesRemaining == 0)
            {
                smoothedQualityCrossoverMix.setTargetValue (
                    ! useNormalMode && crossoverEnabled ? 1.0f : 0.0f);
                smoothedQualityCrossoverLowMix.setTargetValue (
                    monitorMode == smart_ducking::MonitorMode::crossoverLow ? 1.0f : 0.0f);
                smoothedQualityCrossoverHighMix.setTargetValue (
                    monitorMode == smart_ducking::MonitorMode::crossoverHigh ? 1.0f : 0.0f);
                smoothedCrossoverMonitorLfeMute.setTargetValue (
                    crossoverMonitorRequested ? 1.0f : 0.0f);
            }
        }

        if (useNormalMode)
        {
            highInput = mainCanonical;
        }
        else if (mainIsStereo)
        {
            highInput.left = mainCanonical.left
                           + qualityCrossoverMix * (highInput.left - mainCanonical.left);
            highInput.right = mainCanonical.right
                            + qualityCrossoverMix * (highInput.right - mainCanonical.right);
        }
        else
        {
            for (int channel = 0; channel < 5; ++channel)
            {
                const auto blended = mainCanonical.get (channel)
                    + qualityCrossoverMix
                          * (highInput.get (channel) - mainCanonical.get (channel));

                switch (channel)
                {
                    case 0: highInput.left = blended; break;
                    case 1: highInput.centre = blended; break;
                    case 2: highInput.right = blended; break;
                    case 3: highInput.leftSurround = blended; break;
                    case 4: highInput.rightSurround = blended; break;
                    default: break;
                }
            }
        }

        const auto delayedLfe = mainIsStereo ? 0.0f : bypassMainLfe;

        if (! externalCrossoverRequested
            && crossoverWarmupSamplesRemaining == 0
            && qualityCrossoverMix <= 0.0f
            && crossoverMonitorMix <= 0.0f)
            crossoverPathSleeping = true;

        const auto processedHigh = spectralLcrProcessor.processSample (
            highInput,
            voiceCanonical,
            mainIsStereo,
            voiceIsStereo,
            lowBandForVisualisation);
        const auto delayedDryMain = spectralLcrProcessor.getLastDirectDelayedInput();

        const auto currentOutputFrame = spectralLcrProcessor.getProcessedOutputFrameCount();

        if (visualisationActive && currentOutputFrame != observedOutputFrame)
        {
            observedOutputFrame = currentOutputFrame;
            updateReductionPeaks();
        }

        smart_ducking::StereoSpectralLcrProcessor::LcrSample fullBand;

        if (mainIsStereo)
        {
            fullBand.left = (useNormalMode ? 0.0f : qualityCrossoverMix * delayedLow.left)
                          + processedHigh.left
                          + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                * processedHigh.centre;
            fullBand.right = (useNormalMode ? 0.0f : qualityCrossoverMix * delayedLow.right)
                           + processedHigh.right
                           + smart_ducking::StereoSpectralLcrProcessor::centreDownmixGain
                                 * processedHigh.centre;
        }
        else
        {
            const auto lowMix = useNormalMode ? 0.0f : qualityCrossoverMix;
            fullBand.left = lowMix * delayedLow.left + processedHigh.left;
            fullBand.centre = lowMix * delayedLow.centre + processedHigh.centre;
            fullBand.right = lowMix * delayedLow.right + processedHigh.right;
            fullBand.leftSurround = lowMix * delayedLow.leftSurround + processedHigh.leftSurround;
            fullBand.rightSurround = lowMix * delayedLow.rightSurround + processedHigh.rightSurround;
        }

        const auto mapOutput = [mainIsStereo, downmixMix, dropLfeMix, &outputOrderMix] (
                                   const auto& spatial, float lfe) noexcept
        {
            return makeOutputSample (
                { spatial.left, spatial.right, spatial.centre, lfe,
                  spatial.leftSurround, spatial.rightSurround },
                mainIsStereo,
                outputOrderMix,
                downmixMix,
                dropLfeMix);
        };

        const auto crossoverMonitorLfeMute = crossoverLfeMuteIsConstant
            ? crossoverLfeMuteConstant : smoothedCrossoverMonitorLfeMute.getNextValue();
        auto outputValues = mapOutput (fullBand,
                                       delayedLfe * (1.0f - crossoverMonitorLfeMute));

        if (deltaMix > 0.0f)
        {
            const auto dryOutputValues = mapOutput (delayedDryMain, delayedLfe);
            for (size_t channel = 0; channel < outputValues.size(); ++channel)
                outputValues[channel] += deltaMix
                    * ((dryOutputValues[channel] - outputValues[channel])
                       - outputValues[channel]);
        }

        if (crossoverMonitorMix > 0.0f)
        {
            const auto lowOutputValues = mapOutput (delayedLow, 0.0f);
            const auto highOutputValues = mapOutput (delayedHigh, 0.0f);
            const auto crossoverLowShare = crossoverLowMix / crossoverMonitorMixSum;
            const auto crossoverHighShare = crossoverHighMix / crossoverMonitorMixSum;
            for (size_t channel = 0; channel < outputValues.size(); ++channel)
            {
                const auto crossoverMonitor = crossoverLowShare * lowOutputValues[channel]
                                            + crossoverHighShare * highOutputValues[channel];
                outputValues[channel] += crossoverMonitorMix
                    * (crossoverMonitor - outputValues[channel]);
            }
        }

        if (globalBypassMix > 0.0f)
        {
            const auto bypassOutputValues = mapOutput (bypassMainCanonical, bypassMainLfe);
            for (size_t channel = 0; channel < outputValues.size(); ++channel)
                outputValues[channel] += globalBypassMix
                    * (bypassOutputValues[channel] - outputValues[channel]);
        }

        if (visualisationActive)
        {
            const auto canonicalOutputValues = ! mainIsStereo && ! forceDownmix
                ? smart_ducking::applySurroundInputOrder (outputValues, outputOrderMix)
                : outputValues;
            maximumOutputPeaks[0] = juce::jmax (maximumOutputPeaks[0],
                                                std::abs (canonicalOutputValues[0]));
            maximumOutputPeaks[2] = juce::jmax (maximumOutputPeaks[2],
                                                std::abs (canonicalOutputValues[1]));

            if (! mainIsStereo && ! forceDownmix)
            {
                maximumOutputPeaks[1] = juce::jmax (maximumOutputPeaks[1],
                                                    std::abs (canonicalOutputValues[2]));
                maximumOutputPeaks[3] = juce::jmax (maximumOutputPeaks[3],
                                                    std::abs (canonicalOutputValues[4]));
                maximumOutputPeaks[4] = juce::jmax (maximumOutputPeaks[4],
                                                    std::abs (canonicalOutputValues[5]));
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

    if (globalBypassState == GlobalBypassState::fadingToBypass
        && ! smoothedGlobalBypassMix.isSmoothing()
        && smoothedGlobalBypassMix.getCurrentValue() >= 1.0f)
    {
        globalBypassState = GlobalBypassState::bypassed;
        globalBypassDspSleeping.store (true, std::memory_order_relaxed);
    }
    else if (globalBypassState == GlobalBypassState::warmingWet)
    {
        globalBypassWarmupSamplesRemaining -= numSamples;

        if (globalBypassWarmupSamplesRemaining <= 0)
        {
            globalBypassWarmupSamplesRemaining = 0;
            globalBypassState = GlobalBypassState::fadingToWet;
            smoothedGlobalBypassMix.setTargetValue (0.0f);
        }
    }
    else if (globalBypassState == GlobalBypassState::fadingToWet
             && ! smoothedGlobalBypassMix.isSmoothing()
             && smoothedGlobalBypassMix.getCurrentValue() <= 0.0f)
    {
        globalBypassState = GlobalBypassState::active;
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
    const auto activeSlot = currentComparisonSlot.load (std::memory_order_relaxed);
    captureComparisonSlot (activeSlot);
    auto stateToSave = parameters.copyState();
    stateToSave.setProperty ("comparisonSlot", activeSlot, nullptr);
    stateToSave.setProperty ("factoryProgram",
                             currentFactoryProgram.load (std::memory_order_relaxed),
                             nullptr);

    const auto& processorParameters = getParameters();
    for (size_t slot = 0; slot < comparisonSlots.size(); ++slot)
    {
        juce::ValueTree snapshot (slot == 0 ? "ComparisonA" : "ComparisonB");
        for (int index = 0; index < processorParameters.size(); ++index)
            if (const auto* parameter = dynamic_cast<juce::AudioProcessorParameterWithID*> (
                    processorParameters[index]);
                parameter != nullptr && ! isGlobalBypassParameter (parameter))
                snapshot.setProperty (parameter->paramID,
                                      comparisonSlots[slot][static_cast<size_t> (index)],
                                      nullptr);
        stateToSave.addChild (snapshot, -1, nullptr);
    }

    if (const auto stateXml = stateToSave.createXml())
        copyXmlToBinary (*stateXml, destinationData);
}

void SmartDuckingAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto stateXml = getXmlFromBinary (data, sizeInBytes);

    if (stateXml != nullptr && stateXml->hasTagName (parameters.state.getType()))
    {
        auto restoredState = juce::ValueTree::fromXml (*stateXml);
        const auto savedComparisonSlot = juce::jlimit (
            0, 1, static_cast<int> (restoredState.getProperty ("comparisonSlot", 0)));
        const auto savedFactoryProgram = juce::jlimit (
            0,
            getNumPrograms() - 1,
            static_cast<int> (restoredState.getProperty ("factoryProgram", 0)));
        std::array<juce::ValueTree, 2> savedSnapshots {
            restoredState.getChildWithName ("ComparisonA"),
            restoredState.getChildWithName ("ComparisonB")
        };
        for (const auto& snapshot : savedSnapshots)
            if (snapshot.isValid())
                restoredState.removeChild (snapshot, nullptr);
        removeLegacyState (restoredState);

        auto hasCrossoverSlope = false;
        for (const auto& child : restoredState)
            hasCrossoverSlope = hasCrossoverSlope
                              || child.getProperty ("id").toString()
                                     == smart_ducking::parameter_ids::crossoverSlope;

        if (! hasCrossoverSlope)
        {
            juce::ValueTree defaultSlope { "PARAM" };
            defaultSlope.setProperty ("id",
                                      smart_ducking::parameter_ids::crossoverSlope,
                                      nullptr);
            defaultSlope.setProperty ("value", 2.0f, nullptr);
            restoredState.appendChild (defaultSlope, nullptr);
        }

        parameters.replaceState (restoredState);
        initialiseComparisonSlots();

        const auto& processorParameters = getParameters();
        for (size_t slot = 0; slot < savedSnapshots.size(); ++slot)
        {
            if (! savedSnapshots[slot].isValid())
                continue;

            for (int index = 0; index < processorParameters.size(); ++index)
                if (const auto* parameter = dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        processorParameters[index]))
                    comparisonSlots[slot][static_cast<size_t> (index)] = static_cast<float> (
                        savedSnapshots[slot].getProperty (
                            parameter->paramID,
                            comparisonSlots[slot][static_cast<size_t> (index)]));
        }

        currentComparisonSlot.store (savedComparisonSlot, std::memory_order_relaxed);
        currentFactoryProgram.store (savedFactoryProgram, std::memory_order_relaxed);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
SmartDuckingAudioProcessor::createParameterLayout()
{
    using namespace smart_ducking::parameter_ids;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (makeFloatParameter (crossoverHz, "Crossover", frequencyRange (50.0f, 180.0f), 92.0f, "Hz"));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { crossoverSlope, parameterVersion },
        "Crossover Slope",
        juce::StringArray { "24 dB/oct", "36 dB/oct", "48 dB/oct",
                            "60 dB/oct", "72 dB/oct" },
        2));
    layout.add (makeFloatParameter (duckDepthDb, "Duck Depth", { 0.0f, 18.0f, 0.01f }, 10.0f, "dB"));
    layout.add (makeFloatParameter (attackMs, "Attack", { 0.0f, 200.0f, 0.1f }, 50.0f, "ms"));
    layout.add (makeFloatParameter (holdMs, "Hold", { 0.0f, 500.0f, 0.1f }, 150.0f, "ms"));
    layout.add (makeFloatParameter (releaseMs, "Release", { 20.0f, 1000.0f, 0.1f, 0.45f }, 200.0f, "ms"));
    layout.add (makeFloatParameter (lookaheadMs, "Lookahead", { 0.0f, 50.0f, 0.1f }, 10.0f, "ms"));
    layout.add (makeFloatParameter (activityThresholdDb, "Voice Full", { -60.0f, 0.0f, 0.1f }, -30.0f, "dBFS"));
    layout.add (makeFloatParameter (spectralAmountDb, "Spectral Amount", { 0.0f, 18.0f, 0.01f }, 10.0f, "dB"));
    layout.add (makeFloatParameter (spectralLowHz, "Spectral Low", frequencyRange (100.0f, 2000.0f), 150.0f, "Hz"));
    layout.add (makeFloatParameter (spectralHighHz, "Spectral High", frequencyRange (4000.0f, 20000.0f), 20000.0f, "Hz"));
    layout.add (makeFloatParameter (weightFrequencyHz, "Weight Frequency", frequencyRange (500.0f, 20000.0f), 3000.0f, "Hz"));
    layout.add (makeFloatParameter (weightGainDb, "Weight Gain", { -12.0f, 12.0f, 0.01f }, 6.0f, "dB"));
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
    const juce::StringArray channelOrderChoices {
        "ITU / SMPTE", "REAPER / VLC", "Film", "DTS / MPEG-D"
    };
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { mainChannelOrder, parameterVersion },
        "Main Channel Order",
        channelOrderChoices,
        0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { voiceChannelOrder, parameterVersion },
        "Voice Channel Order",
        channelOrderChoices,
        0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { outputChannelOrder, parameterVersion },
        "Output Channel Order",
        channelOrderChoices,
        0));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { downmix, parameterVersion }, "Downmix", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { dropLfe, parameterVersion }, "Drop LFE", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { globalBypass, parameterVersion }, "Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { crossoverBypass, parameterVersion }, "Crossover Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { widebandBypass, parameterVersion }, "Wideband Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { spectralBypass, parameterVersion }, "Spectral Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { monitorMode, parameterVersion },
        "Monitor Mode",
        juce::StringArray { "Normal", "Crossover Low", "Crossover High",
                            "Wideband Solo", "Wideband Delta",
                            "Spectral Solo", "Spectral Delta" },
        0));

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
