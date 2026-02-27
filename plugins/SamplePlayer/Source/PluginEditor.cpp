#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

SamplePlayerAudioProcessorEditor::SamplePlayerAudioProcessorEditor (SamplePlayerAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    webView.reset (new SinglePageBrowser (createWebOptions (*this)));
    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setSize (defaultEditorWidth, defaultPlayerHeight);
    startTimerHz (20);
}

SamplePlayerAudioProcessorEditor::~SamplePlayerAudioProcessorEditor()
{
    stopTimer();
}

void SamplePlayerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void SamplePlayerAudioProcessorEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

juce::WebBrowserComponent::Options SamplePlayerAudioProcessorEditor::createWebOptions (SamplePlayerAudioProcessorEditor& editor)
{
    auto options = juce::WebBrowserComponent::Options{};

#if JUCE_WINDOWS
    options = options.withBackend (juce::WebBrowserComponent::Options::Backend::webview2);
    options = options.withWinWebView2Options (
        juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("NPS_SamplePlayer"))
    );
#endif

    options = options.withNativeIntegrationEnabled()
                     .withKeepPageLoadedWhenBrowserIsHidden()
                     .withEventListener ("autosampler_control", [&editor] (const juce::var& payload)
                     {
                         editor.handleAutoSamplerControlEvent (payload);
                     })
                     .withEventListener ("ui_resize", [&editor] (const juce::var& payload)
                     {
                         editor.handleUIResizeEvent (payload);
                     })
                     .withResourceProvider ([&editor] (const juce::String& url)
                     {
                         return editor.getResource (url);
                     });

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> SamplePlayerAudioProcessorEditor::getResource (const juce::String& url)
{
    auto makeResource = [] (const char* data, int size, const char* mime)
    {
        return juce::WebBrowserComponent::Resource{
            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                    reinterpret_cast<const std::byte*> (data) + size),
            juce::String (mime)
        };
    };

    auto resourcePath = url.fromFirstOccurrenceOf (
        juce::WebBrowserComponent::getResourceProviderRoot(), false, false);

    if (resourcePath.isEmpty() || resourcePath == "/")
        resourcePath = "/index.html";

    auto path = resourcePath.startsWithChar ('/') ? resourcePath.substring (1) : resourcePath;

    if (path == "index.html")
    {
        return makeResource (sampleplayer_BinaryData::index_html,
                             sampleplayer_BinaryData::index_htmlSize,
                             "text/html");
    }

    if (path == "assets/handyman.svg")
    {
        return makeResource (sampleplayer_BinaryData::handyman_svg,
                             sampleplayer_BinaryData::handyman_svgSize,
                             "image/svg+xml");
    }

    return std::nullopt;
}

juce::var SamplePlayerAudioProcessorEditor::makeAutoSamplerStatusVar (const SamplePlayerAudioProcessor::AutoSamplerProgress& progress)
{
    auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
    object->setProperty ("active", progress.active);
    object->setProperty ("expected", progress.expectedTakes);
    object->setProperty ("captured", progress.capturedTakes);
    object->setProperty ("inputDetected", progress.inputDetected);
    object->setProperty ("message", progress.statusMessage);
    return juce::var (object.get());
}

juce::String SamplePlayerAudioProcessorEditor::buildAudioWavDataUrl (const SamplePlayerAudioProcessor::AutoSamplerCompletedTake& take)
{
    const int numChannels = juce::jlimit (1, 2, take.audio.getNumChannels());
    const int numSamples = take.audio.getNumSamples();
    const double sampleRate = take.sampleRate > 0.0 ? take.sampleRate : 44100.0;

    if (numSamples <= 0)
        return {};

    constexpr int bitsPerSample = 16;
    const int bytesPerSample = bitsPerSample / 8;
    const int blockAlign = numChannels * bytesPerSample;
    const int byteRate = static_cast<int> (std::round (sampleRate * static_cast<double> (blockAlign)));
    const int dataBytes = numSamples * blockAlign;
    const int riffSize = 36 + dataBytes;

    juce::MemoryOutputStream out;

    auto writeU16 = [&out] (juce::uint16 value)
    {
        out.writeByte (static_cast<char> (value & 0xff));
        out.writeByte (static_cast<char> ((value >> 8) & 0xff));
    };

    auto writeU32 = [&out] (juce::uint32 value)
    {
        out.writeByte (static_cast<char> (value & 0xff));
        out.writeByte (static_cast<char> ((value >> 8) & 0xff));
        out.writeByte (static_cast<char> ((value >> 16) & 0xff));
        out.writeByte (static_cast<char> ((value >> 24) & 0xff));
    };

    out.write ("RIFF", 4);
    writeU32 (static_cast<juce::uint32> (riffSize));
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    writeU32 (16);
    writeU16 (1); // PCM
    writeU16 (static_cast<juce::uint16> (numChannels));
    writeU32 (static_cast<juce::uint32> (std::max (1, static_cast<int> (std::round (sampleRate)))));
    writeU32 (static_cast<juce::uint32> (juce::jmax (blockAlign, byteRate)));
    writeU16 (static_cast<juce::uint16> (blockAlign));
    writeU16 (bitsPerSample);
    out.write ("data", 4);
    writeU32 (static_cast<juce::uint32> (dataBytes));

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const int sourceChannel = juce::jmin (ch, take.audio.getNumChannels() - 1);
            const float sample = juce::jlimit (-1.0f, 1.0f, take.audio.getSample (sourceChannel, i));
            const int scaled = static_cast<int> (std::round (sample * 32767.0f));
            const auto pcm = static_cast<juce::int16> (juce::jlimit (-32768, 32767, scaled));
            writeU16 (static_cast<juce::uint16> (static_cast<juce::uint16> (pcm)));
        }
    }

    const auto base64 = juce::Base64::toBase64 (out.getData(), out.getDataSize());
    return "data:audio/wav;base64," + base64;
}

void SamplePlayerAudioProcessorEditor::timerCallback()
{
    if (! webView)
        return;

    const auto emitTakeEvent = [this] (const juce::String& fileName,
                                       int rootMidi,
                                       int velocity127,
                                       int velocityLayer,
                                       int velocityLow,
                                       int velocityHigh,
                                       int rrIndex,
                                       bool loopSamples,
                                       bool autoLoopMode,
                                       float loopStartPercent,
                                       float loopEndPercent,
                                       bool cutLoopAtEnd,
                                       float loopCrossfadeMs,
                                       bool normalized,
                                       const juce::String& dataUrl,
                                       bool hasAudio)
    {
        auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
        object->setProperty ("fileName", fileName);
        object->setProperty ("rootMidi", rootMidi);
        object->setProperty ("velocity", velocity127);
        object->setProperty ("velocityLayer", velocityLayer);
        object->setProperty ("velocityLow", velocityLow);
        object->setProperty ("velocityHigh", velocityHigh);
        object->setProperty ("rrIndex", rrIndex);
        object->setProperty ("loopSamples", loopSamples);
        object->setProperty ("autoLoopMode", autoLoopMode);
        object->setProperty ("loopStartPercent", loopStartPercent);
        object->setProperty ("loopEndPercent", loopEndPercent);
        object->setProperty ("cutLoopAtEnd", cutLoopAtEnd);
        object->setProperty ("loopCrossfadeMs", loopCrossfadeMs);
        object->setProperty ("normalized", normalized);
        object->setProperty ("hasAudio", hasAudio);
        object->setProperty ("dataUrl", dataUrl);
        webView->emitEventIfBrowserIsVisible ("autosampler_take", juce::var (object.get()));
    };

    auto triggeredTakes = audioProcessor.popTriggeredAutoSamplerTakes();
    for (const auto& take : triggeredTakes)
    {
        emitTakeEvent (take.fileName,
                       take.rootMidi,
                       take.velocity127,
                       take.velocityLayer,
                       take.velocityLow,
                       take.velocityHigh,
                       take.rrIndex,
                       take.loopSamples,
                       take.autoLoopMode,
                       take.loopStartPercent,
                       take.loopEndPercent,
                       take.cutLoopAtEnd,
                       take.loopCrossfadeMs,
                       take.normalized,
                       {},
                       false);
    }

    auto takes = audioProcessor.popCompletedAutoSamplerTakes();
    for (auto& take : takes)
    {
        const auto dataUrl = buildAudioWavDataUrl (take);
        emitTakeEvent (take.fileName,
                       take.rootMidi,
                       take.velocity127,
                       take.velocityLayer,
                       take.velocityLow,
                       take.velocityHigh,
                       take.rrIndex,
                       take.loopSamples,
                       take.autoLoopMode,
                       take.loopStartPercent,
                       take.loopEndPercent,
                       take.cutLoopAtEnd,
                       take.loopCrossfadeMs,
                       take.normalized,
                       dataUrl,
                       dataUrl.isNotEmpty());
    }

    const auto progress = audioProcessor.getAutoSamplerProgress();
    const bool changed = progress.active != lastAutoSamplerActive
                      || progress.expectedTakes != lastAutoSamplerExpected
                      || progress.capturedTakes != lastAutoSamplerCaptured
                      || progress.inputDetected != lastAutoSamplerInputDetected
                      || progress.statusMessage != lastAutoSamplerStatus;

    if (! changed)
        return;

    lastAutoSamplerActive = progress.active;
    lastAutoSamplerExpected = progress.expectedTakes;
    lastAutoSamplerCaptured = progress.capturedTakes;
    lastAutoSamplerInputDetected = progress.inputDetected;
    lastAutoSamplerStatus = progress.statusMessage;
    webView->emitEventIfBrowserIsVisible ("autosampler_status", makeAutoSamplerStatusVar (progress));
}

void SamplePlayerAudioProcessorEditor::handleAutoSamplerControlEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto action = object->getProperty ("action").toString().trim().toLowerCase();
    if (action.isEmpty())
        return;

    if (action == "stop")
    {
        audioProcessor.stopAutoSamplerCapture (true);
        return;
    }

    if (action != "start")
        return;

    SamplePlayerAudioProcessor::AutoSamplerSettings settings;
    const auto settingsVar = object->getProperty ("settings");
    if (const auto* settingsObj = settingsVar.getDynamicObject())
    {
        settings.startMidi = static_cast<int> (settingsObj->getProperty ("startMidi"));
        settings.endMidi = static_cast<int> (settingsObj->getProperty ("endMidi"));
        settings.intervalSemitones = static_cast<int> (settingsObj->getProperty ("intervalSemitones"));
        settings.velocityLayers = static_cast<int> (settingsObj->getProperty ("velocityLayers"));
        settings.roundRobinsPerNote = static_cast<int> (settingsObj->getProperty ("roundRobinsPerNote"));
        settings.sustainMs = static_cast<float> (double (settingsObj->getProperty ("sustainMs")));
        settings.releaseTailMs = static_cast<float> (double (settingsObj->getProperty ("releaseTailMs")));
        settings.prerollMs = static_cast<float> (double (settingsObj->getProperty ("prerollMs")));
        settings.loopSamples = static_cast<bool> (settingsObj->getProperty ("loopSamples"));
        settings.autoLoopMode = static_cast<bool> (settingsObj->getProperty ("autoLoopMode"));
        settings.loopStartPercent = static_cast<float> (double (settingsObj->getProperty ("loopStartPercent")));
        settings.loopEndPercent = static_cast<float> (double (settingsObj->getProperty ("loopEndPercent")));
        settings.cutLoopAtEnd = static_cast<bool> (settingsObj->getProperty ("cutLoopAtEnd"));
        settings.loopCrossfadeMs = static_cast<float> (double (settingsObj->getProperty ("loopCrossfadeMs")));
        settings.normalizeRecorded = static_cast<bool> (settingsObj->getProperty ("normalizeRecorded"));
    }

    juce::String errorMessage;
    if (! audioProcessor.startAutoSamplerCapture (settings, errorMessage))
    {
        SamplePlayerAudioProcessor::AutoSamplerProgress failedStatus;
        failedStatus.active = false;
        failedStatus.expectedTakes = 0;
        failedStatus.capturedTakes = 0;
        failedStatus.inputDetected = false;
        failedStatus.statusMessage = errorMessage.isNotEmpty() ? errorMessage : "Could not start sampling.";
        webView->emitEventIfBrowserIsVisible ("autosampler_status", makeAutoSamplerStatusVar (failedStatus));
    }
}

void SamplePlayerAudioProcessorEditor::handleUIResizeEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto requestedWidth = static_cast<int> (std::round (double (object->getProperty ("width"))));
    const auto requestedHeight = static_cast<int> (std::round (double (object->getProperty ("height"))));

    if (requestedWidth <= 0 || requestedHeight <= 0)
        return;

    const auto nextWidth = juce::jlimit (minEditorWidth, maxEditorWidth, requestedWidth);
    const auto nextHeight = juce::jlimit (minEditorHeight, maxEditorHeight, requestedHeight);

    if (nextWidth == lastEditorWidth && nextHeight == lastEditorHeight)
        return;

    lastEditorWidth = nextWidth;
    lastEditorHeight = nextHeight;
    setSize (nextWidth, nextHeight);
}
