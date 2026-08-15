#pragma once

#include <array>

namespace smart_ducking
{
struct FactoryPresetValue
{
    const char* parameterId;
    float rawValue;
};

struct FactoryPreset
{
    const char* name;
    std::array<FactoryPresetValue, 8> values;
};

inline constexpr std::array<FactoryPreset, 4> factoryPresets {
    FactoryPreset { "Default", {{
        { "duckDepthDb", 10.0f }, { "spectralAmountDb", 10.0f },
        { "activityThresholdDb", -30.0f }, { "attackMs", 50.0f },
        { "holdMs", 150.0f }, { "releaseMs", 200.0f },
        { "lookaheadMs", 10.0f }, { "weightGainDb", 6.0f }
    }} },
    FactoryPreset { "Gentle Dialogue", {{
        { "duckDepthDb", 8.0f }, { "spectralAmountDb", 5.0f },
        { "activityThresholdDb", -24.0f }, { "attackMs", 70.0f },
        { "holdMs", 120.0f }, { "releaseMs", 320.0f },
        { "lookaheadMs", 8.0f }, { "weightGainDb", 4.0f }
    }} },
    FactoryPreset { "Voice Focus", {{
        { "duckDepthDb", 15.0f }, { "spectralAmountDb", 11.0f },
        { "activityThresholdDb", -32.0f }, { "attackMs", 35.0f },
        { "holdMs", 170.0f }, { "releaseMs", 240.0f },
        { "lookaheadMs", 12.0f }, { "weightGainDb", 7.0f }
    }} },
    FactoryPreset { "Fast Announcer", {{
        { "duckDepthDb", 12.0f }, { "spectralAmountDb", 7.0f },
        { "activityThresholdDb", -24.0f }, { "attackMs", 10.0f },
        { "holdMs", 60.0f }, { "releaseMs", 120.0f },
        { "lookaheadMs", 20.0f }, { "weightGainDb", 5.0f }
    }} }
};
} // namespace smart_ducking
