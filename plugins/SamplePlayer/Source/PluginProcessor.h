#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

class SamplePlayerAudioProcessor : public juce::AudioProcessor
{
public:
    struct AutoSamplerSettings
    {
        int startMidi = 36;
        int endMidi = 72;
        int intervalSemitones = 3;
        int velocityLayers = 3;
        int roundRobinsPerNote = 2;
        float sustainMs = 1800.0f;
        float releaseTailMs = 700.0f;
        float prerollMs = 0.0f;
        juce::String destinationFolder;
        juce::String instrumentName;
        bool keyswitchMode = false;
        juce::String keyswitchKey;
        juce::String wallpaperSourcePath;
        juce::String wallpaperDataUrl;
        juce::String wallpaperFileName;
        juce::String logoSourcePath;
        juce::String logoDataUrl;
        juce::String logoFileName;
        bool loopSamples = false;
        bool autoLoopMode = true;
        float loopStartPercent = 10.0f;
        float loopEndPercent = 90.0f;
        bool cutLoopAtEnd = false;
        float loopCrossfadeMs = 200.0f;
        bool normalizeRecorded = false;
    };

    struct AutoSamplerMidiEvent
    {
        juce::int64 samplePosition = 0;
        int midiNote = 60;
        int velocity127 = 100;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
        bool noteOn = true;
    };

    struct AutoSamplerProgress
    {
        bool active = false;
        int expectedTakes = 0;
        int capturedTakes = 0;
        bool inputDetected = false;
        juce::String statusMessage;
    };

    struct AutoSamplerCompletedTake
    {
        int rootMidi = 60;
        int velocity127 = 100;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
        juce::String fileName;
        double sampleRate = 44100.0;
        bool loopSamples = false;
        bool autoLoopMode = true;
        float loopStartPercent = 10.0f;
        float loopEndPercent = 90.0f;
        bool cutLoopAtEnd = false;
        float loopCrossfadeMs = 200.0f;
        bool normalized = false;
        juce::String filePath;
        juce::AudioBuffer<float> audio;
    };

    struct AutoSamplerTriggeredTake
    {
        int rootMidi = 60;
        int velocity127 = 100;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
        juce::String fileName;
        bool loopSamples = false;
        bool autoLoopMode = true;
        float loopStartPercent = 10.0f;
        float loopEndPercent = 90.0f;
        bool cutLoopAtEnd = false;
        float loopCrossfadeMs = 200.0f;
        bool normalized = false;
    };

    struct ZoneMetadata
    {
        int rootNote = 60;
        int lowNote = 0;
        int highNote = 127;
        int lowVelocity = 1;
        int highVelocity = 127;
        int roundRobinIndex = 1;
        int mapSetSlot = 0;
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
    void setUiSessionStateJson (const juce::String& json);
    void setActiveMapSetId (const juce::String& setId);
    juce::String getUiSessionStateJson (bool lightweightPreferred = false);
    juce::String getSampleDataUrlForMapEntry (int rootMidi,
                                              int velocityLayer,
                                              int rrIndex,
                                              const juce::String& fileName) const;
    juce::String getSampleDataUrlForAbsolutePath (const juce::String& absolutePath,
                                                  const juce::String& fileNameHint = {}) const;
    void queuePreviewMidiEvent (bool noteOn, int midiNote, int velocity127, int midiChannel = 1);
    std::pair<juce::uint64, juce::uint64> getHeldMidiMaskForUi() const noexcept;
    int getActiveMapSetSlotForUi() const noexcept;
    int getSequencerCurrentStepForUi() const noexcept;
    void setSequencerHostTriggerEnabled (bool enabled);

    static juce::String getZoneNamingHint();

    bool startAutoSamplerCapture (const AutoSamplerSettings& settings, juce::String& errorMessage);
    void stopAutoSamplerCapture (bool cancelled);
    AutoSamplerProgress getAutoSamplerProgress() const;
    std::vector<AutoSamplerTriggeredTake> popTriggeredAutoSamplerTakes();
    std::vector<AutoSamplerCompletedTake> popCompletedAutoSamplerTakes();

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
        std::unordered_map<std::string, int> mapSetSlotById;
        std::unordered_map<int, bool> loopPlaybackBySlot;
        std::array<int, 128> keyswitchSlotByMidi {};
    };

    struct DecodedEmbeddedAudioCacheEntry
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        std::size_t bytes = 0;
        juce::uint64 age = 0;
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
        bool ignoreMonoNoteDedupe = false;
    };

    struct BlockSettings
    {
        float outputGainLinear = 1.0f;

        float attackMs = 5.0f;
        float decayMs = 250.0f;
        float sustainLevel = 1.0f;
        float releaseMs = 2000.0f;

        bool loopEnabled = true;
        float loopStartPercent = 5.0f;
        float loopEndPercent = 95.0f;
        float loopCrossfadeMs = 15.0f;

        bool filterEnabled = false;
        float filterCutoffHz = 20000.0f;
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
    void beginPresetLoadTrace (const juce::String& source, int bytesHint = -1);
    void markPresetLoadPlayable (const juce::String& stage, int zoneCount);
    void finishPresetLoadTrace (const juce::String& stage, const juce::String& outcome);

    void handleMidiMessage (const juce::MidiMessage& message, const BlockSettings& settings, bool previewMessage = false);
    void startVoiceForNote (int midiChannel, int midiNoteNumber, float velocity, const BlockSettings& settings);
    void releaseVoicesForNote (int midiChannel, int midiNoteNumber, bool allowTailOff, const BlockSettings& settings);
    void enforceSingleVoicePerMidiNote();
    void stopAllVoices();

    VoiceState* findFreeVoice();
    VoiceState* findFreeStealTailVoice();
    VoiceState* stealOldestVoice();
    void startStealTailFromVoice (const VoiceState& sourceVoice);
    void setMidiHeldState (int midiNote, bool held) noexcept;

    std::shared_ptr<const SampleZone> pickZoneForNote (int midiNoteNumber, int velocity127, bool* usedModwheelLayerSelection = nullptr);

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
    void syncSampleSetFromSessionStateJson (const juce::String& jsonPayload, int requestId = -1);
    std::shared_ptr<const DecodedEmbeddedAudioCacheEntry> findDecodedEmbeddedAudioInCache (juce::uint64 key);
    void storeDecodedEmbeddedAudioInCache (juce::uint64 key, std::shared_ptr<DecodedEmbeddedAudioCacheEntry> entry);
    void trimDecodedEmbeddedAudioCache();

    struct PendingPreviewMidiEvent
    {
        bool noteOn = true;
        int midiNote = 60;
        int velocity127 = 100;
        int midiChannel = 1;
    };

    struct StepSequencerRuntime
    {
        struct Step
        {
            int noteMidi = 60;
            int velocity127 = 100;
            int keyswitchSlot = -1;
        };

        bool enabled = false;
        int currentStep = -1;
        std::array<Step, 16> steps {};
        std::array<int, 128> triggerToPlayedNote {};
        std::array<int, 128> triggerDepthByMidi {};
        std::array<int, 128> playedDepthByMidi {};

        StepSequencerRuntime()
        {
            triggerToPlayedNote.fill (-1);
            triggerDepthByMidi.fill (0);
            playedDepthByMidi.fill (0);
        }
    };

    static constexpr int maxPlayableVoices = 32;
    static constexpr int maxStealTailVoices = 4;
    static constexpr int maxVoices = maxPlayableVoices + maxStealTailVoices;
    static constexpr float voiceStealFadeOutMs = 10.0f;
    static constexpr std::size_t maxDecodedEmbeddedAudioCacheBytes = static_cast<std::size_t> (128 * 1024 * 1024);

    juce::AudioFormatManager formatManager;
    double currentSampleRate = 44100.0;

    std::array<VoiceState, maxVoices> voices;
    uint64_t voiceAgeCounter = 0;
    std::array<int, 128> midiNoteOnCounts {};
    std::atomic<juce::uint64> midiHeldMaskLo { 0 };
    std::atomic<juce::uint64> midiHeldMaskHi { 0 };
    std::unordered_map<int, int> roundRobinCounters;
    mutable juce::CriticalSection pendingPreviewMidiLock;
    std::vector<PendingPreviewMidiEvent> pendingPreviewMidiEvents;

    std::shared_ptr<const SampleSet> currentSampleSet;
    std::shared_ptr<StepSequencerRuntime> stepSequencerRuntime;
    std::atomic<int> activeMapSetSlot { 0 };
    std::atomic<int> pendingActiveMapSetSlotFromMidi { -1 };
    std::atomic<int> sequencerCurrentStepForUi { -1 };
    std::atomic<bool> resetVoicesRequested { false };

    mutable juce::CriticalSection wallpaperLock;
    juce::File wallpaperFile;
    mutable juce::CriticalSection uiSessionStateLock;
    juce::String uiSessionStateJson;
    juce::String uiSessionStateLightweightJson;
    juce::String pendingActiveMapSetId;
    mutable juce::CriticalSection sessionMapSyncLock;
    juce::String lastSessionMapSignature;
    std::atomic<int> sessionStateSyncRequestId { 0 };
    std::atomic<bool> modwheelVelocityLayerControlEnabled { false };
    std::atomic<float> modwheelVelocityLayerControlValue01 { 0.0f };
    std::atomic<bool> activeMapLoopPlaybackEnabled { true };
    mutable juce::CriticalSection decodedEmbeddedAudioCacheLock;
    std::unordered_map<juce::uint64, std::shared_ptr<DecodedEmbeddedAudioCacheEntry>> decodedEmbeddedAudioCache;
    std::size_t decodedEmbeddedAudioCacheTotalBytes = 0;
    juce::uint64 decodedEmbeddedAudioCacheAgeCounter = 0;

    struct ActiveAutoCapture
    {
        int rootMidi = 60;
        int velocity127 = 100;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
        int writePosition = 0;
        int totalSamples = 0;
        juce::AudioBuffer<float> audio;
    };

    struct ScheduledAutoCaptureNoteEvent
    {
        int samplePosition = 0;
        int note = 60;
        int velocity127 = 100;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
    };

    struct SavedAutoSamplerTake
    {
        int rootMidi = 60;
        int velocityLayer = 1;
        int velocityLow = 1;
        int velocityHigh = 127;
        int rrIndex = 1;
        bool normalized = false;
        juce::String fileName;
    };

    static std::pair<int, int> velocityBoundsForLayer (int layer, int totalLayers);
    static int velocityForLayer (int layer, int totalLayers);
    static juce::String midiToNoteToken (int midiNote);

    void appendAutoSamplerMidiOutput (juce::MidiBuffer& midiOutput, int blockNumSamples);
    void processAutoSamplerCapture (const juce::AudioBuffer<float>& inputBuffer, const juce::MidiBuffer& midiMessages);
    void writeInputHistorySample (float left, float right);
    void copyFromInputHistory (ActiveAutoCapture& capture, int numSamples);
    bool shouldCaptureMidiNote (int midiNote) const;

    mutable juce::CriticalSection autoSamplerLock;
    AutoSamplerSettings autoSamplerSettings {};
    bool autoSamplerActive = false;
    bool autoSamplerInputDetected = false;
    int autoSamplerExpectedTakes = 0;
    int autoSamplerCapturedTakes = 0;
    juce::String autoSamplerStatusMessage;
    std::vector<ActiveAutoCapture> activeAutoCaptures;
    std::vector<AutoSamplerTriggeredTake> triggeredAutoCaptures;
    std::vector<AutoSamplerCompletedTake> completedAutoCaptures;
    std::vector<ScheduledAutoCaptureNoteEvent> autoSamplerPendingNoteEvents;
    std::unordered_map<int, int> autoSamplerFallbackRrCounters;
    std::array<bool, 128> autoSamplerNoteMask {};
    std::vector<AutoSamplerMidiEvent> autoSamplerMidiSchedule;
    size_t autoSamplerMidiScheduleIndex = 0;
    juce::int64 autoSamplerTimelineSample = 0;
    double autoSamplerStartWallMs = 0.0;
    std::array<bool, 128> autoSamplerHeldNotes {};
    bool autoSamplerSendAllNotesOff = false;
    std::array<std::vector<float>, 2> autoSamplerInputHistory;
    int autoSamplerHistoryWrite = 0;
    int autoSamplerHistoryValid = 0;
    int autoSamplerHistorySize = 0;
    juce::File autoSamplerDestinationRoot;
    juce::File autoSamplerOutputDirectory;
    int autoSamplerFilesWritten = 0;
    int autoSamplerWriteFailures = 0;
    std::vector<SavedAutoSamplerTake> autoSamplerSavedTakes;
    bool autoSamplerManifestWriteFailed = false;
    juce::String autoSamplerManifestPath;
    juce::ThreadPool sessionStateSyncThreadPool { 1 };
    mutable juce::CriticalSection presetLoadTraceLock;
    int presetLoadTraceId = 0;
    double presetLoadTraceStartMs = 0.0;
    bool presetLoadTraceActive = false;
    juce::String presetLoadTraceSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePlayerAudioProcessor)
};
