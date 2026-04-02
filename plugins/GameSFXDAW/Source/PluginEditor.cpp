#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

GameSFXDAWAudioProcessorEditor::GameSFXDAWAudioProcessorEditor (GameSFXDAWAudioProcessor& p)
    : AudioProcessorEditor (&p)
{
    masterPreviewGainAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *p.parameters.getParameter ("masterPreviewGainDb"),
        masterPreviewGainRelay
    );

    selectedContainerIndexAttachment = std::make_unique<juce::WebSliderParameterAttachment> (
        *p.parameters.getParameter ("selectedContainerIndex"),
        selectedContainerIndexRelay
    );

    webView.reset (new SinglePageBrowser (createWebOptions (*this)));
    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setSize (1280, 820);

    juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<GameSFXDAWAudioProcessorEditor> (this)]
    {
        if (safeThis == nullptr)
            return;

        safeThis->applyStandaloneWindowStyle();
    });
}

GameSFXDAWAudioProcessorEditor::~GameSFXDAWAudioProcessorEditor() = default;

void GameSFXDAWAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void GameSFXDAWAudioProcessorEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

void GameSFXDAWAudioProcessorEditor::applyStandaloneWindowStyle()
{
    auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());

    if (window == nullptr)
        return;

#if JUCE_MAC
    window->setUsingNativeTitleBar (true);
#endif

    window->setResizable (true, false);

    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto bounds = display->userArea;
        window->setBoundsConstrained ({ bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() });
    }
}

juce::WebBrowserComponent::Options GameSFXDAWAudioProcessorEditor::createWebOptions (GameSFXDAWAudioProcessorEditor& editor)
{
    auto options = juce::WebBrowserComponent::Options{};

#if JUCE_WINDOWS
    options = options.withBackend (juce::WebBrowserComponent::Options::Backend::webview2);
    options = options.withWinWebView2Options (
        juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("NPS_GameSFXDAW"))
    );
#endif

    options = options.withNativeIntegrationEnabled()
                     .withKeepPageLoadedWhenBrowserIsHidden()
                     .withOptionsFrom (editor.masterPreviewGainRelay)
                     .withOptionsFrom (editor.selectedContainerIndexRelay)
                     .withResourceProvider ([&editor] (const juce::String& url)
                     {
                         return editor.getResource (url);
                     });

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> GameSFXDAWAudioProcessorEditor::getResource (const juce::String& url)
{
    auto makeResource = [] (const char* data, int size, const char* mime)
    {
        return juce::WebBrowserComponent::Resource{
            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                    reinterpret_cast<const std::byte*> (data) + size),
            juce::String (mime)
        };
    };

    if (url.isEmpty() || url == "/" || url == "/index.html")
    {
        return makeResource (gamesfxdaw_BinaryData::index_html,
                             gamesfxdaw_BinaryData::index_htmlSize,
                             "text/html");
    }

    return std::nullopt;
}
