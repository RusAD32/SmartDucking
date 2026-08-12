#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>

namespace smart_ducking
{
enum class SurroundChannelOrder
{
    ituSmpte = 0,
    reaperVlc,
    film,
    dtsMpegD
};

inline constexpr int surroundChannelOrderCount = 4;

inline constexpr std::array<std::array<int, 6>, surroundChannelOrderCount>
    physicalToCanonicalChannel {
        std::array<int, 6> { 0, 1, 2, 3, 4, 5 }, // L R C LFE Ls Rs
        std::array<int, 6> { 0, 1, 4, 5, 2, 3 }, // L R Ls Rs C LFE
        std::array<int, 6> { 0, 2, 1, 4, 5, 3 }, // L C R Ls Rs LFE
        std::array<int, 6> { 2, 0, 1, 4, 5, 3 }  // C L R Ls Rs LFE
    };

inline constexpr std::array<const char*, surroundChannelOrderCount> surroundChannelOrderNames {
    "ITU / SMPTE", "REAPER / VLC", "Film", "DTS / MPEG-D"
};

inline constexpr std::array<const char*, surroundChannelOrderCount> surroundChannelOrderPreviews {
    "L R C LFE Ls Rs",
    "L R Ls Rs C LFE",
    "L C R Ls Rs LFE",
    "C L R Ls Rs LFE"
};

inline SurroundChannelOrder sanitiseSurroundChannelOrder (float rawValue) noexcept
{
    return static_cast<SurroundChannelOrder> (juce::jlimit (
        0, surroundChannelOrderCount - 1, juce::roundToInt (rawValue)));
}

inline const std::array<int, 6>& getPhysicalToCanonicalChannels (
    SurroundChannelOrder order) noexcept
{
    return physicalToCanonicalChannel[static_cast<size_t> (order)];
}

inline std::array<float, 6> mapSurroundInputToCanonical (
    const std::array<float, 6>& physical,
    SurroundChannelOrder order) noexcept
{
    std::array<float, 6> canonical {};
    const auto& channelMap = getPhysicalToCanonicalChannels (order);

    for (size_t physicalChannel = 0; physicalChannel < physical.size(); ++physicalChannel)
        canonical[static_cast<size_t> (channelMap[physicalChannel])] = physical[physicalChannel];

    return canonical;
}

inline std::array<float, 6> mapSurroundOutputFromCanonical (
    const std::array<float, 6>& canonical,
    SurroundChannelOrder order) noexcept
{
    std::array<float, 6> physical {};
    const auto& channelMap = getPhysicalToCanonicalChannels (order);

    for (size_t physicalChannel = 0; physicalChannel < physical.size(); ++physicalChannel)
        physical[physicalChannel] = canonical[static_cast<size_t> (channelMap[physicalChannel])];

    return physical;
}

inline std::array<int, 5> getMeterChannelsForOrder (SurroundChannelOrder order) noexcept
{
    constexpr std::array<int, 6> canonicalToMeter { 0, 2, 1, -1, 3, 4 };
    std::array<int, 5> result {};
    size_t resultIndex = 0;

    for (const auto canonicalChannel : getPhysicalToCanonicalChannels (order))
        if (const auto meterChannel = canonicalToMeter[static_cast<size_t> (canonicalChannel)];
            meterChannel >= 0)
            result[resultIndex++] = meterChannel;

    return result;
}
} // namespace smart_ducking
