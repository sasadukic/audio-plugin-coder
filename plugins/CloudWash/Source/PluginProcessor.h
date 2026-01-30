#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <mutex>
#include <atomic>

#include "clouds/dsp/granular_processor.h"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/sample_rate_converter.h"
#include "clouds/resources.h"

//==============================================================================
/**
 * CloudWash - Granular Texture Processor
 *
 * Authentic port of Mutable Instruments Clouds DSP.
 */
class CloudWashAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    CloudWashAudioProcessor();
    ~CloudWashAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Parameter Value Tree State (APVTS)
    juce::AudioProcessorValueTreeState apvts;

    //==============================================================================
    // AUDIO METERING & VISUALIZATION DATA
    //==============================================================================
    std::atomic<float> inputPeakLevel { 0.0f };
    std::atomic<float> outputPeakLevel { 0.0f };
    float inputPeakHold { 0.0f };    // Peak hold with decay
    float outputPeakHold { 0.0f };   // Peak hold with decay

    std::atomic<int> activeGrainCount { 0 };
    std::atomic<float> grainDensityViz { 0.0f };
    std::atomic<float> grainTextureViz { 0.0f };

    // Mode and Quality mapping helper
    static int getNumQualityModes() { return 5; }
    static juce::String getQualityModeName(int index);

private:
    //==============================================================================
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // CLOUDS DSP
    //==============================================================================

    // Memory blocks for the processor (use heap allocation like VCV Rack)
    uint8_t* block_mem = nullptr;
    uint8_t* block_ccm = nullptr;

    // Use pointer and heap allocation (matches VCV Rack pattern)
    clouds::GranularProcessor* processor = nullptr;
    
    // Resampling state (Host SR -> 32kHz -> Host SR)
    juce::AudioBuffer<float> resampledInputBuffer;
    juce::AudioBuffer<float> resampledOutputBuffer;
    
    // VCV Rack-style SampleRateConverter for host rate <-> 32kHz conversion
    // Uses dsp::SampleRateConverter from VCV Rack's DSP library
    // This handles arbitrary sample rate ratios properly (44.1k->32k, 48k->32k, etc.)
    struct VCVStyleSRC {
        static constexpr int kFilterSize = 45;
        float history_[kFilterSize * 2][2];  // Double-buffered history for 2 channels
        int history_ptr_;
        float coefficients_[kFilterSize];
        
        void Init(const float* coeffs = clouds::src_filter_1x_2_45) {
            for (int i = 0; i < kFilterSize * 2; ++i) {
                history_[i][0] = 0.0f;
                history_[i][1] = 0.0f;
            }
            for (int i = 0; i < kFilterSize; ++i) {
                coefficients_[i] = coeffs[i];
            }
            history_ptr_ = kFilterSize - 1;
        }
        
        // Process with ratio (positive = upsampling, negative = downsampling)
        // ratio = -2 means 2:1 downsampling (2 in -> 1 out)
        // ratio = +2 means 1:2 upsampling (1 in -> 2 out)
        void Process(const float* inL, const float* inR, float* outL, float* outR, 
                     size_t input_size, int ratio) {
            int history_ptr = history_ptr_;
            const float scale = ratio < 0 ? 1.0f : static_cast<float>(ratio);
            size_t out_idx = 0;
            
            while (input_size > 0) {
                int consumed = ratio < 0 ? -ratio : 1;
                for (int i = 0; i < consumed && input_size > 0; ++i) {
                    // Write to both buffers (double-buffered for wrap-around)
                    history_[history_ptr + kFilterSize][0] = history_[history_ptr][0] = *inL++;
                    history_[history_ptr + kFilterSize][1] = history_[history_ptr][1] = *inR++;
                    --input_size;
                    --history_ptr;
                    if (history_ptr < 0) {
                        history_ptr += kFilterSize;
                    }
                }
                
                int produced = ratio > 0 ? ratio : 1;
                for (int i = 0; i < produced; ++i) {
                    float y_l = 0.0f;
                    float y_r = 0.0f;
                    int x_idx = history_ptr + 1;
                    if (x_idx >= kFilterSize) x_idx -= kFilterSize;
                    
                    for (int j = i; j < kFilterSize; j += produced) {
                        const float h = coefficients_[j];
                        y_l += history_[x_idx][0] * h;
                        y_r += history_[x_idx][1] * h;
                        x_idx += (produced == 1) ? 1 : 0;  // Only advance if consuming one per output
                        if (x_idx >= kFilterSize) x_idx -= kFilterSize;
                    }
                    outL[out_idx] = y_l * scale;
                    outR[out_idx] = y_r * scale;
                    ++out_idx;
                }
            }
            history_ptr_ = history_ptr;
        }
    };
    
    VCVStyleSRC inputSRC;
    VCVStyleSRC outputSRC;

    // Internal buffers for Clouds (ShortFrame)
    std::vector<clouds::ShortFrame> inputFrames;
    std::vector<clouds::ShortFrame> outputFrames;

    bool isFrozen { false };

    double hostSampleRate = 44100.0;
    double internalSampleRate = 32000.0;

    // High fidelity mixing buffer
    juce::AudioBuffer<float> dryBuffer;

    // Thread safety for DSP re-initialization
    std::mutex processorMutex;

    // Quality/Mode change handling (to prevent audio thread blocking)
    // All atomic for thread safety (parameters can change from message thread)
    std::atomic<int> pendingMode { -1 };
    std::atomic<int> pendingQuality { -1 };
    std::atomic<int> silenceBlocksRemaining { 0 };
    std::atomic<int> currentMode { 0 };
    std::atomic<int> currentQuality { 0 };
    std::atomic<bool> cloudsInitialized { false };  // Track if Clouds processor is initialized

    // Preset management
    struct PresetData {
        juce::String name;
        std::map<juce::String, float> parameters;
    };
    std::vector<PresetData> presets;
    int currentPresetIndex { 0 };
    void initializePresets();
    void loadPreset(int index);

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CloudWashAudioProcessor)
};
