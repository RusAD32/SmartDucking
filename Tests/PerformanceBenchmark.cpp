#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

#include "ParameterIds.h"
#include "PluginProcessor.h"
#include "RealFft.h"
#include "SpectralVectorMath.h"

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
              bool offlineRendering = false,
              bool surroundVoice = false,
              int crossoverSlopeIndex = 2,
              int measuredBlocks = 2000,
              bool reuseInputBlock = false)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int warmupBlocks = 128;
    SmartDuckingAudioProcessor processor (variant);
    setParameter (processor, smart_ducking::parameter_ids::processingMode,
                  static_cast<float> (processingMode));
    setParameter (processor, smart_ducking::parameter_ids::fftResolution,
                  static_cast<float> (resolution));
    setParameter (processor, smart_ducking::parameter_ids::crossoverSlope,
                  static_cast<float> (crossoverSlopeIndex));
    if (variant == smart_ducking::PluginVariant::surround || surroundVoice)
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

    const auto fillInputBlock = [&]
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
    };

    if (reuseInputBlock)
        fillInputBlock();

    const auto processBlocks = [&] (int blocks)
    {
        for (int block = 0; block < blocks; ++block)
        {
            if (! reuseInputBlock)
                fillInputBlock();
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
              << (surroundVoice ? "-surround-voice" : "")
              << '\t' << scenarioName
              << (scenario == Scenario::crossoverLowMonitor
                      ? "-" + std::to_string (24 + 12 * crossoverSlopeIndex) + "dB-oct"
                      : "")
              << '\t' << std::fixed << std::setprecision (2)
              << realtimeFactor << "x realtime"
              << '\t' << 100.0 / realtimeFactor << "% single-core"
              << '\t' << "heavy=" << (processor.isHeavyPathActive() ? "yes" : "no")
              << '\t' << "steady-blocks=" << processor.getSteadyActiveBlockCount()
              << '\t' << "main-fft=" << processor.getProcessedFftFrameCount()
              << '\t' << "sc-fft=" << processor.getProcessedSidechainFftCount()
              << '\t' << "checksum=" << checksum << '\n';
}

void runCrossoverCase (bool stereo, int slopeIndex, int measuredBlocks)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int warmupBlocks = 128;
    const auto slopeDbPerOctave = 24.0f + 12.0f * static_cast<float> (slopeIndex);
    std::array<std::array<float, blockSize>, 5> input {};

    for (int channel = 0; channel < 5; ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            input[static_cast<size_t> (channel)][static_cast<size_t> (sample)] =
                0.2f * static_cast<float> (std::sin (
                    0.019 * static_cast<double> (sample)
                    + 0.37 * static_cast<double> (channel)));

    const auto measure = [&] (bool blockPath)
    {
        smart_ducking::LinkwitzRileyCrossover crossover;
        crossover.prepare (sampleRate, 92.0f, slopeDbPerOctave);
        std::array<std::array<float, blockSize>, 5> low {};
        std::array<std::array<float, blockSize>, 5> high {};
        std::array<const float*, 5> inputPointers {};
        std::array<float*, 5> lowPointers {};
        std::array<float*, 5> highPointers {};

        for (int channel = 0; channel < 5; ++channel)
        {
            if (stereo && channel != 0 && channel != 2)
                continue;

            const auto index = static_cast<size_t> (channel);
            inputPointers[index] = input[index].data();
            lowPointers[index] = low[index].data();
            highPointers[index] = high[index].data();
        }

        double checksum = 0.0;
        const auto processBlocks = [&] (int blocks)
        {
            for (int block = 0; block < blocks; ++block)
            {
                if (blockPath)
                {
                    crossover.processSteadyBlock (inputPointers,
                                                  lowPointers,
                                                  highPointers,
                                                  blockSize,
                                                  stereo);
                }
                else
                {
                    for (int sample = 0; sample < blockSize; ++sample)
                    {
                        crossover.beginSample();
                        for (int channel = 0; channel < 5; ++channel)
                        {
                            if (stereo && channel != 0 && channel != 2)
                                continue;

                            const auto index = static_cast<size_t> (channel);
                            const auto bands = crossover.processSample (
                                channel, input[index][static_cast<size_t> (sample)]);
                            low[index][static_cast<size_t> (sample)] = bands.low;
                            high[index][static_cast<size_t> (sample)] = bands.high;
                        }
                    }
                }

                checksum += low[0][static_cast<size_t> (block) & (blockSize - 1)];
            }
        };

        processBlocks (warmupBlocks);
        const auto started = std::chrono::steady_clock::now();
        processBlocks (measuredBlocks);
        const auto seconds = std::chrono::duration<double> (
            std::chrono::steady_clock::now() - started).count();
        return std::pair { seconds, checksum };
    };

    const auto generic = measure (false);
    const auto block = measure (true);
    const auto channelCount = stereo ? 2 : 5;
    const auto channelSamples = static_cast<double> (measuredBlocks)
                              * blockSize * channelCount;
    const auto genericNanoseconds = 1.0e9 * generic.first / channelSamples;
    const auto blockNanoseconds = 1.0e9 * block.first / channelSamples;

    std::cout << "Linkwitz-Riley"
              << '\t' << (stereo ? "Stereo" : "Surround")
              << '\t' << static_cast<int> (slopeDbPerOctave) << "dB/oct"
              << '\t' << std::fixed << std::setprecision (3)
              << "generic=" << genericNanoseconds << "ns/channel-sample"
              << '\t' << "block=" << blockNanoseconds << "ns/channel-sample"
              << '\t' << "speedup=" << generic.first / block.first << "x"
              << '\t' << "checksum=" << generic.second + block.second << '\n';
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuceGui;
    std::cout << "FFT backend\t" << smart_ducking::RealFft::getBackendName()
              << '\t' << smart_ducking::RealFft::getBackendArchitecture()
              << '\t' << "SIMD-width=" << smart_ducking::RealFft::getBackendSimdWidth()
              << '\n';
    std::cout << "Vector math backend\t"
              << smart_ducking::spectral_vector_math::getBackendName()
              << '\t' << "SIMD-width="
              << smart_ducking::spectral_vector_math::getBackendSimdWidth()
              << '\n';

    if (argc == 5 && std::string_view (argv[1]) == "--crossover-profile")
    {
        const auto stereo = std::string_view (argv[2]) != "surround";
        const auto slopeIndex = juce::jlimit (0, 4, std::atoi (argv[3]));
        const auto measuredBlocks = juce::jmax (1, std::atoi (argv[4]));
        runCrossoverCase (stereo, slopeIndex, measuredBlocks);
        return 0;
    }

    if ((argc == 6 || argc == 7) && std::string_view (argv[1]) == "--profile")
    {
        const auto variant = std::string_view (argv[2]) == "surround"
            ? smart_ducking::PluginVariant::surround
            : smart_ducking::PluginVariant::stereo;
        const auto processingMode = std::string_view (argv[3]) == "normal" ? 1 : 0;
        const auto resolution = juce::jlimit (0, 2, std::atoi (argv[4]));
        const auto measuredBlocks = juce::jmax (1, std::atoi (argv[5]));
        const auto visualisationEnabled = argc == 7
                                       && std::string_view (argv[6]) == "visualisation-on";
        runCase (variant,
                 processingMode,
                 resolution,
                 visualisationEnabled,
                 Scenario::active,
                 false,
                 false,
                 2,
                 measuredBlocks,
                 true);
        return 0;
    }

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

    runCase (smart_ducking::PluginVariant::stereo,
             1,
             1,
             false,
             Scenario::silentSidechain,
             false,
             true);

    for (const auto variant : { smart_ducking::PluginVariant::stereo,
                                smart_ducking::PluginVariant::surround })
        for (int mode = 0; mode < 2; ++mode)
            for (int slopeIndex = 0; slopeIndex < 5; ++slopeIndex)
                runCase (variant,
                         mode,
                         1,
                         false,
                         Scenario::crossoverLowMonitor,
                         false,
                         false,
                         slopeIndex);
}
