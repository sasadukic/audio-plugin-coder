#pragma once

#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

//==============================================================================
class NfGnarlyAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit NfGnarlyAudioProcessorEditor (NfGnarlyAudioProcessor&);
    ~NfGnarlyAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    NfGnarlyAudioProcessor& processorRef;

    // ═══════════════════════════════════════════════════════════════
    // CRITICAL: Member Declaration Order (prevents DAW crashes)
    // Destruction happens in REVERSE order of declaration
    // Order: Relays → WebView → Attachments
    // ═══════════════════════════════════════════════════════════════

    // 1. RELAYS FIRST (destroyed last - no dependencies)
    std::unique_ptr<juce::WebSliderRelay> driveRelay;
    std::unique_ptr<juce::WebSliderRelay> cutoffRelay;
    std::unique_ptr<juce::WebSliderRelay> resonanceRelay;

    // 2. WEBVIEW SECOND (destroyed middle - depends on relays via withOptionsFrom)
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // 3. ATTACHMENTS LAST (destroyed first - depend on both relays and parameters)
    std::unique_ptr<juce::WebSliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> cutoffAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> resonanceAttachment;

    //==============================================================================
    // Resource Provider
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    static juce::WebBrowserComponent::Options createWebOptions (NfGnarlyAudioProcessorEditor& editor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NfGnarlyAudioProcessorEditor)
};
