#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

class SamplePlayerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                         public juce::FileDragAndDropTarget,
                                         private juce::Timer
{
public:
    explicit SamplePlayerAudioProcessorEditor (SamplePlayerAudioProcessor&);
    ~SamplePlayerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;

    void configureRotarySlider (juce::Slider& slider, const juce::String& suffix);
    void configureZoneEditSlider (juce::Slider& slider, double min, double max);

    static bool isSupportedSampleFilePath (const juce::File& file);
    static bool isSupportedImageFilePath (const juce::File& file);

    void openSampleFolderChooser();
    void openSampleFileChooser();
    void openWallpaperChooser();

    void refreshSampleSummary();
    void refreshWallpaperImage();
    void refreshZoneEditor();
    void populateZoneEditorFields();
    void clearZoneEditorFields();
    void applyZoneEdits();

    void showErrorMessage (const juce::String& message);

    SamplePlayerAudioProcessor& processorRef;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label mappingHintLabel;

    juce::TextButton loadFolderButton { "Load Folder" };
    juce::TextButton loadFilesButton { "Load Files" };
    juce::TextButton clearSamplesButton { "Clear Samples" };

    juce::TextButton loadWallpaperButton { "Load Wallpaper" };
    juce::TextButton clearWallpaperButton { "Clear Wallpaper" };

    juce::ToggleButton loopEnableButton { "Loop" };
    juce::ToggleButton filterEnableButton { "Filter" };

    juce::Slider outputGainSlider;
    juce::Slider attackSlider;
    juce::Slider decaySlider;
    juce::Slider sustainSlider;
    juce::Slider releaseSlider;
    juce::Slider loopStartSlider;
    juce::Slider loopEndSlider;
    juce::Slider loopCrossfadeSlider;
    juce::Slider filterCutoffSlider;
    juce::Slider filterResonanceSlider;
    juce::Slider filterEnvAmountSlider;

    juce::Label outputGainLabel { {}, "Output" };
    juce::Label attackLabel { {}, "Attack" };
    juce::Label decayLabel { {}, "Decay" };
    juce::Label sustainLabel { {}, "Sustain" };
    juce::Label releaseLabel { {}, "Release" };
    juce::Label loopStartLabel { {}, "Loop Start" };
    juce::Label loopEndLabel { {}, "Loop End" };
    juce::Label loopCrossfadeLabel { {}, "Xfade" };
    juce::Label filterCutoffLabel { {}, "Cutoff" };
    juce::Label filterResonanceLabel { {}, "Resonance" };
    juce::Label filterEnvAmountLabel { {}, "Filter Env" };

    juce::Label summaryTitleLabel { {}, "Loaded Zones" };
    juce::TextEditor sampleSummaryEditor;

    juce::Label zoneEditorTitleLabel { {}, "Zone Editor" };
    juce::Label zoneFileLabel { {}, "No zone selected" };
    juce::Label zoneSelectorLabel { {}, "Zone" };
    juce::ComboBox zoneSelector;
    juce::TextButton applyZoneButton { "Apply Zone" };
    juce::TextButton reloadZoneButton { "Reload Zone" };

    juce::Slider zoneRootNoteSlider;
    juce::Slider zoneLowNoteSlider;
    juce::Slider zoneHighNoteSlider;
    juce::Slider zoneLowVelocitySlider;
    juce::Slider zoneHighVelocitySlider;
    juce::Slider zoneRoundRobinSlider;

    juce::Label zoneRootNoteLabel { {}, "Root" };
    juce::Label zoneLowNoteLabel { {}, "Low Key" };
    juce::Label zoneHighNoteLabel { {}, "High Key" };
    juce::Label zoneLowVelocityLabel { {}, "Low Vel" };
    juce::Label zoneHighVelocityLabel { {}, "High Vel" };
    juce::Label zoneRoundRobinLabel { {}, "Round Robin" };

    std::unique_ptr<SliderAttachment> outputGainAttachment;
    std::unique_ptr<SliderAttachment> attackAttachment;
    std::unique_ptr<SliderAttachment> decayAttachment;
    std::unique_ptr<SliderAttachment> sustainAttachment;
    std::unique_ptr<SliderAttachment> releaseAttachment;
    std::unique_ptr<SliderAttachment> loopStartAttachment;
    std::unique_ptr<SliderAttachment> loopEndAttachment;
    std::unique_ptr<SliderAttachment> loopCrossfadeAttachment;
    std::unique_ptr<SliderAttachment> filterCutoffAttachment;
    std::unique_ptr<SliderAttachment> filterResonanceAttachment;
    std::unique_ptr<SliderAttachment> filterEnvAmountAttachment;
    std::unique_ptr<ButtonAttachment> loopEnableAttachment;
    std::unique_ptr<ButtonAttachment> filterEnableAttachment;

    std::unique_ptr<juce::FileChooser> sampleFolderChooser;
    std::unique_ptr<juce::FileChooser> sampleFileChooser;
    std::unique_ptr<juce::FileChooser> wallpaperChooser;

    juce::Image wallpaperImage;
    juce::String cachedSampleSummary;
    juce::String cachedZoneSignature;
    juce::File cachedWallpaperFile;

    bool dragOverlayActive = false;
    bool ignoreZoneEditorCallbacks = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePlayerAudioProcessorEditor)
};
