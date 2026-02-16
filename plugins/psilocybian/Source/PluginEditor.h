#pragma once

#include "PluginProcessor.h"
#include <cstdint>
#include <limits>
#include <juce_gui_extra/juce_gui_extra.h>

class SpecraumAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit SpecraumAudioProcessorEditor (SpecraumAudioProcessor&);
    ~SpecraumAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    SpecraumAudioProcessor& processorRef;
    std::unique_ptr<juce::WebBrowserComponent> webView;
    std::unique_ptr<juce::FileChooser> folderChooser;
    std::uint32_t lastReferenceRevision = std::numeric_limits<std::uint32_t>::max();
    bool fullscreen = false;
    juce::Component::SafePointer<juce::Component> fullscreenTarget;
    juce::Rectangle<int> windowedBounds;
#if JUCE_WINDOWS
    void* fullscreenNativeWindow = nullptr;
    std::int64_t savedWindowStyle = 0;
    std::int64_t savedWindowExStyle = 0;
    juce::Rectangle<int> savedWindowBounds;
#endif

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    static juce::WebBrowserComponent::Options createWebOptions (SpecraumAudioProcessorEditor& editor);
    void toggleFullscreen();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpecraumAudioProcessorEditor)
};
