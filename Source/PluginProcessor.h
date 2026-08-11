#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "LinkwitzRiley8Crossover.h"
#include "DiagnosticLogger.h"
#include "LcrDuckMatrix.h"
#include "SidechainEnvelope.h"
#include "SpectralSuppressor.h"

namespace smart_ducking
{
enum class PluginVariant
{
    stereo,
    surround
};

enum class MonitorMode
{
    normal = 0,
    crossoverSolo,
    crossoverDelta,
    widebandSolo,
    widebandDelta,
    spectralSolo,
    spectralDelta
};

PluginVariant exchangeRequestedPluginVariant (PluginVariant) noexcept;
[[nodiscard]] PluginVariant getRequestedPluginVariant() noexcept;
} // namespace smart_ducking

class SmartDuckingAudioProcessor final : public juce::AudioProcessor,
                                         private juce::AudioProcessorValueTreeState::Listener,
                                         private juce::Timer
{
public:
    explicit SmartDuckingAudioProcessor (
        smart_ducking::PluginVariant = smart_ducking::PluginVariant::stereo);
    ~SmartDuckingAudioProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    [[nodiscard]] bool isBusesLayoutSupported (const BusesLayout&) const override;

    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override;
    [[nodiscard]] bool hasEditor() const override { return true; }

    [[nodiscard]] const juce::String getName() const override;
    [[nodiscard]] smart_ducking::PluginVariant getPluginVariant() const noexcept { return variant; }
    [[nodiscard]] bool isSurroundVariant() const noexcept
    {
        return variant == smart_ducking::PluginVariant::surround;
    }
    [[nodiscard]] bool hasPendingDspConfiguration() const noexcept
    {
        return configurationPending.load (std::memory_order_relaxed);
    }
    [[nodiscard]] int getActiveFftResolutionIndex() const noexcept
    {
        return currentResolutionIndex.load (std::memory_order_relaxed);
    }
    [[nodiscard]] bool isActiveNormalMode() const noexcept
    {
        return currentEconomyMode.load (std::memory_order_relaxed);
    }
    [[nodiscard]] bool isHeavyPathActive() const noexcept
    {
        return spectralLcrProcessor.isHeavyPathActive();
    }
    [[nodiscard]] int getOutputMeterChannelCount() const noexcept
    {
        return ! isSurroundVariant()
                || (downmixParameter != nullptr && downmixParameter->load() >= 0.5f)
            ? 2 : 5;
    }
    [[nodiscard]] bool acceptsMidi() const override { return false; }
    [[nodiscard]] bool producesMidi() const override { return false; }
    [[nodiscard]] bool isMidiEffect() const override { return false; }
    // Covers the maximum Hold/Release configuration plus delayed STFT,
    // crossover ringing and overlap-add drain after input becomes silent.
    [[nodiscard]] double getTailLengthSeconds() const override { return 2.0; }

    [[nodiscard]] int getNumPrograms() override;
    [[nodiscard]] int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    [[nodiscard]] const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void switchComparisonSlot (int slot);
    [[nodiscard]] int getComparisonSlot() const noexcept
    {
        return currentComparisonSlot.load (std::memory_order_relaxed);
    }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    [[nodiscard]] juce::AudioProcessorValueTreeState& getParameterState() noexcept
    {
        return parameters;
    }

    [[nodiscard]] const juce::AudioProcessorValueTreeState& getParameterState() const noexcept
    {
        return parameters;
    }

    [[nodiscard]] float getSidechainPeakLeft() const noexcept
    {
        return sidechainPeakLeft.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSidechainPeakRight() const noexcept
    {
        return sidechainPeakRight.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSidechainPeakCentre() const noexcept
    {
        return sidechainPeakCentre.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSidechainPeakLeftSurround() const noexcept
    {
        return sidechainPeakLeftSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSidechainPeakRightSurround() const noexcept
    {
        return sidechainPeakRightSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getMainInputPeakLeft() const noexcept
    {
        return mainInputPeakLeft.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getMainInputPeakRight() const noexcept
    {
        return mainInputPeakRight.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getMainInputPeakCentre() const noexcept
    {
        return mainInputPeakCentre.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getMainInputPeakLeftSurround() const noexcept
    {
        return mainInputPeakLeftSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getMainInputPeakRightSurround() const noexcept
    {
        return mainInputPeakRightSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getOutputPeakLeft() const noexcept
    {
        return outputPeakLeft.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getOutputPeakRight() const noexcept
    {
        return outputPeakRight.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getOutputPeakCentre() const noexcept
    {
        return outputPeakCentre.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getOutputPeakLeftSurround() const noexcept
    {
        return outputPeakLeftSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getOutputPeakRightSurround() const noexcept
    {
        return outputPeakRightSurround.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getGainReductionLeftDb() const noexcept
    {
        return gainReductionLeftDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getGainReductionRightDb() const noexcept
    {
        return gainReductionRightDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getGainReductionCentreDb() const noexcept
    {
        return gainReductionCentreDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getGainReductionLeftSurroundDb() const noexcept
    {
        return gainReductionLeftSurroundDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getGainReductionRightSurroundDb() const noexcept
    {
        return gainReductionRightSurroundDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSpectralReductionLeftDb() const noexcept
    {
        return spectralReductionLeftDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSpectralReductionRightDb() const noexcept
    {
        return spectralReductionRightDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSpectralReductionCentreDb() const noexcept
    {
        return spectralReductionCentreDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSpectralReductionLeftSurroundDb() const noexcept
    {
        return spectralReductionLeftSurroundDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getSpectralReductionRightSurroundDb() const noexcept
    {
        return spectralReductionRightSurroundDb.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getPceLeftPeak() const noexcept
    {
        return pceLeftPeak.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getPceCentrePeak() const noexcept
    {
        return pceCentrePeak.load (std::memory_order_relaxed);
    }

    [[nodiscard]] float getPceRightPeak() const noexcept
    {
        return pceRightPeak.load (std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t copySpectrumDisplay (
        float* inputDbDestination,
        float* outputDbDestination,
        float* reductionDbDestination,
        float* spectralMaskDbDestination,
        int maximumBins) const noexcept
    {
        return spectralLcrProcessor.copyDisplayFrame (inputDbDestination,
                                                       outputDbDestination,
                                                       reductionDbDestination,
                                                       spectralMaskDbDestination,
                                                       maximumBins);
    }

    void notifyEditorOpened() noexcept
    {
        diagnosticLogger.recordEditorOpened();
        spectralLcrProcessor.setVisualisationEnabled (true);
    }

    void setVisualisationFrameRate (float framesPerSecond) noexcept
    {
        spectralLcrProcessor.setVisualisationFrameRate (framesPerSecond);
    }

    void notifyEditorClosed() noexcept
    {
        spectralLcrProcessor.setVisualisationEnabled (false);
        diagnosticLogger.recordEditorClosed();
    }
    void notifyEditorPaint() noexcept { diagnosticLogger.recordEditorPaint(); }
    void notifyEditorTimer() noexcept { diagnosticLogger.recordEditorTimer(); }
    void recordEditorPaintDuration (std::uint64_t ticks) noexcept
    {
        diagnosticLogger.recordEditorPaintDuration (ticks);
    }
    void recordEditorTimerDuration (std::uint64_t ticks) noexcept
    {
        diagnosticLogger.recordEditorTimerDuration (ticks);
    }
    void recordSpectrumUpdateDuration (std::uint64_t ticks) noexcept
    {
        diagnosticLogger.recordSpectrumUpdateDuration (ticks);
    }
    void recordSpectrumPaintDuration (std::uint64_t ticks) noexcept
    {
        diagnosticLogger.recordSpectrumPaintDuration (ticks);
    }
    void recordSpectrumDisplayDiagnostics (std::uint64_t frame,
                                           float rawInputMinimumDb,
                                           float rawInputMaximumDb,
                                           float rawOutputMinimumDb,
                                           float rawOutputMaximumDb,
                                           float displayInputMinimumDb,
                                           float displayInputMaximumDb,
                                           float displayOutputMinimumDb,
                                           float displayOutputMaximumDb,
                                           float displayOffsetDb) noexcept
    {
        diagnosticLogger.recordSpectrumFrame (frame,
                                              rawInputMinimumDb,
                                              rawInputMaximumDb,
                                              rawOutputMinimumDb,
                                              rawOutputMaximumDb,
                                              displayInputMinimumDb,
                                              displayInputMaximumDb,
                                              displayOutputMinimumDb,
                                              displayOutputMaximumDb,
                                              displayOffsetDb);
    }
    [[nodiscard]] juce::String getDiagnosticLogFilePath() const
    {
        return diagnosticLogger.getLogFilePath();
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    static BusesProperties makeBuses (smart_ducking::PluginVariant);
    void parameterChanged (const juce::String&, float) override;
    void timerCallback() override;
    void applyRequestedDspConfiguration();
    [[nodiscard]] int getTotalLatencySamples() const noexcept;
    void updateTransportState();
    static void removeLegacyState (juce::ValueTree&);
    void captureComparisonSlot (int);
    void restoreComparisonSlot (int);
    void initialiseComparisonSlots();

    const smart_ducking::PluginVariant variant;
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* duckDepthParameter = nullptr;
    std::atomic<float>* crossoverParameter = nullptr;
    std::atomic<float>* attackParameter = nullptr;
    std::atomic<float>* holdParameter = nullptr;
    std::atomic<float>* releaseParameter = nullptr;
    std::atomic<float>* lookaheadParameter = nullptr;
    std::atomic<float>* activityThresholdParameter = nullptr;
    std::atomic<float>* spectralAmountParameter = nullptr;
    std::atomic<float>* spectralLowParameter = nullptr;
    std::atomic<float>* spectralHighParameter = nullptr;
    std::atomic<float>* weightFrequencyParameter = nullptr;
    std::atomic<float>* weightGainParameter = nullptr;
    std::atomic<float>* weightQParameter = nullptr;
    std::atomic<float>* fftResolutionParameter = nullptr;
    std::atomic<float>* processingModeParameter = nullptr;
    std::atomic<float>* voiceFormatParameter = nullptr;
    std::atomic<float>* vlcModeParameter = nullptr;
    std::atomic<float>* downmixParameter = nullptr;
    std::atomic<float>* dropLfeParameter = nullptr;
    std::atomic<float>* crossoverBypassParameter = nullptr;
    std::atomic<float>* widebandBypassParameter = nullptr;
    std::atomic<float>* spectralBypassParameter = nullptr;
    std::atomic<float>* monitorModeParameter = nullptr;
    std::array<std::atomic<float>*, 25> matrixParameters {};
    std::array<std::vector<float>, 2> comparisonSlots;
    std::atomic<int> currentComparisonSlot { 0 };
    std::atomic<int> currentFactoryProgram { 0 };

    smart_ducking::LinkwitzRiley8Crossover crossover;
    smart_ducking::StereoSpectralLcrProcessor spectralLcrProcessor;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> lowBandDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>
        mainLookaheadDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>
        dryMonitorDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>
        sidechainLookaheadDelay;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedSidechainDelaySamples;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedQualityCrossoverMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedDeltaMonitorMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedQualityCrossoverDeltaMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedCrossoverDeltaLfeMute;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedVoiceSurroundMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedVlcOrderMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedDownmixMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        smoothedDropLfeMix;
    std::atomic<int> currentResolutionIndex { 1 };
    std::atomic<bool> currentEconomyMode { true };
    std::atomic<bool> configurationPending { false };
    std::atomic<bool> transportIsPlaying { false };
    std::atomic<bool> transportStateKnown { true };
    std::atomic<bool> processorPrepared { false };
    std::atomic<std::uint64_t> processBlockCounter { 0 };
    std::uint64_t lastTimerProcessBlockCounter = 0;
    int consecutiveQuiescentTimerTicks = 0;
    double currentSampleRate = 48000.0;
    int maximumLookaheadSamples = 0;

    std::atomic<float> mainInputPeakLeft { 0.0f };
    std::atomic<float> mainInputPeakRight { 0.0f };
    std::atomic<float> mainInputPeakCentre { 0.0f };
    std::atomic<float> mainInputPeakLeftSurround { 0.0f };
    std::atomic<float> mainInputPeakRightSurround { 0.0f };
    std::atomic<float> outputPeakLeft { 0.0f };
    std::atomic<float> outputPeakRight { 0.0f };
    std::atomic<float> outputPeakCentre { 0.0f };
    std::atomic<float> outputPeakLeftSurround { 0.0f };
    std::atomic<float> outputPeakRightSurround { 0.0f };
    std::atomic<float> sidechainPeakLeft { 0.0f };
    std::atomic<float> sidechainPeakRight { 0.0f };
    std::atomic<float> sidechainPeakCentre { 0.0f };
    std::atomic<float> sidechainPeakLeftSurround { 0.0f };
    std::atomic<float> sidechainPeakRightSurround { 0.0f };
    std::atomic<float> gainReductionLeftDb { 0.0f };
    std::atomic<float> gainReductionCentreDb { 0.0f };
    std::atomic<float> gainReductionRightDb { 0.0f };
    std::atomic<float> gainReductionLeftSurroundDb { 0.0f };
    std::atomic<float> gainReductionRightSurroundDb { 0.0f };
    std::atomic<float> spectralReductionLeftDb { 0.0f };
    std::atomic<float> spectralReductionCentreDb { 0.0f };
    std::atomic<float> spectralReductionRightDb { 0.0f };
    std::atomic<float> spectralReductionLeftSurroundDb { 0.0f };
    std::atomic<float> spectralReductionRightSurroundDb { 0.0f };
    std::atomic<float> pceLeftPeak { 0.0f };
    std::atomic<float> pceCentrePeak { 0.0f };
    std::atomic<float> pceRightPeak { 0.0f };

    smart_ducking::DiagnosticLogger diagnosticLogger;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmartDuckingAudioProcessor)
};
