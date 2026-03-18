#pragma once

#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class GameSFXDAWAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit GameSFXDAWAudioProcessorEditor (GameSFXDAWAudioProcessor&);
    ~GameSFXDAWAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyStandaloneWindowStyle();

    juce::WebSliderRelay masterPreviewGainRelay { "masterPreviewGainDb" };
    juce::WebSliderRelay selectedContainerIndexRelay { "selectedContainerIndex" };

    struct SinglePageBrowser : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& newURL) override
        {
            return newURL == getResourceProviderRoot();
        }
    };

    std::unique_ptr<SinglePageBrowser> webView;
    std::unique_ptr<juce::WebSliderParameterAttachment> masterPreviewGainAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> selectedContainerIndexAttachment;

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    static juce::WebBrowserComponent::Options createWebOptions (GameSFXDAWAudioProcessorEditor& editor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GameSFXDAWAudioProcessorEditor)
};
