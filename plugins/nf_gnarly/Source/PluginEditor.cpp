#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
NfGnarlyAudioProcessorEditor::NfGnarlyAudioProcessorEditor (NfGnarlyAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    DBG ("NfGnarly Editor Constructor: START");

    // CRITICAL: Initialize in correct order (same as declaration)

    // 1. Create relays FIRST
    DBG ("NfGnarly Editor: Creating relays...");
    driveRelay = std::make_unique<juce::WebSliderRelay> ("drive");
    cutoffRelay = std::make_unique<juce::WebSliderRelay> ("cutoff");
    resonanceRelay = std::make_unique<juce::WebSliderRelay> ("resonance");
    DBG ("NfGnarly Editor: Relays created");

    // 2. Create WebView SECOND with relay references
    DBG ("NfGnarly Editor: Creating WebView...");
    webView.reset (new juce::WebBrowserComponent (createWebOptions (*this)));
    DBG ("NfGnarly Editor: WebView created");

    // 3. Create attachments BEFORE addAndMakeVisible (CRITICAL!)
    DBG ("NfGnarly Editor: Creating attachments...");
    driveAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("drive"),
        *driveRelay,
        nullptr
    );

    cutoffAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("cutoff"),
        *cutoffRelay,
        nullptr
    );

    resonanceAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *processorRef.parameters.getParameter ("resonance"),
        *resonanceRelay,
        nullptr
    );
    DBG ("NfGnarly Editor: Attachments created");

    // 4. THEN make visible
    addAndMakeVisible (*webView);
    DBG ("NfGnarly Editor: WebView made visible");

    // 5. Load UI from resource provider
    DBG ("NfGnarly Editor: Loading HTML from resource provider...");
    auto startUrl = juce::WebBrowserComponent::getResourceProviderRoot();
    if (! startUrl.endsWithChar ('/'))
        startUrl << '/';
    startUrl << "index.html";
    webView->goToURL (startUrl);
    DBG ("NfGnarly Editor Constructor: COMPLETE");

    setSize (400, 380);
}

NfGnarlyAudioProcessorEditor::~NfGnarlyAudioProcessorEditor()
{
    // Members destroyed in reverse order automatically
}

//==============================================================================
void NfGnarlyAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void NfGnarlyAudioProcessorEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

//==============================================================================
juce::WebBrowserComponent::Options NfGnarlyAudioProcessorEditor::createWebOptions (NfGnarlyAudioProcessorEditor& editor)
{
    DBG ("NfGnarly: createWebOptions START");

    auto options = juce::WebBrowserComponent::Options{}
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2);

    DBG ("NfGnarly: Backend set to webview2");

    options = options.withWinWebView2Options (
        juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("NPS_NfGnarly"))
    );

    DBG ("NfGnarly: WebView2 options set");

    options = options.withNativeIntegrationEnabled()
                    .withKeepPageLoadedWhenBrowserIsHidden()
                    .withResourceProvider ([&editor] (const juce::String& url) {
                        return editor.getResource (url);
                    });

    DBG ("NfGnarly: Native integration and resource provider set");

    options = options.withOptionsFrom (*editor.driveRelay)
                    .withOptionsFrom (*editor.cutoffRelay)
                    .withOptionsFrom (*editor.resonanceRelay);

    DBG ("NfGnarly: createWebOptions COMPLETE");
    return options;
}

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> NfGnarlyAudioProcessorEditor::getResource (const juce::String& url)
{
    DBG ("NfGnarly Resource Request: " + url);

    auto makeResource = [] (const char* data, int size, const char* mime)
    {
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                   reinterpret_cast<const std::byte*> (data) + size),
            juce::String (mime)
        };
    };

    auto path = url.trim();

    // JUCE can pass either a relative path ("/index.html") or a full backend URL
    // ("https://juce.backend/index.html" / "juce://juce.backend/index.html").
    const auto schemePos = path.indexOf ("://");
    if (schemePos >= 0)
    {
        const auto hostStart = schemePos + 3;
        const auto firstSlash = path.indexOfChar (hostStart, '/');
        path = (firstSlash >= 0) ? path.substring (firstSlash) : "/";
    }

    if (path.startsWithIgnoreCase ("juce.backend/"))
        path = path.fromFirstOccurrenceOf ("/", false, false);

    if (const auto queryPos = path.indexOfChar ('?'); queryPos >= 0)
        path = path.substring (0, queryPos);
    if (const auto fragmentPos = path.indexOfChar ('#'); fragmentPos >= 0)
        path = path.substring (0, fragmentPos);

    path = path.replaceCharacter ('\\', '/');
    if (path.startsWithChar ('/'))
        path = path.substring (1);
    if (path.isEmpty())
        path = "index.html";

    const char* data = nullptr;
    int dataSize = 0;
    const char* mime = "application/octet-stream";

    // JUCE BinaryData names are mangled to symbol names (index_html/index_js/index_js2),
    // not full file paths, so map URL paths explicitly.
    if (path == "index.html")
    {
        data = nf_gnarly_BinaryData::index_html;
        dataSize = nf_gnarly_BinaryData::index_htmlSize;
        mime = "text/html";
    }
    else if (path == "js/index.js")
    {
        data = nf_gnarly_BinaryData::index_js;
        dataSize = nf_gnarly_BinaryData::index_jsSize;
        mime = "application/javascript";
    }
    else if (path == "js/juce/index.js")
    {
        data = nf_gnarly_BinaryData::index_js2;
        dataSize = nf_gnarly_BinaryData::index_js2Size;
        mime = "application/javascript";
    }
    else if (path == "favicon.ico")
    {
        // Avoid backend DNS fallback on favicon lookup.
        return std::nullopt;
    }

    if (data != nullptr && dataSize > 0)
    {
        DBG ("NfGnarly: Serving resource " + path);
        return makeResource (data, dataSize, mime);
    }

    DBG ("NfGnarly: Resource not found: " + url);
    return std::nullopt;
}

