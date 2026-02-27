#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

namespace
{
constexpr float velocityScale = 1.0f / 127.0f;
constexpr auto kSampleFilePathsProperty = "sampleFilePaths";
constexpr auto kWallpaperPathProperty = "wallpaperPath";
constexpr auto kZoneOverridesNode = "ZONE_OVERRIDES";
constexpr auto kZoneNode = "ZONE";

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

void SamplePlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
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

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SamplePlayerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr)
        return;

    if (! xmlState->hasTagName (parameters.state.getType()))
        return;

    auto restoredState = juce::ValueTree::fromXml (*xmlState);
    parameters.replaceState (restoredState);

    juce::StringArray samplePathLines;
    samplePathLines.addLines (restoredState.getProperty (kSampleFilePathsProperty).toString());
    restoreSampleFilesFromState (samplePathLines);

    const auto zoneOverrides = restoredState.getChildWithName (kZoneOverridesNode);
    if (zoneOverrides.isValid())
        applyZoneOverridesState (zoneOverrides);

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
        return false;
    }

    newSampleSet->summary = buildSampleSummary (newSampleSet->zones);

    std::atomic_store (&currentSampleSet, std::shared_ptr<const SampleSet> (newSampleSet));
    resetVoicesRequested.store (true);

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

int SamplePlayerAudioProcessor::velocityToLayer (int velocity127, int totalLayers)
{
    const int safeLayers = juce::jlimit (1, 5, totalLayers);
    const int v0 = juce::jlimit (0, 127, velocity127 - 1);
    return 1 + ((v0 * safeLayers) / 128);
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
    AutoSamplerSettings next = settings;
    next.startMidi = juce::jlimit (0, 127, next.startMidi);
    next.endMidi = juce::jlimit (0, 127, next.endMidi);
    next.intervalSemitones = juce::jlimit (1, 12, next.intervalSemitones);
    next.velocityLayers = juce::jlimit (1, 5, next.velocityLayers);
    next.roundRobinsPerNote = juce::jlimit (1, 8, next.roundRobinsPerNote);
    next.sustainMs = juce::jlimit (1.0f, 60000.0f, next.sustainMs);
    next.releaseTailMs = juce::jlimit (0.0f, 60000.0f, next.releaseTailMs);
    next.prerollMs = juce::jlimit (0.0f, 60000.0f, next.prerollMs);
    next.loopStartPercent = juce::jlimit (0.0f, 100.0f, next.loopStartPercent);
    next.loopEndPercent = juce::jlimit (0.0f, 100.0f, next.loopEndPercent);
    if (next.loopEndPercent <= next.loopStartPercent + 0.1f)
        next.loopEndPercent = juce::jmin (100.0f, next.loopStartPercent + 0.1f);
    next.loopCrossfadeMs = juce::jlimit (0.0f, 60000.0f, next.loopCrossfadeMs);

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

                timelineSample += takeSamples;
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
    autoSamplerMidiSchedule = std::move (midiSchedule);
    autoSamplerMidiScheduleIndex = 0;
    autoSamplerTimelineSample = 0;
    autoSamplerStartWallMs = juce::Time::getMillisecondCounterHiRes();
    autoSamplerHeldNotes.fill (false);
    autoSamplerSendAllNotesOff = false;
    autoSamplerRrCounters.clear();
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

    struct NoteEvent
    {
        int samplePosition = 0;
        int note = 60;
        int velocity127 = 100;
    };

    std::vector<NoteEvent> noteEvents;
    noteEvents.reserve (16);

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        if (! message.isNoteOn())
            continue;

        NoteEvent event;
        event.samplePosition = juce::jlimit (0, numSamples - 1, metadata.samplePosition);
        event.note = message.getNoteNumber();
        event.velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (message.getVelocity() * 127.0f)));
        noteEvents.push_back (event);
    }

    const juce::ScopedLock lock (autoSamplerLock);

    if (! autoSamplerActive)
        return;

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

            const int layer = velocityToLayer (event.velocity127, autoSamplerSettings.velocityLayers);
            const int rrKey = (juce::jlimit (0, 127, event.note) << 8) | juce::jlimit (1, 5, layer);
            const int rrIndex = ++autoSamplerRrCounters[rrKey];

            if (rrIndex > autoSamplerSettings.roundRobinsPerNote)
                continue;

            ActiveAutoCapture capture;
            capture.rootMidi = juce::jlimit (0, 127, event.note);
            capture.velocity127 = event.velocity127;
            capture.velocityLayer = layer;
            const auto [velocityLow, velocityHigh] = velocityBoundsForLayer (layer, autoSamplerSettings.velocityLayers);
            capture.velocityLow = velocityLow;
            capture.velocityHigh = velocityHigh;
            capture.rrIndex = rrIndex;
            capture.totalSamples = takeSamples;
            capture.audio.setSize (2, takeSamples);
            capture.audio.clear();
            capture.writePosition = preRollSamples;
            copyFromInputHistory (capture, preRollSamples);

            activeAutoCaptures.push_back (std::move (capture));
            autoSamplerStatusMessage = "Capturing " + midiToNoteToken (event.note)
                                     + " V" + juce::String (layer)
                                     + " RR" + juce::String (rrIndex) + "...";
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
}

void SamplePlayerAudioProcessor::startVoiceForNote (int midiChannel,
                                                     int midiNoteNumber,
                                                     float velocity,
                                                     const BlockSettings& settings)
{
    const int velocity127 = juce::jlimit (1, 127, static_cast<int> (std::round (velocity * 127.0f)));
    auto zone = pickZoneForNote (midiNoteNumber, velocity127);

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
    voice->velocityGain = velocity127 * velocityScale;
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
                                                                                                              int velocity127)
{
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

        if (velocity127 >= m.lowVelocity && velocity127 <= m.highVelocity)
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
                        sampleValue = juce::jmap (static_cast<float> (juce::jlimit (0.0, 1.0, crossfadeT)),
                                                  tailSample,
                                                  headSample);
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
        return;
    }

    juce::String error;
    if (! loadSampleFiles (files, error))
        clearSampleSet();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SamplePlayerAudioProcessor();
}
