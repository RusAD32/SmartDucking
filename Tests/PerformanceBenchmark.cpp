#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>

#include "ParameterIds.h"
#include "PluginProcessor.h"

namespace
{
enum class Scenario
{
    active,
    silentSidechain,
    stagesBypassed,
    globalBypass,
    crossoverLowMonitor
};

void setParameter (SmartDuckingAudioProcessor& processor,
                   const char* id,
                   float rawValue)
{
    if (auto* parameter = processor.getParameterState().getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (rawValue));
}

void runCase (smart_ducking::PluginVariant variant,
              int processingMode,
              int resolution,
              bool visualisationEnabled,
              Scenario scenario = Scenario::active,
              bool offlineRendering = false)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int warmupBlocks = 128;
    constexpr int measuredBlocks = 2000;
    SmartDuckingAudioProcessor processor (variant);
    setParameter (processor, smart_ducking::parameter_ids::processingMode,
                  static_cast<float> (processingMode));
    setParameter (processor, smart_ducking::parameter_ids::fftResolution,
                  static_cast<float> (resolution));
    if (variant == smart_ducking::PluginVariant::surround)
        setParameter (processor, smart_ducking::parameter_ids::voiceFormat, 1.0f);
    if (scenario == Scenario::stagesBypassed)
    {
        setParameter (processor, smart_ducking::parameter_ids::crossoverBypass, 1.0f);
        setParameter (processor, smart_ducking::parameter_ids::widebandBypass, 1.0f);
        setParameter (processor, smart_ducking::parameter_ids::spectralBypass, 1.0f);
    }
    if (scenario == Scenario::globalBypass)
        setParameter (processor, smart_ducking::parameter_ids::globalBypass, 1.0f);
    if (scenario == Scenario::crossoverLowMonitor)
        setParameter (processor,
                      smart_ducking::parameter_ids::monitorMode,
                      static_cast<float> (smart_ducking::MonitorMode::crossoverLow));
    processor.prepareToPlay (sampleRate, blockSize);
    if (visualisationEnabled)
        processor.notifyEditorOpened();
    processor.setNonRealtime (offlineRendering);
    const auto mainChannels = processor.getChannelCountOfBus (true, 0);
    juce::AudioBuffer<float> buffer (mainChannels + 6, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    double voicePhase = 0.0;
    const auto phaseIncrement = juce::MathConstants<double>::twoPi * 997.0 / sampleRate;
    const auto voicePhaseIncrement = juce::MathConstants<double>::twoPi * 283.0 / sampleRate;
    double checksum = 0.0;

    const auto processBlocks = [&] (int blocks)
    {
        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto signal = 0.2f * static_cast<float> (std::sin (phase));
                phase += phaseIncrement;
                const auto voiceSignal = 0.25f * static_cast<float> (std::sin (voicePhase));
                voicePhase += voicePhaseIncrement;
                for (int channel = 0; channel < mainChannels; ++channel)
                    buffer.setSample (channel, sample,
                                      signal * (1.0f - 0.05f * static_cast<float> (channel)));
                if (scenario != Scenario::silentSidechain)
                    for (int channel = 0; channel < 6; ++channel)
                        buffer.setSample (
                            mainChannels + channel,
                            sample,
                            voiceSignal * (1.0f - 0.08f * static_cast<float> (channel)));
            }
            processor.processBlock (buffer, midi);
            checksum += buffer.getSample (0, blockSize - 1);
        }
    };

    processBlocks (warmupBlocks);
    const auto started = std::chrono::steady_clock::now();
    processBlocks (measuredBlocks);
    const auto elapsed = std::chrono::duration<double> (
        std::chrono::steady_clock::now() - started).count();
    const auto audioDuration = measuredBlocks * blockSize / sampleRate;
    const auto realtimeFactor = audioDuration / elapsed;
    const auto scenarioName = [scenario]
    {
        switch (scenario)
        {
            case Scenario::active: return "active";
            case Scenario::silentSidechain: return "silent-sidechain";
            case Scenario::stagesBypassed: return "stages-bypassed";
            case Scenario::globalBypass: return "global-bypass";
            case Scenario::crossoverLowMonitor: return "crossover-low-monitor";
        }

        return "unknown";
    }();

    std::cout << (variant == smart_ducking::PluginVariant::stereo ? "Stereo" : "Surround")
              << '\t' << (processingMode == 0 ? "QUALITY" : "NORMAL")
              << '\t' << "x" << (1 << resolution)
              << '\t' << (visualisationEnabled ? "visualisation-on" : "visualisation-off")
              << (offlineRendering ? "-offline" : "")
              << '\t' << scenarioName
              << '\t' << std::fixed << std::setprecision (2)
              << realtimeFactor << "x realtime"
              << '\t' << 100.0 / realtimeFactor << "% single-core"
              << '\t' << "heavy=" << (processor.isHeavyPathActive() ? "yes" : "no")
              << '\t' << "main-fft=" << processor.getProcessedFftFrameCount()
              << '\t' << "sc-fft=" << processor.getProcessedSidechainFftCount()
              << '\t' << "checksum=" << checksum << '\n';
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuceGui;
    for (const auto variant : { smart_ducking::PluginVariant::stereo,
                                smart_ducking::PluginVariant::surround })
        for (int mode = 0; mode < 2; ++mode)
            for (int resolution = 0; resolution < 3; ++resolution)
                for (const auto visualisationEnabled : { false, true })
                    runCase (variant, mode, resolution, visualisationEnabled);

    for (const auto variant : { smart_ducking::PluginVariant::stereo,
                                smart_ducking::PluginVariant::surround })
        for (const auto scenario : { Scenario::silentSidechain,
                                     Scenario::stagesBypassed,
                                     Scenario::globalBypass })
            runCase (variant, 1, 1, false, scenario);

    runCase (smart_ducking::PluginVariant::surround,
             1,
             1,
             true,
             Scenario::silentSidechain,
             true);

    for (const auto variant : { smart_ducking::PluginVariant::stereo,
                                smart_ducking::PluginVariant::surround })
        for (int mode = 0; mode < 2; ++mode)
            runCase (variant, mode, 1, false, Scenario::crossoverLowMonitor);
}
