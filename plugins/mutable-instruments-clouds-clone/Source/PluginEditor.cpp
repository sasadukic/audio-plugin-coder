#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

MutableInstrumentsCloudsCloneAudioProcessorEditor::MutableInstrumentsCloudsCloneAudioProcessorEditor (MutableInstrumentsCloudsCloneAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    positionRelay = std::make_unique<juce::WebSliderRelay> ("position");
    sizeRelay = std::make_unique<juce::WebSliderRelay> ("size");
    pitchRelay = std::make_unique<juce::WebSliderRelay> ("pitch");
    densityRelay = std::make_unique<juce::WebSliderRelay> ("density");
    textureRelay = std::make_unique<juce::WebSliderRelay> ("texture");
    blendRelay = std::make_unique<juce::WebSliderRelay> ("blend");
    spreadRelay = std::make_unique<juce::WebSliderRelay> ("spread");
    feedbackRelay = std::make_unique<juce::WebSliderRelay> ("feedback");
    reverbRelay = std::make_unique<juce::WebSliderRelay> ("reverb");
    freezeRelay = std::make_unique<juce::WebSliderRelay> ("freeze");

    webView = std::make_unique<juce::WebBrowserComponent> (createWebOptions (*this));

    positionAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("position"), *positionRelay, nullptr);
    sizeAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("size"), *sizeRelay, nullptr);
    pitchAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("pitch"), *pitchRelay, nullptr);
    densityAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("density"), *densityRelay, nullptr);
    textureAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("texture"), *textureRelay, nullptr);
    blendAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("blend"), *blendRelay, nullptr);
    spreadAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("spread"), *spreadRelay, nullptr);
    feedbackAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("feedback"), *feedbackRelay, nullptr);
    reverbAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("reverb"), *reverbRelay, nullptr);
    freezeAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("freeze"), *freezeRelay, nullptr);

    addAndMakeVisible (*webView);

    auto startUrl = juce::WebBrowserComponent::getResourceProviderRoot();
    if (! startUrl.endsWithChar ('/'))
        startUrl << '/';
    startUrl << "index.html";
    webView->goToURL (startUrl);

    setSize (820, 520);
    startTimerHz (30);
}

MutableInstrumentsCloudsCloneAudioProcessorEditor::~MutableInstrumentsCloudsCloneAudioProcessorEditor()
{
    stopTimer();
}

void MutableInstrumentsCloudsCloneAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
}

void MutableInstrumentsCloudsCloneAudioProcessorEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

void MutableInstrumentsCloudsCloneAudioProcessorEditor::timerCallback()
{
    if (webView == nullptr)
        return;

    const auto inLevel = processorRef.inputMeter.load();
    const auto grainLevel = processorRef.grainMeter.load();
    const auto scope = processorRef.getIncomingScopeSnapshot();
    juce::String arr = "[";
    for (int i = 0; i < static_cast<int> (scope.size()); ++i)
    {
        if (i > 0) arr << ",";
        arr << juce::String (scope[static_cast<size_t> (i)], 5);
    }
    arr << "]";

    webView->evaluateJavascript ("if (window.updateIncomingAudio) window.updateIncomingAudio(" +
                                 juce::String (inLevel, 4) + "," +
                                 juce::String (grainLevel, 4) + "," + arr + ");");
}

juce::WebBrowserComponent::Options MutableInstrumentsCloudsCloneAudioProcessorEditor::createWebOptions (MutableInstrumentsCloudsCloneAudioProcessorEditor& editor)
{
    auto options = juce::WebBrowserComponent::Options{}
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("NPS_CloudsClone")))
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([&editor] (const juce::String& url) { return editor.getResource (url); })
        .withOptionsFrom (*editor.positionRelay)
        .withOptionsFrom (*editor.sizeRelay)
        .withOptionsFrom (*editor.pitchRelay)
        .withOptionsFrom (*editor.densityRelay)
        .withOptionsFrom (*editor.textureRelay)
        .withOptionsFrom (*editor.blendRelay)
        .withOptionsFrom (*editor.spreadRelay)
        .withOptionsFrom (*editor.feedbackRelay)
        .withOptionsFrom (*editor.reverbRelay)
        .withOptionsFrom (*editor.freezeRelay);

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> MutableInstrumentsCloudsCloneAudioProcessorEditor::getResource (const juce::String& url)
{
    auto makeResource = [] (const char* data, int size, const char* mime)
    {
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                    reinterpret_cast<const std::byte*> (data) + size),
            juce::String (mime)
        };
    };

    auto path = url.trim();
    const auto root = juce::WebBrowserComponent::getResourceProviderRoot();
    if (path.startsWithIgnoreCase (root))
        path = path.fromFirstOccurrenceOf (root, false, false);

    if (path.startsWithChar ('/'))
        path = path.substring (1);
    if (path.isEmpty())
        path = "index.html";

    if (path == "index.html")
        return makeResource (mutable_instruments_clouds_clone_BinaryData::index_html,
                             mutable_instruments_clouds_clone_BinaryData::index_htmlSize,
                             "text/html");

    if (path == "js/index.js")
        return makeResource (mutable_instruments_clouds_clone_BinaryData::index_js,
                             mutable_instruments_clouds_clone_BinaryData::index_jsSize,
                             "application/javascript");

    if (path == "js/juce/index.js")
        return makeResource (mutable_instruments_clouds_clone_BinaryData::index_js2,
                             mutable_instruments_clouds_clone_BinaryData::index_js2Size,
                             "application/javascript");

    return std::nullopt;
}
