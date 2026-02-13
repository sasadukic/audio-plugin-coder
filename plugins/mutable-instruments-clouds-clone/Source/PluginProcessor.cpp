#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

MutableInstrumentsCloudsCloneAudioProcessor::MutableInstrumentsCloudsCloneAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, juce::Identifier ("MutableInstrumentsCloudsClone"), createParameterLayout())
{
}

MutableInstrumentsCloudsCloneAudioProcessor::~MutableInstrumentsCloudsCloneAudioProcessor()
{
    destroyClouds();
}

juce::AudioProcessorValueTreeState::ParameterLayout MutableInstrumentsCloudsCloneAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "position", 1 }, "Position",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "size", 1 }, "Size",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float> (-48.0f, 48.0f, 1.0f), 0.0f, " st"));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "density", 1 }, "Density",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "texture", 1 }, "Texture",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "blend", 1 }, "Blend",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.65f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "spread", 1 }, "Spread",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "feedback", 1 }, "Feedback",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.2f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "reverb", 1 }, "Reverb",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "freeze", 1 }, "Freeze",
        juce::NormalisableRange<float> (0.0f, 1.0f, 1.0f), 0.0f));

    return layout;
}

const juce::String MutableInstrumentsCloudsCloneAudioProcessor::getName() const { return JucePlugin_Name; }
bool MutableInstrumentsCloudsCloneAudioProcessor::acceptsMidi() const { return false; }
bool MutableInstrumentsCloudsCloneAudioProcessor::producesMidi() const { return false; }
bool MutableInstrumentsCloudsCloneAudioProcessor::isMidiEffect() const { return false; }
double MutableInstrumentsCloudsCloneAudioProcessor::getTailLengthSeconds() const { return 12.0; }
int MutableInstrumentsCloudsCloneAudioProcessor::getNumPrograms() { return 1; }
int MutableInstrumentsCloudsCloneAudioProcessor::getCurrentProgram() { return 0; }
void MutableInstrumentsCloudsCloneAudioProcessor::setCurrentProgram (int index) { juce::ignoreUnused (index); }
const juce::String MutableInstrumentsCloudsCloneAudioProcessor::getProgramName (int index) { juce::ignoreUnused (index); return {}; }
void MutableInstrumentsCloudsCloneAudioProcessor::changeProgramName (int index, const juce::String& newName) { juce::ignoreUnused (index, newName); }

void MutableInstrumentsCloudsCloneAudioProcessor::initialiseClouds()
{
    if (cloudsInitialised)
        return;

    constexpr int memLen = 118784;
    constexpr int ccmLen = 65536 - 128;
    blockMem = static_cast<uint8_t*> (std::calloc (memLen, 1));
    blockCcm = static_cast<uint8_t*> (std::calloc (ccmLen, 1));
    cloudsProcessor = new clouds::GranularProcessor();
    std::memset (cloudsProcessor, 0, sizeof (*cloudsProcessor));
    cloudsProcessor->Init (blockMem, memLen, blockCcm, ccmLen);
    cloudsProcessor->set_playback_mode (clouds::PLAYBACK_MODE_GRANULAR);
    cloudsProcessor->set_quality (0);
    cloudsProcessor->Prepare();
    cloudsInitialised = true;
}

void MutableInstrumentsCloudsCloneAudioProcessor::destroyClouds()
{
    delete cloudsProcessor;
    cloudsProcessor = nullptr;
    std::free (blockMem);
    blockMem = nullptr;
    std::free (blockCcm);
    blockCcm = nullptr;
    cloudsInitialised = false;
}

void MutableInstrumentsCloudsCloneAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate = sampleRate;
    initialiseClouds();

    const int maxFrames = juce::jmax (128, samplesPerBlock * 4);
    resampledInputBuffer.setSize (2, maxFrames, false, true, true);
    resampledOutputBuffer.setSize (2, maxFrames, false, true, true);
    inputFrames.resize (static_cast<size_t> (maxFrames));
    outputFrames.resize (static_cast<size_t> (maxFrames));
    downsampleInputPhase = 0.0;
    upsampleOutputPhase = 0.0;
    prevInputSampleL = 0.0f;
    prevInputSampleR = 0.0f;
    prevOutputSampleL = 0.0f;
    prevOutputSampleR = 0.0f;
    inputMeter.store (0.0f);
    grainMeter.store (0.0f);
    scopeWritePos.store (0);
    for (auto& s : incomingScope)
        s.store (0.0f);
}

void MutableInstrumentsCloudsCloneAudioProcessor::releaseResources() {}

bool MutableInstrumentsCloudsCloneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;

    return true;
}

void MutableInstrumentsCloudsCloneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    if (! cloudsInitialised || cloudsProcessor == nullptr)
    {
        buffer.clear();
        return;
    }

    for (int ch = totalNumInputChannels; ch < totalNumOutputChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    const float position = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("position")->load());
    const float size = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("size")->load());
    const float pitch = std::round (parameters.getRawParameterValue ("pitch")->load());
    const float density = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("density")->load());
    const float texture = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("texture")->load());
    const float blend = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("blend")->load());
    const float spread = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("spread")->load());
    const float feedback = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("feedback")->load());
    const float reverb = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("reverb")->load());
    const bool freeze = parameters.getRawParameterValue ("freeze")->load() > 0.5f;
    clouds::Parameters* p = cloudsProcessor->mutable_parameters();
    p->position = position;
    p->size = size;
    p->pitch = juce::jlimit (-48.0f, 48.0f, pitch);
    p->density = density;
    p->texture = texture;
    p->dry_wet = blend;
    p->stereo_spread = spread;
    p->feedback = feedback;
    p->reverb = reverb;
    p->freeze = freeze;
    p->trigger = false;
    p->gate = false;

    float inEnergy = 0.0f;
    const float* inL = buffer.getReadPointer (0);
    const float* inR = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : inL;
    for (int i = 0; i < numSamples; ++i)
    {
        const float mono = 0.5f * (inL[i] + inR[i]);
        const int idx = scopeWritePos.load (std::memory_order_relaxed);
        incomingScope[idx].store (mono, std::memory_order_relaxed);
        scopeWritePos.store ((idx + 1) % scopeSize, std::memory_order_relaxed);
        inEnergy += 0.5f * (inL[i] * inL[i] + inR[i] * inR[i]);
    }

    resampledInputBuffer.clear();
    auto* rsInL = resampledInputBuffer.getWritePointer (0);
    auto* rsInR = resampledInputBuffer.getWritePointer (1);
    const int maxInternalSamples = juce::jmax (1, resampledInputBuffer.getNumSamples() - 8);
    const double downsampleStep = hostSampleRate / internalSampleRate;
    double readPos = downsampleInputPhase;
    int numInternalSamples = 0;

    while (numInternalSamples < maxInternalSamples && readPos <= static_cast<double> (numSamples - 1))
    {
        const int i0 = static_cast<int> (std::floor (readPos));
        const float frac = static_cast<float> (readPos - static_cast<double> (i0));

        const float x0L = (i0 >= 0) ? inL[i0] : prevInputSampleL;
        const float x0R = (i0 >= 0) ? inR[i0] : prevInputSampleR;

        const int i1 = i0 + 1;
        float x1L = inL[numSamples - 1];
        float x1R = inR[numSamples - 1];
        if (i1 >= 0 && i1 < numSamples)
        {
            x1L = inL[i1];
            x1R = inR[i1];
        }
        else if (i1 < 0)
        {
            x1L = prevInputSampleL;
            x1R = prevInputSampleR;
        }

        rsInL[numInternalSamples] = x0L + (x1L - x0L) * frac;
        rsInR[numInternalSamples] = x0R + (x1R - x0R) * frac;
        ++numInternalSamples;
        readPos += downsampleStep;
    }

    if (numInternalSamples == 0)
    {
        rsInL[0] = inL[0];
        rsInR[0] = inR[0];
        numInternalSamples = 1;
        readPos += downsampleStep;
    }

    downsampleInputPhase = readPos - static_cast<double> (numSamples);
    prevInputSampleL = inL[numSamples - 1];
    prevInputSampleR = inR[numSamples - 1];

    resampledOutputBuffer.clear();
    auto* rsOutL = resampledOutputBuffer.getWritePointer (0);
    auto* rsOutR = resampledOutputBuffer.getWritePointer (1);

    constexpr int cloudsBlockSize = 32;
    const int paddedInternalSamples = ((numInternalSamples + cloudsBlockSize - 1) / cloudsBlockSize) * cloudsBlockSize;
    int offset = 0;
    while (offset < paddedInternalSamples)
    {
        const int chunk = juce::jmin (cloudsBlockSize, paddedInternalSamples - offset);
        for (int i = 0; i < chunk; ++i)
        {
            const bool inRange = (offset + i) < numInternalSamples;
            const float inSampleL = inRange ? rsInL[offset + i] : 0.0f;
            const float inSampleR = inRange ? rsInR[offset + i] : 0.0f;
            const float sL = juce::jlimit (-1.0f, 1.0f, inSampleL * 0.6f);
            const float sR = juce::jlimit (-1.0f, 1.0f, inSampleR * 0.6f);
            inputFrames[static_cast<size_t> (i)].l = static_cast<int16_t> (std::round (sL * 32767.0f));
            inputFrames[static_cast<size_t> (i)].r = static_cast<int16_t> (std::round (sR * 32767.0f));
        }

        cloudsProcessor->Process (inputFrames.data(), outputFrames.data(), chunk);

        for (int i = 0; i < chunk; ++i)
        {
            if ((offset + i) < numInternalSamples)
            {
                rsOutL[offset + i] = static_cast<float> (outputFrames[static_cast<size_t> (i)].l) / 32768.0f;
                rsOutR[offset + i] = static_cast<float> (outputFrames[static_cast<size_t> (i)].r) / 32768.0f;
            }
        }
        offset += chunk;
    }

    float* outL = buffer.getWritePointer (0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    juce::HeapBlock<float> upsampledL (static_cast<size_t> (numSamples));
    juce::HeapBlock<float> upsampledR (static_cast<size_t> (numSamples));
    const double upsampleStep = internalSampleRate / hostSampleRate;
    double outReadPos = upsampleOutputPhase;
    const float lastOutL = rsOutL[juce::jmax (0, numInternalSamples - 1)];
    const float lastOutR = rsOutR[juce::jmax (0, numInternalSamples - 1)];
    for (int i = 0; i < numSamples; ++i)
    {
        const int i0 = static_cast<int> (std::floor (outReadPos));
        const float frac = static_cast<float> (outReadPos - static_cast<double> (i0));

        float y0L = prevOutputSampleL;
        float y0R = prevOutputSampleR;
        if (i0 >= 0)
        {
            if (i0 < numInternalSamples)
            {
                y0L = rsOutL[i0];
                y0R = rsOutR[i0];
            }
            else
            {
                y0L = lastOutL;
                y0R = lastOutR;
            }
        }

        const int i1 = i0 + 1;
        float y1L = lastOutL;
        float y1R = lastOutR;
        if (i1 < 0)
        {
            y1L = prevOutputSampleL;
            y1R = prevOutputSampleR;
        }
        else if (i1 < numInternalSamples)
        {
            y1L = rsOutL[i1];
            y1R = rsOutR[i1];
        }

        upsampledL[i] = y0L + (y1L - y0L) * frac;
        upsampledR[i] = y0R + (y1R - y0R) * frac;
        outReadPos += upsampleStep;
    }
    upsampleOutputPhase = outReadPos - static_cast<double> (numInternalSamples);
    prevOutputSampleL = lastOutL;
    prevOutputSampleR = lastOutR;

    float wetEnergy = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float wetL = juce::jlimit (-1.0f, 1.0f, upsampledL[i]);
        float wetR = juce::jlimit (-1.0f, 1.0f, upsampledR[i]);

        // Clean output stage: fixed trim + peak safety without AGC modulation.
        wetL *= 0.6f;
        wetR *= 0.6f;
        const float samplePeak = juce::jmax (std::abs (wetL), std::abs (wetR));
        if (samplePeak > 0.98f)
        {
            const float scale = 0.98f / (samplePeak + 1.0e-6f);
            wetL *= scale;
            wetR *= scale;
        }

        outL[i] = wetL;
        if (outR != nullptr)
            outR[i] = wetR;

        wetEnergy += 0.5f * (wetL * wetL + wetR * wetR);
    }

    const float inRms = std::sqrt (inEnergy / static_cast<float> (juce::jmax (1, numSamples)));
    const float wetRms = std::sqrt (wetEnergy / static_cast<float> (juce::jmax (1, numSamples)));
    inputMeter.store (0.9f * inputMeter.load() + 0.1f * juce::jlimit (0.0f, 1.0f, inRms * 3.0f));
    grainMeter.store (0.9f * grainMeter.load() + 0.1f * juce::jlimit (0.0f, 1.0f, wetRms * 3.0f));
}

bool MutableInstrumentsCloudsCloneAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* MutableInstrumentsCloudsCloneAudioProcessor::createEditor()
{
    return new MutableInstrumentsCloudsCloneAudioProcessorEditor (*this);
}

void MutableInstrumentsCloudsCloneAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void MutableInstrumentsCloudsCloneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MutableInstrumentsCloudsCloneAudioProcessor();
}

std::array<float, MutableInstrumentsCloudsCloneAudioProcessor::scopeSize>
MutableInstrumentsCloudsCloneAudioProcessor::getIncomingScopeSnapshot() const
{
    std::array<float, scopeSize> out {};
    const int head = scopeWritePos.load (std::memory_order_relaxed);
    for (int i = 0; i < scopeSize; ++i)
    {
        const int idx = (head + i) % scopeSize;
        out[i] = incomingScope[idx].load (std::memory_order_relaxed);
    }
    return out;
}
