#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
constexpr float velocityScale = 1.0f / 127.0f;
constexpr float autoSamplerInterTakePauseMs = 1000.0f;
constexpr auto kSampleFilePathsProperty = "sampleFilePaths";
constexpr auto kUiSessionStateProperty = "uiSessionStateJson";
constexpr auto kModWheelParamId = "modWheel";
constexpr auto kExpressionParamId = "expression";
constexpr auto kZoneOverridesNode = "ZONE_OVERRIDES";
constexpr auto kZoneNode = "ZONE";
constexpr juce::uint32 kBinaryStateMagic = 0x53505342; // "SPSB"
constexpr int kBinaryStateVersion = 1;
constexpr bool kEnableLoadDebugLogging = true;
constexpr std::int64_t kLoadDebugMaxFileBytes = 4 * 1024 * 1024;

const juce::File& getLoadDebugLogFile()
{
    static const juce::File file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                       .getChildFile ("SamplePlayer")
                                       .getChildFile ("load_debug.log");
    return file;
}

void writeLoadDebugLog (const juce::String& message)
{
    if (! kEnableLoadDebugLogging)
        return;

    const auto timestamp = juce::Time::getCurrentTime().toString (true, true, true, true);
    juce::String line;
    line << timestamp
         << " | "
         << message
         << "\n";

    std::fputs (line.toRawUTF8(), stderr);
    std::fflush (stderr);

    const auto logFile = getLoadDebugLogFile();
    const auto parent = logFile.getParentDirectory();

    if (! parent.isDirectory())
    {
        const auto createResult = parent.createDirectory();
        if (createResult.failed())
        {
            juce::String errorLine;
            errorLine << timestamp
                      << " | load_debug_log create directory failed | path="
                      << parent.getFullPathName()
                      << " | error="
                      << createResult.getErrorMessage()
                      << "\n";
            std::fputs (errorLine.toRawUTF8(), stderr);
            std::fflush (stderr);
            return;
        }
    }

    if (logFile.existsAsFile() && logFile.getSize() > kLoadDebugMaxFileBytes)
    {
        juce::String trimmed;
        trimmed << "=== Sample Player load debug log rotated: "
                << juce::Time::getCurrentTime().toString (true, true, true, true)
                << " ===\n";
        logFile.replaceWithText (trimmed, false, false, "\n");
    }

    logFile.appendText (line, false, false, "\n");
}

double elapsedMsFrom (double startMs)
{
    return juce::Time::getMillisecondCounterHiRes() - startMs;
}

juce::String resolveAutoSamplerDestinationPath (const juce::String& rawPath)
{
    auto path = rawPath.trim();
    if (path.isEmpty())
        return {};

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();

    if (path == "~")
        return home;

    if (path.startsWith ("~/") || path.startsWith ("~\\"))
        return juce::File (home).getChildFile (path.substring (2)).getFullPathName();

    if (path.startsWithIgnoreCase ("/Users/you/"))
        return home + path.substring (10);

    return path;
}

juce::String normalizePathSlashes (juce::String path)
{
    return path.replaceCharacter ('\\', '/');
}

juce::String getStandaloneStartupAutoLoadPath()
{
    if (! juce::JUCEApplicationBase::isStandaloneApp())
        return {};

    juce::String requestedPath;
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();

    for (int i = 0; i < args.size(); ++i)
    {
        const auto arg = args[i].trim();

        if (arg.startsWithIgnoreCase ("--autoload-manifest="))
        {
            requestedPath = arg.fromFirstOccurrenceOf ("=", false, false).trim();
            break;
        }

        if (arg.equalsIgnoreCase ("--autoload-manifest") && i + 1 < args.size())
        {
            requestedPath = args[i + 1].trim();
            break;
        }
    }

    if (requestedPath.isEmpty())
        requestedPath = juce::SystemStats::getEnvironmentVariable ("SAMPLEPLAYER_AUTOLOAD_MANIFEST", {}).trim();

    requestedPath = requestedPath.unquoted().trim();
    if (requestedPath.isEmpty())
        return {};

    const auto requestedFile = juce::File::isAbsolutePath (requestedPath)
                                 ? juce::File (requestedPath)
                                 : juce::File::getCurrentWorkingDirectory().getChildFile (requestedPath);

    return requestedFile.getFullPathName();
}

juce::String extractFileNameFromPathString (juce::String path)
{
    path = normalizePathSlashes (path.trim());

    while (path.endsWithChar ('/'))
        path = path.dropLastCharacters (1);

    if (path.isEmpty())
        return {};

    const auto slashIndex = path.lastIndexOfChar ('/');
    return slashIndex >= 0 ? path.substring (slashIndex + 1) : path;
}

juce::String sanitizeKeyswitchKeyToken (const juce::String& rawKey)
{
    auto key = rawKey.trim().toUpperCase();
    if (key.isEmpty())
        return "C0";

    const auto parsed = key;
    if (parsed.length() >= 2)
        return juce::File::createLegalFileName (parsed);

    return "C0";
}

bool parseStrictInt (const juce::String& text, int& value)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty())
        return false;

    auto ptr = trimmed.getCharPointer();
    bool hasDigit = false;

    for (int i = 0; i < trimmed.length(); ++i)
    {
        const auto c = ptr[i];

        if (i == 0 && (c == '+' || c == '-'))
            continue;

        if (! juce::CharacterFunctions::isDigit (c))
            return false;

        hasDigit = true;
    }

    if (! hasDigit)
        return false;

    value = trimmed.getIntValue();
    return true;
}

int msToSamples (double sampleRate, float timeMs)
{
    return juce::jmax (0, static_cast<int> (std::round (sampleRate * 0.001 * static_cast<double> (timeMs))));
}

int velocityToLayerFromVelocity (int velocity127, int totalLayers)
{
    const int safeLayers = juce::jlimit (1, 5, totalLayers);
    const int v0 = juce::jlimit (0, 127, velocity127 - 1);
    return 1 + ((v0 * safeLayers) / 128);
}

struct VelocityLayerBlendState
{
    float gain = 1.0f;
    double delaySec = 0.0;
};

double computeVelocityLayerBlendJitter (juce::uint64 age, int velocityLayer, int roundRobinIndex)
{
    const auto seed = age
                    ^ (static_cast<juce::uint64> (velocityLayer) << 17)
                    ^ (static_cast<juce::uint64> (roundRobinIndex) << 33);
    const auto hashed = (seed * 0x9E3779B97F4A7C15ULL) ^ (seed >> 23);
    const auto normalized = static_cast<double> (hashed & 0xffffULL) / 65535.0;
    return (normalized * 2.0) - 1.0;
}

VelocityLayerBlendState computeVelocityCrossfadeBlendState (const std::vector<int>& layers,
                                                            int currentVelocityLayer,
                                                            int selectionVelocity,
                                                            int crossfadePercent,
                                                            double jitterSigned)
{
    VelocityLayerBlendState state {};

    if (layers.empty())
    {
        state.gain = 0.0f;
        return state;
    }

    if (layers.size() == 1)
    {
        state.gain = layers.front() == currentVelocityLayer ? 1.0f : 0.0f;
        return state;
    }

    const auto layerIt = std::find (layers.begin(), layers.end(), currentVelocityLayer);
    if (layerIt == layers.end())
    {
        state.gain = 0.0f;
        return state;
    }

    const int layerCount = static_cast<int> (layers.size());
    const int velocity127 = juce::jlimit (1, 127, selectionVelocity);
    const int baseIndex = juce::jmin (layerCount - 1,
                                      ((velocity127 - 1) * layerCount) / 127);
    const float xfadePct = juce::jlimit (0.0f, 1.0f, static_cast<float> (crossfadePercent) / 100.0f);

    if (xfadePct <= 0.0001f)
    {
        state.gain = layers[static_cast<size_t> (baseIndex)] == currentVelocityLayer ? 1.0f : 0.0f;
        return state;
    }

    const float layerRange = 127.0f / static_cast<float> (layerCount);
    const float halfWidth = juce::jmax (0.5f, layerRange * xfadePct * 0.5f);

    for (int split = 1; split < layerCount; ++split)
    {
        const float splitVelocity = static_cast<float> (std::ceil ((static_cast<float> (split) * 127.0f)
                                                                   / static_cast<float> (layerCount))) + 1.0f;
        const float center = splitVelocity - 0.5f;
        const float zoneStart = center - halfWidth;
        const float zoneEnd = center + halfWidth;

        if (static_cast<float> (velocity127) < zoneStart || static_cast<float> (velocity127) > zoneEnd)
            continue;

        const float t = juce::jlimit (0.0f, 1.0f,
                                      (static_cast<float> (velocity127) - zoneStart)
                                          / juce::jmax (0.0001f, zoneEnd - zoneStart));
        const float lowerGain = juce::jmax (0.0f, 1.0f - t);
        const float upperGain = juce::jmax (0.0f, t);
        const int lowerLayer = layers[static_cast<size_t> (split - 1)];
        const int upperLayer = layers[static_cast<size_t> (split)];

        const float blendCenterWeight = 1.0f - std::abs ((t * 2.0f) - 1.0f);
        const double baseDelaySec = 0.00045 + (static_cast<double> (blendCenterWeight) * 0.00035);
        const double jitterSec = jitterSigned * 0.00006;
        const double delaySec = juce::jlimit (0.0002, 0.0012, baseDelaySec + jitterSec);

        if (currentVelocityLayer == lowerLayer)
        {
            state.gain = lowerGain;
            state.delaySec = lowerGain <= upperGain ? delaySec : 0.0;
            return state;
        }

        if (currentVelocityLayer == upperLayer)
        {
            state.gain = upperGain;
            state.delaySec = upperGain < lowerGain ? delaySec : 0.0;
            return state;
        }

        state.gain = 0.0f;
        return state;
    }

    state.gain = layers[static_cast<size_t> (baseIndex)] == currentVelocityLayer ? 1.0f : 0.0f;
    return state;
}

int varToInt (const juce::var& value, int fallback = 0)
{
    if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
        return static_cast<int> (std::round (double (value)));

    if (value.isString())
    {
        int parsed = 0;
        if (parseStrictInt (value.toString(), parsed))
            return parsed;
    }

    return fallback;
}

double varToDouble (const juce::var& value, double fallback = 0.0)
{
    if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
        return double (value);

    if (value.isString())
    {
        const auto parsed = value.toString().trim().getDoubleValue();
        if (std::isfinite (parsed))
            return parsed;
    }

    return fallback;
}

juce::String normalizeKeyswitchPlaybackMode (juce::String value)
{
    value = value.trim().toLowerCase();
    return value == "oneshot" ? juce::String ("oneshot") : juce::String ("spread");
}

enum class KeyswitchTriggerMode
{
    manual,
    noteOn,
    noteOff
};

KeyswitchTriggerMode getKeyswitchTriggerModeFromName (juce::String value)
{
    value = value.trim().toLowerCase().removeCharacters (" _-");

    if (value == "on" || value == "noteon" || value == "keyon")
        return KeyswitchTriggerMode::noteOn;

    if (value == "off" || value == "noteoff" || value == "keyoff")
        return KeyswitchTriggerMode::noteOff;

    return KeyswitchTriggerMode::manual;
}

bool resolveKeyswitchOneShotPlayback (const juce::String& name, const juce::String& playbackMode)
{
    return getKeyswitchTriggerModeFromName (name) != KeyswitchTriggerMode::manual
        || normalizeKeyswitchPlaybackMode (playbackMode) == "oneshot";
}

bool decodeDataUrlAudioToMemory (const juce::String& dataUrl, juce::MemoryBlock& output)
{
    const auto trimmed = dataUrl.trim();

    if (! trimmed.startsWithIgnoreCase ("data:"))
        return false;

    const auto comma = trimmed.indexOfChar (',');
    if (comma <= 0)
        return false;

    const auto header = trimmed.substring (0, comma).toLowerCase();
    if (! header.contains (";base64"))
        return false;

    const auto base64Payload = trimmed.substring (comma + 1)
                                   .removeCharacters (" \n\r\t");
    output.reset();
    juce::MemoryOutputStream decoded (output, false);
    return juce::Base64::convertFromBase64 (decoded, base64Payload);
}

juce::String encodeAudioBufferToWavDataUrl (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const int sourceChannels = audio.getNumChannels();
    const int numChannels = juce::jlimit (1, 2, sourceChannels);
    const int numSamples = audio.getNumSamples();
    const double safeSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    if (sourceChannels <= 0 || numSamples <= 0)
        return {};

    constexpr int bitsPerSample = 16;
    const int bytesPerSample = bitsPerSample / 8;
    const int blockAlign = numChannels * bytesPerSample;
    const int byteRate = static_cast<int> (std::round (safeSampleRate * static_cast<double> (blockAlign)));
    const int dataBytes = numSamples * blockAlign;
    const int riffSize = 36 + dataBytes;

    juce::MemoryOutputStream out;

    const auto writeU16 = [&out] (juce::uint16 value)
    {
        out.writeByte (static_cast<char> (value & 0xff));
        out.writeByte (static_cast<char> ((value >> 8) & 0xff));
    };

    const auto writeU32 = [&out] (juce::uint32 value)
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
    writeU32 (static_cast<juce::uint32> (juce::jmax (1, static_cast<int> (std::round (safeSampleRate)))));
    writeU32 (static_cast<juce::uint32> (juce::jmax (blockAlign, byteRate)));
    writeU16 (static_cast<juce::uint16> (blockAlign));
    writeU16 (bitsPerSample);
    out.write ("data", 4);
    writeU32 (static_cast<juce::uint32> (dataBytes));

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const int sourceChannel = juce::jmin (ch, sourceChannels - 1);
            const float sample = juce::jlimit (-1.0f, 1.0f, audio.getSample (sourceChannel, i));
            const int scaled = static_cast<int> (std::round (sample * 32767.0f));
            const auto pcm = static_cast<juce::int16> (juce::jlimit (-32768, 32767, scaled));
            writeU16 (static_cast<juce::uint16> (static_cast<juce::uint16> (pcm)));
        }
    }

    const auto base64 = juce::Base64::toBase64 (out.getData(), out.getDataSize());
    return "data:audio/wav;base64," + base64;
}

bool writeAudioBufferToWavFile (const juce::AudioBuffer<float>& audio,
                                double sampleRate,
                                const juce::File& targetFile)
{
    if (audio.getNumChannels() <= 0 || audio.getNumSamples() <= 0)
        return false;

    const auto parent = targetFile.getParentDirectory();
    if (! parent.isDirectory())
    {
        const auto createResult = parent.createDirectory();
        if (createResult.failed())
            return false;
    }

    auto outputStream = std::unique_ptr<juce::FileOutputStream> (targetFile.createOutputStream());
    if (outputStream == nullptr || ! outputStream->openedOk())
        return false;

    juce::WavAudioFormat wavFormat;
    auto writer = std::unique_ptr<juce::AudioFormatWriter> (
        wavFormat.createWriterFor (outputStream.get(),
                                   sampleRate > 0.0 ? sampleRate : 44100.0,
                                   static_cast<unsigned int> (juce::jlimit (1, 2, audio.getNumChannels())),
                                   16,
                                   {},
                                   0));

    if (writer == nullptr)
        return false;

    outputStream.release(); // Writer owns stream.
    const bool ok = writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    writer.reset();
    return ok && targetFile.existsAsFile() && targetFile.getSize() > 44;
}

bool sessionJsonHasEmbeddedSampleData (const juce::String& sessionJson)
{
    return sessionJson.containsIgnoreCase ("\"sampleDataUrl\"");
}

void hashMix (juce::uint64& hash, juce::uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

juce::String hashToSignature (juce::uint64 hash, int zoneCount)
{
    return juce::String::toHexString (static_cast<juce::int64> (hash)) + ":" + juce::String (zoneCount);
}

struct LightweightStripStats
{
    int sampleDataUrlsRemoved = 0;
    int dataUrlsRemoved = 0;
};

void stripLargePayloadFieldsRecursive (juce::var& value, LightweightStripStats& stats)
{
    if (auto* object = value.getDynamicObject())
    {
        const auto sampleData = object->getProperty ("sampleDataUrl").toString();
        if (sampleData.isNotEmpty())
        {
            object->setProperty ("hasEmbeddedSampleData", true);
            ++stats.sampleDataUrlsRemoved;
        }
        object->removeProperty ("sampleDataUrl");

        const auto dataUrl = object->getProperty ("dataUrl").toString();
        if (dataUrl.isNotEmpty())
        {
            object->setProperty ("hasEmbeddedData", true);
            ++stats.dataUrlsRemoved;
        }
        object->removeProperty ("dataUrl");

        juce::Array<juce::Identifier> keys;
        const auto& props = object->getProperties();
        keys.ensureStorageAllocated (props.size());
        for (int i = 0; i < props.size(); ++i)
            keys.add (props.getName (i));

        for (const auto& key : keys)
        {
            auto child = object->getProperty (key);
            const auto childString = child.toString();
            const bool looksLikeInlineDataUrl = childString.startsWithIgnoreCase ("data:")
                                             && childString.length() > 64;

            if (looksLikeInlineDataUrl)
            {
                const auto keyText = key.toString().toLowerCase();
                const bool sampleLike = keyText.contains ("sample") || keyText.contains ("audio");
                if (sampleLike)
                {
                    object->setProperty ("hasEmbeddedSampleData", true);
                    ++stats.sampleDataUrlsRemoved;
                }
                else
                {
                    object->setProperty ("hasEmbeddedData", true);
                    ++stats.dataUrlsRemoved;
                }

                object->removeProperty (key);
                continue;
            }

            stripLargePayloadFieldsRecursive (child, stats);
            object->setProperty (key, child);
        }

        return;
    }

    if (auto* array = value.getArray())
    {
        for (auto& item : *array)
            stripLargePayloadFieldsRecursive (item, stats);
    }
}

juce::String makeLightweightSessionStateJson (const juce::String& fullJson,
                                              LightweightStripStats* outStats = nullptr)
{
    const auto trimmed = fullJson.trim();
    if (trimmed.isEmpty())
        return {};

    auto parsed = juce::JSON::parse (trimmed);
    if (parsed.isVoid())
        return fullJson;

    LightweightStripStats stats;
    stripLargePayloadFieldsRecursive (parsed, stats);

    if (auto* rootObject = parsed.getDynamicObject())
    {
        if (auto* manifestObj = rootObject->getProperty ("manifest").getDynamicObject())
        {
            manifestObj->removeProperty ("entries");
            manifestObj->removeProperty ("variants");
        }
    }

    if (outStats != nullptr)
        *outStats = stats;

    // Compact json keeps payload smaller and faster to generate.
    return juce::JSON::toString (parsed, false);
}
} // namespace

SamplePlayerAudioProcessor::SamplePlayerAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, juce::Identifier ("SamplePlayer"), createParameterLayout())
{
    formatManager.registerBasicFormats();

    auto initialSet = std::make_shared<SampleSet>();
    initialSet->keyswitchSlotByMidi.fill (-1);
    initialSet->summary = "No samples loaded.\n\n" + getZoneNamingHint();
    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (initialSet));

    auto initialSequencerRuntime = std::make_shared<StepSequencerRuntime>();
    std::atomic_store (&stepSequencerRuntime, initialSequencerRuntime);
    auto initialStrumRuntime = std::make_shared<StepSequencerRuntime>();
    std::atomic_store (&strumSequencerRuntime, initialStrumRuntime);
    sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);

    pendingStandaloneStartupAutoLoadPath = getStandaloneStartupAutoLoadPath();
    if (pendingStandaloneStartupAutoLoadPath.isNotEmpty())
        writeLoadDebugLog ("processor startup auto-load armed | path=" + pendingStandaloneStartupAutoLoadPath);
}

SamplePlayerAudioProcessor::~SamplePlayerAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout SamplePlayerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "outputGainDb", 1 },
        "Output",
        juce::NormalisableRange<float> (-48.0f, 12.0f, 0.1f),
        -3.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " dB";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "attackMs", 1 },
        "Attack",
        juce::NormalisableRange<float> (0.0f, 5000.0f, 0.1f, 0.35f),
        5.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " ms";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "decayMs", 1 },
        "Decay",
        juce::NormalisableRange<float> (0.0f, 5000.0f, 0.1f, 0.35f),
        250.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " ms";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "sustain", 1 },
        "Sustain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        1.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value * 100.0f, 1) + " %";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "releaseMs", 1 },
        "Release",
        juce::NormalisableRange<float> (0.0f, 7000.0f, 0.1f, 0.35f),
        2000.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " ms";
        }));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "loopEnabled", 1 },
        "Loop Enable",
        true));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "loopStartPct", 1 },
        "Loop Start",
        juce::NormalisableRange<float> (0.0f, 99.0f, 0.1f),
        5.0f,
        "%",
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " %";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "loopEndPct", 1 },
        "Loop End",
        juce::NormalisableRange<float> (1.0f, 100.0f, 0.1f),
        95.0f,
        "%",
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " %";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "loopCrossfadeMs", 1 },
        "Loop Crossfade",
        juce::NormalisableRange<float> (0.0f, 250.0f, 0.1f, 0.5f),
        15.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " ms";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kModWheelParamId, 1 },
        "Mod Wheel",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        0.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value * 100.0f, 1) + " %";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kExpressionParamId, 1 },
        "Expression",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        0.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value * 100.0f, 1) + " %";
        }));

    return layout;
}

const juce::String SamplePlayerAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SamplePlayerAudioProcessor::acceptsMidi() const
{
    return true;
}

bool SamplePlayerAudioProcessor::producesMidi() const
{
    return true;
}

bool SamplePlayerAudioProcessor::isMidiEffect() const
{
    return false;
}

double SamplePlayerAudioProcessor::getTailLengthSeconds() const
{
    return std::numeric_limits<double>::infinity();
}

int SamplePlayerAudioProcessor::getNumPrograms()
{
    return 1;
}

int SamplePlayerAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SamplePlayerAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String SamplePlayerAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void SamplePlayerAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void SamplePlayerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    maybeRunStandaloneStartupAutoLoad();

    stopAllVoices();
    roundRobinCounters.clear();

    const juce::ScopedLock lock (autoSamplerLock);
    activeAutoCaptures.clear();
    triggeredAutoCaptures.clear();
    completedAutoCaptures.clear();
    autoSamplerPendingNoteEvents.clear();
    autoSamplerFallbackRrCounters.clear();
    autoSamplerMidiSchedule.clear();
    autoSamplerMidiScheduleIndex = 0;
    autoSamplerTimelineSample = 0;
    autoSamplerStartWallMs = 0.0;
    autoSamplerHeldNotes.fill (false);
    autoSamplerSendAllNotesOff = false;
    autoSamplerInputHistory[0].clear();
    autoSamplerInputHistory[1].clear();
    autoSamplerHistoryWrite = 0;
    autoSamplerHistoryValid = 0;
    autoSamplerHistorySize = 0;
    autoSamplerInputDetected = false;
}

void SamplePlayerAudioProcessor::releaseResources()
{
}

bool SamplePlayerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    const auto input = layouts.getMainInputChannelSet();
    if (! input.isDisabled()
        && input != juce::AudioChannelSet::mono()
        && input != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void SamplePlayerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    if (const auto* modParam = parameters.getRawParameterValue (kModWheelParamId))
        modwheelVelocityLayerControlValue01.store (juce::jlimit (0.0f, 1.0f, modParam->load()), std::memory_order_relaxed);
    if (const auto* expressionParam = parameters.getRawParameterValue (kExpressionParamId))
        expressionControllerValue01.store (juce::jlimit (0.0f, 1.0f, expressionParam->load()), std::memory_order_relaxed);

    if (buffer.getNumSamples() <= 0)
    {
        midiMessages.clear();
        return;
    }

    if (resetVoicesRequested.exchange (false))
    {
        stopAllVoices();
        roundRobinCounters.clear();
    }

    juce::MidiBuffer incomingMidi;
    incomingMidi.swapWith (midiMessages);

    juce::MidiBuffer transposedIncomingMidi;
    for (const auto metadata : incomingMidi)
    {
        auto message = metadata.getMessage();
        if (message.isNoteOnOrOff() || message.isAftertouch())
        {
            const int transposedNote = juce::jlimit (0, 127, message.getNoteNumber() - 12);
            message.setNoteNumber (transposedNote);
        }
        transposedIncomingMidi.addEvent (message, metadata.samplePosition);
    }

    juce::MidiBuffer generatedMidi;
    appendAutoSamplerMidiOutput (generatedMidi, buffer.getNumSamples());

    juce::MidiBuffer renderMidi = transposedIncomingMidi;
    renderMidi.addEvents (generatedMidi, 0, buffer.getNumSamples(), 0);
    const auto settings = getBlockSettingsSnapshot();

    if (auto runtime = std::atomic_load (&strumSequencerRuntime);
        runtime != nullptr && runtime->enabled)
    {
        bool hasHeldTrigger = false;
        for (int note = 0; note <= 127; ++note)
        {
            if (runtime->triggerDepthByMidi[static_cast<size_t> (note)] > 0)
            {
                hasHeldTrigger = true;
                break;
            }
        }

        if (hasHeldTrigger)
        {
            const auto subdivisionCountForRate = [] (int rateIndex) -> int
            {
                if (rateIndex == 0) return 1;
                if (rateIndex == 1) return 2;
                if (rateIndex == 2) return 4;
                if (rateIndex == 3) return 8;
                return 0;
            };

            const auto nextRandom01 = [&runtime]() -> float
            {
                runtime->randomState = runtime->randomState * 1664525u + 1013904223u;
                const auto sample = (runtime->randomState >> 8) & 0x00ffffffu;
                return static_cast<float> (sample) / static_cast<float> (0x00ffffffu);
            };

            const auto nextRandomSigned = [&nextRandom01]() -> float
            {
                return (nextRandom01() * 2.0f) - 1.0f;
            };

            const int quarterSamples = juce::jmax (1, static_cast<int> (currentSampleRate * 0.5));
            if (runtime->currentStep < 0 || runtime->currentStep >= static_cast<int> (runtime->steps.size()))
            {
                runtime->currentStep = 0;
                runtime->currentSubdivision = 0;
            }

            const auto advanceSubdivisionState = [&]() -> int
            {
                const auto& step = runtime->steps[static_cast<size_t> (runtime->currentStep)];
                const int subdivisions = subdivisionCountForRate (step.rateIndex);
                const int interval = juce::jmax (1, quarterSamples / juce::jmax (1, subdivisions));

                if (subdivisions <= 0)
                {
                    runtime->currentSubdivision = 0;
                    runtime->currentStep = (runtime->currentStep + 1) % static_cast<int> (runtime->steps.size());
                    return quarterSamples;
                }

                ++runtime->currentSubdivision;
                if (runtime->currentSubdivision >= subdivisions)
                {
                    runtime->currentSubdivision = 0;
                    runtime->currentStep = (runtime->currentStep + 1) % static_cast<int> (runtime->steps.size());
                }
                return interval;
            };

            int nextTickSample = runtime->samplesUntilNextSubstep;
            while (nextTickSample < buffer.getNumSamples())
            {
                const auto& step = runtime->steps[static_cast<size_t> (runtime->currentStep)];
                const int subdivisions = subdivisionCountForRate (step.rateIndex);

                if (step.keyswitchSlot >= 0)
                {
                    activeMapSetSlot.store (step.keyswitchSlot, std::memory_order_relaxed);
                    pendingActiveMapSetSlotFromMidi.store (step.keyswitchSlot, std::memory_order_relaxed);
                }

                if (subdivisions > 0)
                {
                    const int subIndex = juce::jlimit (0, subdivisions - 1, runtime->currentSubdivision);
                    int velocity127 = juce::jlimit (1, 127, step.subVelocities[static_cast<size_t> (subIndex)]);
                    if (runtime->velocityHumanizePercent > 0)
                    {
                        const float depth = static_cast<float> (runtime->velocityHumanizePercent) * 0.01f;
                        const float jitter = nextRandomSigned() * depth;
                        const float scaled = static_cast<float> (velocity127) * (1.0f + jitter);
                        velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (scaled)));
                    }

                    const float velocity01 = static_cast<float> (velocity127) / 127.0f;

                    for (int note = 0; note <= 127; ++note)
                    {
                        if (runtime->triggerDepthByMidi[static_cast<size_t> (note)] <= 0)
                            continue;

                        const int channel = juce::jlimit (1, 16, runtime->triggerChannelByMidi[static_cast<size_t> (note)]);
                        const int stepPlaybackSlot = step.keyswitchSlot >= 0
                            ? step.keyswitchSlot
                            : juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));
                        BlockSettings stepSettings = settings;

                        if (step.keyswitchSlot >= 0)
                        {
                            bool loopEnabled = true;
                            const auto currentSet = std::atomic_load (&currentSampleSet);
                            if (currentSet != nullptr)
                            {
                                if (const auto loopIt = currentSet->loopPlaybackBySlot.find (step.keyswitchSlot);
                                    loopIt != currentSet->loopPlaybackBySlot.end())
                                {
                                    loopEnabled = loopIt->second;
                                }
                            }
                            activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
                            stepSettings.loopEnabled = loopEnabled;
                        }

                        const bool canDouble = runtime->doubling;
                        if (canDouble)
                        {
                            const auto leftZone = startVoiceForNoteInternal (channel,
                                                                             note,
                                                                             velocity01,
                                                                             stepSettings,
                                                                             false,
                                                                             -1.0f,
                                                                             0,
                                                                             nullptr,
                                                                             stepPlaybackSlot);
                            startVoiceForNoteInternal (channel,
                                                       note,
                                                       velocity01,
                                                       stepSettings,
                                                       true,
                                                       1.0f,
                                                       1,
                                                       leftZone.get(),
                                                       stepPlaybackSlot);
                        }
                        else
                        {
                            startVoiceForNoteInternal (channel,
                                                       note,
                                                       velocity01,
                                                       stepSettings,
                                                       false,
                                                       0.0f,
                                                       0,
                                                       nullptr,
                                                       stepPlaybackSlot);
                        }
                    }
                }

                int nextInterval = advanceSubdivisionState();
                if (runtime->swingPercent > 0)
                {
                    const auto& currentStepState = runtime->steps[static_cast<size_t> (runtime->currentStep)];
                    const int currentSubdivisions = subdivisionCountForRate (currentStepState.rateIndex);
                    if (currentSubdivisions > 1)
                    {
                        const float swingDepth = static_cast<float> (runtime->swingPercent) * 0.01f * 0.5f;
                        if ((runtime->currentSubdivision & 1) != 0)
                            nextInterval = static_cast<int> (std::round (static_cast<float> (nextInterval) * (1.0f + swingDepth)));
                        else
                            nextInterval = static_cast<int> (std::round (static_cast<float> (nextInterval) * (1.0f - swingDepth)));
                        nextInterval = juce::jmax (1, nextInterval);
                    }
                }

                if (runtime->timingHumanizeMs > 0)
                {
                    const int jitterSamples = juce::jmax (0,
                        msToSamples (currentSampleRate, static_cast<float> (runtime->timingHumanizeMs)));
                    if (jitterSamples > 0)
                    {
                        const int jitter = static_cast<int> (std::round (nextRandomSigned() * static_cast<float> (jitterSamples)));
                        nextInterval = juce::jmax (1, nextInterval + jitter);
                    }
                }

                nextTickSample += nextInterval;
            }

            runtime->samplesUntilNextSubstep = nextTickSample - buffer.getNumSamples();
            runtime->samplesUntilNextStep = runtime->samplesUntilNextSubstep;
        }
        else
        {
            runtime->samplesUntilNextStep = 0;
            runtime->samplesUntilNextSubstep = 0;
            runtime->currentStep = -1;
            runtime->currentSubdivision = 0;
        }
    }

    // Step sequencer ratchet tick handler: fire remaining subdivisions after note-on
    if (auto stepRT = std::atomic_load (&stepSequencerRuntime);
        stepRT != nullptr && stepRT->enabled && stepRT->ratchetSubsRemaining > 0)
    {
        const auto subdivisionCountForRateStep = [] (int rateIndex) -> int
        {
            if (rateIndex == 0) return 1;
            if (rateIndex == 1) return 2;
            if (rateIndex == 2) return 4;
            if (rateIndex == 3) return 8;
            return 0;
        };

        int nextTickSample = stepRT->samplesUntilNextSubstep;
        while (nextTickSample < buffer.getNumSamples() && stepRT->ratchetSubsRemaining > 0)
        {
            --stepRT->ratchetSubsRemaining;
            const int playedNote = stepRT->ratchetNote;
            const int velocity127 = juce::jlimit (1, 127, stepRT->ratchetVelocity127);
            const float velocity01 = static_cast<float> (velocity127) / 127.0f;
            const int channel = stepRT->ratchetChannel;
            const int stepPlaybackSlot = stepRT->ratchetPlaybackSlot;

            BlockSettings stepSettings = settings;
            if (stepRT->ratchetKeyswitchSlot >= 0)
            {
                bool loopEnabled = true;
                const auto currentSet = std::atomic_load (&currentSampleSet);
                if (currentSet != nullptr)
                {
                    if (const auto loopIt = currentSet->loopPlaybackBySlot.find (stepRT->ratchetKeyswitchSlot);
                        loopIt != currentSet->loopPlaybackBySlot.end())
                    {
                        loopEnabled = loopIt->second;
                    }
                }
                stepSettings.loopEnabled = loopEnabled;
            }

            const bool canDouble = sequencerDoublingEnabled.load (std::memory_order_relaxed)
                                || stepRT->doubling;
            if (canDouble)
            {
                const auto leftZone = startVoiceForNoteInternal (channel,
                                                                 playedNote,
                                                                 velocity01,
                                                                 stepSettings,
                                                                 false,
                                                                 -1.0f,
                                                                 0,
                                                                 nullptr,
                                                                 stepPlaybackSlot);
                startVoiceForNoteInternal (channel,
                                           playedNote,
                                           velocity01,
                                           stepSettings,
                                           true,
                                           1.0f,
                                           1,
                                           leftZone.get(),
                                           stepPlaybackSlot);
            }
            else
            {
                startVoiceForNoteInternal (channel,
                                           playedNote,
                                           velocity01,
                                           stepSettings,
                                           false,
                                           0.0f,
                                           0,
                                           nullptr,
                                           stepPlaybackSlot);
            }

            // Compute interval for next ratchet tick
            const int stepIdx = juce::jlimit (0, static_cast<int> (stepRT->steps.size()) - 1,
                                              juce::jmax (0, stepRT->currentStep));
            const int subdivisions = subdivisionCountForRateStep (stepRT->steps[static_cast<size_t> (stepIdx)].rateIndex);
            const int interval = juce::jmax (1,
                static_cast<int> ((currentSampleRate * 0.5) / static_cast<double> (juce::jmax (1, subdivisions))));
            nextTickSample += interval;
        }

        stepRT->samplesUntilNextSubstep = nextTickSample - buffer.getNumSamples();
    }

    auto inputBuffer = getBusBuffer (buffer, true, 0);
    auto outputBuffer = getBusBuffer (buffer, false, 0);
    const bool inputHasChannels = inputBuffer.getNumChannels() > 0;
    const bool outputHasChannels = outputBuffer.getNumChannels() > 0;
    const bool captureFromOutputFallback = (! inputHasChannels) && outputHasChannels;

    const juce::AudioBuffer<float>* captureSourceBuffer = captureFromOutputFallback ? &outputBuffer
                                                                                     : &inputBuffer;
    const juce::AudioBuffer<float>* monitorSourceBuffer = inputHasChannels ? &inputBuffer : nullptr;

    // Capture only the internally scheduled autosampler notes so host/user MIDI
    // on this track can't shift RR counters or remap capture roots.
    // When using output fallback, capture after rendering so generated notes are present.
    if (! captureFromOutputFallback)
        processAutoSamplerCapture (*captureSourceBuffer, generatedMidi);

    bool keepAutoSamplerAwake = false;
    {
        const juce::ScopedLock lock (autoSamplerLock);
        keepAutoSamplerAwake = autoSamplerActive;
    }

    juce::AudioBuffer<float> monitorBuffer;
    if (keepAutoSamplerAwake
        && monitorSourceBuffer != nullptr
        && monitorSourceBuffer->getNumChannels() > 0
        && monitorSourceBuffer->getNumSamples() > 0)
    {
        const int monitorChannels = juce::jmax (1, juce::jmin (2, monitorSourceBuffer->getNumChannels()));
        monitorBuffer.setSize (monitorChannels, monitorSourceBuffer->getNumSamples(), false, false, true);
        for (int ch = 0; ch < monitorChannels; ++ch)
            monitorBuffer.copyFrom (ch, 0, *monitorSourceBuffer, ch, 0, monitorSourceBuffer->getNumSamples());
    }
    outputBuffer.clear();

    if (keepAutoSamplerAwake
        && monitorBuffer.getNumChannels() > 0
        && outputBuffer.getNumChannels() > 0
        && outputBuffer.getNumSamples() > 0)
    {
        const int monitorSamples = juce::jmin (outputBuffer.getNumSamples(), monitorBuffer.getNumSamples());
        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
        {
            const int sourceChannel = juce::jmin (ch, monitorBuffer.getNumChannels() - 1);
            outputBuffer.addFrom (ch, 0, monitorBuffer, sourceChannel, 0, monitorSamples, 1.0f);
        }
    }

    std::vector<PendingPreviewMidiEvent> previewMidiEvents;
    {
        const juce::ScopedLock lock (pendingPreviewMidiLock);
        previewMidiEvents.swap (pendingPreviewMidiEvents);
    }

    for (const auto& event : previewMidiEvents)
    {
        const int midiChannel = juce::jlimit (1, 16, event.midiChannel);
        if (event.isController)
        {
            const int controller = juce::jlimit (0, 127, event.controllerNumber);
            const int value = juce::jlimit (0, 127, event.controllerValue);
            handleMidiMessage (juce::MidiMessage::controllerEvent (midiChannel, controller, value), settings, true);
            continue;
        }

        const int midiNote = juce::jlimit (0, 127, event.midiNote);
        const int velocity127 = juce::jlimit (1, 127, event.velocity127);

        if (event.noteOn)
        {
            const auto velocity01 = static_cast<float> (velocity127) / 127.0f;
            handleMidiMessage (juce::MidiMessage::noteOn (midiChannel, midiNote, velocity01), settings, true);
        }
        else
        {
            handleMidiMessage (juce::MidiMessage::noteOff (midiChannel, midiNote), settings, true);
        }
    }

    int renderStart = 0;

    for (auto iter = renderMidi.begin(); iter != renderMidi.end();)
    {
        const int rawSamplePosition = (*iter).samplePosition;
        const auto eventSample = juce::jlimit (0, buffer.getNumSamples(), rawSamplePosition);

        if (eventSample > renderStart)
            renderVoices (outputBuffer, renderStart, eventSample - renderStart, settings);

        std::vector<juce::MidiMessage> keyswitchMessages;
        std::vector<juce::MidiMessage> regularMessages;
        const auto sampleSet = std::atomic_load (&currentSampleSet);

        while (iter != renderMidi.end() && (*iter).samplePosition == rawSamplePosition)
        {
            const auto message = (*iter).getMessage();
            bool prioritizeKeyswitch = false;

            if (message.isNoteOn() && sampleSet != nullptr)
            {
                const int note = juce::jlimit (0, 127, message.getNoteNumber());
                const int keyswitchSlot = sampleSet->keyswitchSlotByMidi[static_cast<size_t> (note)];
                prioritizeKeyswitch = keyswitchSlot >= 0
                                   || (sampleSet->hasKeyswitchSets && note <= 24);
            }

            if (prioritizeKeyswitch)
                keyswitchMessages.push_back (message);
            else
                regularMessages.push_back (message);

            ++iter;
        }

        for (const auto& message : keyswitchMessages)
            handleMidiMessage (message, settings, false);
        for (const auto& message : regularMessages)
            handleMidiMessage (message, settings, false);

        renderStart = eventSample;
    }

    if (renderStart < outputBuffer.getNumSamples())
        renderVoices (outputBuffer, renderStart, outputBuffer.getNumSamples() - renderStart, settings);

    if (captureFromOutputFallback)
        processAutoSamplerCapture (outputBuffer, generatedMidi);

    if (keepAutoSamplerAwake && outputBuffer.getNumChannels() > 0 && outputBuffer.getNumSamples() > 0)
    {
        // Some hosts suspend effectively silent synths even while they are generating MIDI.
        // Use a tiny alternating signal across the whole block so autosampler scheduling keeps advancing.
        constexpr float keepAliveLevel = 1.0e-6f;
        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
        {
            for (int sample = 0; sample < outputBuffer.getNumSamples(); ++sample)
            {
                const auto polarity = (sample & 1) == 0 ? keepAliveLevel : -keepAliveLevel;
                outputBuffer.addSample (ch, sample, polarity);
            }
        }
    }

    midiMessages.swapWith (renderMidi);
}

bool SamplePlayerAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* SamplePlayerAudioProcessor::createEditor()
{
    maybeRunStandaloneStartupAutoLoad();
    return new SamplePlayerAudioProcessorEditor (*this);
}

void SamplePlayerAudioProcessor::maybeRunStandaloneStartupAutoLoad()
{
    if (standaloneStartupAutoLoadTriggered || pendingStandaloneStartupAutoLoadPath.isEmpty())
        return;

    standaloneStartupAutoLoadTriggered = true;

    const auto file = juce::File (pendingStandaloneStartupAutoLoadPath);
    if (! file.existsAsFile())
    {
        writeLoadDebugLog ("processor startup auto-load skipped | missing file | path=" + pendingStandaloneStartupAutoLoadPath);
        return;
    }

    if (file.hasFileExtension ("smpinstm"))
    {
        writeLoadDebugLog ("processor startup auto-load begin | path=" + pendingStandaloneStartupAutoLoadPath + " | format=smpinstm");
        loadMonolithDirect (pendingStandaloneStartupAutoLoadPath);
        return;
    }

    if (file.hasFileExtension ("smpinst") || file.hasFileExtension ("json"))
    {
        writeLoadDebugLog ("processor startup auto-load begin | path=" + pendingStandaloneStartupAutoLoadPath + " | format=" + file.getFileExtension());
        loadManifestDirect (pendingStandaloneStartupAutoLoadPath);
        return;
    }

    writeLoadDebugLog ("processor startup auto-load skipped | unsupported extension | path=" + pendingStandaloneStartupAutoLoadPath);
}

void SamplePlayerAudioProcessor::beginPresetLoadTrace (const juce::String& source, int bytesHint)
{
    const juce::ScopedLock lock (presetLoadTraceLock);
    ++presetLoadTraceId;
    presetLoadTraceStartMs = juce::Time::getMillisecondCounterHiRes();
    presetLoadTraceActive = true;
    presetLoadTraceSource = source;
    writeLoadDebugLog ("preset_load_trace begin | id=" + juce::String (presetLoadTraceId)
                       + " | source=" + source
                       + " | bytesHint=" + juce::String (bytesHint));
}

void SamplePlayerAudioProcessor::markPresetLoadPlayable (const juce::String& stage, int zoneCount)
{
    const juce::ScopedLock lock (presetLoadTraceLock);
    if (! presetLoadTraceActive)
        return;

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - presetLoadTraceStartMs;
    writeLoadDebugLog ("preset_load_trace playable | id=" + juce::String (presetLoadTraceId)
                       + " | source=" + presetLoadTraceSource
                       + " | stage=" + stage
                       + " | zones=" + juce::String (zoneCount)
                       + " | elapsedMs=" + juce::String (elapsedMs, 2));
    presetLoadTraceActive = false;
}

void SamplePlayerAudioProcessor::finishPresetLoadTrace (const juce::String& stage, const juce::String& outcome)
{
    const juce::ScopedLock lock (presetLoadTraceLock);
    if (! presetLoadTraceActive)
        return;

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - presetLoadTraceStartMs;
    writeLoadDebugLog ("preset_load_trace finish | id=" + juce::String (presetLoadTraceId)
                       + " | source=" + presetLoadTraceSource
                       + " | stage=" + stage
                       + " | outcome=" + outcome
                       + " | elapsedMs=" + juce::String (elapsedMs, 2));
    presetLoadTraceActive = false;
}

void SamplePlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto saveStartMs = juce::Time::getMillisecondCounterHiRes();
    auto state = parameters.copyState();

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet != nullptr && sampleSet->sourcePaths.size() > 0)
        state.setProperty (kSampleFilePathsProperty, sampleSet->sourcePaths.joinIntoString ("\n"), nullptr);
    else
        state.removeProperty (kSampleFilePathsProperty, nullptr);

    while (true)
    {
        const auto existing = state.getChildWithName (kZoneOverridesNode);
        if (! existing.isValid())
            break;

        state.removeChild (existing, nullptr);
    }

    auto overrides = buildZoneOverridesState();
    if (overrides.getNumChildren() > 0)
        state.addChild (overrides, -1, nullptr);

    {
        const juce::ScopedLock lock (uiSessionStateLock);
        if (uiSessionStateJson.isNotEmpty())
            state.setProperty (kUiSessionStateProperty, uiSessionStateJson, nullptr);
        else
            state.removeProperty (kUiSessionStateProperty, nullptr);
    }

    destData.reset();
    juce::MemoryOutputStream output (destData, false);
    output.writeInt (static_cast<int> (kBinaryStateMagic));
    output.writeInt (kBinaryStateVersion);
    state.writeToStream (output);

    writeLoadDebugLog ("getStateInformation saved | format=binary-v1 | bytes="
                       + juce::String (static_cast<juce::int64> (destData.getSize()))
                       + " | elapsedMs=" + juce::String (elapsedMsFrom (saveStartMs), 2));
}

void SamplePlayerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto loadStartMs = juce::Time::getMillisecondCounterHiRes();
    beginPresetLoadTrace ("setStateInformation", sizeInBytes);
    writeLoadDebugLog ("setStateInformation begin | bytes=" + juce::String (sizeInBytes));

    juce::ValueTree restoredState;
    bool restoredFromBinary = false;
    double binaryParsedMs = 0.0;
    double xmlParsedMs = 0.0;

    if (data != nullptr && sizeInBytes >= static_cast<int> (sizeof (int) * 2))
    {
        juce::MemoryInputStream input (data, static_cast<size_t> (sizeInBytes), false);
        const auto magic = static_cast<juce::uint32> (input.readInt());
        const int version = input.readInt();

        if (magic == kBinaryStateMagic && version == kBinaryStateVersion)
        {
            const auto binaryParseStartMs = juce::Time::getMillisecondCounterHiRes();
            restoredState = juce::ValueTree::readFromStream (input);
            binaryParsedMs = elapsedMsFrom (binaryParseStartMs);
            restoredFromBinary = restoredState.isValid()
                              && restoredState.hasType (parameters.state.getType());
            writeLoadDebugLog ("setStateInformation binary decode | valid="
                               + juce::String (restoredFromBinary ? "yes" : "no")
                               + " | elapsedMs=" + juce::String (binaryParsedMs, 2));
        }
    }

    if (! restoredFromBinary)
    {
        // Backward compatibility: load older XML-based states.
        std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

        if (xmlState == nullptr)
        {
            writeLoadDebugLog ("setStateInformation aborted | invalid state blob | elapsedMs="
                               + juce::String (elapsedMsFrom (loadStartMs), 2));
            finishPresetLoadTrace ("setStateInformation", "invalid-state-blob");
            return;
        }

        if (! xmlState->hasTagName (parameters.state.getType()))
        {
            writeLoadDebugLog ("setStateInformation aborted | unexpected root tag | elapsedMs="
                               + juce::String (elapsedMsFrom (loadStartMs), 2));
            finishPresetLoadTrace ("setStateInformation", "unexpected-root-tag");
            return;
        }

        const auto xmlParseStartMs = juce::Time::getMillisecondCounterHiRes();
        restoredState = juce::ValueTree::fromXml (*xmlState);
        xmlParsedMs = elapsedMsFrom (xmlParseStartMs);
    }

    if (! restoredState.isValid() || ! restoredState.hasType (parameters.state.getType()))
    {
        writeLoadDebugLog ("setStateInformation aborted | could not parse state tree | elapsedMs="
                           + juce::String (elapsedMsFrom (loadStartMs), 2));
        finishPresetLoadTrace ("setStateInformation", "parse-failed");
        return;
    }

    parameters.replaceState (restoredState);

    const auto restoredUiSessionJson = restoredState.getProperty (kUiSessionStateProperty).toString();
    const bool hasEmbeddedUiSession = restoredUiSessionJson.trim().isNotEmpty();
    const bool embeddedSessionHasSampleData = hasEmbeddedUiSession
                                           && sessionJsonHasEmbeddedSampleData (restoredUiSessionJson);
    writeLoadDebugLog ("setStateInformation parsed | format="
                       + juce::String (restoredFromBinary ? "binary-v1" : "xml")
                       + " | parseMs=" + juce::String (restoredFromBinary ? binaryParsedMs : xmlParsedMs, 2)
                       + " | embeddedSession=" + juce::String (hasEmbeddedUiSession ? "yes" : "no")
                       + " | embeddedSampleData=" + juce::String (embeddedSessionHasSampleData ? "yes" : "no")
                       + " | sessionJsonBytes=" + juce::String (restoredUiSessionJson.getNumBytesAsUTF8()));

    // If session payload is lightweight (no embedded sample data), restore from unpacked file paths.
    if (! embeddedSessionHasSampleData)
    {
        juce::StringArray samplePathLines;
        samplePathLines.addLines (restoredState.getProperty (kSampleFilePathsProperty).toString());
        restoreSampleFilesFromState (samplePathLines);

        const auto zoneOverrides = restoredState.getChildWithName (kZoneOverridesNode);
        if (zoneOverrides.isValid())
            applyZoneOverridesState (zoneOverrides);
    }

    const auto legacyRestoreMs = elapsedMsFrom (loadStartMs);
    setUiSessionStateJson (restoredUiSessionJson);
    writeLoadDebugLog ("setStateInformation completed | legacyRestoreMs=" + juce::String (legacyRestoreMs, 2)
                       + " | totalMs=" + juce::String (elapsedMsFrom (loadStartMs), 2));
}

bool SamplePlayerAudioProcessor::isSupportedSampleFile (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".wav"
        || extension == ".aif"
        || extension == ".aiff"
        || extension == ".flac"
        || extension == ".ogg";
}

bool SamplePlayerAudioProcessor::loadSampleFolder (const juce::File& folder, juce::String& errorMessage)
{
    if (! folder.isDirectory())
    {
        errorMessage = "Selected path is not a valid folder.";
        return false;
    }

    juce::Array<juce::File> collected;

    for (const auto& pattern : { "*.wav", "*.aif", "*.aiff", "*.flac", "*.ogg" })
        folder.findChildFiles (collected, juce::File::findFiles, true, pattern);

    if (collected.isEmpty())
    {
        errorMessage = "No supported sample files found in folder.";
        return false;
    }

    return loadSampleFiles (collected, errorMessage);
}

bool SamplePlayerAudioProcessor::loadSampleFiles (const juce::Array<juce::File>& files, juce::String& errorMessage)
{
    std::vector<juce::File> uniqueFiles;
    uniqueFiles.reserve (static_cast<size_t> (files.size()));

    for (const auto& file : files)
    {
        if (! file.existsAsFile() || ! isSupportedSampleFile (file))
            continue;

        const auto fullPath = file.getFullPathName();

        const auto alreadyAdded = std::any_of (uniqueFiles.begin(), uniqueFiles.end(), [&fullPath] (const juce::File& existing)
        {
            return existing.getFullPathName() == fullPath;
        });

        if (! alreadyAdded)
            uniqueFiles.push_back (file);
    }

    std::sort (uniqueFiles.begin(), uniqueFiles.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFullPathName() < b.getFullPathName();
    });

    if (uniqueFiles.empty())
    {
        errorMessage = "No readable sample files were selected.";
        return false;
    }

    auto newSampleSet = std::make_shared<SampleSet>();
    newSampleSet->keyswitchSlotByMidi.fill (-1);

    // --- Parallel sample file reading ---
    const size_t fileCount = uniqueFiles.size();
    // Each slot holds the loaded zone (nullptr on failure), preserving order.
    std::vector<std::shared_ptr<SampleZone>> loadedZones (fileCount);

    const auto loadOneZone = [this] (const juce::File& file) -> std::shared_ptr<SampleZone>
    {
        // Per-thread format manager avoids thread-safety issues.
        thread_local juce::AudioFormatManager threadFm;
        thread_local bool threadFmReady = false;
        if (! threadFmReady)
        {
            threadFm.registerBasicFormats();
            threadFmReady = true;
        }

        auto reader = std::unique_ptr<juce::AudioFormatReader> (threadFm.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples < 2)
            return nullptr;

        const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
        const auto totalSamples64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                             static_cast<juce::int64> (std::numeric_limits<int>::max()));
        const int totalSamples = static_cast<int> (totalSamples64);
        if (totalSamples < 2)
            return nullptr;

        auto zone = std::make_shared<SampleZone>();
        zone->sourceFile = file;
        zone->sourceSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        zone->metadata = parseZoneMetadataFromFileName (file.getFileNameWithoutExtension());
        zone->audio.setSize (channels, totalSamples);
        reader->read (&zone->audio, 0, totalSamples, 0, true, true);
        return zone;
    };

    const unsigned int hwThreads = std::thread::hardware_concurrency();
    const unsigned int numThreads = juce::jlimit (1u, 8u, hwThreads > 0 ? hwThreads : 4u);

    if (fileCount <= 4 || numThreads <= 1)
    {
        // Small set — load sequentially, no thread overhead.
        for (size_t i = 0; i < fileCount; ++i)
            loadedZones[i] = loadOneZone (uniqueFiles[i]);
    }
    else
    {
        // Split work across threads.
        std::vector<std::thread> workers;
        workers.reserve (numThreads);

        std::atomic<size_t> nextIndex { 0 };

        for (unsigned int t = 0; t < numThreads; ++t)
        {
            workers.emplace_back ([&]()
            {
                while (true)
                {
                    const size_t idx = nextIndex.fetch_add (1, std::memory_order_relaxed);
                    if (idx >= fileCount)
                        break;
                    loadedZones[idx] = loadOneZone (uniqueFiles[idx]);
                }
            });
        }

        for (auto& w : workers)
            w.join();
    }

    for (size_t i = 0; i < fileCount; ++i)
    {
        if (loadedZones[i] != nullptr)
        {
            newSampleSet->zones.push_back (std::move (loadedZones[i]));
            newSampleSet->sourcePaths.add (uniqueFiles[i].getFullPathName());
        }
    }

    std::sort (newSampleSet->zones.begin(), newSampleSet->zones.end(), [] (const auto& a, const auto& b)
    {
        if (a->metadata.mapSetSlot != b->metadata.mapSetSlot)
            return a->metadata.mapSetSlot < b->metadata.mapSetSlot;

        if (a->metadata.rootNote != b->metadata.rootNote)
            return a->metadata.rootNote < b->metadata.rootNote;

        if (a->metadata.lowVelocity != b->metadata.lowVelocity)
            return a->metadata.lowVelocity < b->metadata.lowVelocity;

        if (a->metadata.roundRobinIndex != b->metadata.roundRobinIndex)
            return a->metadata.roundRobinIndex < b->metadata.roundRobinIndex;

        return a->sourceFile.getFileName() < b->sourceFile.getFileName();
    });

    if (newSampleSet->zones.empty())
    {
        errorMessage = "Could not read any sample audio data from the selected files.";
        finishPresetLoadTrace ("loadSampleFiles", "no-readable-zones");
        return false;
    }

    newSampleSet->summary = buildSampleSummary (newSampleSet->zones);

    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (newSampleSet));
    resetVoicesRequested.store (true);
    markPresetLoadPlayable ("loadSampleFiles", static_cast<int> (newSampleSet->zones.size()));

    return true;
}

void SamplePlayerAudioProcessor::clearSampleSet()
{
    auto emptySet = std::make_shared<SampleSet>();
    emptySet->keyswitchSlotByMidi.fill (-1);
    emptySet->summary = "No samples loaded.\n\n" + getZoneNamingHint();

    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (emptySet));
    std::atomic_store (&stepSequencerRuntime, std::make_shared<StepSequencerRuntime>());
    activeMapSetSlot.store (0, std::memory_order_relaxed);
    activeMapLoopPlaybackEnabled.store (true, std::memory_order_relaxed);
    sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
    resetVoicesRequested.store (true);
}

juce::String SamplePlayerAudioProcessor::getSampleSummaryText() const
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr)
        return "No samples loaded.";

    return sampleSet->summary;
}

int SamplePlayerAudioProcessor::getLoadedZoneCount() const
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);
    return sampleSet != nullptr ? static_cast<int> (sampleSet->zones.size()) : 0;
}

bool SamplePlayerAudioProcessor::getZoneEditorInfo (int zoneIndex, ZoneEditorInfo& outInfo) const
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr || zoneIndex < 0 || zoneIndex >= static_cast<int> (sampleSet->zones.size()))
        return false;

    const auto& zone = sampleSet->zones[static_cast<size_t> (zoneIndex)];

    outInfo.index = zoneIndex;
    outInfo.fileName = zone->sourceFile.getFileName();
    outInfo.metadata = zone->metadata;

    return true;
}

juce::StringArray SamplePlayerAudioProcessor::getZoneDisplayNames() const
{
    juce::StringArray names;

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr)
        return names;

    for (size_t i = 0; i < sampleSet->zones.size(); ++i)
    {
        const auto& zone = sampleSet->zones[i];
        const auto& m = zone->metadata;

        juce::String name;
        name << juce::String (static_cast<int> (i) + 1)
             << ": "
             << zone->sourceFile.getFileName()
             << " [n"
             << m.rootNote
             << " k"
             << m.lowNote
             << "-"
             << m.highNote
             << " v"
             << m.lowVelocity
             << "-"
             << m.highVelocity
             << " rr"
             << m.roundRobinIndex
             << "]";

        names.add (name);
    }

    return names;
}

bool SamplePlayerAudioProcessor::updateZoneMetadata (int zoneIndex,
                                                      const ZoneMetadata& metadata,
                                                      juce::String& errorMessage)
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr)
    {
        errorMessage = "No sample set is loaded.";
        return false;
    }

    if (zoneIndex < 0 || zoneIndex >= static_cast<int> (sampleSet->zones.size()))
    {
        errorMessage = "Invalid zone index.";
        return false;
    }

    const auto sanitized = sanitizeZoneMetadata (metadata);
    const auto& target = sampleSet->zones[static_cast<size_t> (zoneIndex)];

    if (zoneMetadataEquals (target->metadata, sanitized))
        return true;

    auto updatedSet = std::make_shared<SampleSet>();
    updatedSet->sourcePaths = sampleSet->sourcePaths;
    updatedSet->zones.reserve (sampleSet->zones.size());

    for (size_t i = 0; i < sampleSet->zones.size(); ++i)
    {
        if (static_cast<int> (i) == zoneIndex)
        {
            auto updatedZone = std::make_shared<SampleZone> (*sampleSet->zones[i]);
            updatedZone->metadata = sanitized;
            updatedSet->zones.push_back (updatedZone);
        }
        else
        {
            updatedSet->zones.push_back (sampleSet->zones[i]);
        }
    }

    updatedSet->summary = buildSampleSummary (updatedSet->zones);

    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (updatedSet));
    resetVoicesRequested.store (true);

    return true;
}

int SamplePlayerAudioProcessor::getUiSessionStateLightweightVersion() const noexcept
{
    return uiSessionStateLightweightVersion.load (std::memory_order_relaxed);
}

void SamplePlayerAudioProcessor::perfLog (const juce::String& eventName,
                                          double elapsedMs,
                                          const juce::String& details)
{
    juce::ignoreUnused (eventName, elapsedMs, details);
}

void SamplePlayerAudioProcessor::perfFlushToFile()
{
}

void SamplePlayerAudioProcessor::setUiSessionStateJson (const juce::String& json)
{
    const auto requestStartMs = juce::Time::getMillisecondCounterHiRes();
    juce::String normalizedJson = json;

    int parsedPitchDownOctaves = 0;
    float parsedModwheelValue01 = 0.0f;
    uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
    float parsedExpressionValue01 = expressionControllerValue01.load (std::memory_order_relaxed);
    float parsedAttackMs = parameters.getRawParameterValue ("attackMs")->load();
    float parsedDecayMs = parameters.getRawParameterValue ("decayMs")->load();
    float parsedSustainLevel = parameters.getRawParameterValue ("sustain")->load();
    float parsedReleaseMs = parameters.getRawParameterValue ("releaseMs")->load();
    if (normalizedJson.isNotEmpty())
    {
        const auto parsedForPitch = juce::JSON::parse (normalizedJson);
        if (const auto* rootObject = parsedForPitch.getDynamicObject())
        {
            if (const auto* uiObject = rootObject->getProperty ("ui").getDynamicObject())
            {
                parsedPitchDownOctaves = juce::jlimit (0, 2,
                    varToInt (uiObject->getProperty ("playerPitchDownOctaves"), 0));
                parsedExpressionValue01 = juce::jlimit (0.0f, 1.0f,
                    static_cast<float> (varToDouble (uiObject->getProperty ("expressionValue"),
                                                     static_cast<double> (parsedExpressionValue01))));
                if (const auto* ampObject = uiObject->getProperty ("ampEnvelope").getDynamicObject())
                {
                    parsedAttackMs = juce::jmax (0.0f,
                        static_cast<float> (varToDouble (ampObject->getProperty ("attackMs"), parsedAttackMs)));
                    parsedDecayMs = juce::jmax (0.0f,
                        static_cast<float> (varToDouble (ampObject->getProperty ("decayMs"), parsedDecayMs)));
                    parsedSustainLevel = juce::jlimit (0.0f, 1.0f,
                        static_cast<float> (varToDouble (ampObject->getProperty ("sustainPercent"),
                                                         static_cast<double> (parsedSustainLevel * 100.0f)) / 100.0));
                    parsedReleaseMs = juce::jmax (0.0f,
                        static_cast<float> (varToDouble (ampObject->getProperty ("releaseMs"), parsedReleaseMs)));
                }
            }
        }
    }
    playerPitchDownOctaves.store (parsedPitchDownOctaves, std::memory_order_relaxed);
    modwheelVelocityLayerControlValue01.store (parsedModwheelValue01, std::memory_order_relaxed);
    expressionControllerValue01.store (parsedExpressionValue01, std::memory_order_relaxed);

    const auto applyParamFromPlainValue = [this] (const juce::String& paramId, float plainValue)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (paramId));
        if (parameter == nullptr)
            return;

        const auto value01 = juce::jlimit (0.0f, 1.0f, parameter->convertTo0to1 (plainValue));
        if (std::abs (parameter->getValue() - value01) <= 0.000001f)
            return;

        parameter->setValueNotifyingHost (value01);
    };

    applyParamFromPlainValue ("attackMs", parsedAttackMs);
    applyParamFromPlainValue ("decayMs", parsedDecayMs);
    applyParamFromPlainValue ("sustain", parsedSustainLevel);
    applyParamFromPlainValue ("releaseMs", parsedReleaseMs);

    if (auto* modParam = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (kModWheelParamId)))
        modParam->setValue (juce::jlimit (0.0f, 1.0f, parsedModwheelValue01));
    if (auto* expressionParam = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (kExpressionParamId)))
        expressionParam->setValue (juce::jlimit (0.0f, 1.0f, parsedExpressionValue01));

    const int midiRequestedSlot = pendingActiveMapSetSlotFromMidi.exchange (-1, std::memory_order_relaxed);
    if (midiRequestedSlot >= 0 && normalizedJson.isNotEmpty())
    {
        juce::String desiredSetId = midiRequestedSlot == 0 ? "base" : juce::String {};

        if (midiRequestedSlot > 0)
        {
            if (const auto sampleSet = std::atomic_load (&currentSampleSet); sampleSet != nullptr)
            {
                for (const auto& entry : sampleSet->mapSetSlotById)
                {
                    if (entry.second == midiRequestedSlot)
                    {
                        desiredSetId = juce::String (entry.first);
                        break;
                    }
                }
            }
        }

        if (desiredSetId.isNotEmpty())
        {
            const auto parsed = juce::JSON::parse (normalizedJson);
            if (auto* rootObject = parsed.getDynamicObject())
            {
                if (auto* uiObject = rootObject->getProperty ("ui").getDynamicObject())
                {
                    const auto activeSetId = uiObject->getProperty ("activeMapSetId").toString().trim();
                    if (activeSetId != desiredSetId)
                    {
                        uiObject->setProperty ("activeMapSetId", desiredSetId);

                        if (auto* keyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
                        {
                            for (auto& setVar : *keyswitchSets)
                            {
                                auto* setObject = setVar.getDynamicObject();
                                if (setObject == nullptr)
                                    continue;

                                const auto candidateId = setObject->getProperty ("id").toString().trim();
                                setObject->setProperty ("active",
                                                        candidateId.isNotEmpty() && candidateId == desiredSetId);
                            }
                        }

                        normalizedJson = juce::JSON::toString (parsed, false);
                        writeLoadDebugLog ("session json active map patched from midi | slot="
                                           + juce::String (midiRequestedSlot)
                                           + " | setId=" + desiredSetId);
                    }
                }
            }
        }
    }

    const auto jsonBytes = normalizedJson.getNumBytesAsUTF8();

    {
        const juce::ScopedLock lock (uiSessionStateLock);
        if (uiSessionStateJson == normalizedJson)
        {
            pendingActiveMapSetId.clear();
            writeLoadDebugLog ("setUiSessionStateJson no-op | bytes=" + juce::String (jsonBytes));
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int zoneCount = sampleSet != nullptr ? static_cast<int> (sampleSet->zones.size()) : 0;
            if (zoneCount > 0)
                markPresetLoadPlayable ("setUiSessionStateJson-no-op", zoneCount);
            else
                finishPresetLoadTrace ("setUiSessionStateJson-no-op", "no-zones");
            return;
        }
        uiSessionStateJson = normalizedJson;
        pendingActiveMapSetId.clear();
    }

    if (monolithDecodeInProgress.load (std::memory_order_acquire))
    {
        writeLoadDebugLog ("setUiSessionStateJson deferred | monolith decode in progress | bytes="
                           + juce::String (jsonBytes));
        return;
    }

    const int requestId = sessionStateSyncRequestId.fetch_add (1, std::memory_order_relaxed) + 1;
    sessionStateSyncThreadPool.removeAllJobs (false, 1);
    writeLoadDebugLog ("setUiSessionStateJson queued | requestId=" + juce::String (requestId)
                       + " | bytes=" + juce::String (jsonBytes)
                       + " | queuePrepMs=" + juce::String (elapsedMsFrom (requestStartMs), 2));

    sessionStateSyncThreadPool.addJob ([this, requestId]()
    {
        const auto syncJobStartMs = juce::Time::getMillisecondCounterHiRes();
        juce::String latestJson;
        {
            const juce::ScopedLock lock (uiSessionStateLock);
            latestJson = uiSessionStateJson;
        }

        const auto jsonBytes = static_cast<juce::int64> (latestJson.getNumBytesAsUTF8());
        const auto parseStartMs = juce::Time::getMillisecondCounterHiRes();
        auto parsed = juce::JSON::parse (latestJson);
        const auto parseMs = elapsedMsFrom (parseStartMs);

        if (parsed.isVoid())
        {
            writeLoadDebugLog ("session sync job parse failed | requestId=" + juce::String (requestId)
                               + " | bytes=" + juce::String (jsonBytes));
            finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "invalid-json");
            return;
        }

        writeLoadDebugLog ("session sync job parsed once | requestId=" + juce::String (requestId)
                           + " | bytes=" + juce::String (jsonBytes)
                           + " | parseMs=" + juce::String (parseMs, 2));

        // --- sample sync first (read-only traversal of the parsed var) ---
        writeLoadDebugLog ("session sync job start | requestId=" + juce::String (requestId)
                           + " | bytes=" + juce::String (jsonBytes));
        syncSampleSetFromSessionStateJson (parsed, jsonBytes, requestId);

        // --- build lightweight cache (strips data URLs destructively from parsed var) ---
        const auto lightweightStartMs = juce::Time::getMillisecondCounterHiRes();

        LightweightStripStats stripStats;
        stripLargePayloadFieldsRecursive (parsed, stripStats);

        if (auto* rootObject = parsed.getDynamicObject())
        {
            if (auto* manifestObj = rootObject->getProperty ("manifest").getDynamicObject())
            {
                manifestObj->removeProperty ("entries");
                manifestObj->removeProperty ("variants");
            }
        }

        auto lightweightJson = juce::JSON::toString (parsed, false);
        if (lightweightJson.isEmpty())
            lightweightJson = latestJson;

        {
            const juce::ScopedLock lock (uiSessionStateLock);
            if (requestId == sessionStateSyncRequestId.load (std::memory_order_relaxed))
            {
                uiSessionStateLightweightJson = lightweightJson;
                uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
            }
        }

        writeLoadDebugLog ("session lightweight cache ready | requestId=" + juce::String (requestId)
                           + " | bytesOut=" + juce::String (lightweightJson.getNumBytesAsUTF8())
                           + " | sampleDataRemoved=" + juce::String (stripStats.sampleDataUrlsRemoved)
                           + " | dataUrlsRemoved=" + juce::String (stripStats.dataUrlsRemoved)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (lightweightStartMs), 2));

        writeLoadDebugLog ("session sync job end | requestId=" + juce::String (requestId)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (syncJobStartMs), 2));
    });
}

namespace
{
bool manualRangesVarHasEntries (const juce::var& value)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        return false;

    const auto& properties = object->getProperties();
    for (int i = 0; i < properties.size(); ++i)
    {
        const auto* rangeObject = properties.getValueAt (i).getDynamicObject();
        if (rangeObject == nullptr)
            continue;

        if (! rangeObject->getProperty ("low").isVoid()
            || ! rangeObject->getProperty ("high").isVoid())
            return true;
    }

    return false;
}

juce::var buildManualRangesVarFromMapping (const juce::Array<juce::var>* mappingArray)
{
    if (mappingArray == nullptr || mappingArray->isEmpty())
        return juce::var {};

    auto* manualRangesObject = new juce::DynamicObject();

    for (const auto& bucketVar : *mappingArray)
    {
        const auto* bucketObject = bucketVar.getDynamicObject();
        if (bucketObject == nullptr)
            continue;

        const int rootMidi = varToInt (bucketObject->getProperty ("rootMidiNote"), -1);
        const int lowNote = varToInt (bucketObject->getProperty ("lowNote"), -1);
        const int highNote = varToInt (bucketObject->getProperty ("highNote"), -1);

        if (! juce::isPositiveAndBelow (rootMidi, 128)
            || ! juce::isPositiveAndBelow (lowNote, 128)
            || ! juce::isPositiveAndBelow (highNote, 128))
            continue;

        auto* rangeObject = new juce::DynamicObject();
        rangeObject->setProperty ("low", lowNote);
        rangeObject->setProperty ("high", highNote);
        manualRangesObject->setProperty (juce::String (rootMidi), juce::var (rangeObject));
    }

    if (manualRangesObject->getProperties().size() == 0)
    {
        delete manualRangesObject;
        return juce::var {};
    }

    return juce::var (manualRangesObject);
}

juce::var pickManualRangesVar (const juce::var& preferred,
                               const juce::var& fallback,
                               const juce::Array<juce::var>* mappingArray)
{
    if (manualRangesVarHasEntries (preferred))
        return preferred;

    if (manualRangesVarHasEntries (fallback))
        return fallback;

    return buildManualRangesVarFromMapping (mappingArray);
}

juce::var buildDirectLoadSessionSnapshot (const juce::var& manifest,
                                          const juce::String& filePath,
                                          const juce::String& manifestBasePath)
{
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty ("version", 1);
    rootObj->setProperty ("manifest", manifest);

    auto* uiObj = new juce::DynamicObject();
    uiObj->setProperty ("manifestFilePath", filePath);
    uiObj->setProperty ("manifestBasePath", manifestBasePath);
    uiObj->setProperty ("activeMapSetId", juce::String ("base"));
    uiObj->setProperty ("baseLoopPlaybackEnabled", true);
    uiObj->setProperty ("basePlaybackMode", juce::var ("spread"));

    if (auto* manifestRoot = manifest.getDynamicObject())
    {
        const auto loopPlaybackEnabledVar = manifestRoot->getProperty ("settings").getProperty ("baseLoopPlaybackEnabled", true);
        uiObj->setProperty ("baseLoopPlaybackEnabled", loopPlaybackEnabledVar);
        const auto basePlaybackMode = normalizeKeyswitchPlaybackMode (
            manifestRoot->getProperty ("settings").getProperty ("basePlaybackMode", "spread").toString());
        uiObj->setProperty ("basePlaybackMode", juce::var (basePlaybackMode));
        const auto baseManualRangesVar = pickManualRangesVar (
            manifestRoot->getProperty ("settings").getProperty ("manualRootRanges", juce::var {}),
            juce::var {},
            manifestRoot->getProperty ("mapping").getArray());
        if (manualRangesVarHasEntries (baseManualRangesVar))
            uiObj->setProperty ("manualRootRanges", baseManualRangesVar);
        const auto openStringMidisVar = manifestRoot->getProperty ("settings").getProperty ("openStringMidis", {});
        if (! openStringMidisVar.isVoid())
            uiObj->setProperty ("playerOpenStringMidis", openStringMidisVar);

        if (auto* manifestKsSets = manifestRoot->getProperty ("keyswitchSets").getArray())
        {
            juce::Array<juce::var> uiKsSets;
            for (int i = 0; i < manifestKsSets->size(); ++i)
            {
                auto* ksObj = (*manifestKsSets)[i].getDynamicObject();
                if (ksObj == nullptr)
                    continue;

                auto* uiKs = new juce::DynamicObject();
                const auto keyswitchName = ksObj->getProperty ("name").toString().trim();
                uiKs->setProperty ("id", ksObj->hasProperty ("id")
                    ? ksObj->getProperty ("id")
                    : juce::var ("keyswitch_" + juce::String (i + 1)));
                uiKs->setProperty ("name", keyswitchName);
                uiKs->setProperty ("key", ksObj->getProperty ("key"));
                uiKs->setProperty ("keyMidi", ksObj->getProperty ("keyMidi"));
                uiKs->setProperty ("loopPlaybackEnabled", ksObj->getProperty ("loopPlaybackEnabled"));
                uiKs->setProperty ("gainDb", ksObj->getProperty ("gainDb"));
                uiKs->setProperty ("openStringMidis", ksObj->getProperty ("openStringMidis"));
                uiKs->setProperty ("playbackMode",
                                   resolveKeyswitchOneShotPlayback (keyswitchName,
                                                                    ksObj->getProperty ("playbackMode").toString())
                                       ? juce::var ("oneshot")
                                       : juce::var ("spread"));
                uiKs->setProperty ("active", i == 0);
                uiKs->setProperty ("index", i);
                const auto manualRangesVar = pickManualRangesVar (
                    ksObj->getProperty ("manualRanges"),
                    juce::var {},
                    ksObj->getProperty ("mapping").getArray());
                if (manualRangesVarHasEntries (manualRangesVar))
                    uiKs->setProperty ("manualRanges", manualRangesVar);
                uiKsSets.add (juce::var (uiKs));
            }

            if (! uiKsSets.isEmpty())
                uiObj->setProperty ("keyswitchSets", uiKsSets);
        }
    }

    rootObj->setProperty ("ui", juce::var (uiObj));
    return juce::var (rootObj);
}
}

void SamplePlayerAudioProcessor::loadManifestDirect (const juce::String& filePath)
{
    const auto file = juce::File (filePath);
    if (! file.existsAsFile())
        return;

    const int requestId = sessionStateSyncRequestId.fetch_add (1, std::memory_order_relaxed) + 1;
    sessionStateSyncThreadPool.removeAllJobs (false, 1);

    writeLoadDebugLog ("loadManifestDirect queued | requestId=" + juce::String (requestId)
                       + " | path=" + filePath);

    sessionStateSyncThreadPool.addJob ([this, requestId, filePath]()
    {
        const auto jobStartMs = juce::Time::getMillisecondCounterHiRes();
        const auto manifestFile = juce::File (filePath);
        const auto text = manifestFile.loadFileAsString();

        if (text.isEmpty())
        {
            finishPresetLoadTrace ("loadManifestDirect", "empty-file");
            writeLoadDebugLog ("loadManifestDirect empty file | requestId=" + juce::String (requestId));
            return;
        }

        const auto parseStartMs = juce::Time::getMillisecondCounterHiRes();
        auto manifestParsed = juce::JSON::parse (text);

        if (manifestParsed.isVoid())
        {
            finishPresetLoadTrace ("loadManifestDirect", "parse-failed");
            writeLoadDebugLog ("loadManifestDirect parse failed | requestId=" + juce::String (requestId));
            return;
        }

        writeLoadDebugLog ("loadManifestDirect parsed | requestId=" + juce::String (requestId)
                           + " | parseMs=" + juce::String (elapsedMsFrom (parseStartMs), 2));

        auto fullSessionVar = buildDirectLoadSessionSnapshot (manifestParsed,
                                                              filePath,
                                                              manifestFile.getParentDirectory().getFullPathName());
        const auto fullSessionJson = juce::JSON::toString (fullSessionVar, false);

        syncSampleSetFromSessionStateJson (fullSessionVar, 0, requestId);

        {
            const juce::ScopedLock lock (uiSessionStateLock);
            if (requestId == sessionStateSyncRequestId.load (std::memory_order_relaxed))
            {
                uiSessionStateJson = fullSessionJson;

                LightweightStripStats stripStats;
                auto lightweightJson = makeLightweightSessionStateJson (fullSessionJson, &stripStats);
                if (lightweightJson.isEmpty())
                    lightweightJson = fullSessionJson;

                uiSessionStateLightweightJson = lightweightJson;
                uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
            }
        }

        writeLoadDebugLog ("loadManifestDirect done | requestId=" + juce::String (requestId)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (jobStartMs), 2));
    });
}

void SamplePlayerAudioProcessor::loadMonolithDirect (const juce::String& filePath)
{
    const auto file = juce::File (filePath);
    if (! file.existsAsFile())
        return;

    monolithDecodeInProgress.store (true, std::memory_order_release);

    const int requestId = sessionStateSyncRequestId.fetch_add (1, std::memory_order_relaxed) + 1;
    sessionStateSyncThreadPool.removeAllJobs (false, 1);

    writeLoadDebugLog ("loadMonolithDirect queued | requestId=" + juce::String (requestId)
                       + " | path=" + filePath);

    sessionStateSyncThreadPool.addJob ([this, requestId, filePath]()
    {
        const auto jobStartMs = juce::Time::getMillisecondCounterHiRes();
        const auto monolithFile = juce::File (filePath);
        const auto text = monolithFile.loadFileAsString();

        if (text.isEmpty())
        {
            monolithDecodeInProgress.store (false, std::memory_order_release);
            finishPresetLoadTrace ("loadMonolithDirect", "empty-file");
            writeLoadDebugLog ("loadMonolithDirect empty file | requestId=" + juce::String (requestId));
            return;
        }

        const auto parseStartMs = juce::Time::getMillisecondCounterHiRes();
        auto monolithParsed = juce::JSON::parse (text);

        if (monolithParsed.isVoid())
        {
            monolithDecodeInProgress.store (false, std::memory_order_release);
            finishPresetLoadTrace ("loadMonolithDirect", "parse-failed");
            writeLoadDebugLog ("loadMonolithDirect parse failed | requestId=" + juce::String (requestId));
            return;
        }

        writeLoadDebugLog ("loadMonolithDirect parsed | requestId=" + juce::String (requestId)
                           + " | parseMs=" + juce::String (elapsedMsFrom (parseStartMs), 2));

        auto fullSessionVar = buildDirectLoadSessionSnapshot (monolithParsed,
                                                              filePath,
                                                              monolithFile.getParentDirectory().getFullPathName());
        const auto fullSessionJson = juce::JSON::toString (fullSessionVar, false);

        syncSampleSetFromSessionStateJson (fullSessionVar, 0, requestId);

        // Publish the native-loaded monolith into the same session snapshot
        // cache the frontend already fetches, so the UI can refresh without
        // re-reading and re-parsing the monolith through the webview bridge.
        {
            const juce::ScopedLock lock (uiSessionStateLock);
            if (requestId == sessionStateSyncRequestId.load (std::memory_order_relaxed))
            {
                uiSessionStateJson = fullSessionJson;

                LightweightStripStats stripStats;
                auto lightweightJson = makeLightweightSessionStateJson (fullSessionJson, &stripStats);
                if (lightweightJson.isEmpty())
                    lightweightJson = fullSessionJson;

                uiSessionStateLightweightJson = lightweightJson;
                uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
            }
        }

        // Decode complete — build lightweight cache from any deferred JS state.
        monolithDecodeInProgress.store (false, std::memory_order_release);

        {
            const juce::ScopedLock lock (uiSessionStateLock);
            if (uiSessionStateJson.isNotEmpty()
                && requestId == sessionStateSyncRequestId.load (std::memory_order_relaxed))
            {
                LightweightStripStats stripStats;
                auto lw = makeLightweightSessionStateJson (uiSessionStateJson, &stripStats);
                if (lw.isEmpty())
                    lw = uiSessionStateJson;
                uiSessionStateLightweightJson = lw;
                uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
            }
        }

        writeLoadDebugLog ("loadMonolithDirect done | requestId=" + juce::String (requestId)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (jobStartMs), 2));
    });
}

void SamplePlayerAudioProcessor::setActiveMapSetId (const juce::String& setId)
{
    const auto normalizedSetId = setId.trim();
    if (normalizedSetId.isEmpty())
        return;

    bool switchedInRam = false;
    auto sampleSet = std::atomic_load (&currentSampleSet);
    {
        if (sampleSet != nullptr)
        {
            const auto it = sampleSet->mapSetSlotById.find (normalizedSetId.toStdString());
            if (it != sampleSet->mapSetSlotById.end())
            {
                const int slot = juce::jmax (0, it->second);
                activeMapSetSlot.store (slot, std::memory_order_relaxed);

                bool loopEnabled = true;
                if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (slot); loopIt != sampleSet->loopPlaybackBySlot.end())
                    loopEnabled = loopIt->second;
                activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
                resetVoicesRequested.store (true);
                switchedInRam = true;
            }
        }
    }

    if (switchedInRam)
    {
        const juce::ScopedLock lock (uiSessionStateLock);
        pendingActiveMapSetId = normalizedSetId;
        writeLoadDebugLog ("active map switch requested | setId=" + normalizedSetId + " | mode=ram-fast");
        return;
    }

    juce::String currentJson;
    {
        const juce::ScopedLock lock (uiSessionStateLock);
        currentJson = uiSessionStateJson;
    }

    if (currentJson.trim().isEmpty())
        return;

    const auto parsed = juce::JSON::parse (currentJson);
    auto* rootObject = parsed.getDynamicObject();
    if (rootObject == nullptr)
        return;

    auto* uiObject = rootObject->getProperty ("ui").getDynamicObject();
    if (uiObject == nullptr)
        return;

    if (! switchedInRam && sampleSet != nullptr)
    {
        int fallbackSlot = -1;
        if (normalizedSetId == "base")
        {
            fallbackSlot = 0;
        }
        else if (auto* keyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
        {
            for (int i = 0; i < keyswitchSets->size(); ++i)
            {
                const auto* setObject = (*keyswitchSets)[i].getDynamicObject();
                if (setObject == nullptr)
                    continue;

                const auto candidateId = setObject->getProperty ("id").toString().trim();
                if (candidateId == normalizedSetId)
                {
                    fallbackSlot = i + 1;
                    break;
                }
            }
        }

        if (fallbackSlot >= 0)
        {
            activeMapSetSlot.store (fallbackSlot, std::memory_order_relaxed);

            bool loopEnabled = true;
            if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (fallbackSlot); loopIt != sampleSet->loopPlaybackBySlot.end())
                loopEnabled = loopIt->second;
            activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
            resetVoicesRequested.store (true);
            switchedInRam = true;

            writeLoadDebugLog ("active map switch fallback | setId=" + normalizedSetId
                               + " | slot=" + juce::String (fallbackSlot));
        }
    }

    const auto currentSetId = uiObject->getProperty ("activeMapSetId").toString().trim();
    if (currentSetId == normalizedSetId && switchedInRam)
        return;

    uiObject->setProperty ("activeMapSetId", normalizedSetId);

    if (auto* keyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
    {
        for (auto& setVar : *keyswitchSets)
        {
            auto* setObject = setVar.getDynamicObject();
            if (setObject == nullptr)
                continue;

            const auto candidateId = setObject->getProperty ("id").toString().trim();
            setObject->setProperty ("active", candidateId.isNotEmpty() && candidateId == normalizedSetId);
        }
    }

    const auto updatedJson = juce::JSON::toString (parsed);

    writeLoadDebugLog ("active map switch requested | setId=" + normalizedSetId + " | mode=resync");
    setUiSessionStateJson (updatedJson);
}

void SamplePlayerAudioProcessor::setKeyswitchSetGainDb (const juce::String& setId, float gainDb)
{
    const auto normalizedSetId = setId.trim();
    if (normalizedSetId.isEmpty())
        return;

    const auto sampleSet = std::atomic_load (&currentSampleSet);
    if (sampleSet == nullptr)
        return;

    const auto slotIt = sampleSet->mapSetSlotById.find (normalizedSetId.toStdString());
    if (slotIt == sampleSet->mapSetSlotById.end())
        return;

    auto updatedSampleSet = std::make_shared<SampleSet> (*sampleSet);
    const int slot = juce::jmax (0, slotIt->second);
    const auto clampedGainDb = juce::jlimit (-24.0f, 24.0f, gainDb);
    updatedSampleSet->gainLinearBySlot[slot] = juce::Decibels::decibelsToGain (clampedGainDb);
    std::atomic_store (&currentSampleSet, std::static_pointer_cast<const SampleSet> (updatedSampleSet));
}

juce::String SamplePlayerAudioProcessor::getUiSessionStateJson (bool lightweightPreferred)
{
    const juce::ScopedLock lock (uiSessionStateLock);

    if (pendingActiveMapSetId.isNotEmpty() && uiSessionStateJson.isNotEmpty())
    {
        const auto parsed = juce::JSON::parse (uiSessionStateJson);
        if (auto* rootObject = parsed.getDynamicObject())
        {
            if (auto* uiObject = rootObject->getProperty ("ui").getDynamicObject())
            {
                const auto desiredSetId = pendingActiveMapSetId.trim();
                uiObject->setProperty ("activeMapSetId", desiredSetId);

                if (auto* keyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
                {
                    for (auto& setVar : *keyswitchSets)
                    {
                        if (auto* setObject = setVar.getDynamicObject())
                        {
                            const auto candidateId = setObject->getProperty ("id").toString().trim();
                            setObject->setProperty ("active", candidateId.isNotEmpty() && candidateId == desiredSetId);
                        }
                    }
                }

                uiSessionStateJson = juce::JSON::toString (parsed);
                uiSessionStateLightweightJson = makeLightweightSessionStateJson (uiSessionStateJson, nullptr);
                uiSessionStateLightweightVersion.fetch_add (1, std::memory_order_relaxed);
                writeLoadDebugLog ("active map persisted to session json | setId=" + desiredSetId);
            }
        }

        pendingActiveMapSetId.clear();
    }

    if (! lightweightPreferred)
        return uiSessionStateJson;

    if (uiSessionStateLightweightJson.isNotEmpty())
        return uiSessionStateLightweightJson;

    return uiSessionStateJson;
}

juce::String SamplePlayerAudioProcessor::getSampleDataUrlForMapEntry (int rootMidi,
                                                                      int velocityLayer,
                                                                      int rrIndex,
                                                                      const juce::String& fileName) const
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);
    if (sampleSet == nullptr || sampleSet->zones.empty())
        return {};

    const int targetRoot = juce::jlimit (0, 127, rootMidi);
    const int targetVelocityLayer = juce::jmax (1, velocityLayer);
    const int targetRr = juce::jmax (1, rrIndex);
    const auto targetFileName = extractFileNameFromPathString (fileName).trim();

    const auto isPlayableZone = [] (const auto& zonePtr) -> bool
    {
        return zonePtr != nullptr
            && zonePtr->audio.getNumSamples() > 0
            && zonePtr->audio.getNumChannels() > 0;
    };

    const auto findZoneByFileName = [&] (const auto& zones) -> std::shared_ptr<const SampleZone>
    {
        if (targetFileName.isEmpty())
            return {};

        for (const auto& zone : zones)
        {
            if (! isPlayableZone (zone))
                continue;
            if (zone->sourceFile.getFileName().equalsIgnoreCase (targetFileName))
                return zone;
        }

        return {};
    };

    const auto findClosestZoneByRoot = [&]() -> std::shared_ptr<const SampleZone>
    {
        std::shared_ptr<const SampleZone> closestZone;
        int closestDistance = std::numeric_limits<int>::max();

        for (const auto& zone : sampleSet->zones)
        {
            if (! isPlayableZone (zone))
                continue;

            const int distance = std::abs (zone->metadata.rootNote - targetRoot);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestZone = zone;
            }
        }

        return closestZone;
    };

    std::vector<std::shared_ptr<const SampleZone>> rootZones;
    rootZones.reserve (sampleSet->zones.size());
    for (const auto& zone : sampleSet->zones)
    {
        if (zone == nullptr)
            continue;
        if (zone->metadata.rootNote == targetRoot)
            rootZones.push_back (zone);
    }

    if (rootZones.empty())
    {
        if (const auto byFileName = findZoneByFileName (sampleSet->zones); byFileName != nullptr)
            return encodeAudioBufferToWavDataUrl (byFileName->audio, byFileName->sourceSampleRate);

        if (const auto closest = findClosestZoneByRoot(); closest != nullptr)
            return encodeAudioBufferToWavDataUrl (closest->audio, closest->sourceSampleRate);

        return {};
    }

    std::vector<std::pair<int, int>> velocityRanges;
    velocityRanges.reserve (rootZones.size());
    for (const auto& zone : rootZones)
    {
        const std::pair<int, int> bounds { zone->metadata.lowVelocity, zone->metadata.highVelocity };
        if (std::find (velocityRanges.begin(), velocityRanges.end(), bounds) == velocityRanges.end())
            velocityRanges.push_back (bounds);
    }

    std::sort (velocityRanges.begin(), velocityRanges.end(), [] (const auto& a, const auto& b)
    {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second < b.second;
    });

    int targetLowVelocity = -1;
    int targetHighVelocity = -1;
    if (! velocityRanges.empty())
    {
        const int layerIndex = juce::jlimit (0,
                                             static_cast<int> (velocityRanges.size()) - 1,
                                             targetVelocityLayer - 1);
        targetLowVelocity = velocityRanges[static_cast<size_t> (layerIndex)].first;
        targetHighVelocity = velocityRanges[static_cast<size_t> (layerIndex)].second;
    }

    std::shared_ptr<const SampleZone> bestZone;
    int bestScore = std::numeric_limits<int>::min();

    for (const auto& zone : rootZones)
    {
        int score = 0;

        if (zone->metadata.lowVelocity == targetLowVelocity
            && zone->metadata.highVelocity == targetHighVelocity)
            score += 40;

        if (zone->metadata.roundRobinIndex == targetRr)
            score += 30;

        if (targetFileName.isNotEmpty()
            && zone->sourceFile.getFileName().equalsIgnoreCase (targetFileName))
            score += 60;

        if (score > bestScore)
        {
            bestScore = score;
            bestZone = zone;
        }
    }

    if (bestZone == nullptr || ! isPlayableZone (bestZone))
    {
        if (const auto byFileNameRoot = findZoneByFileName (rootZones); byFileNameRoot != nullptr)
            bestZone = byFileNameRoot;
        else if (const auto byFileNameGlobal = findZoneByFileName (sampleSet->zones); byFileNameGlobal != nullptr)
            bestZone = byFileNameGlobal;
        else
        {
            for (const auto& zone : rootZones)
            {
                if (! isPlayableZone (zone))
                    continue;
                bestZone = zone;
                break;
            }

            if (bestZone == nullptr)
                bestZone = findClosestZoneByRoot();
        }
    }

    if (! isPlayableZone (bestZone))
        return {};

    return encodeAudioBufferToWavDataUrl (bestZone->audio, bestZone->sourceSampleRate);
}

juce::String SamplePlayerAudioProcessor::getSampleDataUrlForAbsolutePath (const juce::String& absolutePath,
                                                                          const juce::String& fileNameHint) const
{
    auto normalized = resolveAutoSamplerDestinationPath (absolutePath).trim().replaceCharacter ('\\', '/');
    juce::File sourceFile;
    if (normalized.startsWith ("~/") || normalized.startsWith ("~\\"))
        normalized = resolveAutoSamplerDestinationPath (normalized);
    if (juce::File::isAbsolutePath (normalized))
        sourceFile = juce::File (normalized);

    if (! sourceFile.existsAsFile())
    {
        const auto sampleSet = std::atomic_load (&currentSampleSet);
        if (sampleSet != nullptr)
        {
            const auto targetFile = extractFileNameFromPathString (fileNameHint).trim();
            if (targetFile.isNotEmpty())
            {
                for (const auto& zone : sampleSet->zones)
                {
                    if (zone == nullptr)
                        continue;

                    if (zone->audio.getNumSamples() <= 0 || zone->audio.getNumChannels() <= 0)
                        continue;

                    if (zone->sourceFile.getFileName().equalsIgnoreCase (targetFile))
                        return encodeAudioBufferToWavDataUrl (zone->audio, zone->sourceSampleRate);
                }
            }
        }

        const auto targetFile = extractFileNameFromPathString (fileNameHint).trim();
        if (targetFile.isNotEmpty())
        {
            const auto findFileByNameRecursive = [] (const juce::File& rootDir,
                                                     const juce::String& targetFileName,
                                                     int maxDepth) -> juce::File
            {
                if (! rootDir.isDirectory() || targetFileName.isEmpty() || maxDepth < 0)
                    return {};

                juce::Array<juce::File> currentDirs;
                currentDirs.add (rootDir);

                for (int depth = 0; depth <= maxDepth && currentDirs.size() > 0; ++depth)
                {
                    juce::Array<juce::File> nextDirs;
                    for (const auto& dir : currentDirs)
                    {
                        if (! dir.isDirectory())
                            continue;

                        juce::DirectoryIterator fileIt (dir, false, "*", juce::File::findFiles);
                        while (fileIt.next())
                        {
                            const auto file = fileIt.getFile();
                            if (file.getFileName().equalsIgnoreCase (targetFileName))
                                return file;
                        }

                        if (depth >= maxDepth)
                            continue;

                        juce::DirectoryIterator dirIt (dir, false, "*", juce::File::findDirectories);
                        while (dirIt.next())
                        {
                            const auto childDir = dirIt.getFile();
                            if (childDir.isDirectory())
                                nextDirs.add (childDir);
                        }
                    }

                    currentDirs.swapWith (nextDirs);
                }

                return {};
            };

            juce::Array<juce::File> broadRoots;
            const auto addBroadRoot = [&broadRoots] (const juce::File& candidateDir)
            {
                if (! candidateDir.isDirectory())
                    return;

                for (const auto& existing : broadRoots)
                {
                    if (existing == candidateDir)
                        return;
                }

                broadRoots.add (candidateDir);
            };

            const auto userHomeDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
            addBroadRoot (userHomeDir.getChildFile ("Desktop"));
            addBroadRoot (userHomeDir.getChildFile ("Documents"));
            addBroadRoot (userHomeDir.getChildFile ("Downloads"));
            addBroadRoot (userHomeDir.getChildFile ("Music"));

            for (const auto& rootDir : broadRoots)
            {
                const auto hit = findFileByNameRecursive (rootDir, targetFile, 7);
                if (hit.existsAsFile())
                {
                    sourceFile = hit;
                    break;
                }
            }
        }
    }

    if (! sourceFile.existsAsFile())
    {
        writeLoadDebugLog ("sample_data absolute fallback miss | path=" + normalized
                           + " | fileHint=" + fileNameHint);
        return {};
    }

    juce::AudioFormatManager localFormatManager;
    localFormatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (localFormatManager.createReaderFor (sourceFile));
    if (reader == nullptr || reader->lengthInSamples < 2)
        return {};

    const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
    const auto sampleCount64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                        static_cast<juce::int64> (std::numeric_limits<int>::max()));
    const int sampleCount = static_cast<int> (sampleCount64);
    if (sampleCount < 2)
        return {};

    juce::AudioBuffer<float> buffer;
    buffer.setSize (channels, sampleCount);
    reader->read (&buffer, 0, sampleCount, 0, true, true);
    const double sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    return encodeAudioBufferToWavDataUrl (buffer, sampleRate);
}

void SamplePlayerAudioProcessor::queuePreviewMidiEvent (bool noteOn,
                                                        int midiNote,
                                                        int velocity127,
                                                        int midiChannel)
{
    PendingPreviewMidiEvent event;
    event.isController = false;
    event.noteOn = noteOn;
    event.midiNote = juce::jlimit (0, 127, midiNote);
    event.velocity127 = juce::jlimit (1, 127, velocity127);
    event.midiChannel = juce::jlimit (1, 16, midiChannel);

    const juce::ScopedLock lock (pendingPreviewMidiLock);
    pendingPreviewMidiEvents.push_back (event);

    constexpr std::size_t maxPendingPreviewMidiEvents = 256;
    if (pendingPreviewMidiEvents.size() > maxPendingPreviewMidiEvents)
    {
        const auto eraseCount = pendingPreviewMidiEvents.size() - maxPendingPreviewMidiEvents;
        pendingPreviewMidiEvents.erase (pendingPreviewMidiEvents.begin(),
                                        pendingPreviewMidiEvents.begin() + static_cast<std::ptrdiff_t> (eraseCount));
    }
}

void SamplePlayerAudioProcessor::queuePreviewControllerEvent (int controllerNumber,
                                                              int controllerValue,
                                                              int midiChannel)
{
    PendingPreviewMidiEvent event;
    event.isController = true;
    event.controllerNumber = juce::jlimit (0, 127, controllerNumber);
    event.controllerValue = juce::jlimit (0, 127, controllerValue);
    event.midiChannel = juce::jlimit (1, 16, midiChannel);

    const juce::ScopedLock lock (pendingPreviewMidiLock);
    pendingPreviewMidiEvents.push_back (event);

    constexpr std::size_t maxPendingPreviewMidiEvents = 256;
    if (pendingPreviewMidiEvents.size() > maxPendingPreviewMidiEvents)
    {
        const auto eraseCount = pendingPreviewMidiEvents.size() - maxPendingPreviewMidiEvents;
        pendingPreviewMidiEvents.erase (pendingPreviewMidiEvents.begin(),
                                        pendingPreviewMidiEvents.begin() + static_cast<std::ptrdiff_t> (eraseCount));
    }
}

std::pair<juce::uint64, juce::uint64> SamplePlayerAudioProcessor::getHeldMidiMaskForUi() const noexcept
{
    return {
        midiHeldMaskLo.load (std::memory_order_relaxed),
        midiHeldMaskHi.load (std::memory_order_relaxed)
    };
}

std::pair<float, float> SamplePlayerAudioProcessor::getPerformanceWheelValuesForUi() const noexcept
{
    return {
        juce::jlimit (0.0f, 1.0f, modwheelVelocityLayerControlValue01.load (std::memory_order_relaxed)),
        juce::jlimit (0.0f, 1.0f, expressionControllerValue01.load (std::memory_order_relaxed))
    };
}

int SamplePlayerAudioProcessor::getActiveMapSetSlotForUi() const noexcept
{
    return juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));
}

int SamplePlayerAudioProcessor::getSequencerCurrentStepForUi() const noexcept
{
    return sequencerCurrentStepForUi.load (std::memory_order_relaxed);
}

void SamplePlayerAudioProcessor::setSequencerHostTriggerEnabled (bool enabled)
{
    auto updatedRuntime = std::make_shared<StepSequencerRuntime>();
    if (auto currentRuntime = std::atomic_load (&stepSequencerRuntime); currentRuntime != nullptr)
    {
        updatedRuntime->steps = currentRuntime->steps;
        updatedRuntime->doubling = currentRuntime->doubling;
        updatedRuntime->rateIndex = currentRuntime->rateIndex;
        updatedRuntime->followsInputNote = currentRuntime->followsInputNote;
    }

    updatedRuntime->doubling = sequencerDoublingEnabled.load (std::memory_order_relaxed) || updatedRuntime->doubling;

    updatedRuntime->enabled = enabled;
    updatedRuntime->currentStep = -1;
    updatedRuntime->triggerToPlayedNote.fill (-1);
    updatedRuntime->triggerDepthByMidi.fill (0);
    updatedRuntime->playedDepthByMidi.fill (0);
    std::atomic_store (&stepSequencerRuntime, updatedRuntime);

    sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
    resetVoicesRequested.store (true);
}

void SamplePlayerAudioProcessor::applyStrumSettingsFromUi (const juce::var& payload)
{
    bool enabled = true;
    bool doubling = false;
    int uiRateIndex = 2;
    int swingPercent = 0;
    int velocityHumanizePercent = 0;
    int timingHumanizeMs = 0;
    std::array<StepSequencerRuntime::Step, 8> strumSteps {};
    int parsedStepCount = 0;

    for (auto& step : strumSteps)
    {
        step.noteMidi = 60;
        step.velocity127 = 100;
        step.keyswitchSlot = -1;
        step.rateIndex = 2;
        step.subVelocities = { 100, 100, 100, 100, 100, 100, 100, 100 };
    }

    if (const auto* object = payload.getDynamicObject())
    {
        const auto enabledVar = object->getProperty ("enabled");
        if (! enabledVar.isVoid())
            enabled = static_cast<bool> (enabledVar);

        const auto doublingVar = object->getProperty ("doubling");
        if (! doublingVar.isVoid())
            doubling = static_cast<bool> (doublingVar);

        uiRateIndex = juce::jlimit (0, 3, static_cast<int> (object->getProperty ("rateIndex")));
        swingPercent = juce::jlimit (0, 20, static_cast<int> (object->getProperty ("swingPercent")));
        velocityHumanizePercent = juce::jlimit (0, 20, static_cast<int> (object->getProperty ("velocityHumanizePercent")));
        timingHumanizeMs = juce::jlimit (0, 10, static_cast<int> (object->getProperty ("timingHumanizeMs")));

        if (const auto* stepPattern = object->getProperty ("stepPattern").getArray())
        {
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int stepCount = juce::jmin (static_cast<int> (strumSteps.size()), stepPattern->size());

            for (int i = 0; i < stepCount; ++i)
            {
                const auto* stepObj = (*stepPattern)[i].getDynamicObject();
                if (stepObj == nullptr)
                    continue;

                auto& step = strumSteps[static_cast<size_t> (i)];

                const int rateIndex = juce::jlimit (0, 4, static_cast<int> (stepObj->getProperty ("rateIndex")));
                step.rateIndex = rateIndex;
                if (rateIndex >= 4)
                    step.velocity127 = 0; // Rest
                else
                    step.velocity127 = juce::jlimit (1, 127, static_cast<int> (stepObj->getProperty ("velocity")));

                step.subVelocities.fill (step.velocity127 > 0 ? step.velocity127 : 100);
                if (const auto* subVelocities = stepObj->getProperty ("subVelocities").getArray())
                {
                    const int count = juce::jmin (static_cast<int> (step.subVelocities.size()), subVelocities->size());
                    for (int subIndex = 0; subIndex < count; ++subIndex)
                    {
                        step.subVelocities[static_cast<size_t> (subIndex)] = juce::jlimit (1, 127,
                            static_cast<int> ((*subVelocities)[subIndex]));
                    }
                }

                const auto keyswitchId = stepObj->getProperty ("keyswitchSetId").toString().trim();
                if (sampleSet != nullptr && keyswitchId.isNotEmpty())
                {
                    if (const auto it = sampleSet->mapSetSlotById.find (keyswitchId.toStdString());
                        it != sampleSet->mapSetSlotById.end())
                    {
                        step.keyswitchSlot = it->second;
                    }
                }
            }

            parsedStepCount = stepCount;
        }
    }

    strumDoublingEnabled.store (doubling, std::memory_order_relaxed);

    const int activeStepCount = juce::jmax (1, parsedStepCount);
    auto runtime = std::make_shared<StepSequencerRuntime>();
    runtime->enabled = enabled;
    runtime->followsInputNote = true;
    runtime->doubling = doubling;
    runtime->rateIndex = uiRateIndex;
    runtime->swingPercent = swingPercent;
    runtime->velocityHumanizePercent = velocityHumanizePercent;
    runtime->timingHumanizeMs = timingHumanizeMs;
    runtime->randomState = juce::uint32 (juce::Time::getMillisecondCounter());

    for (size_t i = 0; i < runtime->steps.size(); ++i)
        runtime->steps[i] = strumSteps[i % static_cast<size_t> (activeStepCount)];

    // Carry over live trigger state from the old runtime so held notes
    // are not orphaned when the user tweaks swing, rate, etc.
    auto oldRuntime = std::atomic_load (&strumSequencerRuntime);
    const bool oldWasEnabled = oldRuntime != nullptr && oldRuntime->enabled;

    if (enabled && oldWasEnabled)
    {
        runtime->triggerDepthByMidi   = oldRuntime->triggerDepthByMidi;
        runtime->triggerChannelByMidi = oldRuntime->triggerChannelByMidi;
        runtime->triggerToPlayedNote  = oldRuntime->triggerToPlayedNote;
        runtime->playedDepthByMidi    = oldRuntime->playedDepthByMidi;
        runtime->currentStep          = oldRuntime->currentStep;
        runtime->currentSubdivision   = oldRuntime->currentSubdivision;
        runtime->samplesUntilNextStep    = oldRuntime->samplesUntilNextStep;
        runtime->samplesUntilNextSubstep = oldRuntime->samplesUntilNextSubstep;

        // Clamp currentStep to the new step count
        if (runtime->currentStep >= activeStepCount)
        {
            runtime->currentStep = 0;
            runtime->currentSubdivision = 0;
        }
    }
    else
    {
        runtime->samplesUntilNextStep = 0;
        runtime->samplesUntilNextSubstep = 0;
        runtime->currentStep = -1;
        runtime->currentSubdivision = 0;
        runtime->triggerToPlayedNote.fill (-1);
        runtime->triggerDepthByMidi.fill (0);
        runtime->triggerChannelByMidi.fill (1);
        runtime->playedDepthByMidi.fill (0);
    }

    std::atomic_store (&strumSequencerRuntime, runtime);
    sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);

    // Only kill voices when strum is being disabled (not on setting tweaks)
    if (! enabled && oldWasEnabled)
        resetVoicesRequested.store (true);
}

void SamplePlayerAudioProcessor::applySequencerSettingsFromUi (const juce::var& payload)
{
    bool doubling = sequencerDoublingEnabled.load (std::memory_order_relaxed);
    int uiRateIndex = 2;
    std::array<StepSequencerRuntime::Step, 16> sequencerSteps {};

    for (auto& step : sequencerSteps)
    {
        step.noteMidi = 60;
        step.velocity127 = 100;
        step.keyswitchSlot = -1;
        step.rateIndex = 2;
        step.subVelocities = { 100, 100, 100, 100, 100, 100, 100, 100 };
    }

    if (auto currentRuntime = std::atomic_load (&stepSequencerRuntime); currentRuntime != nullptr)
        sequencerSteps = currentRuntime->steps;

    if (const auto* object = payload.getDynamicObject())
    {
        const auto doublingVar = object->getProperty ("doubling");
        if (! doublingVar.isVoid())
            doubling = static_cast<bool> (doublingVar);

        uiRateIndex = juce::jlimit (0, 3, static_cast<int> (object->getProperty ("rateIndex")));

        if (const auto* stepPattern = object->getProperty ("stepPattern").getArray())
        {
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int stepCount = juce::jmin (static_cast<int> (sequencerSteps.size()), stepPattern->size());

            for (int i = 0; i < stepCount; ++i)
            {
                const auto* stepObj = (*stepPattern)[i].getDynamicObject();
                if (stepObj == nullptr)
                    continue;

                auto& step = sequencerSteps[static_cast<size_t> (i)];
                step.noteMidi = juce::jlimit (0, 127, static_cast<int> (stepObj->getProperty ("noteMidi")));
                step.velocity127 = juce::jlimit (1, 127, static_cast<int> (stepObj->getProperty ("velocity")));
                step.rateIndex = juce::jlimit (0, 4, static_cast<int> (stepObj->getProperty ("rateIndex")));
                step.keyswitchSlot = -1;

                const auto keyswitchId = stepObj->getProperty ("keyswitchSetId").toString().trim();
                if (sampleSet != nullptr && keyswitchId.isNotEmpty())
                {
                    if (const auto it = sampleSet->mapSetSlotById.find (keyswitchId.toStdString());
                        it != sampleSet->mapSetSlotById.end())
                    {
                        step.keyswitchSlot = it->second;
                    }
                }
            }
        }
    }

    sequencerDoublingEnabled.store (doubling, std::memory_order_relaxed);

    auto updatedRuntime = std::make_shared<StepSequencerRuntime>();
    if (auto currentRuntime = std::atomic_load (&stepSequencerRuntime); currentRuntime != nullptr)
    {
        updatedRuntime->enabled           = currentRuntime->enabled;
        updatedRuntime->followsInputNote  = currentRuntime->followsInputNote;
        updatedRuntime->steps             = sequencerSteps;

        // Carry over live trigger / ratchet state
        updatedRuntime->triggerDepthByMidi   = currentRuntime->triggerDepthByMidi;
        updatedRuntime->triggerChannelByMidi = currentRuntime->triggerChannelByMidi;
        updatedRuntime->triggerToPlayedNote  = currentRuntime->triggerToPlayedNote;
        updatedRuntime->playedDepthByMidi    = currentRuntime->playedDepthByMidi;
        updatedRuntime->currentStep          = currentRuntime->currentStep;
        updatedRuntime->currentSubdivision   = currentRuntime->currentSubdivision;
        updatedRuntime->samplesUntilNextStep    = currentRuntime->samplesUntilNextStep;
        updatedRuntime->samplesUntilNextSubstep = currentRuntime->samplesUntilNextSubstep;
        updatedRuntime->randomState          = currentRuntime->randomState;

        // Carry over ratchet state
        updatedRuntime->ratchetNote          = currentRuntime->ratchetNote;
        updatedRuntime->ratchetVelocity127   = currentRuntime->ratchetVelocity127;
        updatedRuntime->ratchetChannel       = currentRuntime->ratchetChannel;
        updatedRuntime->ratchetPlaybackSlot  = currentRuntime->ratchetPlaybackSlot;
        updatedRuntime->ratchetKeyswitchSlot = currentRuntime->ratchetKeyswitchSlot;
        updatedRuntime->ratchetSubsRemaining = currentRuntime->ratchetSubsRemaining;
        updatedRuntime->ratchetDoubling      = currentRuntime->ratchetDoubling;
    }
    else
    {
        updatedRuntime->steps = sequencerSteps;
    }

    updatedRuntime->doubling  = doubling;
    updatedRuntime->rateIndex = uiRateIndex;

    std::atomic_store (&stepSequencerRuntime, updatedRuntime);
}

void SamplePlayerAudioProcessor::syncSampleSetFromSessionStateJson (const juce::var& parsedRoot, juce::int64 payloadBytes, int requestId)
{
    const auto syncStartMs = juce::Time::getMillisecondCounterHiRes();

    const auto logExit = [requestId, syncStartMs] (const juce::String& reason)
    {
        writeLoadDebugLog ("session sync exit | requestId=" + juce::String (requestId)
                           + " | reason=" + reason
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (syncStartMs), 2));
    };

    const auto isStaleRequest = [this, requestId]() -> bool
    {
        return requestId >= 0
            && requestId != sessionStateSyncRequestId.load (std::memory_order_relaxed);
    };

    if (isStaleRequest())
    {
        logExit ("stale-before-start");
        return;
    }

    const auto& parsed = parsedRoot;
    const auto* rootObject = parsed.getDynamicObject();
    if (rootObject == nullptr)
    {
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "invalid-json");
        logExit ("invalid-json");
        return;
    }

    const auto* manifestObject = rootObject->getProperty ("manifest").getDynamicObject();
    if (manifestObject == nullptr)
    {
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "missing-manifest");
        logExit ("missing-manifest");
        return;
    }

    const double parseMs = 0.0;

    bool allowPitchUpAboveHighest = false;
    bool useModwheelForVelocityLayers = false;
    bool baseLoopPlaybackEnabled = true;
    juce::String basePlaybackMode = normalizeKeyswitchPlaybackMode (
        manifestObject->getProperty ("settings").getProperty ("basePlaybackMode", "spread").toString());
    juce::String manifestBasePath;
    juce::String autoDestinationPath;
    juce::String activeMapSetId = "base";
    float modwheelValue01 = 0.0f;
    juce::var baseManualRangesVar;
    bool sequencerHostTriggerEnabled = false;
    bool sequencerDoubling = false;
    int sequencerRateIndex = 2;

    struct UiKeyswitchState
    {
        juce::String id;
        juce::String name;
        int keyMidi = -1;
        bool loopPlaybackEnabled = true;
        float gainDb = 0.0f;
        bool oneShotPlayback = false;
        KeyswitchTriggerMode triggerMode = KeyswitchTriggerMode::manual;
        juce::var manualRangesVar;
    };

    struct UiSequencerStepState
    {
        int noteMidi = 60;
        int velocity127 = 100;
        int rateIndex = 2;
        juce::String keyswitchSetId;
    };

    std::vector<UiKeyswitchState> uiKeyswitchStates;
    std::array<UiSequencerStepState, 16> uiSequencerSteps;

    if (const auto* uiObject = rootObject->getProperty ("ui").getDynamicObject())
    {
        allowPitchUpAboveHighest = static_cast<bool> (uiObject->getProperty ("allowPitchUpAboveHighest"));
        if (const auto* autoObject = uiObject->getProperty ("auto").getDynamicObject())
        {
            useModwheelForVelocityLayers = static_cast<bool> (autoObject->getProperty ("modwheelVelocityControl"));
            autoDestinationPath = resolveAutoSamplerDestinationPath (autoObject->getProperty ("destination").toString());
        }

        if (const auto* sequencerObject = uiObject->getProperty ("sequencer").getDynamicObject())
        {
            const auto sequencerEnabledVar = sequencerObject->getProperty ("hostTriggerEnabled");
            if (! sequencerEnabledVar.isVoid())
                sequencerHostTriggerEnabled = static_cast<bool> (sequencerEnabledVar);

            const auto sequencerDoublingVar = sequencerObject->getProperty ("doubling");
            if (! sequencerDoublingVar.isVoid())
                sequencerDoubling = static_cast<bool> (sequencerDoublingVar);

            const auto sequencerRateVar = sequencerObject->getProperty ("rateIndex");
            if (! sequencerRateVar.isVoid())
                sequencerRateIndex = juce::jlimit (0, 3, varToInt (sequencerRateVar, 2));

            if (const auto* sequencerStepsArray = sequencerObject->getProperty ("steps").getArray())
            {
                const int stepCount = juce::jmin (static_cast<int> (uiSequencerSteps.size()),
                                                  sequencerStepsArray->size());
                for (int i = 0; i < stepCount; ++i)
                {
                    const auto* stepObject = (*sequencerStepsArray)[i].getDynamicObject();
                    if (stepObject == nullptr)
                        continue;

                    auto& step = uiSequencerSteps[static_cast<size_t> (i)];
                    int noteMidi = varToInt (stepObject->getProperty ("noteMidi"), step.noteMidi);
                    if (noteMidi < 0 || noteMidi > 127)
                    {
                        int parsedFromToken = -1;
                        if (parseNoteToken (stepObject->getProperty ("note").toString().trim(), parsedFromToken))
                            noteMidi = parsedFromToken;
                    }

                    step.noteMidi = juce::jlimit (0, 127, noteMidi);
                    step.velocity127 = juce::jlimit (1, 127, varToInt (stepObject->getProperty ("velocity"), step.velocity127));
                    step.rateIndex = juce::jlimit (0, 4, varToInt (stepObject->getProperty ("rateIndex"), step.rateIndex));
                    step.keyswitchSetId = stepObject->getProperty ("keyswitchSetId").toString().trim();
                }
            }
        }

        const auto baseLoopPlaybackEnabledVar = uiObject->getProperty ("baseLoopPlaybackEnabled");
        if (! baseLoopPlaybackEnabledVar.isVoid())
            baseLoopPlaybackEnabled = static_cast<bool> (baseLoopPlaybackEnabledVar);
        const auto basePlaybackModeVar = uiObject->getProperty ("basePlaybackMode");
        if (! basePlaybackModeVar.isVoid())
            basePlaybackMode = normalizeKeyswitchPlaybackMode (basePlaybackModeVar.toString());
        manifestBasePath = resolveAutoSamplerDestinationPath (uiObject->getProperty ("manifestBasePath").toString());
        baseManualRangesVar = uiObject->getProperty ("manualRootRanges");
        activeMapSetId = uiObject->getProperty ("activeMapSetId").toString().trim();
        if (activeMapSetId.isEmpty())
            activeMapSetId = "base";

        if (const auto* uiKeyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
        {
            for (int i = 0; i < uiKeyswitchSets->size(); ++i)
            {
                const auto* setObject = (*uiKeyswitchSets)[i].getDynamicObject();
                if (setObject == nullptr)
                    continue;

                UiKeyswitchState state;
                state.id = setObject->getProperty ("id").toString().trim();
                state.name = setObject->getProperty ("name").toString().trim();
                state.manualRangesVar = setObject->getProperty ("manualRanges");
                const auto loopPlaybackEnabledVar = setObject->getProperty ("loopPlaybackEnabled");
                if (! loopPlaybackEnabledVar.isVoid())
                    state.loopPlaybackEnabled = static_cast<bool> (loopPlaybackEnabledVar);
                state.gainDb = juce::jlimit (-24.0f,
                                             24.0f,
                                             static_cast<float> (varToDouble (setObject->getProperty ("gainDb"),
                                                                              0.0)));
                state.triggerMode = getKeyswitchTriggerModeFromName (state.name);
                state.oneShotPlayback = resolveKeyswitchOneShotPlayback (state.name,
                                                                        setObject->getProperty ("playbackMode").toString());

                const auto keyMidiVar = setObject->getProperty ("keyMidi");
                if (! keyMidiVar.isVoid())
                {
                    const int parsedMidi = varToInt (keyMidiVar, -1);
                    if (parsedMidi >= 0 && parsedMidi <= 127)
                        state.keyMidi = parsedMidi;
                }
                if (state.keyMidi < 0)
                {
                    auto keyText = setObject->getProperty ("key").toString().trim();
                    if (keyText.isEmpty())
                        keyText = setObject->getProperty ("keyswitchKey").toString().trim();
                    int parsedMidi = -1;
                    if (parseNoteToken (keyText, parsedMidi))
                        state.keyMidi = parsedMidi;
                }

                if (activeMapSetId.isEmpty() && static_cast<bool> (setObject->getProperty ("active")))
                    activeMapSetId = state.id;

                uiKeyswitchStates.push_back (state);
            }
        }
    }

    const bool ccArmedModwheelVelocity = modwheelVelocityLayerControlSeenFromMidi.load (std::memory_order_relaxed);
    modwheelVelocityLayerControlEnabled.store (useModwheelForVelocityLayers || ccArmedModwheelVelocity,
                                               std::memory_order_relaxed);
    modwheelVelocityLayerControlValue01.store (modwheelValue01, std::memory_order_relaxed);
    sequencerDoublingEnabled.store (sequencerDoubling, std::memory_order_relaxed);
    if (auto* modParam = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (kModWheelParamId)))
        modParam->setValue (juce::jlimit (0.0f, 1.0f, modwheelValue01));

    struct MapSetDescriptor
    {
        juce::String id = "base";
        juce::String name;
        int slot = 0;
        int keyswitchMidi = -1;
        bool loopPlaybackEnabled = true;
        float gainDb = 0.0f;
        bool oneShotPlayback = false;
        KeyswitchTriggerMode triggerMode = KeyswitchTriggerMode::manual;
        juce::var manualRangesVar;
        const juce::Array<juce::var>* mappingArray = nullptr;
    };

    std::vector<MapSetDescriptor> mapSets;
    if (const auto* baseMappingArray = manifestObject->getProperty ("mapping").getArray();
        baseMappingArray != nullptr && ! baseMappingArray->isEmpty())
    {
        MapSetDescriptor baseSet;
        baseSet.id = "base";
        baseSet.name = "base";
        baseSet.slot = 0;
        baseSet.keyswitchMidi = -1;
        baseSet.loopPlaybackEnabled = baseLoopPlaybackEnabled;
        baseSet.oneShotPlayback = (! baseLoopPlaybackEnabled)
            && normalizeKeyswitchPlaybackMode (basePlaybackMode) == "oneshot";
        baseSet.manualRangesVar = pickManualRangesVar (
            baseManualRangesVar,
            manifestObject->getProperty ("settings").getProperty ("manualRootRanges", juce::var {}),
            baseMappingArray);
        baseSet.mappingArray = baseMappingArray;
        mapSets.push_back (baseSet);
    }

    if (const auto* manifestKeyswitchSets = manifestObject->getProperty ("keyswitchSets").getArray())
    {
        for (int i = 0; i < manifestKeyswitchSets->size(); ++i)
        {
            const auto* keyswitchObject = (*manifestKeyswitchSets)[i].getDynamicObject();
            if (keyswitchObject == nullptr)
                continue;

            const auto* mappingArray = keyswitchObject->getProperty ("mapping").getArray();
            if (mappingArray == nullptr || mappingArray->isEmpty())
                continue;

            MapSetDescriptor set;
            set.slot = i + 1;
            set.id = "keyswitch_" + juce::String (i + 1);
            set.name = keyswitchObject->getProperty ("name").toString().trim();
            set.keyswitchMidi = -1;
            set.loopPlaybackEnabled = baseLoopPlaybackEnabled;
            set.gainDb = juce::jlimit (-24.0f,
                                       24.0f,
                                       static_cast<float> (varToDouble (keyswitchObject->getProperty ("gainDb"),
                                                                        0.0)));
            set.triggerMode = getKeyswitchTriggerModeFromName (set.name);
            set.oneShotPlayback = resolveKeyswitchOneShotPlayback (set.name,
                                                                  keyswitchObject->getProperty ("playbackMode").toString());
            set.manualRangesVar = pickManualRangesVar (
                juce::var {},
                keyswitchObject->getProperty ("manualRanges"),
                mappingArray);
            set.mappingArray = mappingArray;

            if (juce::isPositiveAndBelow (i, static_cast<int> (uiKeyswitchStates.size())))
            {
                const auto& uiState = uiKeyswitchStates[static_cast<size_t> (i)];
                if (uiState.id.isNotEmpty())
                    set.id = uiState.id;
                if (uiState.name.isNotEmpty())
                    set.name = uiState.name;
                set.keyswitchMidi = uiState.keyMidi;
                set.loopPlaybackEnabled = uiState.loopPlaybackEnabled;
                set.gainDb = uiState.gainDb;
                set.oneShotPlayback = uiState.oneShotPlayback;
                set.triggerMode = uiState.triggerMode;
                set.manualRangesVar = pickManualRangesVar (
                    uiState.manualRangesVar,
                    keyswitchObject->getProperty ("manualRanges"),
                    mappingArray);
            }

            if (set.keyswitchMidi < 0)
            {
                const auto keyMidiVar = keyswitchObject->getProperty ("keyMidi");
                if (! keyMidiVar.isVoid())
                {
                    const int parsedMidi = varToInt (keyMidiVar, -1);
                    if (parsedMidi >= 0 && parsedMidi <= 127)
                        set.keyswitchMidi = parsedMidi;
                }
            }

            if (set.keyswitchMidi < 0)
            {
                auto keyText = keyswitchObject->getProperty ("keyswitchKey").toString().trim();
                if (keyText.isEmpty())
                    keyText = keyswitchObject->getProperty ("key").toString().trim();
                int parsedMidi = -1;
                if (parseNoteToken (keyText, parsedMidi))
                    set.keyswitchMidi = parsedMidi;
            }

            mapSets.push_back (set);
        }
    }

    const auto buildSequencerRuntime = [&]()
    {
        auto runtime = std::make_shared<StepSequencerRuntime>();
        runtime->enabled = sequencerHostTriggerEnabled;
        runtime->doubling = sequencerDoubling;
        runtime->rateIndex = sequencerRateIndex;

        for (size_t i = 0; i < runtime->steps.size(); ++i)
        {
            const auto& uiStep = uiSequencerSteps[i];
            auto& step = runtime->steps[i];
            step.noteMidi = juce::jlimit (0, 127, uiStep.noteMidi);
            step.velocity127 = juce::jlimit (1, 127, uiStep.velocity127);
            step.rateIndex = juce::jlimit (0, 4, uiStep.rateIndex);
            step.keyswitchSlot = -1;

            const auto targetSetId = uiStep.keyswitchSetId.trim();
            if (targetSetId.isEmpty())
                continue;

            const auto it = std::find_if (mapSets.begin(), mapSets.end(),
                                          [&targetSetId] (const auto& set) { return set.id == targetSetId; });
            if (it != mapSets.end())
                step.keyswitchSlot = it->slot;
        }

        return runtime;
    };

    if (mapSets.empty())
    {
        if (isStaleRequest())
        {
            logExit ("stale-empty-map");
            return;
        }

        auto sequencerRuntime = std::make_shared<StepSequencerRuntime>();
        sequencerRuntime->enabled = false;
        std::atomic_store (&stepSequencerRuntime, sequencerRuntime);
        sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);

        const auto signature = juce::String ("empty");
        {
            const juce::ScopedLock lock (sessionMapSyncLock);
            if (lastSessionMapSignature == signature)
            {
                finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "map-empty-unchanged");
                logExit ("map-empty-unchanged");
                return;
            }
            lastSessionMapSignature = signature;
        }

        clearSampleSet();
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "map-empty-cleared");
        logExit ("map-empty-cleared");
        return;
    }

    int activeSetSlot = 0;
    bool activeSetLoopPlaybackEnabled = baseLoopPlaybackEnabled;
    bool activeSetMatched = false;
    for (const auto& set : mapSets)
    {
        if (set.id == activeMapSetId)
        {
            activeSetSlot = set.slot;
            activeSetLoopPlaybackEnabled = set.loopPlaybackEnabled;
            activeSetMatched = true;
            break;
        }
    }
    if (! activeSetMatched)
    {
        const auto fallbackIt = std::find_if (mapSets.begin(), mapSets.end(), [] (const auto& set)
        {
            return set.slot != 0;
        });
        if (fallbackIt != mapSets.end())
        {
            activeSetSlot = fallbackIt->slot;
            activeSetLoopPlaybackEnabled = fallbackIt->loopPlaybackEnabled;
        }
        else
        {
            activeSetSlot = mapSets.front().slot;
            activeSetLoopPlaybackEnabled = mapSets.front().loopPlaybackEnabled;
        }
    }

    activeMapSetSlot.store (activeSetSlot, std::memory_order_relaxed);
    activeMapLoopPlaybackEnabled.store (activeSetLoopPlaybackEnabled, std::memory_order_relaxed);

    struct EmbeddedVariantDescriptor
    {
        int mapSetSlot = 0;
        int rootMidi = 60;
        int velocityLayer = 1;
        int rrIndex = 1;
        juce::String fileName;
        juce::String sampleDataUrl;
        juce::String samplePath;
    };

    const auto mapBuildStartMs = juce::Time::getMillisecondCounterHiRes();
    std::vector<EmbeddedVariantDescriptor> variants;
    size_t totalMappingBuckets = 0;
    for (const auto& set : mapSets)
        totalMappingBuckets += static_cast<size_t> (set.mappingArray != nullptr ? set.mappingArray->size() : 0);
    variants.reserve (juce::jmax<size_t> (1, totalMappingBuckets * 2));

    std::unordered_map<int, std::vector<int>> rootsBySlot;
    std::unordered_map<int, std::unordered_map<int, std::vector<int>>> layersBySlotRoot;

    juce::uint64 hash = 1469598103934665603ULL;
    hashMix (hash, static_cast<juce::uint64> (allowPitchUpAboveHighest ? 1 : 0));
    hashMix (hash, static_cast<juce::uint64> (static_cast<int> (mapSets.size())));

    for (const auto& mapSet : mapSets)
    {
        if (mapSet.mappingArray == nullptr)
            continue;

        hashMix (hash, static_cast<juce::uint64> ((mapSet.slot + 1) * 97));
        hashMix (hash, static_cast<juce::uint64> (mapSet.id.hashCode64()));
        hashMix (hash, static_cast<juce::uint64> (mapSet.loopPlaybackEnabled ? 1 : 0));
        hashMix (hash, static_cast<juce::uint64> (mapSet.oneShotPlayback ? 1 : 0));
        hashMix (hash, static_cast<juce::uint64> (juce::jmax (0, mapSet.keyswitchMidi + 1)));
        hashMix (hash,
             static_cast<juce::uint64> (std::llround ((static_cast<double> (juce::jlimit (-24.0f, 24.0f, mapSet.gainDb))
                                    + 24.0)
                                   * 100.0)));

        auto& slotRoots = rootsBySlot[mapSet.slot];
        auto& slotLayersByRoot = layersBySlotRoot[mapSet.slot];

        for (const auto& bucketVar : *mapSet.mappingArray)
        {
            const auto* bucketObject = bucketVar.getDynamicObject();
            if (bucketObject == nullptr)
                continue;

            const int rootMidi = juce::jlimit (0, 127, varToInt (bucketObject->getProperty ("rootMidiNote"), 60));
            const int velocityLayer = juce::jlimit (1, 5, varToInt (bucketObject->getProperty ("velocityLayer"), 1));

            slotRoots.push_back (rootMidi);
            auto& rootLayers = slotLayersByRoot[rootMidi];
            if (std::find (rootLayers.begin(), rootLayers.end(), velocityLayer) == rootLayers.end())
                rootLayers.push_back (velocityLayer);

            hashMix (hash, static_cast<juce::uint64> ((mapSet.slot << 8) ^ (rootMidi + 1)));
            hashMix (hash, static_cast<juce::uint64> (velocityLayer + 17));

            const auto* variantsArray = bucketObject->getProperty ("rrVariants").getArray();
            if (variantsArray == nullptr || variantsArray->isEmpty())
                continue;

            int fallbackRr = 1;
            for (const auto& variantVar : *variantsArray)
            {
                const auto* variantObject = variantVar.getDynamicObject();
                if (variantObject == nullptr)
                    continue;

                const auto sampleDataUrl = variantObject->getProperty ("sampleDataUrl").toString().trim();
                const auto samplePath = variantObject->getProperty ("path").toString().trim();
                if (sampleDataUrl.isEmpty() && samplePath.isEmpty())
                {
                    ++fallbackRr;
                    continue;
                }

                auto fileName = variantObject->getProperty ("originalFilename").toString().trim();
                if (fileName.isEmpty())
                    fileName = extractFileNameFromPathString (variantObject->getProperty ("path").toString());
                if (fileName.isEmpty())
                    fileName = "Embedded_" + midiToNoteToken (rootMidi) + "_V" + juce::String (velocityLayer) + "_RR" + juce::String (fallbackRr) + ".wav";

                const int rrIndex = juce::jmax (1, varToInt (variantObject->getProperty ("rrIndex"), fallbackRr));

                variants.push_back ({ mapSet.slot, rootMidi, velocityLayer, rrIndex, fileName, sampleDataUrl, samplePath });

                hashMix (hash, static_cast<juce::uint64> ((mapSet.slot << 12) ^ (rrIndex + 101)));
                hashMix (hash, static_cast<juce::uint64> (samplePath.hashCode64()));
                hashMix (hash, static_cast<juce::uint64> (fileName.hashCode64()));
                ++fallbackRr;
            }
        }
    }

    if (variants.empty())
    {
        const auto existingSampleSet = std::atomic_load (&currentSampleSet);
        const int existingZoneCount = existingSampleSet != nullptr
                                    ? static_cast<int> (existingSampleSet->zones.size())
                                    : 0;

        if (existingZoneCount > 0)
        {
            const auto lightweightSignature = juce::String ("lightweight:")
                                            + hashToSignature (hash, static_cast<int> (totalMappingBuckets));
            {
                const juce::ScopedLock lock (sessionMapSyncLock);
                lastSessionMapSignature = lightweightSignature;
            }

            std::atomic_store (&stepSequencerRuntime, buildSequencerRuntime());
            sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);

            markPresetLoadPlayable ("session-sync-lightweight-retain", existingZoneCount);
            writeLoadDebugLog ("session sync retained existing sample set | requestId=" + juce::String (requestId)
                               + " | payloadBytes=" + juce::String (payloadBytes)
                               + " | parseMs=" + juce::String (parseMs, 2)
                               + " | existingZones=" + juce::String (existingZoneCount));
            logExit ("variants-empty-retain-existing");
            return;
        }

        if (isStaleRequest())
        {
            logExit ("stale-empty-variants");
            return;
        }

        const auto signature = juce::String ("empty");
        {
            const juce::ScopedLock lock (sessionMapSyncLock);
            if (lastSessionMapSignature == signature)
            {
                finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "variants-empty-unchanged");
                logExit ("variants-empty-unchanged");
                return;
            }
            lastSessionMapSignature = signature;
        }

        clearSampleSet();
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "variants-empty-cleared");
        logExit ("variants-empty-cleared");
        return;
    }

    std::unordered_map<int, std::unordered_map<int, std::pair<int, int>>> noteRangesBySlotRoot;
    std::unordered_map<int, std::unordered_map<int, std::unordered_map<int, std::pair<int, int>>>> velocityBoundsBySlotRootLayer;

    for (const auto& mapSet : mapSets)
    {
        auto rootsIt = rootsBySlot.find (mapSet.slot);
        if (rootsIt == rootsBySlot.end())
            continue;

        auto roots = rootsIt->second;
        std::sort (roots.begin(), roots.end());
        roots.erase (std::unique (roots.begin(), roots.end()), roots.end());

        const auto* manualRangesObject = mapSet.manualRangesVar.getDynamicObject();
        const auto tryReadManualRangeForRoot = [manualRangesObject] (int rootMidi,
                                                                      int defaultLow,
                                                                      int defaultHigh,
                                                                      int& outLow,
                                                                      int& outHigh) -> bool
        {
            if (manualRangesObject == nullptr)
                return false;

            const auto applyRangeObject = [defaultLow, defaultHigh, &outLow, &outHigh] (const juce::var& candidate) -> bool
            {
                const auto* rangeObject = candidate.getDynamicObject();
                if (rangeObject == nullptr)
                    return false;

                outLow = juce::jlimit (0, 127, varToInt (rangeObject->getProperty ("low"), defaultLow));
                outHigh = juce::jlimit (0, 127, varToInt (rangeObject->getProperty ("high"), defaultHigh));
                return true;
            };

            if (applyRangeObject (manualRangesObject->getProperty (juce::String (rootMidi))))
                return true;

            const auto& properties = manualRangesObject->getProperties();
            for (int i = 0; i < properties.size(); ++i)
            {
                const auto key = properties.getName (i).toString().trim();
                int parsedMidi = -1;

                if (! parseStrictInt (key, parsedMidi))
                {
                    if (! parseNoteToken (key, parsedMidi))
                        continue;
                }

                if (juce::jlimit (0, 127, parsedMidi) != rootMidi)
                    continue;

                if (applyRangeObject (properties.getValueAt (i)))
                    return true;
            }

            return false;
        };

        for (size_t i = 0; i < roots.size(); ++i)
        {
            const int root = roots[i];
            int low = (i == 0) ? root : juce::jmin (127, roots[i - 1] + 1);
            int high = root;

            if (i == roots.size() - 1 && allowPitchUpAboveHighest)
                high = 127;

            int manualLow = low;
            int manualHigh = high;
            if (tryReadManualRangeForRoot (root, low, high, manualLow, manualHigh))
            {
                low = manualLow;
                high = manualHigh;
            }

            if (low > high)
                std::swap (low, high);

            low = juce::jmin (low, root);
            high = juce::jmax (high, root);
            low = juce::jlimit (0, 127, low);
            high = juce::jlimit (low, 127, high);

            noteRangesBySlotRoot[mapSet.slot][root] = { low, high };
            hashMix (hash, static_cast<juce::uint64> ((mapSet.slot << 16) ^ (root + 409)));
            hashMix (hash, static_cast<juce::uint64> ((low << 8) | high));
        }

        auto layersIt = layersBySlotRoot.find (mapSet.slot);
        if (layersIt == layersBySlotRoot.end())
            continue;

        for (auto& [root, layers] : layersIt->second)
        {
            std::sort (layers.begin(), layers.end());
            layers.erase (std::unique (layers.begin(), layers.end()), layers.end());

            const int layerCount = juce::jmax (1, static_cast<int> (layers.size()));
            for (int i = 0; i < layerCount; ++i)
            {
                const int low0 = (i * 128) / layerCount;
                const int high0 = (((i + 1) * 128) / layerCount) - 1;
                const int lowVelocity = juce::jlimit (1, 127, low0 + 1);
                const int highVelocity = juce::jlimit (lowVelocity, 127, high0 + 1);

                velocityBoundsBySlotRootLayer[mapSet.slot][root][layers[static_cast<size_t> (i)]] = { lowVelocity, highVelocity };
                hashMix (hash, static_cast<juce::uint64> ((mapSet.slot << 24) ^ (root << 16) ^ (layers[static_cast<size_t> (i)] << 8) ^ highVelocity));
            }
        }
    }

    const auto signature = hashToSignature (hash, static_cast<int> (variants.size()));
    {
        const juce::ScopedLock lock (sessionMapSyncLock);
        if (lastSessionMapSignature == signature)
        {
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int zoneCount = sampleSet != nullptr ? static_cast<int> (sampleSet->zones.size()) : 0;
            activeMapSetSlot.store (activeSetSlot, std::memory_order_relaxed);
            activeMapLoopPlaybackEnabled.store (activeSetLoopPlaybackEnabled, std::memory_order_relaxed);
            std::atomic_store (&stepSequencerRuntime, buildSequencerRuntime());
            sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
            if (zoneCount > 0)
                markPresetLoadPlayable ("session-sync-signature-unchanged", zoneCount);
            else
                finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "signature-unchanged-no-zones");
            logExit ("signature-unchanged");
            return;
        }
    }

    if (isStaleRequest())
    {
        logExit ("stale-before-decode");
        return;
    }

    const auto mapBuildMs = elapsedMsFrom (mapBuildStartMs);

    auto newSampleSet = std::make_shared<SampleSet>();
    newSampleSet->keyswitchSlotByMidi.fill (-1);
    newSampleSet->zones.reserve (variants.size());
    newSampleSet->velocityLayersBySlotRoot = layersBySlotRoot;
    for (auto& [slot, roots] : newSampleSet->velocityLayersBySlotRoot)
    {
        juce::ignoreUnused (slot);
        for (auto& [rootNote, layers] : roots)
        {
            juce::ignoreUnused (rootNote);
            std::sort (layers.begin(), layers.end());
            layers.erase (std::unique (layers.begin(), layers.end()), layers.end());
        }
    }
    for (const auto& mapSet : mapSets)
    {
        newSampleSet->mapSetSlotById[mapSet.id.toStdString()] = mapSet.slot;
        newSampleSet->gainLinearBySlot[mapSet.slot] = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, mapSet.gainDb));
        newSampleSet->loopPlaybackBySlot[mapSet.slot] = mapSet.loopPlaybackEnabled;
        newSampleSet->oneShotPlaybackBySlot[mapSet.slot] = mapSet.oneShotPlayback;
        if (mapSet.triggerMode == KeyswitchTriggerMode::noteOn)
            newSampleSet->noteOnTriggerSlots.push_back (mapSet.slot);
        else if (mapSet.triggerMode == KeyswitchTriggerMode::noteOff)
            newSampleSet->noteOffTriggerSlots.push_back (mapSet.slot);
        if (mapSet.keyswitchMidi >= 0 && mapSet.keyswitchMidi <= 127)
        {
            newSampleSet->keyswitchSlotByMidi[static_cast<size_t> (mapSet.keyswitchMidi)] = mapSet.slot;
            newSampleSet->hasKeyswitchSets = true;
        }
    }

    const auto unpackedSampleDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                       .getChildFile ("SamplePlayer")
                                       .getChildFile ("UnpackedSamples");
    const bool unpackDirReady = unpackedSampleDir.isDirectory()
                             || unpackedSampleDir.createDirectory().wasOk();

    thread_local juce::AudioFormatManager asyncFormatManager;
    thread_local bool asyncFormatManagerReady = false;
    if (! asyncFormatManagerReady)
    {
        asyncFormatManager.registerBasicFormats();
        asyncFormatManagerReady = true;
    }

    const auto decodeStageStartMs = juce::Time::getMillisecondCounterHiRes();
    int cacheHits = 0;
    int cacheMisses = 0;
    int decodeFailures = 0;
    int decodedOnMiss = 0;
    std::size_t decodedBytesOnMiss = 0;
    int filePathLoads = 0;
    juce::StringArray unresolvedVariantPaths;

    const juce::File manifestBaseDir = (manifestBasePath.isNotEmpty() && juce::File::isAbsolutePath (manifestBasePath))
                                     ? juce::File (manifestBasePath)
                                     : juce::File {};
    const juce::File autoDestinationDir = (autoDestinationPath.isNotEmpty() && juce::File::isAbsolutePath (autoDestinationPath))
                                        ? juce::File (autoDestinationPath)
                                        : juce::File {};

    juce::Array<juce::File> samplePathSearchRoots;
    const auto addSearchRoot = [&samplePathSearchRoots] (const juce::File& candidateDir)
    {
        if (! candidateDir.isDirectory())
            return;

        for (const auto& existing : samplePathSearchRoots)
        {
            if (existing == candidateDir)
                return;
        }

        samplePathSearchRoots.add (candidateDir);
    };

    addSearchRoot (manifestBaseDir);
    addSearchRoot (autoDestinationDir);

    if (manifestBaseDir.isDirectory())
    {
        const auto manifestParent = manifestBaseDir.getParentDirectory();
        addSearchRoot (manifestParent);
        addSearchRoot (manifestParent.getChildFile ("Audio"));
        addSearchRoot (manifestParent.getChildFile ("samples"));
        addSearchRoot (manifestParent.getChildFile ("Samples"));
    }

    if (autoDestinationDir.isDirectory())
    {
        addSearchRoot (autoDestinationDir.getChildFile ("Audio"));
        addSearchRoot (autoDestinationDir.getChildFile ("samples"));
        addSearchRoot (autoDestinationDir.getChildFile ("Samples"));
        addSearchRoot (autoDestinationDir.getChildFile ("Keyswitches"));
    }

    const auto findFileByNameRecursive = [] (const juce::File& rootDir,
                                             const juce::String& targetFileName,
                                             int maxDepth) -> juce::File
    {
        if (! rootDir.isDirectory() || targetFileName.isEmpty() || maxDepth < 0)
            return {};

        juce::Array<juce::File> currentDirs;
        currentDirs.add (rootDir);

        for (int depth = 0; depth <= maxDepth && currentDirs.size() > 0; ++depth)
        {
            juce::Array<juce::File> nextDirs;
            for (const auto& dir : currentDirs)
            {
                if (! dir.isDirectory())
                    continue;

                juce::DirectoryIterator fileIt (dir, false, "*", juce::File::findFiles);
                while (fileIt.next())
                {
                    const auto file = fileIt.getFile();
                    if (file.getFileName().equalsIgnoreCase (targetFileName))
                        return file;
                }

                if (depth >= maxDepth)
                    continue;

                juce::DirectoryIterator dirIt (dir, false, "*", juce::File::findDirectories);
                while (dirIt.next())
                {
                    const auto childDir = dirIt.getFile();
                    if (childDir.isDirectory())
                        nextDirs.add (childDir);
                }
            }

            currentDirs.swapWith (nextDirs);
        }

        return {};
    };

    juce::Array<juce::File> broadSearchRoots;
    const auto addBroadRoot = [&broadSearchRoots] (const juce::File& candidateDir)
    {
        if (! candidateDir.isDirectory())
            return;

        for (const auto& existing : broadSearchRoots)
        {
            if (existing == candidateDir)
                return;
        }

        broadSearchRoots.add (candidateDir);
    };

    const auto userHomeDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    addBroadRoot (userHomeDir.getChildFile ("Desktop"));
    addBroadRoot (userHomeDir.getChildFile ("Documents"));
    addBroadRoot (userHomeDir.getChildFile ("Downloads"));
    addBroadRoot (userHomeDir.getChildFile ("Music"));

    for (const auto& rootDir : samplePathSearchRoots)
        addBroadRoot (rootDir);

    std::unordered_map<std::string, juce::File> broadNameLookupCache;

    const auto resolveSourceFileFromVariantPath = [&] (const juce::String& variantPath) -> juce::File
    {
        auto path = variantPath.trim();
        if (path.isEmpty())
            return {};

        path = path.replaceCharacter ('\\', '/');

        auto absoluteCandidate = resolveAutoSamplerDestinationPath (path).trim().replaceCharacter ('\\', '/');
        if (absoluteCandidate.startsWith ("~/") || absoluteCandidate.startsWith ("~\\"))
            absoluteCandidate = resolveAutoSamplerDestinationPath (absoluteCandidate);

        if (juce::File::isAbsolutePath (absoluteCandidate))
        {
            const auto absolute = juce::File (absoluteCandidate);
            if (absolute.existsAsFile())
                return absolute;
        }

        auto relativePath = path.replaceCharacters ("\\", "/").trim();
        while (relativePath.startsWith ("./"))
            relativePath = relativePath.substring (2);
        const auto nameOnly = extractFileNameFromPathString (relativePath);

        const auto tryRootDir = [&] (const juce::File& rootDir) -> juce::File
        {
            if (! rootDir.isDirectory())
                return {};

            if (relativePath.isNotEmpty())
            {
                const auto direct = rootDir.getChildFile (relativePath);
                if (direct.existsAsFile())
                    return direct;
            }

            if (nameOnly.isNotEmpty())
            {
                const auto inBase = rootDir.getChildFile (nameOnly);
                if (inBase.existsAsFile())
                    return inBase;

                const auto inAudio = rootDir.getChildFile ("Audio").getChildFile (nameOnly);
                if (inAudio.existsAsFile())
                    return inAudio;

                const auto inSamples = rootDir.getChildFile ("samples").getChildFile (nameOnly);
                if (inSamples.existsAsFile())
                    return inSamples;

                const auto inSamplesCaps = rootDir.getChildFile ("Samples").getChildFile (nameOnly);
                if (inSamplesCaps.existsAsFile())
                    return inSamplesCaps;
            }

            return {};
        };

        for (const auto& rootDir : samplePathSearchRoots)
        {
            if (const auto resolved = tryRootDir (rootDir); resolved.existsAsFile())
                return resolved;
        }

        if (nameOnly.isNotEmpty())
        {
            for (const auto& rootDir : samplePathSearchRoots)
            {
                const auto recursiveHit = findFileByNameRecursive (rootDir, nameOnly, 4);
                if (recursiveHit.existsAsFile())
                {
                    const auto discoveredDir = recursiveHit.getParentDirectory();
                    if (discoveredDir.isDirectory())
                    {
                        bool alreadyKnown = false;
                        for (const auto& existing : samplePathSearchRoots)
                            if (existing == discoveredDir) { alreadyKnown = true; break; }
                        if (! alreadyKnown)
                            samplePathSearchRoots.add (discoveredDir);
                    }
                    return recursiveHit;
                }
            }

            const auto cacheKey = nameOnly.toLowerCase().toStdString();
            if (const auto cacheIt = broadNameLookupCache.find (cacheKey); cacheIt != broadNameLookupCache.end())
            {
                if (cacheIt->second.existsAsFile())
                    return cacheIt->second;
                return {};
            }

            for (const auto& rootDir : broadSearchRoots)
            {
                const auto recursiveHit = findFileByNameRecursive (rootDir, nameOnly, 7);
                if (recursiveHit.existsAsFile())
                {
                    broadNameLookupCache[cacheKey] = recursiveHit;
                    const auto discoveredDir = recursiveHit.getParentDirectory();
                    if (discoveredDir.isDirectory())
                    {
                        bool alreadyKnown = false;
                        for (const auto& existing : samplePathSearchRoots)
                            if (existing == discoveredDir) { alreadyKnown = true; break; }
                        if (! alreadyKnown)
                            samplePathSearchRoots.add (discoveredDir);
                    }
                    return recursiveHit;
                }
            }

            broadNameLookupCache[cacheKey] = juce::File {};
        }

        return {};
    };

    // ---- Phase 1: resolve paths and check caches (sequential) ----
    struct VariantWorkItem
    {
        size_t variantIndex = 0;
        bool fromEmbeddedData = false;
        juce::String sampleDataUrl;
        juce::File resolvedSourceFile;
        juce::uint64 cacheKey = 0;
        std::shared_ptr<const DecodedEmbeddedAudioCacheEntry> cachedAudio;
        bool needsDecode = false; // true = needs I/O in phase 2
        bool failed = false;
    };

    std::vector<VariantWorkItem> workItems;
    workItems.reserve (variants.size());

    for (size_t vi = 0; vi < variants.size(); ++vi)
    {
        if (isStaleRequest())
        {
            logExit ("stale-mid-resolve");
            return;
        }

        const auto& descriptor = variants[vi];
        VariantWorkItem item;
        item.variantIndex = vi;
        item.fromEmbeddedData = descriptor.sampleDataUrl.isNotEmpty();

        if (item.fromEmbeddedData)
        {
            item.sampleDataUrl = descriptor.sampleDataUrl;
            item.cacheKey = static_cast<juce::uint64> (descriptor.sampleDataUrl.hashCode64());
            item.cachedAudio = findDecodedEmbeddedAudioInCache (item.cacheKey);

            if (item.cachedAudio != nullptr)
            {
                ++cacheHits;
                item.needsDecode = false;
            }
            else
            {
                item.needsDecode = true;
            }
        }
        else
        {
            item.resolvedSourceFile = resolveSourceFileFromVariantPath (descriptor.samplePath);
            if (! item.resolvedSourceFile.existsAsFile())
            {
                if (unresolvedVariantPaths.size() < 8)
                    unresolvedVariantPaths.add (descriptor.samplePath);
                ++decodeFailures;
                item.failed = true;
            }
            else
            {
                item.cacheKey = static_cast<juce::uint64> (
                    item.resolvedSourceFile.getFullPathName().hashCode64()
                    ^ (static_cast<juce::int64> (item.resolvedSourceFile.getSize()) * 0x9E3779B97F4A7C15ULL)
                    ^ (item.resolvedSourceFile.getLastModificationTime().toMilliseconds() * 0x517CC1B727220A95ULL));

                item.cachedAudio = findDecodedEmbeddedAudioInCache (item.cacheKey);
                if (item.cachedAudio != nullptr)
                {
                    ++cacheHits;
                    item.needsDecode = false;
                }
                else
                {
                    item.needsDecode = true;
                }
            }
        }

        workItems.push_back (std::move (item));
    }

    // ---- Phase 2: decode/read audio data in parallel ----
    size_t itemsNeedingDecode = 0;
    for (const auto& w : workItems)
        if (w.needsDecode && ! w.failed) ++itemsNeedingDecode;

    const unsigned int hwThreads2 = std::thread::hardware_concurrency();
    const unsigned int numDecodeThreads = juce::jlimit (1u, 8u, hwThreads2 > 0 ? hwThreads2 : 4u);
    const bool useParallelDecode = itemsNeedingDecode > 4 && numDecodeThreads > 1;

    std::atomic<int> parallelCacheMisses { 0 };
    std::atomic<int> parallelDecodeFailures { 0 };
    std::atomic<int> parallelDecodedOnMiss { 0 };
    std::atomic<size_t> parallelDecodedBytesOnMiss { 0 };
    std::atomic<int> parallelFilePathLoads { 0 };

    const auto decodeOneItem = [&parallelCacheMisses, &parallelDecodeFailures,
                                &parallelDecodedOnMiss, &parallelDecodedBytesOnMiss,
                                &parallelFilePathLoads]
                               (VariantWorkItem& item)
    {
        if (item.failed || ! item.needsDecode)
            return;

        thread_local juce::AudioFormatManager threadFm;
        thread_local bool threadFmReady = false;
        if (! threadFmReady)
        {
            threadFm.registerBasicFormats();
            threadFmReady = true;
        }

        if (item.fromEmbeddedData)
        {
            parallelCacheMisses.fetch_add (1, std::memory_order_relaxed);
            juce::MemoryBlock audioData;
            if (! decodeDataUrlAudioToMemory (item.sampleDataUrl, audioData) || audioData.getSize() == 0)
            {
                parallelDecodeFailures.fetch_add (1, std::memory_order_relaxed);
                item.failed = true;
                return;
            }

            auto input = std::make_unique<juce::MemoryInputStream> (audioData.getData(), audioData.getSize(), false);
            auto reader = std::unique_ptr<juce::AudioFormatReader> (threadFm.createReaderFor (std::move (input)));

            if (reader == nullptr || reader->lengthInSamples < 2)
            {
                parallelDecodeFailures.fetch_add (1, std::memory_order_relaxed);
                item.failed = true;
                return;
            }

            const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
            const auto totalSamples64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                 static_cast<juce::int64> (std::numeric_limits<int>::max()));
            const int totalSamples = static_cast<int> (totalSamples64);

            if (totalSamples < 2)
            {
                parallelDecodeFailures.fetch_add (1, std::memory_order_relaxed);
                item.failed = true;
                return;
            }

            auto decodedEntry = std::make_shared<DecodedEmbeddedAudioCacheEntry>();
            decodedEntry->sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
            decodedEntry->audio.setSize (channels, totalSamples);
            reader->read (&decodedEntry->audio, 0, totalSamples, 0, true, true);
            decodedEntry->bytes = static_cast<std::size_t> (channels)
                                * static_cast<std::size_t> (totalSamples)
                                * sizeof (float);

            item.cachedAudio = decodedEntry;
            parallelDecodedOnMiss.fetch_add (1, std::memory_order_relaxed);
            parallelDecodedBytesOnMiss.fetch_add (decodedEntry->bytes, std::memory_order_relaxed);
        }
        else
        {
            auto reader = std::unique_ptr<juce::AudioFormatReader> (threadFm.createReaderFor (item.resolvedSourceFile));
            if (reader == nullptr || reader->lengthInSamples < 2)
            {
                parallelDecodeFailures.fetch_add (1, std::memory_order_relaxed);
                item.failed = true;
                return;
            }

            const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
            const auto totalSamples64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                 static_cast<juce::int64> (std::numeric_limits<int>::max()));
            const int totalSamples = static_cast<int> (totalSamples64);

            if (totalSamples < 2)
            {
                parallelDecodeFailures.fetch_add (1, std::memory_order_relaxed);
                item.failed = true;
                return;
            }

            auto decodedEntry = std::make_shared<DecodedEmbeddedAudioCacheEntry>();
            decodedEntry->sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
            decodedEntry->audio.setSize (channels, totalSamples);
            reader->read (&decodedEntry->audio, 0, totalSamples, 0, true, true);
            decodedEntry->bytes = static_cast<std::size_t> (channels)
                                * static_cast<std::size_t> (totalSamples)
                                * sizeof (float);

            item.cachedAudio = decodedEntry;
            parallelFilePathLoads.fetch_add (1, std::memory_order_relaxed);
        }
    };

    if (useParallelDecode)
    {
        std::atomic<size_t> nextWorkIdx { 0 };
        std::vector<std::thread> decodeWorkers;
        decodeWorkers.reserve (numDecodeThreads);

        for (unsigned int t = 0; t < numDecodeThreads; ++t)
        {
            decodeWorkers.emplace_back ([&]()
            {
                while (true)
                {
                    const size_t idx = nextWorkIdx.fetch_add (1, std::memory_order_relaxed);
                    if (idx >= workItems.size())
                        break;
                    auto& item = workItems[idx];
                    if (item.needsDecode && ! item.failed)
                        decodeOneItem (item);
                }
            });
        }

        for (auto& w : decodeWorkers)
            w.join();
    }
    else
    {
        for (auto& item : workItems)
        {
            if (isStaleRequest())
            {
                logExit ("stale-mid-decode");
                return;
            }
            if (item.needsDecode && ! item.failed)
                decodeOneItem (item);
        }
    }

    // Flush decoded entries to the cache (sequential — cache uses a lock).
    for (auto& item : workItems)
    {
        if (item.failed || item.cachedAudio == nullptr)
            continue;
        if (item.needsDecode)
            storeDecodedEmbeddedAudioInCache (item.cacheKey, std::const_pointer_cast<DecodedEmbeddedAudioCacheEntry> (item.cachedAudio));
    }

    cacheMisses += parallelCacheMisses.load (std::memory_order_relaxed);
    decodeFailures += parallelDecodeFailures.load (std::memory_order_relaxed);
    decodedOnMiss += parallelDecodedOnMiss.load (std::memory_order_relaxed);
    decodedBytesOnMiss += parallelDecodedBytesOnMiss.load (std::memory_order_relaxed);
    filePathLoads += parallelFilePathLoads.load (std::memory_order_relaxed);

    // ---- Phase 3: assemble zones (sequential) ----
    for (size_t wi = 0; wi < workItems.size(); ++wi)
    {
        const auto& item = workItems[wi];
        if (item.failed || item.cachedAudio == nullptr
            || item.cachedAudio->audio.getNumSamples() < 2
            || item.cachedAudio->audio.getNumChannels() <= 0)
            continue;

        const auto& descriptor = variants[item.variantIndex];
        const auto& cachedAudio = item.cachedAudio;
        const auto cacheKey = item.cacheKey;

        const auto zone = std::make_shared<SampleZone>();

        auto safeFileName = extractFileNameFromPathString (descriptor.fileName).trim();
        if (safeFileName.isEmpty())
            safeFileName = "Embedded_" + midiToNoteToken (descriptor.rootMidi) + ".wav";

        juce::File sourceFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile (safeFileName);

        if (! item.fromEmbeddedData && item.resolvedSourceFile.existsAsFile())
        {
            sourceFile = item.resolvedSourceFile;
            newSampleSet->sourcePaths.addIfNotAlreadyThere (sourceFile.getFullPathName());
        }
        else if (unpackDirReady)
        {
            const auto legalName = juce::File::createLegalFileName (safeFileName);
            const auto keyHex = juce::String::toHexString (static_cast<juce::int64> (cacheKey));
            const auto unpackedFile = unpackedSampleDir.getChildFile (keyHex + "_" + legalName);

            if (! unpackedFile.existsAsFile() || unpackedFile.getSize() < 48)
                writeAudioBufferToWavFile (cachedAudio->audio, cachedAudio->sampleRate, unpackedFile);

            if (unpackedFile.existsAsFile())
            {
                sourceFile = unpackedFile;
                newSampleSet->sourcePaths.addIfNotAlreadyThere (unpackedFile.getFullPathName());
            }
        }

        zone->sourceFile = sourceFile;
        zone->sourceSampleRate = cachedAudio->sampleRate > 0.0 ? cachedAudio->sampleRate : 44100.0;
        zone->audio.makeCopyOf (cachedAudio->audio);

        ZoneMetadata metadata;
        metadata.rootNote = descriptor.rootMidi;

        if (const auto slotIt = noteRangesBySlotRoot.find (descriptor.mapSetSlot); slotIt != noteRangesBySlotRoot.end())
        {
            if (const auto rangeIt = slotIt->second.find (descriptor.rootMidi); rangeIt != slotIt->second.end())
            {
                metadata.lowNote = rangeIt->second.first;
                metadata.highNote = rangeIt->second.second;
            }
            else
            {
                metadata.lowNote = descriptor.rootMidi;
                metadata.highNote = descriptor.rootMidi;
            }
        }
        else
        {
            metadata.lowNote = descriptor.rootMidi;
            metadata.highNote = descriptor.rootMidi;
        }

        if (const auto slotIt = velocityBoundsBySlotRootLayer.find (descriptor.mapSetSlot); slotIt != velocityBoundsBySlotRootLayer.end())
        {
            if (const auto rootIt = slotIt->second.find (descriptor.rootMidi); rootIt != slotIt->second.end())
            {
                if (const auto layerIt = rootIt->second.find (descriptor.velocityLayer); layerIt != rootIt->second.end())
                {
                    metadata.lowVelocity = layerIt->second.first;
                    metadata.highVelocity = layerIt->second.second;
                }
            }
        }

        metadata.velocityLayer = descriptor.velocityLayer;
        metadata.roundRobinIndex = descriptor.rrIndex;
        metadata.mapSetSlot = descriptor.mapSetSlot;
        zone->metadata = sanitizeZoneMetadata (metadata);

        newSampleSet->zones.push_back (zone);
    }

    if (newSampleSet->zones.empty())
    {
        if (isStaleRequest())
        {
            logExit ("stale-empty-zones");
            return;
        }

        clearSampleSet();
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "no-valid-zones");
        {
            const juce::ScopedLock lock (sessionMapSyncLock);
            lastSessionMapSignature = "empty";
        }
        writeLoadDebugLog ("session sync cleared (no valid zones) | requestId=" + juce::String (requestId)
                           + " | payloadBytes=" + juce::String (payloadBytes)
                           + " | parseMs=" + juce::String (parseMs, 2)
                           + " | mapBuildMs=" + juce::String (mapBuildMs, 2)
                           + " | decodeMs=" + juce::String (elapsedMsFrom (decodeStageStartMs), 2)
                           + " | cacheHits=" + juce::String (cacheHits)
                           + " | cacheMisses=" + juce::String (cacheMisses)
                           + " | filePathLoads=" + juce::String (filePathLoads)
                           + " | decodeFailures=" + juce::String (decodeFailures)
                           + " | unresolvedPaths=" + unresolvedVariantPaths.joinIntoString (" ; "));
        return;
    }

    std::sort (newSampleSet->zones.begin(), newSampleSet->zones.end(), [] (const auto& a, const auto& b)
    {
        if (a->metadata.mapSetSlot != b->metadata.mapSetSlot)
            return a->metadata.mapSetSlot < b->metadata.mapSetSlot;

        if (a->metadata.rootNote != b->metadata.rootNote)
            return a->metadata.rootNote < b->metadata.rootNote;

        if (a->metadata.lowVelocity != b->metadata.lowVelocity)
            return a->metadata.lowVelocity < b->metadata.lowVelocity;

        if (a->metadata.roundRobinIndex != b->metadata.roundRobinIndex)
            return a->metadata.roundRobinIndex < b->metadata.roundRobinIndex;

        return a->sourceFile.getFileName() < b->sourceFile.getFileName();
    });

    if (isStaleRequest())
    {
        logExit ("stale-before-apply");
        return;
    }

    newSampleSet->summary = buildSampleSummary (newSampleSet->zones);
    activeMapSetSlot.store (activeSetSlot, std::memory_order_relaxed);
    activeMapLoopPlaybackEnabled.store (activeSetLoopPlaybackEnabled, std::memory_order_relaxed);
    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (newSampleSet));
    std::atomic_store (&stepSequencerRuntime, buildSequencerRuntime());
    sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
    resetVoicesRequested.store (true);
    markPresetLoadPlayable ("session-sync", static_cast<int> (newSampleSet->zones.size()));

    {
        const juce::ScopedLock lock (sessionMapSyncLock);
        lastSessionMapSignature = signature;
    }

    writeLoadDebugLog ("session sync complete | requestId=" + juce::String (requestId)
                       + " | payloadBytes=" + juce::String (payloadBytes)
                       + " | variants=" + juce::String (static_cast<int> (variants.size()))
                       + " | zones=" + juce::String (static_cast<int> (newSampleSet->zones.size()))
                       + " | parseMs=" + juce::String (parseMs, 2)
                       + " | mapBuildMs=" + juce::String (mapBuildMs, 2)
                       + " | decodeMs=" + juce::String (elapsedMsFrom (decodeStageStartMs), 2)
                       + " | totalMs=" + juce::String (elapsedMsFrom (syncStartMs), 2)
                       + " | cacheHits=" + juce::String (cacheHits)
                       + " | cacheMisses=" + juce::String (cacheMisses)
                       + " | filePathLoads=" + juce::String (filePathLoads)
                       + " | decoded=" + juce::String (decodedOnMiss)
                       + " | decodedBytes=" + juce::String (static_cast<juce::int64> (decodedBytesOnMiss))
                       + " | decodeFailures=" + juce::String (decodeFailures)
                       + " | unresolvedPaths=" + unresolvedVariantPaths.joinIntoString (" ; ")
                       + " | cacheBytes=" + juce::String (static_cast<juce::int64> (decodedEmbeddedAudioCacheTotalBytes)));
}

std::shared_ptr<const SamplePlayerAudioProcessor::DecodedEmbeddedAudioCacheEntry>
SamplePlayerAudioProcessor::findDecodedEmbeddedAudioInCache (juce::uint64 key)
{
    const juce::ScopedLock lock (decodedEmbeddedAudioCacheLock);
    const auto it = decodedEmbeddedAudioCache.find (key);
    if (it == decodedEmbeddedAudioCache.end() || it->second == nullptr)
        return {};

    it->second->age = ++decodedEmbeddedAudioCacheAgeCounter;
    return it->second;
}

void SamplePlayerAudioProcessor::storeDecodedEmbeddedAudioInCache (juce::uint64 key,
                                                                   std::shared_ptr<DecodedEmbeddedAudioCacheEntry> entry)
{
    if (entry == nullptr)
        return;

    {
        const juce::ScopedLock lock (decodedEmbeddedAudioCacheLock);
        if (const auto it = decodedEmbeddedAudioCache.find (key); it != decodedEmbeddedAudioCache.end() && it->second != nullptr)
        {
            const auto existingBytes = it->second->bytes;
            decodedEmbeddedAudioCacheTotalBytes = existingBytes > decodedEmbeddedAudioCacheTotalBytes
                                                    ? 0
                                                    : (decodedEmbeddedAudioCacheTotalBytes - existingBytes);
        }

        entry->age = ++decodedEmbeddedAudioCacheAgeCounter;
        decodedEmbeddedAudioCacheTotalBytes += entry->bytes;
        decodedEmbeddedAudioCache[key] = std::move (entry);
    }

    trimDecodedEmbeddedAudioCache();
}

void SamplePlayerAudioProcessor::trimDecodedEmbeddedAudioCache()
{
    const juce::ScopedLock lock (decodedEmbeddedAudioCacheLock);

    while (decodedEmbeddedAudioCacheTotalBytes > maxDecodedEmbeddedAudioCacheBytes
           && ! decodedEmbeddedAudioCache.empty())
    {
        auto oldestIt = decodedEmbeddedAudioCache.end();
        juce::uint64 oldestAge = std::numeric_limits<juce::uint64>::max();

        for (auto it = decodedEmbeddedAudioCache.begin(); it != decodedEmbeddedAudioCache.end(); ++it)
        {
            if (it->second == nullptr)
                continue;

            if (it->second->age < oldestAge)
            {
                oldestAge = it->second->age;
                oldestIt = it;
            }
        }

        if (oldestIt == decodedEmbeddedAudioCache.end())
            break;

        const auto bytes = oldestIt->second != nullptr ? oldestIt->second->bytes : 0;
        decodedEmbeddedAudioCache.erase (oldestIt);
        decodedEmbeddedAudioCacheTotalBytes = bytes > decodedEmbeddedAudioCacheTotalBytes
                                                ? 0
                                                : (decodedEmbeddedAudioCacheTotalBytes - bytes);
    }
}

std::pair<int, int> SamplePlayerAudioProcessor::velocityBoundsForLayer (int layer, int totalLayers)
{
    const int safeLayers = juce::jlimit (1, 5, totalLayers);
    const int safeLayer = juce::jlimit (1, safeLayers, layer);

    const int low0 = ((safeLayer - 1) * 128) / safeLayers;
    const int high0 = ((safeLayer * 128) / safeLayers) - 1;

    const int low = juce::jlimit (1, 127, low0 + 1);
    const int high = juce::jlimit (low, 127, high0 + 1);
    return { low, high };
}

int SamplePlayerAudioProcessor::velocityForLayer (int layer, int totalLayers)
{
    const auto [low, high] = velocityBoundsForLayer (layer, totalLayers);
    return juce::jlimit (1, 127, low + ((high - low) / 2));
}

juce::String SamplePlayerAudioProcessor::midiToNoteToken (int midiNote)
{
    static constexpr const char* noteNames[12] =
    {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int clamped = juce::jlimit (0, 127, midiNote);
    const int semitone = clamped % 12;
    const int octave = (clamped / 12) - 1;
    return juce::String (noteNames[semitone]) + juce::String (octave);
}

void SamplePlayerAudioProcessor::appendAutoSamplerMidiOutput (juce::MidiBuffer& midiOutput, int blockNumSamples)
{
    if (blockNumSamples <= 0)
        return;

    const juce::ScopedLock lock (autoSamplerLock);
    autoSamplerPendingNoteEvents.clear();

    if (autoSamplerSendAllNotesOff)
    {
        for (int note = 0; note < static_cast<int> (autoSamplerHeldNotes.size()); ++note)
        {
            if (! autoSamplerHeldNotes[static_cast<size_t> (note)])
                continue;

            midiOutput.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            autoSamplerHeldNotes[static_cast<size_t> (note)] = false;
        }

        autoSamplerSendAllNotesOff = false;
    }

    if (! autoSamplerActive || autoSamplerMidiSchedule.empty())
        return;

    const juce::int64 blockStart = autoSamplerTimelineSample;
    const juce::int64 blockEnd = blockStart + blockNumSamples;

    while (autoSamplerMidiScheduleIndex < autoSamplerMidiSchedule.size())
    {
        const auto& event = autoSamplerMidiSchedule[autoSamplerMidiScheduleIndex];

        if (event.samplePosition < blockStart)
        {
            ++autoSamplerMidiScheduleIndex;
            continue;
        }

        if (event.samplePosition >= blockEnd)
            break;

        const int sampleOffset = static_cast<int> (event.samplePosition - blockStart);
        if (event.noteOn)
        {
            const auto velocity01 = juce::jlimit (0.0f, 1.0f, static_cast<float> (event.velocity127) / 127.0f);
            midiOutput.addEvent (juce::MidiMessage::noteOn (1, event.midiNote, velocity01), sampleOffset);
            autoSamplerHeldNotes[static_cast<size_t> (juce::jlimit (0, 127, event.midiNote))] = true;

            ScheduledAutoCaptureNoteEvent pendingNote;
            pendingNote.samplePosition = sampleOffset;
            pendingNote.note = juce::jlimit (0, 127, event.midiNote);
            pendingNote.velocity127 = juce::jlimit (1, 127, event.velocity127);
            pendingNote.velocityLayer = juce::jlimit (1, 5, event.velocityLayer);
            pendingNote.velocityLow = juce::jlimit (1, 127, event.velocityLow);
            pendingNote.velocityHigh = juce::jlimit (pendingNote.velocityLow, 127, event.velocityHigh);
            pendingNote.rrIndex = juce::jmax (1, event.rrIndex);
            autoSamplerPendingNoteEvents.push_back (pendingNote);

            AutoSamplerTriggeredTake triggered;
            triggered.rootMidi = juce::jlimit (0, 127, event.midiNote);
            triggered.velocity127 = juce::jlimit (1, 127, event.velocity127);
            triggered.velocityLayer = juce::jlimit (1, 5, event.velocityLayer);
            triggered.velocityLow = juce::jlimit (1, 127, event.velocityLow);
            triggered.velocityHigh = juce::jlimit (triggered.velocityLow, 127, event.velocityHigh);
            triggered.rrIndex = juce::jmax (1, event.rrIndex);
            triggered.loopSamples = autoSamplerSettings.loopSamples;
            triggered.autoLoopMode = autoSamplerSettings.autoLoopMode;
            triggered.loopStartPercent = autoSamplerSettings.loopStartPercent;
            triggered.loopEndPercent = autoSamplerSettings.loopEndPercent;
            triggered.cutLoopAtEnd = autoSamplerSettings.cutLoopAtEnd;
            triggered.loopCrossfadeMs = autoSamplerSettings.loopCrossfadeMs;
            triggered.normalized = autoSamplerSettings.normalizeRecorded;
            triggered.fileName = autoSamplerSettings.instrumentName
                               + "_"
                               + midiToNoteToken (triggered.rootMidi)
                               + "_V" + juce::String (triggered.velocityLayer)
                               + "_RR" + juce::String (triggered.rrIndex) + ".wav";
            triggeredAutoCaptures.push_back (std::move (triggered));
        }
        else
        {
            midiOutput.addEvent (juce::MidiMessage::noteOff (1, event.midiNote), sampleOffset);
            autoSamplerHeldNotes[static_cast<size_t> (juce::jlimit (0, 127, event.midiNote))] = false;
        }

        ++autoSamplerMidiScheduleIndex;
    }

    autoSamplerTimelineSample = blockEnd;
}

bool SamplePlayerAudioProcessor::startAutoSamplerCapture (const AutoSamplerSettings& settings, juce::String& errorMessage)
{
    const auto sanitizeFloat = [] (float value, float fallback) -> float
    {
        return std::isfinite (value) ? value : fallback;
    };

    AutoSamplerSettings next = settings;
    next.startMidi = juce::jlimit (0, 127, next.startMidi);
    next.endMidi = juce::jlimit (0, 127, next.endMidi);
    next.intervalSemitones = juce::jlimit (1, 12, next.intervalSemitones);
    next.velocityLayers = juce::jlimit (1, 5, next.velocityLayers);
    next.roundRobinsPerNote = juce::jlimit (1, 8, next.roundRobinsPerNote);
    next.sustainMs = juce::jlimit (1.0f, 60000.0f, sanitizeFloat (next.sustainMs, 1800.0f));
    next.releaseTailMs = juce::jlimit (0.0f, 60000.0f, sanitizeFloat (next.releaseTailMs, 700.0f));
    next.prerollMs = juce::jlimit (0.0f, 60000.0f, sanitizeFloat (next.prerollMs, 0.0f));
    next.destinationFolder = resolveAutoSamplerDestinationPath (settings.destinationFolder);
    next.instrumentName = juce::File::createLegalFileName (settings.instrumentName.trim());
    next.keyswitchMode = settings.keyswitchMode;
    next.keyswitchKey = sanitizeKeyswitchKeyToken (settings.keyswitchKey);
    if (next.instrumentName.isEmpty())
        next.instrumentName = "Instrument";
    next.loopStartPercent = juce::jlimit (0.0f, 100.0f, sanitizeFloat (next.loopStartPercent, 10.0f));
    next.loopEndPercent = juce::jlimit (0.0f, 100.0f, sanitizeFloat (next.loopEndPercent, 90.0f));
    if (next.loopEndPercent <= next.loopStartPercent + 0.1f)
        next.loopEndPercent = juce::jmin (100.0f, next.loopStartPercent + 0.1f);
    if (next.cutLoopAtEnd && next.loopEndPercent < 5.0f)
        next.loopEndPercent = 90.0f;
    next.loopCrossfadeMs = juce::jlimit (0.0f, 60000.0f, sanitizeFloat (next.loopCrossfadeMs, 200.0f));

    if (next.destinationFolder.isEmpty())
    {
        errorMessage = "Choose a destination folder before starting auto sampling.";
        return false;
    }

    if (! juce::File::isAbsolutePath (next.destinationFolder))
    {
        errorMessage = "Destination folder must be an absolute path.";
        return false;
    }

    auto destinationRoot = juce::File (next.destinationFolder);
    if (destinationRoot.existsAsFile())
    {
        errorMessage = "Destination path points to a file. Choose a folder.";
        return false;
    }

    if (! destinationRoot.isDirectory())
    {
        const auto rootCreateResult = destinationRoot.createDirectory();
        if (rootCreateResult.failed())
        {
            errorMessage = "Could not create destination folder: " + rootCreateResult.getErrorMessage();
            return false;
        }
    }

    juce::File manifestDirectoryRoot = destinationRoot;
    auto audioDirectory = destinationRoot.getChildFile ("Audio");
    if (next.keyswitchMode)
    {
        auto keyswitchesRoot = destinationRoot.getChildFile ("Keyswitches");
        if (keyswitchesRoot.existsAsFile())
        {
            errorMessage = "Keyswitches output path points to a file. Remove it or choose another destination.";
            return false;
        }

        if (! keyswitchesRoot.isDirectory())
        {
            const auto keyswitchesCreateResult = keyswitchesRoot.createDirectory();
            if (keyswitchesCreateResult.failed())
            {
                errorMessage = "Could not create Keyswitches folder: " + keyswitchesCreateResult.getErrorMessage();
                return false;
            }
        }
        manifestDirectoryRoot = keyswitchesRoot;
    }
    if (audioDirectory.existsAsFile())
    {
        errorMessage = "Sample output path points to a file. Remove it or choose another destination.";
        return false;
    }

    if (! audioDirectory.isDirectory())
    {
        const auto audioCreateResult = audioDirectory.createDirectory();
        if (audioCreateResult.failed())
        {
            errorMessage = "Could not create sample output folder: " + audioCreateResult.getErrorMessage();
            return false;
        }
    }

    const int low = juce::jmin (next.startMidi, next.endMidi);
    const int high = juce::jmax (next.startMidi, next.endMidi);

    std::array<bool, 128> noteMask {};
    std::vector<int> scheduledNotes;
    int noteCount = 0;

    for (int midi = low; midi <= high; midi += next.intervalSemitones)
    {
        if (! noteMask[static_cast<size_t> (midi)])
        {
            noteMask[static_cast<size_t> (midi)] = true;
            scheduledNotes.push_back (midi);
            ++noteCount;
        }
    }

    if (! noteMask[static_cast<size_t> (high)])
    {
        noteMask[static_cast<size_t> (high)] = true;
        scheduledNotes.push_back (high);
        ++noteCount;
    }

    if (noteCount <= 0)
    {
        errorMessage = "No notes available for capture.";
        return false;
    }

    const int preRollSamples = msToSamples (currentSampleRate, next.prerollMs);
    const int historySize = juce::jmax (1, preRollSamples + 4);
    const int sustainSamples = msToSamples (currentSampleRate, next.sustainMs);
    const int tailSamples = msToSamples (currentSampleRate, next.releaseTailMs);
    const int takeSamples = juce::jmax (4, preRollSamples + sustainSamples + tailSamples);
    const int interTakePauseSamples = msToSamples (currentSampleRate, autoSamplerInterTakePauseMs);

    std::vector<AutoSamplerMidiEvent> midiSchedule;
    midiSchedule.reserve (static_cast<size_t> (noteCount * next.velocityLayers * next.roundRobinsPerNote * 2));

    juce::int64 timelineSample = 0;
    for (const int note : scheduledNotes)
    {
        for (int layer = 1; layer <= next.velocityLayers; ++layer)
        {
            const int velocity127 = velocityForLayer (layer, next.velocityLayers);
            const auto [velocityLow, velocityHigh] = velocityBoundsForLayer (layer, next.velocityLayers);
            for (int rr = 1; rr <= next.roundRobinsPerNote; ++rr)
            {
                AutoSamplerMidiEvent noteOn;
                // Emit the note immediately at take start so hosts see MIDI out activity right away.
                // Preroll is still preserved in captured audio via input-history padding.
                noteOn.samplePosition = timelineSample;
                noteOn.midiNote = note;
                noteOn.velocity127 = velocity127;
                noteOn.velocityLayer = layer;
                noteOn.velocityLow = velocityLow;
                noteOn.velocityHigh = velocityHigh;
                noteOn.rrIndex = rr;
                noteOn.noteOn = true;
                midiSchedule.push_back (noteOn);

                AutoSamplerMidiEvent noteOff;
                noteOff.samplePosition = noteOn.samplePosition + sustainSamples;
                noteOff.midiNote = note;
                noteOff.velocity127 = velocity127;
                noteOff.velocityLayer = layer;
                noteOff.velocityLow = velocityLow;
                noteOff.velocityHigh = velocityHigh;
                noteOff.rrIndex = rr;
                noteOff.noteOn = false;
                midiSchedule.push_back (noteOff);

                timelineSample += (takeSamples + interTakePauseSamples);
            }
        }
    }

    const juce::ScopedLock lock (autoSamplerLock);
    autoSamplerSettings = next;
    autoSamplerNoteMask = noteMask;
    autoSamplerExpectedTakes = noteCount * next.velocityLayers * next.roundRobinsPerNote;
    autoSamplerCapturedTakes = 0;
    autoSamplerInputDetected = false;
    autoSamplerStatusMessage = "Sampling armed. Sending MIDI notes to target instrument.";
    autoSamplerActive = true;
    activeAutoCaptures.clear();
    triggeredAutoCaptures.clear();
    completedAutoCaptures.clear();
    autoSamplerPendingNoteEvents.clear();
    autoSamplerFallbackRrCounters.clear();
    autoSamplerMidiSchedule = std::move (midiSchedule);
    autoSamplerMidiScheduleIndex = 0;
    autoSamplerTimelineSample = 0;
    autoSamplerStartWallMs = juce::Time::getMillisecondCounterHiRes();
    autoSamplerHeldNotes.fill (false);
    autoSamplerSendAllNotesOff = false;
    autoSamplerHistorySize = historySize;
    autoSamplerHistoryWrite = 0;
    autoSamplerHistoryValid = 0;
    autoSamplerInputHistory[0].assign (static_cast<size_t> (historySize), 0.0f);
    autoSamplerInputHistory[1].assign (static_cast<size_t> (historySize), 0.0f);
    autoSamplerDestinationRoot = manifestDirectoryRoot;
    autoSamplerOutputDirectory = audioDirectory;
    autoSamplerFilesWritten = 0;
    autoSamplerWriteFailures = 0;
    autoSamplerSavedTakes.clear();
    autoSamplerManifestWriteFailed = false;
    autoSamplerManifestPath.clear();
    autoSamplerStatusMessage = "Sampling armed. Writing files to " + autoSamplerOutputDirectory.getFullPathName();
    return true;
}

void SamplePlayerAudioProcessor::stopAutoSamplerCapture (bool cancelled)
{
    const juce::ScopedLock lock (autoSamplerLock);
    autoSamplerActive = false;
    activeAutoCaptures.clear();
    triggeredAutoCaptures.clear();
    autoSamplerPendingNoteEvents.clear();
    autoSamplerFallbackRrCounters.clear();
    autoSamplerMidiSchedule.clear();
    autoSamplerMidiScheduleIndex = 0;
    autoSamplerTimelineSample = 0;
    autoSamplerStartWallMs = 0.0;
    autoSamplerSendAllNotesOff = true;
    autoSamplerStatusMessage = cancelled ? "Sampling cancelled." : "Sampling stopped.";
    autoSamplerDestinationRoot = juce::File {};
    autoSamplerOutputDirectory = juce::File {};
    autoSamplerFilesWritten = 0;
    autoSamplerWriteFailures = 0;
    autoSamplerSavedTakes.clear();
    autoSamplerManifestWriteFailed = false;
    autoSamplerManifestPath.clear();
}

SamplePlayerAudioProcessor::AutoSamplerProgress SamplePlayerAudioProcessor::getAutoSamplerProgress() const
{
    const juce::ScopedLock lock (autoSamplerLock);
    AutoSamplerProgress progress;
    progress.active = autoSamplerActive;
    progress.expectedTakes = autoSamplerExpectedTakes;
    progress.capturedTakes = autoSamplerCapturedTakes;
    progress.inputDetected = autoSamplerInputDetected;
    progress.statusMessage = autoSamplerStatusMessage;

    if (progress.active
        && autoSamplerTimelineSample <= 0
        && autoSamplerStartWallMs > 0.0
        && (juce::Time::getMillisecondCounterHiRes() - autoSamplerStartWallMs) > 1200.0)
    {
        progress.statusMessage = "Waiting for host processing. In Ableton: set track Monitor to In or press Play.";
    }

    return progress;
}

std::vector<SamplePlayerAudioProcessor::AutoSamplerCompletedTake> SamplePlayerAudioProcessor::popCompletedAutoSamplerTakes()
{
    const juce::ScopedLock lock (autoSamplerLock);
    auto popped = std::move (completedAutoCaptures);
    completedAutoCaptures.clear();
    return popped;
}

std::vector<SamplePlayerAudioProcessor::AutoSamplerTriggeredTake> SamplePlayerAudioProcessor::popTriggeredAutoSamplerTakes()
{
    const juce::ScopedLock lock (autoSamplerLock);
    auto popped = std::move (triggeredAutoCaptures);
    triggeredAutoCaptures.clear();
    return popped;
}

bool SamplePlayerAudioProcessor::shouldCaptureMidiNote (int midiNote) const
{
    const int clamped = juce::jlimit (0, 127, midiNote);
    return autoSamplerNoteMask[static_cast<size_t> (clamped)];
}

void SamplePlayerAudioProcessor::writeInputHistorySample (float left, float right)
{
    if (autoSamplerHistorySize <= 0
        || autoSamplerInputHistory[0].empty()
        || autoSamplerInputHistory[1].empty())
        return;

    autoSamplerInputHistory[0][static_cast<size_t> (autoSamplerHistoryWrite)] = left;
    autoSamplerInputHistory[1][static_cast<size_t> (autoSamplerHistoryWrite)] = right;
    autoSamplerHistoryWrite = (autoSamplerHistoryWrite + 1) % autoSamplerHistorySize;
    autoSamplerHistoryValid = juce::jmin (autoSamplerHistoryValid + 1, autoSamplerHistorySize);
}

void SamplePlayerAudioProcessor::copyFromInputHistory (ActiveAutoCapture& capture, int numSamples)
{
    if (numSamples <= 0 || autoSamplerHistorySize <= 0)
        return;

    const int available = juce::jmin (numSamples, autoSamplerHistoryValid);
    if (available <= 0)
        return;

    const int destStart = juce::jmax (0, numSamples - available);
    int index = autoSamplerHistoryWrite - available;
    while (index < 0)
        index += autoSamplerHistorySize;

    for (int i = 0; i < available; ++i)
    {
        const int readIndex = (index + i) % autoSamplerHistorySize;
        const int writeIndex = destStart + i;
        if (writeIndex >= capture.totalSamples)
            break;

        capture.audio.setSample (0, writeIndex, autoSamplerInputHistory[0][static_cast<size_t> (readIndex)]);
        capture.audio.setSample (1, writeIndex, autoSamplerInputHistory[1][static_cast<size_t> (readIndex)]);
    }
}

void SamplePlayerAudioProcessor::processAutoSamplerCapture (const juce::AudioBuffer<float>& inputBuffer,
                                                            const juce::MidiBuffer& midiMessages)
{
    const int numSamples = inputBuffer.getNumSamples();

    if (numSamples <= 0)
        return;

    const int numInputChannels = inputBuffer.getNumChannels();
    const auto* inL = numInputChannels > 0 ? inputBuffer.getReadPointer (0) : nullptr;
    const auto* inR = numInputChannels > 1 ? inputBuffer.getReadPointer (1) : inL;

    const juce::ScopedLock lock (autoSamplerLock);

    if (! autoSamplerActive)
        return;

    auto noteEvents = std::move (autoSamplerPendingNoteEvents);
    autoSamplerPendingNoteEvents.clear();

    if (noteEvents.empty())
    {
        for (const auto metadata : midiMessages)
        {
            const auto message = metadata.getMessage();
            if (! message.isNoteOn())
                continue;

            ScheduledAutoCaptureNoteEvent fallbackEvent;
            fallbackEvent.samplePosition = juce::jlimit (0, numSamples - 1, metadata.samplePosition);
            fallbackEvent.note = juce::jlimit (0, 127, message.getNoteNumber());
            fallbackEvent.velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (message.getVelocity() * 127.0f)));
            fallbackEvent.velocityLayer = velocityToLayerFromVelocity (fallbackEvent.velocity127, autoSamplerSettings.velocityLayers);

            const auto [velocityLow, velocityHigh] = velocityBoundsForLayer (fallbackEvent.velocityLayer,
                                                                              autoSamplerSettings.velocityLayers);
            fallbackEvent.velocityLow = velocityLow;
            fallbackEvent.velocityHigh = velocityHigh;

            const int rrKey = (fallbackEvent.note << 8) | juce::jlimit (1, 5, fallbackEvent.velocityLayer);
            fallbackEvent.rrIndex = juce::jmax (1, ++autoSamplerFallbackRrCounters[rrKey]);
            noteEvents.push_back (fallbackEvent);
        }
    }

    const bool hasInputAudio = (numInputChannels > 0 && inL != nullptr);
    if (! hasInputAudio)
        autoSamplerStatusMessage = "No capture audio detected. Capturing silence; route source audio to Sample Player input.";

    const int preRollSamples = msToSamples (currentSampleRate, autoSamplerSettings.prerollMs);
    const int sustainSamples = msToSamples (currentSampleRate, autoSamplerSettings.sustainMs);
    const int tailSamples = msToSamples (currentSampleRate, autoSamplerSettings.releaseTailMs);
    const int takeSamples = juce::jmax (4, preRollSamples + sustainSamples + tailSamples);

    size_t nextNoteEventIndex = 0;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        while (nextNoteEventIndex < noteEvents.size()
               && noteEvents[nextNoteEventIndex].samplePosition == sample)
        {
            const auto& event = noteEvents[nextNoteEventIndex];
            ++nextNoteEventIndex;

            if (! shouldCaptureMidiNote (event.note))
                continue;

            ActiveAutoCapture capture;
            capture.rootMidi = juce::jlimit (0, 127, event.note);
            capture.velocity127 = event.velocity127;
            capture.velocityLayer = juce::jlimit (1, 5, event.velocityLayer);
            capture.velocityLow = juce::jlimit (1, 127, event.velocityLow);
            capture.velocityHigh = juce::jlimit (capture.velocityLow, 127, event.velocityHigh);
            capture.rrIndex = juce::jmax (1, event.rrIndex);
            capture.totalSamples = takeSamples;
            capture.audio.setSize (2, takeSamples);
            capture.audio.clear();
            capture.writePosition = preRollSamples;
            copyFromInputHistory (capture, preRollSamples);

            activeAutoCaptures.push_back (std::move (capture));
            autoSamplerStatusMessage = "Capturing " + midiToNoteToken (event.note)
                                     + " V" + juce::String (event.velocityLayer)
                                     + " RR" + juce::String (event.rrIndex) + "...";
        }

        const float left = (inL != nullptr ? inL[sample] : 0.0f);
        const float right = (inR != nullptr ? inR[sample] : left);

        if (! autoSamplerInputDetected
            && (std::abs (left) > 0.00005f || std::abs (right) > 0.00005f))
            autoSamplerInputDetected = true;

        writeInputHistorySample (left, right);

        for (size_t i = 0; i < activeAutoCaptures.size();)
        {
            auto& capture = activeAutoCaptures[i];
            if (capture.writePosition < capture.totalSamples)
            {
                capture.audio.setSample (0, capture.writePosition, left);
                capture.audio.setSample (1, capture.writePosition, right);
                ++capture.writePosition;
            }

            if (capture.writePosition >= capture.totalSamples)
            {
                juce::AudioBuffer<float> finalAudio = std::move (capture.audio);
                int finalSamples = finalAudio.getNumSamples();

                if (autoSamplerSettings.loopSamples && autoSamplerSettings.cutLoopAtEnd)
                {
                    const auto endRatio = juce::jlimit (0.0f, 1.0f, autoSamplerSettings.loopEndPercent * 0.01f);
                    const auto cutSamples = juce::jlimit (4, finalSamples, static_cast<int> (std::round (endRatio * static_cast<float> (finalSamples))));
                    if (cutSamples < finalSamples)
                    {
                        juce::AudioBuffer<float> trimmed;
                        trimmed.setSize (2, cutSamples);
                        trimmed.copyFrom (0, 0, finalAudio, 0, 0, cutSamples);
                        trimmed.copyFrom (1, 0, finalAudio, juce::jmin (1, finalAudio.getNumChannels() - 1), 0, cutSamples);
                        finalAudio = std::move (trimmed);
                        finalSamples = cutSamples;
                    }
                }

                bool normalized = false;
                if (autoSamplerSettings.normalizeRecorded)
                {
                    float peak = 0.0f;
                    for (int ch = 0; ch < finalAudio.getNumChannels(); ++ch)
                    {
                        const auto channelPeak = finalAudio.getMagnitude (ch, 0, finalSamples);
                        if (channelPeak > peak)
                            peak = channelPeak;
                    }

                    if (peak > 0.000001f)
                    {
                        const auto gain = 0.999f / peak;
                        finalAudio.applyGain (gain);
                        normalized = true;
                    }
                }

                AutoSamplerCompletedTake completed;
                completed.rootMidi = capture.rootMidi;
                completed.velocity127 = capture.velocity127;
                completed.velocityLayer = capture.velocityLayer;
                completed.velocityLow = capture.velocityLow;
                completed.velocityHigh = capture.velocityHigh;
                completed.rrIndex = capture.rrIndex;
                completed.sampleRate = currentSampleRate;
                completed.loopSamples = autoSamplerSettings.loopSamples;
                completed.autoLoopMode = autoSamplerSettings.autoLoopMode;
                completed.loopStartPercent = autoSamplerSettings.loopStartPercent;
                completed.loopEndPercent = autoSamplerSettings.loopEndPercent;
                completed.cutLoopAtEnd = autoSamplerSettings.cutLoopAtEnd;
                completed.loopCrossfadeMs = autoSamplerSettings.loopCrossfadeMs;
                completed.normalized = normalized;
                completed.fileName = autoSamplerSettings.instrumentName
                                   + "_"
                                   + midiToNoteToken (capture.rootMidi)
                                   + "_V" + juce::String (capture.velocityLayer)
                                   + "_RR" + juce::String (capture.rrIndex) + ".wav";
                juce::File outputFile = autoSamplerOutputDirectory.getChildFile (completed.fileName);
                if (writeAudioBufferToWavFile (finalAudio, currentSampleRate, outputFile))
                {
                    completed.filePath = outputFile.getFullPathName();
                    ++autoSamplerFilesWritten;
                    SavedAutoSamplerTake savedTake;
                    savedTake.rootMidi = capture.rootMidi;
                    savedTake.velocityLayer = capture.velocityLayer;
                    savedTake.velocityLow = capture.velocityLow;
                    savedTake.velocityHigh = capture.velocityHigh;
                    savedTake.rrIndex = capture.rrIndex;
                    savedTake.normalized = normalized;
                    savedTake.fileName = completed.fileName;
                    autoSamplerSavedTakes.push_back (std::move (savedTake));
                }
                else
                {
                    completed.filePath = outputFile.getFullPathName();
                    ++autoSamplerWriteFailures;
                    writeLoadDebugLog ("autosampler write failed | file=" + completed.filePath);
                }
                completed.audio = std::move (finalAudio);
                completedAutoCaptures.push_back (std::move (completed));
                ++autoSamplerCapturedTakes;

                activeAutoCaptures.erase (activeAutoCaptures.begin() + static_cast<std::ptrdiff_t> (i));
                continue;
            }

            ++i;
        }
    }

    if (autoSamplerActive
        && autoSamplerExpectedTakes > 0
        && autoSamplerCapturedTakes >= autoSamplerExpectedTakes
        && activeAutoCaptures.empty())
    {
        autoSamplerActive = false;
        autoSamplerSendAllNotesOff = true;

        autoSamplerManifestWriteFailed = false;
        autoSamplerManifestPath.clear();

        if (autoSamplerOutputDirectory.isDirectory())
        {
            const auto manifestDirectory = autoSamplerDestinationRoot.isDirectory()
                                         ? autoSamplerDestinationRoot
                                         : autoSamplerOutputDirectory.getParentDirectory();
            auto audioRelativeFolder = normalizePathSlashes (autoSamplerOutputDirectory.getRelativePathFrom (manifestDirectory));
            while (audioRelativeFolder.startsWithChar ('/'))
                audioRelativeFolder = audioRelativeFolder.substring (1);
            while (audioRelativeFolder.startsWithIgnoreCase ("./"))
                audioRelativeFolder = audioRelativeFolder.substring (2);
            if (audioRelativeFolder == ".")
                audioRelativeFolder.clear();
            const auto makeAudioVariantPath = [&audioRelativeFolder] (const juce::String& fileName)
            {
                const auto safeFileName = extractFileNameFromPathString (fileName);
                if (audioRelativeFolder.isEmpty())
                    return safeFileName;
                return normalizePathSlashes (audioRelativeFolder + "/" + safeFileName);
            };
            std::map<std::pair<int, int>, std::vector<SavedAutoSamplerTake>> grouped;
            for (const auto& take : autoSamplerSavedTakes)
                grouped[{ take.rootMidi, take.velocityLayer }].push_back (take);

            auto manifestObject = juce::DynamicObject::Ptr (new juce::DynamicObject());
            manifestObject->setProperty ("formatVersion", 1);
            manifestObject->setProperty ("packType", autoSamplerSettings.keyswitchMode ? "keyswitch" : "instrument");
            manifestObject->setProperty ("instrumentName", autoSamplerSettings.instrumentName);
            if (autoSamplerSettings.keyswitchMode)
                manifestObject->setProperty ("keyswitchKey", autoSamplerSettings.keyswitchKey);
            manifestObject->setProperty ("createdUtc", juce::Time::getCurrentTime().toISO8601 (true));

            auto settingsObject = juce::DynamicObject::Ptr (new juce::DynamicObject());
            settingsObject->setProperty ("intervalSemitones", autoSamplerSettings.intervalSemitones);
            settingsObject->setProperty ("velocityLayers", autoSamplerSettings.velocityLayers);
            settingsObject->setProperty ("roundRobinsPerNote", autoSamplerSettings.roundRobinsPerNote);
            settingsObject->setProperty ("sustainMs", autoSamplerSettings.sustainMs);
            settingsObject->setProperty ("releaseTailMs", autoSamplerSettings.releaseTailMs);
            settingsObject->setProperty ("prerollMs", autoSamplerSettings.prerollMs);
            settingsObject->setProperty ("autoLoopSamples", autoSamplerSettings.loopSamples);
            settingsObject->setProperty ("autoLoopEnabled", autoSamplerSettings.autoLoopMode);
            settingsObject->setProperty ("autoLoopStartPercent", autoSamplerSettings.loopStartPercent);
            settingsObject->setProperty ("autoLoopEndPercent", autoSamplerSettings.loopEndPercent);
            settingsObject->setProperty ("autoLoopCutAtEnd", autoSamplerSettings.cutLoopAtEnd);
            settingsObject->setProperty ("autoLoopCrossfadeMs", autoSamplerSettings.loopCrossfadeMs);
            settingsObject->setProperty ("autoNormalizeRecorded", autoSamplerSettings.normalizeRecorded);
            manifestObject->setProperty ("settings", juce::var (settingsObject.get()));

            juce::Array<juce::var> mapping;
            for (auto& [bucketKey, takes] : grouped)
            {
                std::sort (takes.begin(), takes.end(), [] (const SavedAutoSamplerTake& a, const SavedAutoSamplerTake& b)
                {
                    return a.rrIndex < b.rrIndex;
                });

                auto bucket = juce::DynamicObject::Ptr (new juce::DynamicObject());
                bucket->setProperty ("rootMidiNote", bucketKey.first);
                bucket->setProperty ("velocityLayer", bucketKey.second);

                juce::Array<juce::var> rrVariants;
                for (const auto& take : takes)
                {
                    auto variant = juce::DynamicObject::Ptr (new juce::DynamicObject());
                    variant->setProperty ("path", makeAudioVariantPath (take.fileName));
                    variant->setProperty ("originalFilename", take.fileName);
                    variant->setProperty ("rrIndex", take.rrIndex);
                    variant->setProperty ("loopEnabled", autoSamplerSettings.loopSamples);
                    variant->setProperty ("sampleStartNorm", 0.0);
                    variant->setProperty ("loopStartNorm", juce::jlimit (0.0, 1.0, static_cast<double> (autoSamplerSettings.loopStartPercent) / 100.0));
                    variant->setProperty ("loopEndNorm", juce::jlimit (0.0, 1.0, static_cast<double> (autoSamplerSettings.loopEndPercent) / 100.0));
                    variant->setProperty ("loopFadeInNorm", juce::jlimit (0.0, 1.0, static_cast<double> (autoSamplerSettings.loopStartPercent) / 100.0));
                    rrVariants.add (juce::var (variant.get()));
                }

                bucket->setProperty ("rrVariants", juce::var (rrVariants));
                mapping.add (juce::var (bucket.get()));
            }

            manifestObject->setProperty ("mapping", juce::var (mapping));

            juce::String manifestBaseName = autoSamplerSettings.instrumentName;
            if (autoSamplerSettings.keyswitchMode)
                manifestBaseName = juce::File::createLegalFileName (autoSamplerSettings.instrumentName + "_" + autoSamplerSettings.keyswitchKey);
            if (manifestBaseName.isEmpty())
                manifestBaseName = autoSamplerSettings.keyswitchMode ? "Keyswitch_C0" : "Instrument";

            const auto manifestFile = manifestDirectory.getChildFile (manifestBaseName + ".json");
            const auto manifestJson = juce::JSON::toString (juce::var (manifestObject.get()), true);
            if (manifestFile.replaceWithText (manifestJson, false, false, "\n"))
            {
                autoSamplerManifestPath = manifestFile.getFullPathName();
            }
            else
            {
                autoSamplerManifestWriteFailed = true;
                ++autoSamplerWriteFailures;
                writeLoadDebugLog ("autosampler manifest write failed | file=" + manifestFile.getFullPathName());
            }
        }

        if (autoSamplerWriteFailures > 0)
        {
            autoSamplerStatusMessage = "Sampling finished. Saved "
                                     + juce::String (autoSamplerFilesWritten)
                                     + " file(s), "
                                     + juce::String (autoSamplerWriteFailures)
                                     + " failed.";
        }
        else
        {
            autoSamplerStatusMessage = "Sampling finished. Saved "
                                     + juce::String (autoSamplerFilesWritten)
                                     + " file(s) to "
                                     + autoSamplerOutputDirectory.getFullPathName();
            if (autoSamplerManifestPath.isNotEmpty())
                autoSamplerStatusMessage << " + JSON manifest.";
        }
    }
}

juce::String SamplePlayerAudioProcessor::getZoneNamingHint()
{
    return "Supported filename tags:\n"
           "  note60 / n60 / C3  -> root note\n"
           "  vel1-64 / v65-127  -> velocity layer\n"
           "  rr1 / rr2          -> round robin index\n"
           "\nExample: Piano_C3_vel1-80_rr2.wav";
}

SamplePlayerAudioProcessor::ZoneMetadata SamplePlayerAudioProcessor::parseZoneMetadataFromFileName (const juce::String& fileNameWithoutExtension)
{
    ZoneMetadata metadata;

    auto text = fileNameWithoutExtension.toLowerCase();
    text = text.replaceCharacter ('(', ' ')
               .replaceCharacter (')', ' ')
               .replaceCharacter ('[', ' ')
               .replaceCharacter (']', ' ')
               .replaceCharacter ('{', ' ')
               .replaceCharacter ('}', ' ')
               .replaceCharacter ('.', ' ')
               .replaceCharacter (',', ' ')
               .replaceCharacter ('_', ' ');

    juce::StringArray tokens;
    tokens.addTokens (text, " ", "");
    tokens.removeEmptyStrings();

    for (const auto& token : tokens)
    {
        int parsed = 0;

        if (token.startsWith ("note") && parseStrictInt (token.substring (4), parsed))
        {
            metadata.rootNote = parsed;
            continue;
        }

        if (token.startsWith ("n") && parseStrictInt (token.substring (1), parsed))
        {
            metadata.rootNote = parsed;
            continue;
        }

        if (token.startsWith ("rr") && parseStrictInt (token.substring (2), parsed))
        {
            metadata.roundRobinIndex = parsed;
            continue;
        }

        int low = 0;
        int high = 0;

        if (token.startsWith ("vel") && parseIntRange (token.substring (3), low, high))
        {
            metadata.lowVelocity = low;
            metadata.highVelocity = high;
            continue;
        }

        if (token.startsWith ("v") && parseIntRange (token.substring (1), low, high))
        {
            metadata.lowVelocity = low;
            metadata.highVelocity = high;
            continue;
        }

        if (token.startsWith ("lokey") && parseStrictInt (token.substring (5), parsed))
        {
            metadata.lowNote = parsed;
            continue;
        }

        if (token.startsWith ("hikey") && parseStrictInt (token.substring (5), parsed))
        {
            metadata.highNote = parsed;
            continue;
        }

        if (token.startsWith ("key") && parseIntRange (token.substring (3), low, high))
        {
            metadata.lowNote = low;
            metadata.highNote = high;
            continue;
        }

        if (parseNoteToken (token, parsed))
        {
            metadata.rootNote = parsed;
            continue;
        }
    }

    return sanitizeZoneMetadata (metadata);
}

SamplePlayerAudioProcessor::ZoneMetadata SamplePlayerAudioProcessor::sanitizeZoneMetadata (ZoneMetadata metadata)
{
    metadata.rootNote = juce::jlimit (0, 127, metadata.rootNote);

    metadata.lowNote = juce::jlimit (0, 127, metadata.lowNote);
    metadata.highNote = juce::jlimit (0, 127, metadata.highNote);

    if (metadata.lowNote > metadata.highNote)
        std::swap (metadata.lowNote, metadata.highNote);

    metadata.lowVelocity = juce::jlimit (1, 127, metadata.lowVelocity);
    metadata.highVelocity = juce::jlimit (1, 127, metadata.highVelocity);

    if (metadata.lowVelocity > metadata.highVelocity)
        std::swap (metadata.lowVelocity, metadata.highVelocity);

    metadata.velocityLayer = juce::jlimit (1, 5, metadata.velocityLayer);
    metadata.roundRobinIndex = juce::jmax (1, metadata.roundRobinIndex);
    metadata.mapSetSlot = juce::jmax (0, metadata.mapSetSlot);

    return metadata;
}

bool SamplePlayerAudioProcessor::zoneMetadataEquals (const ZoneMetadata& a, const ZoneMetadata& b)
{
    return a.rootNote == b.rootNote
        && a.lowNote == b.lowNote
        && a.highNote == b.highNote
        && a.lowVelocity == b.lowVelocity
        && a.highVelocity == b.highVelocity
        && a.velocityLayer == b.velocityLayer
        && a.roundRobinIndex == b.roundRobinIndex;
}

bool SamplePlayerAudioProcessor::parseNoteToken (const juce::String& token, int& midiNoteOut)
{
    const auto text = token.toLowerCase();

    if (text.length() < 2)
        return false;

    const auto noteLetter = text[0];

    int semitone = -1;
    switch (noteLetter)
    {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        default: return false;
    }

    int index = 1;

    if (text.length() > index && (text[index] == '#' || text[index] == 'b'))
    {
        semitone += (text[index] == '#') ? 1 : -1;
        ++index;
    }

    const auto octaveText = text.substring (index);
    int octave = 0;

    if (! parseStrictInt (octaveText, octave))
        return false;

    while (semitone < 0)
        semitone += 12;

    semitone %= 12;

    const int midiNote = ((octave + 1) * 12) + semitone;

    if (midiNote < 0 || midiNote > 127)
        return false;

    midiNoteOut = midiNote;
    return true;
}

bool SamplePlayerAudioProcessor::parseIntRange (const juce::String& text, int& low, int& high)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty())
        return false;

    const auto dashPos = trimmed.indexOfChar ('-');

    if (dashPos < 0)
    {
        int single = 0;
        if (! parseStrictInt (trimmed, single))
            return false;

        low = single;
        high = single;
        return true;
    }

    const auto first = trimmed.substring (0, dashPos);
    const auto second = trimmed.substring (dashPos + 1);

    int parsedLow = 0;
    int parsedHigh = 0;

    if (! parseStrictInt (first, parsedLow) || ! parseStrictInt (second, parsedHigh))
        return false;

    low = parsedLow;
    high = parsedHigh;

    return true;
}

juce::String SamplePlayerAudioProcessor::buildSampleSummary (const std::vector<std::shared_ptr<SampleZone>>& zones)
{
    if (zones.empty())
        return "No samples loaded.";

    juce::String summary;
    summary << "Loaded zones: " << static_cast<int> (zones.size()) << "\n\n";

    constexpr int maxRows = 96;

    for (size_t i = 0; i < zones.size() && static_cast<int> (i) < maxRows; ++i)
    {
        const auto& zone = zones[i];
        const auto& m = zone->metadata;

        summary << zone->sourceFile.getFileName() << "\n"
                << "  root: " << m.rootNote
                << " | key: " << m.lowNote << "-" << m.highNote
                << " | vel: " << m.lowVelocity << "-" << m.highVelocity
                << " | rr: " << m.roundRobinIndex << "\n";
    }

    if (static_cast<int> (zones.size()) > maxRows)
        summary << "\n... and " << (static_cast<int> (zones.size()) - maxRows) << " more zones";

    summary << "\n\n" << getZoneNamingHint();

    return summary;
}

void SamplePlayerAudioProcessor::handleMidiMessage (const juce::MidiMessage& message,
                                                    const BlockSettings& settings,
                                                    bool previewMessage)
{
    if (message.isNoteOn())
    {
        const int note = juce::jlimit (0, 127, message.getNoteNumber());
        const int noteVelocity127 = juce::jlimit (1, 127,
                                                  static_cast<int> (std::round (message.getFloatVelocity() * 127.0f)));
        const auto sampleSet = std::atomic_load (&currentSampleSet);
        const int activeSlot = juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));

        if (sampleSet != nullptr)
        {
            const int keyswitchSlot = sampleSet->keyswitchSlotByMidi[static_cast<size_t> (note)];
            if (keyswitchSlot >= 0)
            {
                auto& noteOnCount = midiNoteOnCounts[static_cast<size_t> (note)];
                noteOnCount = juce::jmin (1024, noteOnCount + 1);
                midiNoteLastVelocity127[static_cast<size_t> (note)] = noteVelocity127;
                setMidiHeldState (note, true);
                activeMapSetSlot.store (keyswitchSlot, std::memory_order_relaxed);
                pendingActiveMapSetSlotFromMidi.store (keyswitchSlot, std::memory_order_relaxed);
                bool loopEnabled = true;
                if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (keyswitchSlot); loopIt != sampleSet->loopPlaybackBySlot.end())
                    loopEnabled = loopIt->second;
                activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
                return;
            }

            if (sampleSet->hasKeyswitchSets && note <= 24)
                return;
        }

        {
            const auto processRuntime = [&] (const std::shared_ptr<StepSequencerRuntime>& runtime,
                                             bool isStrumRuntime) -> bool
            {
                if (runtime == nullptr
                    || ! runtime->enabled
                    || sampleSet == nullptr
                    || sampleSet->zones.empty())
                {
                    return false;
                }

                const int previousPlayedNote = runtime->triggerToPlayedNote[static_cast<size_t> (note)];
                if (previousPlayedNote >= 0 && previousPlayedNote <= 127)
                {
                    auto& previousDepth = runtime->playedDepthByMidi[static_cast<size_t> (previousPlayedNote)];
                    if (previousDepth > 0)
                        --previousDepth;
                    setMidiHeldState (previousPlayedNote, previousDepth > 0);
                    if (previousDepth <= 0)
                        releaseVoicesForNote (message.getChannel(), previousPlayedNote, true, settings);
                }

                const bool hadHeldTriggerBefore = std::any_of (runtime->triggerDepthByMidi.begin(),
                                                               runtime->triggerDepthByMidi.end(),
                                                               [] (int depth) { return depth > 0; });

                runtime->triggerDepthByMidi[static_cast<size_t> (note)] = 1;
                runtime->triggerChannelByMidi[static_cast<size_t> (note)] = message.getChannel();

                const auto subdivisionCountForRate = [] (int rateIndex) -> int
                {
                    if (rateIndex == 0) return 1;
                    if (rateIndex == 1) return 2;
                    if (rateIndex == 2) return 4;
                    if (rateIndex == 3) return 8;
                    return 0;
                };

                int stepIndex = 0;
                if (isStrumRuntime)
                {
                    if (! hadHeldTriggerBefore)
                    {
                        runtime->currentStep = 0;
                        runtime->currentSubdivision = 0;
                    }
                    else if (runtime->currentStep < 0 || runtime->currentStep >= static_cast<int> (runtime->steps.size()))
                    {
                        runtime->currentStep = 0;
                        runtime->currentSubdivision = 0;
                    }
                    stepIndex = runtime->currentStep;
                }
                else
                {
                    stepIndex = (juce::jmax (-1, runtime->currentStep) + 1)
                              % static_cast<int> (runtime->steps.size());
                    runtime->currentStep = stepIndex;
                }

                sequencerCurrentStepForUi.store (stepIndex, std::memory_order_relaxed);

                const auto& step = runtime->steps[static_cast<size_t> (stepIndex)];
                if (step.keyswitchSlot >= 0)
                {
                    activeMapSetSlot.store (step.keyswitchSlot, std::memory_order_relaxed);
                    pendingActiveMapSetSlotFromMidi.store (step.keyswitchSlot, std::memory_order_relaxed);
                    bool loopEnabled = true;
                    if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (step.keyswitchSlot);
                        loopIt != sampleSet->loopPlaybackBySlot.end())
                    {
                        loopEnabled = loopIt->second;
                    }
                    activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
                }

                if (step.velocity127 <= 0)
                {
                    runtime->triggerToPlayedNote[static_cast<size_t> (note)] = -1;
                    return true;
                }

                const int playedNote = isStrumRuntime
                    ? note
                    : (runtime->followsInputNote
                        ? note
                        : juce::jlimit (0, 127, step.noteMidi));
                runtime->triggerToPlayedNote[static_cast<size_t> (note)] = playedNote;

                const int stepPlaybackSlot = step.keyswitchSlot >= 0
                    ? step.keyswitchSlot
                    : juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));
                BlockSettings stepSettings = settings;
                if (step.keyswitchSlot >= 0)
                {
                    bool loopEnabled = true;
                    if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (step.keyswitchSlot);
                        loopIt != sampleSet->loopPlaybackBySlot.end())
                    {
                        loopEnabled = loopIt->second;
                    }
                    activeMapLoopPlaybackEnabled.store (loopEnabled, std::memory_order_relaxed);
                    stepSettings.loopEnabled = loopEnabled;
                }

                auto& playedDepth = runtime->playedDepthByMidi[static_cast<size_t> (playedNote)];
                playedDepth = juce::jmin (1024, playedDepth + 1);

                setMidiHeldState (note, false);
                setMidiHeldState (playedNote, true);

                int velocity127 = juce::jlimit (1, 127, step.velocity127);
                {
                    const int subdivisions = subdivisionCountForRate (step.rateIndex);
                    if (subdivisions <= 0)
                    {
                        runtime->samplesUntilNextSubstep = juce::jmax (1, static_cast<int> (currentSampleRate * 0.5));
                        runtime->samplesUntilNextStep = runtime->samplesUntilNextSubstep;
                        runtime->currentSubdivision = 0;
                        if (! isStrumRuntime)
                            runtime->ratchetSubsRemaining = 0;
                        runtime->currentStep = (runtime->currentStep + 1) % static_cast<int> (runtime->steps.size());
                        return true;
                    }

                    if (isStrumRuntime)
                    {
                        const int subIndex = juce::jlimit (0, subdivisions - 1, runtime->currentSubdivision);
                        velocity127 = juce::jlimit (1, 127, step.subVelocities[static_cast<size_t> (subIndex)]);
                        runtime->currentSubdivision = (runtime->currentSubdivision + 1) % subdivisions;
                        if (runtime->currentSubdivision == 0)
                            runtime->currentStep = (runtime->currentStep + 1) % static_cast<int> (runtime->steps.size());
                    }
                    else
                    {
                        // Step sequencer: fire first subdivision now, set up ratchet for the rest
                        runtime->currentSubdivision = 0;
                        runtime->ratchetNote          = playedNote;
                        runtime->ratchetVelocity127   = velocity127;
                        runtime->ratchetChannel       = message.getChannel();
                        runtime->ratchetPlaybackSlot  = stepPlaybackSlot;
                        runtime->ratchetKeyswitchSlot = step.keyswitchSlot;
                        runtime->ratchetDoubling      = sequencerDoublingEnabled.load (std::memory_order_relaxed);
                        runtime->ratchetSubsRemaining = subdivisions - 1;
                    }

                    const int interval = juce::jmax (1,
                        static_cast<int> ((currentSampleRate * 0.5) / static_cast<double> (subdivisions)));
                    runtime->samplesUntilNextSubstep = interval;
                    runtime->samplesUntilNextStep = interval;
                }

                const float velocity01 = static_cast<float> (velocity127) / 127.0f;
                const bool canDouble = sequencerDoublingEnabled.load (std::memory_order_relaxed)
                                    || runtime->doubling;
                if (canDouble)
                {
                    const auto leftZone = startVoiceForNoteInternal (message.getChannel(),
                                                                     playedNote,
                                                                     velocity01,
                                                                     stepSettings,
                                                                     false,
                                                                     -1.0f,
                                                                     0,
                                                                     nullptr,
                                                                     stepPlaybackSlot);
                    startVoiceForNoteInternal (message.getChannel(),
                                               playedNote,
                                               velocity01,
                                               stepSettings,
                                               true,
                                               1.0f,
                                               1,
                                               leftZone.get(),
                                               stepPlaybackSlot);
                }
                else
                {
                    startVoiceForNoteInternal (message.getChannel(),
                                               playedNote,
                                               velocity01,
                                               stepSettings,
                                               false,
                                               0.0f,
                                               0,
                                               nullptr,
                                               stepPlaybackSlot);
                }

                midiNoteLastVelocity127[static_cast<size_t> (playedNote)] = velocity127;
                triggerAuxiliaryKeyswitchSlots (true,
                                                message.getChannel(),
                                                playedNote,
                                                velocity01,
                                                stepSettings,
                                                stepPlaybackSlot);
                return true;
            };

            {
                auto strumRT = std::atomic_load (&strumSequencerRuntime);
                const bool strumEnabled = strumRT != nullptr && strumRT->enabled;
                if (strumEnabled && processRuntime (strumRT, true))
                    return;
            }

            if (! previewMessage)
            {
                if (processRuntime (std::atomic_load (&stepSequencerRuntime), false))
                    return;
            }
        }

        bool hasAnyZoneInActiveSlot = false;
        if (sampleSet != nullptr)
        {
            for (const auto& zone : sampleSet->zones)
            {
                if (zone != nullptr && zone->metadata.mapSetSlot == activeSlot)
                {
                    hasAnyZoneInActiveSlot = true;
                    break;
                }
            }
        }

        if (! hasAnyZoneInActiveSlot)
        {
            setMidiHeldState (note, false);
            return;
        }

        auto& noteOnCount = midiNoteOnCounts[static_cast<size_t> (note)];
        noteOnCount = juce::jmin (1024, noteOnCount + 1);
        midiNoteLastVelocity127[static_cast<size_t> (note)] = noteVelocity127;
        setMidiHeldState (note, true);

        {
            auto strumRT = std::atomic_load (&strumSequencerRuntime);
            const bool doublingOn = strumDoublingEnabled.load (std::memory_order_relaxed)
                || (strumRT != nullptr && strumRT->doubling);
            if (doublingOn)
            {
                const auto leftZone = startVoiceForNoteInternal (message.getChannel(), note, message.getFloatVelocity(), settings, false, -1.0f, 0);
                startVoiceForNoteInternal (message.getChannel(), note, message.getFloatVelocity(), settings, true, 1.0f, 1, leftZone.get());
            }
            else
            {
                startVoiceForNote (message.getChannel(), note, message.getFloatVelocity(), settings);
            }
        }

        triggerAuxiliaryKeyswitchSlots (true,
                                        message.getChannel(),
                                        note,
                                        message.getFloatVelocity(),
                                        settings,
                                        activeSlot);
        return;
    }

    if (message.isNoteOff())
    {
        const int note = juce::jlimit (0, 127, message.getNoteNumber());

        {
            const auto releaseRuntimeForNote = [&] (const std::shared_ptr<StepSequencerRuntime>& runtime) -> bool
            {
                if (runtime == nullptr || ! runtime->enabled)
                    return false;

                const int playedNote = runtime->triggerToPlayedNote[static_cast<size_t> (note)];
                if (playedNote < 0 || playedNote > 127)
                    return false;

                setMidiHeldState (note, false);
                runtime->triggerDepthByMidi[static_cast<size_t> (note)] = 0;
                runtime->triggerToPlayedNote[static_cast<size_t> (note)] = -1;
                runtime->triggerChannelByMidi[static_cast<size_t> (note)] = 1;

                auto& playedDepth = runtime->playedDepthByMidi[static_cast<size_t> (playedNote)];
                if (playedDepth > 0)
                    --playedDepth;
                setMidiHeldState (playedNote, playedDepth > 0);
                if (playedDepth <= 0)
                {
                    releaseVoicesForNote (message.getChannel(), playedNote, true, settings);
                    const int releaseVelocity127 = juce::jmax (1, midiNoteLastVelocity127[static_cast<size_t> (playedNote)]);
                    triggerAuxiliaryKeyswitchSlots (false,
                                                    message.getChannel(),
                                                    playedNote,
                                                    static_cast<float> (releaseVelocity127) / 127.0f,
                                                    settings,
                                                    -1);
                }
                return true;
            };

            if (releaseRuntimeForNote (std::atomic_load (&strumSequencerRuntime)))
                return;

            if (! previewMessage)
            {
                if (releaseRuntimeForNote (std::atomic_load (&stepSequencerRuntime)))
                    return;
            }
        }

        if (const auto sampleSet = std::atomic_load (&currentSampleSet); sampleSet != nullptr)
        {
            const int keyswitchSlot = sampleSet->keyswitchSlotByMidi[static_cast<size_t> (note)];
            if (keyswitchSlot >= 0)
            {
                auto& noteOnCount = midiNoteOnCounts[static_cast<size_t> (note)];
                if (noteOnCount > 0)
                    --noteOnCount;
                setMidiHeldState (note, noteOnCount > 0);
                return;
            }
        }

        auto& noteOnCount = midiNoteOnCounts[static_cast<size_t> (note)];

        if (noteOnCount > 0)
            --noteOnCount;
        setMidiHeldState (note, noteOnCount > 0);

        // Keep the newest retriggered note held when overlapped note-offs arrive.
        if (noteOnCount > 0)
            return;

        releaseVoicesForNote (message.getChannel(), note, true, settings);
        const int releaseVelocity127 = juce::jmax (1, midiNoteLastVelocity127[static_cast<size_t> (note)]);
        triggerAuxiliaryKeyswitchSlots (false,
                                        message.getChannel(),
                                        note,
                                        static_cast<float> (releaseVelocity127) / 127.0f,
                                        settings,
                                        -1);
        return;
    }

    if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        auto sequencerRuntime = std::atomic_load (&stepSequencerRuntime);
        if (sequencerRuntime != nullptr)
        {
            sequencerRuntime->triggerDepthByMidi.fill (0);
            sequencerRuntime->triggerToPlayedNote.fill (-1);
            sequencerRuntime->playedDepthByMidi.fill (0);
            sequencerRuntime->currentStep = -1;
            sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
        }
        stopAllVoices();
        return;
    }

    if (message.isController() && message.getControllerNumber() == 123)
    {
        auto sequencerRuntime = std::atomic_load (&stepSequencerRuntime);
        if (sequencerRuntime != nullptr)
        {
            sequencerRuntime->triggerDepthByMidi.fill (0);
            sequencerRuntime->triggerToPlayedNote.fill (-1);
            sequencerRuntime->playedDepthByMidi.fill (0);
            sequencerRuntime->currentStep = -1;
            sequencerCurrentStepForUi.store (-1, std::memory_order_relaxed);
        }
        stopAllVoices();
        return;
    }

    if (message.isController() && message.getControllerNumber() == 1)
    {
        const auto cc = juce::jlimit (0, 127, message.getControllerValue());
        const float value01 = static_cast<float> (cc) / 127.0f;
        modwheelVelocityLayerControlSeenFromMidi.store (true, std::memory_order_relaxed);
        modwheelVelocityLayerControlEnabled.store (true, std::memory_order_relaxed);
        modwheelVelocityLayerControlValue01.store (value01, std::memory_order_relaxed);
        if (auto* modParam = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (kModWheelParamId)))
        {
            if (std::abs (modParam->getValue() - value01) > 0.0005f)
                modParam->setValueNotifyingHost (value01);
        }
        return;
    }

    if (message.isController() && message.getControllerNumber() == 11)
    {
        const auto cc = juce::jlimit (0, 127, message.getControllerValue());
        const float value01 = static_cast<float> (cc) / 127.0f;
        expressionControllerValue01.store (value01, std::memory_order_relaxed);
        if (auto* expressionParam = dynamic_cast<juce::RangedAudioParameter*> (parameters.getParameter (kExpressionParamId)))
        {
            if (std::abs (expressionParam->getValue() - value01) > 0.0005f)
                expressionParam->setValueNotifyingHost (value01);
        }
        return;
    }
}

void SamplePlayerAudioProcessor::startVoiceForNote (int midiChannel,
                                                     int midiNoteNumber,
                                                     float velocity,
                                                     const BlockSettings& settings)
{
    const int velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (velocity * 127.0f)));
    bool usedModwheelLayerSelection = false;
    auto zone = pickZoneForNote (midiNoteNumber,
                                 velocity127,
                                 &usedModwheelLayerSelection,
                                 0,
                                 nullptr,
                                 -1);
    if (zone == nullptr)
        return;

    const auto sampleSet = std::atomic_load (&currentSampleSet);
    std::vector<int> velocityLayers;
    bool useRealtimeLayerBlend = false;

    if (sampleSet != nullptr && modwheelVelocityLayerControlEnabled.load (std::memory_order_relaxed))
    {
        if (const auto slotIt = sampleSet->velocityLayersBySlotRoot.find (zone->metadata.mapSetSlot);
            slotIt != sampleSet->velocityLayersBySlotRoot.end())
        {
            if (const auto rootIt = slotIt->second.find (zone->metadata.rootNote);
                rootIt != slotIt->second.end() && rootIt->second.size() > 1)
            {
                velocityLayers = rootIt->second;
                useRealtimeLayerBlend = true;
            }
        }
    }

    startVoiceFromZone (midiChannel,
                        midiNoteNumber,
                        velocity,
                        settings,
                        zone,
                        false,
                        false,
                        useRealtimeLayerBlend,
                        usedModwheelLayerSelection,
                        0.0f);

    if (! useRealtimeLayerBlend)
        return;

    const int preferredRoundRobinIndex = zone->metadata.roundRobinIndex;
    for (const auto layer : velocityLayers)
    {
        if (layer == zone->metadata.velocityLayer)
            continue;

        auto layerZone = pickZoneForRootLayer (midiNoteNumber,
                                               zone->metadata.rootNote,
                                               layer,
                                               preferredRoundRobinIndex,
                                               zone->metadata.mapSetSlot);
        if (layerZone == nullptr)
            continue;

        startVoiceFromZone (midiChannel,
                            midiNoteNumber,
                            velocity,
                            settings,
                            layerZone,
                            true,
                            false,
                            true,
                            true,
                            0.0f);
    }
}

std::shared_ptr<const SamplePlayerAudioProcessor::SampleZone> SamplePlayerAudioProcessor::startVoiceFromZone (int midiChannel,
                                                                                                                int midiNoteNumber,
                                                                                                                float velocity,
                                                                                                                const BlockSettings& settings,
                                                                                                                std::shared_ptr<const SampleZone> selectedZone,
                                                                                                                bool suppressMonoCut,
                                                                                                                bool useRetriggerFadeTail,
                                                                                                                bool ignoreMonoNoteDedupeForVoice,
                                                                                                                bool usedModwheelLayerSelection,
                                                                                                                float pan)
{
    const int velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (velocity * 127.0f)));

    if (selectedZone == nullptr)
        return {};

    if (! suppressMonoCut)
    {
        for (auto& existing : voices)
        {
            if (! existing.active)
                continue;

            if (existing.midiNote != midiNoteNumber)
                continue;

            if (useRetriggerFadeTail)
                startStealTailFromVoice (existing, strumRetriggerFadeOutMs);

            existing = VoiceState {};
        }
    }

    auto* voice = findFreeVoice();
    if (voice == nullptr)
    {
        voice = stealOldestVoice();
        if (voice != nullptr)
            startStealTailFromVoice (*voice);
    }

    if (voice == nullptr)
        return {};

    *voice = VoiceState {};
    voice->active = true;
    voice->midiNote = midiNoteNumber;
    voice->midiChannel = midiChannel;
    voice->zone = selectedZone;
    voice->position = 0.0;
    const bool ignoreMidiVelocity = usedModwheelLayerSelection && settings.loopEnabled;
    voice->velocityGain = ignoreMidiVelocity ? 1.0f : (velocity127 * velocityScale);
    voice->age = ++voiceAgeCounter;

    bool oneShotPlayback = false;
    if (const auto sampleSet = std::atomic_load (&currentSampleSet); sampleSet != nullptr)
    {
        if (const auto it = sampleSet->oneShotPlaybackBySlot.find (voice->zone->metadata.mapSetSlot);
            it != sampleSet->oneShotPlaybackBySlot.end())
        {
            oneShotPlayback = it->second;
        }
    }

    const int playbackNote = oneShotPlayback ? voice->zone->metadata.rootNote : midiNoteNumber;
    const auto semitoneOffset = static_cast<double> (playbackNote - voice->zone->metadata.rootNote);
    const auto pitch = std::pow (2.0, semitoneOffset / 12.0);
    const auto sampleRateRatio = voice->zone->sourceSampleRate / juce::jmax (1.0, currentSampleRate);
    const int pitchDownOctaves = juce::jlimit (0, 2, playerPitchDownOctaves.load (std::memory_order_relaxed));
    const auto octaveDownRatio = std::pow (2.0, -static_cast<double> (pitchDownOctaves));

    voice->pitchRatio = juce::jmax (0.0001, sampleRateRatio * pitch * octaveDownRatio);

    voice->sustainLevel = juce::jlimit (0.0f, 1.0f, settings.sustainLevel);
    voice->pan = juce::jlimit (-1.0f, 1.0f, pan);
    const float leftGain = std::sqrt (0.5f * (1.0f - voice->pan));
    const float rightGain = std::sqrt (0.5f * (1.0f + voice->pan));
    voice->panGains = { leftGain, rightGain };
    voice->ignoreMonoNoteDedupe = ignoreMonoNoteDedupeForVoice || suppressMonoCut;

    if (pan < -0.5f)
        voice->delaySamplesRemaining = msToSamples (currentSampleRate, 10.0f);

    voice->attackSamplesRemaining = msToSamples (currentSampleRate, settings.attackMs);
    if (voice->attackSamplesRemaining > 0)
    {
        voice->envelopeGain = 0.0f;
        voice->attackDelta = 1.0f / static_cast<float> (voice->attackSamplesRemaining);
    }
    else
    {
        voice->envelopeGain = 1.0f;
    }

    voice->decaySamplesRemaining = msToSamples (currentSampleRate, settings.decayMs);
    if (voice->decaySamplesRemaining > 0)
    {
        voice->decayDelta = (1.0f - voice->sustainLevel) / static_cast<float> (voice->decaySamplesRemaining);
    }
    else if (voice->attackSamplesRemaining <= 0)
    {
        voice->envelopeGain = voice->sustainLevel;
    }

    return selectedZone;
}

void SamplePlayerAudioProcessor::triggerAuxiliaryKeyswitchSlots (bool triggerOnNoteOn,
                                                                 int midiChannel,
                                                                 int midiNoteNumber,
                                                                 float velocity,
                                                                 const BlockSettings& settings,
                                                                 int primarySlotToSkip)
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);
    if (sampleSet == nullptr || sampleSet->zones.empty())
        return;

    const auto& triggerSlots = triggerOnNoteOn ? sampleSet->noteOnTriggerSlots
                                               : sampleSet->noteOffTriggerSlots;
    if (triggerSlots.empty())
        return;

    const float safeVelocity = juce::jlimit (1.0f / 127.0f, 1.0f, velocity);

    for (const auto slot : triggerSlots)
    {
        if (slot < 0 || slot == primarySlotToSkip)
            continue;

        BlockSettings slotSettings = settings;
        if (const auto loopIt = sampleSet->loopPlaybackBySlot.find (slot);
            loopIt != sampleSet->loopPlaybackBySlot.end())
        {
            slotSettings.loopEnabled = loopIt->second;
        }

        startVoiceForNoteInternal (midiChannel,
                                   midiNoteNumber,
                                   safeVelocity,
                                   slotSettings,
                                   true,
                                   0.0f,
                                   0,
                                   nullptr,
                                   slot);
    }
}

std::shared_ptr<const SamplePlayerAudioProcessor::SampleZone> SamplePlayerAudioProcessor::startVoiceForNoteInternal (int midiChannel,
                                                                                                                        int midiNoteNumber,
                                                                                                                        float velocity,
                                                                                                                        const BlockSettings& settings,
                                                                                                                        bool suppressMonoCut,
                                                                                                                        float pan,
                                                                                                                        int rrOffset,
                                                                                                                        const SampleZone* excludedZone,
                                                                                                                        int forcedMapSetSlot)
{
    const int velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (velocity * 127.0f)));
    bool usedModwheelLayerSelection = false;
    auto zone = pickZoneForNote (midiNoteNumber,
                                 velocity127,
                                 &usedModwheelLayerSelection,
                                 rrOffset,
                                 excludedZone,
                                 forcedMapSetSlot);

    return startVoiceFromZone (midiChannel,
                               midiNoteNumber,
                               velocity,
                               settings,
                               zone,
                               suppressMonoCut,
                               forcedMapSetSlot >= 0,
                               false,
                               usedModwheelLayerSelection,
                               pan);
}

void SamplePlayerAudioProcessor::releaseVoicesForNote (int midiChannel,
                                                        int midiNoteNumber,
                                                        bool allowTailOff,
                                                        const BlockSettings& settings)
{
    juce::ignoreUnused (midiChannel);

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        if (voice.midiNote != midiNoteNumber)
            continue;

        if (! allowTailOff)
        {
            voice.active = false;
            continue;
        }

        tryStartRelease (voice, settings);
    }
}

void SamplePlayerAudioProcessor::enforceSingleVoicePerMidiNote()
{
    std::array<VoiceState*, 128> newestVoiceByNote {};

    for (auto& voice : voices)
    {
        if (! voice.active || voice.ignoreMonoNoteDedupe)
            continue;

        const int note = juce::jlimit (0, 127, voice.midiNote);
        auto*& newest = newestVoiceByNote[static_cast<size_t> (note)];

        if (newest == nullptr || voice.age > newest->age)
            newest = &voice;
    }

    for (auto& voice : voices)
    {
        if (! voice.active || voice.ignoreMonoNoteDedupe)
            continue;

        const int note = juce::jlimit (0, 127, voice.midiNote);
        if (newestVoiceByNote[static_cast<size_t> (note)] == &voice)
            continue;

        voice = VoiceState {};
    }
}

void SamplePlayerAudioProcessor::setMidiHeldState (int midiNote, bool held) noexcept
{
    const int clampedNote = juce::jlimit (0, 127, midiNote);
    const int bitIndex = clampedNote & 63;
    const juce::uint64 bit = juce::uint64 (1) << static_cast<juce::uint64> (bitIndex);
    auto& mask = clampedNote < 64 ? midiHeldMaskLo : midiHeldMaskHi;

    auto current = mask.load (std::memory_order_relaxed);
    while (true)
    {
        const auto desired = held ? (current | bit) : (current & ~bit);
        if (mask.compare_exchange_weak (current, desired, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
}

void SamplePlayerAudioProcessor::stopAllVoices()
{
    for (auto& voice : voices)
        voice = VoiceState {};

    midiNoteOnCounts.fill (0);
    midiNoteLastVelocity127.fill (0);
    midiHeldMaskLo.store (0, std::memory_order_relaxed);
    midiHeldMaskHi.store (0, std::memory_order_relaxed);
}

SamplePlayerAudioProcessor::VoiceState* SamplePlayerAudioProcessor::findFreeVoice()
{
    for (int i = 0; i < maxPlayableVoices; ++i)
    {
        auto& voice = voices[static_cast<size_t> (i)];
        if (! voice.active)
            return &voice;
    }

    return nullptr;
}

SamplePlayerAudioProcessor::VoiceState* SamplePlayerAudioProcessor::findFreeStealTailVoice()
{
    VoiceState* oldestTail = nullptr;

    for (int i = maxPlayableVoices; i < maxVoices; ++i)
    {
        auto& voice = voices[static_cast<size_t> (i)];
        if (! voice.active || voice.zone == nullptr)
            return &voice;

        if (oldestTail == nullptr || voice.age < oldestTail->age)
            oldestTail = &voice;
    }

    return oldestTail;
}

SamplePlayerAudioProcessor::VoiceState* SamplePlayerAudioProcessor::stealOldestVoice()
{
    VoiceState* oldest = nullptr;

    for (int i = 0; i < maxPlayableVoices; ++i)
    {
        auto& voice = voices[static_cast<size_t> (i)];
        if (! voice.active)
            continue;

        if (oldest == nullptr || voice.age < oldest->age)
            oldest = &voice;
    }

    return oldest;
}

void SamplePlayerAudioProcessor::startStealTailFromVoice (const VoiceState& sourceVoice, float fadeOutMs)
{
    if (! sourceVoice.active || sourceVoice.zone == nullptr)
        return;

    auto* tailVoice = findFreeStealTailVoice();
    if (tailVoice == nullptr)
        return;

    *tailVoice = sourceVoice;
    tailVoice->attackSamplesRemaining = 0;
    tailVoice->attackDelta = 0.0f;
    tailVoice->decaySamplesRemaining = 0;
    tailVoice->decayDelta = 0.0f;

    const int fadeSamples = juce::jmax (1, msToSamples (currentSampleRate, fadeOutMs));
    const auto startEnvelope = juce::jmax (0.0f, tailVoice->envelopeGain);
    tailVoice->releaseSamplesRemaining = fadeSamples;
    tailVoice->releaseDelta = juce::jmax (0.000001f,
                                          startEnvelope / static_cast<float> (fadeSamples));
    tailVoice->ignoreMonoNoteDedupe = true;
    tailVoice->age = ++voiceAgeCounter;
}

std::shared_ptr<const SamplePlayerAudioProcessor::SampleZone> SamplePlayerAudioProcessor::pickZoneForNote (int midiNoteNumber,
                                                                                                              int velocity127,
                                                                                                              bool* usedModwheelLayerSelection,
                                                                                                              int rrOffset,
                                                                                                              const SampleZone* excludedZone,
                                                                                                              int forcedMapSetSlot)
{
    if (usedModwheelLayerSelection != nullptr)
        *usedModwheelLayerSelection = false;

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr || sampleSet->zones.empty())
        return {};

    const int activeSlot = forcedMapSetSlot >= 0
        ? forcedMapSetSlot
        : juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));
    const bool oneShotPlayback = [&sampleSet, activeSlot]() -> bool
    {
        if (const auto it = sampleSet->oneShotPlaybackBySlot.find (activeSlot); it != sampleSet->oneShotPlaybackBySlot.end())
            return it->second;
        return false;
    }();

    std::vector<std::shared_ptr<const SampleZone>> noteAndVelocityMatches;
    std::vector<std::shared_ptr<const SampleZone>> noteOnlyMatches;

    for (const auto& zone : sampleSet->zones)
    {
        const auto& m = zone->metadata;
        if (m.mapSetSlot != activeSlot)
            continue;

        if (! oneShotPlayback && (midiNoteNumber < m.lowNote || midiNoteNumber > m.highNote))
            continue;

        noteOnlyMatches.push_back (zone);
    }

    int selectionVelocity = juce::jlimit (1, 127, velocity127);
    if (modwheelVelocityLayerControlEnabled.load (std::memory_order_relaxed))
    {
        std::pair<int, int> firstBounds { -1, -1 };
        bool hasMultipleVelocityLayers = false;

        for (const auto& zone : noteOnlyMatches)
        {
            const std::pair<int, int> bounds { zone->metadata.lowVelocity, zone->metadata.highVelocity };

            if (firstBounds.first < 0)
                firstBounds = bounds;
            else if (bounds != firstBounds)
            {
                hasMultipleVelocityLayers = true;
                break;
            }
        }

        if (hasMultipleVelocityLayers)
        {
            const auto modwheel01 = juce::jlimit (0.0f, 1.0f,
                                                  modwheelVelocityLayerControlValue01.load (std::memory_order_relaxed));
            selectionVelocity = juce::jlimit (1, 127,
                                              1 + static_cast<int> (std::round (modwheel01 * 126.0f)));
            if (usedModwheelLayerSelection != nullptr)
                *usedModwheelLayerSelection = true;
        }
    }

    for (const auto& zone : noteOnlyMatches)
    {
        if (selectionVelocity >= zone->metadata.lowVelocity
            && selectionVelocity <= zone->metadata.highVelocity)
            noteAndVelocityMatches.push_back (zone);
    }

    auto* candidatePool = &noteAndVelocityMatches;
    if (candidatePool->empty())
        candidatePool = &noteOnlyMatches;

    // Nearest-root fallback: when no zone explicitly covers the played note,
    // find zones sharing the closest root and pitch-shift to it.  This ensures
    // stretched key ranges always sound even when explicit low/high metadata
    // is missing, stale, or computed differently than the UI display.
    if (! oneShotPlayback && candidatePool->empty())
    {
        int nearestRoot = -1;
        int nearestDistance = 999;

        for (const auto& zone : sampleSet->zones)
        {
            if (zone == nullptr)
                continue;

            const auto& m = zone->metadata;
            if (m.mapSetSlot != activeSlot)
                continue;

            const int dist = std::abs (m.rootNote - midiNoteNumber);
            if (dist < nearestDistance)
            {
                nearestDistance = dist;
                nearestRoot = m.rootNote;
            }
        }

        if (nearestRoot >= 0)
        {
            for (const auto& zone : sampleSet->zones)
            {
                if (zone == nullptr)
                    continue;

                const auto& m = zone->metadata;
                if (m.mapSetSlot != activeSlot)
                    continue;

                if (m.rootNote != nearestRoot)
                    continue;

                noteOnlyMatches.push_back (zone);
                if (selectionVelocity >= m.lowVelocity && selectionVelocity <= m.highVelocity)
                    noteAndVelocityMatches.push_back (zone);
            }

            candidatePool = &noteAndVelocityMatches;
            if (candidatePool->empty())
                candidatePool = &noteOnlyMatches;
        }
    }

    if (candidatePool->empty())
        return {};

    std::sort (candidatePool->begin(), candidatePool->end(), [] (const auto& a, const auto& b)
    {
        if (a->metadata.roundRobinIndex != b->metadata.roundRobinIndex)
            return a->metadata.roundRobinIndex < b->metadata.roundRobinIndex;

        if (a->metadata.lowVelocity != b->metadata.lowVelocity)
            return a->metadata.lowVelocity < b->metadata.lowVelocity;

        return a->sourceFile.getFileName() < b->sourceFile.getFileName();
    });

    const int rrKey = (activeSlot << 8) | (oneShotPlayback ? 255 : juce::jlimit (0, 127, midiNoteNumber));
    const auto poolSize = static_cast<int> (candidatePool->size());
    const auto zoneChoiceId = [] (const SampleZone& zone) -> juce::uint64
    {
        juce::uint64 hash = static_cast<juce::uint64> (zone.sourceFile.getFullPathName().hashCode64());
        hash ^= static_cast<juce::uint64> ((zone.metadata.roundRobinIndex + 1) * 131);
        hash ^= (static_cast<juce::uint64> (zone.metadata.lowVelocity + 1) << 24);
        hash ^= (static_cast<juce::uint64> (zone.metadata.highVelocity + 1) << 40);
        return hash;
    };

    const juce::uint64 excludedChoiceId = excludedZone != nullptr ? zoneChoiceId (*excludedZone) : 0;

    if (forcedMapSetSlot >= 0)
    {
        auto& rrCounter = roundRobinCounters[rrKey];
        int chosenIndex = poolSize > 0
            ? ((rrCounter + juce::jmax (0, rrOffset)) % poolSize)
            : 0;

        if (excludedChoiceId != 0 && poolSize > 1)
        {
            for (int attempt = 0; attempt < poolSize; ++attempt)
            {
                const int candidateIndex = (chosenIndex + attempt) % poolSize;
                const auto& candidate = candidatePool->at (static_cast<size_t> (candidateIndex));
                if (candidate != nullptr && zoneChoiceId (*candidate) != excludedChoiceId)
                {
                    chosenIndex = candidateIndex;
                    break;
                }
            }
        }

        if (excludedZone == nullptr)
            rrCounter = (rrCounter + 1) % 8192;

        return candidatePool->at (static_cast<size_t> (chosenIndex));
    }

    // Random RR selection with last-2 exclusion.
    // Rule: never repeat the last 2 played indices (when pool is large enough).
    // When doubling: excludedZone ensures L and R don't pick the same sample.
    auto& history = rrHistory[rrKey];

    auto nextRandom = [&]() -> int
    {
        rrRandomState = rrRandomState * 1664525u + 1013904223u;
        return static_cast<int> ((rrRandomState >> 8) & 0x00ffffffu);
    };

    int chosenIndex = 0;

    if (poolSize <= 1)
    {
        chosenIndex = 0;
    }
    else if (poolSize == 2)
    {
        // With only 2 options, just avoid last-1 played
        chosenIndex = nextRandom() % poolSize;
        if (chosenIndex == history[0])
            chosenIndex = (chosenIndex + 1) % poolSize;
    }
    else
    {
        // Pool >= 3: avoid last-2 played indices
        const int maxAttempts = poolSize * 2;
        for (int attempt = 0; attempt < maxAttempts; ++attempt)
        {
            chosenIndex = nextRandom() % poolSize;
            if (chosenIndex != history[0] && chosenIndex != history[1])
                break;
        }
    }

    // If excludedZone is set (doubling right voice), ensure we don't pick same as left
    if (excludedChoiceId != 0 && poolSize > 1)
    {
        const auto& chosen = candidatePool->at (static_cast<size_t> (chosenIndex));
        if (chosen != nullptr && zoneChoiceId (*chosen) == excludedChoiceId)
        {
            for (int attempt = 0; attempt < poolSize; ++attempt)
            {
                const int candidateIndex = (chosenIndex + 1 + attempt) % poolSize;
                const auto& candidate = candidatePool->at (static_cast<size_t> (candidateIndex));
                if (candidate != nullptr && zoneChoiceId (*candidate) != excludedChoiceId)
                {
                    chosenIndex = candidateIndex;
                    break;
                }
            }
        }
    }

    // Update history: shift [0] -> [1], store new in [0]
    // Skip for doubled right voices (excludedZone set) so the doubled pair
    // doesn't consume two history slots per musical event.
    if (excludedZone == nullptr)
    {
        history[1] = history[0];
        history[0] = chosenIndex;
    }

    return candidatePool->at (static_cast<size_t> (chosenIndex));
}

std::shared_ptr<const SamplePlayerAudioProcessor::SampleZone> SamplePlayerAudioProcessor::pickZoneForRootLayer (int midiNoteNumber,
                                                                                                                  int rootNote,
                                                                                                                  int velocityLayer,
                                                                                                                  int preferredRoundRobinIndex,
                                                                                                                  int forcedMapSetSlot) const
{
    juce::ignoreUnused (midiNoteNumber);

    const auto sampleSet = std::atomic_load (&currentSampleSet);
    if (sampleSet == nullptr || sampleSet->zones.empty())
        return {};

    const int activeSlot = forcedMapSetSlot >= 0
        ? forcedMapSetSlot
        : juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));

    std::vector<std::shared_ptr<const SampleZone>> candidates;
    for (const auto& zone : sampleSet->zones)
    {
        if (zone == nullptr)
            continue;

        const auto& metadata = zone->metadata;
        if (metadata.mapSetSlot != activeSlot)
            continue;
        if (metadata.rootNote != rootNote)
            continue;
        if (metadata.velocityLayer != velocityLayer)
            continue;

        candidates.push_back (zone);
    }

    if (candidates.empty())
        return {};

    std::sort (candidates.begin(), candidates.end(), [] (const auto& a, const auto& b)
    {
        if (a->metadata.roundRobinIndex != b->metadata.roundRobinIndex)
            return a->metadata.roundRobinIndex < b->metadata.roundRobinIndex;

        return a->sourceFile.getFileName() < b->sourceFile.getFileName();
    });

    if (preferredRoundRobinIndex > 0)
    {
        for (const auto& zone : candidates)
        {
            if (zone != nullptr && zone->metadata.roundRobinIndex == preferredRoundRobinIndex)
                return zone;
        }
    }

    return candidates.front();
}

bool SamplePlayerAudioProcessor::hasMultipleRoundRobinsForNote (int midiNoteNumber, int velocity127) const
{
    const auto sampleSet = std::atomic_load (&currentSampleSet);
    if (sampleSet == nullptr || sampleSet->zones.empty())
        return false;

    const int clampedNote = juce::jlimit (0, 127, midiNoteNumber);
    const int clampedVelocity = juce::jlimit (1, 127, velocity127);
    const int activeSlot = juce::jmax (0, activeMapSetSlot.load (std::memory_order_relaxed));

    std::set<int> rrMatches;
    std::set<int> rrNoteMatches;

    for (const auto& zone : sampleSet->zones)
    {
        if (zone == nullptr)
            continue;

        const auto& m = zone->metadata;
        if (m.mapSetSlot != activeSlot)
            continue;

        if (clampedNote < m.lowNote || clampedNote > m.highNote)
            continue;

        rrNoteMatches.insert (m.roundRobinIndex);
        if (clampedVelocity >= m.lowVelocity && clampedVelocity <= m.highVelocity)
            rrMatches.insert (m.roundRobinIndex);
    }

    if (rrMatches.size() > 1)
        return true;

    if (! rrMatches.empty() || ! rrNoteMatches.empty())
        return rrMatches.empty() && rrNoteMatches.size() > 1;

    // Nearest-root fallback for stretched notes outside explicit ranges.
    int nearestRoot = -1;
    int nearestDistance = 999;

    for (const auto& zone : sampleSet->zones)
    {
        if (zone == nullptr)
            continue;

        const auto& m = zone->metadata;
        if (m.mapSetSlot != activeSlot)
            continue;

        const int dist = std::abs (m.rootNote - clampedNote);
        if (dist < nearestDistance)
        {
            nearestDistance = dist;
            nearestRoot = m.rootNote;
        }
    }

    if (nearestRoot < 0)
        return false;

    for (const auto& zone : sampleSet->zones)
    {
        if (zone == nullptr)
            continue;

        const auto& m = zone->metadata;
        if (m.mapSetSlot != activeSlot)
            continue;

        if (m.rootNote != nearestRoot)
            continue;

        rrNoteMatches.insert (m.roundRobinIndex);
        if (clampedVelocity >= m.lowVelocity && clampedVelocity <= m.highVelocity)
            rrMatches.insert (m.roundRobinIndex);
    }

    if (rrMatches.size() > 1)
        return true;

    return rrMatches.empty() && rrNoteMatches.size() > 1;
}

SamplePlayerAudioProcessor::BlockSettings SamplePlayerAudioProcessor::getBlockSettingsSnapshot() const
{
    BlockSettings settings;

    settings.outputGainLinear = juce::Decibels::decibelsToGain (parameters.getRawParameterValue ("outputGainDb")->load());

    settings.attackMs = juce::jmax (0.0f, parameters.getRawParameterValue ("attackMs")->load());
    settings.decayMs = juce::jmax (0.0f, parameters.getRawParameterValue ("decayMs")->load());
    settings.sustainLevel = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("sustain")->load());
    settings.releaseMs = juce::jmax (0.0f, parameters.getRawParameterValue ("releaseMs")->load());

    settings.loopEnabled = parameters.getRawParameterValue ("loopEnabled")->load() >= 0.5f;
    if (! activeMapLoopPlaybackEnabled.load (std::memory_order_relaxed))
        settings.loopEnabled = false;
    settings.loopStartPercent = parameters.getRawParameterValue ("loopStartPct")->load();
    settings.loopEndPercent = parameters.getRawParameterValue ("loopEndPct")->load();
    settings.loopCrossfadeMs = parameters.getRawParameterValue ("loopCrossfadeMs")->load();

    if (settings.loopEndPercent <= settings.loopStartPercent + 0.1f)
        settings.loopEndPercent = juce::jmin (100.0f, settings.loopStartPercent + 0.1f);

    return settings;
}

SamplePlayerAudioProcessor::LoopSettings SamplePlayerAudioProcessor::buildLoopSettingsForZone (const SampleZone& zone,
                                                                                                 const BlockSettings& settings) const
{
    LoopSettings loop;

    if (! settings.loopEnabled)
        return loop;

    const int totalSamples = zone.audio.getNumSamples();

    if (totalSamples < 4)
        return loop;

    const int maxIndex = totalSamples - 1;

    const auto startSample = static_cast<int> (std::round ((settings.loopStartPercent * 0.01f) * static_cast<float> (maxIndex)));
    auto endSample = static_cast<int> (std::round ((settings.loopEndPercent * 0.01f) * static_cast<float> (maxIndex)));

    loop.startSample = juce::jlimit (0, juce::jmax (0, maxIndex - 2), startSample);
    endSample = juce::jlimit (loop.startSample + 1, maxIndex, endSample);

    if (endSample <= loop.startSample + 1)
        endSample = juce::jmin (maxIndex, loop.startSample + 2);

    loop.endSample = endSample;

    const auto maxCrossfade = juce::jmax (0, loop.loopLength() - 1);
    const auto crossfadeAtSourceRate = static_cast<int> (std::round (settings.loopCrossfadeMs * 0.001f
                                                                      * static_cast<float> (zone.sourceSampleRate)));

    loop.crossfadeSamples = juce::jlimit (0, maxCrossfade, crossfadeAtSourceRate);
    loop.enabled = loop.loopLength() > 1;

    return loop;
}

void SamplePlayerAudioProcessor::renderVoices (juce::AudioBuffer<float>& outputBuffer,
                                                int startSample,
                                                int numSamples,
                                                const BlockSettings& settings)
{
    if (numSamples <= 0)
        return;

    enforceSingleVoicePerMidiNote();

    for (auto& voice : voices)
    {
        if (! voice.active || voice.zone == nullptr)
            continue;

        renderSingleVoice (voice, outputBuffer, startSample, numSamples, settings);
    }
}

void SamplePlayerAudioProcessor::renderSingleVoice (VoiceState& voice,
                                                     juce::AudioBuffer<float>& outputBuffer,
                                                     int startSample,
                                                     int numSamples,
                                                     const BlockSettings& settings)
{
    if (! voice.active || voice.zone == nullptr)
        return;

    const auto& zone = *voice.zone;

    if (zone.audio.getNumSamples() < 2)
    {
        voice.active = false;
        return;
    }

    const auto loop = buildLoopSettingsForZone (zone, settings);
    const int zoneLength = zone.audio.getNumSamples();
    float mapSetGainLinear = 1.0f;
    float velocityLayerGain = 1.0f;
    double velocityLayerDelaySourceSamples = 0.0;
    if (const auto sampleSet = std::atomic_load (&currentSampleSet); sampleSet != nullptr)
    {
        if (const auto gainIt = sampleSet->gainLinearBySlot.find (zone.metadata.mapSetSlot);
            gainIt != sampleSet->gainLinearBySlot.end())
        {
            mapSetGainLinear = juce::jmax (0.0f, gainIt->second);
        }

        velocityLayerGain = getRealtimeVelocityLayerGain (voice, *sampleSet);
        velocityLayerDelaySourceSamples = getRealtimeVelocityLayerDelaySourceSamples (voice, *sampleSet);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        if (! voice.active)
            break;

        // Haas delay: output silence while counting down
        if (voice.delaySamplesRemaining > 0)
        {
            --voice.delaySamplesRemaining;
            continue;
        }

        if (! loop.enabled && voice.position >= static_cast<double> (zoneLength - 1))
        {
            voice.active = false;
            break;
        }

        if (loop.enabled)
            wrapLoopPosition (voice.position, loop);

        if (voice.releaseSamplesRemaining > 0)
        {
            voice.envelopeGain -= voice.releaseDelta;
            --voice.releaseSamplesRemaining;

            if (voice.releaseSamplesRemaining <= 0 || voice.envelopeGain <= 0.0f)
            {
                voice.active = false;
                break;
            }
        }
        else
        {
            if (voice.attackSamplesRemaining > 0)
            {
                voice.envelopeGain += voice.attackDelta;
                --voice.attackSamplesRemaining;

                if (voice.attackSamplesRemaining <= 0)
                    voice.envelopeGain = 1.0f;
            }
            else if (voice.decaySamplesRemaining > 0)
            {
                voice.envelopeGain -= voice.decayDelta;
                --voice.decaySamplesRemaining;

                if (voice.decaySamplesRemaining <= 0 || voice.envelopeGain <= voice.sustainLevel)
                    voice.envelopeGain = voice.sustainLevel;
            }
            else
            {
                voice.envelopeGain = voice.sustainLevel;
            }
        }

        const auto envelope = juce::jmax (0.0f, voice.envelopeGain);
        const auto amp = settings.outputGainLinear * mapSetGainLinear * velocityLayerGain * voice.velocityGain * envelope;

        if (amp > 0.0f)
        {
            const double renderPosition = voice.position - velocityLayerDelaySourceSamples;

            if (renderPosition >= 0.0)
            {
                for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
                {
                    const int sourceChannel = juce::jmin (channel, zone.audio.getNumChannels() - 1);
                    float sampleValue = readSampleLinear (zone, sourceChannel, renderPosition);

                    if (loop.enabled && loop.crossfadeSamples > 0)
                    {
                        const double crossfadeStart = static_cast<double> (loop.endSample - loop.crossfadeSamples);

                        if (renderPosition >= crossfadeStart)
                        {
                            const double crossfadePosition = renderPosition - crossfadeStart;
                            const double crossfadeT = crossfadePosition / static_cast<double> (loop.crossfadeSamples);
                            const double wrappedPosition = static_cast<double> (loop.startSample) + crossfadePosition;

                            const auto tailSample = sampleValue;
                            const auto headSample = readSampleLinear (zone, sourceChannel, wrappedPosition);
                            const auto t = juce::jlimit (0.0, 1.0, crossfadeT);
                            const auto tailGain = static_cast<float> (std::cos (t * juce::MathConstants<double>::halfPi));
                            const auto headGain = static_cast<float> (std::sin (t * juce::MathConstants<double>::halfPi));
                            sampleValue = (tailSample * tailGain) + (headSample * headGain);
                        }
                    }

                    float panGain = 1.0f;
                    if (channel == 0)
                        panGain = voice.panGains[0];
                    else if (channel == 1)
                        panGain = voice.panGains[1];
                    outputBuffer.addSample (channel, startSample + i, sampleValue * amp * panGain);
                }
            }
        }

        voice.position += voice.pitchRatio;

        if (loop.enabled)
        {
            wrapLoopPosition (voice.position, loop);
        }
        else if (voice.position >= static_cast<double> (zoneLength - 1))
        {
            voice.active = false;
        }
    }
}

float SamplePlayerAudioProcessor::getRealtimeVelocityLayerGain (const VoiceState& voice,
                                                                const SampleSet& sampleSet) const
{
    if (voice.zone == nullptr)
        return 1.0f;

    if (! modwheelVelocityLayerControlEnabled.load (std::memory_order_relaxed))
        return 1.0f;

    const auto slotIt = sampleSet.velocityLayersBySlotRoot.find (voice.zone->metadata.mapSetSlot);
    if (slotIt == sampleSet.velocityLayersBySlotRoot.end())
        return 1.0f;

    const auto rootIt = slotIt->second.find (voice.zone->metadata.rootNote);
    if (rootIt == slotIt->second.end() || rootIt->second.size() <= 1)
        return 1.0f;

    const auto modwheel01 = juce::jlimit (0.0f, 1.0f,
                                          modwheelVelocityLayerControlValue01.load (std::memory_order_relaxed));
    const int selectionVelocity = juce::jlimit (1, 127,
                                                1 + static_cast<int> (std::round (modwheel01 * 126.0f)));
    const double jitterSigned = computeVelocityLayerBlendJitter (voice.age,
                                                                 voice.zone->metadata.velocityLayer,
                                                                 voice.zone->metadata.roundRobinIndex);
    return computeVelocityCrossfadeBlendState (rootIt->second,
                                               voice.zone->metadata.velocityLayer,
                                               selectionVelocity,
                                               70,
                                               jitterSigned).gain;
}

double SamplePlayerAudioProcessor::getRealtimeVelocityLayerDelaySourceSamples (const VoiceState& voice,
                                                                               const SampleSet& sampleSet) const
{
    if (voice.zone == nullptr)
        return 0.0;

    if (! modwheelVelocityLayerControlEnabled.load (std::memory_order_relaxed))
        return 0.0;

    const auto slotIt = sampleSet.velocityLayersBySlotRoot.find (voice.zone->metadata.mapSetSlot);
    if (slotIt == sampleSet.velocityLayersBySlotRoot.end())
        return 0.0;

    const auto rootIt = slotIt->second.find (voice.zone->metadata.rootNote);
    if (rootIt == slotIt->second.end() || rootIt->second.size() <= 1)
        return 0.0;

    const auto modwheel01 = juce::jlimit (0.0f, 1.0f,
                                          modwheelVelocityLayerControlValue01.load (std::memory_order_relaxed));
    const int selectionVelocity = juce::jlimit (1, 127,
                                                1 + static_cast<int> (std::round (modwheel01 * 126.0f)));
    const double jitterSigned = computeVelocityLayerBlendJitter (voice.age,
                                                                 voice.zone->metadata.velocityLayer,
                                                                 voice.zone->metadata.roundRobinIndex);
    const auto blendState = computeVelocityCrossfadeBlendState (rootIt->second,
                                                                voice.zone->metadata.velocityLayer,
                                                                selectionVelocity,
                                                                70,
                                                                jitterSigned);

    if (currentSampleRate <= 0.0)
        return 0.0;

    return blendState.delaySec * currentSampleRate;
}

float SamplePlayerAudioProcessor::readSampleLinear (const SampleZone& zone, int channel, double samplePosition)
{
    const int totalSamples = zone.audio.getNumSamples();

    if (totalSamples < 1)
        return 0.0f;

    const int lastIndex = totalSamples - 1;
    const auto clampedPosition = juce::jlimit (0.0, static_cast<double> (lastIndex), samplePosition);

    const int indexA = static_cast<int> (clampedPosition);
    const int indexB = juce::jmin (indexA + 1, lastIndex);

    const float fraction = static_cast<float> (clampedPosition - static_cast<double> (indexA));
    const auto* samples = zone.audio.getReadPointer (channel);

    return samples[indexA] + (samples[indexB] - samples[indexA]) * fraction;
}

void SamplePlayerAudioProcessor::wrapLoopPosition (double& position, const LoopSettings& loop)
{
    if (! loop.enabled || loop.loopLength() <= 1)
        return;

    const auto end = static_cast<double> (loop.endSample);

    if (position < end)
        return;

    const auto start = static_cast<double> (loop.startSample);
    const auto length = static_cast<double> (loop.loopLength());

    while (position >= end)
        position -= length;

    if (position < start)
        position = start;
}

bool SamplePlayerAudioProcessor::tryStartRelease (VoiceState& voice, const BlockSettings& settings)
{
    if (! voice.active)
        return false;

    if (voice.releaseSamplesRemaining > 0)
        return true;

    const auto releaseMs = juce::jmax (0.0f, settings.releaseMs);

    if (releaseMs <= 0.001f)
    {
        voice.active = false;
        return false;
    }

    voice.attackSamplesRemaining = 0;
    voice.decaySamplesRemaining = 0;

    voice.releaseSamplesRemaining = juce::jmax (1, msToSamples (currentSampleRate, releaseMs));
    voice.releaseDelta = juce::jmax (0.000001f,
                                     juce::jmax (0.0f, voice.envelopeGain)
                                         / static_cast<float> (voice.releaseSamplesRemaining));

    return true;
}

juce::ValueTree SamplePlayerAudioProcessor::buildZoneOverridesState() const
{
    juce::ValueTree overridesTree (kZoneOverridesNode);

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr)
        return overridesTree;

    for (const auto& zone : sampleSet->zones)
    {
        juce::ValueTree zoneNode (kZoneNode);
        zoneNode.setProperty ("path", zone->sourceFile.getFullPathName(), nullptr);
        zoneNode.setProperty ("root", zone->metadata.rootNote, nullptr);
        zoneNode.setProperty ("lowNote", zone->metadata.lowNote, nullptr);
        zoneNode.setProperty ("highNote", zone->metadata.highNote, nullptr);
        zoneNode.setProperty ("lowVel", zone->metadata.lowVelocity, nullptr);
        zoneNode.setProperty ("highVel", zone->metadata.highVelocity, nullptr);
        zoneNode.setProperty ("rr", zone->metadata.roundRobinIndex, nullptr);
        overridesTree.addChild (zoneNode, -1, nullptr);
    }

    return overridesTree;
}

void SamplePlayerAudioProcessor::applyZoneOverridesState (const juce::ValueTree& overridesTree)
{
    if (! overridesTree.isValid())
        return;

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr || sampleSet->zones.empty())
        return;

    std::unordered_map<std::string, ZoneMetadata> overridesByPath;

    for (int i = 0; i < overridesTree.getNumChildren(); ++i)
    {
        const auto zoneNode = overridesTree.getChild (i);

        if (! zoneNode.hasType (kZoneNode))
            continue;

        const auto path = zoneNode.getProperty ("path").toString();

        if (path.isEmpty())
            continue;

        ZoneMetadata metadata;
        metadata.rootNote = static_cast<int> (zoneNode.getProperty ("root", metadata.rootNote));
        metadata.lowNote = static_cast<int> (zoneNode.getProperty ("lowNote", metadata.lowNote));
        metadata.highNote = static_cast<int> (zoneNode.getProperty ("highNote", metadata.highNote));
        metadata.lowVelocity = static_cast<int> (zoneNode.getProperty ("lowVel", metadata.lowVelocity));
        metadata.highVelocity = static_cast<int> (zoneNode.getProperty ("highVel", metadata.highVelocity));
        metadata.roundRobinIndex = static_cast<int> (zoneNode.getProperty ("rr", metadata.roundRobinIndex));

        overridesByPath[path.toStdString()] = sanitizeZoneMetadata (metadata);
    }

    if (overridesByPath.empty())
        return;

    bool changed = false;

    auto updatedSet = std::make_shared<SampleSet>();
    updatedSet->sourcePaths = sampleSet->sourcePaths;
    updatedSet->zones.reserve (sampleSet->zones.size());

    for (const auto& zone : sampleSet->zones)
    {
        const auto it = overridesByPath.find (zone->sourceFile.getFullPathName().toStdString());

        if (it != overridesByPath.end() && ! zoneMetadataEquals (zone->metadata, it->second))
        {
            auto updatedZone = std::make_shared<SampleZone> (*zone);
            updatedZone->metadata = it->second;
            updatedSet->zones.push_back (updatedZone);
            changed = true;
        }
        else
        {
            updatedSet->zones.push_back (zone);
        }
    }

    if (! changed)
        return;

    updatedSet->summary = buildSampleSummary (updatedSet->zones);
    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (updatedSet));
    resetVoicesRequested.store (true);
}

void SamplePlayerAudioProcessor::restoreSampleFilesFromState (const juce::StringArray& pathList)
{
    juce::Array<juce::File> files;

    for (const auto& path : pathList)
    {
        const auto file = juce::File (path.trim());

        if (file.existsAsFile() && isSupportedSampleFile (file))
            files.add (file);
    }

    if (files.isEmpty())
    {
        clearSampleSet();
        finishPresetLoadTrace ("restoreSampleFilesFromState", "no-valid-files");
        return;
    }

    juce::String error;
    if (! loadSampleFiles (files, error))
    {
        clearSampleSet();
        finishPresetLoadTrace ("restoreSampleFilesFromState", "load-failed");
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SamplePlayerAudioProcessor();
}
