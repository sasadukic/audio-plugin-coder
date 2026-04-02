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

juce::String getMimeTypeForFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    return "application/octet-stream";
}

juce::String buildFileDataUrl (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    juce::MemoryBlock bytes;
    if (! file.loadFileAsData (bytes) || bytes.getSize() == 0)
        return {};

    return "data:" + getMimeTypeForFile (file) + ";base64,"
         + juce::Base64::toBase64 (bytes.getData(), bytes.getSize());
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

juce::String extractResourcePathFromUrl (const juce::String& rawUrl)
{
    auto url = rawUrl.trim();
    if (url.isEmpty())
        return {};

    const auto root = juce::WebBrowserComponent::getResourceProviderRoot();
    if (url.startsWithIgnoreCase (root))
        url = url.fromFirstOccurrenceOf (root, false, false);
    else if (const auto schemePos = url.indexOf ("://"); schemePos >= 0)
    {
        if (const auto pathPos = url.indexOfChar (schemePos + 3, '/'); pathPos >= 0)
            url = url.substring (pathPos);
        else
            url.clear();
    }

    url = url.upToFirstOccurrenceOf ("?", false, false);
    url = url.upToFirstOccurrenceOf ("#", false, false);

    if (url.isEmpty() || url == "/")
        return "index.html";

    if (url.startsWithChar ('/'))
        url = url.substring (1);

    return sanitizeRelativeAssetPath (url);
}

} // namespace

SamplePlayerAudioProcessorEditor::SamplePlayerAudioProcessorEditor (SamplePlayerAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    webView.reset (new SinglePageBrowser (createWebOptions (*this)));
    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    lastPushedLightweightVersion = audioProcessor.getUiSessionStateLightweightVersion();
    lastPushedLightweightSessionJson = audioProcessor.getUiSessionStateJson (true);

    setSize (defaultEditorWidth, defaultPlayerHeight);

    startTimerHz (10);
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
                     .withEventListener ("frontend_ready", [&editor] (const juce::var&)
                     {
                         editor.frontendReadyForEvents = true;
                     })
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
                     .withEventListener ("pick_graphic_file", [&editor] (const juce::var& payload)
                     {
                         editor.handlePickGraphicFileEvent (payload);
                     })
                     .withEventListener ("session_state_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleSessionStateSetEvent (payload);
                     })
                     .withEventListener ("active_map_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleActiveMapSetEvent (payload);
                     })
                     .withEventListener ("keyswitch_gain_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleKeyswitchGainSetEvent (payload);
                     })
                     .withEventListener ("sequencer_host_trigger_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleSequencerHostTriggerSetEvent (payload);
                     })
                     .withEventListener ("sequencer_settings_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleSequencerSettingsSetEvent (payload);
                     })
                     .withEventListener ("strum_settings_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleStrumSettingsSetEvent (payload);
                     })
                     .withEventListener ("session_state_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleSessionStateGetEvent (payload);
                     })
                     .withEventListener ("sample_data_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleSampleDataGetEvent (payload);
                     })
                     .withEventListener ("preview_midi", [&editor] (const juce::var& payload)
                     {
                         editor.handlePreviewMidiEvent (payload);
                     })
                     .withEventListener ("performance_wheel_set", [&editor] (const juce::var& payload)
                     {
                         editor.handlePerformanceWheelSetEvent (payload);
                     })
                     .withEventListener ("amp_envelope_set", [&editor] (const juce::var& payload)
                     {
                         editor.handleAmpEnvelopeSetEvent (payload);
                     })
                     .withEventListener ("save_instrument_bundle", [&editor] (const juce::var& payload)
                     {
                         editor.handleSaveInstrumentBundleEvent (payload);
                     })
                     .withEventListener ("graphic_data_get", [&editor] (const juce::var& payload)
                     {
                         editor.handleGraphicDataGetEvent (payload);
                     })
                     .withEventListener ("debug_log", [&editor] (const juce::var& payload)
                     {
                         editor.handleDebugLogEvent (payload);
                     })
                     .withEventListener ("pick_audio_files", [&editor] (const juce::var& payload)
                     {
                         editor.handlePickAudioFilesEvent (payload);
                     })
                     .withEventListener ("pick_audio_folder", [&editor] (const juce::var& payload)
                     {
                         editor.handlePickAudioFolderEvent (payload);
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

    auto makeStringResource = [] (const juce::String& text, const char* mime)
    {
        const auto utf8 = text.toUTF8();
        const auto* bytesBegin = reinterpret_cast<const std::byte*> (utf8.getAddress());
        const auto numBytes = static_cast<size_t> (std::strlen (utf8.getAddress()));

        return juce::WebBrowserComponent::Resource{
            std::vector<std::byte> (bytesBegin, bytesBegin + numBytes),
            juce::String (mime)
        };
    };

    const auto path = extractResourcePathFromUrl (url);

    if (path.isEmpty())
    {
        appendUiDebugLog ("resource request ignored | unresolved path | url=" + url);
        return std::nullopt;
    }

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

    if (path == "session-state-light.json" || path == "session-state-full.json")
    {
        const auto jsonPayload = buildSessionStateJsonForFrontend (path == "session-state-full.json");
        appendUiDebugLog ("resource request served | path=" + path
                          + " | bytesOut=" + juce::String (jsonPayload.getNumBytesAsUTF8()));
        return makeStringResource (jsonPayload, "application/json");
    }

    appendUiDebugLog ("resource request missing | path=" + path + " | url=" + url);
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

juce::String SamplePlayerAudioProcessorEditor::buildSessionStateJsonForFrontend (bool requestFull) const
{
    return audioProcessor.getUiSessionStateJson (! requestFull);
}

void SamplePlayerAudioProcessorEditor::timerCallback()
{
    if (! webView)
        return;

    const auto _timerStart = juce::Time::getMillisecondCounterHiRes();

    // Flush profiler ring buffer to log file periodically
    audioProcessor.perfFlushToFile();
    const auto _afterFlush = juce::Time::getMillisecondCounterHiRes();

    if (! frontendReadyForEvents)
        return;

    const int currentLightweightVersion = audioProcessor.getUiSessionStateLightweightVersion();
    if (currentLightweightVersion != lastPushedLightweightVersion)
    {
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();
        if (nowMs < suppressLightweightPushUntilMs)
        {
            lastPushedLightweightVersion = currentLightweightVersion;
        }
        else
        {
            const auto lightweightSessionJson = audioProcessor.getUiSessionStateJson (true);
            lastPushedLightweightVersion = currentLightweightVersion;
            if (lightweightSessionJson.isNotEmpty()
                && lightweightSessionJson != lastPushedLightweightSessionJson)
            {
                lastPushedLightweightSessionJson = lightweightSessionJson;
                auto payloadObject = juce::DynamicObject::Ptr (new juce::DynamicObject());
                payloadObject->setProperty ("lightweight", true);
                payloadObject->setProperty ("full", false);
                payloadObject->setProperty ("version", currentLightweightVersion);
                payloadObject->setProperty ("reason", "auto");
                const auto _beforePush = juce::Time::getMillisecondCounterHiRes();
                webView->emitEventIfBrowserIsVisible ("session_state_changed", juce::var (payloadObject.get()));
                const auto _afterPush = juce::Time::getMillisecondCounterHiRes();
                appendUiDebugLog ("session_state_changed auto | version="
                                  + juce::String (currentLightweightVersion)
                                  + " | emitMs=" + juce::String (_afterPush - _beforePush, 2)
                                  + " | flushMs=" + juce::String (_afterFlush - _timerStart, 2));
            }
        }
    }

    {
        constexpr int kMaxEmitsPerTick = 1;
        int emitted = 0;
        while (emitted < kMaxEmitsPerTick)
        {
            PendingGraphicDataEmit pending;
            {
                const juce::ScopedLock lock (pendingGraphicDataEmitLock);
                if (pendingGraphicDataEmitQueue.empty())
                    break;
                pending = std::move (pendingGraphicDataEmitQueue.front());
                pendingGraphicDataEmitQueue.pop_front();
            }

            auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
            object->setProperty ("requestId", pending.requestId);
            object->setProperty ("path", pending.path);
            object->setProperty ("success", pending.dataUrl.isNotEmpty());
            object->setProperty ("dataUrl", pending.dataUrl);
            object->setProperty ("message", pending.message);
            webView->emitEventIfBrowserIsVisible ("graphic_data_payload", juce::var (object.get()));

            appendUiDebugLog ("graphic_data_get emitted | requestId=" + juce::String (pending.requestId)
                              + " | path=" + pending.path.quoted()
                              + " | bytesOut=" + juce::String (pending.bytesOut)
                              + " | encodingMs=" + juce::String (pending.encodingElapsedMs, 2)
                              + " | success=" + juce::String (pending.dataUrl.isNotEmpty() ? "yes" : "no")
                              + (pending.message.isNotEmpty() ? " | message=" + pending.message.quoted() : juce::String()));
            ++emitted;
        }
    }

    {
        constexpr int kMaxEmitsPerTick = 1;
        int emitted = 0;
        while (emitted < kMaxEmitsPerTick)
        {
            PendingSampleDataEmit pending;
            {
                const juce::ScopedLock lock (pendingSampleDataEmitLock);
                if (pendingSampleDataEmitQueue.empty())
                    break;
                pending = std::move (pendingSampleDataEmitQueue.front());
                pendingSampleDataEmitQueue.pop_front();
            }

            auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());
            object->setProperty ("requestId", pending.requestId);
            object->setProperty ("order", pending.order);
            object->setProperty ("dataUrl", pending.dataUrl);
            object->setProperty ("hasData", pending.dataUrl.isNotEmpty());
            webView->emitEventIfBrowserIsVisible ("sample_data_payload", juce::var (object.get()));

            appendUiDebugLog ("sample_data_get emitted | requestId=" + juce::String (pending.requestId)
                              + " | order=" + juce::String (pending.order)
                              + " | root=" + juce::String (pending.rootMidi)
                              + " | velocityLayer=" + juce::String (pending.velocityLayer)
                              + " | rr=" + juce::String (pending.rrIndex)
                              + " | manifestPath=" + juce::String (pending.hasManifestPath ? "yes" : "no")
                              + " | manifestPathCandidates=" + juce::String (pending.manifestPathCandidateCount)
                              + " | bytesOut=" + juce::String (pending.bytesOut)
                              + " | encodingMs=" + juce::String (pending.encodingElapsedMs, 2));
            ++emitted;
        }
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
    const auto [modWheelValue, expressionValue] = audioProcessor.getPerformanceWheelValuesForUi();
    const int activeMapSetSlot = audioProcessor.getActiveMapSetSlotForUi();
    const int sequencerStep = audioProcessor.getSequencerCurrentStepForUi();
    const bool midiActivityChanged = heldMaskLo != lastPushedHeldMidiMaskLo
                                  || heldMaskHi != lastPushedHeldMidiMaskHi
                                  || std::abs (modWheelValue - lastPushedModWheelValue) > 0.0005f
                                  || std::abs (expressionValue - lastPushedExpressionValue) > 0.0005f
                                  || activeMapSetSlot != lastPushedActiveMapSetSlot
                                  || sequencerStep != lastPushedSequencerStep;

    if (midiActivityChanged)
    {
        lastPushedHeldMidiMaskLo = heldMaskLo;
        lastPushedHeldMidiMaskHi = heldMaskHi;
        lastPushedModWheelValue = modWheelValue;
        lastPushedExpressionValue = expressionValue;
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
        payload->setProperty ("modWheelValue", modWheelValue);
        payload->setProperty ("expressionValue", expressionValue);
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
    {
        const auto _timerMs = juce::Time::getMillisecondCounterHiRes() - _timerStart;
        if (_timerMs > 5.0)
            audioProcessor.perfLog ("timerCallback", _timerMs, "SLOW-nochange");
        return;
    }

    lastAutoSamplerActive = progress.active;
    lastAutoSamplerExpected = progress.expectedTakes;
    lastAutoSamplerCaptured = progress.capturedTakes;
    lastAutoSamplerInputDetected = progress.inputDetected;
    lastAutoSamplerStatus = progress.statusMessage;
    webView->emitEventIfBrowserIsVisible ("autosampler_status", makeAutoSamplerStatusVar (progress));

    const auto _timerMs = juce::Time::getMillisecondCounterHiRes() - _timerStart;
    if (_timerMs > 5.0)
        audioProcessor.perfLog ("timerCallback", _timerMs, "SLOW");
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
    juce::String mode = "open";
    juce::String defaultName;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        const auto currentPath = object->getProperty ("currentPath").toString().trim();
        mode = object->getProperty ("mode").toString().trim().toLowerCase();
        defaultName = object->getProperty ("defaultName").toString().trim();
        if (juce::File::isAbsolutePath (currentPath))
        {
            const auto current = juce::File (currentPath);
            if (current.isDirectory())
                initialDir = current;
            else if (current.exists())
                initialDir = current.getParentDirectory();
        }
    }

    if (mode != "save")
        mode = "open";

    if (mode == "save" && defaultName.isNotEmpty())
        initialDir = initialDir.getChildFile (defaultName);

    loadInstrumentChooser = std::make_unique<juce::FileChooser> (mode == "save" ? "Save instrument JSON" : "Open instrument JSON",
                                                                  initialDir,
                                                                  "*.json;*.smpinst;*.smpinstm",
                                                                  true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    const int chooserFlags = (mode == "save"
                                ? (juce::FileBrowserComponent::saveMode
                                   | juce::FileBrowserComponent::canSelectFiles
                                   | juce::FileBrowserComponent::warnAboutOverwriting)
                                : (juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectFiles));
    loadInstrumentChooser->launchAsync (chooserFlags,
                                        [safeThis, requestId, mode] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || safeThis->webView == nullptr)
            return;

        auto emitResult = [safeThis, requestId] (bool success,
                                                 const juce::String& path,
                                                 const juce::String& fileName,
                                                 const juce::String& text,
                                                 const juce::String& message,
                                                 bool nativeLoaded,
                                                 const juce::String& format)
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
            payload->setProperty ("nativeLoaded", nativeLoaded);
            payload->setProperty ("format", format);
            safeThis->webView->emitEventIfBrowserIsVisible ("instrument_manifest_picked", juce::var (payload.get()));
        };

        auto file = chooser.getResult();
        if (mode == "save")
        {
            if (file == juce::File {})
            {
                emitResult (false, {}, {}, {}, "Save canceled.", false, {});
                safeThis->loadInstrumentChooser.reset();
                return;
            }

            if (file.hasFileExtension (".json") == false
                && file.hasFileExtension (".smpinst") == false
                && file.hasFileExtension (".smpinstm") == false)
            {
                file = file.withFileExtension (".json");
            }

            const auto format = file.hasFileExtension ("smpinstm") ? juce::String ("smpinstm")
                               : (file.hasFileExtension ("smpinst") ? juce::String ("smpinst")
                                                                     : juce::String ("json"));
            emitResult (true,
                        file.getFullPathName(),
                        file.getFileName(),
                        {},
                        {},
                        false,
                        format);
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        if (! file.existsAsFile())
        {
            emitResult (false, {}, {}, {}, "Load canceled.", false, {});
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        const bool isMonolith = file.hasFileExtension ("smpinstm");
        const bool isManifest = file.hasFileExtension ("smpinst") || file.hasFileExtension ("json");
        const auto format = isMonolith ? juce::String ("smpinstm")
                                       : (file.hasFileExtension ("smpinst") ? juce::String ("smpinst")
                                                                            : juce::String ("json"));

        if (isMonolith)
        {
            safeThis->audioProcessor.loadMonolithDirect (file.getFullPathName());
            emitResult (true,
                        file.getFullPathName(),
                        file.getFileName(),
                        {},
                        {},
                        true,
                        format);
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        if (isManifest)
        {
            safeThis->audioProcessor.loadManifestDirect (file.getFullPathName());
            emitResult (true,
                        file.getFullPathName(),
                        file.getFileName(),
                        {},
                        {},
                        true,
                        format);
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
                        "Could not read selected file.",
                        false,
                        format);
            safeThis->loadInstrumentChooser.reset();
            return;
        }

        emitResult (true,
                    file.getFullPathName(),
                    file.getFileName(),
                    text,
                    {},
                    false,
                    format);

        safeThis->loadInstrumentChooser.reset();
    });
}

void SamplePlayerAudioProcessorEditor::handlePickGraphicFileEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    int requestId = -1;
    juce::String currentPath;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        currentPath = object->getProperty ("currentPath").toString().trim();
    }

    auto emitResult = [this, requestId] (bool success,
                                         const juce::String& path,
                                         const juce::String& fileName,
                                         const juce::String& dataUrl,
                                         const juce::String& message)
    {
        if (! webView)
            return;

        auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        payload->setProperty ("requestId", requestId);
        payload->setProperty ("success", success);
        payload->setProperty ("path", path);
        payload->setProperty ("fileName", fileName);
        payload->setProperty ("dataUrl", dataUrl);
        payload->setProperty ("message", message);
        webView->emitEventIfBrowserIsVisible ("native_graphic_file_picked", juce::var (payload.get()));
    };

    juce::File initialDir = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
    if (juce::File::isAbsolutePath (currentPath))
    {
        const juce::File currentFile (currentPath);
        if (currentFile.isDirectory())
            initialDir = currentFile;
        else if (currentFile.exists())
            initialDir = currentFile.getParentDirectory();
    }

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    graphicFileChooser = std::make_unique<juce::FileChooser> (
        "Select wallpaper image",
        initialDir,
        "*.png;*.jpg;*.jpeg;*.webp;*.gif;*.svg",
        true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    graphicFileChooser->launchAsync (chooserFlags, [safeThis, emitResult] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;

        const auto file = chooser.getResult();
        if (! file.existsAsFile())
        {
            emitResult (false, {}, {}, {}, "Wallpaper selection canceled.");
            safeThis->graphicFileChooser.reset();
            return;
        }

        const auto dataUrl = buildFileDataUrl (file);
        if (dataUrl.isEmpty())
        {
            emitResult (false,
                        file.getFullPathName(),
                        file.getFileName(),
                        {},
                        "Could not read selected wallpaper file.");
            safeThis->graphicFileChooser.reset();
            return;
        }

        emitResult (true,
                    file.getFullPathName(),
                    file.getFileName(),
                    dataUrl,
                    {});
        safeThis->graphicFileChooser.reset();
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
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String jsonPayload;

    if (const auto* object = eventPayload.getDynamicObject())
        jsonPayload = object->getProperty ("json").toString();
    else if (eventPayload.isString())
        jsonPayload = eventPayload.toString();

    // The payload came from the UI itself.  Suppress the timer's automatic
    // lightweight session push for a short window so local edits such as
    // doubling do not bounce a large JSON payload straight back into JS.
    suppressLightweightPushUntilMs = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    audioProcessor.setUiSessionStateJson (jsonPayload);
    audioProcessor.perfLog ("session_state_set", juce::Time::getMillisecondCounterHiRes() - t0,
                            "bytes=" + juce::String (jsonPayload.getNumBytesAsUTF8()));
}

void SamplePlayerAudioProcessorEditor::handleActiveMapSetEvent (const juce::var& eventPayload)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String setId;

    if (const auto* object = eventPayload.getDynamicObject())
        setId = object->getProperty ("setId").toString().trim();
    else if (eventPayload.isString())
        setId = eventPayload.toString().trim();

    if (setId.isEmpty())
        return;

    audioProcessor.setActiveMapSetId (setId);
    audioProcessor.perfLog ("active_map_set", juce::Time::getMillisecondCounterHiRes() - t0,
                            "setId=" + setId);
}

void SamplePlayerAudioProcessorEditor::handleKeyswitchGainSetEvent (const juce::var& eventPayload)
{
    juce::String setId;
    float gainDb = 0.0f;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        setId = object->getProperty ("setId").toString().trim();
        gainDb = juce::jlimit (-24.0f,
                               24.0f,
                               static_cast<float> (double (object->getProperty ("gainDb"))));
    }
    else if (eventPayload.isString())
    {
        setId = eventPayload.toString().trim();
    }

    if (setId.isEmpty())
        return;

    audioProcessor.setKeyswitchSetGainDb (setId, gainDb);
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

void SamplePlayerAudioProcessorEditor::handleSequencerSettingsSetEvent (const juce::var& eventPayload)
{
    audioProcessor.applySequencerSettingsFromUi (eventPayload);
}

void SamplePlayerAudioProcessorEditor::handleStrumSettingsSetEvent (const juce::var& eventPayload)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    audioProcessor.applyStrumSettingsFromUi (eventPayload);
    audioProcessor.perfLog ("strum_settings_set", juce::Time::getMillisecondCounterHiRes() - t0, {});
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
    const auto jsonPayload = buildSessionStateJsonForFrontend (requestFull);
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

    const bool hasManPath = manifestPath.isNotEmpty();
    const int candCount = manifestPathCandidates.size();

    // Offload expensive WAV encoding to a background thread.  Results are
    // queued and drained by the 10Hz timer (max 2 per tick) to prevent
    // WKWebView IPC channel flooding which stalls the message thread.
    sampleDataRequestPool.addJob ([this, requestId, order, rootMidi, velocityLayer, rrIndex,
                                   fileName, manifestPath, manifestPathCandidates,
                                   hasManPath, candCount]() mutable
    {
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

        PendingSampleDataEmit pending;
        pending.requestId = requestId;
        pending.order = order;
        pending.dataUrl = std::move (dataUrl);
        pending.rootMidi = rootMidi;
        pending.velocityLayer = velocityLayer;
        pending.rrIndex = rrIndex;
        pending.hasManifestPath = hasManPath;
        pending.manifestPathCandidateCount = candCount;
        pending.bytesOut = pending.dataUrl.getNumBytesAsUTF8();
        pending.encodingElapsedMs = juce::Time::getMillisecondCounterHiRes() - requestStartMs;

        {
            const juce::ScopedLock lock (pendingSampleDataEmitLock);
            pendingSampleDataEmitQueue.push_back (std::move (pending));
        }
    });
}

void SamplePlayerAudioProcessorEditor::handlePreviewMidiEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto controllerNumberVar = object->getProperty ("controllerNumber");
    if (! controllerNumberVar.isVoid())
    {
        const int controllerNumber = static_cast<int> (std::round (double (controllerNumberVar)));
        const int controllerValue = static_cast<int> (std::round (double (object->getProperty ("controllerValue"))));
        const int midiChannel = static_cast<int> (std::round (double (object->getProperty ("channel"))));
        audioProcessor.queuePreviewControllerEvent (controllerNumber, controllerValue, midiChannel);
        return;
    }

    const bool noteOn = static_cast<bool> (object->getProperty ("noteOn"));
    const int midiNote = static_cast<int> (std::round (double (object->getProperty ("midiNote"))));
    const int velocity127 = static_cast<int> (std::round (double (object->getProperty ("velocity"))));
    const int midiChannel = static_cast<int> (std::round (double (object->getProperty ("channel"))));

    audioProcessor.queuePreviewMidiEvent (noteOn, midiNote, velocity127, midiChannel);
}

void SamplePlayerAudioProcessorEditor::handlePerformanceWheelSetEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto wheel = object->getProperty ("wheel").toString().trim().toLowerCase();
    auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (
        audioProcessor.parameters.getParameter (wheel == "expression" ? "expression" : "modWheel"));
    if (parameter == nullptr)
        return;

    const float value = juce::jlimit (0.0f, 1.0f, static_cast<float> (double (object->getProperty ("value"))));
    const auto phase = object->getProperty ("phase").toString().trim().toLowerCase();
    bool& gestureActive = (wheel == "expression") ? expressionGestureActive : modWheelGestureActive;

    if (phase == "begin")
    {
        if (! gestureActive)
        {
            parameter->beginChangeGesture();
            gestureActive = true;
        }
        parameter->setValueNotifyingHost (value);
        return;
    }

    if (phase == "end")
    {
        parameter->setValueNotifyingHost (value);
        if (gestureActive)
        {
            parameter->endChangeGesture();
            gestureActive = false;
        }
        return;
    }

    if (! gestureActive)
    {
        parameter->beginChangeGesture();
        gestureActive = true;
    }

    parameter->setValueNotifyingHost (value);
}

void SamplePlayerAudioProcessorEditor::handleAmpEnvelopeSetEvent (const juce::var& eventPayload)
{
    const auto* object = eventPayload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto applyParam = [this] (const juce::String& paramId, float plainValue)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (audioProcessor.parameters.getParameter (paramId));
        if (parameter == nullptr)
            return;

        const auto value01 = juce::jlimit (0.0f, 1.0f, parameter->convertTo0to1 (plainValue));
        if (std::abs (parameter->getValue() - value01) <= 0.000001f)
            return;

        parameter->setValueNotifyingHost (value01);
    };

    applyParam ("attackMs", static_cast<float> (juce::jmax (0.0, double (object->getProperty ("attackMs")))));
    applyParam ("decayMs", static_cast<float> (juce::jmax (0.0, double (object->getProperty ("decayMs")))));
    applyParam ("sustain", static_cast<float> (juce::jlimit (0.0, 1.0, double (object->getProperty ("sustainPercent")) / 100.0)));
    applyParam ("releaseMs", static_cast<float> (juce::jmax (0.0, double (object->getProperty ("releaseMs")))));
}

void SamplePlayerAudioProcessorEditor::handleSaveInstrumentBundleEvent (const juce::var& eventPayload)
{
    const auto _t0 = juce::Time::getMillisecondCounterHiRes();
    if (! webView)
        return;

    struct SaveAsset
    {
        juce::String relativePath;
        juce::String dataUrl;
        juce::String sourcePath;
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
    const bool overwriteExisting = static_cast<bool> (object->getProperty ("overwriteExisting"));

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
            item.sourcePath = assetObject->getProperty ("sourcePath").toString().trim();
            if (item.relativePath.isEmpty() || (item.dataUrl.isEmpty() && item.sourcePath.isEmpty()))
                continue;
            assets.push_back (std::move (item));
        }
    }

    {
        juce::String wallpaperPath;
        bool hasGraphics = false;
        if (const auto parsed = juce::JSON::parse (manifestJson); parsed.isObject())
        {
            if (const auto* manifestObject = parsed.getDynamicObject())
            {
                const auto graphicsVar = manifestObject->getProperty ("graphics");
                if (graphicsVar.isObject())
                {
                    hasGraphics = true;
                    if (const auto* graphicsObject = graphicsVar.getDynamicObject())
                        wallpaperPath = graphicsObject->getProperty ("wallpaperPath").toString().trim();
                }
            }
        }

        appendUiDebugLog ("native save_instrument_bundle received | hasGraphics="
                          + juce::String (hasGraphics ? "yes" : "no")
                          + " | wallpaperPath=" + wallpaperPath.quoted()
                          + " | askForPath=" + juce::String (askForPath ? "yes" : "no")
                          + " | overwriteExisting=" + juce::String (overwriteExisting ? "yes" : "no")
                          + " | manifestPath=" + manifestPath.quoted()
                          + " | assetCount=" + juce::String (static_cast<int> (assets.size())));
    }

    const auto writeBundle = [emitResult, manifestJson, assets] (juce::File manifestFile)
    {
        if (manifestFile == juce::File {})
        {
            emitResult (false, {}, {}, "Save canceled.", 0, 0);
            return;
        }

        if (manifestFile.hasFileExtension (".json") == false
            && manifestFile.hasFileExtension (".smpinst") == false
            && manifestFile.hasFileExtension (".smpinstm") == false)
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

            bool wroteAsset = false;
            if (asset.dataUrl.isNotEmpty())
            {
                juce::MemoryBlock bytes;
                if (decodeDataUrlToMemory (asset.dataUrl, bytes) && bytes.getSize() > 0)
                    wroteAsset = targetFile.replaceWithData (bytes.getData(), bytes.getSize());
            }

            if (! wroteAsset && juce::File::isAbsolutePath (asset.sourcePath))
            {
                const juce::File sourceFile (asset.sourcePath);
                if (sourceFile.existsAsFile())
                {
                    if (sourceFile == targetFile)
                    {
                        wroteAsset = true;
                    }
                    else
                    {
                        if (targetFile.existsAsFile())
                            targetFile.deleteFile();
                        wroteAsset = sourceFile.copyFileTo (targetFile);
                    }
                }
            }

            if (wroteAsset)
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
        audioProcessor.perfLog ("save_instrument_bundle", juce::Time::getMillisecondCounterHiRes() - _t0, "direct-path");
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
            const bool isMonolith = static_cast<bool> (manifestObject->getProperty ("monolith"));
            if (instrumentName.isNotEmpty())
                defaultName = instrumentName + (isMonolith ? ".smpinstm" : ".json");
            else if (isMonolith)
                defaultName = "Instrument.smpinstm";
        }
    }

    saveInstrumentChooser = std::make_unique<juce::FileChooser> ("Save instrument JSON",
                                                                  initialDir.getChildFile (defaultName),
                                                                  "*.json;*.smpinst;*.smpinstm",
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

void SamplePlayerAudioProcessorEditor::handleGraphicDataGetEvent (const juce::var& eventPayload)
{
    if (! webView)
        return;

    int requestId = -1;
    juce::String path;

    if (const auto* object = eventPayload.getDynamicObject())
    {
        requestId = static_cast<int> (std::round (double (object->getProperty ("requestId"))));
        path = object->getProperty ("path").toString().trim();
    }

    auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
    payload->setProperty ("requestId", requestId);
    payload->setProperty ("path", path);

    if (! juce::File::isAbsolutePath (path))
    {
        payload->setProperty ("success", false);
        payload->setProperty ("dataUrl", juce::String());
        payload->setProperty ("message", "Invalid wallpaper path.");
        webView->emitEventIfBrowserIsVisible ("graphic_data_payload", juce::var (payload.get()));
        return;
    }

    // Mirror sample-data transport: encode off the message thread and queue
    // one emit per timer tick so large image payloads do not stall WebView IPC.
    sampleDataRequestPool.addJob ([this, requestId, path]()
    {
        const auto requestStartMs = juce::Time::getMillisecondCounterHiRes();
        const juce::File file (path);
        const auto dataUrl = buildFileDataUrl (file);

        PendingGraphicDataEmit pending;
        pending.requestId = requestId;
        pending.path = path;
        pending.dataUrl = dataUrl;
        pending.message = dataUrl.isNotEmpty() ? juce::String() : juce::String ("Could not load wallpaper.");
        pending.bytesOut = pending.dataUrl.getNumBytesAsUTF8();
        pending.encodingElapsedMs = juce::Time::getMillisecondCounterHiRes() - requestStartMs;

        {
            const juce::ScopedLock lock (pendingGraphicDataEmitLock);
            pendingGraphicDataEmitQueue.push_back (std::move (pending));
        }
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

// ---------------------------------------------------------------------------
//  Native audio file / folder picker handlers
// ---------------------------------------------------------------------------

static bool isAudioFileExtension (const juce::String& ext)
{
    const auto lower = ext.toLowerCase();
    return lower == ".wav" || lower == ".wave" || lower == ".aif" || lower == ".aiff"
        || lower == ".flac" || lower == ".ogg" || lower == ".mp3";
}

static juce::var buildNativeFileListPayload (const juce::Array<juce::File>& files)
{
    juce::Array<juce::var> entries;

    for (const auto& file : files)
    {
        if (! file.existsAsFile())
            continue;
        if (! isAudioFileExtension (file.getFileExtension()))
            continue;

        auto entry = juce::DynamicObject::Ptr (new juce::DynamicObject());
        entry->setProperty ("name", file.getFileName());
        entry->setProperty ("path", file.getFullPathName());
        entry->setProperty ("size", file.getSize());
        entries.add (juce::var (entry.get()));
    }

    auto payload = juce::DynamicObject::Ptr (new juce::DynamicObject());
    payload->setProperty ("files", entries);
    return juce::var (payload.get());
}

void SamplePlayerAudioProcessorEditor::handlePickAudioFilesEvent (const juce::var& /*eventPayload*/)
{
    if (! webView)
        return;

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectMultipleItems;

    audioFileChooser = std::make_unique<juce::FileChooser> (
        "Select audio files",
        juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
        true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    audioFileChooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || safeThis->webView == nullptr)
            return;

        const auto results = chooser.getResults();
        if (results.isEmpty())
        {
            safeThis->audioFileChooser.reset();
            return;
        }

        safeThis->webView->emitEventIfBrowserIsVisible ("native_audio_files_picked",
                                                         buildNativeFileListPayload (results));
        appendUiDebugLog ("native audio files picked | count=" + juce::String (results.size()));
        safeThis->audioFileChooser.reset();
    });
}

void SamplePlayerAudioProcessorEditor::handlePickAudioFolderEvent (const juce::var& /*eventPayload*/)
{
    if (! webView)
        return;

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories;

    audioFileChooser = std::make_unique<juce::FileChooser> (
        "Select folder with audio files",
        juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
        "*",
        true);

    juce::Component::SafePointer<SamplePlayerAudioProcessorEditor> safeThis (this);
    audioFileChooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || safeThis->webView == nullptr)
            return;

        const auto folder = chooser.getResult();
        if (! folder.isDirectory())
        {
            safeThis->audioFileChooser.reset();
            return;
        }

        juce::Array<juce::File> audioFiles;
        for (const auto& entry : juce::RangedDirectoryIterator (folder,
                                                                 true,
                                                                 "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
                                                                 juce::File::findFiles))
        {
            audioFiles.add (entry.getFile());
        }

        if (audioFiles.isEmpty())
        {
            safeThis->audioFileChooser.reset();
            return;
        }

        safeThis->webView->emitEventIfBrowserIsVisible ("native_audio_files_picked",
                                                         buildNativeFileListPayload (audioFiles));
        appendUiDebugLog ("native audio folder picked | folder=" + folder.getFullPathName()
                          + " | fileCount=" + juce::String (audioFiles.size()));
        safeThis->audioFileChooser.reset();
    });
}
