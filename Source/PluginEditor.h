#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

class SmartDuckingAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                private juce::Timer
{
public:
    explicit SmartDuckingAudioProcessorEditor (SmartDuckingAudioProcessor&);
    ~SmartDuckingAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct OnWaveLookAndFeel;
    struct ParameterControl;
    struct EnvelopePanel;
    struct SpectrumPanel;

    enum class EditorMode
    {
        simple,
        advanced
    };

    enum class AdvancedPage
    {
        dynamics,
        spectrum,
        routing
    };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    ParameterControl& addSliderControl (juce::Component& page,
                                        std::vector<std::unique_ptr<ParameterControl>>& collection,
                                        const char* parameterId,
                                        const juce::String& title,
                                        const juce::String& unit,
                                        bool percentage = false,
                                        bool linear = false);
    void setEditorMode (EditorMode);
    void setAdvancedPage (AdvancedPage);
    void updateModeButtons();
    void updateAdvancedButtons();
    void layoutSimplePanel();
    void layoutAdvancedPanel();
    void layoutMatrix();
    void setMonitorMode (smart_ducking::MonitorMode);
    void updateStageButtons();
    int getVoiceSpatialChannelCount() const noexcept;
    int getMainSpatialChannelCount() const noexcept;
    void applyMatrixValues (const std::array<float, 25>&);

    static void drawSignalMeter (juce::Graphics&,
                                 juce::Rectangle<float>,
                                 const juce::String&,
                                 const std::array<float, 5>&,
                                 juce::Colour,
                                 int visibleChannels,
                                 const std::array<float, 5>* wideReductionValues = nullptr,
                                 const std::array<float, 5>* spectralReductionValues = nullptr,
                                 bool highlightLabel = false);

    SmartDuckingAudioProcessor& processor;
    std::unique_ptr<OnWaveLookAndFeel> onWaveLookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<SpectrumPanel> spectrumPanel;
    juce::Image onWaveLogo;

    juce::TextButton simpleModeButton { "SIMPLE" };
    juce::TextButton advancedModeButton { "ADVANCED" };
    juce::Component stageControlPanel;
    std::array<juce::Label, 3> stageLabels;
    std::array<juce::TextButton, 9> stageButtons {
        juce::TextButton { "B" }, juce::TextButton { "S" },
        juce::TextButton { juce::String::charToString (0x0394) },
        juce::TextButton { "B" }, juce::TextButton { "S" },
        juce::TextButton { juce::String::charToString (0x0394) },
        juce::TextButton { "B" }, juce::TextButton { "S" },
        juce::TextButton { juce::String::charToString (0x0394) }
    };

    juce::Component simplePanel;
    juce::Component advancedPanel;
    juce::Component dynamicsPage;
    juce::Component spectrumPage;
    juce::Component routingPage;
    juce::Label simplePageLabel;
    std::array<juce::TextButton, 3> advancedPageButtons {
        juce::TextButton { "DYNAMICS" },
        juce::TextButton { "SPECTRUM" },
        juce::TextButton { "ROUTING" }
    };

    std::vector<std::unique_ptr<ParameterControl>> simpleControls;
    std::vector<std::unique_ptr<ParameterControl>> dynamicsControls;
    std::vector<std::unique_ptr<ParameterControl>> spectrumControls;
    std::vector<std::unique_ptr<ParameterControl>> matrixControls;

    juce::Label fftLabel;
    juce::ComboBox fftChoice;
    juce::Label processingModeLabel;
    juce::ComboBox processingModeChoice;
    juce::Label voiceFormatLabel;
    juce::ComboBox voiceFormatChoice;
    juce::ToggleButton downmixToggle { "DOWNMIX" };
    juce::ToggleButton dropLfeToggle { "DROP LFE" };

    juce::Label matrixVoiceHeader;
    juce::Label matrixOriginalHeader;
    std::array<juce::Label, 5> matrixRowHeaders;
    std::array<juce::Label, 5> matrixColumnHeaders;
    juce::TextButton matrixDiagonalButton { "DIAGONAL" };
    juce::TextButton matrixCentreButton { "CENTRE FOCUS" };
    juce::TextButton matrixResetButton { "RESET" };

    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::unique_ptr<ComboBoxAttachment> fftAttachment;
    std::unique_ptr<ComboBoxAttachment> processingModeAttachment;
    std::unique_ptr<ComboBoxAttachment> voiceFormatAttachment;
    std::unique_ptr<ButtonAttachment> downmixAttachment;
    std::unique_ptr<ButtonAttachment> dropLfeAttachment;
    std::array<std::unique_ptr<ButtonAttachment>, 3> stageBypassAttachments;

    std::array<float, 5> displayedMainPeaks {};
    std::array<float, 5> displayedVoicePeaks {};
    std::array<float, 5> displayedOutputPeaks {};
    std::array<float, 5> displayedReductionDb {};
    std::array<float, 5> displayedSpectralDb {};

    EditorMode editorMode = EditorMode::simple;
    AdvancedPage advancedPage = AdvancedPage::routing;
    juce::Rectangle<float> duckingLampBounds;
    juce::Rectangle<float> activityStatusBounds;
    juce::Rectangle<float> mainInputMeterBounds;
    bool duckingIndicatorActive = false;
    bool editorVisualisationActive = true;
    int displayedMatrixRows = 0;
    int displayedMatrixColumns = 0;

    static constexpr int editorWidth = 1000;
    static constexpr int editorHeight = 650;
    static constexpr int headerHeight = 72;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmartDuckingAudioProcessorEditor)
};
