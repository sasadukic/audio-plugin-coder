#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

class SamplePlayerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit SamplePlayerAudioProcessorEditor (SamplePlayerAudioProcessor&);
    ~SamplePlayerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int defaultEditorWidth = 1040;
    static constexpr int defaultPlayerHeight = 760;
    static constexpr int minEditorWidth = 720;
    static constexpr int maxEditorWidth = 2200;
    static constexpr int minEditorHeight = 240;
    static constexpr int maxEditorHeight = 2600;

    struct SinglePageBrowser : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;

        bool pageAboutToLoad (const juce::String& newURL) override
        {
            return newURL == getResourceProviderRoot();
        }
    };

    std::unique_ptr<SinglePageBrowser> webView;
    SamplePlayerAudioProcessor& audioProcessor;
    juce::String lastAutoSamplerStatus;
    int lastAutoSamplerExpected = -1;
    int lastAutoSamplerCaptured = -1;
    bool lastAutoSamplerActive = false;
    bool lastAutoSamplerInputDetected = false;
    int lastEditorWidth = defaultEditorWidth;
    int lastEditorHeight = defaultPlayerHeight;
    juce::String lastPushedLightweightSessionJson;
    juce::uint64 lastPushedHeldMidiMaskLo = 0;
    juce::uint64 lastPushedHeldMidiMaskHi = 0;
    int lastPushedActiveMapSetSlot = -1;
    int lastPushedSequencerStep = -2;
    std::unique_ptr<juce::FileChooser> destinationFolderChooser;
    std::unique_ptr<juce::FileChooser> saveInstrumentChooser;
    std::unique_ptr<juce::FileChooser> loadInstrumentChooser;

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    static juce::WebBrowserComponent::Options createWebOptions (SamplePlayerAudioProcessorEditor& editor);
    static juce::var makeAutoSamplerStatusVar (const SamplePlayerAudioProcessor::AutoSamplerProgress& progress);
    static juce::String buildAudioWavDataUrl (const SamplePlayerAudioProcessor::AutoSamplerCompletedTake& take);
    static juce::String buildFileDataUrl (const juce::File& file);

    void timerCallback() override;
    void handleAutoSamplerControlEvent (const juce::var& eventPayload);
    void handleDestinationFolderPickEvent (const juce::var& eventPayload);
    void handlePickInstrumentManifestEvent (const juce::var& eventPayload);
    void handleUIResizeEvent (const juce::var& eventPayload);
    void handleSessionStateSetEvent (const juce::var& eventPayload);
    void handleActiveMapSetEvent (const juce::var& eventPayload);
    void handleSequencerHostTriggerSetEvent (const juce::var& eventPayload);
    void handleSessionStateGetEvent (const juce::var& eventPayload);
    void handleSampleDataGetEvent (const juce::var& eventPayload);
    void handleGraphicDataGetEvent (const juce::var& eventPayload);
    void handlePreviewMidiEvent (const juce::var& eventPayload);
    void handleSaveInstrumentBundleEvent (const juce::var& eventPayload);
    void handleDebugLogEvent (const juce::var& eventPayload);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePlayerAudioProcessorEditor)
};
