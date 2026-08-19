#include "DiagnosticLogger.h"

namespace smart_ducking
{
std::atomic<unsigned int> DiagnosticLogger::nextInstanceId { 1 };

DiagnosticLogger::DiagnosticLogger()
    : juce::Thread ("Smart Ducking diagnostics"),
      instanceId (nextInstanceId.fetch_add (1, std::memory_order_relaxed))
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto fileName = "SmartDucking-"
                        + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S")
                        + "-instance" + juce::String (instanceId) + ".log";
    const auto fallbackDirectory = juce::File::getSpecialLocation (
        juce::File::userApplicationDataDirectory).getFullPathName();
    logFile = juce::File (juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA",
                                                                      fallbackDirectory))
                  .getChildFile ("RusAD")
                  .getChildFile ("SmartDucking")
                  .getChildFile (fileName);
    startThread (juce::Thread::Priority::low);
#endif
}

DiagnosticLogger::~DiagnosticLogger()
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    signalThreadShouldExit();
    notify();
    stopThread (1500);
#endif
}

void DiagnosticLogger::recordPrepare (double newSampleRate,
                                      int newMaximumBlockSize,
                                      int newFftResolution,
                                      int newLatencySamples) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    sampleRateHz.store (juce::roundToInt (newSampleRate), std::memory_order_relaxed);
    maximumBlockSize.store (newMaximumBlockSize, std::memory_order_relaxed);
    fftResolution.store (newFftResolution, std::memory_order_relaxed);
    latencySamples.store (newLatencySamples, std::memory_order_relaxed);
    prepared.store (true, std::memory_order_release);
#else
    juce::ignoreUnused (newSampleRate,
                        newMaximumBlockSize,
                        newFftResolution,
                        newLatencySamples);
#endif
}

void DiagnosticLogger::recordAudioBlock (int newBlockSize,
                                         bool sidechainActive,
                                         bool outputIsFinite,
                                         int newSidechainMode,
                                         std::uint64_t newSidechainFftCount,
                                         std::uint64_t newSidechainPceFrameCount,
                                         bool newVisualisationEnabled,
                                         std::uint64_t newProcessBlockTicks,
                                         std::uint64_t newSpectralFrameCount,
                                         std::uint64_t newSpectralFrameTicks,
                                         std::uint64_t newDryVisualisationCount,
                                         std::uint64_t newDryVisualisationTicks,
                                         std::uint64_t newActiveVisualisationCount,
                                         std::uint64_t newActiveVisualisationTicks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    lastBlockSize.store (newBlockSize, std::memory_order_relaxed);
    audioBlocks.fetch_add (1, std::memory_order_relaxed);
    audioSamples.fetch_add (static_cast<std::uint64_t> (juce::jmax (0, newBlockSize)),
                            std::memory_order_relaxed);

    if (sidechainActive)
        sidechainActiveBlocks.fetch_add (1, std::memory_order_relaxed);

    if (! outputIsFinite)
        nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);

    sidechainMode.store (newSidechainMode, std::memory_order_relaxed);
    sidechainFftCount.store (newSidechainFftCount, std::memory_order_relaxed);
    sidechainPceFrameCount.store (newSidechainPceFrameCount, std::memory_order_relaxed);
    visualisationEnabled.store (newVisualisationEnabled, std::memory_order_relaxed);
    processBlockTicks.fetch_add (newProcessBlockTicks, std::memory_order_relaxed);
    spectralFrameCount.store (newSpectralFrameCount, std::memory_order_relaxed);
    spectralFrameTicks.store (newSpectralFrameTicks, std::memory_order_relaxed);
    dryVisualisationCount.store (newDryVisualisationCount, std::memory_order_relaxed);
    dryVisualisationTicks.store (newDryVisualisationTicks, std::memory_order_relaxed);
    activeVisualisationCount.store (newActiveVisualisationCount, std::memory_order_relaxed);
    activeVisualisationTicks.store (newActiveVisualisationTicks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (newBlockSize,
                        sidechainActive,
                        outputIsFinite,
                        newSidechainMode,
                        newSidechainFftCount,
                        newSidechainPceFrameCount,
                        newVisualisationEnabled,
                        newProcessBlockTicks,
                        newSpectralFrameCount,
                        newSpectralFrameTicks,
                        newDryVisualisationCount,
                        newDryVisualisationTicks,
                        newActiveVisualisationCount,
                        newActiveVisualisationTicks);
#endif
}

void DiagnosticLogger::recordSpectralStages (std::uint64_t newFrameInputTicks,
                                              std::uint64_t newClassificationTicks,
                                              std::uint64_t newMainAnalysisTicks,
                                              std::uint64_t newSidechainAnalysisTicks,
                                              std::uint64_t newControlTicks,
                                              std::uint64_t newMaskTicks,
                                              std::uint64_t newWideGainTicks,
                                              std::uint64_t newInverseTicks,
                                              std::uint64_t newOlaTicks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    frameInputTicks.store (newFrameInputTicks, std::memory_order_relaxed);
    classificationTicks.store (newClassificationTicks, std::memory_order_relaxed);
    mainAnalysisTicks.store (newMainAnalysisTicks, std::memory_order_relaxed);
    sidechainAnalysisTicks.store (newSidechainAnalysisTicks, std::memory_order_relaxed);
    controlTicks.store (newControlTicks, std::memory_order_relaxed);
    maskTicks.store (newMaskTicks, std::memory_order_relaxed);
    wideGainTicks.store (newWideGainTicks, std::memory_order_relaxed);
    inverseTicks.store (newInverseTicks, std::memory_order_relaxed);
    olaTicks.store (newOlaTicks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (newFrameInputTicks,
                        newClassificationTicks,
                        newMainAnalysisTicks,
                        newSidechainAnalysisTicks,
                        newControlTicks,
                        newMaskTicks,
                        newWideGainTicks,
                        newInverseTicks,
                        newOlaTicks);
#endif
}

void DiagnosticLogger::recordResolutionChange (int newFftResolution,
                                                int newLatencySamples) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    fftResolution.store (newFftResolution, std::memory_order_relaxed);
    latencySamples.store (newLatencySamples, std::memory_order_relaxed);
#else
    juce::ignoreUnused (newFftResolution, newLatencySamples);
#endif
}

void DiagnosticLogger::recordEditorOpened() noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorOpen.store (true, std::memory_order_release);
#endif
}

void DiagnosticLogger::recordEditorClosed() noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorOpen.store (false, std::memory_order_release);
#endif
}

void DiagnosticLogger::recordEditorPaint() noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorPaints.fetch_add (1, std::memory_order_relaxed);
#endif
}

void DiagnosticLogger::recordEditorTimer() noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorTimerTicks.fetch_add (1, std::memory_order_relaxed);
#endif
}

void DiagnosticLogger::recordEditorPaintDuration (std::uint64_t ticks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorPaintDurationTicks.fetch_add (ticks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (ticks);
#endif
}

void DiagnosticLogger::recordEditorTimerDuration (std::uint64_t ticks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    editorTimerDurationTicks.fetch_add (ticks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (ticks);
#endif
}

void DiagnosticLogger::recordSpectrumUpdateDuration (std::uint64_t ticks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    spectrumUpdateCount.fetch_add (1, std::memory_order_relaxed);
    spectrumUpdateTicks.fetch_add (ticks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (ticks);
#endif
}

void DiagnosticLogger::recordSpectrumPaintDuration (std::uint64_t ticks) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    spectrumPaintCount.fetch_add (1, std::memory_order_relaxed);
    spectrumPaintTicks.fetch_add (ticks, std::memory_order_relaxed);
#else
    juce::ignoreUnused (ticks);
#endif
}

void DiagnosticLogger::recordSpectrumFrame (std::uint64_t newFrame,
                                            float newRawInputMinimumDb,
                                            float newRawInputMaximumDb,
                                            float newRawOutputMinimumDb,
                                            float newRawOutputMaximumDb,
                                            float newDisplayInputMinimumDb,
                                            float newDisplayInputMaximumDb,
                                            float newDisplayOutputMinimumDb,
                                            float newDisplayOutputMaximumDb,
                                            float newDisplayOffsetDb) noexcept
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    spectrumFrame.store (newFrame, std::memory_order_relaxed);
    rawInputMinimumDb.store (newRawInputMinimumDb, std::memory_order_relaxed);
    rawInputMaximumDb.store (newRawInputMaximumDb, std::memory_order_relaxed);
    rawOutputMinimumDb.store (newRawOutputMinimumDb, std::memory_order_relaxed);
    rawOutputMaximumDb.store (newRawOutputMaximumDb, std::memory_order_relaxed);
    displayInputMinimumDb.store (newDisplayInputMinimumDb, std::memory_order_relaxed);
    displayInputMaximumDb.store (newDisplayInputMaximumDb, std::memory_order_relaxed);
    displayOutputMinimumDb.store (newDisplayOutputMinimumDb, std::memory_order_relaxed);
    displayOutputMaximumDb.store (newDisplayOutputMaximumDb, std::memory_order_relaxed);
    spectrumDisplayOffsetDb.store (newDisplayOffsetDb, std::memory_order_relaxed);
#else
    juce::ignoreUnused (newFrame,
                        newRawInputMinimumDb,
                        newRawInputMaximumDb,
                        newRawOutputMinimumDb,
                        newRawOutputMaximumDb,
                        newDisplayInputMinimumDb,
                        newDisplayInputMaximumDb,
                        newDisplayOutputMinimumDb,
                        newDisplayOutputMaximumDb,
                        newDisplayOffsetDb);
#endif
}

void DiagnosticLogger::run()
{
    const auto parent = logFile.getParentDirectory();

    if (! parent.createDirectory().wasOk())
        return;

    juce::FileOutputStream stream (logFile, 8192);

    if (! stream.openedOk())
        return;

    stream.setPosition (logFile.getSize());
    stream.writeText ("timestamp\tinstance\tprepared\tsample_rate\tmax_block\tlast_block\tfft_mode"
                      "\tlatency\taudio_blocks\taudio_samples\tsc_active_blocks\tnonfinite_blocks"
                      "\tsc_mode\tsc_fft_count\tsc_pce_frame_count"
                      "\teditor_open\tpaint_count\ttimer_count"
                      "\thi_res_ticks_per_second\tvisualisation_enabled"
                      "\tprocess_block_ticks\tspectral_frame_count\tspectral_frame_ticks"
                      "\tframe_input_ticks\tclassification_ticks"
                      "\tmain_analysis_ticks\tsidechain_analysis_ticks"
                      "\tcontrol_ticks\tmask_ticks\twide_gain_ticks"
                      "\tinverse_ticks\tola_ticks"
                      "\tdry_visualisation_count\tdry_visualisation_ticks"
                      "\tactive_visualisation_count\tactive_visualisation_ticks"
                      "\teditor_paint_duration_ticks\teditor_timer_duration_ticks"
                      "\tspectrum_update_count\tspectrum_update_ticks"
                      "\tspectrum_paint_count\tspectrum_paint_ticks"
                      "\tspectrum_frame\traw_in_min_db\traw_in_max_db"
                      "\traw_out_min_db\traw_out_max_db"
                      "\tdisplay_in_min_db\tdisplay_in_max_db"
                      "\tdisplay_out_min_db\tdisplay_out_max_db"
                      "\tspectrum_offset_db\n",
                      false,
                      false,
                      nullptr);
    stream.flush();

    while (! threadShouldExit())
    {
        writeSnapshot (stream);
        stream.flush();
        wait (1000.0);
    }

    writeSnapshot (stream);
    stream.flush();
}

void DiagnosticLogger::writeSnapshot (juce::FileOutputStream& stream)
{
    const auto line = juce::Time::getCurrentTime().toISO8601 (true)
                    + "\t" + juce::String (instanceId)
                    + "\t" + juce::String (prepared.load (std::memory_order_acquire) ? 1 : 0)
                    + "\t" + juce::String (sampleRateHz.load (std::memory_order_relaxed))
                    + "\t" + juce::String (maximumBlockSize.load (std::memory_order_relaxed))
                    + "\t" + juce::String (lastBlockSize.load (std::memory_order_relaxed))
                    + "\t" + juce::String (fftResolution.load (std::memory_order_relaxed))
                    + "\t" + juce::String (latencySamples.load (std::memory_order_relaxed))
                    + "\t" + juce::String (audioBlocks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (audioSamples.load (std::memory_order_relaxed))
                    + "\t" + juce::String (sidechainActiveBlocks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (nonFiniteBlocks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (sidechainMode.load (std::memory_order_relaxed))
                    + "\t" + juce::String (sidechainFftCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (sidechainPceFrameCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (editorOpen.load (std::memory_order_acquire) ? 1 : 0)
                    + "\t" + juce::String (editorPaints.load (std::memory_order_relaxed))
                    + "\t" + juce::String (editorTimerTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (juce::Time::getHighResolutionTicksPerSecond())
                    + "\t" + juce::String (visualisationEnabled.load (std::memory_order_relaxed) ? 1 : 0)
                    + "\t" + juce::String (processBlockTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectralFrameCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectralFrameTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (frameInputTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (classificationTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (mainAnalysisTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (sidechainAnalysisTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (controlTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (maskTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (wideGainTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (inverseTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (olaTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (dryVisualisationCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (dryVisualisationTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (activeVisualisationCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (activeVisualisationTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (editorPaintDurationTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (editorTimerDurationTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectrumUpdateCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectrumUpdateTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectrumPaintCount.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectrumPaintTicks.load (std::memory_order_relaxed))
                    + "\t" + juce::String (spectrumFrame.load (std::memory_order_relaxed))
                    + "\t" + juce::String (rawInputMinimumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (rawInputMaximumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (rawOutputMinimumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (rawOutputMaximumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (displayInputMinimumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (displayInputMaximumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (displayOutputMinimumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (displayOutputMaximumDb.load (std::memory_order_relaxed), 3)
                    + "\t" + juce::String (spectrumDisplayOffsetDb.load (std::memory_order_relaxed), 3)
                    + "\n";
    stream.writeText (line, false, false, nullptr);
}
} // namespace smart_ducking
