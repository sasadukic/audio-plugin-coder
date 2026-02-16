#pragma once

#include <array>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

class BassicAudioProcessor : public juce::AudioProcessor
{
public:
    BassicAudioProcessor();
    ~BassicAudioProcessor() override;

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

    juce::AudioProcessorValueTreeState parameters;

private:
    class SynthSound : public juce::SynthesiserSound
    {
    public:
        bool appliesToNote (int) override { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    class SynthVoice : public juce::SynthesiserVoice
    {
    public:
        explicit SynthVoice (juce::AudioProcessorValueTreeState& state);

        bool canPlaySound (juce::SynthesiserSound* sound) override;
        void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*, int currentPitchWheelPosition) override;
        void stopNote (float velocity, bool allowTailOff) override;
        void pitchWheelMoved (int newPitchWheelValue) override;
        void controllerMoved (int controllerNumber, int newControllerValue) override;
        void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override;

        void setCurrentPlaybackSampleRate (double newRate) override;
        void setLegatoTransition (bool isLegato) noexcept { legatoTransition = isLegato; }
        void setFilterDriftSeed (float cutoffVarianceInPercent) noexcept { cutoffVariancePercent = cutoffVarianceInPercent; }

    private:
        struct ExpEnvelope
        {
            void setSampleRate (double newSampleRate) noexcept;
            void setParameters (float attackSeconds, float decaySeconds, float sustainLevel, float releaseSeconds) noexcept;
            void noteOn (bool retrigger) noexcept;
            void noteOff() noexcept;
            void reset() noexcept;
            float getNextSample() noexcept;
            bool isActive() const noexcept;

        private:
            enum class State
            {
                idle,
                attack,
                decay,
                sustain,
                release
            };

            static float calcRate (double sr, float seconds) noexcept;
            static bool isNear (float a, float b) noexcept;

            double sampleRate = 44100.0;
            State state = State::idle;
            float value = 0.0f;
            float sustainTarget = 0.6f;
            float attackRate = 1.0f;
            float decayRate = 1.0f;
            float releaseRate = 1.0f;
        };

        struct PublicLadderFilter : public juce::dsp::LadderFilter<float>
        {
            using juce::dsp::LadderFilter<float>::processSample;
        };

        juce::AudioProcessorValueTreeState& apvts;
        PublicLadderFilter ladderFilter;
        juce::dsp::StateVariableTPTFilter<float> bassThinFilter;
        ExpEnvelope ampEnvelope;
        juce::Random random;

        double sampleRate = 44100.0;
        float currentFrequency = 440.0f;
        juce::LinearSmoothedValue<float> smoothedFrequency;
        float phase = 0.0f;
        float subPhase = 0.0f;
        float lfoPhase = 0.0f;
        float lfoSampleAndHold = 0.0f;
        float noteAgeSamples = 0.0f;
        float noteDriftCents = 0.0f;
        float cutoffVariancePercent = 0.0f;
        float cutoffDriftPercent = 0.0f;
        float envTimeVariance = 1.0f;
        float envAttackSecondsCurrent = 0.0015f;
        float envDecaySecondsCurrent = 0.24f;
        float envSustainLevelCurrent = 0.62f;
        float envReleaseSecondsCurrent = 0.20f;
        bool legatoTransition = false;

        std::atomic<float>* sawLevel = nullptr;
        std::atomic<float>* squareLevel = nullptr;
        std::atomic<float>* subLevel = nullptr;
        std::atomic<float>* noiseLevel = nullptr;
        std::atomic<float>* subOscMode = nullptr;
        std::atomic<float>* lfoRate = nullptr;
        std::atomic<float>* lfoWaveform = nullptr;
        std::atomic<float>* lfoDelay = nullptr;
        std::atomic<float>* lfoPitch = nullptr;
        std::atomic<float>* lfoPwm = nullptr;
        std::atomic<float>* tune = nullptr;
        std::atomic<float>* vcoRange = nullptr;
        std::atomic<float>* pulseWidth = nullptr;
        std::atomic<float>* filterCutoff = nullptr;
        std::atomic<float>* filterResonance = nullptr;
        std::atomic<float>* filterEnvAmt = nullptr;
        std::atomic<float>* filterLfoMod = nullptr;
        std::atomic<float>* filterKeyTrack = nullptr;
        std::atomic<float>* envAttack = nullptr;
        std::atomic<float>* envDecay = nullptr;
        std::atomic<float>* envSustain = nullptr;
        std::atomic<float>* envRelease = nullptr;
        std::atomic<float>* vcaMode = nullptr;
        std::atomic<float>* portamento = nullptr;
        std::atomic<float>* portamentoMode = nullptr;
        std::atomic<float>* filterDrive = nullptr;
        std::atomic<float>* masterLevel = nullptr;

        float getParam (const std::atomic<float>* p, float fallback) const noexcept;
        void updateVoiceParameters();
        static float polyBlep (float t, float dt) noexcept;
    };

    juce::Synthesiser synth;
    SynthVoice* monoVoice = nullptr;

    std::array<bool, 128> heldNotes {};
    std::array<float, 128> heldVelocities {};
    std::vector<int> heldOrder;
    int activeExternalNote = -1;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void removeHeldNote (int note);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassicAudioProcessor)
};
