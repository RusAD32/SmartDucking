#pragma once

#include <algorithm>
#include <vector>

namespace smart_ducking
{
struct PceHistory
{
    void resize (int bins)
    {
        const auto size = static_cast<size_t> (std::max (0, bins));
        crossReal.resize (size);
        crossImaginary.resize (size);
        powerLeft.resize (size);
        powerRight.resize (size);
        rawCentreFactor.resize (size);
    }

    void clear() noexcept
    {
        std::fill (crossReal.begin(), crossReal.end(), 0.0f);
        std::fill (crossImaginary.begin(), crossImaginary.end(), 0.0f);
        std::fill (powerLeft.begin(), powerLeft.end(), 0.0f);
        std::fill (powerRight.begin(), powerRight.end(), 0.0f);
    }

    std::vector<float> crossReal;
    std::vector<float> crossImaginary;
    std::vector<float> powerLeft;
    std::vector<float> powerRight;
    std::vector<float> rawCentreFactor;
};
} // namespace smart_ducking
