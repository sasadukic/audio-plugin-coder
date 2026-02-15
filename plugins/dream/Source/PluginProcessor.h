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
    std::array<float, oscilloscopeSamples> getOscilloscopeSnapshotRight() const;
    void setOscilloscopeLengthMode (int mode) noexcept;
    int getOscilloscopeLengthMode() const noexcept;
    void setSoloBand (int bandIndex) noexcept;
    double getCurrentAnalysisSampleRate() const noexcept;
    float getRmsDb() const noexcept;
    float getLufsIntegrated() const noexcept;
    bool buildSmoothPresetFromFolder (const juce::File& folder, juce::String& outMessage, int smoothingAmount);
    bool hasReferenceSpectrumData() const noexcept;
    std::uint32_t getReferenceSpectrumRevision() const noexcept;
    void clearReferenceSpectrum() noexcept;
    void setReferenceSpectrumFromUi (const std::array<float, spectrumBins>& bins, bool hasData) noexcept;
    void setResonanceSuppressorConfig (bool enabled,
                                       float overlayLevelDb,
                                       float overlayWidthDb,
                                       float tiltDb) noexcept;
    std::array<float, 6> getResonanceSuppressorFrequencySnapshot() const noexcept;
    std::array<float, 6> getResonanceSuppressorGainSnapshot() const noexcept;

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
    std::array<std::atomic<float>, oscilloscopeSamples> oscilloscopeDataRight {};
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
    std::atomic<int> soloBand { -1 };
    float rmsSmoothedDb = -96.0f;
    std::array<float, spectrumBins> spectrumBinFrequencyHz {};
    std::atomic<bool> resonanceSuppressorEnabled { false };
    std::atomic<float> resonanceOverlayLevelDb { 0.0f };
    std::atomic<float> resonanceOverlayWidthDb { 12.0f };
    std::atomic<float> resonanceOverlayTiltDb { 5.0f };

    static constexpr int resonanceSuppressorBands = 6;
    struct ResonanceSuppressorBandState
    {
        std::array<juce::dsp::IIR::Filter<float>, 2> filters;
        float currentFrequencyHz = 1000.0f;
        float currentGainDb = 0.0f;
        float currentQ = 5.0f;
    };
    std::array<ResonanceSuppressorBandState, resonanceSuppressorBands> resonanceBands;
    std::array<std::atomic<float>, resonanceSuppressorBands> resonanceBandFrequencyUi {};
    std::array<std::atomic<float>, resonanceSuppressorBands> resonanceBandGainUi {};

    juce::dsp::IIR::Filter<float> lufsHighPass;
    juce::dsp::IIR::Filter<float> lufsHighShelf;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloHighPass200;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloLowPass200;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloHighPass2k;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloLowPass2k;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloHighPass5k;
    std::array<juce::dsp::IIR::Filter<float>, 2> soloLowPass5k;
    double lufsWeightedEnergySum = 0.0;
    double lufsWeightedSampleCount = 0.0;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void pushAnalyserSample (float sample) noexcept;
    void buildSpectrumFrame() noexcept;
    void updateSpectrumLayout (double sampleRate) noexcept;
    void updateSoloBandFilters (double sampleRate) noexcept;
    void resetSoloBandFilters() noexcept;
    void applySoloBandToBuffer (juce::AudioBuffer<float>& buffer) noexcept;
    void resetResonanceSuppressor() noexcept;
    void updateResonanceSuppressorTargets (int numSamples) noexcept;
    void applyResonanceSuppressorToBuffer (juce::AudioBuffer<float>& buffer) noexcept;
    static std::array<float, spectrumBins> buildSpectrumBinPositions (double sampleRate) noexcept;
    static float computeFftMagnitudeScale();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DreamAudioProcessor)
};
