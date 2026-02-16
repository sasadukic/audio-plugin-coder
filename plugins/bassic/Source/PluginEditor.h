#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class BassicAudioProcessor;

class BassicAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit BassicAudioProcessorEditor (BassicAudioProcessor&);
    ~BassicAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SynthLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider& slider) override;
        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               const juce::Slider::SliderStyle style, juce::Slider& slider) override;
        void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    };

    struct Section
    {
        juce::String title;
        std::vector<juce::Component*> controls;
        float widthWeight = 1.0f;
    };

    BassicAudioProcessor& processorRef;
    SynthLookAndFeel synthLnf;
    std::vector<std::unique_ptr<juce::Slider>> sliders;
    std::vector<std::unique_ptr<juce::Label>> labels;
    std::vector<std::unique_ptr<juce::ComboBox>> combos;
    std::vector<std::unique_ptr<juce::ToggleButton>> buttons;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;
    std::vector<Section> sections;

    juce::Slider* addFader (const juce::String& paramId, const juce::String& labelText, bool colouredCap = false);
    juce::Slider* addKnob (const juce::String& paramId, const juce::String& labelText);
    juce::ComboBox* addChoice (const juce::String& paramId, const juce::String& labelText);
    juce::ToggleButton* addToggle (const juce::String& paramId, const juce::String& labelText);
    void addToSection (size_t sectionIndex, juce::Component* control);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassicAudioProcessorEditor)
};
