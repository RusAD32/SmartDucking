#pragma once

#include <algorithm>
#include <array>

namespace smart_ducking
{
struct LcrDuckMatrix
{
    struct Controls
    {
        float left = 0.0f;
        float centre = 0.0f;
        float right = 0.0f;
        float leftSurround = 0.0f;
        float rightSurround = 0.0f;

        [[nodiscard]] float get (int channel) const noexcept
        {
            switch (channel)
            {
                case 0: return left;
                case 1: return centre;
                case 2: return right;
                case 3: return leftSurround;
                case 4: return rightSurround;
                default: return 0.0f;
            }
        }

        void set (int channel, float value) noexcept
        {
            switch (channel)
            {
                case 0: left = value; break;
                case 1: centre = value; break;
                case 2: right = value; break;
                case 3: leftSurround = value; break;
                case 4: rightSurround = value; break;
                default: break;
            }
        }
    };

    static constexpr int numChannels = 5;

    [[nodiscard]] static constexpr int mirroredChannel (int channel) noexcept
    {
        constexpr std::array<int, numChannels> mirrors { 2, 1, 0, 4, 3 };
        return channel >= 0 && channel < numChannels
            ? mirrors[static_cast<size_t> (channel)]
            : channel;
    }

    [[nodiscard]] static constexpr size_t mirroredCoefficientIndex (
        int voiceChannel,
        int mainChannel) noexcept
    {
        return static_cast<size_t> (mirroredChannel (voiceChannel) * numChannels
                                   + mirroredChannel (mainChannel));
    }

    // Rows are Voice L/C/R/Ls/Rs; columns are Main L/C/R/Ls/Rs.
    std::array<float, numChannels * numChannels> coefficients {
        1.00f, 0.25f, 0.00f, 0.35f, 0.00f,
        0.10f, 1.00f, 0.10f, 0.05f, 0.05f,
        0.00f, 0.25f, 1.00f, 0.00f, 0.35f,
        0.35f, 0.00f, 0.00f, 1.00f, 0.05f,
        0.00f, 0.00f, 0.35f, 0.05f, 1.00f
    };

    [[nodiscard]] Controls apply (Controls detectors) const noexcept
    {
        Controls result;

        for (int target = 0; target < numChannels; ++target)
        {
            float maximum = 0.0f;

            for (int detector = 0; detector < numChannels; ++detector)
                maximum = std::max (
                    maximum,
                    std::clamp (coefficients[static_cast<size_t> (detector * numChannels + target)],
                                0.0f,
                                1.0f)
                        * std::clamp (detectors.get (detector), 0.0f, 1.0f));

            result.set (target, maximum);
        }

        return result;
    }
};
} // namespace smart_ducking
