#pragma once

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

class DreamAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int spectrumBins = 256;
    static constexpr int oscilloscopeSamples = 256;
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;

    DreamAudioProcessor();
    ~DreamAudioProcessor() override;

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

    std::array<float, spectrumBins> getSpectrumSnapshot() const;
    std::array<float, spectrumBins> getReferenceSpectrumSnapshot() const;
    std::array<float, oscilloscopeSamples> getOscilloscopeSnapshot() const;
    void setOscilloscopeLengthMode (int mode) noexcept;
    int getOscilloscopeLengthMode() const noexcept;
    double getCurrentAnalysisSampleRate() const noexcept;
    float getRmsDb() const noexcept;
    float getLufsIntegrated() const noexcept;
    bool buildSmoothPresetFromFolder (const juce::File& folder, juce::String& outMessage);
    bool hasReferenceSpectrumData() const noexcept;
    std::uint32_t getReferenceSpectrumRevision() const noexcept;
    void clearReferenceSpectrum() noexcept;

private:
    juce::AudioProcessorValueTreeState parameters;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { fftSize, juce::dsp::WindowingFunction<float>::hann, true };
    std::array<float, fftSize> fifo {};
    std::array<float, fftSize * 2> fftData {};
    std::array<float, spectrumBins> smoothedSpectrum {};
    std::array<std::atomic<float>, spectrumBins> spectrumData {};
    std::array<std::atomic<float>, spectrumBins> referenceSpectrumData {};
    std::array<std::atomic<float>, oscilloscopeSamples> oscilloscopeData {};
    std::array<float, spectrumBins> spectrumBinPosition {};
    std::atomic<bool> hasReferenceSpectrum { false };
    std::atomic<std::uint32_t> referenceSpectrumRevision { 0 };
    int oscilloscopeLastBin = -1;
    double oscilloscopeQuarterPositionSamples = 0.0;
    int oscilloscopeLastLengthMode = 0;
    int fifoIndex = 0;
    float fftMagnitudeToDbScale = 1.0f;
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<float> currentTempoBpm { 120.0f };
    std::atomic<int> oscilloscopeLengthMode { 0 };
    std::atomic<float> rmsDb { -96.0f };
    std::atomic<float> lufsIntegrated { -96.0f };
    float rmsSmoothedDb = -96.0f;

    juce::dsp::IIR::Filter<float> lufsHighPass;
    juce::dsp::IIR::Filter<float> lufsHighShelf;
    double lufsWeightedEnergySum = 0.0;
    double lufsWeightedSampleCount = 0.0;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void pushAnalyserSample (float sample) noexcept;
    void buildSpectrumFrame() noexcept;
    void updateSpectrumLayout (double sampleRate) noexcept;
    static std::array<float, spectrumBins> buildSpectrumBinPositions (double sampleRate) noexcept;
    static float computeFftMagnitudeScale();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DreamAudioProcessor)
};
