#ifndef SMART_DUCKING_USE_XSIMD
#define SMART_DUCKING_USE_XSIMD 1
#endif

#ifndef SMART_DUCKING_HAS_XSIMD_AVX2_KERNEL
#define SMART_DUCKING_HAS_XSIMD_AVX2_KERNEL 0
#endif

#include "SpectralVectorMathKernels.h"

#include <algorithm>

namespace smart_ducking::spectral_vector_math
{
namespace detail
{
void powersToDecibelsScalar (const float* powers,
                             float* destinationDb,
                             int count,
                             float floorDb) noexcept
{
    for (auto index = 0; index < count; ++index)
        destinationDb[index] = gainToDecibelsScalar (
            std::sqrt (std::max (0.0f, powers[index])), floorDb);
}

float smoothMaskAndApplyGainScalar (
    const float* targetsDb,
    float* temporalReductionDb,
    float* appliedReductionDb,
    float* interleavedTransform,
    int bins,
    const MaskSmoothingParameters& parameters) noexcept
{
    auto maximum = 0.0f;

    for (auto bin = 0; bin < bins; ++bin)
    {
        const auto target = targetsDb[bin];
        auto& smoothed = temporalReductionDb[bin];

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
        appliedReductionDb[bin] = applied;
        maximum = std::max (maximum, applied);

        if (interleavedTransform != nullptr)
        {
            const auto gain = decibelsToGainScalar (
                -(applied + parameters.additionalReductionDb));
            interleavedTransform[2 * bin] *= gain;
            interleavedTransform[2 * bin + 1] *= gain;
        }
    }

    return maximum;
}

float applyNormalCrossoverGainScalar (
    const float* crossoverHighWeights,
    float* spectralReductionDb,
    float* interleavedTransform,
    int bins,
    const NormalCrossoverParameters& parameters) noexcept
{
    auto minimum = 1.0f;

    for (auto bin = 0; bin < bins; ++bin)
    {
        const auto highWeight = 1.0f - parameters.crossoverStageMix
                              + parameters.crossoverStageMix
                                    * crossoverHighWeights[bin];
        const auto lowWeight = 1.0f - highWeight;
        const auto spectralGain = decibelsToGainScalar (
            -std::max (0.0f, spectralReductionDb[bin]));
        const auto normalGain = lowWeight
                              + highWeight * spectralGain * parameters.widebandGain;
        interleavedTransform[2 * bin] *= normalGain;
        interleavedTransform[2 * bin + 1] *= normalGain;

        const auto effectiveSpectralGain = lowWeight + highWeight * spectralGain;
        minimum = std::min (minimum, effectiveSpectralGain);

        if (parameters.updateDisplayMask)
        {
            spectralReductionDb[bin] = -gainToDecibelsScalar (
                std::max (parameters.minimumGain, effectiveSpectralGain), -160.0f);
        }
    }

    return minimum;
}

const Functions& getScalarFunctions() noexcept
{
    static const Functions functions {
        &powersToDecibelsScalar,
        &smoothMaskAndApplyGainScalar,
        &applyNormalCrossoverGainScalar,
        "scalar",
        1
    };
    return functions;
}

const Functions& getBaselineVectorFunctions() noexcept
{
    static const Functions functions = makeVectorFunctions<xsimd::default_arch>();
    return functions;
}

const Functions& getBestVectorFunctions() noexcept
{
#if SMART_DUCKING_HAS_XSIMD_AVX2_KERNEL \
    && (defined(__x86_64__) || defined(__i386__) \
        || defined(_M_X64) || defined(_M_IX86))
    if (xsimd::available_architectures().has (xsimd::avx2 {}))
        return getAvx2Functions();
#endif

    return getBaselineVectorFunctions();
}

const Functions& getFunctions (Backend backend) noexcept
{
    switch (backend)
    {
        case Backend::scalar:
            return getScalarFunctions();
        case Backend::vector:
            return getBestVectorFunctions();
        case Backend::selected:
#if SMART_DUCKING_USE_XSIMD
            return getBestVectorFunctions();
#else
            return getScalarFunctions();
#endif
    }

    return getScalarFunctions();
}
}

void powersToDecibels (const float* powers,
                       float* destinationDb,
                       int count,
                       float floorDb,
                       Backend backend) noexcept
{
    if (powers == nullptr || destinationDb == nullptr || count <= 0)
        return;

    detail::getFunctions (backend).powersToDecibels (
        powers, destinationDb, count, floorDb);
}

float smoothMaskAndApplyGain (
    const float* targetsDb,
    float* temporalReductionDb,
    float* appliedReductionDb,
    float* interleavedTransform,
    int bins,
    const MaskSmoothingParameters& parameters,
    Backend backend) noexcept
{
    if (targetsDb == nullptr || temporalReductionDb == nullptr
        || appliedReductionDb == nullptr || bins <= 0)
        return 0.0f;

    return detail::getFunctions (backend).smoothMaskAndApplyGain (
        targetsDb,
        temporalReductionDb,
        appliedReductionDb,
        interleavedTransform,
        bins,
        parameters);
}

float applyNormalCrossoverGain (
    const float* crossoverHighWeights,
    float* spectralReductionDb,
    float* interleavedTransform,
    int bins,
    const NormalCrossoverParameters& parameters,
    Backend backend) noexcept
{
    if (crossoverHighWeights == nullptr || spectralReductionDb == nullptr
        || interleavedTransform == nullptr || bins <= 0)
        return 1.0f;

    return detail::getFunctions (backend).applyNormalCrossoverGain (
        crossoverHighWeights,
        spectralReductionDb,
        interleavedTransform,
        bins,
        parameters);
}

const char* getBackendName (Backend backend) noexcept
{
    return detail::getFunctions (backend).name;
}

int getBackendSimdWidth (Backend backend) noexcept
{
    return detail::getFunctions (backend).simdWidth;
}
}
