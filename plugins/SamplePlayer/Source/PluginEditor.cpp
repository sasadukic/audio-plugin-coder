#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
void appendUiDebugLog (const juce::String& message)
{
    const auto logFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("SamplePlayer")
                             .getChildFile ("load_debug.log");
    const auto parent = logFile.getParentDirectory();
    if (! parent.isDirectory())
        parent.createDirectory();

    juce::String line;
    line << juce::Time::getCurrentTime().toString (true, true, true, true)
         << " | UI | "
         << message
         << "\n";
    logFile.appendText (line, false, false, "\n");
}

bool decodeDataUrlToMemory (const juce::String& dataUrl, juce::MemoryBlock& output, juce::String* outMimeType = nullptr)
{
    const auto trimmed = dataUrl.trim();
    if (! trimmed.startsWithIgnoreCase ("data:"))
        return false;

    const auto comma = trimmed.indexOfChar (',');
    if (comma <= 5)
        return false;

    const auto header = trimmed.substring (5, comma);
    const auto lowerHeader = header.toLowerCase();
    if (! lowerHeader.contains (";base64"))
        return false;

    auto mimeType = header.upToFirstOccurrenceOf (";", false, false).trim();
    if (mimeType.isEmpty())
        mimeType = "application/octet-stream";
    if (outMimeType != nullptr)
        *outMimeType = mimeType;

    const auto payload = trimmed.substring (comma + 1).removeCharacters (" \n\r\t");
    output.reset();
    juce::MemoryOutputStream out (output, false);
    return juce::Base64::convertFromBase64 (out, payload);
}

juce::String sanitizeRelativeAssetPath (const juce::String& rawPath)
{
    auto path = rawPath.trim().replaceCharacter ('\\', '/');
    while (path.startsWithChar ('/'))
        path = path.substring (1);

    juce::StringArray parts;
    parts.addTokens (path, "/", "");
    parts.removeEmptyStrings();

    juce::StringArray safe;
    for (const auto& part : parts)
    {
        const auto token = part.trim();
        if (token.isEmpty() || token == "." || token == "..")
            continue;
        safe.add (token);
    }

    return safe.joinIntoString ("/");
}
} // namespace

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
                     .withEventListener ("pick_destination_folder", [&editor] (const juce::var& payload)
                     {
                         editor.handleDestinationFolderPickEvent (payload);
                     })
                     .withEventListener ("pick_instrument_manifest", [&editor] (const juce::var& payload)
                     {
                         editor.handlePickInstrumentManifestEvent (payload);
                     })
                     .withEventListener ("session_state_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleSessionStateSetEvent (payload);
                     })
                     .withEventListener ("active_map_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleActiveMapSetEvent (payload);
                     })
                     .withEventListener ("sequencer_host_trigger_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleSequencerHostTriggerSetEvent (payload);
                     })
                     .withEventListener ("session_state_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleSessionStateGetEvent (payload);
                     })
                     .withEventListener ("sample_data_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleSampleDataGetEvent (payload);
                     })
                     .withEventListener ("graphic_data_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleGraphicDataGetEvent (payload);
                     })
                     .withEventListener ("preview_midi", [&editor] (const juce::var& payload)
                     {
                         editor.handlePreviewMidiEvent (payload);
                     })
                     .withEventListener ("save_instrument_bundle", [&editor] (const juce::var& payload)
                     {
                         editor.handleSaveInstrumentBundleEvent (payload);
                     })
                     .withEventListener ("debug_log", [&editor] (const juce::var& payload)
                     {
                         editor.handleDebugLogEvent (payload);
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

juce::String SamplePlayerAudioProcessorEditor::buildFileDataUrl (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    juce::MemoryBlock bytes;
    if (! file.loadFileAsData (bytes) || bytes.getSize() == 0)
        return {};

    const auto ext = file.getFileExtension().toLowerCase();
    juce::String mimeType = "application/octet-stream";
    if (ext == ".png") mimeType = "image/png";
    else if (ext == ".jpg" || ext == ".jpeg") mimeType = "image/jpeg";
    else if (ext == ".webp") mimeType = "image/webp";
    else if (ext == ".bmp") mimeType = "image/bmp";
    else if (ext == ".gif") mimeType = "image/gif";
    else if (ext == ".svg") mimeType = "image/svg+xml";
    else if (ext == ".avif") mimeType = "image/avif";
    else if (ext == ".wav") mimeType = "audio/wav";
    else if (ext == ".aif" || ext == ".aiff") mimeType = "audio/aiff";
    else if (ext == ".flac") mimeType = "audio/flac";
    else if (ext == ".ogg") mimeType = "audio/ogg";
    else if (ext == ".mp3") mimeType = "audio/mpeg";

    return "data:" + mimeType + ";base64," + juce::Base64::toBase64 (bytes.getData(), bytes.getSize());
}

void SamplePlayerAudioProcessorEditor::timerCallback()
{
    if (! webView)
        return;

    const auto lightweightSessionJson = audioProcessor.getUiSessionStateJson (true);
    if (lightweightSessionJson.isNotEmpty()
        && lightweightSessionJson != lastPushedLightweightSessionJson)
    {
        lastPushedLightweightSessionJson = lightweightSessionJson;
        auto payloadObject = juce::DynamicObject::Ptr (new juce::DynamicObject());
        payloadObject->setProperty ("json", lightweightSessionJson);
        payloadObject->setProperty ("lightweight", true);
        payloadObject->setProperty ("full", false);
        webView->emitEventIfBrowserIsVisible ("session_state_payload", juce::var (payloadObject.get()));
        appendUiDebugLog ("session_state_push auto | bytesOut="
                          + juce::String (lightweightSessionJson.getNumBytesAsUTF8()));
    }

    const auto emitTakeEvent = [this] (const juce::String& fileName,
                                       const juce::String& filePath,
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
        object->setProperty ("filePath", filePath);
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
                       {},
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
                       take.filePath,
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

    const auto [heldMaskLo, heldMaskHi] = audioProcessor.getHeldMidiMaskForUi();
    const int activeMapSetSlot = audioProcessor.getActiveMapSetSlotForUi();
    const int sequencerStep = audioProcessor.getSequencerCurrentStepForUi();
    const bool midiActivityChanged = heldMaskLo != lastPushedHeldMidiMaskLo
                                  || heldMaskHi != lastPushedHeldMidiMaskHi
                                  || activeMapSetSlot != lastPushedActiveMapSetSlot
                                  || sequencerStep != lastPushedSequencerStep;

    if (midiActivityChanged)
    {
        lastPushedHeldMidiMaskLo = heldMaskLo;
        lastPushedHeldMidiMaskHi = heldMaskHi;
        lastPushedActiveMapSetSlot = activeMapSetSlot;
        lastPushedSequencerStep = sequencerStep;

        juce::Array<juce::var> notes;
        notes.ensureStorageAllocated (16);

        for (int midi = 0; midi < 64; ++midi)
        {
            const juce::uint64 bit = juce::uint64 (1) << static_cast<juce::uint64> (midi);
            if ((heldMaskLo & bit) != 0)
                notes.add (midi);
        }

        for (int midi = 64; midi < 128; ++midi)
        {
            const juce::uint64 bit = juce::uint64 (1) << static_cast<juce::uint64> (midi - 64);
            if ((heldMaskHi & bit) != 0)
                notes.add (midi);
        }

        auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        payload->setProperty ("notes", juce::var (notes));
        payload->setProperty ("activeSlot", activeMapSetSlot);
        payload->setProperty ("sequencerStep", sequencerStep);
        webView->emitEventIfBrowserIsVisible ("midi_activity", juce::var (payload.get()));
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
        settings.destinationFolder = settingsObj->getProperty ("destinationFolder").toString();
        settings.instrumentName = settingsObj->getProperty ("instrumentName").toString();
        settings.keyswitchMode = static_cast<bool> (settingsObj->getProperty ("keyswitchMode"));
        settings.keyswitchKey = settingsObj->getProperty ("keyswitchKey").toString();
        settings.wallpaperSourcePath = settingsObj->getProperty ("wallpaperSourcePath").toString();
        settings.wallpaperDataUrl = settingsObj->getProperty ("wallpaperDataUrl").toString();
        settings.wallpaperFileName = settingsObj->getProperty ("wallpaperFileName").toString();
        settings.logoSourcePath = settingsObj->getProperty ("logoSourcePath").toString();
        settings.logoDataUrl = settingsObj->getProperty ("logoDataUrl").toString();
        settings.logoFileName = settingsObj->getProperty ("logoFileName").toString();
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

void SamplePlayerAudioProcessorEditor::handleDestinationFolderPickEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    juce::File initialDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    if (const auto* object = eventPayload.getDynamicObject())
    {
        const auto currentPath = object->getProperty ("currentPath").toString().trim();
        if (juce::File::isAbsolutePath (currentPath))
        {
            const auto current = juce::File (currentPath);
            if (current.isDirectory())
                initialDir = current;
            else if (current.exists())
                initialDir = current.getParentDirectory();
        }
    }

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories;

    destinationFolderChooser = std::make_unique<juce::FileChooser> ("Choose destination folder", initialDir, "*", true);
    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    destinationFolderChooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;

        const auto selectedFolder = chooser.getResult();
        if (! selectedFolder.isDirectory())
        {
            safeThis->destinationFolderChooser.reset();
            return;
        }

        auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        payload->setProperty ("path", selectedFolder.getFullPathName());
        safeThis->webView->emitEventIfBrowserIsVisible ("destination_folder_selected", juce::var (payload.get()));
        appendUiDebugLog ("destination folder selected | path=" + selectedFolder.getFullPathName());
        safeThis->destinationFolderChooser.reset();
    });
}

void SamplePlayerAudioProcessorEditor::handlePickInstrumentManifestEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    int requestId = -1;
    juce::File initialDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        const auto currentPath = object->getProperty ("currentPath").toString().trim();
        if (juce::File::isAbsolutePath (currentPath))
        {
            const auto current = juce::File (currentPath);
            if (current.isDirectory())
                initialDir = current;
            else if (current.exists())
                initialDir = current.getParentDirectory();
        }
    }

    loadInstrumentChooser = std::make_unique<juce::FileChooser> ("Open instrument JSON",
                                                                  initialDir,
                                                                  "*.json;*.smpinst",
                                                                  true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    loadInstrumentChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                        [safeThis, requestId] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || safeThis->webView == nullptr)
            return;

        auto emitResult = [safeThis, requestId] (bool success,
                                                 const juce::String& path,
                                                 const juce::String& fileName,
                                                 const juce::String& text,
                                                 const juce::String& message)
        {
            if (safeThis == nullptr || safeThis->webView == nullptr)
                return;

            auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
            payload->setProperty ("requestId", requestId);
            payload->setProperty ("success", success);
            payload->setProperty ("path", path);
            payload->setProperty ("fileName", fileName);
            payload->setProperty ("text", text);
            payload->setProperty ("message", message);
            safeThis->webView->emitEventIfBrowserIsVisible ("instrument_manifest_picked", juce::var (payload.get()));
        };

        const auto file = chooser.getResult();
        if (! file.existsAsFile())
        {
            emitResult (false, {}, {}, {}, "Load canceled.");
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        const auto text = file.loadFileAsString();
        if (text.isEmpty() && file.getSize() > 0)
        {
            emitResult (false,
                        file.getFullPathName(),
                        file.getFileName(),
                        {},
                        "Could not read selected file.");
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        emitResult (true,
                    file.getFullPathName(),
                    file.getFileName(),
                    text,
                    {});
        safeThis->loadInstrumentChooser.reset();
    });
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

void SamplePlayerAudioProcessorEditor::handleSessionStateSetEvent (const juce::var& eventPayload)
{
    juce::String jsonPayload;

    if (const auto* object = eventPayload.getDynamicObject())
        jsonPayload = object->getProperty ("json").toString();
    else if (eventPayload.isString())
        jsonPayload = eventPayload.toString();

    audioProcessor.setUiSessionStateJson (jsonPayload);
}

void SamplePlayerAudioProcessorEditor::handleActiveMapSetEvent (const juce::var& eventPayload)
{
    juce::String setId;

    if (const auto* object = eventPayload.getDynamicObject())
        setId = object->getProperty ("setId").toString().trim();
    else if (eventPayload.isString())
        setId = eventPayload.toString().trim();

    if (setId.isEmpty())
        return;

    audioProcessor.setActiveMapSetId (setId);
}

void SamplePlayerAudioProcessorEditor::handleSequencerHostTriggerSetEvent (const juce::var& eventPayload)
{
    bool enabled = false;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        const auto enabledVar = object->getProperty ("enabled");
        if (! enabledVar.isVoid())
            enabled = static_cast<bool> (enabledVar);
        else
            enabled = static_cast<bool> (object->getProperty ("value"));
    }
    else if (! eventPayload.isVoid())
    {
        enabled = static_cast<bool> (eventPayload);
    }

    audioProcessor.setSequencerHostTriggerEnabled (enabled);
}

void SamplePlayerAudioProcessorEditor::handleSessionStateGetEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    const auto requestStartMs = juce::Time::getMillisecondCounterHiRes();
    bool requestFull = false;
    auto reason = juce::String ("default");

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestFull = static_cast<bool> (object->getProperty ("full"));
        const auto mode = object->getProperty ("mode").toString().trim().toLowerCase();
        if (mode == "full")
            requestFull = true;
        else if (mode == "light")
            requestFull = false;

        const auto payloadReason = object->getProperty ("reason").toString().trim();
        if (payloadReason.isNotEmpty())
            reason = payloadReason;
    }

    appendUiDebugLog ("session_state_get begin | mode=" + juce::String (requestFull ? "full" : "light")
                      + " | reason=" + reason);
    const auto payloadFetchStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto jsonPayload = audioProcessor.getUiSessionStateJson (! requestFull);
    const auto payloadFetchMs = juce::Time::getMillisecondCounterHiRes() - payloadFetchStartMs;

    auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
    object->setProperty ("json", jsonPayload);
    object->setProperty ("lightweight", ! requestFull);
    object->setProperty ("full", requestFull);
    const auto emitStartMs = juce::Time::getMillisecondCounterHiRes();
    webView->emitEventIfBrowserIsVisible ("session_state_payload", juce::var (object.get()));
    const auto emitMs = juce::Time::getMillisecondCounterHiRes() - emitStartMs;

    appendUiDebugLog ("session_state_get handled | mode=" + juce::String (requestFull ? "full" : "light")
                      + " | reason=" + reason
                      + " | bytesOut=" + juce::String (jsonPayload.getNumBytesAsUTF8())
                      + " | fetchMs=" + juce::String (payloadFetchMs, 2)
                      + " | emitMs=" + juce::String (emitMs, 2)
                      + " | elapsedMs=" + juce::String (juce::Time::getMillisecondCounterHiRes() - requestStartMs, 2));
}

void SamplePlayerAudioProcessorEditor::handleSampleDataGetEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    int requestId = -1;
    int order = -1;
    int rootMidi = 60;
    int velocityLayer = 1;
    int rrIndex = 1;
    juce::String fileName;
    juce::String manifestPath;
    juce::StringArray manifestPathCandidates;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        order = static_cast<int> (std::round (double (object->getProperty ("order"))));
        rootMidi = static_cast<int> (std::round (double (object->getProperty ("rootMidi"))));
        velocityLayer = static_cast<int> (std::round (double (object->getProperty ("velocityLayer"))));
        rrIndex = static_cast<int> (std::round (double (object->getProperty ("rrIndex"))));
        fileName = object->getProperty ("fileName").toString().trim();
        manifestPath = object->getProperty ("manifestPath").toString().trim();
        if (const auto* pathArray = object->getProperty ("manifestPathCandidates").getArray())
        {
            for (const auto& pathVar : *pathArray)
            {
                const auto pathCandidate = pathVar.toString().trim();
                if (pathCandidate.isNotEmpty())
                    manifestPathCandidates.addIfNotAlreadyThere (pathCandidate);
            }
        }
    }

    const auto requestStartMs = juce::Time::getMillisecondCounterHiRes();
    auto dataUrl = audioProcessor.getSampleDataUrlForMapEntry (rootMidi, velocityLayer, rrIndex, fileName);
    if (dataUrl.isEmpty())
    {
        if (manifestPath.isNotEmpty())
            manifestPathCandidates.insert (0, manifestPath);

        for (const auto& pathCandidate : manifestPathCandidates)
        {
            dataUrl = audioProcessor.getSampleDataUrlForAbsolutePath (pathCandidate, fileName);
            if (dataUrl.isNotEmpty())
                break;
        }
    }

    auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
    object->setProperty ("requestId", requestId);
    object->setProperty ("order", order);
    object->setProperty ("dataUrl", dataUrl);
    object->setProperty ("hasData", dataUrl.isNotEmpty());
    webView->emitEventIfBrowserIsVisible ("sample_data_payload", juce::var (object.get()));

    appendUiDebugLog ("sample_data_get handled | requestId=" + juce::String (requestId)
                      + " | order=" + juce::String (order)
                      + " | root=" + juce::String (rootMidi)
                      + " | velocityLayer=" + juce::String (velocityLayer)
                      + " | rr=" + juce::String (rrIndex)
                      + " | manifestPath=" + juce::String (manifestPath.isNotEmpty() ? "yes" : "no")
                      + " | manifestPathCandidates=" + juce::String (manifestPathCandidates.size())
                      + " | bytesOut=" + juce::String (dataUrl.getNumBytesAsUTF8())
                      + " | elapsedMs=" + juce::String (juce::Time::getMillisecondCounterHiRes() - requestStartMs, 2));
}

void SamplePlayerAudioProcessorEditor::handleGraphicDataGetEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    int requestId = -1;
    juce::String kind;
    juce::String path;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        kind = object->getProperty ("kind").toString().trim();
        path = object->getProperty ("path").toString().trim();
    }

    auto response = juce::DynamicObject::Ptr (new juce::DynamicObject());
    response->setProperty ("requestId", requestId);
    response->setProperty ("kind", kind);
    response->setProperty ("path", path);

    juce::String dataUrl;
    juce::String mimeType;
    juce::String fileName;
    if (juce::File::isAbsolutePath (path))
    {
        const auto file = juce::File (path);
        if (file.existsAsFile())
        {
            dataUrl = buildFileDataUrl (file);
            fileName = file.getFileName();
            mimeType = file.getFileExtension().toLowerCase();
        }
    }

    response->setProperty ("dataUrl", dataUrl);
    response->setProperty ("hasData", dataUrl.isNotEmpty());
    response->setProperty ("fileName", fileName);
    response->setProperty ("mimeType", mimeType);
    webView->emitEventIfBrowserIsVisible ("graphic_data_payload", juce::var (response.get()));
}

void SamplePlayerAudioProcessorEditor::handlePreviewMidiEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const bool noteOn = static_cast<bool> (object->getProperty ("noteOn"));
    const int midiNote = static_cast<int> (std::round (double (object->getProperty ("midiNote"))));
    const int velocity127 = static_cast<int> (std::round (double (object->getProperty ("velocity"))));
    const int midiChannel = static_cast<int> (std::round (double (object->getProperty ("channel"))));

    audioProcessor.queuePreviewMidiEvent (noteOn, midiNote, velocity127, midiChannel);
}

void SamplePlayerAudioProcessorEditor::handleSaveInstrumentBundleEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    struct SaveAsset
    {
        juce::String relativePath;
        juce::String dataUrl;
    };

    auto emitResult = [this] (bool success,
                              const juce::String& manifestPath,
                              const juce::String& manifestBasePath,
                              const juce::String& message,
                              int writtenAssets,
                              int failedAssets)
    {
        auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        payload->setProperty ("success", success);
        payload->setProperty ("manifestPath", manifestPath);
        payload->setProperty ("manifestBasePath", manifestBasePath);
        payload->setProperty ("message", message);
        payload->setProperty ("writtenAssets", writtenAssets);
        payload->setProperty ("failedAssets", failedAssets);
        webView->emitEventIfBrowserIsVisible ("instrument_save_result", juce::var (payload.get()));
    };

    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
    {
        emitResult (false, {}, {}, "Invalid save payload.", 0, 0);
        return;
    }

    auto manifestJson = object->getProperty ("manifestJson").toString();
    auto manifestPath = object->getProperty ("manifestPath").toString().trim();
    const bool askForPath = static_cast<bool> (object->getProperty ("askForPath"));

    if (manifestJson.trim().isEmpty())
    {
        emitResult (false, {}, {}, "Missing manifest JSON.", 0, 0);
        return;
    }

    std::vector<SaveAsset> assets;
    if (const auto* assetsArray = object->getProperty ("assets").getArray())
    {
        assets.reserve (static_cast<size_t> (assetsArray->size()));
        for (const auto& assetVar : *assetsArray)
        {
            const auto* assetObject = assetVar.getDynamicObject();
            if (assetObject == nullptr)
                continue;

            SaveAsset item;
            item.relativePath = assetObject->getProperty ("path").toString().trim();
            item.dataUrl = assetObject->getProperty ("dataUrl").toString().trim();
            if (item.relativePath.isEmpty() || item.dataUrl.isEmpty())
                continue;
            assets.push_back (std::move (item));
        }
    }

    const auto writeBundle = [emitResult, manifestJson, assets] (juce::File manifestFile)
    {
        if (manifestFile == juce::File {})
        {
            emitResult (false, {}, {}, "Save canceled.", 0, 0);
            return;
        }

        if (manifestFile.hasFileExtension (".json") == false
            && manifestFile.hasFileExtension (".smpinst") == false)
        {
            manifestFile = manifestFile.withFileExtension (".json");
        }

        const auto baseDir = manifestFile.getParentDirectory();
        if (! baseDir.isDirectory())
        {
            const auto createDirResult = baseDir.createDirectory();
            if (createDirResult.failed())
            {
                emitResult (false,
                            manifestFile.getFullPathName(),
                            baseDir.getFullPathName(),
                            "Could not create destination folder: " + createDirResult.getErrorMessage(),
                            0,
                            0);
                return;
            }
        }

        int writtenAssets = 0;
        int failedAssets = 0;

        for (const auto& asset : assets)
        {
            const auto relativePath = sanitizeRelativeAssetPath (asset.relativePath);
            if (relativePath.isEmpty())
            {
                ++failedAssets;
                continue;
            }

            juce::MemoryBlock bytes;
            if (! decodeDataUrlToMemory (asset.dataUrl, bytes) || bytes.getSize() == 0)
            {
                ++failedAssets;
                continue;
            }

            const auto targetFile = baseDir.getChildFile (relativePath);
            const auto parent = targetFile.getParentDirectory();
            if (! parent.isDirectory())
            {
                const auto createParentResult = parent.createDirectory();
                if (createParentResult.failed())
                {
                    ++failedAssets;
                    continue;
                }
            }

            if (targetFile.replaceWithData (bytes.getData(), bytes.getSize()))
                ++writtenAssets;
            else
                ++failedAssets;
        }

        const bool manifestOk = manifestFile.replaceWithText (manifestJson, false, false, "\n");
        if (! manifestOk)
        {
            emitResult (false,
                        manifestFile.getFullPathName(),
                        baseDir.getFullPathName(),
                        "Could not write manifest JSON.",
                        writtenAssets,
                        failedAssets);
            return;
        }

        juce::String message;
        message << "Saved instrument to " << manifestFile.getFullPathName()
                << " (" << juce::String (writtenAssets) << " assets";
        if (failedAssets > 0)
            message << ", " << juce::String (failedAssets) << " failed";
        message << ").";

        emitResult (failedAssets == 0,
                    manifestFile.getFullPathName(),
                    baseDir.getFullPathName(),
                    message,
                    writtenAssets,
                    failedAssets);
    };

    if (juce::File::isAbsolutePath (manifestPath))
    {
        writeBundle (juce::File (manifestPath));
        return;
    }

    if (! askForPath)
    {
        emitResult (false, {}, {}, "No manifest path set. Choose where to save JSON first.", 0, 0);
        return;
    }

    juce::File initialDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    juce::String defaultName = "Instrument.json";
    if (const auto parsed = juce::JSON::parse (manifestJson); parsed.isObject())
    {
        if (const auto* manifestObject = parsed.getDynamicObject())
        {
            const auto instrumentName = juce::File::createLegalFileName (manifestObject->getProperty ("instrumentName").toString().trim());
            if (instrumentName.isNotEmpty())
                defaultName = instrumentName + ".json";
        }
    }

    saveInstrumentChooser = std::make_unique<juce::FileChooser> ("Save instrument JSON",
                                                                  initialDir.getChildFile (defaultName),
                                                                  "*.json;*.smpinst",
                                                                  true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    saveInstrumentChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                        [safeThis, writeBundle] (const juce::FileChooser& chooser) mutable
    {
        if (safeThis == nullptr)
            return;

        writeBundle (chooser.getResult());
        safeThis->saveInstrumentChooser.reset();
    });
}

void SamplePlayerAudioProcessorEditor::handleDebugLogEvent (const juce::var& eventPayload)
{
    juce::String message;

    if (const auto* object = eventPayload.getDynamicObject())
        message = object->getProperty ("message").toString();
    else if (eventPayload.isString())
        message = eventPayload.toString();

    message = message.trim();
    if (message.isEmpty())
        return;

    appendUiDebugLog (message);
}
