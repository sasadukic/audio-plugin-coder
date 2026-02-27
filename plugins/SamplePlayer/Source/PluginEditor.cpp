#include "PluginEditor.h"

namespace
{
constexpr int panelCornerRadius = 14;
}

SamplePlayerAudioProcessorEditor::SamplePlayerAudioProcessorEditor (SamplePlayerAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (1240, 760);
    setOpaque (true);

    titleLabel.setText ("Sample Player", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions ("Avenir Next", 36.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Kontakt-style workflow: velocity layers, round robin, looping crossfades, ADSR, filter, and custom wallpaper", juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (juce::FontOptions ("Avenir Next", 14.0f, juce::Font::plain)));
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcbd6e3));
    addAndMakeVisible (subtitleLabel);

    mappingHintLabel.setText (SamplePlayerAudioProcessor::getZoneNamingHint(), juce::dontSendNotification);
    mappingHintLabel.setJustificationType (juce::Justification::topLeft);
    mappingHintLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc2cad3));
    mappingHintLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
    addAndMakeVisible (mappingHintLabel);

    configureRotarySlider (outputGainSlider, " dB");
    configureRotarySlider (attackSlider, " ms");
    configureRotarySlider (decaySlider, " ms");
    configureRotarySlider (sustainSlider, " %");
    configureRotarySlider (releaseSlider, " ms");
    configureRotarySlider (loopStartSlider, " %");
    configureRotarySlider (loopEndSlider, " %");
    configureRotarySlider (loopCrossfadeSlider, " ms");
    configureRotarySlider (filterCutoffSlider, " Hz");
    configureRotarySlider (filterResonanceSlider, "");
    configureRotarySlider (filterEnvAmountSlider, " oct");

    sustainSlider.textFromValueFunction = [] (double v)
    {
        return juce::String (v * 100.0, 1) + " %";
    };

    filterCutoffSlider.textFromValueFunction = [] (double v)
    {
        if (v >= 1000.0)
            return juce::String (v / 1000.0, 2) + " kHz";

        return juce::String (v, 0) + " Hz";
    };

    for (auto* label : { &outputGainLabel, &attackLabel, &decayLabel, &sustainLabel, &releaseLabel,
                         &loopStartLabel, &loopEndLabel, &loopCrossfadeLabel,
                         &filterCutoffLabel, &filterResonanceLabel, &filterEnvAmountLabel,
                         &summaryTitleLabel, &zoneEditorTitleLabel, &zoneSelectorLabel,
                         &zoneRootNoteLabel, &zoneLowNoteLabel, &zoneHighNoteLabel,
                         &zoneLowVelocityLabel, &zoneHighVelocityLabel, &zoneRoundRobinLabel })
    {
        label->setColour (juce::Label::textColourId, juce::Colour (0xffeef5fc));
        label->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*label);
    }

    summaryTitleLabel.setJustificationType (juce::Justification::centredLeft);
    zoneEditorTitleLabel.setJustificationType (juce::Justification::centredLeft);

    zoneFileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb6d0e8));
    zoneFileLabel.setJustificationType (juce::Justification::centredLeft);
    zoneFileLabel.setFont (juce::Font (juce::FontOptions ("Avenir Next", 13.0f, juce::Font::italic)));
    addAndMakeVisible (zoneFileLabel);

    loopEnableButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    filterEnableButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (loopEnableButton);
    addAndMakeVisible (filterEnableButton);

    for (auto* button : { &loadFolderButton, &loadFilesButton, &clearSamplesButton, &loadWallpaperButton, &clearWallpaperButton,
                          &applyZoneButton, &reloadZoneButton })
    {
        button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff253548));
        button->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff35618f));
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible (*button);
    }

    sampleSummaryEditor.setMultiLine (true);
    sampleSummaryEditor.setReturnKeyStartsNewLine (false);
    sampleSummaryEditor.setReadOnly (true);
    sampleSummaryEditor.setScrollbarsShown (true);
    sampleSummaryEditor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
    sampleSummaryEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x9917202b));
    sampleSummaryEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2a3645));
    sampleSummaryEditor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffdce7f5));
    addAndMakeVisible (sampleSummaryEditor);

    zoneSelector.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1b2735));
    zoneSelector.setColour (juce::ComboBox::textColourId, juce::Colour (0xffe7f0fb));
    zoneSelector.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff33485d));
    zoneSelector.onChange = [this]
    {
        if (! ignoreZoneEditorCallbacks)
            populateZoneEditorFields();
    };
    addAndMakeVisible (zoneSelector);

    configureZoneEditSlider (zoneRootNoteSlider, 0.0, 127.0);
    configureZoneEditSlider (zoneLowNoteSlider, 0.0, 127.0);
    configureZoneEditSlider (zoneHighNoteSlider, 0.0, 127.0);
    configureZoneEditSlider (zoneLowVelocitySlider, 1.0, 127.0);
    configureZoneEditSlider (zoneHighVelocitySlider, 1.0, 127.0);
    configureZoneEditSlider (zoneRoundRobinSlider, 1.0, 64.0);

    outputGainAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "outputGainDb", outputGainSlider);
    attackAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "attackMs", attackSlider);
    decayAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "decayMs", decaySlider);
    sustainAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "sustain", sustainSlider);
    releaseAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "releaseMs", releaseSlider);
    loopStartAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "loopStartPct", loopStartSlider);
    loopEndAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "loopEndPct", loopEndSlider);
    loopCrossfadeAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "loopCrossfadeMs", loopCrossfadeSlider);
    filterCutoffAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "filterCutoff", filterCutoffSlider);
    filterResonanceAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "filterResonance", filterResonanceSlider);
    filterEnvAmountAttachment = std::make_unique<SliderAttachment> (processorRef.parameters, "filterEnvAmount", filterEnvAmountSlider);
    loopEnableAttachment = std::make_unique<ButtonAttachment> (processorRef.parameters, "loopEnabled", loopEnableButton);
    filterEnableAttachment = std::make_unique<ButtonAttachment> (processorRef.parameters, "filterEnabled", filterEnableButton);

    loadFolderButton.onClick = [this] { openSampleFolderChooser(); };
    loadFilesButton.onClick = [this] { openSampleFileChooser(); };
    clearSamplesButton.onClick = [this]
    {
        processorRef.clearSampleSet();
        refreshSampleSummary();
        refreshZoneEditor();
    };

    loadWallpaperButton.onClick = [this] { openWallpaperChooser(); };
    clearWallpaperButton.onClick = [this]
    {
        processorRef.setWallpaperFile (juce::File {});
        refreshWallpaperImage();
    };

    applyZoneButton.onClick = [this] { applyZoneEdits(); };
    reloadZoneButton.onClick = [this] { populateZoneEditorFields(); };

    refreshSampleSummary();
    refreshWallpaperImage();
    refreshZoneEditor();

    startTimerHz (6);
}

SamplePlayerAudioProcessorEditor::~SamplePlayerAudioProcessorEditor() = default;

void SamplePlayerAudioProcessorEditor::configureRotarySlider (juce::Slider& slider, const juce::String& suffix)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 20);
    slider.setTextValueSuffix (suffix);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.08f,
                                juce::MathConstants<float>::pi * 2.92f,
                                true);
    slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff6ea6d8));
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff304559));
    slider.setColour (juce::Slider::thumbColourId, juce::Colour (0xffd5e5f5));
    slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffecf5ff));
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff2d3b4d));
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff15202d));

    addAndMakeVisible (slider);
}

void SamplePlayerAudioProcessorEditor::configureZoneEditSlider (juce::Slider& slider, double min, double max)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setRange (min, max, 1.0);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 22);
    slider.setNumDecimalPlacesToDisplay (0);
    slider.setColour (juce::Slider::trackColourId, juce::Colour (0xff5b86b1));
    slider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff25374b));
    slider.setColour (juce::Slider::thumbColourId, juce::Colour (0xffd8ecff));
    slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffeaf4ff));
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff182433));
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff32485f));

    addAndMakeVisible (slider);
}

bool SamplePlayerAudioProcessorEditor::isSupportedSampleFilePath (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg";
}

bool SamplePlayerAudioProcessorEditor::isSupportedImageFilePath (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif";
}

void SamplePlayerAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (! wallpaperImage.isNull())
    {
        g.drawImageWithin (wallpaperImage,
                           0,
                           0,
                           getWidth(),
                           getHeight(),
                           juce::RectanglePlacement::stretchToFit,
                           false);

        g.fillAll (juce::Colour (0xaa0d1117));
    }
    else
    {
        juce::ColourGradient gradient (juce::Colour (0xff111827),
                                       0.0f,
                                       0.0f,
                                       juce::Colour (0xff22364c),
                                       0.0f,
                                       static_cast<float> (getHeight()),
                                       false);
        gradient.addColour (0.58, juce::Colour (0xff172334));
        gradient.addColour (1.0, juce::Colour (0xff0d141f));
        g.setGradientFill (gradient);
        g.fillRect (bounds);
    }

    auto padded = getLocalBounds().reduced (16);
    auto panelArea = padded.withTrimmedTop (96);
    auto leftPanel = panelArea.removeFromLeft (528).reduced (2);
    auto rightPanel = panelArea.reduced (2);

    g.setColour (juce::Colour (0x8f121a24));
    g.fillRoundedRectangle (leftPanel.toFloat(), static_cast<float> (panelCornerRadius));

    g.setColour (juce::Colour (0x9f111924));
    g.fillRoundedRectangle (rightPanel.toFloat(), static_cast<float> (panelCornerRadius));

    g.setColour (juce::Colour (0x44a9d5ff));
    g.drawRoundedRectangle (leftPanel.toFloat(), static_cast<float> (panelCornerRadius), 1.0f);
    g.drawRoundedRectangle (rightPanel.toFloat(), static_cast<float> (panelCornerRadius), 1.0f);

    if (dragOverlayActive)
    {
        g.setColour (juce::Colour (0xaa1a2f45));
        g.fillRoundedRectangle (padded.toFloat(), 18.0f);

        g.setColour (juce::Colour (0xff89c6ff));
        g.drawRoundedRectangle (padded.toFloat(), 18.0f, 2.0f);

        g.setFont (juce::Font (juce::FontOptions ("Avenir Next", 22.0f, juce::Font::bold)));
        g.drawFittedText ("Drop sample files/folders or an image wallpaper",
                          padded,
                          juce::Justification::centred,
                          2);
    }
}

void SamplePlayerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (16);

    auto topStrip = bounds.removeFromTop (92);
    auto titleArea = topStrip.removeFromLeft (460);

    titleLabel.setBounds (titleArea.removeFromTop (46));
    subtitleLabel.setBounds (titleArea);

    auto buttonArea = topStrip.reduced (0, 10);

    auto row1 = buttonArea.removeFromTop (30);
    loadFolderButton.setBounds (row1.removeFromLeft (124));
    row1.removeFromLeft (8);
    loadFilesButton.setBounds (row1.removeFromLeft (124));
    row1.removeFromLeft (8);
    clearSamplesButton.setBounds (row1.removeFromLeft (128));

    buttonArea.removeFromTop (8);

    auto row2 = buttonArea.removeFromTop (30);
    loadWallpaperButton.setBounds (row2.removeFromLeft (140));
    row2.removeFromLeft (8);
    clearWallpaperButton.setBounds (row2.removeFromLeft (140));

    auto contentArea = bounds;
    auto leftPanel = contentArea.removeFromLeft (528).reduced (12);
    auto rightPanel = contentArea.reduced (12);

    auto knobGrid = leftPanel.removeFromTop (472);

    auto layoutKnobCell = [] (juce::Rectangle<int> cell, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (cell.removeFromTop (20));
        label.setJustificationType (juce::Justification::centred);
        slider.setBounds (cell);
    };

    auto layoutKnobRow = [&layoutKnobCell] (juce::Rectangle<int> row,
                                             juce::Label& l1, juce::Slider& s1,
                                             juce::Label& l2, juce::Slider& s2,
                                             juce::Label& l3, juce::Slider& s3)
    {
        auto cell1 = row.removeFromLeft (row.getWidth() / 3).reduced (4);
        auto cell2 = row.removeFromLeft (row.getWidth() / 2).reduced (4);
        auto cell3 = row.reduced (4);

        layoutKnobCell (cell1, l1, s1);
        layoutKnobCell (cell2, l2, s2);
        layoutKnobCell (cell3, l3, s3);
    };

    auto rowHeight = knobGrid.getHeight() / 4;

    auto knobRow1 = knobGrid.removeFromTop (rowHeight);
    auto knobRow2 = knobGrid.removeFromTop (rowHeight);
    auto knobRow3 = knobGrid.removeFromTop (rowHeight);
    auto knobRow4 = knobGrid;

    layoutKnobRow (knobRow1, outputGainLabel, outputGainSlider, attackLabel, attackSlider, decayLabel, decaySlider);
    layoutKnobRow (knobRow2, sustainLabel, sustainSlider, releaseLabel, releaseSlider, loopStartLabel, loopStartSlider);
    layoutKnobRow (knobRow3, loopEndLabel, loopEndSlider, loopCrossfadeLabel, loopCrossfadeSlider, filterCutoffLabel, filterCutoffSlider);

    auto row4Cell1 = knobRow4.removeFromLeft (knobRow4.getWidth() / 3).reduced (4);
    auto row4Cell2 = knobRow4.removeFromLeft (knobRow4.getWidth() / 2).reduced (4);
    auto row4Cell3 = knobRow4.reduced (4);

    layoutKnobCell (row4Cell1, filterResonanceLabel, filterResonanceSlider);
    layoutKnobCell (row4Cell2, filterEnvAmountLabel, filterEnvAmountSlider);

    auto toggleArea = row4Cell3.reduced (10, 22);
    loopEnableButton.setBounds (toggleArea.removeFromTop (26));
    toggleArea.removeFromTop (8);
    filterEnableButton.setBounds (toggleArea.removeFromTop (26));

    leftPanel.removeFromTop (8);
    mappingHintLabel.setBounds (leftPanel);

    auto summaryArea = rightPanel.removeFromTop ((rightPanel.getHeight() * 48) / 100);
    summaryTitleLabel.setBounds (summaryArea.removeFromTop (24));
    summaryArea.removeFromTop (4);
    sampleSummaryEditor.setBounds (summaryArea);

    rightPanel.removeFromTop (10);

    zoneEditorTitleLabel.setBounds (rightPanel.removeFromTop (24));
    zoneFileLabel.setBounds (rightPanel.removeFromTop (22));
    rightPanel.removeFromTop (6);

    auto selectorRow = rightPanel.removeFromTop (30);
    zoneSelectorLabel.setBounds (selectorRow.removeFromLeft (58));
    reloadZoneButton.setBounds (selectorRow.removeFromRight (112));
    selectorRow.removeFromRight (8);
    zoneSelector.setBounds (selectorRow);

    rightPanel.removeFromTop (8);

    auto layoutZoneRow = [] (juce::Rectangle<int> row,
                             juce::Label& l1, juce::Slider& s1,
                             juce::Label& l2, juce::Slider& s2)
    {
        auto left = row.removeFromLeft (row.getWidth() / 2).reduced (0, 2);
        auto right = row.reduced (0, 2);

        l1.setBounds (left.removeFromLeft (84));
        s1.setBounds (left);

        l2.setBounds (right.removeFromLeft (96));
        s2.setBounds (right);
    };

    auto zoneRow1 = rightPanel.removeFromTop (34);
    auto zoneRow2 = rightPanel.removeFromTop (34);
    auto zoneRow3 = rightPanel.removeFromTop (34);

    layoutZoneRow (zoneRow1, zoneRootNoteLabel, zoneRootNoteSlider, zoneRoundRobinLabel, zoneRoundRobinSlider);
    layoutZoneRow (zoneRow2, zoneLowNoteLabel, zoneLowNoteSlider, zoneHighNoteLabel, zoneHighNoteSlider);
    layoutZoneRow (zoneRow3, zoneLowVelocityLabel, zoneLowVelocitySlider, zoneHighVelocityLabel, zoneHighVelocitySlider);

    rightPanel.removeFromTop (8);
    applyZoneButton.setBounds (rightPanel.removeFromTop (30).removeFromLeft (132));
}

bool SamplePlayerAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const auto file = juce::File (path);

        if (file.isDirectory())
            return true;

        if (isSupportedSampleFilePath (file) || isSupportedImageFilePath (file))
            return true;
    }

    return false;
}

void SamplePlayerAudioProcessorEditor::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused (x, y);

    if (isInterestedInFileDrag (files))
    {
        dragOverlayActive = true;
        repaint();
    }
}

void SamplePlayerAudioProcessorEditor::fileDragExit (const juce::StringArray& files)
{
    juce::ignoreUnused (files);

    dragOverlayActive = false;
    repaint();
}

void SamplePlayerAudioProcessorEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused (x, y);

    dragOverlayActive = false;
    repaint();

    juce::Array<juce::File> sampleFiles;
    juce::Array<juce::File> directories;
    juce::File wallpaperCandidate;

    for (const auto& path : files)
    {
        const auto file = juce::File (path);

        if (file.isDirectory())
        {
            directories.add (file);
            continue;
        }

        if (isSupportedSampleFilePath (file))
        {
            sampleFiles.add (file);
            continue;
        }

        if (isSupportedImageFilePath (file) && wallpaperCandidate == juce::File {})
            wallpaperCandidate = file;
    }

    for (const auto& directory : directories)
    {
        for (const auto& pattern : { "*.wav", "*.aif", "*.aiff", "*.flac", "*.ogg" })
            directory.findChildFiles (sampleFiles, juce::File::findFiles, true, pattern);
    }

    bool didSomething = false;
    juce::String error;

    if (! sampleFiles.isEmpty())
    {
        if (processorRef.loadSampleFiles (sampleFiles, error))
        {
            didSomething = true;
        }
        else
        {
            showErrorMessage (error);
        }
    }

    if (wallpaperCandidate != juce::File {})
    {
        if (processorRef.setWallpaperFile (wallpaperCandidate))
        {
            didSomething = true;
        }
        else if (sampleFiles.isEmpty())
        {
            showErrorMessage ("Could not load wallpaper image.");
        }
    }

    if (! didSomething)
        showErrorMessage ("Drop audio files/folders or a wallpaper image file.");

    refreshSampleSummary();
    refreshWallpaperImage();
    refreshZoneEditor();
}

void SamplePlayerAudioProcessorEditor::timerCallback()
{
    refreshSampleSummary();
    refreshWallpaperImage();
    refreshZoneEditor();
}

void SamplePlayerAudioProcessorEditor::openSampleFolderChooser()
{
    sampleFolderChooser = std::make_unique<juce::FileChooser> (
        "Select a folder containing sample files",
        juce::File {},
        "*");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    sampleFolderChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        const auto folder = chooser.getResult();
        sampleFolderChooser.reset();

        if (folder == juce::File {})
            return;

        juce::String error;
        if (! processorRef.loadSampleFolder (folder, error))
            showErrorMessage (error);

        refreshSampleSummary();
        refreshZoneEditor();
    });
}

void SamplePlayerAudioProcessorEditor::openSampleFileChooser()
{
    sampleFileChooser = std::make_unique<juce::FileChooser> (
        "Select sample files",
        juce::File {},
        "*.wav;*.aif;*.aiff;*.flac;*.ogg");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::canSelectMultipleItems;

    sampleFileChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        const auto files = chooser.getResults();
        sampleFileChooser.reset();

        if (files.isEmpty())
            return;

        juce::String error;
        if (! processorRef.loadSampleFiles (files, error))
            showErrorMessage (error);

        refreshSampleSummary();
        refreshZoneEditor();
    });
}

void SamplePlayerAudioProcessorEditor::openWallpaperChooser()
{
    wallpaperChooser = std::make_unique<juce::FileChooser> (
        "Select wallpaper image",
        juce::File {},
        "*.png;*.jpg;*.jpeg;*.bmp;*.gif");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    wallpaperChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();
        wallpaperChooser.reset();

        if (file == juce::File {})
            return;

        if (! processorRef.setWallpaperFile (file))
            showErrorMessage ("Could not load wallpaper image.");

        refreshWallpaperImage();
    });
}

void SamplePlayerAudioProcessorEditor::refreshSampleSummary()
{
    const auto summary = processorRef.getSampleSummaryText();

    if (summary != cachedSampleSummary)
    {
        cachedSampleSummary = summary;
        sampleSummaryEditor.setText (summary, false);
    }

    summaryTitleLabel.setText ("Loaded Zones (" + juce::String (processorRef.getLoadedZoneCount()) + ")",
                               juce::dontSendNotification);
}

void SamplePlayerAudioProcessorEditor::refreshWallpaperImage()
{
    const auto file = processorRef.getWallpaperFile();

    if (file.getFullPathName() == cachedWallpaperFile.getFullPathName())
        return;

    cachedWallpaperFile = file;
    wallpaperImage = juce::Image {};

    if (file.existsAsFile())
        wallpaperImage = juce::ImageFileFormat::loadFrom (file);

    repaint();
}

void SamplePlayerAudioProcessorEditor::refreshZoneEditor()
{
    const auto zoneNames = processorRef.getZoneDisplayNames();
    const auto signature = zoneNames.joinIntoString ("\n");

    if (signature == cachedZoneSignature)
        return;

    cachedZoneSignature = signature;

    const auto previousId = zoneSelector.getSelectedId();

    ignoreZoneEditorCallbacks = true;
    zoneSelector.clear();

    for (int i = 0; i < zoneNames.size(); ++i)
        zoneSelector.addItem (zoneNames[i], i + 1);

    if (zoneNames.isEmpty())
    {
        zoneSelector.setSelectedId (0, juce::dontSendNotification);
        clearZoneEditorFields();
        ignoreZoneEditorCallbacks = false;
        return;
    }

    const auto targetId = juce::jlimit (1, zoneNames.size(), previousId > 0 ? previousId : 1);
    zoneSelector.setSelectedId (targetId, juce::dontSendNotification);
    ignoreZoneEditorCallbacks = false;

    populateZoneEditorFields();
}

void SamplePlayerAudioProcessorEditor::populateZoneEditorFields()
{
    const int selectedId = zoneSelector.getSelectedId();

    if (selectedId <= 0)
    {
        clearZoneEditorFields();
        return;
    }

    SamplePlayerAudioProcessor::ZoneEditorInfo info;
    if (! processorRef.getZoneEditorInfo (selectedId - 1, info))
    {
        clearZoneEditorFields();
        return;
    }

    ignoreZoneEditorCallbacks = true;

    zoneRootNoteSlider.setEnabled (true);
    zoneLowNoteSlider.setEnabled (true);
    zoneHighNoteSlider.setEnabled (true);
    zoneLowVelocitySlider.setEnabled (true);
    zoneHighVelocitySlider.setEnabled (true);
    zoneRoundRobinSlider.setEnabled (true);
    applyZoneButton.setEnabled (true);
    reloadZoneButton.setEnabled (true);

    zoneRootNoteSlider.setValue (info.metadata.rootNote, juce::dontSendNotification);
    zoneLowNoteSlider.setValue (info.metadata.lowNote, juce::dontSendNotification);
    zoneHighNoteSlider.setValue (info.metadata.highNote, juce::dontSendNotification);
    zoneLowVelocitySlider.setValue (info.metadata.lowVelocity, juce::dontSendNotification);
    zoneHighVelocitySlider.setValue (info.metadata.highVelocity, juce::dontSendNotification);
    zoneRoundRobinSlider.setValue (info.metadata.roundRobinIndex, juce::dontSendNotification);

    zoneFileLabel.setText ("Editing: " + info.fileName, juce::dontSendNotification);

    ignoreZoneEditorCallbacks = false;
}

void SamplePlayerAudioProcessorEditor::clearZoneEditorFields()
{
    ignoreZoneEditorCallbacks = true;

    for (auto* slider : { &zoneRootNoteSlider, &zoneLowNoteSlider, &zoneHighNoteSlider,
                          &zoneLowVelocitySlider, &zoneHighVelocitySlider, &zoneRoundRobinSlider })
    {
        slider->setValue (0.0, juce::dontSendNotification);
        slider->setEnabled (false);
    }

    applyZoneButton.setEnabled (false);
    reloadZoneButton.setEnabled (false);

    zoneFileLabel.setText ("No zone selected", juce::dontSendNotification);

    ignoreZoneEditorCallbacks = false;
}

void SamplePlayerAudioProcessorEditor::applyZoneEdits()
{
    if (ignoreZoneEditorCallbacks)
        return;

    const int selectedId = zoneSelector.getSelectedId();

    if (selectedId <= 0)
        return;

    SamplePlayerAudioProcessor::ZoneMetadata metadata;
    metadata.rootNote = static_cast<int> (zoneRootNoteSlider.getValue());
    metadata.lowNote = static_cast<int> (zoneLowNoteSlider.getValue());
    metadata.highNote = static_cast<int> (zoneHighNoteSlider.getValue());
    metadata.lowVelocity = static_cast<int> (zoneLowVelocitySlider.getValue());
    metadata.highVelocity = static_cast<int> (zoneHighVelocitySlider.getValue());
    metadata.roundRobinIndex = static_cast<int> (zoneRoundRobinSlider.getValue());

    juce::String error;
    if (! processorRef.updateZoneMetadata (selectedId - 1, metadata, error))
    {
        showErrorMessage (error);
        return;
    }

    refreshSampleSummary();
    refreshZoneEditor();
    populateZoneEditorFields();
}

void SamplePlayerAudioProcessorEditor::showErrorMessage (const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                            "Sample Player",
                                            message);
}
