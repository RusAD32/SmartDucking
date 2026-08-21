#pragma once

#include <algorithm>
#include <cmath>

#include <xsimd/xsimd.hpp>

#include "SpectralVectorMath.h"

namespace smart_ducking::spectral_vector_math::detail
{
using PowersToDecibelsFunction = void (*) (const float*, float*, int, float) noexcept;
using SmoothMaskFunction = float (*) (const float*,
                                      float*,
                                      float*,
                                      float*,
                                      int,
                                      const MaskSmoothingParameters&) noexcept;
using NormalCrossoverFunction = float (*) (const float*,
                                           float*,
                                           float*,
                                           int,
                                           const NormalCrossoverParameters&) noexcept;

struct Functions
{
    PowersToDecibelsFunction powersToDecibels = nullptr;
    SmoothMaskFunction smoothMaskAndApplyGain = nullptr;
    NormalCrossoverFunction applyNormalCrossoverGain = nullptr;
    const char* name = "scalar";
    int simdWidth = 1;
};

inline float gainToDecibelsScalar (float gain, float floorDb) noexcept
{
    return gain > 0.0f
        ? std::max (floorDb, std::log10 (gain) * 20.0f)
        : floorDb;
}

inline float decibelsToGainScalar (float decibels) noexcept
{
    return decibels > -100.0f
        ? std::pow (10.0f, decibels * 0.05f)
        : 0.0f;
}

template <class Arch>
void powersToDecibelsVector (const float* powers,
                             float* destinationDb,
                             int count,
                             float floorDb) noexcept
{
    using Batch = xsimd::batch<float, Arch>;
    constexpr auto width = static_cast<int> (Batch::size);
    const Batch zero (0.0f);
    const Batch floor (floorDb);
    const Batch decibelScale (20.0f);
    auto index = 0;

    for (; index + width <= count; index += width)
    {
        const auto power = xsimd::max (
            Batch::load_unaligned (powers + index), zero);
        const auto decibels = xsimd::max (
            decibelScale * xsimd::log10 (xsimd::sqrt (power)), floor);
        decibels.store_unaligned (destinationDb + index);
    }

    for (; index < count; ++index)
        destinationDb[index] = gainToDecibelsScalar (
            std::sqrt (std::max (0.0f, powers[index])), floorDb);
}

template <class Arch>
float smoothMaskAndApplyGainVector (
    const float* targetsDb,
    float* temporalReductionDb,
    float* appliedReductionDb,
    float* interleavedTransform,
    int bins,
    const MaskSmoothingParameters& parameters) noexcept
{
    using Batch = xsimd::batch<float, Arch>;
    constexpr auto width = static_cast<int> (Batch::size);
    const Batch attack (parameters.attackCoefficient);
    const Batch release (parameters.releaseCoefficient);
    const Batch one (1.0f);
    const Batch reductionScale (parameters.reductionScale);
    const Batch additionalReduction (parameters.additionalReductionDb);
    const Batch decibelToExponent (0.05f);
    const Batch minusInfinityDb (-100.0f);
    const Batch zero (0.0f);
    auto maximumReduction = zero;
    auto index = 0;

    for (; index + width <= bins; index += width)
    {
        const auto target = Batch::load_unaligned (targetsDb + index);
        auto smoothed = target;

        if (! parameters.initialiseImmediately)
        {
            const auto previous = Batch::load_unaligned (temporalReductionDb + index);
            const auto coefficient = xsimd::select (target > previous, attack, release);
            smoothed = coefficient * previous + (one - coefficient) * target;
        }

        smoothed.store_unaligned (temporalReductionDb + index);
        const auto applied = smoothed * reductionScale;
        applied.store_unaligned (appliedReductionDb + index);
        maximumReduction = xsimd::max (maximumReduction, applied);

        if (interleavedTransform != nullptr)
        {
            const auto gainDb = -(applied + additionalReduction);
            const auto gain = xsimd::select (
                gainDb > minusInfinityDb,
                xsimd::exp10 (gainDb * decibelToExponent),
                zero);
            const auto duplicatedLow = xsimd::zip_lo (gain, gain);
            const auto duplicatedHigh = xsimd::zip_hi (gain, gain);
            auto* transform = interleavedTransform + 2 * index;
            (Batch::load_unaligned (transform) * duplicatedLow).store_unaligned (transform);
            (Batch::load_unaligned (transform + width) * duplicatedHigh)
                .store_unaligned (transform + width);
        }
    }

    auto maximum = xsimd::reduce_max (maximumReduction);

    for (; index < bins; ++index)
    {
        const auto target = targetsDb[index];
        auto& smoothed = temporalReductionDb[index];

        if (parameters.initialiseImmediately)
        {
            smoothed = target;
        }
        else
        {
            const auto coefficient = target > smoothed
                ? parameters.attackCoefficient
                : parameters.releaseCoefficient;
            smoothed = coefficient * smoothed + (1.0f - coefficient) * target;
        }

        const auto applied = smoothed * parameters.reductionScale;
        appliedReductionDb[index] = applied;
        maximum = std::max (maximum, applied);

        if (interleavedTransform != nullptr)
        {
            const auto gain = decibelsToGainScalar (
                -(applied + parameters.additionalReductionDb));
            interleavedTransform[2 * index] *= gain;
            interleavedTransform[2 * index + 1] *= gain;
        }
    }

    return maximum;
}

template <class Arch>
float applyNormalCrossoverGainVector (
    const float* crossoverHighWeights,
    float* spectralReductionDb,
    float* interleavedTransform,
    int bins,
    const NormalCrossoverParameters& parameters) noexcept
{
    using Batch = xsimd::batch<float, Arch>;
    constexpr auto width = static_cast<int> (Batch::size);
    const Batch one (1.0f);
    const Batch zero (0.0f);
    const Batch crossoverMix (parameters.crossoverStageMix);
    const Batch inverseCrossoverMix (1.0f - parameters.crossoverStageMix);
    const Batch widebandGain (parameters.widebandGain);
    const Batch decibelToExponent (-0.05f);
    const Batch minusInfinityDb (-100.0f);
    const Batch minimumGain (parameters.minimumGain);
    const Batch floorDb (-160.0f);
    const Batch decibelScale (20.0f);
    auto minimumEffectiveGain = one;
    auto index = 0;

    for (; index + width <= bins; index += width)
    {
        const auto highWeight = inverseCrossoverMix
                              + crossoverMix
                                    * Batch::load_unaligned (crossoverHighWeights + index);
        const auto lowWeight = one - highWeight;
        const auto reduction = xsimd::max (
            Batch::load_unaligned (spectralReductionDb + index), zero);
        const auto gainDb = -reduction;
        const auto spectralGain = xsimd::select (
            gainDb > minusInfinityDb,
            xsimd::exp10 (reduction * decibelToExponent),
            zero);
        const auto normalGain = lowWeight
                              + highWeight * spectralGain * widebandGain;
        const auto duplicatedLow = xsimd::zip_lo (normalGain, normalGain);
        const auto duplicatedHigh = xsimd::zip_hi (normalGain, normalGain);
        auto* transform = interleavedTransform + 2 * index;
        (Batch::load_unaligned (transform) * duplicatedLow).store_unaligned (transform);
        (Batch::load_unaligned (transform + width) * duplicatedHigh)
            .store_unaligned (transform + width);

        const auto effectiveSpectralGain = lowWeight + highWeight * spectralGain;
        minimumEffectiveGain = xsimd::min (minimumEffectiveGain, effectiveSpectralGain);

        if (parameters.updateDisplayMask)
        {
            const auto decibels = xsimd::max (
                decibelScale * xsimd::log10 (
                    xsimd::max (minimumGain, effectiveSpectralGain)),
                floorDb);
            (-decibels).store_unaligned (spectralReductionDb + index);
        }
    }

    auto minimum = xsimd::reduce_min (minimumEffectiveGain);

    for (; index < bins; ++index)
    {
        const auto highWeight = 1.0f - parameters.crossoverStageMix
                              + parameters.crossoverStageMix
                                    * crossoverHighWeights[index];
        const auto lowWeight = 1.0f - highWeight;
        const auto spectralGain = decibelsToGainScalar (
            -std::max (0.0f, spectralReductionDb[index]));
        const auto normalGain = lowWeight
                              + highWeight * spectralGain * parameters.widebandGain;
        interleavedTransform[2 * index] *= normalGain;
        interleavedTransform[2 * index + 1] *= normalGain;

        const auto effectiveSpectralGain = lowWeight + highWeight * spectralGain;
        minimum = std::min (minimum, effectiveSpectralGain);

        if (parameters.updateDisplayMask)
        {
            spectralReductionDb[index] = -gainToDecibelsScalar (
                std::max (parameters.minimumGain, effectiveSpectralGain), -160.0f);
        }
    }

    return minimum;
}

template <class Arch>
Functions makeVectorFunctions() noexcept
{
    return {
        &powersToDecibelsVector<Arch>,
        &smoothMaskAndApplyGainVector<Arch>,
        &applyNormalCrossoverGainVector<Arch>,
        Arch::name(),
        static_cast<int> (xsimd::batch<float, Arch>::size)
    };
}

[[nodiscard]] const Functions& getScalarFunctions() noexcept;
[[nodiscard]] const Functions& getBaselineVectorFunctions() noexcept;

#if SMART_DUCKING_HAS_XSIMD_AVX2_KERNEL
[[nodiscard]] const Functions& getAvx2Functions() noexcept;
#endif
}

