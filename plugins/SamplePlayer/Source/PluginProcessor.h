#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

class SamplePlayerAudioProcessor : public juce::AudioProcessor
{
public:
    struct ZoneMetadata
    {
        int rootNote = 60;
        int lowNote = 0;
        int highNote = 127;
        int lowVelocity = 1;
        int highVelocity = 127;
        int roundRobinIndex = 1;
    };

    struct ZoneEditorInfo
    {
        int index = -1;
        juce::String fileName;
        ZoneMetadata metadata;
    };

    SamplePlayerAudioProcessor();
    ~SamplePlayerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    bool loadSampleFolder (const juce::File& folder, juce::String& errorMessage);
    bool loadSampleFiles (const juce::Array<juce::File>& files, juce::String& errorMessage);
    void clearSampleSet();

    juce::String getSampleSummaryText() const;
    int getLoadedZoneCount() const;

    bool getZoneEditorInfo (int zoneIndex, ZoneEditorInfo& outInfo) const;
    juce::StringArray getZoneDisplayNames() const;
    bool updateZoneMetadata (int zoneIndex, const ZoneMetadata& metadata, juce::String& errorMessage);

    bool setWallpaperFile (const juce::File& file);
    juce::File getWallpaperFile() const;

    static juce::String getZoneNamingHint();

    juce::AudioProcessorValueTreeState parameters;

private:
    struct SampleZone
    {
        juce::File sourceFile;
        juce::AudioBuffer<float> audio;
        double sourceSampleRate = 44100.0;
        ZoneMetadata metadata;
    };

    struct SampleSet
    {
        std::vector<std::shared_ptr<SampleZone>> zones;
        juce::StringArray sourcePaths;
        juce::String summary;
    };

    struct VoiceState
    {
        struct FilterState
        {
            float low = 0.0f;
            float band = 0.0f;

            void reset()
            {
                low = 0.0f;
                band = 0.0f;
            }
        };

        bool active = false;
        int midiNote = -1;
        int midiChannel = 1;

        std::shared_ptr<const SampleZone> zone;

        double position = 0.0;
        double pitchRatio = 1.0;

        float velocityGain = 1.0f;
        float envelopeGain = 0.0f;
        float sustainLevel = 1.0f;

        int attackSamplesRemaining = 0;
        float attackDelta = 0.0f;

        int decaySamplesRemaining = 0;
        float decayDelta = 0.0f;

        int releaseSamplesRemaining = 0;
        float releaseDelta = 0.0f;

        std::array<FilterState, 2> filterStates {};

        uint64_t age = 0;
    };

    struct BlockSettings
    {
        float outputGainLinear = 1.0f;

        float attackMs = 5.0f;
        float decayMs = 250.0f;
        float sustainLevel = 1.0f;
        float releaseMs = 350.0f;

        bool loopEnabled = true;
        float loopStartPercent = 5.0f;
        float loopEndPercent = 95.0f;
        float loopCrossfadeMs = 15.0f;

        bool filterEnabled = false;
        float filterCutoffHz = 18000.0f;
        float filterResonance = 0.1f;
        float filterEnvelopeAmountOctaves = 0.0f;
    };

    struct LoopSettings
    {
        bool enabled = false;
        int startSample = 0;
        int endSample = 0;
        int crossfadeSamples = 0;

        int loopLength() const { return endSample - startSample; }
    };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static bool isSupportedSampleFile (const juce::File& file);
    static ZoneMetadata parseZoneMetadataFromFileName (const juce::String& fileNameWithoutExtension);
    static ZoneMetadata sanitizeZoneMetadata (ZoneMetadata metadata);
    static bool zoneMetadataEquals (const ZoneMetadata& a, const ZoneMetadata& b);
    static bool parseNoteToken (const juce::String& token, int& midiNoteOut);
    static bool parseIntRange (const juce::String& text, int& low, int& high);

    static juce::String buildSampleSummary (const std::vector<std::shared_ptr<SampleZone>>& zones);

    void handleMidiMessage (const juce::MidiMessage& message, const BlockSettings& settings);
    void startVoiceForNote (int midiChannel, int midiNoteNumber, float velocity, const BlockSettings& settings);
    void releaseVoicesForNote (int midiChannel, int midiNoteNumber, bool allowTailOff, const BlockSettings& settings);
    void stopAllVoices();

    VoiceState* findFreeVoice();
    VoiceState* stealOldestVoice();

    std::shared_ptr<const SampleZone> pickZoneForNote (int midiNoteNumber, int velocity127);

    BlockSettings getBlockSettingsSnapshot() const;
    LoopSettings buildLoopSettingsForZone (const SampleZone& zone, const BlockSettings& settings) const;

    void renderVoices (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples, const BlockSettings& settings);
    void renderSingleVoice (VoiceState& voice,
                            juce::AudioBuffer<float>& outputBuffer,
                            int startSample,
                            int numSamples,
                            const BlockSettings& settings);

    float processVoiceFilterSample (VoiceState& voice,
                                    int channel,
                                    float inputSample,
                                    const BlockSettings& settings) const;

    static float readSampleLinear (const SampleZone& zone, int channel, double samplePosition);
    static void wrapLoopPosition (double& position, const LoopSettings& loop);

    bool tryStartRelease (VoiceState& voice, const BlockSettings& settings);

    juce::ValueTree buildZoneOverridesState() const;
    void applyZoneOverridesState (const juce::ValueTree& overridesTree);
    void restoreSampleFilesFromState (const juce::StringArray& pathList);

    static constexpr int maxVoices = 32;

    juce::AudioFormatManager formatManager;
    double currentSampleRate = 44100.0;

    std::array<VoiceState, maxVoices> voices;
    uint64_t voiceAgeCounter = 0;
    std::unordered_map<int, int> roundRobinCounters;

    std::shared_ptr<const SampleSet> currentSampleSet;
    std::atomic<bool> resetVoicesRequested { false };

    mutable juce::CriticalSection wallpaperLock;
    juce::File wallpaperFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePlayerAudioProcessor)
};
