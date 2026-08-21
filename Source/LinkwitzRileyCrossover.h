#pragma once

#include <array>
#include <cmath>
#include <utility>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace smart_ducking
{
class LinkwitzRileyCrossover
{
public:
    struct Bands
    {
        float low = 0.0f;
        float high = 0.0f;
    };

    void prepare (double newSampleRate,
                  float initialCutoffHz,
                  float initialSlopeDbPerOctave = 48.0f) noexcept
    {
        sampleRate = juce::jmax (1.0, newSampleRate);
        smoothedCutoff.reset (sampleRate, cutoffSmoothingTimeSeconds);
        smoothedCutoff.setCurrentAndTargetValue (clampCutoff (initialCutoffHz));
        activeOrder = slopeToPrototypeOrder (initialSlopeDbPerOctave);
        pendingOrder = activeOrder;
        activeBankIndex = 0;
        activeBank().configure (sampleRate, smoothedCutoff.getCurrentValue(), activeOrder);
        pendingBank().configure (sampleRate, smoothedCutoff.getCurrentValue(), activeOrder);
        slopeTransition.reset (sampleRate, slopeTransitionTimeSeconds);
        slopeTransition.setCurrentAndTargetValue (0.0f);
        currentSlopeTransitionMix = 0.0f;
        slopeTransitionActive = false;
        queuedOrder = noQueuedOrder;
        coefficientUpdateCountdown = coefficientUpdateIntervalSamples;
    }

    void reset() noexcept
    {
        for (auto& bank : filterBanks)
            bank.reset();
    }

    void setCutoffHz (float cutoffHz) noexcept
    {
        const auto clamped = clampCutoff (cutoffHz);

        if (std::abs (clamped - smoothedCutoff.getTargetValue()) > 1.0e-4f)
            smoothedCutoff.setTargetValue (clamped);
    }

    void setSlopeDbPerOctave (float slopeDbPerOctave) noexcept
    {
        const auto requestedOrder = slopeToPrototypeOrder (slopeDbPerOctave);

        if (! slopeTransitionActive)
        {
            if (requestedOrder == activeOrder)
                return;

            startSlopeTransition (requestedOrder);
            return;
        }

        if (requestedOrder == pendingOrder)
        {
            queuedOrder = noQueuedOrder;
            slopeTransition.setTargetValue (1.0f);
            return;
        }

        if (requestedOrder == activeOrder)
        {
            queuedOrder = noQueuedOrder;
            slopeTransition.setTargetValue (0.0f);
            return;
        }

        // A mouse wheel or automation lane can pass through several choices
        // quickly. Finish the current click-free transition and retain only
        // the latest destination instead of jumping back to an unmixed bank.
        queuedOrder = requestedOrder;
        slopeTransition.setTargetValue (1.0f);
    }

    void beginSample() noexcept
    {
        if (slopeTransitionActive && ! slopeTransition.isSmoothing())
        {
            if (slopeTransition.getTargetValue() >= 0.5f)
            {
                activeBankIndex = 1 - activeBankIndex;
                activeOrder = pendingOrder;
            }

            slopeTransitionActive = false;
            slopeTransition.setCurrentAndTargetValue (0.0f);

            if (queuedOrder != noQueuedOrder && queuedOrder != activeOrder)
            {
                const auto nextOrder = queuedOrder;
                queuedOrder = noQueuedOrder;
                startSlopeTransition (nextOrder);
            }
            else
            {
                queuedOrder = noQueuedOrder;
            }
        }

        currentSlopeTransitionMix = slopeTransitionActive
            ? slopeTransition.getNextValue() : 0.0f;

        const auto wasSmoothing = smoothedCutoff.isSmoothing();
        const auto cutoff = smoothedCutoff.getNextValue();
        const auto reachedTarget = wasSmoothing && ! smoothedCutoff.isSmoothing();

        if (reachedTarget
            || (wasSmoothing && coefficientUpdateCountdown <= 0))
        {
            activeBank().updateCoefficients (sampleRate, cutoff);

            if (slopeTransitionActive)
                pendingBank().updateCoefficients (sampleRate, cutoff);

            coefficientUpdateCountdown = coefficientUpdateIntervalSamples;
        }

        if (coefficientUpdateCountdown > 0)
            --coefficientUpdateCountdown;
    }

    [[nodiscard]] Bands processSample (int channel, float input) noexcept
    {
        jassert (juce::isPositiveAndBelow (channel, numChannels));
        const auto active = activeBank().processSample (channel, input);

        if (! slopeTransitionActive)
            return active;

        const auto pending = pendingBank().processSample (channel, input);
        return {
            active.low + currentSlopeTransitionMix * (pending.low - active.low),
            active.high + currentSlopeTransitionMix * (pending.high - active.high)
        };
    }

    [[nodiscard]] bool canProcessSteadyBlock() const noexcept
    {
        return ! slopeTransitionActive && ! smoothedCutoff.isSmoothing();
    }

    void processSteadyBlock (const std::array<const float*, 5>& input,
                             const std::array<float*, 5>& lowOutput,
                             const std::array<float*, 5>& highOutput,
                             int numSamples,
                             bool stereo) noexcept
    {
        if (numSamples <= 0)
            return;

        jassert (canProcessSteadyBlock());

        if constexpr (juce::dsp::SIMDRegister<double>::size() >= 2)
        {
            const auto processPair = [&] (int firstChannel, int secondChannel)
            {
                const auto firstIndex = static_cast<size_t> (firstChannel);
                const auto secondIndex = static_cast<size_t> (secondChannel);
                activeBank().processSteadyPairBlock (
                    firstChannel,
                    secondChannel,
                    input[firstIndex],
                    input[secondIndex],
                    lowOutput[firstIndex],
                    lowOutput[secondIndex],
                    highOutput[firstIndex],
                    highOutput[secondIndex],
                    numSamples);
            };

            processPair (0, stereo ? 2 : 1);

            if (! stereo)
            {
                processPair (2, 3);
                activeBank().processSteadyBlock (4,
                                                 input[4],
                                                 lowOutput[4],
                                                 highOutput[4],
                                                 numSamples);
            }

            coefficientUpdateCountdown = juce::jmax (
                0, coefficientUpdateCountdown - numSamples);
            return;
        }

        const auto processChannel = [&] (int channel)
        {
            const auto index = static_cast<size_t> (channel);
            jassert (input[index] != nullptr);
            jassert (lowOutput[index] != nullptr);
            jassert (highOutput[index] != nullptr);
            activeBank().processSteadyBlock (channel,
                                             input[index],
                                             lowOutput[index],
                                             highOutput[index],
                                             numSamples);
        };

        processChannel (0);

        if (stereo)
        {
            processChannel (2);
        }
        else
        {
            processChannel (1);
            processChannel (2);
            processChannel (3);
            processChannel (4);
        }

        // beginSample() normally advances this countdown even while the
        // cutoff is stationary. Preserve that timeline so later automation
        // enters the generic path at exactly the same coefficient-update
        // phase.
        coefficientUpdateCountdown = juce::jmax (
            0, coefficientUpdateCountdown - numSamples);
    }

    [[nodiscard]] float getCurrentCutoffHz() const noexcept
    {
        return smoothedCutoff.getCurrentValue();
    }

    [[nodiscard]] int getCurrentSlopeDbPerOctave() const noexcept
    {
        const auto order = slopeTransitionActive && slopeTransition.getTargetValue() >= 0.5f
            ? pendingOrder : activeOrder;
        return order * 12;
    }

private:
    struct Coefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct State
    {
        void reset() noexcept
        {
            z1 = 0.0;
            z2 = 0.0;
        }

        [[nodiscard]] float processFirstOrder (
            float input, const Coefficients& coefficients) noexcept
        {
            const auto output = coefficients.b0 * static_cast<double> (input) + z1;
            z1 = coefficients.b1 * static_cast<double> (input)
               - coefficients.a1 * output;
            return static_cast<float> (output);
        }

        [[nodiscard]] float processSecondOrder (
            float input, const Coefficients& coefficients) noexcept
        {
            const auto output = coefficients.b0 * static_cast<double> (input) + z1;
            z1 = coefficients.b1 * static_cast<double> (input) - coefficients.a1 * output + z2;
            z2 = coefficients.b2 * static_cast<double> (input) - coefficients.a2 * output;
            return static_cast<float> (output);
        }

        double z1 = 0.0;
        double z2 = 0.0;
    };

    struct FilterBank
    {
        void configure (double bankSampleRate,
                        float cutoffHz,
                        int newPrototypeOrder) noexcept
        {
            prototypeOrder = newPrototypeOrder;
            reset();
            updateCoefficients (bankSampleRate, cutoffHz);
        }

        void reset() noexcept
        {
            for (auto& channel : lowStates)
                for (auto& state : channel)
                    state.reset();

            for (auto& channel : highStates)
                for (auto& state : channel)
                    state.reset();
        }

        void updateCoefficients (double bankSampleRate, float cutoffHz) noexcept
        {
            numSections = 0;
            numFirstOrderSections = 0;

            // A Linkwitz-Riley response is a Butterworth prototype cascaded
            // twice. Odd prototypes contribute one first-order section to
            // each copy, giving the exact LR6 and LR10 options.
            if ((prototypeOrder & 1) != 0)
            {
                for (int copy = 0; copy < 2; ++copy)
                    addSection (makeFirstOrderLowAndHighPass (
                        bankSampleRate, cutoffHz));

                numFirstOrderSections = numSections;
            }

            for (int copy = 0; copy < 2; ++copy)
            {
                for (int pair = 1; pair <= prototypeOrder / 2; ++pair)
                {
                    const auto angle = (2.0 * static_cast<double> (pair) - 1.0)
                                     * juce::MathConstants<double>::pi
                                     / (2.0 * static_cast<double> (prototypeOrder));
                    const auto q = 1.0 / (2.0 * std::sin (angle));
                    addSection (makeSecondOrderLowAndHighPass (
                        bankSampleRate, cutoffHz, q));
                }
            }

            highPolarity = (prototypeOrder & 1) != 0 ? -1.0f : 1.0f;
        }

        [[nodiscard]] Bands processSample (int channel, float input) noexcept
        {
            auto low = input;
            auto high = input;

            for (int section = 0; section < numFirstOrderSections; ++section)
            {
                const auto index = static_cast<size_t> (section);
                low = lowStates[static_cast<size_t> (channel)][index]
                          .processFirstOrder (low, lowCoefficients[index]);
                high = highStates[static_cast<size_t> (channel)][index]
                           .processFirstOrder (high, highCoefficients[index]);
            }

            for (int section = numFirstOrderSections; section < numSections; ++section)
            {
                const auto index = static_cast<size_t> (section);
                low = lowStates[static_cast<size_t> (channel)][index]
                          .processSecondOrder (low, lowCoefficients[index]);
                high = highStates[static_cast<size_t> (channel)][index]
                           .processSecondOrder (high, highCoefficients[index]);
            }

            return { low, highPolarity * high };
        }

        void processSteadyBlock (int channel,
                                 const float* input,
                                 float* lowOutput,
                                 float* highOutput,
                                 int numSamples) noexcept
        {
            switch (prototypeOrder)
            {
                case 2: processFixedOrderBlock<2> (channel, input, lowOutput,
                                                   highOutput, numSamples); break;
                case 3: processFixedOrderBlock<3> (channel, input, lowOutput,
                                                   highOutput, numSamples); break;
                case 4: processFixedOrderBlock<4> (channel, input, lowOutput,
                                                   highOutput, numSamples); break;
                case 5: processFixedOrderBlock<5> (channel, input, lowOutput,
                                                   highOutput, numSamples); break;
                case 6: processFixedOrderBlock<6> (channel, input, lowOutput,
                                                   highOutput, numSamples); break;
                default: jassertfalse; break;
            }
        }

        void processSteadyPairBlock (int firstChannel,
                                     int secondChannel,
                                     const float* firstInput,
                                     const float* secondInput,
                                     float* firstLowOutput,
                                     float* secondLowOutput,
                                     float* firstHighOutput,
                                     float* secondHighOutput,
                                     int numSamples) noexcept
        {
            switch (prototypeOrder)
            {
                case 2: processFixedOrderPairBlock<2> (
                    firstChannel, secondChannel, firstInput, secondInput,
                    firstLowOutput, secondLowOutput, firstHighOutput,
                    secondHighOutput, numSamples); break;
                case 3: processFixedOrderPairBlock<3> (
                    firstChannel, secondChannel, firstInput, secondInput,
                    firstLowOutput, secondLowOutput, firstHighOutput,
                    secondHighOutput, numSamples); break;
                case 4: processFixedOrderPairBlock<4> (
                    firstChannel, secondChannel, firstInput, secondInput,
                    firstLowOutput, secondLowOutput, firstHighOutput,
                    secondHighOutput, numSamples); break;
                case 5: processFixedOrderPairBlock<5> (
                    firstChannel, secondChannel, firstInput, secondInput,
                    firstLowOutput, secondLowOutput, firstHighOutput,
                    secondHighOutput, numSamples); break;
                case 6: processFixedOrderPairBlock<6> (
                    firstChannel, secondChannel, firstInput, secondInput,
                    firstLowOutput, secondLowOutput, firstHighOutput,
                    secondHighOutput, numSamples); break;
                default: jassertfalse; break;
            }
        }

        template <int prototype>
        void processFixedOrderBlock (int channel,
                                     const float* input,
                                     float* lowOutput,
                                     float* highOutput,
                                     int numSamples) noexcept
        {
            constexpr auto firstOrderSections = (prototype & 1) != 0 ? 2 : 0;
            constexpr auto totalSections = firstOrderSections
                                         + 2 * (prototype / 2);
            const auto channelIndex = static_cast<size_t> (channel);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                auto low = input[sample];
                auto high = input[sample];

                for (int section = 0; section < firstOrderSections; ++section)
                {
                    const auto index = static_cast<size_t> (section);
                    low = lowStates[channelIndex][index].processFirstOrder (
                        low, lowCoefficients[index]);
                    high = highStates[channelIndex][index].processFirstOrder (
                        high, highCoefficients[index]);
                }

                for (int section = firstOrderSections;
                     section < totalSections;
                     ++section)
                {
                    const auto index = static_cast<size_t> (section);
                    low = lowStates[channelIndex][index].processSecondOrder (
                        low, lowCoefficients[index]);
                    high = highStates[channelIndex][index].processSecondOrder (
                        high, highCoefficients[index]);
                }

                lowOutput[sample] = low;
                highOutput[sample] = highPolarity * high;
            }
        }

        template <int prototype>
        void processFixedOrderPairBlock (int firstChannel,
                                         int secondChannel,
                                         const float* firstInput,
                                         const float* secondInput,
                                         float* firstLowOutput,
                                         float* secondLowOutput,
                                         float* firstHighOutput,
                                         float* secondHighOutput,
                                         int numSamples) noexcept
        {
            using Vector = juce::dsp::SIMDRegister<double>;
            struct PackedState
            {
                Vector z1;
                Vector z2;
            };

            constexpr auto firstOrderSections = (prototype & 1) != 0 ? 2 : 0;
            constexpr auto totalSections = firstOrderSections
                                         + 2 * (prototype / 2);
            std::array<PackedState, bankMaxSections> packedLowStates;
            std::array<PackedState, bankMaxSections> packedHighStates;
            const auto firstIndex = static_cast<size_t> (firstChannel);
            const auto secondIndex = static_cast<size_t> (secondChannel);

            for (int section = 0; section < totalSections; ++section)
            {
                const auto index = static_cast<size_t> (section);
                packedLowStates[index].z1.set (0, lowStates[firstIndex][index].z1);
                packedLowStates[index].z1.set (1, lowStates[secondIndex][index].z1);
                packedLowStates[index].z2.set (0, lowStates[firstIndex][index].z2);
                packedLowStates[index].z2.set (1, lowStates[secondIndex][index].z2);
                packedHighStates[index].z1.set (0, highStates[firstIndex][index].z1);
                packedHighStates[index].z1.set (1, highStates[secondIndex][index].z1);
                packedHighStates[index].z2.set (0, highStates[firstIndex][index].z2);
                packedHighStates[index].z2.set (1, highStates[secondIndex][index].z2);
            }

            for (int sample = 0; sample < numSamples; ++sample)
            {
                Vector low;
                low.set (0, static_cast<double> (firstInput[sample]));
                low.set (1, static_cast<double> (secondInput[sample]));
                auto high = low;

                for (int section = 0; section < firstOrderSections; ++section)
                {
                    const auto index = static_cast<size_t> (section);
                    const auto lowOutput = low * lowCoefficients[index].b0
                                         + packedLowStates[index].z1;
                    packedLowStates[index].z1 = low * lowCoefficients[index].b1
                                              - lowOutput * lowCoefficients[index].a1;
                    low = lowOutput;
                    const auto highOutput = high * highCoefficients[index].b0
                                          + packedHighStates[index].z1;
                    packedHighStates[index].z1 = high * highCoefficients[index].b1
                                               - highOutput * highCoefficients[index].a1;
                    high = highOutput;
                }

                for (int section = firstOrderSections;
                     section < totalSections;
                     ++section)
                {
                    const auto index = static_cast<size_t> (section);
                    const auto lowOutput = low * lowCoefficients[index].b0
                                         + packedLowStates[index].z1;
                    packedLowStates[index].z1 = low * lowCoefficients[index].b1
                                              - lowOutput * lowCoefficients[index].a1
                                              + packedLowStates[index].z2;
                    packedLowStates[index].z2 = low * lowCoefficients[index].b2
                                              - lowOutput * lowCoefficients[index].a2;
                    low = lowOutput;
                    const auto highOutput = high * highCoefficients[index].b0
                                          + packedHighStates[index].z1;
                    packedHighStates[index].z1 = high * highCoefficients[index].b1
                                               - highOutput * highCoefficients[index].a1
                                               + packedHighStates[index].z2;
                    packedHighStates[index].z2 = high * highCoefficients[index].b2
                                               - highOutput * highCoefficients[index].a2;
                    high = highOutput;
                }

                firstLowOutput[sample] = static_cast<float> (low.get (0));
                secondLowOutput[sample] = static_cast<float> (low.get (1));
                firstHighOutput[sample] = highPolarity
                                        * static_cast<float> (high.get (0));
                secondHighOutput[sample] = highPolarity
                                         * static_cast<float> (high.get (1));
            }

            for (int section = 0; section < totalSections; ++section)
            {
                const auto index = static_cast<size_t> (section);
                lowStates[firstIndex][index].z1 = packedLowStates[index].z1.get (0);
                lowStates[secondIndex][index].z1 = packedLowStates[index].z1.get (1);
                lowStates[firstIndex][index].z2 = packedLowStates[index].z2.get (0);
                lowStates[secondIndex][index].z2 = packedLowStates[index].z2.get (1);
                highStates[firstIndex][index].z1 = packedHighStates[index].z1.get (0);
                highStates[secondIndex][index].z1 = packedHighStates[index].z1.get (1);
                highStates[firstIndex][index].z2 = packedHighStates[index].z2.get (0);
                highStates[secondIndex][index].z2 = packedHighStates[index].z2.get (1);
            }
        }

        void addSection (const std::pair<Coefficients, Coefficients>& coefficients) noexcept
        {
            jassert (numSections < bankMaxSections);
            const auto index = static_cast<size_t> (numSections++);
            lowCoefficients[index] = coefficients.first;
            highCoefficients[index] = coefficients.second;
        }

        int prototypeOrder = 4;
        int numSections = 4;
        int numFirstOrderSections = 0;
        float highPolarity = 1.0f;
        static constexpr int bankNumChannels = 5;
        static constexpr int bankMaxSections = 6;
        std::array<Coefficients, bankMaxSections> lowCoefficients;
        std::array<Coefficients, bankMaxSections> highCoefficients;
        std::array<std::array<State, bankMaxSections>, bankNumChannels> lowStates;
        std::array<std::array<State, bankMaxSections>, bankNumChannels> highStates;
    };

    [[nodiscard]] float clampCutoff (float cutoffHz) const noexcept
    {
        return juce::jlimit (10.0f,
                             static_cast<float> (sampleRate * 0.45),
                             cutoffHz);
    }

    [[nodiscard]] static int slopeToPrototypeOrder (float slopeDbPerOctave) noexcept
    {
        return juce::jlimit (2, 6, juce::roundToInt (slopeDbPerOctave / 12.0f));
    }

    static std::pair<Coefficients, Coefficients> makeFirstOrderLowAndHighPass (
        double currentSampleRate,
        float cutoffHz) noexcept
    {
        const auto k = std::tan (juce::MathConstants<double>::pi
                               * static_cast<double> (cutoffHz)
                               / currentSampleRate);
        const auto inverse = 1.0 / (1.0 + k);

        Coefficients low;
        low.b0 = k * inverse;
        low.b1 = low.b0;
        low.a1 = (k - 1.0) * inverse;

        Coefficients high;
        high.b0 = inverse;
        high.b1 = -inverse;
        high.a1 = low.a1;
        return { low, high };
    }

    static std::pair<Coefficients, Coefficients> makeSecondOrderLowAndHighPass (
        double currentSampleRate,
        float cutoffHz,
        double q) noexcept
    {
        const auto omega = juce::MathConstants<double>::twoPi
                         * static_cast<double> (cutoffHz)
                         / currentSampleRate;
        const auto cosine = std::cos (omega);
        const auto sine = std::sin (omega);
        const auto alpha = sine / (2.0 * q);
        const auto inverseA0 = 1.0 / (1.0 + alpha);

        Coefficients low;
        low.b0 = 0.5 * (1.0 - cosine) * inverseA0;
        low.b1 = (1.0 - cosine) * inverseA0;
        low.b2 = low.b0;
        low.a1 = -2.0 * cosine * inverseA0;
        low.a2 = (1.0 - alpha) * inverseA0;

        Coefficients high;
        high.b0 = 0.5 * (1.0 + cosine) * inverseA0;
        high.b1 = -(1.0 + cosine) * inverseA0;
        high.b2 = high.b0;
        high.a1 = low.a1;
        high.a2 = low.a2;
        return { low, high };
    }

    void startSlopeTransition (int requestedOrder) noexcept
    {
        pendingOrder = requestedOrder;
        pendingBank().configure (sampleRate,
                                 smoothedCutoff.getCurrentValue(),
                                 pendingOrder);
        slopeTransition.reset (sampleRate, slopeTransitionTimeSeconds);
        slopeTransition.setCurrentAndTargetValue (0.0f);
        slopeTransition.setTargetValue (1.0f);
        currentSlopeTransitionMix = 0.0f;
        slopeTransitionActive = true;
    }

    [[nodiscard]] FilterBank& activeBank() noexcept
    {
        return filterBanks[static_cast<size_t> (activeBankIndex)];
    }

    [[nodiscard]] FilterBank& pendingBank() noexcept
    {
        return filterBanks[static_cast<size_t> (1 - activeBankIndex)];
    }

    static constexpr int numChannels = 5;
    static constexpr int coefficientUpdateIntervalSamples = 16;
    static constexpr int noQueuedOrder = -1;
    static constexpr double cutoffSmoothingTimeSeconds = 0.05;
    static constexpr double slopeTransitionTimeSeconds = 0.10;

    double sampleRate = 48000.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedCutoff;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> slopeTransition;
    std::array<FilterBank, 2> filterBanks;
    int activeBankIndex = 0;
    int activeOrder = 4;
    int pendingOrder = 4;
    int queuedOrder = noQueuedOrder;
    int coefficientUpdateCountdown = 0;
    float currentSlopeTransitionMix = 0.0f;
    bool slopeTransitionActive = false;
};
} // namespace smart_ducking
