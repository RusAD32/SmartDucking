#pragma once

namespace smart_ducking::spectral_vector_math
{
enum class Backend
{
    selected,
    scalar,
    vector
};

struct MaskSmoothingParameters
{
    float attackCoefficient = 0.0f;
    float releaseCoefficient = 0.0f;
    float reductionScale = 0.0f;
    float additionalReductionDb = 0.0f;
    bool initialiseImmediately = false;
};

struct NormalCrossoverParameters
{
    float widebandGain = 1.0f;
    float crossoverStageMix = 1.0f;
    float minimumGain = 1.0e-12f;
    bool updateDisplayMask = false;
};

void powersToDecibels (const float* powers,
                       float* destinationDb,
                       int count,
                       float floorDb,
                       Backend backend = Backend::selected) noexcept;

[[nodiscard]] float smoothMaskAndApplyGain (
    const float* targetsDb,
    float* temporalReductionDb,
    float* appliedReductionDb,
    float* interleavedTransform,
    int bins,
    const MaskSmoothingParameters& parameters,
    Backend backend = Backend::selected) noexcept;

[[nodiscard]] float applyNormalCrossoverGain (
    const float* crossoverHighWeights,
    float* spectralReductionDb,
    float* interleavedTransform,
    int bins,
    const NormalCrossoverParameters& parameters,
    Backend backend = Backend::selected) noexcept;

[[nodiscard]] const char* getBackendName (
    Backend backend = Backend::selected) noexcept;

[[nodiscard]] int getBackendSimdWidth (
    Backend backend = Backend::selected) noexcept;
}

