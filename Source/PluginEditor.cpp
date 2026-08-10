#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

#include "BinaryData.h"
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
const auto voiceColour = juce::Colour::fromRGB (74, 222, 128);
const auto wideReductionColour = juce::Colour::fromRGB (239, 68, 68);
const auto maskColour = juce::Colour::fromRGB (251, 191, 36);
const auto spectralReductionColour = maskColour;
const auto borderColour = juce::Colours::white.withAlpha (0.10f);
const auto cardColour = juce::Colours::white.withAlpha (0.045f);

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

int meterValueIndexForRow (int row, int visibleChannels) noexcept
{
    return visibleChannels == 2 && row == 1 ? 2 : row;
}

void drawCard (juce::Graphics& graphics, juce::Rectangle<float> bounds, float corner = 10.0f)
{
    graphics.setColour (cardColour);
    graphics.fillRoundedRectangle (bounds, corner);
    graphics.setColour (borderColour);
    graphics.drawRoundedRectangle (bounds.reduced (0.5f), corner, 1.0f);
}

juce::String frequencyText (float frequency)
{
    if (frequency >= 1000.0f)
        return juce::String (frequency / 1000.0f, frequency >= 10000.0f ? 0 : 1) + " kHz";

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
        return "Centre frequency of the sidechain weighting filter used to prioritise detection.";
    if (parameterId == weightGainDb)
        return "Boost or cut applied by the sidechain weighting filter.";
    if (parameterId == weightQ)
        return "Bandwidth of the sidechain weighting filter; higher values focus it more narrowly.";
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

struct SmartDuckingAudioProcessorEditor::OnWaveLookAndFeel final : juce::LookAndFeel_V4
{
    OnWaveLookAndFeel()
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
        graphics.setFont (juce::FontOptions { 12.0f, juce::Font::bold });
        graphics.setColour (button.getToggleState() ? textColour : secondaryTextColour);
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
                       juce::ComboBox&) override
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
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (box.getLocalBounds().reduced (10, 1).withTrimmedRight (20));
        label.setFont (juce::FontOptions { 12.0f, juce::Font::bold });
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
    static constexpr int historyLength = 180;

    SpectrumPanel (SmartDuckingAudioProcessor& newProcessor)
        : processor (newProcessor), state (newProcessor.getParameterState())
    {
        inputDb.fill (-90.0f);
        outputDb.fill (-90.0f);
        reductionDb.fill (0.0f);
        spectralMaskDb.fill (0.0f);
        wideHistory.fill (0.0f);
        spectralHistory.fill (0.0f);
        initialiseCrossoverTemplate();
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

    void pushReduction (float wideDb, float spectralDb)
    {
        wideHistory[static_cast<size_t> (historyWritePosition)] = wideDb;
        spectralHistory[static_cast<size_t> (historyWritePosition)] = spectralDb;
        historyWritePosition = (historyWritePosition + 1) % historyLength;
        repaint();
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
        if (draggedParameter != nullptr)
            updateDraggedParameter (event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
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
        if (auto* parameter = parameterNear (event.position, 10.0f))
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
        setMouseCursor (juce::MouseCursor::NormalCursor);
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

    static constexpr int crossoverTemplatePoints = 33;
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

    float getNyquist() const noexcept
    {
        const auto sampleRate = processor.getSampleRate();
        return static_cast<float> ((sampleRate > 1000.0 ? sampleRate : 48000.0) * 0.5);
    }

    float frequencyToX (float frequency, juce::Rectangle<float> bounds) const noexcept
    {
        constexpr float minimumFrequency = 20.0f;
        const auto nyquist = getNyquist();
        const auto normal = std::log (juce::jlimit (minimumFrequency, nyquist, frequency)
                                      / minimumFrequency)
                          / std::log (nyquist / minimumFrequency);
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

    void initialiseCrossoverTemplate() noexcept
    {
        constexpr auto halfSpanInOctaves = 1.2f;

        for (int point = 0; point < crossoverTemplatePoints; ++point)
        {
            const auto normal = static_cast<float> (point)
                              / static_cast<float> (crossoverTemplatePoints - 1);
            const auto octaveOffset = juce::jmap (normal,
                                                  -halfSpanInOctaves,
                                                  halfSpanInOctaves);
            const auto ratio = std::pow (2.0f, octaveOffset);
            auto lowPassDb = -20.0f * std::log10 (1.0f + std::pow (ratio, 8.0f));
            auto highPassDb = -20.0f * std::log10 (
                1.0f + std::pow (1.0f / ratio, 8.0f));

            if (point == 0)
            {
                lowPassDb = 0.0f;
                highPassDb = spectrumFloorDb;
            }
            else if (point == crossoverTemplatePoints - 1)
            {
                lowPassDb = spectrumFloorDb;
                highPassDb = 0.0f;
            }

            crossoverLowTemplateDb[static_cast<size_t> (point)] = lowPassDb;
            crossoverHighTemplateDb[static_cast<size_t> (point)] = highPassDb;
        }
    }

    void updateCrossoverGeometry (juce::Rectangle<float> bounds)
    {
        const auto* parameter = state.getParameter (
            smart_ducking::parameter_ids::crossoverHz);

        if (parameter == nullptr)
            return;

        const auto frequency = parameter->convertFrom0to1 (parameter->getValue());
        const auto nyquist = getNyquist();

        if (std::abs (frequency - cachedCrossoverFrequency) < 0.001f
            && std::abs (nyquist - cachedCrossoverNyquist) < 0.001f
            && bounds == cachedCrossoverBounds)
            return;

        cachedCrossoverFrequency = frequency;
        cachedCrossoverNyquist = nyquist;
        cachedCrossoverBounds = bounds;
        const auto centreX = frequencyToX (frequency, bounds);
        constexpr auto halfSpanInOctaves = 1.2f;
        const auto pixelsPerOctave = bounds.getWidth()
                                   / (std::log (nyquist / 20.0f) / std::log (2.0f));
        const auto halfWidth = juce::jmax (1.0f, pixelsPerOctave * halfSpanInOctaves);

        for (int point = 0; point < crossoverTemplatePoints; ++point)
        {
            const auto normal = static_cast<float> (point)
                              / static_cast<float> (crossoverTemplatePoints - 1);
            const auto x = centreX + juce::jmap (normal, -halfWidth, halfWidth);
            const auto index = static_cast<size_t> (point);
            crossoverLowPoints[index] = { x, dbToY (crossoverLowTemplateDb[index], bounds) };
            crossoverHighPoints[index] = { x, dbToY (crossoverHighTemplateDb[index], bounds) };
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

        for (const auto frequency : { 100.0f, 1000.0f, 10000.0f })
        {
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
                graphics.drawText (frequency >= 1000.0f
                                       ? juce::String (juce::roundToInt (frequency / 1000.0f)) + "k"
                                       : juce::String (juce::roundToInt (frequency)),
                                   juce::Rectangle<float> (x - 22.0f,
                                                           bounds.getBottom() - 54.0f,
                                                           44.0f,
                                                           18.0f),
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
                auto labelBounds = juce::Rectangle<float> (x - 62.0f,
                                                            crossover ? bounds.getY() + 1.0f
                                                                      : bounds.getBottom() - 20.0f,
                                                            124.0f,
                                                            20.0f);
                labelBounds.setX (juce::jlimit (bounds.getX(),
                                                bounds.getRight() - labelBounds.getWidth(),
                                                labelBounds.getX()));
                markerLabelBounds[markerIndex] = labelBounds;
                graphics.drawText (juce::String (markerNames[markerIndex]) + "  "
                                       + frequencyText (frequency),
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

    void drawHistory (juce::Graphics& graphics, juce::Rectangle<float> bounds)
    {
        graphics.setColour (juce::Colours::black.withAlpha (0.18f));
        graphics.fillRoundedRectangle (bounds, 7.0f);
        auto graph = bounds.reduced (74.0f, 7.0f);
        graph.removeFromRight (8.0f);

        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.setColour (wideReductionColour);
        graphics.drawText ("WIDE", bounds.removeFromLeft (66.0f).removeFromTop (22.0f),
                           juce::Justification::centredLeft);
        graphics.setColour (spectralReductionColour);
        graphics.drawText ("SPECTRAL", bounds.removeFromLeft (66.0f),
                           juce::Justification::centredLeft);

        graphics.setColour (juce::Colours::white.withAlpha (0.055f));
        graphics.drawHorizontalLine (juce::roundToInt (graph.getY()), graph.getX(), graph.getRight());
        graphics.drawHorizontalLine (juce::roundToInt (graph.getCentreY()), graph.getX(), graph.getRight());

        juce::Path widePath;
        juce::Path spectralPath;

        for (int point = 0; point < historyLength; ++point)
        {
            const auto index = (historyWritePosition + point) % historyLength;
            const auto x = graph.getX() + graph.getWidth()
                                         * static_cast<float> (point)
                                         / static_cast<float> (historyLength - 1);
            const auto wideY = graph.getY() + graph.getHeight()
                                            * juce::jlimit (0.0f,
                                                            1.0f,
                                                            wideHistory[static_cast<size_t> (index)]
                                                                / maximumDisplayedReductionDb);
            const auto spectralY = graph.getY() + graph.getHeight()
                                                * juce::jlimit (
                                                    0.0f,
                                                    1.0f,
                                                    spectralHistory[static_cast<size_t> (index)]
                                                        / maximumDisplayedReductionDb);

            if (point == 0)
            {
                widePath.startNewSubPath (x, wideY);
                spectralPath.startNewSubPath (x, spectralY);
            }
            else
            {
                widePath.lineTo (x, wideY);
                spectralPath.lineTo (x, spectralY);
            }
        }

        graphics.setColour (wideReductionColour);
        graphics.strokePath (widePath, juce::PathStrokeType (1.5f));
        graphics.setColour (spectralReductionColour);
        graphics.strokePath (spectralPath, juce::PathStrokeType (1.5f));
    }

    void updateDraggedParameter (float mouseX)
    {
        if (draggedParameter == nullptr)
            return;

        const auto normal = juce::jlimit (0.0f,
                                          1.0f,
                                          (mouseX - spectrumBounds.getX())
                                              / spectrumBounds.getWidth());
        const auto frequency = 20.0f * std::pow (getNyquist() / 20.0f, normal);
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
            return "Crossover between broadband and spectral processing. Drag to adjust; double-click the marker to reset; right-click or double-click its value to type a frequency.";

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
    std::array<float, historyLength> wideHistory {};
    std::array<float, historyLength> spectralHistory {};
    std::array<float, crossoverTemplatePoints> crossoverLowTemplateDb {};
    std::array<float, crossoverTemplatePoints> crossoverHighTemplateDb {};
    std::array<juce::Point<float>, crossoverTemplatePoints> crossoverLowPoints {};
    std::array<juce::Point<float>, crossoverTemplatePoints> crossoverHighPoints {};
    juce::Path crossoverLowPath;
    juce::Path crossoverHighPath;
    std::array<juce::Rectangle<float>, 3> markerLabelBounds {};
    juce::Rectangle<float> spectrumBounds;
    juce::Rectangle<float> historyBounds;
    juce::TextEditor markerEditor;
    juce::RangedAudioParameter* draggedParameter = nullptr;
    juce::RangedAudioParameter* editingParameter = nullptr;
    std::uint64_t lastDisplayFrame = 0;
    double lastSpectrumFrameMilliseconds = 0.0;
    float spectrumDisplayOffsetDb = 0.0f;
    float cachedCrossoverFrequency = -1.0f;
    float cachedCrossoverNyquist = -1.0f;
    juce::Rectangle<float> cachedCrossoverBounds;
    int historyWritePosition = 0;
    bool hasSpectrumFrame = false;
    bool hasSpectrumCalibration = false;
};

SmartDuckingAudioProcessorEditor::SmartDuckingAudioProcessorEditor (
    SmartDuckingAudioProcessor& audioProcessor)
    : juce::AudioProcessorEditor (audioProcessor),
      processor (audioProcessor),
      onWaveLookAndFeel (std::make_unique<OnWaveLookAndFeel>()),
      spectrumPanel (std::make_unique<SpectrumPanel> (audioProcessor))
{
    using namespace smart_ducking::parameter_ids;

    onWaveLogo = juce::ImageCache::getFromMemory (BinaryData::OnWaveLogo_png,
                                                  BinaryData::OnWaveLogo_pngSize);
    setOpaque (true);
    setLookAndFeel (onWaveLookAndFeel.get());
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 850);
    setResizable (true, true);
    setResizeLimits (820, 560, 1400, 900);
    setSize (editorWidth, editorHeight);

    addAndMakeVisible (simpleModeButton);
    addAndMakeVisible (advancedModeButton);
    simpleModeButton.setClickingTogglesState (false);
    advancedModeButton.setClickingTogglesState (false);
    simpleModeButton.onClick = [this] { setEditorMode (EditorMode::simple); };
    advancedModeButton.onClick = [this] { setEditorMode (EditorMode::advanced); };
    simpleModeButton.setTooltip ("Show the essential controls only.");
    advancedModeButton.setTooltip ("Show dynamics, spectral shaping and routing controls.");

    addAndMakeVisible (*spectrumPanel);
    addAndMakeVisible (simplePanel);
    addAndMakeVisible (advancedPanel);
    simplePanel.addAndMakeVisible (simplePageLabel);
    simplePageLabel.setText ("ESSENTIAL CONTROLS", juce::dontSendNotification);
    simplePageLabel.setJustificationType (juce::Justification::centred);
    simplePageLabel.setColour (juce::Label::textColourId, secondaryTextColour);
    simplePageLabel.setColour (juce::Label::backgroundColourId,
                               juce::Colours::white.withAlpha (0.025f));
    simplePageLabel.setColour (juce::Label::outlineColourId, borderColour);
    simplePageLabel.setFont (juce::FontOptions { 14.0f, juce::Font::bold });
    simplePageLabel.setInterceptsMouseClicks (false, false);

    addSliderControl (simplePanel, simpleControls, duckDepthDb, "DUCKING DEPTH", "dB");
    addSliderControl (simplePanel, simpleControls, spectralAmountDb, "SPECTRAL DEPTH", "dB");
    addSliderControl (simplePanel, simpleControls, activityThresholdDb, "THRESHOLD", "dBFS");
    addSliderControl (simplePanel, simpleControls, attackMs, "ATTACK", "ms");
    addSliderControl (simplePanel, simpleControls, holdMs, "HOLD", "ms");
    addSliderControl (simplePanel, simpleControls, releaseMs, "RELEASE", "ms");

    for (size_t index = 0; index < advancedPageButtons.size(); ++index)
    {
        auto& button = advancedPageButtons[index];
        advancedPanel.addAndMakeVisible (button);
        button.setClickingTogglesState (false);
        button.onClick = [this, index]
        {
            setAdvancedPage (static_cast<AdvancedPage> (index));
        };
    }

    for (auto* page : { &dynamicsPage, &spectrumPage, &routingPage })
        advancedPanel.addAndMakeVisible (*page);

    addSliderControl (dynamicsPage, dynamicsControls, duckDepthDb, "DUCKING DEPTH", "dB");
    addSliderControl (dynamicsPage, dynamicsControls, activityThresholdDb, "THRESHOLD", "dBFS");
    addSliderControl (dynamicsPage, dynamicsControls, attackMs, "ATTACK", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, holdMs, "HOLD", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, releaseMs, "RELEASE", "ms");
    addSliderControl (dynamicsPage, dynamicsControls, lookaheadMs, "LOOKAHEAD", "ms");

    addSliderControl (spectrumPage, spectrumControls, spectralAmountDb, "AMOUNT", "dB");
    addSliderControl (spectrumPage, spectrumControls, weightFrequencyHz, "WEIGHT FREQ", "Hz");
    addSliderControl (spectrumPage, spectrumControls, weightGainDb, "WEIGHT GAIN", "dB");
    addSliderControl (spectrumPage, spectrumControls, weightQ, "WEIGHT Q", "");

    spectrumPage.addAndMakeVisible (fftLabel);
    spectrumPage.addAndMakeVisible (fftChoice);
    spectrumPage.addAndMakeVisible (processingModeLabel);
    spectrumPage.addAndMakeVisible (processingModeChoice);
    processingModeLabel.setText ("PROCESSING MODE", juce::dontSendNotification);
    processingModeLabel.setJustificationType (juce::Justification::centred);
    processingModeLabel.setColour (juce::Label::textColourId, secondaryTextColour);
    processingModeLabel.setFont (juce::FontOptions { 11.0f, juce::Font::bold });
    processingModeChoice.addItem ("QUALITY", 1);
    processingModeChoice.addItem ("NORMAL", 2);
    processingModeChoice.setJustificationType (juce::Justification::centred);
    processingModeChoice.setTooltip (
        "Normal uses the complementary FFT-domain crossover and 2x overlap. Quality uses "
        "the time-domain LR8 crossover and 4x STFT overlap for the most accurate transient "
        "response at higher CPU cost.");
    processingModeLabel.setTooltip (processingModeChoice.getTooltip());
    processingModeAttachment = std::make_unique<ComboBoxAttachment> (
        processor.getParameterState(), processingMode, processingModeChoice);

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
    fftAttachment = std::make_unique<ComboBoxAttachment> (processor.getParameterState(),
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

    for (auto* button : { &matrixDiagonalButton, &matrixResetButton })
        routingPage.addAndMakeVisible (*button);

    matrixDiagonalButton.onClick = [this]
    {
        std::array<float, 25> values {};
        for (int channel = 0; channel < 5; ++channel)
            values[static_cast<size_t> (channel * 5 + channel)] = 1.0f;
        applyMatrixValues (values);
    };
    matrixResetButton.onClick = [this] { applyMatrixValues (defaultMatrixValues); };
    matrixDiagonalButton.setTooltip ("Route every voice channel only to the matching main channel.");
    matrixResetButton.setTooltip ("Restore the default ducking matrix.");

    addAndMakeVisible (voiceFormatLabel);
    addAndMakeVisible (voiceFormatChoice);
    voiceFormatLabel.setText ("VOICE", juce::dontSendNotification);
    voiceFormatLabel.setJustificationType (juce::Justification::centredLeft);
    voiceFormatLabel.setColour (juce::Label::textColourId, secondaryTextColour);
    voiceFormatLabel.setFont (juce::FontOptions { 11.0f, juce::Font::bold });
    voiceFormatChoice.addItem ("STEREO", 1);
    voiceFormatChoice.addItem ("SURROUND", 2);
    voiceFormatChoice.setJustificationType (juce::Justification::centred);
    voiceFormatChoice.setTooltip ("Select whether the sidechain voice signal is stereo or surround.");
    voiceFormatLabel.setTooltip (voiceFormatChoice.getTooltip());
    voiceFormatAttachment = std::make_unique<ComboBoxAttachment> (
        processor.getParameterState(), voiceFormat, voiceFormatChoice);

    for (auto* toggle : { &downmixToggle, &dropLfeToggle })
    {
        addAndMakeVisible (*toggle);
        toggle->setVisible (processor.isSurroundVariant());
    }

    downmixToggle.setTooltip ("Downmix the processed main 5.1 signal to stereo at the output.");
    dropLfeToggle.setTooltip ("Exclude the LFE channel when creating the stereo downmix.");
    downmixAttachment = std::make_unique<ButtonAttachment> (
        processor.getParameterState(), downmix, downmixToggle);
    dropLfeAttachment = std::make_unique<ButtonAttachment> (
        processor.getParameterState(), dropLfe, dropLfeToggle);

    if (const auto* downmixValue = processor.getParameterState().getRawParameterValue (downmix))
        dropLfeToggle.setEnabled (downmixValue->load() >= 0.5f);

    setAdvancedPage (AdvancedPage::dynamics);
    setEditorMode (EditorMode::simple);
    processor.setVisualisationFrameRate (meterRefreshHz);
    processor.notifyEditorOpened();
    startTimerHz (static_cast<int> (meterRefreshHz));
}

SmartDuckingAudioProcessorEditor::~SmartDuckingAudioProcessorEditor()
{
    stopTimer();
    processor.notifyEditorClosed();
    tooltipWindow.reset();
    setLookAndFeel (nullptr);
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
        processor.getParameterState(), parameterId, result.slider));

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
    processor.notifyEditorPaint();
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

    const auto onWaveLogoBounds = juce::Rectangle<float> (18.0f, 12.0f, 62.0f, 40.0f);

    if (onWaveLogo.isValid())
    {
        graphics.saveState();
        graphics.setOpacity (1.0f);
        graphics.setColour (textColour);
        graphics.drawImage (onWaveLogo,
                            onWaveLogoBounds,
                            juce::RectanglePlacement::centred,
                            true);
        graphics.restoreState();
    }

    graphics.setColour (textColour);
    graphics.setFont (juce::FontOptions { 21.0f, juce::Font::bold });
    constexpr int titleX = 88;
    constexpr int titleWidth = 158;
    graphics.drawFittedText ("SMART DUCKING",
                             juce::Rectangle<int> (titleX, 17, titleWidth, 36),
                             juce::Justification::centredLeft,
                             1,
                             0.62f);

    constexpr float pluginLogoX = static_cast<float> (titleX + titleWidth + 6);
    const auto pluginLogoRight = juce::jmax (pluginLogoX + 24.0f,
                                              static_cast<float> (voiceFormatLabel.getX() - 8));
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
    const auto outputChannels = processor.isSurroundVariant() ? 5 : 2;
    const auto* vlcMode = processor.getParameterState().getRawParameterValue (
        smart_ducking::parameter_ids::vlcMode);
    const auto vlcModeIsEnabled = processor.isSurroundVariant()
                               && vlcMode != nullptr
                               && vlcMode->load() >= 0.5f;
    auto mainInputMeter = leftMeters.removeFromLeft (leftHalf);
    mainInputMeterBounds = mainInputMeter;

    drawSignalMeter (graphics,
                     mainInputMeter,
                     "MAIN IN",
                     displayedMainPeaks,
                     secondaryTextColour,
                     processor.isSurroundVariant() ? 5 : 2,
                     nullptr,
                     nullptr,
                     vlcModeIsEnabled);
    leftMeters.removeFromLeft (6.0f);
    drawSignalMeter (graphics,
                     leftMeters,
                     "VOICE SC",
                     displayedVoicePeaks,
                     voiceColour,
                     voiceChannels);
    drawSignalMeter (graphics,
                     rightMeters,
                     "OUTPUT",
                     displayedOutputPeaks,
                     secondaryTextColour,
                     outputChannels,
                     &displayedReductionDb,
                     &displayedSpectralDb,
                     false);

    const auto lowerBounds = (editorMode == EditorMode::simple ? simplePanel.getBounds()
                                                                : advancedPanel.getBounds())
                                 .toFloat();
    graphics.setColour (juce::Colours::black.withAlpha (0.08f));
    graphics.fillRoundedRectangle (lowerBounds, 10.0f);
    graphics.setColour (borderColour);
    graphics.drawRoundedRectangle (lowerBounds.reduced (0.5f), 10.0f, 1.0f);
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    processor.recordEditorPaintDuration (static_cast<std::uint64_t> (
        juce::Time::getHighResolutionTicks() - diagnosticPaintStart));
#endif
}

void SmartDuckingAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop (headerHeight).reduced (12, 10);
    auto lampArea = header.removeFromRight (24);
    duckingLampBounds = lampArea.toFloat().withSizeKeepingCentre (12.0f, 12.0f);
    header.removeFromRight (6);

    const auto placeHeaderControl = [&header] (juce::Component& component,
                                                int width,
                                                int height)
    {
        component.setBounds (header.removeFromRight (width)
                                   .withSizeKeepingCentre (width, height));
    };

    if (processor.isSurroundVariant())
    {
        placeHeaderControl (dropLfeToggle, 88, 34);
        header.removeFromRight (4);
        placeHeaderControl (downmixToggle, 86, 34);
        header.removeFromRight (8);
    }

    placeHeaderControl (voiceFormatChoice, 112, 38);
    header.removeFromRight (4);
    placeHeaderControl (voiceFormatLabel, 45, 28);

    bounds.reduce (12, 10);
    const auto leftMeterWidth = juce::jlimit (150, 166, getWidth() / 6);
    const auto rightMeterWidth = juce::jlimit (90, 100, getWidth() / 10);
    const auto bottomHeight = juce::jlimit (210, 250, getHeight() * 34 / 100);
    auto lower = bounds.removeFromBottom (bottomHeight);
    bounds.removeFromBottom (10);
    simplePanel.setBounds (lower);
    advancedPanel.setBounds (lower);

    auto modeRail = lower.reduced (10).removeFromRight (82);
    const auto modeButtonHeight = 42;
    const auto modeButtonsHeight = modeButtonHeight * 2 + 6;
    modeRail = modeRail.withSizeKeepingCentre (modeRail.getWidth(), modeButtonsHeight);
    simpleModeButton.setBounds (modeRail.removeFromTop (modeButtonHeight));
    modeRail.removeFromTop (6);
    advancedModeButton.setBounds (modeRail.removeFromTop (modeButtonHeight));

    spectrumPanel->setBounds (bounds.withTrimmedLeft (leftMeterWidth)
                                    .withTrimmedRight (rightMeterWidth));

    if (editorMode == EditorMode::simple)
    {
        layoutSimplePanel();
    }
    else
    {
        layoutAdvancedPanel();
    }

    simpleModeButton.toFront (false);
    advancedModeButton.toFront (false);
}

void SmartDuckingAudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
    if (! processor.isSurroundVariant()
        || ! event.mods.isPopupMenu()
        || ! mainInputMeterBounds.contains (event.position))
        return;

    auto* parameter = processor.getParameterState().getParameter (
        smart_ducking::parameter_ids::vlcMode);

    if (parameter == nullptr)
        return;

    const auto vlcOrderIsEnabled = parameter->getValue() >= 0.5f;
    juce::PopupMenu menu;
    menu.setLookAndFeel (onWaveLookAndFeel.get());
    menu.addSectionHeader ("MAIN IN CHANNEL ORDER");
    menu.addItem (1,
                  "L R C Lfe Ls Rs",
                  true,
                  ! vlcOrderIsEnabled);
    menu.addItem (2,
                  "L R Ls Rs C Lfe",
                  true,
                  vlcOrderIsEnabled);
    const auto clickArea = localAreaToGlobal (
        juce::Rectangle<int> (event.getPosition().x,
                              event.getPosition().y,
                              1,
                              1));
    auto safeThis = juce::Component::SafePointer<SmartDuckingAudioProcessorEditor> (this);
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetComponent (*this)
            .withTargetScreenArea (clickArea)
            .withPreferredPopupDirection (
                juce::PopupMenu::Options::PopupDirection::upwards)
            .withMinimumWidth (220)
            .withStandardItemHeight (30)
            .withInitiallySelectedItem (vlcOrderIsEnabled ? 2 : 1)
            .withParentComponent (this)
            .withDeletionCheck (*this),
        [safeThis] (int result)
        {
            if ((result != 1 && result != 2) || safeThis == nullptr)
                return;

            if (auto* vlcParameter = safeThis->processor.getParameterState().getParameter (
                    smart_ducking::parameter_ids::vlcMode))
            {
                const auto requestedValue = result == 2 ? 1.0f : 0.0f;

                if (std::abs (vlcParameter->getValue() - requestedValue) < 0.001f)
                    return;

                vlcParameter->beginChangeGesture();
                vlcParameter->setValueNotifyingHost (requestedValue);
                vlcParameter->endChangeGesture();
                safeThis->repaint();
            }
        });
}

void SmartDuckingAudioProcessorEditor::setEditorMode (EditorMode newMode)
{
    const auto enteringAdvanced = newMode == EditorMode::advanced
                               && editorMode != EditorMode::advanced;
    editorMode = newMode;

    if (enteringAdvanced)
        setAdvancedPage (AdvancedPage::dynamics);

    simplePanel.setVisible (editorMode == EditorMode::simple);
    advancedPanel.setVisible (editorMode == EditorMode::advanced);
    updateModeButtons();
    resized();
    repaint();
}

void SmartDuckingAudioProcessorEditor::setAdvancedPage (AdvancedPage newPage)
{
    advancedPage = newPage;
    dynamicsPage.setVisible (advancedPage == AdvancedPage::dynamics);
    spectrumPage.setVisible (advancedPage == AdvancedPage::spectrum);
    routingPage.setVisible (advancedPage == AdvancedPage::routing);
    updateAdvancedButtons();
    layoutAdvancedPanel();
    repaint();
}

void SmartDuckingAudioProcessorEditor::updateModeButtons()
{
    simpleModeButton.setToggleState (editorMode == EditorMode::simple,
                                     juce::dontSendNotification);
    advancedModeButton.setToggleState (editorMode == EditorMode::advanced,
                                       juce::dontSendNotification);
}

void SmartDuckingAudioProcessorEditor::updateAdvancedButtons()
{
    for (size_t index = 0; index < advancedPageButtons.size(); ++index)
        advancedPageButtons[index].setToggleState (
            static_cast<int> (advancedPage) == static_cast<int> (index),
            juce::dontSendNotification);
}

void SmartDuckingAudioProcessorEditor::layoutSimplePanel()
{
    if (simplePanel.getWidth() <= 0)
        return;

    auto bounds = simplePanel.getLocalBounds().reduced (12, 9);
    bounds.removeFromRight (90);
    simplePageLabel.setBounds (bounds.removeFromTop (34));
    bounds.removeFromTop (8);
    const auto width = bounds.getWidth() / juce::jmax (1, static_cast<int> (simpleControls.size()));

    for (size_t index = 0; index < simpleControls.size(); ++index)
        simpleControls[index]->setBounds (bounds.removeFromLeft (
            index == simpleControls.size() - 1 ? bounds.getWidth() : width));
}

void SmartDuckingAudioProcessorEditor::layoutAdvancedPanel()
{
    if (advancedPanel.getWidth() <= 0)
        return;

    auto bounds = advancedPanel.getLocalBounds().reduced (12, 9);
    bounds.removeFromRight (90);
    auto navigation = bounds.removeFromTop (34);
    const auto navigationWidth = navigation.getWidth()
                               / static_cast<int> (advancedPageButtons.size());

    for (size_t index = 0; index < advancedPageButtons.size(); ++index)
        advancedPageButtons[index].setBounds (navigation.removeFromLeft (
            index == advancedPageButtons.size() - 1 ? navigation.getWidth() : navigationWidth));

    bounds.removeFromTop (8);
    for (auto* page : { &dynamicsPage, &spectrumPage, &routingPage })
        page->setBounds (bounds);

    if (! dynamicsControls.empty())
    {
        auto page = dynamicsPage.getLocalBounds();
        const auto referenceWidth = page.getWidth()
                                  / juce::jmax (1, static_cast<int> (simpleControls.size()));
        const auto width = juce::jmin (referenceWidth,
                                       page.getWidth()
                                           / static_cast<int> (dynamicsControls.size()));
        page = page.withSizeKeepingCentre (
            width * static_cast<int> (dynamicsControls.size()),
            page.getHeight());
        for (size_t index = 0; index < dynamicsControls.size(); ++index)
            dynamicsControls[index]->setBounds (page.removeFromLeft (
                index == dynamicsControls.size() - 1 ? page.getWidth() : width));
    }

    if (! spectrumControls.empty())
    {
        auto page = spectrumPage.getLocalBounds();
        const auto referenceWidth = page.getWidth()
                                  / juce::jmax (1, static_cast<int> (simpleControls.size()));
        const auto controlsWidth = referenceWidth
                                 * static_cast<int> (spectrumControls.size());
        auto fftArea = page.removeFromRight (juce::jmax (130,
                                                         page.getWidth() - controlsWidth))
                               .reduced (8);
        auto modeArea = fftArea.removeFromTop (juce::jmin (68, fftArea.getHeight() / 2));
        processingModeLabel.setBounds (modeArea.removeFromTop (26));
        processingModeChoice.setBounds (modeArea.removeFromTop (34));
        fftArea.removeFromTop (4);
        fftLabel.setBounds (fftArea.removeFromTop (26));
        fftChoice.setBounds (fftArea.removeFromTop (34));
        const auto width = page.getWidth() / static_cast<int> (spectrumControls.size());
        for (size_t index = 0; index < spectrumControls.size(); ++index)
            spectrumControls[index]->setBounds (page.removeFromLeft (
                index == spectrumControls.size() - 1 ? page.getWidth() : width));
    }

    layoutMatrix();
}

int SmartDuckingAudioProcessorEditor::getVoiceSpatialChannelCount() const noexcept
{
    const auto* voiceFormat = processor.getParameterState().getRawParameterValue (
        smart_ducking::parameter_ids::voiceFormat);
    return voiceFormat != nullptr && voiceFormat->load() >= 0.5f ? 5 : 3;
}

int SmartDuckingAudioProcessorEditor::getMainSpatialChannelCount() const noexcept
{
    return processor.isSurroundVariant() ? 5 : 3;
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

void SmartDuckingAudioProcessorEditor::timerCallback()
{
    const auto shouldVisualise = isShowing();

    if (shouldVisualise != editorVisualisationActive)
    {
        editorVisualisationActive = shouldVisualise;

        if (editorVisualisationActive)
            processor.notifyEditorOpened();
        else
            processor.notifyEditorClosed();
    }

    if (! editorVisualisationActive)
        return;

#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    const auto diagnosticTimerStart = juce::Time::getHighResolutionTicks();
#endif
    processor.notifyEditorTimer();

    if (processor.isSurroundVariant())
    {
        if (const auto* downmix = processor.getParameterState().getRawParameterValue (
                smart_ducking::parameter_ids::downmix))
            dropLfeToggle.setEnabled (downmix->load() >= 0.5f);
    }

    if (getVoiceSpatialChannelCount() != displayedMatrixRows
        || getMainSpatialChannelCount() != displayedMatrixColumns)
        layoutMatrix();

    const std::array<float, 5> mainPeaks {
        processor.getMainInputPeakLeft(),
        processor.getMainInputPeakCentre(),
        processor.getMainInputPeakRight(),
        processor.getMainInputPeakLeftSurround(),
        processor.getMainInputPeakRightSurround()
    };
    const std::array<float, 5> voicePeaks {
        processor.getSidechainPeakLeft(),
        processor.getSidechainPeakCentre(),
        processor.getSidechainPeakRight(),
        processor.getSidechainPeakLeftSurround(),
        processor.getSidechainPeakRightSurround()
    };
    const std::array<float, 5> outputPeaks {
        processor.getOutputPeakLeft(),
        processor.getOutputPeakCentre(),
        processor.getOutputPeakRight(),
        processor.getOutputPeakLeftSurround(),
        processor.getOutputPeakRightSurround()
    };
    const std::array<float, 5> wideReduction {
        processor.getGainReductionLeftDb(),
        processor.getGainReductionCentreDb(),
        processor.getGainReductionRightDb(),
        processor.getGainReductionLeftSurroundDb(),
        processor.getGainReductionRightSurroundDb()
    };
    const std::array<float, 5> spectralReduction {
        processor.getSpectralReductionLeftDb(),
        processor.getSpectralReductionCentreDb(),
        processor.getSpectralReductionRightDb(),
        processor.getSpectralReductionLeftSurroundDb(),
        processor.getSpectralReductionRightSurroundDb()
    };

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

    const auto maximumWide = *std::max_element (displayedReductionDb.begin(),
                                                 displayedReductionDb.end());
    const auto maximumSpectral = *std::max_element (displayedSpectralDb.begin(),
                                                     displayedSpectralDb.end());
    spectrumPanel->updateSpectrum();

    const auto ducking = juce::jmax (maximumWide, maximumSpectral) > 0.05f;
    const auto duckingStateChanged = ducking != duckingIndicatorActive;
    duckingIndicatorActive = ducking;

    const auto spectrum = spectrumPanel->getBounds().toFloat();
    const auto leftMeterWidth = juce::jmax (108.0f, spectrum.getX() - 24.0f);
    const auto rightMeterX = spectrum.getRight() + 8.0f;
    const auto rightMeterWidth = static_cast<float> (getWidth()) - rightMeterX - 12.0f;
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

    if (duckingStateChanged)
        repaint (duckingLampBounds.expanded (6.0f).toNearestInt());
#if SMART_DUCKING_ENABLE_DIAGNOSTICS
    processor.recordEditorTimerDuration (static_cast<std::uint64_t> (
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
    bool highlightLabel)
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
        graphics.setColour (highlightLabel ? maskColour : secondaryTextColour);
        graphics.setFont (juce::FontOptions { 9.5f, juce::Font::bold });
        graphics.drawFittedText (label,
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
        const auto channel = meterValueIndexForRow (row, visibleChannels);
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
        graphics.drawFittedText (label,
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

        for (int channel = 0; channel < reductionChannels; ++channel)
        {
            const auto groupX = groupXForChannel (channel);
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
