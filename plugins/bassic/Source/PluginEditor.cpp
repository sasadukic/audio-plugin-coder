#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
constexpr int kPanelMargin = 10;
constexpr int kFaderWidth = 30;
constexpr int kFaderHeight = 190;
constexpr int kKnobSize = 48;
constexpr int kComboWidth = 86;
constexpr int kComboHeight = 26;
constexpr int kToggleHeight = 34;
constexpr int kLabelHeight = 20;
constexpr int kControlPanelHeight = 340;
const juce::Colour kBgMain = juce::Colour::fromRGB (15, 17, 20);       // #0f1114
const juce::Colour kBgPanel = juce::Colour::fromRGB (23, 26, 31);       // #171a1f
const juce::Colour kBgControl = juce::Colour::fromRGB (30, 34, 40);     // #1e2228
const juce::Colour kBorder = juce::Colour::fromRGB (42, 47, 54);        // #2a2f36
const juce::Colour kTextPrimary = juce::Colour::fromRGB (230, 230, 230);// #e6e6e6
const juce::Colour kTextMuted = juce::Colour::fromRGB (139, 145, 153);  // #8b9199
const juce::Colour kNeutralCap = juce::Colour::fromRGB (181, 181, 181); // #b5b5b5
}

void BassicAudioProcessorEditor::SynthLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                                    float sliderPosProportional, float rotaryStartAngle,
                                                                    float rotaryEndAngle, juce::Slider&)
{
    const float size = juce::jmax (8.0f, (float) juce::jmin (width, height) - 12.0f);
    const auto bounds = juce::Rectangle<float> (size, size)
        .withCentre (juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).getCentre());
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    g.setColour (kBgControl);
    g.fillEllipse (bounds);
    g.setColour (kBorder);
    g.drawEllipse (bounds, 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.0f, -radius + 6.0f, 2.0f, radius * 0.55f, 1.0f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    g.setColour (kTextPrimary);
    g.fillPath (pointer);
}

void BassicAudioProcessorEditor::SynthLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                                                    float sliderPos, float minSliderPos, float maxSliderPos,
                                                                    const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused (slider);
    if (style != juce::Slider::LinearVertical)
        return;

    const auto body = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f, 2.0f);
    g.setColour (kBgControl);
    g.fillRoundedRectangle (body, 4.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (body, 4.0f, 1.0f);

    const auto track = juce::Rectangle<float> (body.getCentreX() - 2.0f, body.getY() + 10.0f, 4.0f, body.getHeight() - 20.0f);
    g.setColour (kBorder);
    g.fillRoundedRectangle (track, 2.0f);

    const float sourceMin = juce::jmin (minSliderPos, maxSliderPos);
    const float sourceMax = juce::jmax (minSliderPos, maxSliderPos);
    float mappedSliderPos = sliderPos;
    const float travelMin = track.getY() + 8.0f;
    const float travelMax = track.getBottom() - 8.0f;

    if (sourceMax > sourceMin + 1.0e-6f && travelMax > travelMin)
        mappedSliderPos = juce::jmap (sliderPos, sourceMin, sourceMax, travelMax, travelMin);

    mappedSliderPos = juce::jlimit (travelMin, travelMax, mappedSliderPos);

    juce::Rectangle<float> thumb (body.getX() + 5.0f, mappedSliderPos - 11.0f, body.getWidth() - 10.0f, 22.0f);
    g.setColour (kNeutralCap);
    g.fillRoundedRectangle (thumb, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.drawRoundedRectangle (thumb, 3.0f, 1.0f);
    g.drawLine (thumb.getX() + 2.0f, thumb.getCentreY(), thumb.getRight() - 2.0f, thumb.getCentreY(), 2.0f);
}

void BassicAudioProcessorEditor::SynthLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                                                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    const bool drawVertical = button.getProperties().getWithDefault ("flat101Vertical", false);
    const bool hideText = button.getProperties().getWithDefault ("flat101HideText", false);
    auto bounds = button.getLocalBounds().toFloat().reduced (2.0f);
    constexpr float textLaneHeight = 13.0f;
    auto switchArea = bounds;
    auto textArea = bounds;
    if (!drawVertical && !hideText)
        switchArea = bounds.withTrimmedBottom (textLaneHeight);

    juce::Rectangle<float> pill;
    float knobD = 0.0f;
    juce::Rectangle<float> knob;

    if (drawVertical)
    {
        const float w = juce::jmin (20.0f, switchArea.getWidth());
        const float h = juce::jlimit (48.0f, 90.0f, switchArea.getHeight());
        pill = juce::Rectangle<float> (switchArea.getCentreX() - w * 0.5f, switchArea.getCentreY() - h * 0.5f, w, h);
        knobD = w - 6.0f;
        const bool on = button.getToggleState();
        const float knobY = on ? (pill.getY() + 3.0f) : (pill.getBottom() - knobD - 3.0f);
        knob = juce::Rectangle<float> (pill.getCentreX() - knobD * 0.5f, knobY, knobD, knobD);
    }
    else
    {
        const float h = juce::jmin (22.0f, switchArea.getHeight());
        const float pillW = juce::jlimit (42.0f, 56.0f, h * 2.2f);
        pill = juce::Rectangle<float> (switchArea.getCentreX() - pillW * 0.5f, switchArea.getCentreY() - h * 0.5f, pillW, h);
        knobD = h - 6.0f;
        const bool on = button.getToggleState();
        const float knobX = on ? (pill.getRight() - knobD - 3.0f) : (pill.getX() + 3.0f);
        knob = juce::Rectangle<float> (knobX, pill.getY() + 3.0f, knobD, knobD);
    }

    g.setColour (kBgControl);
    const float cornerRadius = juce::jmin (pill.getWidth(), pill.getHeight()) * 0.5f;
    g.fillRoundedRectangle (pill, cornerRadius);
    g.setColour (kBorder);
    g.drawRoundedRectangle (pill, cornerRadius, 1.0f);
    g.setColour (kTextPrimary);
    g.fillEllipse (knob);

    if (!hideText)
    {
        g.setColour (kTextMuted);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        const auto text = button.getButtonText();
        if (drawVertical)
            g.drawText (text, button.getLocalBounds().withY ((int) (pill.getBottom() + 2.0f)), juce::Justification::centredTop, true);
        else
            g.drawText (text, textArea.getSmallestIntegerContainer().removeFromBottom ((int) textLaneHeight),
                        juce::Justification::centred, true);
    }
}

BassicAudioProcessorEditor::BassicAudioProcessorEditor (BassicAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p)
{
    setLookAndFeel (&synthLnf);

    sections = {
        { "VCO", {}, 1.0f },
        { "SOURCE MIXER", {}, 1.35f },
        { "VCF", {}, 1.3f },
        { "VCA", {}, 0.55f },
        { "ENV", {}, 1.1f },
        { "GLOBAL", {}, 1.1f }
    };

    addToSection (0, addChoice ("vcoRange", "RANGE"));

    addToSection (1, addFader ("saw", "SAW"));
    addToSection (1, addFader ("square", "PULSE"));
    addToSection (1, addToggle ("subMode", "SUB -2 OCT"));
    addToSection (1, addFader ("sub", "SUB LEVEL"));
    addToSection (1, addFader ("noise", "NOISE"));

    addToSection (2, addFader ("cutoff", "FREQ"));
    addToSection (2, addFader ("resonance", "RES"));
    addToSection (2, addFader ("envAmt", "ENV"));
    addToSection (2, addFader ("vcfMod", "MOD"));
    addToSection (2, addFader ("vcfKybd", "KYBD"));

    addToSection (3, addToggle ("vcaMode", "GATE MODE"));

    addToSection (4, addFader ("attack", "A"));
    addToSection (4, addFader ("decay", "D"));
    addToSection (4, addFader ("sustain", "S"));
    addToSection (4, addFader ("release", "R"));

    addToSection (5, addKnob ("level", "VOLUME"));
    addToSection (5, addKnob ("portamento", "PORTAMENTO"));
    addToSection (5, addChoice ("portamentoMode", "GLIDE MODE"));

    setSize (1580, 720);
}

BassicAudioProcessorEditor::~BassicAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

juce::Slider* BassicAudioProcessorEditor::addFader (const juce::String& paramId, const juce::String& labelText, bool colouredCap)
{
    juce::ignoreUnused (colouredCap);
    auto slider = std::make_unique<juce::Slider>();
    slider->setSliderStyle (juce::Slider::LinearVertical);
    slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider->getProperties().set ("flat101Colored", false);
    addAndMakeVisible (*slider);

    auto label = std::make_unique<juce::Label>();
    label->setText (labelText, juce::dontSendNotification);
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, kTextMuted);
    label->setFont (juce::FontOptions (10.0f, juce::Font::bold));
    addAndMakeVisible (*label);

    sliderAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.parameters, paramId, *slider));

    auto* ptr = slider.get();
    sliders.push_back (std::move (slider));
    labels.push_back (std::move (label));
    return ptr;
}

juce::Slider* BassicAudioProcessorEditor::addKnob (const juce::String& paramId, const juce::String& labelText)
{
    auto slider = std::make_unique<juce::Slider>();
    slider->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider->setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                 juce::MathConstants<float>::pi * 2.8f,
                                 true);
    slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible (*slider);

    auto label = std::make_unique<juce::Label>();
    label->setText (labelText, juce::dontSendNotification);
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, kTextMuted);
    label->setFont (juce::FontOptions (10.0f, juce::Font::bold));
    addAndMakeVisible (*label);

    sliderAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.parameters, paramId, *slider));

    auto* ptr = slider.get();
    sliders.push_back (std::move (slider));
    labels.push_back (std::move (label));
    return ptr;
}

juce::ComboBox* BassicAudioProcessorEditor::addChoice (const juce::String& paramId, const juce::String& labelText)
{
    auto combo = std::make_unique<juce::ComboBox>();
    combo->setColour (juce::ComboBox::backgroundColourId, kBgControl);
    combo->setColour (juce::ComboBox::textColourId, kTextPrimary);
    combo->setColour (juce::ComboBox::outlineColourId, kBorder);
    addAndMakeVisible (*combo);

    auto label = std::make_unique<juce::Label>();
    label->setText (labelText, juce::dontSendNotification);
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::textColourId, kTextMuted);
    label->setFont (juce::FontOptions (10.0f, juce::Font::bold));
    addAndMakeVisible (*label);

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (processorRef.parameters.getParameter (paramId)))
    {
        for (int i = 0; i < p->choices.size(); ++i)
            combo->addItem (p->choices[i], i + 1);
    }

    comboAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef.parameters, paramId, *combo));

    auto* ptr = combo.get();
    combos.push_back (std::move (combo));
    labels.push_back (std::move (label));
    return ptr;
}

juce::ToggleButton* BassicAudioProcessorEditor::addToggle (const juce::String& paramId, const juce::String& labelText)
{
    auto button = std::make_unique<juce::ToggleButton>();
    button->setButtonText (labelText);
    button->setClickingTogglesState (true);
    if (paramId == "vcaMode")
    {
        button->getProperties().set ("flat101Vertical", true);
        button->getProperties().set ("flat101HideText", true);
    }
    addAndMakeVisible (*button);

    buttonAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processorRef.parameters, paramId, *button));

    auto* ptr = button.get();
    buttons.push_back (std::move (button));
    return ptr;
}

void BassicAudioProcessorEditor::addToSection (size_t sectionIndex, juce::Component* control)
{
    if (sectionIndex < sections.size() && control != nullptr)
        sections[sectionIndex].controls.push_back (control);
}

void BassicAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBgMain);

    auto bounds = getLocalBounds().reduced (kPanelMargin);
    auto top = bounds.removeFromTop (juce::jmin (kControlPanelHeight, bounds.getHeight()));
    g.setColour (kBgPanel);
    g.fillRect (top);

    float totalWeight = 0.0f;
    for (const auto& section : sections)
        totalWeight += juce::jmax (0.1f, section.widthWeight);

    int remainingWidth = top.getWidth();
    float remainingWeight = totalWeight;
    auto sectionArea = top;
    g.setColour (kBorder);

    for (size_t i = 0; i < sections.size(); ++i)
    {
        const float sectionWeight = juce::jmax (0.1f, sections[i].widthWeight);
        const int width = (i == sections.size() - 1)
            ? remainingWidth
            : juce::jmax (1, juce::roundToInt ((float) remainingWidth * (sectionWeight / remainingWeight)));
        auto r = sectionArea.removeFromLeft (width);
        remainingWidth -= width;
        remainingWeight -= sectionWeight;

        g.drawRect (r, 1);
        auto titleBar = r.removeFromTop (28);
        g.setColour (kBgControl);
        g.fillRect (titleBar);
        g.setColour (kTextPrimary);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (sections[i].title, titleBar, juce::Justification::centred);
        g.setColour (kBorder);
    }
}

void BassicAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (kPanelMargin);
    auto top = bounds.removeFromTop (juce::jmin (kControlPanelHeight, bounds.getHeight()));

    std::vector<int> sectionWidths;
    sectionWidths.reserve (sections.size());
    float totalWeight = 0.0f;
    for (const auto& section : sections)
        totalWeight += juce::jmax (0.1f, section.widthWeight);

    int remainingWidth = top.getWidth();
    float remainingWeight = totalWeight;
    for (size_t i = 0; i < sections.size(); ++i)
    {
        const float sectionWeight = juce::jmax (0.1f, sections[i].widthWeight);
        const int width = (i == sections.size() - 1)
            ? remainingWidth
            : juce::jmax (1, juce::roundToInt ((float) remainingWidth * (sectionWeight / remainingWeight)));
        sectionWidths.push_back (width);
        remainingWidth -= width;
        remainingWeight -= sectionWeight;
    }

    int uniformSliderWidth = kFaderWidth;
    {
        bool foundAnySection = false;
        int minItemW = kFaderWidth;
        auto probeArea = top;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            auto r = probeArea.removeFromLeft (sectionWidths[i]);
            r.removeFromTop (36);
            r.reduce (8, 8);

            const int controlCount = (int) sections[i].controls.size();
            if (controlCount <= 0)
                continue;

            const int itemGap = 4;
            const int totalGap = itemGap * juce::jmax (0, controlCount - 1);
            const int itemW = juce::jmax (16, (r.getWidth() - totalGap) / controlCount);
            minItemW = foundAnySection ? juce::jmin (minItemW, itemW) : itemW;
            foundAnySection = true;
        }

        if (foundAnySection)
            uniformSliderWidth = juce::jmax (10, juce::jmin (kFaderWidth, minItemW - 2));
    }

    auto sectionArea = top;

    int labelIndex = 0;

    for (size_t i = 0; i < sections.size(); ++i)
    {
        auto r = sectionArea.removeFromLeft (sectionWidths[i]);
        r.removeFromTop (36);
        r.reduce (8, 8);

        const int controlCount = (int) sections[i].controls.size();
        if (controlCount <= 0)
            continue;

        const int itemGap = 4;
        const int totalGap = itemGap * juce::jmax (0, controlCount - 1);
        const int itemW = juce::jmax (16, (r.getWidth() - totalGap) / controlCount);
        const int usedWidth = itemW * controlCount + totalGap;
        int itemX = r.getX() + juce::jmax (0, (r.getWidth() - usedWidth) / 2);

        for (int c = 0; c < controlCount; ++c)
        {
            auto item = juce::Rectangle<int> (itemX, r.getY(), itemW, r.getHeight()).reduced (1, 0);
            itemX += itemW + itemGap;

            if (auto* slider = dynamic_cast<juce::Slider*> (sections[i].controls[(size_t) c]))
            {
                if (slider->getSliderStyle() == juce::Slider::LinearVertical)
                {
                    auto labelArea = item.removeFromBottom (kLabelHeight);
                    const int faderH = juce::jmax (24, juce::jmin (kFaderHeight, item.getHeight()));
                    auto faderArea = item.removeFromBottom (faderH);
                    slider->setBounds (juce::Rectangle<int> (uniformSliderWidth, faderH).withCentre (faderArea.getCentre()));
                    if (labelIndex < (int) labels.size())
                        labels[(size_t) labelIndex++]->setBounds (labelArea);
                }
                else
                {
                    const int maxKnobH = juce::jmax (18, item.getHeight() - kLabelHeight);
                    const int knobSize = juce::jmax (18, juce::jmin (kKnobSize, juce::jmin (item.getWidth(), maxKnobH)));
                    auto knobArea = item.removeFromTop (knobSize);
                    slider->setBounds (juce::Rectangle<int> (knobSize, knobSize).withCentre (knobArea.getCentre()));
                    if (labelIndex < (int) labels.size())
                        labels[(size_t) labelIndex++]->setBounds (item.removeFromTop (kLabelHeight));
                }
            }
            else if (auto* combo = dynamic_cast<juce::ComboBox*> (sections[i].controls[(size_t) c]))
            {
                if (labelIndex < (int) labels.size())
                    labels[(size_t) labelIndex++]->setBounds (item.removeFromTop (kLabelHeight));
                const int comboH = juce::jmax (18, juce::jmin (kComboHeight, item.getHeight()));
                auto comboArea = item.removeFromTop (comboH);
                const int comboW = juce::jmax (24, juce::jmin (kComboWidth, comboArea.getWidth()));
                combo->setBounds (juce::Rectangle<int> (comboW, comboH).withCentre (comboArea.getCentre()));
            }
            else if (auto* toggle = dynamic_cast<juce::ToggleButton*> (sections[i].controls[(size_t) c]))
            {
                const bool drawVertical = toggle->getProperties().getWithDefault ("flat101Vertical", false);
                if (drawVertical)
                {
                    const int areaH = juce::jmax (52, juce::jmin (96, item.getHeight()));
                    auto toggleArea = item.removeFromTop (areaH);
                    const int toggleW = juce::jmax (16, juce::jmin (28, toggleArea.getWidth()));
                    const int toggleH = juce::jmax (52, juce::jmin (96, toggleArea.getHeight()));
                    toggle->setBounds (juce::Rectangle<int> (toggleW, toggleH).withCentre (toggleArea.getCentre()));
                }
                else
                {
                    const int toggleH = juce::jmax (18, juce::jmin (kToggleHeight, item.getHeight()));
                    auto toggleArea = item.removeFromTop (toggleH);
                    const int toggleW = juce::jmax (42, juce::jmin (56, toggleArea.getWidth()));
                    toggle->setBounds (juce::Rectangle<int> (toggleW, toggleH).withCentre (toggleArea.getCentre()));
                }
            }
        }
    }
}
