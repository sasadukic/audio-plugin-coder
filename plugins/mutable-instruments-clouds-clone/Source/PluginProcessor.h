#pragma once

#include <array>
#include <atomic>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>

// Original Mutable Instruments Clouds DSP headers (MIT, Emilie Gillet).
#include "clouds/dsp/granular_processor.h"
#include "clouds/dsp/frame.h"

class MutableInstrumentsCloudsCloneAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int scopeSize = 128;
    MutableInstrumentsCloudsCloneAudioProcessor();
    ~MutableInstrumentsCloudsCloneAudioProcessor() override;

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
    std::atomic<float> inputMeter { 0.0f };
    std::atomic<float> grainMeter { 0.0f };
    std::array<float, scopeSize> getIncomingScopeSnapshot() const;

private:
    std::array<std::atomic<float>, scopeSize> incomingScope {};
    std::atomic<int> scopeWritePos { 0 };

    uint8_t* blockMem = nullptr;
    uint8_t* blockCcm = nullptr;
    clouds::GranularProcessor* cloudsProcessor = nullptr;
    bool cloudsInitialised = false;

    double hostSampleRate = 44100.0;
    static constexpr double internalSampleRate = 32000.0;
    juce::AudioBuffer<float> resampledInputBuffer;
    juce::AudioBuffer<float> resampledOutputBuffer;
    std::vector<clouds::ShortFrame> inputFrames;
    std::vector<clouds::ShortFrame> outputFrames;

    double downsampleInputPhase = 0.0;
    double upsampleOutputPhase = 0.0;
    float prevInputSampleL = 0.0f;
    float prevInputSampleR = 0.0f;
    float prevOutputSampleL = 0.0f;
    float prevOutputSampleR = 0.0f;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void initialiseClouds();
    void destroyClouds();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MutableInstrumentsCloudsCloneAudioProcessor)
};
