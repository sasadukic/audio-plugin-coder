#include "PluginProcessor.h"
#include "PluginEditor.h"

GameSFXDAWAudioProcessor::GameSFXDAWAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, juce::Identifier ("GameSFXDAW"), createParameterLayout())
{
}

GameSFXDAWAudioProcessor::~GameSFXDAWAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout GameSFXDAWAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "masterPreviewGainDb", 1 },
        "Master Preview Gain",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [] (float value, int)
        {
            return juce::String (value, 1) + " dB";
        }
    ));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "selectedContainerIndex", 1 },
        "Selected Container Index",
        0,
        512,
        0
    ));

    return layout;
}

const juce::String GameSFXDAWAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool GameSFXDAWAudioProcessor::acceptsMidi() const
{
    return false;
}

bool GameSFXDAWAudioProcessor::producesMidi() const
{
    return false;
}

bool GameSFXDAWAudioProcessor::isMidiEffect() const
{
    return false;
}

double GameSFXDAWAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int GameSFXDAWAudioProcessor::getNumPrograms()
{
    return 1;
}

int GameSFXDAWAudioProcessor::getCurrentProgram()
{
    return 0;
}

void GameSFXDAWAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String GameSFXDAWAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void GameSFXDAWAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void GameSFXDAWAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (juce::jmax (1, getTotalNumOutputChannels()));

    previewGain.prepare (spec);
    previewGain.setRampDurationSeconds (0.02);
}

void GameSFXDAWAudioProcessor::releaseResources()
{
}

bool GameSFXDAWAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void GameSFXDAWAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    const auto gainDb = parameters.getRawParameterValue ("masterPreviewGainDb")->load();
    const auto selectedContainerIndex = parameters.getRawParameterValue ("selectedContainerIndex")->load();
    previewGain.setGainDecibels (gainDb);
    juce::ignoreUnused (selectedContainerIndex);

    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);
    previewGain.process (context);
}

bool GameSFXDAWAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* GameSFXDAWAudioProcessor::createEditor()
{
    return new GameSFXDAWAudioProcessorEditor (*this);
}

void GameSFXDAWAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void GameSFXDAWAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState.get() != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GameSFXDAWAudioProcessor();
}
