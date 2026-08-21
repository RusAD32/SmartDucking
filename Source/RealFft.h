#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include <juce_dsp/juce_dsp.h>

#ifndef SMART_DUCKING_USE_PFFFT
 #define SMART_DUCKING_USE_PFFFT 0
#endif

#if SMART_DUCKING_USE_PFFFT
 #include <pffft/pffft.h>
#endif

namespace smart_ducking
{
// Real-only in-place FFT with the positive-frequency layout used by JUCE:
// DC at [0], bin k at [2*k, 2*k+1], and Nyquist at [size].
class RealFft final
{
public:
    explicit RealFft (int order)
        : size (1 << order)
#if SMART_DUCKING_USE_PFFFT
        , setup (pffft_new_setup (size, PFFFT_REAL))
        , work (static_cast<float*> (pffft_aligned_malloc (
              static_cast<std::size_t> (size) * sizeof (float))))
#else
        , juceFft (order)
#endif
    {
#if SMART_DUCKING_USE_PFFFT
        if (setup == nullptr || work == nullptr)
            throw std::bad_alloc {};
#endif
    }

    RealFft (const RealFft&) = delete;
    RealFft& operator= (const RealFft&) = delete;

    void forward (float* data) const noexcept
    {
#if SMART_DUCKING_USE_PFFFT
        jassert (isAligned (data));
        pffft_transform_ordered (setup.get(), data, data, work.get(), PFFFT_FORWARD);

        // PFFFT packs Nyquist into the imaginary part of DC. Keep the rest of
        // the spectral code independent of the selected backend.
        data[size] = data[1];
        data[size + 1] = 0.0f;
        data[1] = 0.0f;
#else
        juceFft.performRealOnlyForwardTransform (data, true);
#endif
    }

    void inverse (float* data) const noexcept
    {
#if SMART_DUCKING_USE_PFFFT
        jassert (isAligned (data));
        data[1] = data[size];
        pffft_transform_ordered (setup.get(), data, data, work.get(), PFFFT_BACKWARD);

        // JUCE normalises its inverse transform; PFFFT intentionally does not.
        juce::FloatVectorOperations::multiply (
            data, 1.0f / static_cast<float> (size), size);
#else
        juceFft.performRealOnlyInverseTransform (data);
#endif
    }

    [[nodiscard]] int getSize() const noexcept { return size; }

    [[nodiscard]] static constexpr const char* getBackendName() noexcept
    {
#if SMART_DUCKING_USE_PFFFT
        return "PFFFT";
#else
        return "JUCE";
#endif
    }

    [[nodiscard]] static const char* getBackendArchitecture() noexcept
    {
#if SMART_DUCKING_USE_PFFFT
        return pffft_simd_arch();
#else
        return "platform-selected";
#endif
    }

    [[nodiscard]] static int getBackendSimdWidth() noexcept
    {
#if SMART_DUCKING_USE_PFFFT
        return pffft_simd_size();
#else
        return 0;
#endif
    }

private:
#if SMART_DUCKING_USE_PFFFT
    struct SetupDeleter
    {
        void operator() (PFFFT_Setup* value) const noexcept
        {
            pffft_destroy_setup (value);
        }
    };

    struct WorkDeleter
    {
        void operator() (float* value) const noexcept
        {
            pffft_aligned_free (value);
        }
    };

    [[nodiscard]] static bool isAligned (const float* data) noexcept
    {
        return (reinterpret_cast<std::uintptr_t> (data) & 0x0fu) == 0;
    }
#endif

    int size = 0;
#if SMART_DUCKING_USE_PFFFT
    std::unique_ptr<PFFFT_Setup, SetupDeleter> setup;
    std::unique_ptr<float, WorkDeleter> work;
#else
    juce::dsp::FFT juceFft;
#endif
};
} // namespace smart_ducking
