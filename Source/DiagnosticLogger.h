#pragma once

#include <atomic>
#include <cstdint>

#include <juce_core/juce_core.h>

#ifndef SMART_DUCKING_ENABLE_DIAGNOSTICS
// Set to 1 temporarily when collecting profiler logs. Keeping diagnostics
// compiled out avoids measuring the profiler itself in normal builds.
#define SMART_DUCKING_ENABLE_DIAGNOSTICS 0
#endif

namespace smart_ducking
{
class DiagnosticLogger final : private juce::Thread
{
public:
    DiagnosticLogger();
    ~DiagnosticLogger() override;

    void recordPrepare (double sampleRate,
                        int maximumBlockSize,
                        int fftResolution,
                        int latencySamples) noexcept;
    void recordAudioBlock (int blockSize,
                           bool sidechainActive,
                           bool outputIsFinite,
                           int sidechainMode,
                           std::uint64_t sidechainFftCount,
                           std::uint64_t sidechainPceFrameCount,
                           bool visualisationEnabled,
                           std::uint64_t processBlockTicks,
                           std::uint64_t spectralFrameCount,
                           std::uint64_t spectralFrameTicks,
                           std::uint64_t dryVisualisationCount,
                           std::uint64_t dryVisualisationTicks,
                           std::uint64_t activeVisualisationCount,
                           std::uint64_t activeVisualisationTicks) noexcept;
    void recordSpectralStages (std::uint64_t frameInputTicks,
                               std::uint64_t classificationTicks,
                               std::uint64_t mainAnalysisTicks,
                               std::uint64_t sidechainAnalysisTicks,
                               std::uint64_t controlTicks,
                               std::uint64_t maskTicks,
                               std::uint64_t wideGainTicks,
                               std::uint64_t inverseTicks,
                               std::uint64_t olaTicks) noexcept;
    void recordProcessorStages (std::uint64_t mainDelayTicks,
                                std::uint64_t sidechainDelayTicks,
                                std::uint64_t inputRoutingTicks,
                                std::uint64_t crossoverTicks,
                                std::uint64_t outputRoutingTicks) noexcept;
    void recordResolutionChange (int fftResolution, int latencySamples) noexcept;
    void recordEditorOpened() noexcept;
    void recordEditorClosed() noexcept;
    void recordEditorPaint() noexcept;
    void recordEditorTimer() noexcept;
    void recordEditorPaintDuration (std::uint64_t ticks) noexcept;
    void recordEditorTimerDuration (std::uint64_t ticks) noexcept;
    void recordSpectrumUpdateDuration (std::uint64_t ticks) noexcept;
    void recordSpectrumPaintDuration (std::uint64_t ticks) noexcept;
    void recordSpectrumFrame (std::uint64_t frame,
                              float rawInputMinimumDb,
                              float rawInputMaximumDb,
                              float rawOutputMinimumDb,
                              float rawOutputMaximumDb,
                              float displayInputMinimumDb,
                              float displayInputMaximumDb,
                              float displayOutputMinimumDb,
                              float displayOutputMaximumDb,
                              float displayOffsetDb) noexcept;

    [[nodiscard]] juce::String getLogFilePath() const { return logFile.getFullPathName(); }

private:
    void run() override;
    void writeSnapshot (juce::FileOutputStream&);

    static std::atomic<unsigned int> nextInstanceId;

    juce::File logFile;
    unsigned int instanceId = 0;
    std::atomic<bool> prepared { false };
    std::atomic<bool> editorOpen { false };
    std::atomic<int> sampleRateHz { 0 };
    std::atomic<int> maximumBlockSize { 0 };
    std::atomic<int> lastBlockSize { 0 };
    std::atomic<int> fftResolution { 1 };
    std::atomic<int> latencySamples { 0 };
    std::atomic<std::uint64_t> audioBlocks { 0 };
    std::atomic<std::uint64_t> audioSamples { 0 };
    std::atomic<std::uint64_t> sidechainActiveBlocks { 0 };
    std::atomic<std::uint64_t> nonFiniteBlocks { 0 };
    std::atomic<int> sidechainMode { 0 };
    std::atomic<std::uint64_t> sidechainFftCount { 0 };
    std::atomic<std::uint64_t> sidechainPceFrameCount { 0 };
    std::atomic<std::uint64_t> editorPaints { 0 };
    std::atomic<std::uint64_t> editorTimerTicks { 0 };
    std::atomic<bool> visualisationEnabled { false };
    std::atomic<std::uint64_t> processBlockTicks { 0 };
    std::atomic<std::uint64_t> mainDelayTicks { 0 };
    std::atomic<std::uint64_t> sidechainDelayTicks { 0 };
    std::atomic<std::uint64_t> inputRoutingTicks { 0 };
    std::atomic<std::uint64_t> crossoverTicks { 0 };
    std::atomic<std::uint64_t> outputRoutingTicks { 0 };
    std::atomic<std::uint64_t> spectralFrameCount { 0 };
    std::atomic<std::uint64_t> spectralFrameTicks { 0 };
    std::atomic<std::uint64_t> dryVisualisationCount { 0 };
    std::atomic<std::uint64_t> dryVisualisationTicks { 0 };
    std::atomic<std::uint64_t> activeVisualisationCount { 0 };
    std::atomic<std::uint64_t> activeVisualisationTicks { 0 };
    std::atomic<std::uint64_t> frameInputTicks { 0 };
    std::atomic<std::uint64_t> classificationTicks { 0 };
    std::atomic<std::uint64_t> mainAnalysisTicks { 0 };
    std::atomic<std::uint64_t> sidechainAnalysisTicks { 0 };
    std::atomic<std::uint64_t> controlTicks { 0 };
    std::atomic<std::uint64_t> maskTicks { 0 };
    std::atomic<std::uint64_t> wideGainTicks { 0 };
    std::atomic<std::uint64_t> inverseTicks { 0 };
    std::atomic<std::uint64_t> olaTicks { 0 };
    std::atomic<std::uint64_t> editorPaintDurationTicks { 0 };
    std::atomic<std::uint64_t> editorTimerDurationTicks { 0 };
    std::atomic<std::uint64_t> spectrumUpdateCount { 0 };
    std::atomic<std::uint64_t> spectrumUpdateTicks { 0 };
    std::atomic<std::uint64_t> spectrumPaintCount { 0 };
    std::atomic<std::uint64_t> spectrumPaintTicks { 0 };
    std::atomic<std::uint64_t> spectrumFrame { 0 };
    std::atomic<float> rawInputMinimumDb { -160.0f };
    std::atomic<float> rawInputMaximumDb { -160.0f };
    std::atomic<float> rawOutputMinimumDb { -160.0f };
    std::atomic<float> rawOutputMaximumDb { -160.0f };
    std::atomic<float> displayInputMinimumDb { -60.0f };
    std::atomic<float> displayInputMaximumDb { -60.0f };
    std::atomic<float> displayOutputMinimumDb { -60.0f };
    std::atomic<float> displayOutputMaximumDb { -60.0f };
    std::atomic<float> spectrumDisplayOffsetDb { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiagnosticLogger)
};
} // namespace smart_ducking
