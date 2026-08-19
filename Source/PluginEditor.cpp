#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

#include "BinaryData.h"
#include "CrossoverMagnitude.h"
#include "ParameterIds.h"

namespace
{
constexpr float meterFloorDb = -60.0f;
constexpr float spectrumFloorDb = -60.0f;
constexpr float meterRefreshHz = 15.0f;
constexpr float maximumDisplayedReductionDb = 18.0f;

const auto backgroundColour = juce::Colour::fromRGB (27, 15, 45);
const auto backgroundSoftColour = juce::Colour::fromRGB (36, 16, 58);
const auto surfaceColour = juce::Colour::fromRGB (47, 23, 71);
const auto primaryColour = juce::Colour::fromRGB (138, 43, 226);
const auto primarySoftColour = juce::Colour::fromRGB (168, 90, 255);
const auto textColour = juce::Colour::fromRGB (255, 255, 255);
const auto secondaryTextColour = juce::Colour::fromRGB (215, 199, 233);
const auto mutedTextColour = juce::Colour::fromRGB (184, 166, 204);
const auto voiceColour = juce::Colour::fromRGB (132, 226, 44);
const auto wideReductionColour = juce::Colour::fromRGB (239, 68, 68);
const auto maskColour = juce::Colour::fromRGB (251, 191, 36);
const auto spectralReductionColour = maskColour;
const auto borderColour = juce::Colours::white.withAlpha (0.10f);
const auto cardColour = juce::Colours::white.withAlpha (0.045f);
const juce::Identifier bypassIconProperty { "bypassIcon" };
const juce::Identifier crossoverMonitorBandProperty { "crossoverMonitorBand" };
const juce::Identifier comboContextLabelProperty { "comboContextLabel" };
constexpr int factoryPresetItemBase = 1;
constexpr int userPresetItemBase = 100;
constexpr int savePresetItemId = 10000;
constexpr int deletePresetItemId = 10001;

float decaySignalPeak (float displayed, float current) noexcept
{
    constexpr float decayDbPerSecond = 24.0f;
    const auto decayGain = std::pow (10.0f, -decayDbPerSecond / (20.0f * meterRefreshHz));
    displayed = std::isfinite (displayed) ? displayed : 0.0f;
    current = std::isfinite (current) ? current : 0.0f;
    return std::max (current, displayed * decayGain);
}

float decayReduction (float displayedDb, float currentDb) noexcept
{
    constexpr float returnDbPerSecond = 18.0f;
    displayedDb = std::isfinite (displayedDb) ? displayedDb : 0.0f;
    currentDb = std::isfinite (currentDb) ? currentDb : 0.0f;
    return std::max (currentDb, displayedDb - returnDbPerSecond / meterRefreshHz);
}

float signalProportion (float linearPeak) noexcept
{
    if (! std::isfinite (linearPeak))
        return 0.0f;

    const auto decibels = juce::Decibels::gainToDecibels (linearPeak, meterFloorDb);
    return juce::jlimit (0.0f, 1.0f, (decibels - meterFloorDb) / -meterFloorDb);
}

void drawCard (juce::Graphics& graphics, juce::Rectangle<float> bounds, float corner = 10.0f)
{
    graphics.setColour (cardColour);
    graphics.fillRoundedRectangle (bounds, corner);
    graphics.setColour (borderColour);
    graphics.drawRoundedRectangle (bounds.reduced (0.5f), corner, 1.0f);
}

class GlobalBypassPanel final : public juce::Component
{
public:
    void paint (juce::Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        graphics.setColour (backgroundSoftColour.withAlpha (0.78f));
        graphics.fillRoundedRectangle (bounds, 7.0f);
        graphics.setColour (primarySoftColour.withAlpha (0.26f));
        graphics.drawRoundedRectangle (bounds, 7.0f, 1.0f);
    }
};

void drawBypassIcon (juce::Graphics& graphics,
                     juce::Rectangle<float> bounds,
                     juce::Colour colour)
{
    const auto size = juce::jmin (18.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()));
    bounds = bounds.withSizeKeepingCentre (size, size * 0.72f);
    const auto left = bounds.getX();
    const auto right = bounds.getRight();
    const auto centreY = bounds.getCentreY();
    const auto splitX = left + bounds.getWidth() * 0.27f;
    const auto mergeX = left + bounds.getWidth() * 0.73f;
    const auto upperY = bounds.getY() + 1.0f;
    const auto lowerY = bounds.getBottom() - 1.0f;

    juce::Path signal;
    signal.startNewSubPath (left, centreY);
    signal.lineTo (splitX, centreY);
    signal.cubicTo (splitX + 1.5f, upperY,
                    mergeX - 1.5f, upperY,
                    mergeX, centreY);
    signal.lineTo (right, centreY);
    signal.startNewSubPath (splitX, centreY);
    signal.cubicTo (splitX + 1.5f, lowerY,
                    mergeX - 1.5f, lowerY,
                    mergeX, centreY);

    graphics.setColour (colour);
    graphics.strokePath (signal, juce::PathStrokeType (1.8f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
    graphics.fillEllipse (juce::Rectangle<float> (3.4f, 3.4f).withCentre ({ splitX, centreY }));
    graphics.fillEllipse (juce::Rectangle<float> (3.4f, 3.4f).withCentre ({ mergeX, centreY }));
}

void drawCrossoverMonitorIcon (juce::Graphics& graphics,
                               juce::Rectangle<float> bounds,
                               juce::Colour colour,
                               bool lowPass)
{
    bounds = bounds.withSizeKeepingCentre (
        juce::jmin (36.0f, bounds.getWidth() - 8.0f),
        juce::jmin (18.0f, bounds.getHeight() - 8.0f));
    const auto headphone = bounds.removeFromLeft (14.0f);
    bounds.removeFromLeft (4.0f);
    const auto curve = bounds;
    juce::Path headband;
    headband.startNewSubPath (headphone.getX() + 1.5f, headphone.getBottom() - 5.0f);
    headband.cubicTo (headphone.getX() + 1.5f, headphone.getY() + 1.0f,
                      headphone.getRight() - 1.5f, headphone.getY() + 1.0f,
                      headphone.getRight() - 1.5f, headphone.getBottom() - 5.0f);
    graphics.setColour (colour);
    graphics.strokePath (headband,
                         juce::PathStrokeType (1.8f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    graphics.fillRoundedRectangle (
        juce::Rectangle<float> (headphone.getX(), headphone.getBottom() - 7.0f, 3.2f, 6.0f),
        1.2f);
    graphics.fillRoundedRectangle (
        juce::Rectangle<float> (headphone.getRight() - 3.2f,
                                headphone.getBottom() - 7.0f,
                                3.2f,
                                6.0f),
        1.2f);

    const auto upper = curve.getY() + 3.0f;
    const auto lower = curve.getBottom() - 3.0f;
    const auto knee = curve.getX() + curve.getWidth() * 0.48f;
    juce::Path response;

    if (lowPass)
    {
        response.startNewSubPath (curve.getX(), upper);
        response.lineTo (knee, upper);
        response.cubicTo (knee + curve.getWidth() * 0.16f, upper,
                          curve.getRight() - curve.getWidth() * 0.18f, lower,
                          curve.getRight(), lower);
    }
    else
    {
        response.startNewSubPath (curve.getX(), lower);
        response.cubicTo (curve.getX() + curve.getWidth() * 0.18f, lower,
                          knee - curve.getWidth() * 0.16f, upper,
                          knee, upper);
        response.lineTo (curve.getRight(), upper);
    }

    graphics.strokePath (response,
                         juce::PathStrokeType (1.8f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

juce::String frequencyText (float frequency)
{
    if (frequency >= 1000.0f)
        return juce::String (frequency / 1000.0f, 2) + " kHz";

    return juce::String (juce::roundToInt (frequency)) + " Hz";
}

juce::String parameterTooltip (const juce::String& parameterId)
{
    using namespace smart_ducking::parameter_ids;

    if (parameterId == duckDepthDb)
        return "Maximum broadband attenuation applied while the sidechain is active.";
    if (parameterId == activityThresholdDb)
        return "Sidechain level above which the voice is considered active and ducking can begin.";
    if (parameterId == attackMs)
        return "How quickly gain reduction reaches its target after the voice becomes active.";
    if (parameterId == holdMs)
        return "Minimum time the current ducking state is held before release can begin.";
    if (parameterId == releaseMs)
        return "How quickly the original signal returns after sidechain activity ends.";
    if (parameterId == lookaheadMs)
        return "How far the voice detector acts ahead of the matching Main audio. Reported latency stays fixed.";
    if (parameterId == spectralAmountDb)
        return "Maximum frequency-selective attenuation driven by the sidechain spectrum.";
    if (parameterId == weightFrequencyHz)
        return "Centre frequency of the spectral detector weighting EQ.";
    if (parameterId == weightGainDb)
        return "Boost or cut in spectral detector sensitivity; negative values de-emphasise this region.";
    if (parameterId == weightQ)
        return "Bandwidth of the spectral detector weighting EQ; higher values focus it more narrowly.";
    if (parameterId == spectralLowHz)
        return "Lowest frequency affected by spectral ducking. This is also adjustable on the spectrum.";
    if (parameterId == spectralHighHz)
        return "Highest frequency affected by spectral ducking. This is also adjustable on the spectrum.";
    if (parameterId.startsWith ("matrix"))
        return "How strongly this voice channel controls ducking in the selected main channel.";

    return "Adjust this processing parameter. Double-click the value to type an exact number.";
}

const std::array<const char*, 25> matrixParameterIds {
    smart_ducking::parameter_ids::matrixLL,
    smart_ducking::parameter_ids::matrixLC,
    smart_ducking::parameter_ids::matrixLR,
    smart_ducking::parameter_ids::matrixLLs,
    smart_ducking::parameter_ids::matrixLRs,
    smart_ducking::parameter_ids::matrixCL,
    smart_ducking::parameter_ids::matrixCC,
    smart_ducking::parameter_ids::matrixCR,
    smart_ducking::parameter_ids::matrixCLs,
    smart_ducking::parameter_ids::matrixCRs,
    smart_ducking::parameter_ids::matrixRL,
    smart_ducking::parameter_ids::matrixRC,
    smart_ducking::parameter_ids::matrixRR,
    smart_ducking::parameter_ids::matrixRLs,
    smart_ducking::parameter_ids::matrixRRs,
    smart_ducking::parameter_ids::matrixLsL,
    smart_ducking::parameter_ids::matrixLsC,
    smart_ducking::parameter_ids::matrixLsR,
    smart_ducking::parameter_ids::matrixLsLs,
    smart_ducking::parameter_ids::matrixLsRs,
    smart_ducking::parameter_ids::matrixRsL,
    smart_ducking::parameter_ids::matrixRsC,
    smart_ducking::parameter_ids::matrixRsR,
    smart_ducking::parameter_ids::matrixRsLs,
    smart_ducking::parameter_ids::matrixRsRs
};

const std::array<float, 25> defaultMatrixValues {
    1.00f, 0.25f, 0.00f, 0.35f, 0.00f,
    0.10f, 1.00f, 0.10f, 0.05f, 0.05f,
    0.00f, 0.25f, 1.00f, 0.00f, 0.35f,
    0.35f, 0.00f, 0.00f, 1.00f, 0.05f,
    0.00f, 0.00f, 0.35f, 0.05f, 1.00f
};
} // namespace

struct SmartDuckingAudioProcessorEditor::RusADLookAndFeel final : juce::LookAndFeel_V4
{
    RusADLookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, backgroundSoftColour);
        setColour (juce::PopupMenu::textColourId, textColour);
        setColour (juce::PopupMenu::headerTextColourId, secondaryTextColour);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, primaryColour.withAlpha (0.35f));
        setColour (juce::PopupMenu::highlightedTextColourId, textColour);
        setColour (juce::TooltipWindow::backgroundColourId, surfaceColour);
        setColour (juce::TooltipWindow::textColourId, textColour);
        setColour (juce::TooltipWindow::outlineColourId, borderColour);
    }

    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions { 11.0f });
    }

    void drawPopupMenuBackground (juce::Graphics& graphics,
                                  int width,
                                  int height) override
    {
        graphics.fillAll (backgroundSoftColour);
        graphics.setColour (borderColour);
        graphics.drawRect (juce::Rectangle<int> (0, 0, width, height), 1);
    }

    void drawRotarySlider (juce::Graphics& graphics,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPosition,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider&) override
    {
        const auto diameter = static_cast<float> (juce::jmin (width, height)) - 20.0f;
        auto bounds = juce::Rectangle<float> (static_cast<float> (x),
                                              static_cast<float> (y),
                                              static_cast<float> (width),
                                              static_cast<float> (height))
                          .withSizeKeepingCentre (diameter, diameter);
        const auto radius = diameter * 0.5f;
        const auto centre = bounds.getCentre();
        const auto angle = rotaryStartAngle
                         + sliderPosition * (rotaryEndAngle - rotaryStartAngle);

        graphics.setColour (juce::Colours::black.withAlpha (0.30f));
        graphics.fillEllipse (bounds);
        graphics.setColour (borderColour);
        graphics.drawEllipse (bounds, 1.0f);

        juce::Path track;
        track.addCentredArc (centre.x,
                             centre.y,
                             radius - 4.0f,
                             radius - 4.0f,
                             0.0f,
                             rotaryStartAngle,
                             rotaryEndAngle,
                             true);
        graphics.setColour (juce::Colours::white.withAlpha (0.10f));
        graphics.strokePath (track, juce::PathStrokeType (4.0f,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc (centre.x,
                                centre.y,
                                radius - 4.0f,
                                radius - 4.0f,
                                0.0f,
                                rotaryStartAngle,
                                angle,
                                true);
        graphics.setColour (primarySoftColour);
        graphics.strokePath (valueArc, juce::PathStrokeType (4.0f,
                                                             juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));

        juce::Path pointer;
        pointer.addRoundedRectangle (-1.5f, -radius + 9.0f, 3.0f, radius * 0.38f, 1.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x,
                                                                                     centre.y));
        graphics.setColour (textColour);
        graphics.fillPath (pointer);
    }

    void drawButtonBackground (juce::Graphics& graphics,
                               juce::Button& button,
                               const juce::Colour&,
                               bool isMouseOverButton,
                               bool isButtonDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
        auto fill = button.getToggleState() ? primaryColour : juce::Colours::white.withAlpha (0.035f);

        if (isButtonDown)
            fill = fill.brighter (0.12f);
        else if (isMouseOverButton)
            fill = fill.brighter (0.06f);

        graphics.setColour (fill);
        graphics.fillRoundedRectangle (bounds, 7.0f);
        graphics.setColour (button.getToggleState() ? primarySoftColour.withAlpha (0.9f)
                                                     : borderColour);
        graphics.drawRoundedRectangle (bounds, 7.0f, 1.0f);
    }

    void drawButtonText (juce::Graphics& graphics,
                         juce::TextButton& button,
                         bool,
                         bool) override
    {
        const auto colour = button.getToggleState() ? textColour : secondaryTextColour;
        if (static_cast<bool> (button.getProperties().getWithDefault (
                bypassIconProperty, false)))
        {
            drawBypassIcon (graphics, button.getLocalBounds().toFloat(), colour);
            return;
        }

        const auto crossoverMonitorBand = static_cast<int> (
            button.getProperties().getWithDefault (crossoverMonitorBandProperty, 0));
        if (crossoverMonitorBand != 0)
        {
            drawCrossoverMonitorIcon (graphics,
                                      button.getLocalBounds().toFloat(),
                                      colour,
                                      crossoverMonitorBand == 1);
            return;
        }

        graphics.setFont (juce::FontOptions { 12.0f, juce::Font::bold });
        graphics.setColour (colour);
        graphics.drawFittedText (button.getButtonText(),
                                 button.getLocalBounds().reduced (8, 2),
                                 juce::Justification::centred,
                                 1);
    }

    void drawToggleButton (juce::Graphics& graphics,
                           juce::ToggleButton& button,
                           bool isMouseOverButton,
                           bool) override
    {
        const auto isEnabled = button.isEnabled();
        const auto visuallyOn = isEnabled && button.getToggleState();
        auto bounds = button.getLocalBounds().toFloat().reduced (2.0f);
        graphics.setColour (visuallyOn ? primaryColour.withAlpha (0.20f)
                                       : juce::Colours::white.withAlpha (
                                             isEnabled ? 0.035f : 0.015f));
        graphics.fillRoundedRectangle (bounds, 7.0f);
        graphics.setColour (visuallyOn ? primarySoftColour.withAlpha (0.78f)
                                       : borderColour.withMultipliedAlpha (
                                             isEnabled ? 1.0f : 0.45f));
        graphics.drawRoundedRectangle (bounds, 7.0f, 1.0f);

        auto indicatorArea = bounds.removeFromLeft (24.0f);
        const auto indicatorDiameter = juce::jmin (11.0f,
                                                   juce::jmin (indicatorArea.getWidth(),
                                                               indicatorArea.getHeight()));
        auto indicator = indicatorArea.withSizeKeepingCentre (indicatorDiameter,
                                                               indicatorDiameter);
        graphics.setColour (visuallyOn ? primarySoftColour
                                       : mutedTextColour.withAlpha (
                                             isEnabled ? 0.45f : 0.20f));
        graphics.fillEllipse (indicator);
        graphics.setColour (! isEnabled ? mutedTextColour.withAlpha (0.36f)
                                        : (isMouseOverButton ? textColour
                                                             : secondaryTextColour));
        graphics.setFont (juce::FontOptions { 10.0f, juce::Font::bold });
        graphics.drawFittedText (button.getButtonText(),
                                 bounds.toNearestInt().reduced (4, 0),
                                 juce::Justification::centredLeft,
                                 1);
    }

    void drawComboBox (juce::Graphics& graphics,
                       int width,
                       int height,
                       bool,
                       int,
                       int,
                       int,
                       int,
                       juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f,
                                              static_cast<float> (width),
                                              static_cast<float> (height))
                          .reduced (0.5f);
        graphics.setColour (juce::Colours::white.withAlpha (0.055f));
        graphics.fillRoundedRectangle (bounds, 7.0f);
        graphics.setColour (borderColour);
        graphics.drawRoundedRectangle (bounds, 7.0f, 1.0f);

        juce::Path arrow;
        const auto centreY = bounds.getCentreY();
        arrow.startNewSubPath (bounds.getRight() - 19.0f, centreY - 2.0f);
        arrow.lineTo (bounds.getRight() - 14.0f, centreY + 3.0f);
        arrow.lineTo (bounds.getRight() - 9.0f, centreY - 2.0f);
        graphics.setColour (secondaryTextColour);
        graphics.strokePath (arrow, juce::PathStrokeType (1.5f));

        const auto contextLabel = box.getProperties()[comboContextLabelProperty].toString();

        if (contextLabel.isNotEmpty())
        {
            auto labelBounds = bounds.toNearestInt().reduced (10, 0).withTrimmedRight (20);
            labelBounds.setHeight (13);
            labelBounds.setY (2);
            graphics.setColour (mutedTextColour.withAlpha (0.82f));
            graphics.setFont (juce::FontOptions { 7.5f, juce::Font::bold });
            graphics.drawFittedText (contextLabel,
                                     labelBounds,
                                     juce::Justification::centredLeft,
                                     1);
        }
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        auto textBounds = box.getLocalBounds().reduced (10, 1).withTrimmedRight (20);
        const auto hasContextLabel =
            box.getProperties()[comboContextLabelProperty].toString().isNotEmpty();

        if (hasContextLabel)
            textBounds = textBounds.withTrimmedTop (8);

        label.setBounds (textBounds);
        label.setFont (juce::FontOptions { hasContextLabel ? 10.5f : 12.0f,
                                           juce::Font::bold });
        label.setColour (juce::Label::textColourId, textColour);
    }

    void drawLinearSlider (juce::Graphics& graphics,
                           int x,
                           int y,
                           int width,
                           int height,
                           float,
                           float,
                           float,
                           juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float> (static_cast<float> (x),
                                              static_cast<float> (y),
                                              static_cast<float> (width),
                                              static_cast<float> (height))
                          .reduced (1.0f);

        if (style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical)
        {
            const auto proportion = static_cast<float> (
                (slider.getValue() - slider.getMinimum())
                / juce::jmax (1.0e-9, slider.getMaximum() - slider.getMinimum()));
            graphics.setColour (juce::Colours::black.withAlpha (0.18f));
            graphics.fillRoundedRectangle (bounds, 5.0f);
            graphics.setColour (primaryColour.withAlpha (0.08f + 0.78f * proportion));
            graphics.fillRoundedRectangle (bounds, 5.0f);
            graphics.setColour (proportion > 0.6f ? textColour
                                                  : secondaryTextColour.withAlpha (0.9f));
            graphics.setFont (juce::FontOptions {
                static_cast<float> (juce::jlimit (7, 11, height - 2)),
                juce::Font::bold });
            graphics.drawFittedText (slider.getTextFromValue (slider.getValue()),
                                     bounds.toNearestInt().reduced (3, 1),
                                     juce::Justification::centred,
                                     1);
            return;
        }

        LookAndFeel_V4::drawLinearSlider (graphics,
                                          x,
                                          y,
                                          width,
                                          height,
                                          0.0f,
                                          0.0f,
                                          0.0f,
                                          style,
                                          slider);
    }
};

struct SmartDuckingAudioProcessorEditor::ParameterControl final : juce::Component
{
    ParameterControl (const juce::String& title,
                      const juce::String& unit,
                      bool percentage,
                      bool linear)
        : heatmap (linear)
    {
        addAndMakeVisible (label);
        addAndMakeVisible (slider);
        label.setText (title, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, secondaryTextColour);
        label.setFont (juce::FontOptions { 12.5f, juce::Font::bold });

        slider.setSliderStyle (linear ? juce::Slider::LinearBar
                                      : juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (linear ? juce::Slider::NoTextBox
                                       : juce::Slider::TextBoxBelow,
                                false,
                                84,
                                22);
        slider.setColour (juce::Slider::textBoxTextColourId, textColour);
        slider.setColour (juce::Slider::textBoxBackgroundColourId,
                          juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxOutlineColourId,
                          juce::Colours::transparentBlack);

        if (percentage)
        {
            slider.textFromValueFunction = [] (double value)
            {
                return juce::String (juce::roundToInt (value * 100.0)) + "%";
            };
            slider.valueFromTextFunction = [] (const juce::String& text)
            {
                return text.retainCharacters ("-0123456789.").getDoubleValue() / 100.0;
            };
        }
        else
        {
            slider.textFromValueFunction = [unit] (double value)
            {
                const auto decimals = unit == "Hz" ? 0 : (unit.isEmpty() ? 2 : 1);
                return juce::String (value, decimals)
                     + (unit.isEmpty() ? juce::String {} : " " + unit);
            };
            slider.valueFromTextFunction = [] (const juce::String& text)
            {
                return text.retainCharacters ("-0123456789.,")
                           .replaceCharacter (',', '.')
                           .getDoubleValue();
            };
        }
    }

    void paint (juce::Graphics& graphics) override
    {
        if (! heatmap)
            drawCard (graphics, getLocalBounds().toFloat().reduced (1.0f), 8.0f);
    }

    void resized() override
    {
        if (heatmap)
        {
            label.setBounds ({});
            slider.setBounds (getLocalBounds().reduced (1));
            return;
        }

        auto bounds = getLocalBounds().reduced (5);
        label.setBounds (bounds.removeFromTop (24));
        slider.setBounds (bounds.reduced (4, 1));
    }

    juce::Label label;
    juce::Slider slider;
    bool heatmap = false;
};

struct SmartDuckingAudioProcessorEditor::EnvelopePanel final : juce::Component
{
    struct InvisibleSlider final : juce::Slider
    {
        void paint (juce::Graphics&) override {}
    };

    EnvelopePanel()
    {
        for (auto* slider : { &attack, &hold, &release })
        {
            addAndMakeVisible (*slider);
            slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider->setMouseDragSensitivity (160);
            slider->onValueChange = [this] { repaint(); };
        }

        attack.setRange (0.0, 200.0, 0.1);
        hold.setRange (0.0, 500.0, 0.1);
        release.setRange (20.0, 1000.0, 0.1);
        attack.setTooltip (parameterTooltip (smart_ducking::parameter_ids::attackMs));
        hold.setTooltip (parameterTooltip (smart_ducking::parameter_ids::holdMs));
        release.setTooltip (parameterTooltip (smart_ducking::parameter_ids::releaseMs));
    }

    void paint (juce::Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        drawCard (graphics, bounds, 9.0f);
        auto graph = bounds.reduced (22.0f, 14.0f);
        const auto baseY = graph.getBottom() - 7.0f;
        const auto topY = graph.getY() + 8.0f;
        const auto attackNormal = static_cast<float> (attack.getValue() / 200.0);
        const auto holdNormal = static_cast<float> (hold.getValue() / 500.0);
        const auto releaseNormal = static_cast<float> ((release.getValue() - 20.0) / 980.0);
        const auto attackX = graph.getX() + graph.getWidth() * (0.20f + 0.11f * attackNormal);
        const auto holdX = juce::jmin (graph.getRight() - graph.getWidth() * 0.27f,
                                      attackX + graph.getWidth() * (0.20f + 0.13f * holdNormal));
        const auto endX = holdX + (graph.getRight() - holdX)
                                   * (0.72f + 0.25f * releaseNormal);

        juce::Path envelope;
        envelope.startNewSubPath (graph.getX(), baseY);
        envelope.lineTo (attackX, topY);
        envelope.lineTo (holdX, topY);
        envelope.cubicTo (holdX + (endX - holdX) * 0.42f,
                          topY + (baseY - topY) * 0.08f,
                          holdX + (endX - holdX) * 0.68f,
                          baseY - (baseY - topY) * 0.32f,
                          endX,
                          baseY);

        juce::Path fill = envelope;
        fill.lineTo (endX, baseY);
        fill.lineTo (graph.getX(), baseY);
        fill.closeSubPath();
        graphics.setColour (primaryColour.withAlpha (0.13f));
        graphics.fillPath (fill);
        graphics.setColour (primarySoftColour);
        graphics.strokePath (envelope, juce::PathStrokeType (2.2f,
                                                             juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));

        for (const auto point : { juce::Point<float> { graph.getX(), baseY },
                                  juce::Point<float> { attackX, topY },
                                  juce::Point<float> { holdX, topY },
                                  juce::Point<float> { endX, baseY } })
        {
            graphics.setColour (backgroundSoftColour);
            graphics.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (point));
            graphics.setColour (primarySoftColour);
            graphics.drawEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (point), 1.5f);
        }

        graphics.setFont (juce::FontOptions { 9.0f, juce::Font::bold });
        const auto drawEnvelopeValue = [&graphics] (const juce::String& name,
                                                     const juce::String& value,
                                                     juce::Rectangle<float> valueBounds,
                                                     juce::Justification justification)
        {
            graphics.setColour (secondaryTextColour);
            graphics.drawFittedText (name + "  " + value,
                                     valueBounds.toNearestInt(),
                                     justification,
                                     1,
                                     0.75f);
        };

        drawEnvelopeValue ("ATTACK",
                           juce::String (attack.getValue(), 0) + " ms",
                           juce::Rectangle<float> (attackX + 8.0f,
                                                   graph.getCentreY() + 1.0f,
                                                   graph.getWidth() * 0.20f,
                                                   18.0f),
                           juce::Justification::centredLeft);
        drawEnvelopeValue ("HOLD",
                           juce::String (hold.getValue(), 0) + " ms",
                           juce::Rectangle<float> (attackX,
                                                   topY + 10.0f,
                                                   holdX - attackX,
                                                   18.0f),
                           juce::Justification::centred);
        drawEnvelopeValue ("RELEASE",
                           juce::String (release.getValue(), 0) + " ms",
                           juce::Rectangle<float> (holdX + 8.0f,
                                                   graph.getCentreY() + 1.0f,
                                                   juce::jmax (80.0f,
                                                               endX - holdX - 16.0f),
                                                   18.0f),
                           juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (14, 8);
        const auto third = bounds.getWidth() / 3;
        attack.setBounds (bounds.removeFromLeft (third));
        hold.setBounds (bounds.removeFromLeft (third));
        release.setBounds (bounds);
    }

    InvisibleSlider attack;
    InvisibleSlider hold;
    InvisibleSlider release;
};

struct SmartDuckingAudioProcessorEditor::SpectrumPanel final : juce::Component,
                                                                juce::SettableTooltipClient
{
    static constexpr int displayBins = smart_ducking::StereoSpectralLcrProcessor::displayBinCount;

    SpectrumPanel (SmartDuckingAudioProcessor& newProcessor)
        : processor (newProcessor), state (newProcessor.getParameterState())
    {
        setComponentID ("spectrumPanel");
        inputDb.fill (-90.0f);
        outputDb.fill (-90.0f);
        reductionDb.fill (0.0f);
        spectralMaskDb.fill (0.0f);
        addChildComponent (markerEditor);
        markerEditor.setColour (juce::TextEditor::backgroundColourId, backgroundSoftColour);
        markerEditor.setColour (juce::TextEditor::textColourId, textColour);
        markerEditor.setColour (juce::TextEditor::outlineColourId, primarySoftColour);
        markerEditor.setColour (juce::TextEditor::focusedOutlineColourId, primarySoftColour);
        markerEditor.setJustification (juce::Justification::centred);
        markerEditor.setInputRestrictions (12, "0123456789.,");
        markerEditor.onReturnKey = [this] { commitMarkerEdit(); };
        markerEditor.onEscapeKey = [this] { cancelMarkerEdit(); };
        markerEditor.onFocusLost = [this]
        {
            if (editingParameter != nullptr)
                cancelMarkerEdit();
        };
        setTooltip ("Spectrum: input, output and spectral gain reduction. Drag a frequency marker to adjust it.");
    }

    void updateSpectrum()
    {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        const auto diagnosticUpdateStart = juce::Time::getHighResolutionTicks();
#endif
        const auto nowMilliseconds = juce::Time::getMillisecondCounterHiRes();
        std::array<float, displayBins> rawInput {};
        std::array<float, displayBins> rawOutput {};
        std::array<float, displayBins> rawReduction {};
        std::array<float, displayBins> rawSpectralMask {};
        const auto frame = processor.copySpectrumDisplay (rawInput.data(),
                                                          rawOutput.data(),
                                                          rawReduction.data(),
                                                          rawSpectralMask.data(),
                                                          displayBins);

        if (frame != 0 && frame != lastDisplayFrame)
        {
            lastDisplayFrame = frame;
            lastSpectrumFrameMilliseconds = nowMilliseconds;
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            const auto rawInputRange = std::minmax_element (rawInput.begin(), rawInput.end());
            const auto rawOutputRange = std::minmax_element (rawOutput.begin(), rawOutput.end());
            const auto rawInputMinimumDb = *rawInputRange.first;
            const auto rawInputMaximumDb = *rawInputRange.second;
            const auto rawOutputMinimumDb = *rawOutputRange.first;
            const auto rawOutputMaximumDb = *rawOutputRange.second;
#endif

            // Retain the proven analyser path from the earlier UI: display the
            // input/output FFT frames directly. Re-anchor their absolute level
            // to the processor's input meter so backend-specific FFT scaling
            // cannot collapse both curves onto the floor.
            const auto inputPeak = std::max ({ processor.getMainInputPeakLeft(),
                                               processor.getMainInputPeakCentre(),
                                               processor.getMainInputPeakRight(),
                                               processor.getMainInputPeakLeftSurround(),
                                               processor.getMainInputPeakRightSurround() });
            const auto spectrumPeak = *std::max_element (rawInput.begin(), rawInput.end());

            if (inputPeak > 1.0e-8f && std::isfinite (spectrumPeak))
            {
                const auto targetPeakDb = juce::Decibels::gainToDecibels (inputPeak, -160.0f);
                const auto targetDisplayOffsetDb = targetPeakDb - spectrumPeak;

                if (! hasSpectrumCalibration)
                {
                    spectrumDisplayOffsetDb = targetDisplayOffsetDb;
                    hasSpectrumCalibration = true;
                }
                else
                {
                    constexpr auto calibrationSmoothing = 0.12f;
                    spectrumDisplayOffsetDb += calibrationSmoothing
                                             * (targetDisplayOffsetDb
                                                - spectrumDisplayOffsetDb);
                }

                for (size_t bin = 0; bin < rawInput.size(); ++bin)
                {
                    rawInput[bin] += spectrumDisplayOffsetDb;
                    rawOutput[bin] += spectrumDisplayOffsetDb;
                }
            }

            smoothSpectrum (rawInput, inputDb, spectrumFloorDb);
            smoothSpectrum (rawOutput, outputDb, spectrumFloorDb);
            smoothSpectrum (rawReduction, reductionDb, 0.0f);
            smoothSpectrum (rawSpectralMask, spectralMaskDb, 0.0f);
            taperSpectrumEdges (inputDb);
            taperSpectrumEdges (outputDb);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
            const auto displayInputRange = std::minmax_element (inputDb.begin(), inputDb.end());
            const auto displayOutputRange = std::minmax_element (outputDb.begin(), outputDb.end());
            processor.recordSpectrumDisplayDiagnostics (
                frame,
                rawInputMinimumDb,
                rawInputMaximumDb,
                rawOutputMinimumDb,
                rawOutputMaximumDb,
                *displayInputRange.first,
                *displayInputRange.second,
                *displayOutputRange.first,
                *displayOutputRange.second,
                spectrumDisplayOffsetDb);
#endif

            hasSpectrumFrame = true;
            repaint();
        }
        else if (hasSpectrumFrame
                 && lastSpectrumFrameMilliseconds > 0.0
                 && nowMilliseconds - lastSpectrumFrameMilliseconds
                        >= staleSpectrumDelayMilliseconds)
        {
            auto maximumSignalDb = spectrumFloorDb;
            auto maximumReduction = 0.0f;

            for (size_t bin = 0; bin < inputDb.size(); ++bin)
            {
                inputDb[bin] += staleSpectrumDecay
                              * (spectrumFloorDb - inputDb[bin]);
                outputDb[bin] += staleSpectrumDecay
                               * (spectrumFloorDb - outputDb[bin]);
                reductionDb[bin] *= 1.0f - staleSpectrumDecay;
                spectralMaskDb[bin] *= 1.0f - staleSpectrumDecay;
                maximumSignalDb = std::max ({ maximumSignalDb,
                                              inputDb[bin],
                                              outputDb[bin] });
                maximumReduction = std::max ({ maximumReduction,
                                               reductionDb[bin],
                                               spectralMaskDb[bin] });
            }

            taperSpectrumEdges (inputDb);
            taperSpectrumEdges (outputDb);

            if (maximumSignalDb <= spectrumFloorDb + 0.15f
                && maximumReduction <= 0.02f)
            {
                inputDb.fill (spectrumFloorDb);
                outputDb.fill (spectrumFloorDb);
                reductionDb.fill (0.0f);
                spectralMaskDb.fill (0.0f);
                spectrumDisplayOffsetDb = 0.0f;
                lastSpectrumFrameMilliseconds = 0.0;
                hasSpectrumFrame = false;
                hasSpectrumCalibration = false;
            }

            repaint();
        }
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        processor.recordSpectrumUpdateDuration (static_cast<std::uint64_t> (
            juce::Time::getHighResolutionTicks() - diagnosticUpdateStart));
#endif
    }

    void paint (juce::Graphics& graphics) override
    {
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        const auto diagnosticPaintStart = juce::Time::getHighResolutionTicks();
#endif
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        drawCard (graphics, bounds, 10.0f);
        auto content = bounds.reduced (12.0f, 10.0f);
        spectrumBounds = content.withTrimmedLeft (30.0f).withTrimmedRight (30.0f);
        drawSpectrum (graphics, spectrumBounds);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
        processor.recordSpectrumPaintDuration (static_cast<std::uint64_t> (
            juce::Time::getHighResolutionTicks() - diagnosticPaintStart));
#endif
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (markerEditor.isVisible()
            && ! markerEditor.getBounds().contains (event.getPosition()))
        {
            cancelMarkerEdit();
            return;
        }

        if (! spectrumBounds.contains (event.position))
            return;

        if (isNearWeightNode (event.position, 14.0f))
        {
            if (event.mods.isPopupMenu())
            {
                resetWeightNode();
                return;
            }

            beginWeightNodeDrag (event);
            return;
        }

        draggedParameter = parameterNear (event.position, 12.0f);

        if (event.mods.isPopupMenu())
        {
            if (draggedParameter != nullptr)
                startMarkerEdit (*draggedParameter, event.position);

            draggedParameter = nullptr;
            return;
        }

        if (draggedParameter != nullptr)
        {
            draggedParameter->beginChangeGesture();
            updateDraggedParameter (event.position.x);
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (weightNodeDragging)
            updateWeightNodeDrag (event);
        else if (draggedParameter != nullptr)
            updateDraggedParameter (event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (weightNodeDragging)
        {
            endWeightNodeDrag();
            return;
        }

        if (draggedParameter != nullptr)
        {
            draggedParameter->endChangeGesture();
            draggedParameter = nullptr;
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (draggedParameter != nullptr)
        {
            draggedParameter->endChangeGesture();
            draggedParameter = nullptr;
        }

        if (isNearWeightNode (event.position, 14.0f))
        {
            resetWeightNode();
            return;
        }

        if (auto* parameter = parameterAtLabel (event.position))
        {
            startMarkerEdit (*parameter, event.position);
            return;
        }

        if (auto* parameter = parameterNear (event.position, 12.0f))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
            parameter->endChangeGesture();
            repaint();
        }
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        if (isNearWeightNode (event.position, 12.0f))
        {
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            setTooltip ("Weight node: drag for frequency/gain, Shift-drag or mouse wheel for Q, double-click to reset.");
        }
        else if (auto* parameter = parameterNear (event.position, 10.0f))
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            setTooltip (tooltipForMarker (*parameter));
        }
        else
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            setTooltip ("Spectrum: input, output and spectral gain reduction. Hover over a marker for controls.");
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        crossoverWheelAccumulator = 0.0f;
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (isOverCrossoverDragArea (event.position))
        {
            if (auto* slope = state.getParameter (
                    smart_ducking::parameter_ids::crossoverSlope))
            {
                constexpr auto wheelEpsilon = 1.0e-7f;
                const auto direction = std::abs (wheel.deltaY) > wheelEpsilon
                    ? wheel.deltaY
                    : wheel.deltaX;

                if (std::abs (direction) > wheelEpsilon)
                {
                    if (crossoverWheelAccumulator * direction < 0.0f)
                        crossoverWheelAccumulator = 0.0f;

                    crossoverWheelAccumulator += direction;
                    const auto now = juce::Time::getMillisecondCounterHiRes();

                    // macOS trackpads report many very small smooth deltas.
                    // Step on the first non-zero event, then rate-limit the
                    // continuing gesture instead of requiring a large delta.
                    if (wheel.isSmooth
                        && now - lastCrossoverWheelStepMilliseconds < 120.0)
                        return;

                    const auto current = juce::roundToInt (
                        slope->convertFrom0to1 (slope->getValue()));
                    const auto next = juce::jlimit (
                        0, 4, current + (crossoverWheelAccumulator > 0.0f ? 1 : -1));
                    crossoverWheelAccumulator = 0.0f;
                    lastCrossoverWheelStepMilliseconds = now;

                    if (next != current)
                    {
                        slope->beginChangeGesture();
                        slope->setValueNotifyingHost (slope->convertTo0to1 (
                            static_cast<float> (next)));
                        slope->endChangeGesture();
                        repaint();
                    }
                }
            }

            return;
        }

        crossoverWheelAccumulator = 0.0f;

        if (! isNearWeightNode (event.position, 18.0f))
        {
            Component::mouseWheelMove (event, wheel);
            return;
        }

        if (auto* q = state.getParameter (smart_ducking::parameter_ids::weightQ))
        {
            const auto current = q->convertFrom0to1 (q->getValue());
            const auto next = current + 0.25f * wheel.deltaY;
            q->beginChangeGesture();
            q->setValueNotifyingHost (juce::jlimit (0.0f,
                                                     1.0f,
                                                     q->convertTo0to1 (next)));
            q->endChangeGesture();
            repaint();
        }
    }

private:
    static constexpr std::array<const char*, 3> markerParameterIds {
        smart_ducking::parameter_ids::crossoverHz,
        smart_ducking::parameter_ids::spectralLowHz,
        smart_ducking::parameter_ids::spectralHighHz
    };

    static constexpr std::array<const char*, 3> markerNames {
        "CROSSOVER", "LOW", "HIGH"
    };

    static constexpr int crossoverCurvePoints = 257;
    static constexpr double staleSpectrumDelayMilliseconds = 120.0;
    static constexpr float staleSpectrumDecay = 0.34f;

    void smoothSpectrum (const std::array<float, displayBins>& source,
                         std::array<float, displayBins>& destination,
                         float floorValue)
    {
        std::array<float, displayBins> frequencySmoothed {};

        for (int bin = 0; bin < displayBins; ++bin)
        {
            auto weightedValue = 0.0f;
            auto totalWeight = 0.0f;

            const auto radius = floorValue < 0.0f ? 5 : 3;

            for (int offset = -radius; offset <= radius; ++offset)
            {
                const auto neighbour = juce::jlimit (0, displayBins - 1, bin + offset);
                const auto weight = static_cast<float> (radius + 1 - std::abs (offset));
                const auto sample = source[static_cast<size_t> (neighbour)];
                weightedValue += (std::isfinite (sample) ? sample : floorValue) * weight;
                totalWeight += weight;
            }

            frequencySmoothed[static_cast<size_t> (bin)] = weightedValue / totalWeight;
        }

        for (int bin = 0; bin < displayBins; ++bin)
        {
            const auto index = static_cast<size_t> (bin);
            const auto target = frequencySmoothed[index];

            if (! hasSpectrumFrame)
            {
                destination[index] = juce::jmax (floorValue, target);
                continue;
            }

            const auto rising = target > destination[index];
            const auto coefficient = floorValue < 0.0f
                                   ? (rising ? 0.34f : 0.16f)
                                   : (rising ? 0.36f : 0.14f);
            destination[index] += coefficient * (target - destination[index]);
            destination[index] = juce::jmax (floorValue, destination[index]);
        }
    }

    static void taperSpectrumEdges (std::array<float, displayBins>& values) noexcept
    {
        constexpr auto taperBins = 5;

        for (int bin = 0; bin < taperBins; ++bin)
        {
            const auto normal = static_cast<float> (bin)
                              / static_cast<float> (taperBins);
            const auto blend = normal * normal * (3.0f - 2.0f * normal);
            const auto left = static_cast<size_t> (bin);
            const auto right = static_cast<size_t> (displayBins - 1 - bin);
            values[left] = juce::jmap (blend, spectrumFloorDb, values[left]);
            values[right] = juce::jmap (blend, spectrumFloorDb, values[right]);
        }

        values.front() = spectrumFloorDb;
        values.back() = spectrumFloorDb;
    }

    float getMaximumDisplayFrequency() const noexcept
    {
        return smart_ducking::StereoSpectralLcrProcessor::getDisplayMaximumFrequency();
    }

    float frequencyToX (float frequency, juce::Rectangle<float> bounds) const noexcept
    {
        constexpr auto minimumFrequency =
            smart_ducking::StereoSpectralLcrProcessor::minimumDisplayFrequencyHz;
        const auto maximumFrequency = getMaximumDisplayFrequency();
        const auto normal = std::log (juce::jlimit (minimumFrequency,
                                                    maximumFrequency,
                                                    frequency)
                                      / minimumFrequency)
                          / std::log (maximumFrequency / minimumFrequency);
        return bounds.getX() + static_cast<float> (normal) * bounds.getWidth();
    }

    float dbToY (float decibels, juce::Rectangle<float> bounds) const noexcept
    {
        return juce::jmap (juce::jlimit (spectrumFloorDb, 6.0f, decibels),
                           spectrumFloorDb,
                           6.0f,
                           bounds.getBottom() - 2.0f,
                           bounds.getY() + 2.0f);
    }

    void updateCrossoverGeometry (juce::Rectangle<float> bounds)
    {
        const auto* parameter = state.getParameter (
            smart_ducking::parameter_ids::crossoverHz);

        if (parameter == nullptr)
            return;

        const auto frequency = parameter->convertFrom0to1 (parameter->getValue());
        const auto* slopeParameter = state.getParameter (
            smart_ducking::parameter_ids::crossoverSlope);
        const auto slopeIndex = slopeParameter != nullptr
            ? juce::roundToInt (slopeParameter->convertFrom0to1 (
                slopeParameter->getValue()))
            : 2;
        const auto slopeDbPerOctave = 24 + 12 * juce::jlimit (0, 4, slopeIndex);
        const auto maximumDisplayFrequency = getMaximumDisplayFrequency();

        if (std::abs (frequency - cachedCrossoverFrequency) < 0.001f
            && slopeDbPerOctave == cachedCrossoverSlopeDbPerOctave
            && std::abs (maximumDisplayFrequency
                         - cachedMaximumDisplayFrequency) < 0.001f
            && bounds == cachedCrossoverBounds)
            return;

        cachedCrossoverFrequency = frequency;
        cachedCrossoverSlopeDbPerOctave = slopeDbPerOctave;
        cachedMaximumDisplayFrequency = maximumDisplayFrequency;
        cachedCrossoverBounds = bounds;

        for (int point = 0; point < crossoverCurvePoints; ++point)
        {
            const auto normal = static_cast<float> (point)
                              / static_cast<float> (crossoverCurvePoints - 1);
            const auto curveFrequency = 20.0f
                                      * std::pow (maximumDisplayFrequency / 20.0f,
                                                  normal);
            const auto highMagnitude = smart_ducking::crossoverHighMagnitude (
                curveFrequency, frequency, static_cast<float> (slopeDbPerOctave));
            const auto lowMagnitude = 1.0f - highMagnitude;
            const auto lowPassDb = juce::Decibels::gainToDecibels (
                lowMagnitude, spectrumFloorDb);
            const auto highPassDb = juce::Decibels::gainToDecibels (
                highMagnitude, spectrumFloorDb);
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto index = static_cast<size_t> (point);
            crossoverLowPoints[index] = { x, dbToY (lowPassDb, bounds) };
            crossoverHighPoints[index] = { x, dbToY (highPassDb, bounds) };
        }

        const auto rebuildPath = [&] (juce::Path& path, const auto& points)
        {
            path.clear();
            path.startNewSubPath (bounds.getX(), points.front().y);

            for (const auto& point : points)
                path.lineTo (point);

            path.lineTo (bounds.getRight(), points.back().y);
        };

        rebuildPath (crossoverLowPath, crossoverLowPoints);
        rebuildPath (crossoverHighPath, crossoverHighPoints);
    }

    juce::Path makeSpectrumPath (const std::array<float, displayBins>& values,
                                 juce::Rectangle<float> bounds) const
    {
        juce::Path path;
        path.preallocateSpace (displayBins * 3);

        for (int bin = 0; bin < displayBins; ++bin)
        {
            const auto normal = static_cast<float> (bin)
                              / static_cast<float> (displayBins - 1);
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto y = dbToY (values[static_cast<size_t> (bin)], bounds);

            if (bin == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        return path;
    }

    void drawSpectrum (juce::Graphics& graphics, juce::Rectangle<float> bounds)
    {
        graphics.setColour (juce::Colours::black.withAlpha (0.18f));
        graphics.fillRoundedRectangle (bounds, 7.0f);

        graphics.setFont (juce::FontOptions { 10.0f });
        const auto spectrumAxisX = bounds.getX() - 4.0f;
        for (const auto db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            const auto y = dbToY (db, bounds);
            graphics.setColour (juce::Colours::white.withAlpha (0.065f));
            graphics.drawHorizontalLine (juce::roundToInt (y), bounds.getX(), bounds.getRight());
            graphics.setColour (secondaryTextColour.withAlpha (0.42f));
            graphics.drawHorizontalLine (juce::roundToInt (y),
                                         spectrumAxisX - 7.0f,
                                         spectrumAxisX);
            graphics.setColour (mutedTextColour.withAlpha (0.7f));
            graphics.drawText (juce::String (juce::roundToInt (db)),
                               juce::Rectangle<float> (spectrumAxisX - 25.0f,
                                                       y - 8.0f,
                                                       18.0f,
                                                       16.0f),
                               juce::Justification::centredRight);
        }

        for (const auto frequency : { 100.0f, 1000.0f, 10000.0f, 20000.0f })
        {
            if (frequency > getMaximumDisplayFrequency())
                continue;

            const auto x = frequencyToX (frequency, bounds);
            auto labelWouldOverlapMarker = false;

            for (const auto* parameterId : markerParameterIds)
                if (const auto* parameter = state.getParameter (parameterId))
                    labelWouldOverlapMarker = labelWouldOverlapMarker
                        || std::abs (frequencyToX (
                                         parameter->convertFrom0to1 (parameter->getValue()),
                                         bounds)
                                     - x) < 28.0f;

            graphics.setColour (juce::Colours::white.withAlpha (0.065f));
            graphics.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());

            if (! labelWouldOverlapMarker)
            {
                graphics.setColour (mutedTextColour.withAlpha (0.78f));
                const auto labelBounds = juce::Rectangle<float> (
                    juce::jlimit (bounds.getX(), bounds.getRight() - 44.0f, x - 22.0f),
                    bounds.getBottom() - 54.0f,
                    44.0f,
                    18.0f);
                graphics.drawText (frequency >= 1000.0f
                                       ? juce::String (juce::roundToInt (frequency / 1000.0f)) + "k"
                                       : juce::String (juce::roundToInt (frequency)),
                                   labelBounds,
                                   juce::Justification::centred);
            }
        }

        const auto maskMaximumHeight = bounds.getHeight() * 0.46f;
        juce::Path maskArea;
        juce::Path maskBoundary;
        maskArea.preallocateSpace (displayBins * 3 + 8);
        maskBoundary.preallocateSpace (displayBins * 3);
        maskArea.startNewSubPath (bounds.getX(), bounds.getY());

        for (int bin = 0; bin < displayBins; ++bin)
        {
            const auto normal = static_cast<float> (bin)
                              / static_cast<float> (displayBins - 1);
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto mask = spectralMaskDb[static_cast<size_t> (bin)];
            const auto y = bounds.getY()
                         + maskMaximumHeight
                               * juce::jlimit (0.0f,
                                               1.0f,
                                               mask / maximumDisplayedReductionDb);

            if (bin == 0)
                maskBoundary.startNewSubPath (x, y);
            else
                maskBoundary.lineTo (x, y);

            maskArea.lineTo (x, y);
        }

        maskArea.lineTo (bounds.getRight(), bounds.getY());
        maskArea.closeSubPath();
        graphics.setColour (maskColour.withAlpha (0.58f));
        graphics.fillPath (maskArea);
        graphics.setColour (maskColour);
        graphics.strokePath (maskBoundary, juce::PathStrokeType (1.7f,
                                                                 juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));

        graphics.setFont (juce::FontOptions { 10.0f });
        const auto maskAxisX = bounds.getRight() + 4.0f;
        for (const auto reduction : { 0.0f, 6.0f, 12.0f, 18.0f })
        {
            const auto y = bounds.getY()
                         + maskMaximumHeight * reduction / maximumDisplayedReductionDb;
            graphics.setColour (maskColour.withAlpha (0.42f));
            graphics.drawHorizontalLine (juce::roundToInt (y),
                                         maskAxisX,
                                         maskAxisX + 7.0f);
            graphics.setColour (maskColour.withAlpha (0.72f));
            graphics.drawText (juce::String (juce::roundToInt (reduction)),
                               juce::Rectangle<float> (maskAxisX + 7.0f,
                                                       y - 8.0f,
                                                       18.0f,
                                                       16.0f),
                               juce::Justification::centredLeft);
        }

        const auto inputPath = makeSpectrumPath (inputDb, bounds);
        const auto outputPath = makeSpectrumPath (outputDb, bounds);
        auto inputArea = inputPath;
        inputArea.lineTo (bounds.getRight(), dbToY (spectrumFloorDb, bounds));
        inputArea.lineTo (bounds.getX(), dbToY (spectrumFloorDb, bounds));
        inputArea.closeSubPath();
        graphics.setColour (primaryColour.withAlpha (0.13f));
        graphics.fillPath (inputArea);

        // Keep a continuous band between input and output so gain reduction
        // remains readable without replacing the actual input spectrum fill.
        juce::Path reductionArea;
        reductionArea.preallocateSpace (displayBins * 6 + 8);

        for (int bin = 0; bin < displayBins; ++bin)
        {
            const auto index = static_cast<size_t> (bin);
            const auto normal = static_cast<float> (bin)
                              / static_cast<float> (displayBins - 1);
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto y = juce::jmin (dbToY (inputDb[index], bounds),
                                       dbToY (outputDb[index], bounds));

            if (bin == 0)
                reductionArea.startNewSubPath (x, y);
            else
                reductionArea.lineTo (x, y);
        }

        for (int bin = displayBins - 1; bin >= 0; --bin)
        {
            const auto index = static_cast<size_t> (bin);
            const auto normal = static_cast<float> (bin)
                              / static_cast<float> (displayBins - 1);
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto y = juce::jmax (dbToY (inputDb[index], bounds),
                                       dbToY (outputDb[index], bounds));
            reductionArea.lineTo (x, y);
        }

        reductionArea.closeSubPath();
        const auto peakReduction = *std::max_element (reductionDb.begin(), reductionDb.end());
        graphics.setColour (primarySoftColour.withAlpha (
            0.05f + 0.13f * juce::jlimit (0.0f, 1.0f, peakReduction / 12.0f)));
        graphics.fillPath (reductionArea);

        updateCrossoverGeometry (bounds);
        const auto drawCrossoverPaths = [&] (juce::Colour colour, float thickness)
        {
            graphics.setColour (colour);
            const juce::PathStrokeType stroke (thickness,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded);
            graphics.strokePath (crossoverLowPath, stroke);
            graphics.strokePath (crossoverHighPath, stroke);
        };

        drawCrossoverPaths (backgroundColour.withAlpha (0.84f), 3.1f);
        drawCrossoverPaths (primarySoftColour.withAlpha (0.68f), 1.4f);

        const auto drawSpectrumPath = [&] (const juce::Path& path,
                                            juce::Colour colour,
                                            float thickness)
        {
            graphics.setColour (colour);
            graphics.strokePath (path,
                                 juce::PathStrokeType (thickness,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        };

        // Dark underlays keep the two boundaries distinct from both the live
        // fill and the amber mask, even where several paths overlap.
        drawSpectrumPath (inputPath, backgroundColour.withAlpha (0.82f), 3.2f);
        drawSpectrumPath (inputPath, secondaryTextColour.withAlpha (0.72f), 1.5f);
        drawSpectrumPath (outputPath, backgroundColour.withAlpha (0.84f), 3.4f);
        drawSpectrumPath (outputPath, textColour.withAlpha (0.82f), 1.7f);
        drawWeightingNode (graphics, bounds);

        for (size_t markerIndex = 0; markerIndex < markerParameterIds.size(); ++markerIndex)
        {
            if (const auto* parameter = state.getParameter (markerParameterIds[markerIndex]))
            {
                const auto frequency = parameter->convertFrom0to1 (parameter->getValue());
                const auto x = frequencyToX (frequency, bounds);
                const auto crossover = markerIndex == 0;
                const auto markerColour = crossover ? primarySoftColour : maskColour;
                const auto markerY = crossover ? dbToY (-6.0f, bounds)
                                               : bounds.getBottom() - 29.0f;
                graphics.setColour (markerColour.withAlpha (0.58f));
                graphics.drawVerticalLine (juce::roundToInt (x),
                                           bounds.getY() + 36.0f,
                                           bounds.getBottom() - 36.0f);

                if (crossover)
                {
                    const auto handle = juce::Rectangle<float> (10.0f, 10.0f)
                                            .withCentre (juce::Point<float> { x, markerY });
                    graphics.setColour (backgroundSoftColour);
                    graphics.fillEllipse (handle);
                    graphics.setColour (markerColour);
                    graphics.drawEllipse (handle, 2.0f);
                }
                else
                {
                    graphics.setColour (backgroundSoftColour);
                    graphics.fillEllipse (x - 6.0f, markerY - 6.0f, 12.0f, 12.0f);
                    graphics.setColour (markerColour);
                    graphics.drawEllipse (x - 6.0f, markerY - 6.0f, 12.0f, 12.0f, 2.0f);
                }

                graphics.setColour (secondaryTextColour);
                graphics.setFont (juce::FontOptions { 10.5f, juce::Font::bold });
                const auto labelWidth = crossover ? 206.0f : 124.0f;
                auto labelBounds = juce::Rectangle<float> (x - 0.5f * labelWidth,
                                                            crossover ? bounds.getY() + 1.0f
                                                                      : bounds.getBottom() - 20.0f,
                                                            labelWidth,
                                                            20.0f);
                labelBounds.setX (juce::jlimit (bounds.getX(),
                                                bounds.getRight() - labelBounds.getWidth(),
                                                labelBounds.getX()));
                markerLabelBounds[markerIndex] = labelBounds;
                auto markerText = juce::String (markerNames[markerIndex]) + "  "
                                + frequencyText (frequency);

                if (crossover)
                    markerText += " - "
                               + juce::String (cachedCrossoverSlopeDbPerOctave)
                               + " dB/oct";

                graphics.drawText (markerText,
                                   labelBounds,
                                   juce::Justification::centred);
            }
        }

        auto legend = juce::Rectangle<float> (bounds.getX() + 38.0f,
                                               bounds.getY() + 42.0f,
                                               116.0f,
                                               50.0f);
        graphics.setFont (juce::FontOptions { 10.5f, juce::Font::bold });
        graphics.setColour (secondaryTextColour.withAlpha (0.72f));
        graphics.drawLine (legend.getX(), legend.getY() + 6.0f,
                           legend.getX() + 15.0f, legend.getY() + 6.0f, 1.8f);
        graphics.drawText ("MAIN IN", legend.withTrimmedLeft (22.0f).removeFromTop (15.0f),
                           juce::Justification::centredLeft);
        graphics.setColour (textColour.withAlpha (0.82f));
        graphics.drawLine (legend.getX(), legend.getY() + 22.0f,
                           legend.getX() + 15.0f, legend.getY() + 22.0f, 1.8f);
        graphics.drawText ("OUTPUT",
                           legend.withTrimmedLeft (22.0f).withY (legend.getY() + 16.0f)
                                 .withHeight (15.0f),
                           juce::Justification::centredLeft);
        graphics.setColour (maskColour);
        graphics.drawLine (legend.getX(), legend.getY() + 38.0f,
                           legend.getX() + 15.0f, legend.getY() + 38.0f, 1.8f);
        graphics.drawText ("MASK",
                           legend.withTrimmedLeft (22.0f).withY (legend.getY() + 32.0f)
                                 .withHeight (15.0f),
                           juce::Justification::centredLeft);
    }

    [[nodiscard]] juce::Rectangle<float> weightNodeArea() const noexcept
    {
        return spectrumBounds.reduced (24.0f, 34.0f);
    }

    [[nodiscard]] juce::Point<float> weightNodePosition() const
    {
        const auto area = weightNodeArea();
        const auto* frequency = state.getParameter (
            smart_ducking::parameter_ids::weightFrequencyHz);
        const auto* gain = state.getParameter (smart_ducking::parameter_ids::weightGainDb);

        if (frequency == nullptr || gain == nullptr || area.isEmpty())
            return area.getCentre();

        const auto rawFrequency = frequency->convertFrom0to1 (frequency->getValue());
        const auto rawGain = gain->convertFrom0to1 (gain->getValue());
        return { frequencyToX (rawFrequency, spectrumBounds),
                 area.getBottom() - gain->convertTo0to1 (rawGain) * area.getHeight() };
    }

    [[nodiscard]] bool isNearWeightNode (juce::Point<float> position,
                                         float radius) const
    {
        return position.getDistanceFrom (weightNodePosition()) <= radius;
    }

    [[nodiscard]] bool isOverCrossoverDragArea (juce::Point<float> position) const
    {
        const auto* parameter = parameterNear (position, 12.0f);
        return parameter != nullptr
            && parameter->paramID == smart_ducking::parameter_ids::crossoverHz;
    }

    void drawWeightingNode (juce::Graphics& graphics,
                            juce::Rectangle<float> bounds)
    {
        const auto* frequency = state.getParameter (
            smart_ducking::parameter_ids::weightFrequencyHz);
        const auto* gain = state.getParameter (smart_ducking::parameter_ids::weightGainDb);
        const auto* q = state.getParameter (smart_ducking::parameter_ids::weightQ);

        if (frequency == nullptr || gain == nullptr || q == nullptr)
            return;

        const auto rawFrequency = frequency->convertFrom0to1 (frequency->getValue());
        const auto rawGain = gain->convertFrom0to1 (gain->getValue());
        const auto rawQ = q->convertFrom0to1 (q->getValue());
        const auto area = weightNodeArea();
        const auto baseline = area.getBottom()
                            - gain->convertTo0to1 (0.0f) * area.getHeight();
        const auto nodeY = area.getBottom()
                         - gain->convertTo0to1 (rawGain) * area.getHeight();
        const auto gainOffset = baseline - nodeY;
        const auto sigmaOctaves = 1.0f / juce::jmax (0.2f, 2.0f * rawQ);
        juce::Path curve;

        graphics.setColour (voiceColour.withAlpha (0.12f));
        graphics.drawHorizontalLine (juce::roundToInt (baseline),
                                     area.getX(), area.getRight());

        constexpr int points = 96;
        for (int point = 0; point < points; ++point)
        {
            const auto normal = static_cast<float> (point) / static_cast<float> (points - 1);
            const auto frequencyAtPoint = 20.0f
                * std::pow (getMaximumDisplayFrequency() / 20.0f, normal);
            const auto distance = std::log2 (frequencyAtPoint / rawFrequency);
            const auto bell = std::exp (-0.5f * distance * distance
                                        / (sigmaOctaves * sigmaOctaves));
            const auto x = bounds.getX() + normal * bounds.getWidth();
            const auto y = baseline - gainOffset * bell;

            if (point == 0)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);
        }

        graphics.setColour (voiceColour.withAlpha (0.18f));
        graphics.strokePath (curve, juce::PathStrokeType (4.0f,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
        graphics.setColour (voiceColour.withAlpha (0.82f));
        graphics.strokePath (curve, juce::PathStrokeType (1.4f,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));

        const auto node = weightNodePosition();
        const auto handle = juce::Rectangle<float> (14.0f, 14.0f).withCentre (node);
        graphics.setColour (backgroundSoftColour);
        graphics.fillEllipse (handle);
        graphics.setColour (voiceColour);
        graphics.fillEllipse (handle.reduced (3.0f));
        graphics.drawEllipse (handle, 1.5f);

        const auto preferredLabelY = rawGain >= 0.0f ? node.y - 28.0f : node.y + 10.0f;
        auto label = juce::Rectangle<float> (node.x - 88.0f,
                                             juce::jlimit (bounds.getY() + 2.0f,
                                                           bounds.getBottom() - 20.0f,
                                                           preferredLabelY),
                                             176.0f,
                                             18.0f);
        label.setX (juce::jlimit (bounds.getX(),
                                  bounds.getRight() - label.getWidth(),
                                  label.getX()));
        graphics.setColour (voiceColour.withAlpha (0.88f));
        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.drawText ("WEIGHT  " + frequencyText (rawFrequency)
                               + "  " + juce::String (rawGain, 1) + " dB"
                               + "  Q " + juce::String (rawQ, 2),
                           label,
                           juce::Justification::centred);
    }

    void beginWeightNodeDrag (const juce::MouseEvent& event)
    {
        auto* frequency = state.getParameter (
            smart_ducking::parameter_ids::weightFrequencyHz);
        auto* gain = state.getParameter (smart_ducking::parameter_ids::weightGainDb);
        auto* q = state.getParameter (smart_ducking::parameter_ids::weightQ);

        if (frequency == nullptr || gain == nullptr || q == nullptr)
            return;

        weightNodeDragging = true;
        weightDragAdjustsQ = event.mods.isShiftDown();
        weightDragStartY = event.position.y;
        weightDragStartQ = q->convertFrom0to1 (q->getValue());
        frequency->beginChangeGesture();
        gain->beginChangeGesture();
        q->beginChangeGesture();
        updateWeightNodeDrag (event);
    }

    void updateWeightNodeDrag (const juce::MouseEvent& event)
    {
        auto* frequency = state.getParameter (
            smart_ducking::parameter_ids::weightFrequencyHz);
        auto* gain = state.getParameter (smart_ducking::parameter_ids::weightGainDb);
        auto* q = state.getParameter (smart_ducking::parameter_ids::weightQ);

        if (! weightNodeDragging || frequency == nullptr || gain == nullptr || q == nullptr)
            return;

        if (weightDragAdjustsQ)
        {
            const auto qRange = q->convertFrom0to1 (1.0f) - q->convertFrom0to1 (0.0f);
            const auto nextQ = weightDragStartQ
                             + (weightDragStartY - event.position.y)
                                   * qRange / juce::jmax (40.0f, weightNodeArea().getHeight());
            q->setValueNotifyingHost (juce::jlimit (0.0f,
                                                     1.0f,
                                                     q->convertTo0to1 (nextQ)));
        }
        else
        {
            const auto normalX = juce::jlimit (0.0f,
                                               1.0f,
                                               (event.position.x - spectrumBounds.getX())
                                                   / spectrumBounds.getWidth());
            const auto rawFrequency = 20.0f
                                    * std::pow (getMaximumDisplayFrequency() / 20.0f,
                                                normalX);
            const auto area = weightNodeArea();
            const auto normalGain = juce::jlimit (0.0f,
                                                  1.0f,
                                                  (area.getBottom() - event.position.y)
                                                      / area.getHeight());
            frequency->setValueNotifyingHost (juce::jlimit (
                0.0f, 1.0f, frequency->convertTo0to1 (rawFrequency)));
            gain->setValueNotifyingHost (normalGain);
        }

        repaint();
    }

    void endWeightNodeDrag()
    {
        if (! weightNodeDragging)
            return;

        for (const auto* parameterId : { smart_ducking::parameter_ids::weightFrequencyHz,
                                         smart_ducking::parameter_ids::weightGainDb,
                                         smart_ducking::parameter_ids::weightQ })
            if (auto* parameter = state.getParameter (parameterId))
                parameter->endChangeGesture();

        weightNodeDragging = false;
    }

    void resetWeightNode()
    {
        for (const auto* parameterId : { smart_ducking::parameter_ids::weightFrequencyHz,
                                         smart_ducking::parameter_ids::weightGainDb,
                                         smart_ducking::parameter_ids::weightQ })
        {
            if (auto* parameter = state.getParameter (parameterId))
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->getDefaultValue());
                parameter->endChangeGesture();
            }
        }

        repaint();
    }

    void updateDraggedParameter (float mouseX)
    {
        if (draggedParameter == nullptr)
            return;

        const auto normal = juce::jlimit (0.0f,
                                          1.0f,
                                          (mouseX - spectrumBounds.getX())
                                              / spectrumBounds.getWidth());
        const auto frequency = 20.0f
                             * std::pow (getMaximumDisplayFrequency() / 20.0f, normal);
        draggedParameter->setValueNotifyingHost (draggedParameter->convertTo0to1 (frequency));
        repaint();
    }

    juce::RangedAudioParameter* parameterNear (juce::Point<float> position,
                                                float maximumDistance) const
    {
        if (! spectrumBounds.contains (position))
            return nullptr;

        juce::RangedAudioParameter* result = nullptr;
        auto closestDistance = maximumDistance;

        for (const auto* parameterId : markerParameterIds)
        {
            if (auto* parameter = state.getParameter (parameterId))
            {
                const auto frequency = parameter->convertFrom0to1 (parameter->getValue());
                const auto distance = std::abs (frequencyToX (frequency, spectrumBounds)
                                                - position.x);

                if (distance <= closestDistance)
                {
                    closestDistance = distance;
                    result = parameter;
                }
            }
        }

        return result;
    }

    juce::RangedAudioParameter* parameterAtLabel (juce::Point<float> position) const
    {
        for (size_t markerIndex = 0; markerIndex < markerLabelBounds.size(); ++markerIndex)
            if (markerLabelBounds[markerIndex].contains (position))
                return state.getParameter (markerParameterIds[markerIndex]);

        return nullptr;
    }

    juce::String tooltipForMarker (const juce::RangedAudioParameter& parameter) const
    {
        if (parameter.paramID == smart_ducking::parameter_ids::crossoverHz)
            return "Crossover between broadband and spectral processing. Drag to adjust frequency; use the mouse wheel over the marker for 24/36/48/60/72 dB/oct; double-click the marker to reset frequency; right-click or double-click its value to type a frequency.";

        if (parameter.paramID == smart_ducking::parameter_ids::spectralLowHz)
            return "Lowest frequency affected by spectral ducking. Drag to adjust; double-click the marker to reset; right-click or double-click its value to type a frequency.";

        return "Highest frequency affected by spectral ducking. Drag to adjust; double-click the marker to reset; right-click or double-click its value to type a frequency.";
    }

    void startMarkerEdit (juce::RangedAudioParameter& parameter, juce::Point<float> position)
    {
        editingParameter = &parameter;
        auto editorBounds = juce::Rectangle<int> (0, 0, 92, 26);
        editorBounds.setCentre (juce::roundToInt (position.x), juce::roundToInt (position.y));
        editorBounds.setX (juce::jlimit (juce::roundToInt (spectrumBounds.getX()),
                                        juce::roundToInt (spectrumBounds.getRight())
                                            - editorBounds.getWidth(),
                                        editorBounds.getX()));
        editorBounds.setY (juce::jlimit (juce::roundToInt (spectrumBounds.getY()),
                                        juce::roundToInt (spectrumBounds.getBottom())
                                            - editorBounds.getHeight(),
                                        editorBounds.getY()));
        markerEditor.setBounds (editorBounds);
        markerEditor.setText (juce::String (parameter.convertFrom0to1 (parameter.getValue()), 1),
                              false);
        markerEditor.setVisible (true);
        markerEditor.toFront (true);
        markerEditor.grabKeyboardFocus();
        markerEditor.selectAll();
    }

    void commitMarkerEdit()
    {
        auto* parameter = editingParameter;
        editingParameter = nullptr;

        if (parameter != nullptr)
        {
            auto text = markerEditor.getText().replaceCharacter (',', '.');
            if (text.containsAnyOf ("0123456789"))
            {
                const auto value = static_cast<float> (text.getDoubleValue());
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
                parameter->endChangeGesture();
            }
        }

        markerEditor.setVisible (false);
        repaint();
    }

    void cancelMarkerEdit()
    {
        editingParameter = nullptr;
        markerEditor.setVisible (false);
        grabKeyboardFocus();
    }

    SmartDuckingAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    std::array<float, displayBins> inputDb {};
    std::array<float, displayBins> outputDb {};
    std::array<float, displayBins> reductionDb {};
    std::array<float, displayBins> spectralMaskDb {};
    std::array<juce::Point<float>, crossoverCurvePoints> crossoverLowPoints {};
    std::array<juce::Point<float>, crossoverCurvePoints> crossoverHighPoints {};
    juce::Path crossoverLowPath;
    juce::Path crossoverHighPath;
    std::array<juce::Rectangle<float>, 3> markerLabelBounds {};
    juce::Rectangle<float> spectrumBounds;
    juce::TextEditor markerEditor;
    juce::RangedAudioParameter* draggedParameter = nullptr;
    juce::RangedAudioParameter* editingParameter = nullptr;
    std::uint64_t lastDisplayFrame = 0;
    double lastSpectrumFrameMilliseconds = 0.0;
    float spectrumDisplayOffsetDb = 0.0f;
    float cachedCrossoverFrequency = -1.0f;
    int cachedCrossoverSlopeDbPerOctave = -1;
    float cachedMaximumDisplayFrequency = -1.0f;
    juce::Rectangle<float> cachedCrossoverBounds;
    float weightDragStartY = 0.0f;
    float weightDragStartQ = 0.8f;
    float crossoverWheelAccumulator = 0.0f;
    double lastCrossoverWheelStepMilliseconds = 0.0;
    bool hasSpectrumFrame = false;
    bool hasSpectrumCalibration = false;
    bool weightNodeDragging = false;
    bool weightDragAdjustsQ = false;
};

SmartDuckingAudioProcessorEditor::SmartDuckingAudioProcessorEditor (
    SmartDuckingAudioProcessor& newProcessor)
    : juce::AudioProcessorEditor (newProcessor),
      audioProcessor (newProcessor),
      rusadLookAndFeel (std::make_unique<RusADLookAndFeel>()),
      spectrumPanel (std::make_unique<SpectrumPanel> (newProcessor))
{
    using namespace smart_ducking::parameter_ids;

    globalBypassPanel = std::make_unique<GlobalBypassPanel>();
    globalBypassPanel->setComponentID ("globalBypassPanel");
    RusADLogo = juce::ImageCache::getFromMemory (BinaryData::RusADLogo_png,
                                                 BinaryData::RusADLogo_pngSize);
    setOpaque (true);
    setLookAndFeel (rusadLookAndFeel.get());
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 850);
    setResizable (true, true);
    setResizeLimits (audioProcessor.isSurroundVariant() ? 960 : 820,
                     560, 1400, 900);
    setSize (editorWidth, editorHeight);

    addAndMakeVisible (presetChoice);
    presetChoice.setComponentID ("presetChoice");
    presetChoice.setJustificationType (juce::Justification::centred);
    presetChoice.setTooltip (
        "Load a factory or user preset, save the current settings, or delete a user preset.");
    presetChoice.onChange = [this] { handlePresetSelection(); };
    userPresets = presetStore.loadAll();
    selectedFactoryPreset = audioProcessor.getCurrentProgram();
    for (int index = 0; index < audioProcessor.getNumPrograms(); ++index)
        if (audioProcessor.doesPresetStateMatch (
                audioProcessor.createFactoryPresetState (index)))
        {
            selectedFactoryPreset = index;
            break;
        }
    for (const auto& preset : userPresets)
        if (audioProcessor.doesPresetStateMatch (preset.state))
        {
            selectedFactoryPreset = -1;
            selectedUserPresetName = preset.name;
            break;
        }
    lastFactoryProgramChangeCount = audioProcessor.getFactoryProgramChangeCount();
    rebuildPresetMenu();

    globalBypassButton.getProperties().set (bypassIconProperty, true);
    globalBypassButton.setComponentID ("globalBypassButton");
    globalBypassButton.setClickingTogglesState (true);
    globalBypassButton.setTooltip (
        "Bypass ducking and monitoring while preserving channel-order routing and Downmix.");
    globalBypassAttachment = std::make_unique<ButtonAttachment> (
        audioProcessor.getParameterState(), globalBypass, globalBypassButton);

    addAndMakeVisible (stageControlPanel);
    stageControlPanel.setComponentID ("stageControlPanel");
    addAndMakeVisible (*globalBypassPanel);
    globalBypassPanel->addAndMakeVisible (globalBypassButton);

    for (size_t slot = 0; slot < comparisonButtons.size(); ++slot)
    {
        auto& button = comparisonButtons[slot];
        addAndMakeVisible (button);
        button.setClickingTogglesState (false);
        button.setTooltip (
            "Switch comparison slot: the active settings are stored before the selected slot is recalled.");
        button.onClick = [this, slot]
        {
            audioProcessor.switchComparisonSlot (static_cast<int> (slot));
            for (size_t index = 0; index < comparisonButtons.size(); ++index)
                comparisonButtons[index].setToggleState (index == slot,
                                                          juce::dontSendNotification);
        };
    }
    comparisonButtons[0].setToggleState (true, juce::dontSendNotification);

    const std::array<const char*, 3> stageNames { "CROSSOVER", "WIDEBAND", "SPECTRAL" };
    const std::array<const char*, 3> bypassParameterIds {
        crossoverBypass, widebandBypass, spectralBypass
    };
    const std::array<smart_ducking::MonitorMode, 3> firstMonitorModes {
        smart_ducking::MonitorMode::crossoverLow,
        smart_ducking::MonitorMode::widebandSolo,
        smart_ducking::MonitorMode::spectralSolo
    };
    const std::array<smart_ducking::MonitorMode, 3> secondMonitorModes {
        smart_ducking::MonitorMode::crossoverHigh,
        smart_ducking::MonitorMode::widebandDelta,
        smart_ducking::MonitorMode::spectralDelta
    };

    for (size_t stage = 0; stage < stageLabels.size(); ++stage)
    {
        auto& label = stageLabels[stage];
        stageControlPanel.addAndMakeVisible (label);
        label.setComponentID ("stageLabel" + juce::String (stage));
        label.setText (stageNames[stage], juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredRight);
        label.setColour (juce::Label::textColourId, secondaryTextColour);
        label.setFont (juce::FontOptions { 10.0f, juce::Font::bold });
        label.setInterceptsMouseClicks (false, false);

        for (size_t control = 0; control < 3; ++control)
        {
            auto& button = stageButtons[stage * 3 + control];
            stageControlPanel.addAndMakeVisible (button);
            if (control == 0)
            {
                button.getProperties().set (bypassIconProperty, true);
                button.setComponentID ("stageBypassButton" + juce::String (stage));
            }
            else if (stage == 0)
            {
                button.getProperties().set (crossoverMonitorBandProperty,
                                            static_cast<int> (control));
            }
            button.setTooltip (control == 0
                ? "Bypass this stage independently."
                : stage == 0 && control == 1
                    ? "Monitor only the protected low band without LFE."
                    : stage == 0
                        ? "Monitor only the processed high band without LFE."
                        : control == 1
                    ? "Solo this stage while bypassing the other two."
                    : "Monitor only the signal removed by this stage.");
        }

        auto& bypass = stageButtons[stage * 3];
        bypass.setClickingTogglesState (true);
        stageBypassAttachments[stage] = std::make_unique<ButtonAttachment> (
            audioProcessor.getParameterState(), bypassParameterIds[stage], bypass);
        stageButtons[stage * 3 + 1].onClick = [this, mode = firstMonitorModes[stage]]
        {
            setMonitorMode (mode);
        };
        stageButtons[stage * 3 + 2].onClick = [this, mode = secondMonitorModes[stage]]
        {
            setMonitorMode (mode);
        };
    }

    addAndMakeVisible (*spectrumPanel);
    addAndMakeVisible (pagePanel);

    for (size_t index = 0; index < pageButtons.size(); ++index)
    {
        auto& button = pageButtons[index];
        pagePanel.addAndMakeVisible (button);
        button.setClickingTogglesState (false);
        button.onClick = [this, index]
        {
            setPage (static_cast<Page> (index));
        };
    }

    for (auto* page : { &dynamicsPage, &routingPage })
        pagePanel.addAndMakeVisible (*page);

    addSliderControl (dynamicsPage, dynamicsControls, duckDepthDb, "DUCKING DEPTH", "dB");
    addSliderControl (dynamicsPage, dynamicsControls, spectralAmountDb, "SPECTRAL DEPTH", "dB");
    addSliderControl (dynamicsPage, dynamicsControls, activityThresholdDb, "THRESHOLD", "dBFS");
    addSliderControl (dynamicsPage, dynamicsControls, attackMs, "ATTACK", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, holdMs, "HOLD", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, releaseMs, "RELEASE", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, lookaheadMs, "LOOKAHEAD", "ms");

    dynamicsPage.addAndMakeVisible (fftLabel);
    dynamicsPage.addAndMakeVisible (fftChoice);
    dynamicsPage.addAndMakeVisible (processingModeLabel);
    dynamicsPage.addAndMakeVisible (processingModeChoice);
    processingModeLabel.setText ("PROCESSING MODE", juce::dontSendNotification);
    processingModeLabel.setJustificationType (juce::Justification::centred);
    processingModeLabel.setColour (juce::Label::textColourId, secondaryTextColour);
    processingModeLabel.setFont (juce::FontOptions { 11.0f, juce::Font::bold });
    processingModeChoice.addItem ("QUALITY", 1);
    processingModeChoice.addItem ("NORMAL", 2);
    processingModeChoice.setJustificationType (juce::Justification::centred);
    processingModeChoice.setTooltip (
        "Normal uses the selected complementary FFT-domain crossover and 2x overlap. Quality uses "
        "the selected time-domain Linkwitz-Riley crossover and 4x STFT overlap for the most accurate transient "
        "response at higher CPU cost.");
    processingModeLabel.setTooltip (processingModeChoice.getTooltip());
    processingModeAttachment = std::make_unique<ComboBoxAttachment> (
        audioProcessor.getParameterState(), processingMode, processingModeChoice);

    fftLabel.setText ("FFT RESOLUTION", juce::dontSendNotification);
    fftLabel.setJustificationType (juce::Justification::centred);
    fftLabel.setColour (juce::Label::textColourId, secondaryTextColour);
    fftLabel.setFont (juce::FontOptions { 11.0f, juce::Font::bold });
    fftChoice.addItem ("x1  FAST", 1);
    fftChoice.addItem ("x2  NORMAL", 2);
    fftChoice.addItem ("x4  FINE", 3);
    fftChoice.setJustificationType (juce::Justification::centred);
    fftChoice.setTooltip ("FFT resolution: higher settings improve frequency detail but increase latency and CPU use.");
    fftLabel.setTooltip (fftChoice.getTooltip());
    fftAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.getParameterState(),
                                                          fftResolution,
                                                          fftChoice);

    for (size_t index = 0; index < matrixParameterIds.size(); ++index)
    {
        auto& control = addSliderControl (routingPage,
                                          matrixControls,
                                          matrixParameterIds[index],
                                          "",
                                          "",
                                          true,
                                          true);
        control.label.setVisible (false);
        control.slider.onValueChange = [this, index]
        {
            mirrorMatrixEdit (index);
        };
    }

    const std::array<const char*, 5> matrixChannelNames { "L", "C", "R", "Ls", "Rs" };
    const auto configureHeader = [] (juce::Label& label,
                                     const juce::String& text,
                                     juce::Justification justification)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (justification);
        label.setColour (juce::Label::textColourId, secondaryTextColour);
        label.setFont (juce::FontOptions { 10.0f, juce::Font::bold });
        label.setInterceptsMouseClicks (false, false);
    };

    routingPage.addAndMakeVisible (matrixVoiceHeader);
    routingPage.addAndMakeVisible (matrixOriginalHeader);
    configureHeader (matrixVoiceHeader, "VOICE", juce::Justification::centred);
    configureHeader (matrixOriginalHeader, "ORIGINAL", juce::Justification::centred);

    for (size_t channel = 0; channel < matrixChannelNames.size(); ++channel)
    {
        routingPage.addAndMakeVisible (matrixRowHeaders[channel]);
        routingPage.addAndMakeVisible (matrixColumnHeaders[channel]);
        configureHeader (matrixRowHeaders[channel], matrixChannelNames[channel],
                         juce::Justification::centred);
        configureHeader (matrixColumnHeaders[channel], matrixChannelNames[channel],
                         juce::Justification::centred);
    }

    for (auto* button : { &matrixDiagonalButton, &matrixResetButton,
                          &matrixSymmetryButton })
        routingPage.addAndMakeVisible (*button);

    matrixDiagonalButton.onClick = [this]
    {
        std::array<float, 25> values {};
        for (int channel = 0; channel < 5; ++channel)
            values[static_cast<size_t> (channel * 5 + channel)] = 1.0f;
        applyMatrixValues (values);
    };
    matrixResetButton.onClick = [this] { applyMatrixValues (defaultMatrixValues); };
    matrixSymmetryButton.setClickingTogglesState (true);
    matrixSymmetryButton.onClick = [this] { saveUiState(); };
    matrixDiagonalButton.setTooltip ("Route every voice channel only to the matching main channel.");
    matrixResetButton.setTooltip ("Restore the default ducking matrix.");
    matrixSymmetryButton.setTooltip (
        "Mirror manual matrix edits between L/R and Ls/Rs without rewriting host automation.");

    addAndMakeVisible (voiceFormatChoice);
    voiceFormatChoice.setComponentID ("voiceFormatChoice");
    voiceFormatChoice.getProperties().set (comboContextLabelProperty, "VOICE");
    voiceFormatChoice.addItem ("STEREO", 1);
    voiceFormatChoice.addItem ("SURROUND", 2);
    voiceFormatChoice.setJustificationType (juce::Justification::centred);
    voiceFormatChoice.setTooltip ("Select whether the sidechain voice signal is stereo or surround.");
    voiceFormatAttachment = std::make_unique<ComboBoxAttachment> (
        audioProcessor.getParameterState(), voiceFormat, voiceFormatChoice);

    for (auto* toggle : { &downmixToggle, &dropLfeToggle })
    {
        addAndMakeVisible (*toggle);
        toggle->setVisible (audioProcessor.isSurroundVariant());
    }

    downmixToggle.setTooltip ("Downmix the processed main 5.1 signal to stereo at the output.");
    dropLfeToggle.setTooltip ("Exclude the LFE channel when creating the stereo downmix.");
    downmixAttachment = std::make_unique<ButtonAttachment> (
        audioProcessor.getParameterState(), downmix, downmixToggle);
    dropLfeAttachment = std::make_unique<ButtonAttachment> (
        audioProcessor.getParameterState(), dropLfe, dropLfeToggle);

    if (const auto* downmixValue = audioProcessor.getParameterState().getRawParameterValue (downmix))
        dropLfeToggle.setEnabled (downmixValue->load() >= 0.5f);

    restoreUiState();
    setPage (currentPage);
    audioProcessor.setVisualisationFrameRate (meterRefreshHz);
    audioProcessor.notifyEditorOpened();
    lastObservedProcessBlockCount = audioProcessor.getProcessBlockCount();
    startTimerHz (static_cast<int> (meterRefreshHz));
}

SmartDuckingAudioProcessorEditor::~SmartDuckingAudioProcessorEditor()
{
    stopTimer();
    audioProcessor.notifyEditorClosed();
    tooltipWindow.reset();
    setLookAndFeel (nullptr);
}

juce::ValueTree SmartDuckingAudioProcessorEditor::selectedPresetState() const
{
    return selectedPresetSnapshot;
}

void SmartDuckingAudioProcessorEditor::refreshSelectedPresetSnapshot()
{
    selectedPresetSnapshot = {};
    if (selectedFactoryPreset >= 0
        && selectedFactoryPreset < audioProcessor.getNumPrograms())
    {
        selectedPresetSnapshot = audioProcessor.createFactoryPresetState (
            selectedFactoryPreset);
        return;
    }

    for (const auto& preset : userPresets)
        if (preset.name == selectedUserPresetName)
        {
            selectedPresetSnapshot = preset.state;
            return;
        }
}

void SmartDuckingAudioProcessorEditor::rebuildPresetMenu()
{
    refreshSelectedPresetSnapshot();
    const juce::ScopedValueSetter<bool> rebuilding (rebuildingPresetMenu, true);
    presetChoice.clear (juce::dontSendNotification);
    presetChoice.addSectionHeading ("FACTORY PRESETS");
    for (int index = 0; index < audioProcessor.getNumPrograms(); ++index)
        presetChoice.addItem (audioProcessor.getProgramName (index),
                              factoryPresetItemBase + index);

    if (! userPresets.empty())
    {
        presetChoice.addSeparator();
        presetChoice.addSectionHeading ("USER PRESETS");
        for (size_t index = 0; index < userPresets.size(); ++index)
            presetChoice.addItem (userPresets[index].name,
                                  userPresetItemBase + static_cast<int> (index));
    }

    presetChoice.addSeparator();
    presetChoice.addItem ("SAVE CURRENT AS...", savePresetItemId);
    if (selectedUserPresetName.isNotEmpty())
        presetChoice.addItem ("DELETE \"" + selectedUserPresetName + "\"...",
                              deletePresetItemId);

    lastPresetDisplayText.clear();
    updatePresetDisplay();
}

void SmartDuckingAudioProcessorEditor::handlePresetSelection()
{
    if (rebuildingPresetMenu)
        return;

    const auto selectedId = presetChoice.getSelectedId();
    if (selectedId >= factoryPresetItemBase
        && selectedId < factoryPresetItemBase + audioProcessor.getNumPrograms())
    {
        selectedFactoryPreset = selectedId - factoryPresetItemBase;
        selectedUserPresetName.clear();
        audioProcessor.setCurrentProgram (selectedFactoryPreset);
        lastFactoryProgramChangeCount = audioProcessor.getFactoryProgramChangeCount();
        rebuildPresetMenu();
        return;
    }

    const auto userIndex = selectedId - userPresetItemBase;
    if (userIndex >= 0 && userIndex < static_cast<int> (userPresets.size()))
    {
        const auto& preset = userPresets[static_cast<size_t> (userIndex)];
        if (audioProcessor.applyPresetState (preset.state))
        {
            selectedFactoryPreset = -1;
            selectedUserPresetName = preset.name;
        }
        rebuildPresetMenu();
        return;
    }

    if (selectedId == savePresetItemId)
        showSavePresetDialog();
    else if (selectedId == deletePresetItemId)
        confirmDeleteUserPreset();

    updatePresetDisplay();
}

void SmartDuckingAudioProcessorEditor::updatePresetDisplay()
{
    const auto state = selectedPresetState();
    juce::String name;
    if (selectedFactoryPreset >= 0
        && selectedFactoryPreset < audioProcessor.getNumPrograms())
        name = audioProcessor.getProgramName (selectedFactoryPreset);
    else
        name = selectedUserPresetName;

    if (name.isEmpty())
        name = "CUSTOM";
    else if (! audioProcessor.doesPresetStateMatch (state))
        name += " *";

    if (name != lastPresetDisplayText)
    {
        const juce::ScopedValueSetter<bool> rebuilding (rebuildingPresetMenu, true);
        presetChoice.setText (name, juce::dontSendNotification);
        lastPresetDisplayText = name;
    }
}

void SmartDuckingAudioProcessorEditor::showSavePresetDialog()
{
    if (presetNameDialog != nullptr)
        return;

    presetNameDialog = std::make_unique<juce::AlertWindow> (
        "Save User Preset",
        "Choose a name for the current Smart Ducking settings.",
        juce::MessageBoxIconType::QuestionIcon,
        this);
    presetNameDialog->addTextEditor ("presetName", selectedUserPresetName,
                                     "Name:");
    presetNameDialog->addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    presetNameDialog->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    auto safeThis = juce::Component::SafePointer<SmartDuckingAudioProcessorEditor> (this);
    presetNameDialog->enterModalState (
        true,
        juce::ModalCallbackFunction::create ([safeThis] (int result)
        {
            if (safeThis == nullptr || safeThis->presetNameDialog == nullptr)
                return;

            const auto name = safeThis->presetNameDialog
                                  ->getTextEditorContents ("presetName").trim();
            safeThis->presetNameDialog.reset();
            if (result == 1 && name.isNotEmpty())
                safeThis->saveUserPreset (name);
            else
                safeThis->updatePresetDisplay();
        }),
        false);
}

void SmartDuckingAudioProcessorEditor::saveUserPreset (const juce::String& name)
{
    const auto existing = std::find_if (userPresets.begin(), userPresets.end(),
                                        [&name] (const auto& preset)
    {
        return preset.name.equalsIgnoreCase (name);
    });

    if (existing != userPresets.end())
    {
        const auto existingName = existing->name;
        auto safeThis = juce::Component::SafePointer<SmartDuckingAudioProcessorEditor> (this);
        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions {}
                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle ("Replace User Preset")
                .withMessage ("A preset named \"" + existing->name
                              + "\" already exists. Replace it?")
                .withButton ("REPLACE")
                .withButton ("CANCEL")
                .withAssociatedComponent (this),
            [safeThis, existingName] (int result)
            {
                if (safeThis != nullptr && result == 1)
                {
                    const auto saveResult = safeThis->presetStore.save (
                        existingName, safeThis->audioProcessor.createCurrentPresetState());
                    if (saveResult.wasOk())
                    {
                        safeThis->userPresets = safeThis->presetStore.loadAll();
                        safeThis->selectedFactoryPreset = -1;
                        safeThis->selectedUserPresetName = existingName;
                        safeThis->rebuildPresetMenu();
                    }
                    else
                        juce::AlertWindow::showMessageBoxAsync (
                            juce::MessageBoxIconType::WarningIcon,
                            "Could Not Save Preset", saveResult.getErrorMessage(), "OK",
                            safeThis.getComponent());
                }
                else if (safeThis != nullptr)
                    safeThis->updatePresetDisplay();
            });
        return;
    }

    const auto saveResult = presetStore.save (name, audioProcessor.createCurrentPresetState());
    if (saveResult.failed())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Could Not Save Preset", saveResult.getErrorMessage(), "OK", this);
        updatePresetDisplay();
        return;
    }

    userPresets = presetStore.loadAll();
    selectedFactoryPreset = -1;
    selectedUserPresetName = name;
    rebuildPresetMenu();
}

void SmartDuckingAudioProcessorEditor::confirmDeleteUserPreset()
{
    if (selectedUserPresetName.isEmpty())
        return;

    const auto name = selectedUserPresetName;
    auto safeThis = juce::Component::SafePointer<SmartDuckingAudioProcessorEditor> (this);
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions {}
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Delete User Preset")
            .withMessage ("Delete \"" + name + "\"? This cannot be undone.")
            .withButton ("DELETE")
            .withButton ("CANCEL")
            .withAssociatedComponent (this),
        [safeThis, name] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
            {
                const auto preset = std::find_if (
                    safeThis->userPresets.begin(), safeThis->userPresets.end(),
                    [&name] (const auto& candidate) { return candidate.name == name; });
                const auto removed = preset == safeThis->userPresets.end()
                    || safeThis->presetStore.remove (*preset);
                if (! removed)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Could Not Delete Preset",
                        "The preset file could not be deleted.",
                        "OK", safeThis.getComponent());
                }
                else
                {
                    safeThis->userPresets = safeThis->presetStore.loadAll();
                    safeThis->selectedUserPresetName.clear();
                    safeThis->selectedFactoryPreset =
                        safeThis->audioProcessor.getCurrentProgram();
                }
            }
            safeThis->rebuildPresetMenu();
        });
}

SmartDuckingAudioProcessorEditor::ParameterControl&
SmartDuckingAudioProcessorEditor::addSliderControl (
    juce::Component& page,
    std::vector<std::unique_ptr<ParameterControl>>& collection,
    const char* parameterId,
    const juce::String& title,
    const juce::String& unit,
    bool percentage,
    bool linear)
{
    auto control = std::make_unique<ParameterControl> (title, unit, percentage, linear);
    auto& result = *control;
    const auto tooltip = parameterTooltip (parameterId);
    result.slider.setTooltip (tooltip);
    result.label.setTooltip (tooltip);
    page.addAndMakeVisible (result);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (
        audioProcessor.getParameterState(), parameterId, result.slider));

    // SliderAttachment installs the parameter's default text conversion, so the
    // compact UI formatting must be restored after the attachment is created.
    if (percentage)
    {
        result.slider.textFromValueFunction = [] (double value)
        {
            return juce::String (juce::roundToInt (value * 100.0)) + "%";
        };
        result.slider.valueFromTextFunction = [] (const juce::String& text)
        {
            return text.retainCharacters ("-0123456789.,")
                       .replaceCharacter (',', '.')
                       .getDoubleValue() / 100.0;
        };
    }
    else
    {
        result.slider.textFromValueFunction = [unit] (double value)
        {
            const auto decimals = unit == "Hz" ? 0 : (unit.isEmpty() ? 2 : 1);
            return juce::String (value, decimals)
                 + (unit.isEmpty() ? juce::String {} : " " + unit);
        };
        result.slider.valueFromTextFunction = [] (const juce::String& text)
        {
            return text.retainCharacters ("-0123456789.,")
                       .replaceCharacter (',', '.')
                       .getDoubleValue();
        };
    }

    result.slider.updateText();

    collection.push_back (std::move (control));
    return result;
}

void SmartDuckingAudioProcessorEditor::paint (juce::Graphics& graphics)
{
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto diagnosticPaintStart = juce::Time::getHighResolutionTicks();
#endif
    audioProcessor.notifyEditorPaint();
    juce::ColourGradient backgroundGradient (backgroundSoftColour,
                                               0.0f,
                                               0.0f,
                                               juce::Colour::fromRGB (18, 0, 34),
                                               static_cast<float> (getWidth()),
                                               static_cast<float> (getHeight()),
                                               false);
    graphics.setGradientFill (backgroundGradient);
    graphics.fillAll();

    graphics.setColour (juce::Colours::black.withAlpha (0.16f));
    graphics.fillRect (getLocalBounds().removeFromTop (headerHeight));
    graphics.setColour (borderColour);
    graphics.drawHorizontalLine (headerHeight - 1, 0.0f, static_cast<float> (getWidth()));

    const auto RusADLogoBounds = juce::Rectangle<float> (18.0f, 15.0f, 40.0f, 40.0f);

    if (RusADLogo.isValid())
    {
        graphics.saveState();
        graphics.setOpacity (1.0f);
        graphics.setColour (textColour);
        graphics.drawImage (RusADLogo,
                            RusADLogoBounds,
                            juce::RectanglePlacement::centred,
                            false);
        graphics.restoreState();
    }

    graphics.setColour (textColour);
    graphics.setFont (juce::FontOptions { 21.0f, juce::Font::bold });
    constexpr int titleX = 66;
    constexpr int titleWidth = 158;
    graphics.drawFittedText ("SMART DUCKING",
                             juce::Rectangle<int> (titleX, 17, titleWidth, 36),
                             juce::Justification::centredLeft,
                             1,
                             0.62f);

    constexpr float pluginLogoX = static_cast<float> (titleX + titleWidth + 6);
    const auto firstHeaderControlX = presetChoice.getX() > 0
        ? presetChoice.getX() : comparisonButtons[0].getX();
    const auto pluginLogoRight = juce::jmax (pluginLogoX + 24.0f,
                                              static_cast<float> (firstHeaderControlX - 8));
    const auto pluginLogoWidth = juce::jlimit (24.0f, 62.0f,
                                                pluginLogoRight - pluginLogoX);
    const auto logoBounds = juce::Rectangle<float> (pluginLogoX, 17.0f,
                                                     pluginLogoWidth, 30.0f);
    const auto logoWidth = logoBounds.getWidth();
    juce::Path wave;
    wave.startNewSubPath (logoBounds.getX(), logoBounds.getCentreY());
    wave.cubicTo (logoBounds.getX() + logoWidth * (10.0f / 62.0f),
                  logoBounds.getY() - 4.0f,
                  logoBounds.getX() + logoWidth * (16.0f / 62.0f),
                  logoBounds.getBottom() + 4.0f,
                  logoBounds.getX() + logoWidth * (26.0f / 62.0f),
                  logoBounds.getCentreY());
    wave.cubicTo (logoBounds.getX() + logoWidth * (36.0f / 62.0f),
                  logoBounds.getY() - 4.0f,
                  logoBounds.getX() + logoWidth * (43.0f / 62.0f),
                  logoBounds.getBottom() + 4.0f,
                  logoBounds.getRight(),
                  logoBounds.getCentreY());
    graphics.setColour (primarySoftColour.withAlpha (0.58f));
    graphics.strokePath (wave, juce::PathStrokeType (4.0f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

    const auto lampGlow = duckingIndicatorActive ? voiceColour : mutedTextColour;
    graphics.setColour (lampGlow.withAlpha (duckingIndicatorActive ? 0.16f : 0.05f));
    graphics.fillEllipse (duckingLampBounds.expanded (5.0f));
    graphics.setColour (backgroundSoftColour);
    graphics.fillEllipse (duckingLampBounds);
    graphics.setColour (lampGlow.withAlpha (duckingIndicatorActive ? 1.0f : 0.38f));
    graphics.fillEllipse (duckingLampBounds.reduced (2.0f));
    graphics.setColour (lampGlow.withAlpha (0.75f));
    graphics.drawEllipse (duckingLampBounds, 1.0f);
    drawCard (graphics, stageControlPanel.getBounds().toFloat(), 8.0f);

    const auto spectrum = spectrumPanel->getBounds().toFloat();
    const auto meterTop = spectrum.getY();
    const auto meterHeight = spectrum.getHeight();
    const auto leftWidth = juce::jmax (108.0f, spectrum.getX() - 24.0f);
    const auto rightX = spectrum.getRight() + 8.0f;
    const auto rightWidth = static_cast<float> (getWidth()) - rightX - 12.0f;
    auto leftMeters = juce::Rectangle<float> (12.0f, meterTop, leftWidth, meterHeight);
    auto rightMeters = juce::Rectangle<float> (rightX, meterTop, rightWidth, meterHeight);
    const auto leftHalf = (leftMeters.getWidth() - 6.0f) * 0.5f;
    const auto voiceChannels = getVoiceSpatialChannelCount() == 5 ? 5 : 2;
    const auto outputChannels = audioProcessor.getOutputMeterChannelCount();
    const auto readChannelOrder = [this] (const char* parameterId)
    {
        const auto* value = audioProcessor.getParameterState().getRawParameterValue (parameterId);
        return smart_ducking::sanitiseSurroundChannelOrder (
            value != nullptr ? value->load() : 0.0f);
    };
    const std::array<int, 5> stereoMeterOrder { 0, 2, 1, 3, 4 };
    const auto mainMeterOrder = audioProcessor.isSurroundVariant()
        ? smart_ducking::getMeterChannelsForOrder (readChannelOrder (
              smart_ducking::parameter_ids::mainChannelOrder))
        : stereoMeterOrder;
    const auto voiceMeterOrder = voiceChannels == 5
        ? smart_ducking::getMeterChannelsForOrder (readChannelOrder (
              smart_ducking::parameter_ids::voiceChannelOrder))
        : stereoMeterOrder;
    const auto outputMeterOrder = outputChannels == 5
        ? smart_ducking::getMeterChannelsForOrder (readChannelOrder (
              smart_ducking::parameter_ids::outputChannelOrder))
        : stereoMeterOrder;
    auto mainInputMeter = leftMeters.removeFromLeft (leftHalf);
    mainInputMeterBounds = mainInputMeter;

    drawSignalMeter (graphics,
                     mainInputMeter,
                     "MAIN IN",
                     displayedMainPeaks,
                     secondaryTextColour,
                     audioProcessor.isSurroundVariant() ? 5 : 2,
                     nullptr,
                     nullptr,
                     mainMeterOrder,
                     audioProcessor.isSurroundVariant());
    leftMeters.removeFromLeft (6.0f);
    voiceInputMeterBounds = leftMeters;
    drawSignalMeter (graphics,
                     leftMeters,
                     "VOICE SC",
                     displayedVoicePeaks,
                     voiceColour,
                     voiceChannels,
                     nullptr,
                     nullptr,
                     voiceMeterOrder,
                     voiceChannels == 5);
    outputMeterBounds = rightMeters;
    drawSignalMeter (graphics,
                     rightMeters,
                     "OUTPUT",
                     displayedOutputPeaks,
                     secondaryTextColour,
                     outputChannels,
                     &displayedReductionDb,
                     &displayedSpectralDb,
                     outputMeterOrder,
                     outputChannels == 5);

    const auto lowerBounds = pagePanel.getBounds().toFloat();
    graphics.setColour (juce::Colours::black.withAlpha (0.08f));
    graphics.fillRoundedRectangle (lowerBounds, 10.0f);
    graphics.setColour (borderColour);
    graphics.drawRoundedRectangle (lowerBounds.reduced (0.5f), 10.0f, 1.0f);

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    audioProcessor.recordEditorPaintDuration (static_cast<std::uint64_t> (
        juce::Time::getHighResolutionTicks() - diagnosticPaintStart));
#endif
}

void SmartDuckingAudioProcessorEditor::paintOverChildren (juce::Graphics& graphics)
{
    if (! offlineRenderingDisplayed)
        return;

    graphics.setColour (backgroundColour.withAlpha (0.88f));
    graphics.fillRect (getLocalBounds());
    graphics.setColour (textColour);
    graphics.setFont (juce::FontOptions { 28.0f, juce::Font::bold });
    graphics.drawText ("RENDERING",
                       getLocalBounds(),
                       juce::Justification::centred,
                       false);
}

void SmartDuckingAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    const auto headerBounds = bounds.removeFromTop (headerHeight).reduced (12, 10);
    auto rightHeader = headerBounds;
    auto lampArea = rightHeader.removeFromRight (18);
    duckingLampBounds = lampArea.toFloat().withSizeKeepingCentre (12.0f, 12.0f);
    rightHeader.removeFromRight (6);

    const auto placeRightHeaderControl = [&rightHeader] (juce::Component& component,
                                                         int width,
                                                         int height)
    {
        component.setBounds (rightHeader.removeFromRight (width)
                                        .withSizeKeepingCentre (width, height));
    };

    if (audioProcessor.isSurroundVariant())
    {
        placeRightHeaderControl (dropLfeToggle, 88, 34);
        rightHeader.removeFromRight (4);
        placeRightHeaderControl (downmixToggle, 86, 34);
        rightHeader.removeFromRight (8);
    }

    placeRightHeaderControl (voiceFormatChoice, 112, 36);

    constexpr auto leftHeaderControlsX = 326;
    auto leftHeader = juce::Rectangle<int> (
        leftHeaderControlsX,
        headerBounds.getY(),
        juce::jmax (0, rightHeader.getRight() - leftHeaderControlsX),
        headerBounds.getHeight());
    const auto placeLeftHeaderControl = [&leftHeader] (juce::Component& component,
                                                       int width,
                                                       int height)
    {
        component.setBounds (leftHeader.removeFromLeft (width)
                                       .withSizeKeepingCentre (width, height));
    };

    placeLeftHeaderControl (presetChoice, 150, 36);
    leftHeader.removeFromLeft (8);
    placeLeftHeaderControl (comparisonButtons[0], 32, 32);
    leftHeader.removeFromLeft (3);
    placeLeftHeaderControl (comparisonButtons[1], 32, 32);

    auto secondRow = bounds.removeFromTop (54).reduced (12, 5);
    auto globalBypassArea = secondRow.removeFromRight (50);
    secondRow.removeFromRight (8);
    stageControlPanel.setBounds (secondRow);
    globalBypassPanel->setBounds (globalBypassArea);
    globalBypassButton.setBounds (
        globalBypassPanel->getLocalBounds().withSizeKeepingCentre (34, 32));
    auto stageContent = stageControlPanel.getLocalBounds().reduced (8, 4);
    const auto stageWidth = stageContent.getWidth() / 3;

    for (size_t stage = 0; stage < stageLabels.size(); ++stage)
    {
        auto group = stageContent.removeFromLeft (
            stage == stageLabels.size() - 1 ? stageContent.getWidth() : stageWidth);
        constexpr auto maximumStageContentWidth = 370;
        constexpr auto stageLabelWidth = 92;
        constexpr auto stageLabelGap = 8;
        auto compactGroup = group.withSizeKeepingCentre (
            juce::jmin (maximumStageContentWidth, group.getWidth()),
            group.getHeight());
        stageLabels[stage].setBounds (compactGroup.removeFromLeft (stageLabelWidth));
        compactGroup.removeFromLeft (stageLabelGap);
        const auto buttonWidth = compactGroup.getWidth() / 3;

        for (size_t control = 0; control < 3; ++control)
            stageButtons[stage * 3 + control].setBounds (compactGroup.removeFromLeft (
                control == 2 ? compactGroup.getWidth() : buttonWidth).reduced (2, 3));
    }

    bounds.reduce (12, 6);
    const auto leftMeterWidth = juce::jlimit (150, 166, getWidth() / 6);
    const auto rightMeterWidth = juce::jlimit (90, 100, getWidth() / 10);
    const auto bottomHeight = juce::jlimit (210, 250, getHeight() * 34 / 100);
    auto lower = bounds.removeFromBottom (bottomHeight);
    bounds.removeFromBottom (10);
    pagePanel.setBounds (lower);

    spectrumPanel->setBounds (bounds.withTrimmedLeft (leftMeterWidth)
                                    .withTrimmedRight (rightMeterWidth));
    layoutPagePanel();
}

void SmartDuckingAudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
    if (offlineRenderingDisplayed)
        return;

    if (! event.mods.isLeftButtonDown())
        return;

    if (audioProcessor.isSurroundVariant()
        && mainInputMeterBounds.contains (event.position))
    {
        showChannelOrderMenu (smart_ducking::parameter_ids::mainChannelOrder,
                              "MAIN IN CHANNEL ORDER",
                              mainInputMeterBounds,
                              event.position);
        return;
    }

    if (getVoiceSpatialChannelCount() == 5
        && voiceInputMeterBounds.contains (event.position))
    {
        showChannelOrderMenu (smart_ducking::parameter_ids::voiceChannelOrder,
                              "VOICE SC CHANNEL ORDER",
                              voiceInputMeterBounds,
                              event.position);
        return;
    }

    if (audioProcessor.getOutputMeterChannelCount() == 5
        && outputMeterBounds.contains (event.position))
        showChannelOrderMenu (smart_ducking::parameter_ids::outputChannelOrder,
                              "OUTPUT CHANNEL ORDER",
                              outputMeterBounds,
                              event.position);
}

void SmartDuckingAudioProcessorEditor::showChannelOrderMenu (
    const char* parameterId,
    const juce::String& title,
    juce::Rectangle<float> targetBounds,
    juce::Point<float> clickPosition)
{
    auto* parameter = audioProcessor.getParameterState().getParameter (parameterId);

    if (parameter == nullptr)
        return;

    const auto currentOrder = juce::jlimit (
        0,
        smart_ducking::surroundChannelOrderCount - 1,
        juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue())));
    juce::PopupMenu menu;
    menu.setLookAndFeel (rusadLookAndFeel.get());
    menu.addSectionHeader (title);

    for (int order = 0; order < smart_ducking::surroundChannelOrderCount; ++order)
        menu.addItem (
            order + 1,
            juce::String (smart_ducking::surroundChannelOrderNames[static_cast<size_t> (order)])
                + " - "
                + smart_ducking::surroundChannelOrderPreviews[static_cast<size_t> (order)],
            true,
            order == currentOrder);

    auto clickArea = juce::Rectangle<int> (0, 0, 2, 2);
    clickArea.setCentre (juce::roundToInt (clickPosition.x),
                         juce::roundToInt (clickPosition.y));
    clickArea = localAreaToGlobal (clickArea);
    auto safeThis = juce::Component::SafePointer<SmartDuckingAudioProcessorEditor> (this);
    const auto parameterIdString = juce::String (parameterId);
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetComponent (*this)
            .withTargetScreenArea (clickArea)
            .withPreferredPopupDirection (
                juce::PopupMenu::Options::PopupDirection::downwards)
            .withMinimumWidth (260)
            .withStandardItemHeight (30)
            .withInitiallySelectedItem (currentOrder + 1)
            .withParentComponent (this)
            .withDeletionCheck (*this),
        [safeThis, parameterIdString, targetBounds] (int result)
        {
            if (result < 1
                || result > smart_ducking::surroundChannelOrderCount
                || safeThis == nullptr)
                return;

            if (auto* channelOrderParameter =
                    safeThis->audioProcessor.getParameterState().getParameter (
                        parameterIdString))
            {
                const auto requestedValue = channelOrderParameter->convertTo0to1 (
                    static_cast<float> (result - 1));

                if (std::abs (channelOrderParameter->getValue() - requestedValue) < 0.001f)
                    return;

                channelOrderParameter->beginChangeGesture();
                channelOrderParameter->setValueNotifyingHost (requestedValue);
                channelOrderParameter->endChangeGesture();
                safeThis->repaint (targetBounds.expanded (2.0f).toNearestInt());
            }
        });
}

void SmartDuckingAudioProcessorEditor::setPage (Page newPage)
{
    currentPage = newPage;
    dynamicsPage.setVisible (currentPage == Page::dynamics);
    routingPage.setVisible (currentPage == Page::routing);
    updatePageButtons();
    saveUiState();
    layoutPagePanel();
    repaint();
}

void SmartDuckingAudioProcessorEditor::saveUiState()
{
    auto& state = audioProcessor.getParameterState().state;
    state.setProperty ("uiPage", static_cast<int> (currentPage), nullptr);
    state.removeProperty ("uiEditorMode", nullptr);
    state.removeProperty ("uiAdvancedPage", nullptr);
    state.setProperty ("uiMatrixSymmetryLink",
                       matrixSymmetryButton.getToggleState(),
                       nullptr);
}

void SmartDuckingAudioProcessorEditor::restoreUiState()
{
    const auto& state = audioProcessor.getParameterState().state;
    if (state.hasProperty ("uiPage"))
    {
        currentPage = static_cast<Page> (juce::jlimit (
            0, 1, static_cast<int> (state.getProperty ("uiPage", 0))));
    }
    else
    {
        // Legacy Advanced/Routing was index 2. Both old Simple and the retired
        // Advanced/Spectrum page migrate to the complete Dynamics page.
        currentPage = static_cast<int> (state.getProperty ("uiAdvancedPage", 0)) == 2
            ? Page::routing : Page::dynamics;
    }
    matrixSymmetryButton.setToggleState (
        static_cast<bool> (state.getProperty ("uiMatrixSymmetryLink", false)),
        juce::dontSendNotification);
}

void SmartDuckingAudioProcessorEditor::mirrorMatrixEdit (size_t sourceIndex)
{
    if (applyingSymmetryEdit
        || ! matrixSymmetryButton.getToggleState()
        || sourceIndex >= matrixControls.size())
        return;

    auto& source = matrixControls[sourceIndex]->slider;
    const auto isManualEdit = source.isMouseButtonDown()
                           || source.hasKeyboardFocus (true);

    if (! isManualEdit)
        return;

    const auto voice = static_cast<int> (sourceIndex / 5);
    const auto main = static_cast<int> (sourceIndex % 5);
    const auto mirrorIndex = smart_ducking::LcrDuckMatrix::mirroredCoefficientIndex (
        voice, main);

    if (mirrorIndex == sourceIndex || mirrorIndex >= matrixControls.size())
        return;

    const juce::ScopedValueSetter<bool> guard (applyingSymmetryEdit, true);
    matrixControls[mirrorIndex]->slider.setValue (source.getValue(),
                                                   juce::sendNotificationSync);
}

void SmartDuckingAudioProcessorEditor::setMonitorMode (smart_ducking::MonitorMode requestedMode)
{
    auto* parameter = audioProcessor.getParameterState().getParameter (
        smart_ducking::parameter_ids::monitorMode);

    if (parameter == nullptr)
        return;

    const auto currentMode = static_cast<smart_ducking::MonitorMode> (juce::jlimit (
        0,
        6,
        juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue()))));
    const auto newMode = currentMode == requestedMode
        ? smart_ducking::MonitorMode::normal
        : requestedMode;
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (
        static_cast<float> (newMode)));
    parameter->endChangeGesture();
    updateStageButtons();
}

void SmartDuckingAudioProcessorEditor::updateStageButtons()
{
    const auto* monitor = audioProcessor.getParameterState().getRawParameterValue (
        smart_ducking::parameter_ids::monitorMode);
    const auto mode = static_cast<smart_ducking::MonitorMode> (juce::jlimit (
        0, 6, monitor != nullptr ? juce::roundToInt (monitor->load()) : 0));
    const std::array<smart_ducking::MonitorMode, 3> firstMonitorModes {
        smart_ducking::MonitorMode::crossoverLow,
        smart_ducking::MonitorMode::widebandSolo,
        smart_ducking::MonitorMode::spectralSolo
    };
    const std::array<smart_ducking::MonitorMode, 3> secondMonitorModes {
        smart_ducking::MonitorMode::crossoverHigh,
        smart_ducking::MonitorMode::widebandDelta,
        smart_ducking::MonitorMode::spectralDelta
    };

    for (size_t stage = 0; stage < stageLabels.size(); ++stage)
    {
        stageButtons[stage * 3 + 1].setToggleState (mode == firstMonitorModes[stage],
                                                    juce::dontSendNotification);
        stageButtons[stage * 3 + 2].setToggleState (mode == secondMonitorModes[stage],
                                                    juce::dontSendNotification);
    }
}

void SmartDuckingAudioProcessorEditor::updatePageButtons()
{
    for (size_t index = 0; index < pageButtons.size(); ++index)
        pageButtons[index].setToggleState (
            static_cast<int> (currentPage) == static_cast<int> (index),
            juce::dontSendNotification);
}

void SmartDuckingAudioProcessorEditor::layoutPagePanel()
{
    if (pagePanel.getWidth() <= 0)
        return;

    auto bounds = pagePanel.getLocalBounds().reduced (12, 9);
    auto navigation = bounds.removeFromTop (34);
    const auto navigationWidth = navigation.getWidth()
                               / static_cast<int> (pageButtons.size());

    for (size_t index = 0; index < pageButtons.size(); ++index)
        pageButtons[index].setBounds (navigation.removeFromLeft (
            index == pageButtons.size() - 1 ? navigation.getWidth() : navigationWidth));

    bounds.removeFromTop (8);
    for (auto* page : { &dynamicsPage, &routingPage })
        page->setBounds (bounds);

    if (! dynamicsControls.empty())
    {
        auto page = dynamicsPage.getLocalBounds();
        const auto selectorWidth = juce::jlimit (140, 180, page.getWidth() / 5);
        auto selectorArea = page.removeFromRight (selectorWidth).reduced (8, 0);
        page.removeFromRight (4);
        auto modeArea = selectorArea.removeFromTop (
            juce::jmin (68, selectorArea.getHeight() / 2));
        processingModeLabel.setBounds (modeArea.removeFromTop (26));
        processingModeChoice.setBounds (modeArea.removeFromTop (34));
        selectorArea.removeFromTop (4);
        fftLabel.setBounds (selectorArea.removeFromTop (26));
        fftChoice.setBounds (selectorArea.removeFromTop (34));

        const auto width = page.getWidth() / static_cast<int> (dynamicsControls.size());
        for (size_t index = 0; index < dynamicsControls.size(); ++index)
            dynamicsControls[index]->setBounds (page.removeFromLeft (
                index == dynamicsControls.size() - 1 ? page.getWidth() : width));
    }

    layoutMatrix();
}

int SmartDuckingAudioProcessorEditor::getVoiceSpatialChannelCount() const noexcept
{
    const auto* voiceFormat = audioProcessor.getParameterState().getRawParameterValue (
        smart_ducking::parameter_ids::voiceFormat);
    return voiceFormat != nullptr && voiceFormat->load() >= 0.5f ? 5 : 3;
}

int SmartDuckingAudioProcessorEditor::getMainSpatialChannelCount() const noexcept
{
    return audioProcessor.isSurroundVariant() ? 5 : 3;
}

void SmartDuckingAudioProcessorEditor::layoutMatrix()
{
    if (matrixControls.size() != 25 || routingPage.getWidth() <= 0)
        return;

    const auto rows = getVoiceSpatialChannelCount();
    const auto columns = getMainSpatialChannelCount();
    displayedMatrixRows = rows;
    displayedMatrixColumns = columns;
    auto bounds = routingPage.getLocalBounds().reduced (4);
    auto buttonRow = bounds.removeFromBottom (26);
    bounds.removeFromBottom (4);
    const auto buttonWidth = juce::jmin (128, buttonRow.getWidth() / 5);
    matrixResetButton.setBounds (buttonRow.removeFromRight (buttonWidth).reduced (3, 0));
    matrixDiagonalButton.setBounds (buttonRow.removeFromRight (buttonWidth).reduced (3, 0));
    matrixSymmetryButton.setBounds (buttonRow.removeFromRight (buttonWidth).reduced (3, 0));

    constexpr int leftHeaderWidth = 42;
    constexpr int topHeaderHeight = 30;
    auto top = bounds.removeFromTop (topHeaderHeight);
    matrixVoiceHeader.setBounds (top.removeFromLeft (leftHeaderWidth));
    matrixOriginalHeader.setBounds (top.removeFromTop (16));
    const auto columnWidth = top.getWidth() / columns;

    for (int channel = 0; channel < 5; ++channel)
    {
        const auto visible = channel < columns;
        matrixColumnHeaders[static_cast<size_t> (channel)].setVisible (visible);
        if (visible)
            matrixColumnHeaders[static_cast<size_t> (channel)].setBounds (
                top.removeFromLeft (channel == columns - 1 ? top.getWidth() : columnWidth));
    }

    auto rowHeaders = bounds.removeFromLeft (leftHeaderWidth);
    const auto rowHeight = bounds.getHeight() / rows;
    const auto cellWidth = bounds.getWidth() / columns;

    for (int channel = 0; channel < 5; ++channel)
    {
        const auto visible = channel < rows;
        matrixRowHeaders[static_cast<size_t> (channel)].setVisible (visible);
        if (visible)
            matrixRowHeaders[static_cast<size_t> (channel)].setBounds (
                rowHeaders.getX(),
                rowHeaders.getY() + channel * rowHeight,
                rowHeaders.getWidth(),
                channel == rows - 1 ? rowHeaders.getBottom()
                                           - (rowHeaders.getY() + channel * rowHeight)
                                     : rowHeight);
    }

    for (int voice = 0; voice < 5; ++voice)
    {
        for (int main = 0; main < 5; ++main)
        {
            const auto index = static_cast<size_t> (voice * 5 + main);
            const auto visible = voice < rows && main < columns;
            matrixControls[index]->setVisible (visible);

            if (visible)
            {
                const auto x = bounds.getX() + main * cellWidth;
                const auto y = bounds.getY() + voice * rowHeight;
                const auto width = main == columns - 1 ? bounds.getRight() - x : cellWidth;
                const auto height = voice == rows - 1 ? bounds.getBottom() - y : rowHeight;
                matrixControls[index]->setBounds (
                    juce::Rectangle<int> (x, y, width, height).reduced (1));
            }
        }
    }
}

void SmartDuckingAudioProcessorEditor::applyMatrixValues (const std::array<float, 25>& values)
{
    if (matrixControls.size() != values.size())
        return;

    for (size_t index = 0; index < values.size(); ++index)
        matrixControls[index]->slider.setValue (values[index], juce::sendNotificationSync);
}

void SmartDuckingAudioProcessorEditor::resetAudioVisuals()
{
    displayedMainPeaks.fill (0.0f);
    displayedVoicePeaks.fill (0.0f);
    displayedOutputPeaks.fill (0.0f);
    displayedReductionDb.fill (0.0f);
    displayedSpectralDb.fill (0.0f);
    duckingIndicatorActive = false;
    repaint();
}

void SmartDuckingAudioProcessorEditor::timerCallback()
{
    const auto factoryProgramChangeCount = audioProcessor.getFactoryProgramChangeCount();
    if (factoryProgramChangeCount != lastFactoryProgramChangeCount)
    {
        lastFactoryProgramChangeCount = factoryProgramChangeCount;
        selectedFactoryPreset = audioProcessor.getCurrentProgram();
        selectedUserPresetName.clear();
        rebuildPresetMenu();
    }
    else
        updatePresetDisplay();

    const auto offlineRendering = audioProcessor.isOfflineRenderingActive();

    if (offlineRendering != offlineRenderingDisplayed)
    {
        offlineRenderingDisplayed = offlineRendering;
        repaint();
    }

    const auto shouldVisualise = isShowing() && ! offlineRendering;

    if (shouldVisualise != editorVisualisationActive)
    {
        editorVisualisationActive = shouldVisualise;

        if (editorVisualisationActive)
        {
            resetAudioVisuals();
            audioProcessor.notifyEditorOpened();
            lastObservedProcessBlockCount = audioProcessor.getProcessBlockCount();
            waitingForFreshAudioBlock = true;
            audioVisualsSuppressed = true;
        }
        else
            audioProcessor.notifyEditorClosed();
    }

    if (! editorVisualisationActive || offlineRendering)
        return;

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto diagnosticTimerStart = juce::Time::getHighResolutionTicks();
#endif
    audioProcessor.notifyEditorTimer();

    const auto processBlockCount = audioProcessor.getProcessBlockCount();
    const auto receivedFreshAudioBlock = processBlockCount
                                      != lastObservedProcessBlockCount;
    lastObservedProcessBlockCount = processBlockCount;

    if (receivedFreshAudioBlock)
        waitingForFreshAudioBlock = false;

    const auto suppressAudioVisuals = waitingForFreshAudioBlock
        || (audioProcessor.isAudioCallbackQuiescent() && ! receivedFreshAudioBlock);

    if (suppressAudioVisuals && ! audioVisualsSuppressed)
        resetAudioVisuals();

    audioVisualsSuppressed = suppressAudioVisuals;

    if (audioProcessor.isSurroundVariant())
    {
        if (const auto* downmix = audioProcessor.getParameterState().getRawParameterValue (
                smart_ducking::parameter_ids::downmix))
            dropLfeToggle.setEnabled (downmix->load() >= 0.5f);
    }

    if (getVoiceSpatialChannelCount() != displayedMatrixRows
        || getMainSpatialChannelCount() != displayedMatrixColumns)
        layoutMatrix();

    auto mainPeaks = std::array<float, 5> {};
    auto voicePeaks = std::array<float, 5> {};
    auto outputPeaks = std::array<float, 5> {};
    auto wideReduction = std::array<float, 5> {};
    auto spectralReduction = std::array<float, 5> {};

    if (! suppressAudioVisuals)
    {
        mainPeaks = { audioProcessor.getMainInputPeakLeft(),
                      audioProcessor.getMainInputPeakCentre(),
                      audioProcessor.getMainInputPeakRight(),
                      audioProcessor.getMainInputPeakLeftSurround(),
                      audioProcessor.getMainInputPeakRightSurround() };
        voicePeaks = { audioProcessor.getSidechainPeakLeft(),
                       audioProcessor.getSidechainPeakCentre(),
                       audioProcessor.getSidechainPeakRight(),
                       audioProcessor.getSidechainPeakLeftSurround(),
                       audioProcessor.getSidechainPeakRightSurround() };
        outputPeaks = { audioProcessor.getOutputPeakLeft(),
                        audioProcessor.getOutputPeakCentre(),
                        audioProcessor.getOutputPeakRight(),
                        audioProcessor.getOutputPeakLeftSurround(),
                        audioProcessor.getOutputPeakRightSurround() };
        wideReduction = { audioProcessor.getGainReductionLeftDb(),
                          audioProcessor.getGainReductionCentreDb(),
                          audioProcessor.getGainReductionRightDb(),
                          audioProcessor.getGainReductionLeftSurroundDb(),
                          audioProcessor.getGainReductionRightSurroundDb() };
        spectralReduction = { audioProcessor.getSpectralReductionLeftDb(),
                              audioProcessor.getSpectralReductionCentreDb(),
                              audioProcessor.getSpectralReductionRightDb(),
                              audioProcessor.getSpectralReductionLeftSurroundDb(),
                              audioProcessor.getSpectralReductionRightSurroundDb() };
    }

    for (size_t channel = 0; channel < displayedMainPeaks.size(); ++channel)
    {
        displayedMainPeaks[channel] = decaySignalPeak (displayedMainPeaks[channel],
                                                       mainPeaks[channel]);
        displayedVoicePeaks[channel] = decaySignalPeak (displayedVoicePeaks[channel],
                                                        voicePeaks[channel]);
        displayedOutputPeaks[channel] = decaySignalPeak (displayedOutputPeaks[channel],
                                                         outputPeaks[channel]);
        displayedReductionDb[channel] = decayReduction (displayedReductionDb[channel],
                                                        wideReduction[channel]);
        displayedSpectralDb[channel] = decayReduction (displayedSpectralDb[channel],
                                                       spectralReduction[channel]);
    }

    spectrumPanel->updateSpectrum();

    updateStageButtons();
    const auto comparisonSlot = audioProcessor.getComparisonSlot();
    for (size_t slot = 0; slot < comparisonButtons.size(); ++slot)
        comparisonButtons[slot].setToggleState (
            static_cast<int> (slot) == comparisonSlot,
            juce::dontSendNotification);
    const auto heavyPathActive = ! suppressAudioVisuals
                              && audioProcessor.isHeavyPathActive();
    const auto duckingStateChanged = heavyPathActive != duckingIndicatorActive;
    duckingIndicatorActive = heavyPathActive;

    const auto spectrum = spectrumPanel->getBounds().toFloat();
    const auto leftMeterWidth = juce::jmax (108.0f, spectrum.getX() - 24.0f);
    const auto rightMeterX = spectrum.getRight() + 8.0f;
    const auto rightMeterWidth = static_cast<float> (getWidth()) - rightMeterX - 12.0f;
    if (! suppressAudioVisuals)
    {
        repaint (juce::Rectangle<float> (12.0f,
                                         spectrum.getY(),
                                         leftMeterWidth,
                                         spectrum.getHeight())
                     .expanded (2.0f)
                     .toNearestInt());
        repaint (juce::Rectangle<float> (rightMeterX,
                                         spectrum.getY(),
                                         rightMeterWidth,
                                         spectrum.getHeight())
                     .expanded (2.0f)
                     .toNearestInt());
    }

    if (duckingStateChanged)
        repaint (duckingLampBounds.expanded (6.0f).toNearestInt());
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    audioProcessor.recordEditorTimerDuration (static_cast<std::uint64_t> (
        juce::Time::getHighResolutionTicks() - diagnosticTimerStart));
#endif
}

void SmartDuckingAudioProcessorEditor::drawSignalMeter (
    juce::Graphics& graphics,
    juce::Rectangle<float> bounds,
    const juce::String& label,
    const std::array<float, 5>& values,
    juce::Colour colour,
    int visibleChannels,
    const std::array<float, 5>* wideReductionValues,
    const std::array<float, 5>* spectralReductionValues,
    std::array<int, 5> channelOrder,
    bool menuAvailable)
{
    drawCard (graphics, bounds, 9.0f);
    const auto showsReduction = wideReductionValues != nullptr
                            && spectralReductionValues != nullptr;
    visibleChannels = juce::jlimit (1, 5, visibleChannels);
    const auto horizontalPadding = visibleChannels >= 5 ? 5.0f : 7.0f;
    auto content = bounds.reduced (horizontalPadding, 8.0f);
    auto title = content.removeFromTop (24.0f);

    if (showsReduction)
    {
        auto reductionLegend = title.withSizeKeepingCentre (54.0f, title.getHeight());
        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.setColour (secondaryTextColour);
        graphics.drawText ("GR", reductionLegend.removeFromLeft (28.0f),
                           juce::Justification::centred);
        graphics.setColour (wideReductionColour);
        graphics.drawText ("W", reductionLegend.removeFromLeft (13.0f),
                           juce::Justification::centred);
        graphics.setColour (spectralReductionColour);
        graphics.drawText ("S", reductionLegend, juce::Justification::centred);
    }
    else
    {
        graphics.setColour (secondaryTextColour);
        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.drawFittedText (label + (menuAvailable ? "  \xE2\x96\xBE" : ""),
                                 title.toNearestInt(),
                                 juce::Justification::centred,
                                 1);
    }

    content.removeFromTop (4.0f);
    auto footer = content.removeFromBottom (showsReduction ? 30.0f : 16.0f);
    auto channelLabels = showsReduction ? footer.removeFromTop (14.0f) : footer;
    auto meterArea = content;
    const auto barWidth = meterArea.getWidth() / static_cast<float> (visibleChannels);
    const std::array<const char*, 5> channelNames { "L", "C", "R", "Ls", "Rs" };
    std::array<juce::Rectangle<float>, 5> meterColumns {};

    for (int row = 0; row < visibleChannels; ++row)
    {
        const auto channel = juce::jlimit (0, 4, channelOrder[static_cast<size_t> (row)]);
        const auto barPadding = visibleChannels >= 5 ? 1.0f : 2.0f;
        auto column = meterArea.removeFromLeft (barWidth).reduced (barPadding, 0.0f);
        meterColumns[static_cast<size_t> (row)] = column;
        graphics.setColour (juce::Colours::black.withAlpha (0.30f));
        graphics.fillRoundedRectangle (column, 3.0f);
        auto fill = column.reduced (2.0f);
        const auto proportion = signalProportion (values[static_cast<size_t> (channel)]);
        fill.removeFromTop (fill.getHeight() * (1.0f - proportion));
        graphics.setColour (colour.withAlpha (0.88f));
        graphics.fillRoundedRectangle (fill, 2.0f);
        graphics.setColour (mutedTextColour);
        graphics.setFont (juce::FontOptions { 8.5f });
        graphics.drawText (channelNames[static_cast<size_t> (channel)],
                           juce::Rectangle<float> (column.getX(),
                                                   channelLabels.getY(),
                                                   column.getWidth(),
                                                   channelLabels.getHeight()),
                           juce::Justification::centred);
    }

    if (showsReduction)
    {
        graphics.setColour (secondaryTextColour);
        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.drawFittedText (label + (menuAvailable ? "  \xE2\x96\xBE" : ""),
                                 footer.toNearestInt(),
                                 juce::Justification::centred,
                                 1);

        const auto reductionChannels = visibleChannels == 2 ? 3 : visibleChannels;
        const auto stripGap = visibleChannels == 2 ? 1.0f : 0.5f;
        const auto referenceWidth = meterColumns[0].getWidth();
        const auto stripWidth = visibleChannels == 2
            ? juce::jlimit (2.5f, 4.0f, referenceWidth * 0.16f)
            : juce::jlimit (1.25f, 2.5f, referenceWidth * 0.22f);
        const auto groupWidth = 2.0f * stripWidth + stripGap;
        const auto reductionArea = juce::Rectangle<float> (
            meterColumns[0].getX(),
            meterColumns[0].getY() + 2.0f,
            meterColumns[static_cast<size_t> (visibleChannels - 1)].getRight()
                - meterColumns[0].getX(),
            meterColumns[0].getHeight() - 4.0f);

        const auto groupXForChannel = [&meterColumns,
                                       visibleChannels,
                                       groupWidth] (int channel)
        {
            if (visibleChannels == 2)
            {
                if (channel == 0)
                    return meterColumns[0].getX();

                if (channel == 1)
                    return 0.5f * (meterColumns[0].getCentreX()
                                   + meterColumns[1].getCentreX())
                         - 0.5f * groupWidth;

                return meterColumns[1].getRight() - groupWidth;
            }

            return meterColumns[static_cast<size_t> (channel)].getCentreX()
                 - 0.5f * groupWidth;
        };

        for (int row = 0; row < reductionChannels; ++row)
        {
            const auto channel = visibleChannels == 2
                ? row
                : juce::jlimit (0, 4, channelOrder[static_cast<size_t> (row)]);
            const auto groupX = groupXForChannel (row);
            const auto drawReduction = [&graphics,
                                        &reductionArea,
                                        stripWidth] (float x,
                                                     float reductionDb,
                                                     juce::Colour reductionColour)
            {
                auto strip = juce::Rectangle<float> (
                    x,
                    reductionArea.getY(),
                    stripWidth,
                    reductionArea.getHeight()
                        * juce::jlimit (0.0f,
                                        1.0f,
                                        reductionDb / maximumDisplayedReductionDb));

                if (strip.getHeight() > 0.5f)
                {
                    graphics.setColour (reductionColour);
                    graphics.fillRoundedRectangle (strip, stripWidth * 0.45f);
                }
            };

            drawReduction (groupX,
                           (*wideReductionValues)[static_cast<size_t> (channel)],
                           wideReductionColour);
            drawReduction (groupX + stripWidth + stripGap,
                           (*spectralReductionValues)[static_cast<size_t> (channel)],
                           spectralReductionColour);
        }
    }
}
