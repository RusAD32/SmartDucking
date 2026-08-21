#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <vector>

#include <juce_core/juce_core.h>

namespace smart_ducking
{
// Integer-only delay storage for the fixed PDC paths.  The backing size is
// rounded up to a power of two, trading a small amount of memory for mask-based
// wrapping and contiguous block copies.  All allocation happens in prepare().
class PowerOfTwoDelayBuffer
{
public:
    struct TwoTapOutput
    {
        float first = 0.0f;
        float second = 0.0f;
    };

    void prepare (int numberOfChannels, int maximumDelaySamples)
    {
        jassert (numberOfChannels > 0 && maximumDelaySamples >= 0);
        const auto minimumSize = static_cast<unsigned> (
            juce::jmax (2, maximumDelaySamples + 1));
        ringSize = static_cast<size_t> (std::bit_ceil (minimumSize));
        ringMask = ringSize - 1;
        maximumDelay = maximumDelaySamples;
        storage.resize (static_cast<size_t> (numberOfChannels));
        writePositions.resize (static_cast<size_t> (numberOfChannels));

        for (auto& channel : storage)
            channel.resize (ringSize);

        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : storage)
            std::fill (channel.begin(), channel.end(), 0.0f);

        std::fill (writePositions.begin(), writePositions.end(), size_t { 0 });
    }

    [[nodiscard]] float processSample (int channel,
                                       float input,
                                       int delaySamples) noexcept
    {
        const auto index = static_cast<size_t> (channel);
        jassert (index < storage.size());
        jassert (delaySamples >= 0 && delaySamples <= maximumDelay);
        auto& position = writePositions[index];
        auto& samples = storage[index];
        samples[position] = input;
        const auto output = samples[(position - static_cast<size_t> (delaySamples))
                                    & ringMask];
        position = (position + 1) & ringMask;
        return output;
    }

    [[nodiscard]] TwoTapOutput processSample (int channel,
                                              float input,
                                              int firstDelaySamples,
                                              int secondDelaySamples) noexcept
    {
        const auto index = static_cast<size_t> (channel);
        jassert (index < storage.size());
        jassert (firstDelaySamples >= 0 && firstDelaySamples <= maximumDelay);
        jassert (secondDelaySamples >= 0 && secondDelaySamples <= maximumDelay);
        auto& position = writePositions[index];
        auto& samples = storage[index];
        samples[position] = input;
        const TwoTapOutput output {
            samples[(position - static_cast<size_t> (firstDelaySamples)) & ringMask],
            samples[(position - static_cast<size_t> (secondDelaySamples)) & ringMask]
        };
        position = (position + 1) & ringMask;
        return output;
    }

    void processBlock (int channel,
                       const float* input,
                       float* output,
                       int numSamples,
                       int delaySamples) noexcept
    {
        if (numSamples <= 0)
            return;

        const auto index = static_cast<size_t> (channel);
        jassert (index < storage.size());
        jassert (input != nullptr && output != nullptr);
        jassert (delaySamples >= 0 && delaySamples <= maximumDelay);

        if (delaySamples == 0)
        {
            std::copy_n (input, numSamples, output);
            for (int sample = 0; sample < numSamples; ++sample)
                static_cast<void> (processSample (channel, input[sample], 0));
            return;
        }

        auto& position = writePositions[index];
        auto& samples = storage[index];
        auto processed = 0;

        while (processed < numSamples)
        {
            const auto readPosition =
                (position - static_cast<size_t> (delaySamples)) & ringMask;
            const auto remaining = static_cast<size_t> (numSamples - processed);
            const auto chunk = std::min ({
                remaining,
                ringSize - position,
                ringSize - readPosition,
                static_cast<size_t> (delaySamples)
            });
            std::copy_n (samples.data() + readPosition,
                         chunk,
                         output + processed);
            std::copy_n (input + processed,
                         chunk,
                         samples.data() + position);
            position = (position + chunk) & ringMask;
            processed += static_cast<int> (chunk);
        }
    }

    void processBlockTwoTap (int channel,
                             const float* input,
                             float* firstOutput,
                             float* secondOutput,
                             int numSamples,
                             int firstDelaySamples,
                             int secondDelaySamples) noexcept
    {
        if (numSamples <= 0)
            return;

        const auto index = static_cast<size_t> (channel);
        jassert (index < storage.size());
        jassert (input != nullptr && firstOutput != nullptr && secondOutput != nullptr);
        jassert (firstOutput != secondOutput);
        jassert (firstDelaySamples >= 0 && firstDelaySamples <= maximumDelay);
        jassert (secondDelaySamples >= 0 && secondDelaySamples <= maximumDelay);

        auto& position = writePositions[index];
        auto& samples = storage[index];
        auto processed = 0;

        while (processed < numSamples)
        {
            const auto firstReadPosition =
                (position - static_cast<size_t> (firstDelaySamples)) & ringMask;
            const auto secondReadPosition =
                (position - static_cast<size_t> (secondDelaySamples)) & ringMask;
            const auto remaining = static_cast<size_t> (numSamples - processed);
            auto chunk = std::min (remaining, ringSize - position);

            if (firstDelaySamples > 0)
                chunk = std::min ({ chunk,
                                    ringSize - firstReadPosition,
                                    static_cast<size_t> (firstDelaySamples) });

            if (secondDelaySamples > 0)
                chunk = std::min ({ chunk,
                                    ringSize - secondReadPosition,
                                    static_cast<size_t> (secondDelaySamples) });

            // Capture the source before writing either destination. This keeps
            // the operation safe when one output aliases the input buffer.
            std::copy_n (input + processed, chunk, samples.data() + position);
            std::copy_n (samples.data() + firstReadPosition,
                         chunk,
                         firstOutput + processed);
            std::copy_n (samples.data() + secondReadPosition,
                         chunk,
                         secondOutput + processed);
            position = (position + chunk) & ringMask;
            processed += static_cast<int> (chunk);
        }
    }

    [[nodiscard]] size_t getAllocatedSamples() const noexcept
    {
        return ringSize * storage.size();
    }

private:
    std::vector<std::vector<float>> storage;
    std::vector<size_t> writePositions;
    size_t ringSize = 2;
    size_t ringMask = 1;
    int maximumDelay = 0;
};
} // namespace smart_ducking
