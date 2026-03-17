#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr float velocityScale = 1.0f / 127.0f;
constexpr float autoSamplerInterTakePauseMs = 1000.0f;
constexpr auto kSampleFilePathsProperty = "sampleFilePaths";
constexpr auto kWallpaperPathProperty = "wallpaperPath";
constexpr auto kUiSessionStateProperty = "uiSessionStateJson";
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

    const auto logFile = getLoadDebugLogFile();
    const auto parent = logFile.getParentDirectory();

    if (! parent.isDirectory())
    {
        const auto createResult = parent.createDirectory();
        if (createResult.failed())
            return;
    }

    if (logFile.existsAsFile() && logFile.getSize() > kLoadDebugMaxFileBytes)
    {
        juce::String trimmed;
        trimmed << "=== Sample Player load debug log rotated: "
                << juce::Time::getCurrentTime().toString (true, true, true, true)
                << " ===\n";
        logFile.replaceWithText (trimmed, false, false, "\n");
    }

    juce::String line;
    line << juce::Time::getCurrentTime().toString (true, true, true, true)
         << " | "
         << message
         << "\n";

    logFile.appendText (line, false, false, "\n");
}

double elapsedMsFrom (double startMs)
{
    return juce::Time::getMillisecondCounterHiRes() - startMs;
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
    initialSet->summary = "No samples loaded.\n\n" + getZoneNamingHint();
    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (initialSet));
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
        350.0f,
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

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "filterEnabled", 1 },
        "Filter Enable",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "filterCutoff", 1 },
        "Filter Cutoff",
        juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f),
        18000.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            if (value >= 1000.0f)
                return juce::String (value / 1000.0f, 2) + " kHz";

            return juce::String (value, 0) + " Hz";
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "filterResonance", 1 },
        "Filter Resonance",
        juce::NormalisableRange<float> (0.0f, 0.99f, 0.001f),
        0.1f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 2);
        }));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "filterEnvAmount", 1 },
        "Filter Env",
        juce::NormalisableRange<float> (-4.0f, 4.0f, 0.01f),
        0.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 2) + " oct";
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

    juce::MidiBuffer generatedMidi;
    appendAutoSamplerMidiOutput (generatedMidi, buffer.getNumSamples());

    auto inputBuffer = getBusBuffer (buffer, true, 0);
    // Capture only the internally scheduled autosampler notes so host/user MIDI
    // on this track can't shift RR counters or remap capture roots.
    processAutoSamplerCapture (inputBuffer, generatedMidi);

    juce::MidiBuffer outgoingMidi = incomingMidi;
    outgoingMidi.addEvents (generatedMidi, 0, buffer.getNumSamples(), 0);

    bool keepAutoSamplerAwake = false;
    {
        const juce::ScopedLock lock (autoSamplerLock);
        keepAutoSamplerAwake = autoSamplerActive;
    }

    juce::AudioBuffer<float> monitorBuffer;
    if (keepAutoSamplerAwake
        && inputBuffer.getNumChannels() > 0
        && inputBuffer.getNumSamples() > 0)
    {
        const int monitorChannels = juce::jmax (1, juce::jmin (2, inputBuffer.getNumChannels()));
        monitorBuffer.setSize (monitorChannels, inputBuffer.getNumSamples(), false, false, true);
        for (int ch = 0; ch < monitorChannels; ++ch)
            monitorBuffer.copyFrom (ch, 0, inputBuffer, ch, 0, inputBuffer.getNumSamples());
    }

    auto outputBuffer = getBusBuffer (buffer, false, 0);
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

    const auto settings = getBlockSettingsSnapshot();

    int renderStart = 0;

    for (const auto metadata : incomingMidi)
    {
        const auto eventSample = juce::jlimit (0, buffer.getNumSamples(), metadata.samplePosition);

        if (eventSample > renderStart)
            renderVoices (outputBuffer, renderStart, eventSample - renderStart, settings);

        handleMidiMessage (metadata.getMessage(), settings);
        renderStart = eventSample;
    }

    if (renderStart < outputBuffer.getNumSamples())
        renderVoices (outputBuffer, renderStart, outputBuffer.getNumSamples() - renderStart, settings);

    if (keepAutoSamplerAwake && outputBuffer.getNumChannels() > 0 && outputBuffer.getNumSamples() > 0)
    {
        // Keep the host process callback alive while autosampler MIDI is being generated.
        constexpr float keepAliveLevel = 1.0e-9f;
        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
            outputBuffer.addSample (ch, 0, keepAliveLevel);
    }

    midiMessages.swapWith (outgoingMidi);
}

bool SamplePlayerAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* SamplePlayerAudioProcessor::createEditor()
{
    return new SamplePlayerAudioProcessorEditor (*this);
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

    const auto currentWallpaper = getWallpaperFile();

    if (currentWallpaper.existsAsFile())
        state.setProperty (kWallpaperPathProperty, currentWallpaper.getFullPathName(), nullptr);
    else
        state.removeProperty (kWallpaperPathProperty, nullptr);

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
    writeLoadDebugLog ("setStateInformation parsed | format="
                       + juce::String (restoredFromBinary ? "binary-v1" : "xml")
                       + " | parseMs=" + juce::String (restoredFromBinary ? binaryParsedMs : xmlParsedMs, 2)
                       + " | embeddedSession=" + juce::String (hasEmbeddedUiSession ? "yes" : "no")
                       + " | sessionJsonBytes=" + juce::String (restoredUiSessionJson.getNumBytesAsUTF8()));

    // Fast path: if embedded session JSON exists, it already contains mapping + sample payloads.
    // Skip legacy synchronous file-path restore on plugin load.
    if (! hasEmbeddedUiSession)
    {
        juce::StringArray samplePathLines;
        samplePathLines.addLines (restoredState.getProperty (kSampleFilePathsProperty).toString());
        restoreSampleFilesFromState (samplePathLines);

        const auto zoneOverrides = restoredState.getChildWithName (kZoneOverridesNode);
        if (zoneOverrides.isValid())
            applyZoneOverridesState (zoneOverrides);
    }

    const auto legacyRestoreMs = elapsedMsFrom (loadStartMs);

    const auto wallpaperPath = restoredState.getProperty (kWallpaperPathProperty).toString();

    if (wallpaperPath.isNotEmpty())
    {
        if (! setWallpaperFile (juce::File (wallpaperPath)))
            setWallpaperFile (juce::File {});
    }
    else
    {
        setWallpaperFile (juce::File {});
    }

    const auto wallpaperStageMs = elapsedMsFrom (loadStartMs);
    setUiSessionStateJson (restoredUiSessionJson);
    writeLoadDebugLog ("setStateInformation completed | legacyRestoreMs=" + juce::String (legacyRestoreMs, 2)
                       + " | wallpaperStageMs=" + juce::String (wallpaperStageMs, 2)
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

    for (const auto& file : uniqueFiles)
    {
        auto reader = std::unique_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (file));

        if (reader == nullptr || reader->lengthInSamples < 2)
            continue;

        const auto zone = std::make_shared<SampleZone>();
        zone->sourceFile = file;
        zone->sourceSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        zone->metadata = parseZoneMetadataFromFileName (file.getFileNameWithoutExtension());

        const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
        const auto totalSamples64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                             static_cast<juce::int64> (std::numeric_limits<int>::max()));
        const int totalSamples = static_cast<int> (totalSamples64);

        if (totalSamples < 2)
            continue;

        zone->audio.setSize (channels, totalSamples);
        reader->read (&zone->audio, 0, totalSamples, 0, true, true);

        newSampleSet->zones.push_back (zone);
        newSampleSet->sourcePaths.add (file.getFullPathName());
    }

    std::sort (newSampleSet->zones.begin(), newSampleSet->zones.end(), [] (const auto& a, const auto& b)
    {
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
    emptySet->summary = "No samples loaded.\n\n" + getZoneNamingHint();

    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (emptySet));
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

bool SamplePlayerAudioProcessor::setWallpaperFile (const juce::File& file)
{
    if (file == juce::File {})
    {
        const juce::ScopedLock lock (wallpaperLock);
        wallpaperFile = juce::File {};
        return true;
    }

    if (! file.existsAsFile())
        return false;

    const auto image = juce::ImageFileFormat::loadFrom (file);
    if (image.isNull())
        return false;

    const juce::ScopedLock lock (wallpaperLock);
    wallpaperFile = file;
    return true;
}

juce::File SamplePlayerAudioProcessor::getWallpaperFile() const
{
    const juce::ScopedLock lock (wallpaperLock);
    return wallpaperFile;
}

void SamplePlayerAudioProcessor::setUiSessionStateJson (const juce::String& json)
{
    const auto requestStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto jsonBytes = json.getNumBytesAsUTF8();

    {
        const juce::ScopedLock lock (uiSessionStateLock);
        if (uiSessionStateJson == json)
        {
            writeLoadDebugLog ("setUiSessionStateJson no-op | bytes=" + juce::String (jsonBytes));
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int zoneCount = sampleSet != nullptr ? static_cast<int> (sampleSet->zones.size()) : 0;
            if (zoneCount > 0)
                markPresetLoadPlayable ("setUiSessionStateJson-no-op", zoneCount);
            else
                finishPresetLoadTrace ("setUiSessionStateJson-no-op", "no-zones");
            return;
        }
        uiSessionStateJson = json;
        uiSessionStateLightweightJson.clear();
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

        const auto lightweightStartMs = juce::Time::getMillisecondCounterHiRes();
        LightweightStripStats stripStats;
        auto lightweightJson = makeLightweightSessionStateJson (latestJson, &stripStats);
        if (lightweightJson.isEmpty())
            lightweightJson = latestJson;

        {
            const juce::ScopedLock lock (uiSessionStateLock);
            if (requestId == sessionStateSyncRequestId.load (std::memory_order_relaxed))
                uiSessionStateLightweightJson = lightweightJson;
        }

        writeLoadDebugLog ("session lightweight cache ready | requestId=" + juce::String (requestId)
                           + " | bytesOut=" + juce::String (lightweightJson.getNumBytesAsUTF8())
                           + " | sampleDataRemoved=" + juce::String (stripStats.sampleDataUrlsRemoved)
                           + " | dataUrlsRemoved=" + juce::String (stripStats.dataUrlsRemoved)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (lightweightStartMs), 2));

        writeLoadDebugLog ("session sync job start | requestId=" + juce::String (requestId)
                           + " | bytes=" + juce::String (latestJson.getNumBytesAsUTF8()));
        syncSampleSetFromSessionStateJson (latestJson, requestId);
        writeLoadDebugLog ("session sync job end | requestId=" + juce::String (requestId)
                           + " | elapsedMs=" + juce::String (elapsedMsFrom (syncJobStartMs), 2));
    });
}

juce::String SamplePlayerAudioProcessor::getUiSessionStateJson (bool lightweightPreferred) const
{
    const juce::ScopedLock lock (uiSessionStateLock);
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
    const auto targetFileName = juce::File (fileName).getFileName().trim();

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
        return {};

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

    if (bestZone == nullptr && ! rootZones.empty())
        bestZone = rootZones.front();

    if (bestZone == nullptr || bestZone->audio.getNumSamples() <= 0)
        return {};

    return encodeAudioBufferToWavDataUrl (bestZone->audio, bestZone->sourceSampleRate);
}

void SamplePlayerAudioProcessor::syncSampleSetFromSessionStateJson (const juce::String& jsonPayload, int requestId)
{
    const auto syncStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto payloadBytes = jsonPayload.getNumBytesAsUTF8();

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

    if (jsonPayload.trim().isEmpty())
    {
        finishPresetLoadTrace ("syncSampleSetFromSessionStateJson", "empty-json");
        logExit ("empty-json");
        return;
    }

    const auto parseStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto parsed = juce::JSON::parse (jsonPayload);
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

    const auto parseMs = elapsedMsFrom (parseStartMs);

    int activeKeyswitchIndex = -1;
    bool allowPitchUpAboveHighest = false;
    bool useModwheelForVelocityLayers = false;
    float modwheelValue01 = juce::jlimit (0.0f, 1.0f,
                                          modwheelVelocityLayerControlValue01.load (std::memory_order_relaxed));
    juce::var manualRangesVar;

    if (const auto* uiObject = rootObject->getProperty ("ui").getDynamicObject())
    {
        allowPitchUpAboveHighest = static_cast<bool> (uiObject->getProperty ("allowPitchUpAboveHighest"));
        modwheelValue01 = juce::jlimit (0.0f, 1.0f,
                                        static_cast<float> (varToDouble (uiObject->getProperty ("modWheelValue"),
                                                                         static_cast<double> (modwheelValue01))));

        if (const auto* autoObject = uiObject->getProperty ("auto").getDynamicObject())
            useModwheelForVelocityLayers = static_cast<bool> (autoObject->getProperty ("modwheelVelocityControl"));

        manualRangesVar = uiObject->getProperty ("manualRootRanges");

        const auto activeMapSetId = uiObject->getProperty ("activeMapSetId").toString().trim();
        if (const auto* uiKeyswitchSets = uiObject->getProperty ("keyswitchSets").getArray())
        {
            for (int i = 0; i < uiKeyswitchSets->size(); ++i)
            {
                const auto* setObject = (*uiKeyswitchSets)[i].getDynamicObject();
                if (setObject == nullptr)
                    continue;

                bool isActiveSet = static_cast<bool> (setObject->getProperty ("active"));
                if (! isActiveSet && activeMapSetId.isNotEmpty())
                    isActiveSet = setObject->getProperty ("id").toString() == activeMapSetId;

                if (! isActiveSet)
                    continue;

                activeKeyswitchIndex = i;
                manualRangesVar = setObject->getProperty ("manualRanges");
                break;
            }
        }
    }

    modwheelVelocityLayerControlEnabled.store (useModwheelForVelocityLayers, std::memory_order_relaxed);
    modwheelVelocityLayerControlValue01.store (modwheelValue01, std::memory_order_relaxed);

    juce::var activeMappingVar = manifestObject->getProperty ("mapping");
    if (activeKeyswitchIndex >= 0)
    {
        if (const auto* manifestKeyswitchSets = manifestObject->getProperty ("keyswitchSets").getArray())
        {
            if (juce::isPositiveAndBelow (activeKeyswitchIndex, manifestKeyswitchSets->size()))
            {
                if (const auto* selectedKeyswitch = (*manifestKeyswitchSets)[activeKeyswitchIndex].getDynamicObject())
                    activeMappingVar = selectedKeyswitch->getProperty ("mapping");
            }
        }
    }

    const auto* mappingArray = activeMappingVar.getArray();
    if (mappingArray == nullptr || mappingArray->isEmpty())
    {
        if (isStaleRequest())
        {
            logExit ("stale-empty-map");
            return;
        }

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

    struct EmbeddedVariantDescriptor
    {
        int rootMidi = 60;
        int velocityLayer = 1;
        int rrIndex = 1;
        juce::String fileName;
        juce::String sampleDataUrl;
    };

    const auto mapBuildStartMs = juce::Time::getMillisecondCounterHiRes();
    std::vector<EmbeddedVariantDescriptor> variants;
    variants.reserve (static_cast<size_t> (mappingArray->size()) * 2);

    std::vector<int> roots;
    roots.reserve (static_cast<size_t> (mappingArray->size()));
    std::unordered_map<int, std::vector<int>> layersByRoot;

    juce::uint64 hash = 1469598103934665603ULL;
    hashMix (hash, static_cast<juce::uint64> (allowPitchUpAboveHighest ? 1 : 0));
    hashMix (hash, static_cast<juce::uint64> (juce::jmax (0, activeKeyswitchIndex + 1)));

    for (const auto& bucketVar : *mappingArray)
    {
        const auto* bucketObject = bucketVar.getDynamicObject();
        if (bucketObject == nullptr)
            continue;

        const int rootMidi = juce::jlimit (0, 127, varToInt (bucketObject->getProperty ("rootMidiNote"), 60));
        const int velocityLayer = juce::jlimit (1, 5, varToInt (bucketObject->getProperty ("velocityLayer"), 1));

        roots.push_back (rootMidi);
        auto& rootLayers = layersByRoot[rootMidi];
        if (std::find (rootLayers.begin(), rootLayers.end(), velocityLayer) == rootLayers.end())
            rootLayers.push_back (velocityLayer);

        hashMix (hash, static_cast<juce::uint64> (rootMidi + 1));
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
            if (sampleDataUrl.isEmpty())
            {
                ++fallbackRr;
                continue;
            }

            auto fileName = variantObject->getProperty ("originalFilename").toString().trim();
            if (fileName.isEmpty())
                fileName = juce::File (variantObject->getProperty ("path").toString()).getFileName();
            if (fileName.isEmpty())
                fileName = "Embedded_" + midiToNoteToken (rootMidi) + "_V" + juce::String (velocityLayer) + "_RR" + juce::String (fallbackRr) + ".wav";

            const int rrIndex = juce::jmax (1, varToInt (variantObject->getProperty ("rrIndex"), fallbackRr));

            variants.push_back ({ rootMidi, velocityLayer, rrIndex, fileName, sampleDataUrl });

            hashMix (hash, static_cast<juce::uint64> (rrIndex + 101));
            hashMix (hash, static_cast<juce::uint64> (sampleDataUrl.hashCode64()));
            hashMix (hash, static_cast<juce::uint64> (fileName.hashCode64()));
            ++fallbackRr;
        }
    }

    if (variants.empty())
    {
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

    std::sort (roots.begin(), roots.end());
    roots.erase (std::unique (roots.begin(), roots.end()), roots.end());

    std::unordered_map<int, std::pair<int, int>> noteRangesByRoot;
    noteRangesByRoot.reserve (roots.size());

    const auto* manualRangesObject = manualRangesVar.getDynamicObject();

    for (size_t i = 0; i < roots.size(); ++i)
    {
        const int root = roots[i];
        int low = (i == 0) ? root : juce::jmin (127, roots[i - 1] + 1);
        int high = root;

        if (i == roots.size() - 1 && allowPitchUpAboveHighest)
            high = 127;

        if (manualRangesObject != nullptr)
        {
            const auto* rootRangeObject = manualRangesObject->getProperty (juce::String (root)).getDynamicObject();
            if (rootRangeObject != nullptr)
            {
                low = juce::jlimit (0, 127, varToInt (rootRangeObject->getProperty ("low"), low));
                high = juce::jlimit (0, 127, varToInt (rootRangeObject->getProperty ("high"), high));
            }
        }

        if (low > high)
            std::swap (low, high);

        low = juce::jmin (low, root);
        high = juce::jmax (high, root);
        low = juce::jlimit (0, 127, low);
        high = juce::jlimit (low, 127, high);

        noteRangesByRoot[root] = { low, high };
        hashMix (hash, static_cast<juce::uint64> (root + 409));
        hashMix (hash, static_cast<juce::uint64> ((low << 8) | high));
    }

    std::unordered_map<int, std::unordered_map<int, std::pair<int, int>>> velocityBoundsByRootLayer;
    velocityBoundsByRootLayer.reserve (layersByRoot.size());

    for (auto& [root, layers] : layersByRoot)
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

            velocityBoundsByRootLayer[root][layers[static_cast<size_t> (i)]] = { lowVelocity, highVelocity };
            hashMix (hash, static_cast<juce::uint64> ((root << 16) ^ (layers[static_cast<size_t> (i)] << 8) ^ highVelocity));
        }
    }

    const auto signature = hashToSignature (hash, static_cast<int> (variants.size()));
    {
        const juce::ScopedLock lock (sessionMapSyncLock);
        if (lastSessionMapSignature == signature)
        {
            const auto sampleSet = std::atomic_load (&currentSampleSet);
            const int zoneCount = sampleSet != nullptr ? static_cast<int> (sampleSet->zones.size()) : 0;
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
    newSampleSet->zones.reserve (variants.size());

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

    for (const auto& descriptor : variants)
    {
        if (isStaleRequest())
        {
            logExit ("stale-mid-decode");
            return;
        }

        const auto cacheKey = static_cast<juce::uint64> (descriptor.sampleDataUrl.hashCode64());
        auto cachedAudio = findDecodedEmbeddedAudioInCache (cacheKey);

        if (cachedAudio == nullptr)
        {
            ++cacheMisses;
            juce::MemoryBlock audioData;
            if (! decodeDataUrlAudioToMemory (descriptor.sampleDataUrl, audioData) || audioData.getSize() == 0)
            {
                ++decodeFailures;
                continue;
            }

            auto input = std::make_unique<juce::MemoryInputStream> (audioData.getData(), audioData.getSize(), false);
            auto reader = std::unique_ptr<juce::AudioFormatReader> (asyncFormatManager.createReaderFor (std::move (input)));

            if (reader == nullptr || reader->lengthInSamples < 2)
            {
                ++decodeFailures;
                continue;
            }

            const int channels = static_cast<int> (juce::jlimit<juce::uint32> (1U, 2U, reader->numChannels));
            const auto totalSamples64 = juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                 static_cast<juce::int64> (std::numeric_limits<int>::max()));
            const int totalSamples = static_cast<int> (totalSamples64);

            if (totalSamples < 2)
            {
                ++decodeFailures;
                continue;
            }

            auto decodedEntry = std::make_shared<DecodedEmbeddedAudioCacheEntry>();
            decodedEntry->sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
            decodedEntry->audio.setSize (channels, totalSamples);
            reader->read (&decodedEntry->audio, 0, totalSamples, 0, true, true);
            decodedEntry->bytes = static_cast<std::size_t> (channels)
                                * static_cast<std::size_t> (totalSamples)
                                * sizeof (float);

            storeDecodedEmbeddedAudioInCache (cacheKey, decodedEntry);
            cachedAudio = decodedEntry;
            ++decodedOnMiss;
            decodedBytesOnMiss += decodedEntry->bytes;
        }
        else
        {
            ++cacheHits;
        }

        if (cachedAudio == nullptr || cachedAudio->audio.getNumSamples() < 2 || cachedAudio->audio.getNumChannels() <= 0)
            continue;

        const auto zone = std::make_shared<SampleZone>();

        auto safeFileName = juce::File (descriptor.fileName).getFileName().trim();
        if (safeFileName.isEmpty())
            safeFileName = "Embedded_" + midiToNoteToken (descriptor.rootMidi) + ".wav";

        zone->sourceFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile (safeFileName);
        zone->sourceSampleRate = cachedAudio->sampleRate > 0.0 ? cachedAudio->sampleRate : 44100.0;
        zone->audio.makeCopyOf (cachedAudio->audio);

        ZoneMetadata metadata;
        metadata.rootNote = descriptor.rootMidi;

        if (const auto rangeIt = noteRangesByRoot.find (descriptor.rootMidi); rangeIt != noteRangesByRoot.end())
        {
            metadata.lowNote = rangeIt->second.first;
            metadata.highNote = rangeIt->second.second;
        }
        else
        {
            metadata.lowNote = descriptor.rootMidi;
            metadata.highNote = descriptor.rootMidi;
        }

        if (const auto rootIt = velocityBoundsByRootLayer.find (descriptor.rootMidi); rootIt != velocityBoundsByRootLayer.end())
        {
            if (const auto layerIt = rootIt->second.find (descriptor.velocityLayer); layerIt != rootIt->second.end())
            {
                metadata.lowVelocity = layerIt->second.first;
                metadata.highVelocity = layerIt->second.second;
            }
        }

        metadata.roundRobinIndex = descriptor.rrIndex;
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
                           + " | decodeFailures=" + juce::String (decodeFailures));
        return;
    }

    std::sort (newSampleSet->zones.begin(), newSampleSet->zones.end(), [] (const auto& a, const auto& b)
    {
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
    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (newSampleSet));
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
                       + " | decoded=" + juce::String (decodedOnMiss)
                       + " | decodedBytes=" + juce::String (static_cast<juce::int64> (decodedBytesOnMiss))
                       + " | decodeFailures=" + juce::String (decodeFailures)
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
            triggered.fileName = "AUTO_" + midiToNoteToken (triggered.rootMidi)
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
    next.loopStartPercent = juce::jlimit (0.0f, 100.0f, sanitizeFloat (next.loopStartPercent, 10.0f));
    next.loopEndPercent = juce::jlimit (0.0f, 100.0f, sanitizeFloat (next.loopEndPercent, 90.0f));
    if (next.loopEndPercent <= next.loopStartPercent + 0.1f)
        next.loopEndPercent = juce::jmin (100.0f, next.loopStartPercent + 0.1f);
    if (next.cutLoopAtEnd && next.loopEndPercent < 5.0f)
        next.loopEndPercent = 90.0f;
    next.loopCrossfadeMs = juce::jlimit (0.0f, 60000.0f, sanitizeFloat (next.loopCrossfadeMs, 200.0f));

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
        autoSamplerStatusMessage = "No input audio detected. Capturing silence; route source audio to Sample Player input.";

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
                completed.fileName = "AUTO_" + midiToNoteToken (capture.rootMidi)
                                   + "_V" + juce::String (capture.velocityLayer)
                                   + "_RR" + juce::String (capture.rrIndex) + ".wav";
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
        autoSamplerStatusMessage = "Sampling finished.";
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

    metadata.roundRobinIndex = juce::jmax (1, metadata.roundRobinIndex);

    return metadata;
}

bool SamplePlayerAudioProcessor::zoneMetadataEquals (const ZoneMetadata& a, const ZoneMetadata& b)
{
    return a.rootNote == b.rootNote
        && a.lowNote == b.lowNote
        && a.highNote == b.highNote
        && a.lowVelocity == b.lowVelocity
        && a.highVelocity == b.highVelocity
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

void SamplePlayerAudioProcessor::handleMidiMessage (const juce::MidiMessage& message, const BlockSettings& settings)
{
    if (message.isNoteOn())
    {
        startVoiceForNote (message.getChannel(), message.getNoteNumber(), message.getFloatVelocity(), settings);
        return;
    }

    if (message.isNoteOff())
    {
        releaseVoicesForNote (message.getChannel(), message.getNoteNumber(), true, settings);
        return;
    }

    if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        stopAllVoices();
        return;
    }

    if (message.isController() && message.getControllerNumber() == 123)
    {
        stopAllVoices();
        return;
    }

    if (message.isController() && message.getControllerNumber() == 1)
    {
        const auto cc = juce::jlimit (0, 127, message.getControllerValue());
        modwheelVelocityLayerControlValue01.store (static_cast<float> (cc) / 127.0f,
                                                   std::memory_order_relaxed);
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
    auto zone = pickZoneForNote (midiNoteNumber, velocity127, &usedModwheelLayerSelection);

    if (zone == nullptr)
        return;

    auto* voice = findFreeVoice();
    if (voice == nullptr)
        voice = stealOldestVoice();

    if (voice == nullptr)
        return;

    *voice = VoiceState {};

    voice->active = true;
    voice->midiNote = midiNoteNumber;
    voice->midiChannel = midiChannel;
    voice->zone = std::move (zone);
    voice->position = 0.0;
    const bool ignoreMidiVelocity = usedModwheelLayerSelection && settings.loopEnabled;
    voice->velocityGain = ignoreMidiVelocity ? 1.0f : (velocity127 * velocityScale);
    voice->age = ++voiceAgeCounter;

    const auto semitoneOffset = static_cast<double> (midiNoteNumber - voice->zone->metadata.rootNote);
    const auto pitch = std::pow (2.0, semitoneOffset / 12.0);
    const auto sampleRateRatio = voice->zone->sourceSampleRate / juce::jmax (1.0, currentSampleRate);

    voice->pitchRatio = juce::jmax (0.0001, sampleRateRatio * pitch);

    voice->sustainLevel = juce::jlimit (0.0f, 1.0f, settings.sustainLevel);

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

    for (auto& filterState : voice->filterStates)
        filterState.reset();
}

void SamplePlayerAudioProcessor::releaseVoicesForNote (int midiChannel,
                                                        int midiNoteNumber,
                                                        bool allowTailOff,
                                                        const BlockSettings& settings)
{
    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        if (voice.midiChannel != midiChannel || voice.midiNote != midiNoteNumber)
            continue;

        if (! allowTailOff)
        {
            voice.active = false;
            continue;
        }

        tryStartRelease (voice, settings);
    }
}

void SamplePlayerAudioProcessor::stopAllVoices()
{
    for (auto& voice : voices)
        voice = VoiceState {};
}

SamplePlayerAudioProcessor::VoiceState* SamplePlayerAudioProcessor::findFreeVoice()
{
    for (auto& voice : voices)
        if (! voice.active)
            return &voice;

    return nullptr;
}

SamplePlayerAudioProcessor::VoiceState* SamplePlayerAudioProcessor::stealOldestVoice()
{
    VoiceState* oldest = nullptr;

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        if (oldest == nullptr || voice.age < oldest->age)
            oldest = &voice;
    }

    return oldest;
}

std::shared_ptr<const SamplePlayerAudioProcessor::SampleZone> SamplePlayerAudioProcessor::pickZoneForNote (int midiNoteNumber,
                                                                                                              int velocity127,
                                                                                                              bool* usedModwheelLayerSelection)
{
    if (usedModwheelLayerSelection != nullptr)
        *usedModwheelLayerSelection = false;

    const auto sampleSet = std::atomic_load (&currentSampleSet);

    if (sampleSet == nullptr || sampleSet->zones.empty())
        return {};

    std::vector<std::shared_ptr<const SampleZone>> noteAndVelocityMatches;
    std::vector<std::shared_ptr<const SampleZone>> noteOnlyMatches;

    for (const auto& zone : sampleSet->zones)
    {
        const auto& m = zone->metadata;

        if (midiNoteNumber < m.lowNote || midiNoteNumber > m.highNote)
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

    if (candidatePool->empty())
    {
        const auto nearestIt = std::min_element (sampleSet->zones.begin(), sampleSet->zones.end(), [midiNoteNumber] (const auto& a, const auto& b)
        {
            return std::abs (a->metadata.rootNote - midiNoteNumber) < std::abs (b->metadata.rootNote - midiNoteNumber);
        });

        if (nearestIt == sampleSet->zones.end())
            return {};

        return *nearestIt;
    }

    std::sort (candidatePool->begin(), candidatePool->end(), [] (const auto& a, const auto& b)
    {
        if (a->metadata.roundRobinIndex != b->metadata.roundRobinIndex)
            return a->metadata.roundRobinIndex < b->metadata.roundRobinIndex;

        if (a->metadata.lowVelocity != b->metadata.lowVelocity)
            return a->metadata.lowVelocity < b->metadata.lowVelocity;

        return a->sourceFile.getFileName() < b->sourceFile.getFileName();
    });

    auto& rrCounter = roundRobinCounters[midiNoteNumber];
    const auto poolSize = static_cast<int> (candidatePool->size());
    const int wrappedIndex = poolSize > 0 ? (rrCounter % poolSize) : 0;
    rrCounter = (rrCounter + 1) % 8192;

    return candidatePool->at (static_cast<size_t> (wrappedIndex));
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
    settings.loopStartPercent = parameters.getRawParameterValue ("loopStartPct")->load();
    settings.loopEndPercent = parameters.getRawParameterValue ("loopEndPct")->load();
    settings.loopCrossfadeMs = parameters.getRawParameterValue ("loopCrossfadeMs")->load();

    if (settings.loopEndPercent <= settings.loopStartPercent + 0.1f)
        settings.loopEndPercent = juce::jmin (100.0f, settings.loopStartPercent + 0.1f);

    settings.filterEnabled = parameters.getRawParameterValue ("filterEnabled")->load() >= 0.5f;
    settings.filterCutoffHz = juce::jlimit (20.0f, 20000.0f, parameters.getRawParameterValue ("filterCutoff")->load());
    settings.filterResonance = juce::jlimit (0.0f, 0.99f, parameters.getRawParameterValue ("filterResonance")->load());
    settings.filterEnvelopeAmountOctaves = juce::jlimit (-4.0f, 4.0f, parameters.getRawParameterValue ("filterEnvAmount")->load());

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

    for (int i = 0; i < numSamples; ++i)
    {
        if (! voice.active)
            break;

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
        const auto amp = settings.outputGainLinear * voice.velocityGain * envelope;

        if (amp > 0.0f)
        {
            for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
            {
                const int sourceChannel = juce::jmin (channel, zone.audio.getNumChannels() - 1);
                float sampleValue = readSampleLinear (zone, sourceChannel, voice.position);

                if (loop.enabled && loop.crossfadeSamples > 0)
                {
                    const double crossfadeStart = static_cast<double> (loop.endSample - loop.crossfadeSamples);

                    if (voice.position >= crossfadeStart)
                    {
                        const double crossfadePosition = voice.position - crossfadeStart;
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

                sampleValue = processVoiceFilterSample (voice, channel, sampleValue, settings);
                outputBuffer.addSample (channel, startSample + i, sampleValue * amp);
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

float SamplePlayerAudioProcessor::processVoiceFilterSample (VoiceState& voice,
                                                             int channel,
                                                             float inputSample,
                                                             const BlockSettings& settings) const
{
    if (! settings.filterEnabled)
        return inputSample;

    if (channel < 0 || channel >= static_cast<int> (voice.filterStates.size()))
        return inputSample;

    auto& filter = voice.filterStates[static_cast<size_t> (channel)];

    const auto envelope = juce::jlimit (0.0f, 1.0f, voice.envelopeGain);
    const auto cutoffWithEnvelope = settings.filterCutoffHz
                                  * std::pow (2.0f, settings.filterEnvelopeAmountOctaves * envelope);

    const auto maxCutoff = juce::jmax (40.0f, static_cast<float> (currentSampleRate * 0.49));
    const auto cutoff = juce::jlimit (20.0f, maxCutoff, cutoffWithEnvelope);

    const auto resonance = juce::jlimit (0.0f, 0.99f, settings.filterResonance);
    const auto damping = juce::jlimit (0.05f, 1.0f, 1.0f - resonance * 0.95f);

    auto f = 2.0f * std::sin (juce::MathConstants<float>::pi * cutoff / static_cast<float> (currentSampleRate));
    f = juce::jlimit (0.001f, 1.9f, f);

    filter.low += f * filter.band;
    const auto high = inputSample - filter.low - damping * filter.band;
    filter.band += f * high;

    return filter.low;
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
