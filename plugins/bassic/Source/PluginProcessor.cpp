#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
inline float noteToHz (int midiNote) noexcept
{
    return static_cast<float> (juce::MidiMessage::getMidiNoteInHertz (juce::jlimit (0, 127, midiNote)));
}

inline float semitonesToRatio (float semitones) noexcept
{
    return std::pow (2.0f, semitones / 12.0f);
}
} // namespace

void BassicAudioProcessor::SynthVoice::ExpEnvelope::setSampleRate (double newSampleRate) noexcept
{
    sampleRate = juce::jmax (1.0, newSampleRate);
}

float BassicAudioProcessor::SynthVoice::ExpEnvelope::calcRate (double sr, float seconds) noexcept
{
    const float clampedSeconds = juce::jmax (0.0001f, seconds);
    const float denominator = clampedSeconds * static_cast<float> (juce::jmax (1.0, sr));
    return 1.0f - std::exp (-4.6051702f / denominator);
}

bool BassicAudioProcessor::SynthVoice::ExpEnvelope::isNear (float a, float b) noexcept
{
    return std::abs (a - b) <= 1.0e-4f;
}

void BassicAudioProcessor::SynthVoice::ExpEnvelope::setParameters (float attackSeconds, float decaySeconds,
                                                                  float sustainLevel, float releaseSeconds) noexcept
{
    sustainTarget = juce::jlimit (0.0f, 1.0f, sustainLevel);
    attackRate = calcRate (sampleRate, juce::jmax (0.0005f, attackSeconds));
    decayRate = calcRate (sampleRate, juce::jmax (0.001f, decaySeconds));
    releaseRate = calcRate (sampleRate, juce::jmax (0.001f, releaseSeconds));

    if (state == State::sustain)
        value = sustainTarget;
}

void BassicAudioProcessor::SynthVoice::ExpEnvelope::noteOn (bool retrigger) noexcept
{
    if (retrigger)
        value = 0.0f;

    state = State::attack;
}

void BassicAudioProcessor::SynthVoice::ExpEnvelope::noteOff() noexcept
{
    if (state != State::idle)
        state = State::release;
}

void BassicAudioProcessor::SynthVoice::ExpEnvelope::reset() noexcept
{
    state = State::idle;
    value = 0.0f;
}

float BassicAudioProcessor::SynthVoice::ExpEnvelope::getNextSample() noexcept
{
    switch (state)
    {
        case State::idle:
            return 0.0f;

        case State::attack:
            value += (1.0f - value) * attackRate;
            if (value >= 0.999f)
            {
                value = 1.0f;
                state = State::decay;
            }
            break;

        case State::decay:
            value += (sustainTarget - value) * decayRate;
            if (isNear (value, sustainTarget))
            {
                value = sustainTarget;
                state = State::sustain;
            }
            break;

        case State::sustain:
            value = sustainTarget;
            break;

        case State::release:
            value += (0.0f - value) * releaseRate;
            if (value <= 0.0001f)
                reset();
            break;
    }

    return value;
}

bool BassicAudioProcessor::SynthVoice::ExpEnvelope::isActive() const noexcept
{
    return state != State::idle;
}

BassicAudioProcessor::BassicAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, juce::Identifier ("BassicSynth"), createParameterLayout())
{
    synth.clearVoices();
    monoVoice = new SynthVoice (parameters);
    synth.addVoice (monoVoice);
    synth.clearSounds();
    synth.addSound (new SynthSound());

    heldNotes.fill (false);
    heldVelocities.fill (0.0f);
}

BassicAudioProcessor::~BassicAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout BassicAudioProcessor::createParameterLayout()
{
    using Range = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("lfoRate", "LFO Rate", Range (0.05f, 30.0f, 0.0f, 0.35f), 4.0f));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("lfoWave", "LFO Wave", juce::StringArray { "Triangle", "Square", "Sample & Hold" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("lfoDelay", "LFO Delay", Range (0.0f, 2.0f, 0.0f, 0.35f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("lfoPitch", "LFO Pitch", Range (0.0f, 1.0f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("lfoPwm", "LFO PWM", Range (0.0f, 1.0f), 0.25f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("tune", "Tune", Range (-12.0f, 12.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("vcoRange", "VCO Range", juce::StringArray { "16'", "8'", "4'", "2'" }, 1));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("pulseWidth", "Pulse Width", Range (0.10f, 0.90f), 0.50f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("saw", "Saw", Range (0.0f, 1.0f), 0.75f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("square", "Pulse", Range (0.0f, 1.0f), 0.35f));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("subMode", "Sub Osc -2 Oct", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("sub", "Sub Level", Range (0.0f, 1.0f), 0.30f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("noise", "Noise", Range (0.0f, 1.0f), 0.06f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("cutoff", "VCF Freq", Range (40.0f, 18000.0f, 0.01f, 0.25f), 2500.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("resonance", "VCF Res", Range (0.0f, 1.0f), 0.72f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("envAmt", "VCF Env", Range (0.0f, 1.0f), 0.55f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("vcfMod", "VCF Mod", Range (0.0f, 1.0f), 0.15f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("vcfKybd", "VCF Kybd", Range (0.0f, 1.0f), 0.25f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("filterDrive", "Filter Drive", Range (0.0f, 1.0f), 0.28f));

    params.push_back (std::make_unique<juce::AudioParameterBool> ("vcaMode", "VCA Gate Mode", false));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("attack", "Attack", Range (0.0005f, 2.0f, 0.0f, 0.35f), 0.0015f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("decay", "Decay", Range (0.005f, 3.0f, 0.0f, 0.35f), 0.24f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("sustain", "Sustain", Range (0.0f, 1.0f), 0.62f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("release", "Release", Range (0.01f, 4.0f, 0.0f, 0.35f), 0.20f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("portamento", "Portamento", Range (0.0f, 1.0f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("portamentoMode", "Portamento Mode", juce::StringArray { "Always", "Legato" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("level", "Level", Range (0.0f, 1.0f), 0.72f));

    return { params.begin(), params.end() };
}

const juce::String BassicAudioProcessor::getName() const { return JucePlugin_Name; }
bool BassicAudioProcessor::acceptsMidi() const { return true; }
bool BassicAudioProcessor::producesMidi() const { return false; }
bool BassicAudioProcessor::isMidiEffect() const { return false; }
double BassicAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int BassicAudioProcessor::getNumPrograms() { return 1; }
int BassicAudioProcessor::getCurrentProgram() { return 0; }
void BassicAudioProcessor::setCurrentProgram (int) {}
const juce::String BassicAudioProcessor::getProgramName (int) { return {}; }
void BassicAudioProcessor::changeProgramName (int, const juce::String&) {}

void BassicAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    synth.setCurrentPlaybackSampleRate (sampleRate);
    heldNotes.fill (false);
    heldVelocities.fill (0.0f);
    heldOrder.clear();
    activeExternalNote = -1;
}

void BassicAudioProcessor::releaseResources() {}

bool BassicAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void BassicAudioProcessor::removeHeldNote (int note)
{
    note = juce::jlimit (0, 127, note);
    heldNotes[static_cast<size_t> (note)] = false;
    heldVelocities[static_cast<size_t> (note)] = 0.0f;
    heldOrder.erase (std::remove (heldOrder.begin(), heldOrder.end(), note), heldOrder.end());
}

void BassicAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    buffer.clear();

    juce::MidiBuffer perfMidi;

    auto incomingEvent = midiMessages.cbegin();
    const auto incomingEnd = midiMessages.cend();

    auto resolveDirectMonoAtSample = [&] (int sampleIndex)
    {
        const int desiredNote = heldOrder.empty() ? -1 : heldOrder.back();
        if (desiredNote == activeExternalNote)
            return;

        const bool legato = (activeExternalNote >= 0 && desiredNote >= 0);
        if (activeExternalNote >= 0)
            perfMidi.addEvent (juce::MidiMessage::noteOff (1, activeExternalNote), sampleIndex);

        if (desiredNote >= 0)
        {
            if (monoVoice != nullptr)
                monoVoice->setLegatoTransition (legato);
            const float vel = juce::jlimit (0.05f, 1.0f, heldVelocities[static_cast<size_t> (desiredNote)]);
            perfMidi.addEvent (juce::MidiMessage::noteOn (1, desiredNote, vel), sampleIndex);
        }

        activeExternalNote = desiredNote;
    };

    for (int sample = 0; sample < numSamples; ++sample)
    {
        while (incomingEvent != incomingEnd && (*incomingEvent).samplePosition == sample)
        {
            const auto msg = (*incomingEvent).getMessage();
            if (msg.isNoteOn())
            {
                const int note = juce::jlimit (0, 127, msg.getNoteNumber());
                removeHeldNote (note);
                heldNotes[static_cast<size_t> (note)] = true;
                heldVelocities[static_cast<size_t> (note)] = msg.getVelocity();
                heldOrder.push_back (note);
                resolveDirectMonoAtSample (sample);
            }
            else if (msg.isNoteOff())
            {
                const int note = juce::jlimit (0, 127, msg.getNoteNumber());
                removeHeldNote (note);
                resolveDirectMonoAtSample (sample);
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                heldNotes.fill (false);
                heldVelocities.fill (0.0f);
                heldOrder.clear();
                resolveDirectMonoAtSample (sample);
            }
            ++incomingEvent;
        }
    }

    synth.renderNextBlock (buffer, perfMidi, 0, numSamples);
}

bool BassicAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* BassicAudioProcessor::createEditor()
{
    return new BassicAudioProcessorEditor (*this);
}

void BassicAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BassicAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

BassicAudioProcessor::SynthVoice::SynthVoice (juce::AudioProcessorValueTreeState& state)
    : apvts (state)
{
    ladderFilter.setMode (juce::dsp::LadderFilterMode::LPF24);
    bassThinFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    sawLevel = apvts.getRawParameterValue ("saw");
    squareLevel = apvts.getRawParameterValue ("square");
    subLevel = apvts.getRawParameterValue ("sub");
    noiseLevel = apvts.getRawParameterValue ("noise");
    subOscMode = apvts.getRawParameterValue ("subMode");
    lfoRate = apvts.getRawParameterValue ("lfoRate");
    lfoWaveform = apvts.getRawParameterValue ("lfoWave");
    lfoDelay = apvts.getRawParameterValue ("lfoDelay");
    lfoPitch = apvts.getRawParameterValue ("lfoPitch");
    lfoPwm = apvts.getRawParameterValue ("lfoPwm");
    tune = apvts.getRawParameterValue ("tune");
    vcoRange = apvts.getRawParameterValue ("vcoRange");
    pulseWidth = apvts.getRawParameterValue ("pulseWidth");
    filterCutoff = apvts.getRawParameterValue ("cutoff");
    filterResonance = apvts.getRawParameterValue ("resonance");
    filterEnvAmt = apvts.getRawParameterValue ("envAmt");
    filterLfoMod = apvts.getRawParameterValue ("vcfMod");
    filterKeyTrack = apvts.getRawParameterValue ("vcfKybd");
    envAttack = apvts.getRawParameterValue ("attack");
    envDecay = apvts.getRawParameterValue ("decay");
    envSustain = apvts.getRawParameterValue ("sustain");
    envRelease = apvts.getRawParameterValue ("release");
    vcaMode = apvts.getRawParameterValue ("vcaMode");
    portamento = apvts.getRawParameterValue ("portamento");
    portamentoMode = apvts.getRawParameterValue ("portamentoMode");
    filterDrive = apvts.getRawParameterValue ("filterDrive");
    masterLevel = apvts.getRawParameterValue ("level");
}

bool BassicAudioProcessor::SynthVoice::canPlaySound (juce::SynthesiserSound* sound)
{
    return dynamic_cast<SynthSound*> (sound) != nullptr;
}

void BassicAudioProcessor::SynthVoice::setCurrentPlaybackSampleRate (double newRate)
{
    juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);
    sampleRate = newRate > 1.0 ? newRate : 44100.0;

    juce::dsp::ProcessSpec ladderSpec;
    ladderSpec.sampleRate = sampleRate * 2.0;
    ladderSpec.maximumBlockSize = 1024;
    ladderSpec.numChannels = 1;

    ladderFilter.reset();
    ladderFilter.prepare (ladderSpec);
    ladderFilter.setMode (juce::dsp::LadderFilterMode::LPF24);

    juce::dsp::ProcessSpec hpSpec;
    hpSpec.sampleRate = sampleRate;
    hpSpec.maximumBlockSize = 512;
    hpSpec.numChannels = 1;

    bassThinFilter.reset();
    bassThinFilter.prepare (hpSpec);
    bassThinFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    bassThinFilter.setCutoffFrequency (140.0f);
    bassThinFilter.setResonance (0.5f);

    ampEnvelope.setSampleRate (sampleRate);
    ampEnvelope.reset();
    smoothedFrequency.reset (sampleRate, 0.02);
    smoothedFrequency.setCurrentAndTargetValue (currentFrequency);

    lfoPhase = random.nextFloat();
    lfoSampleAndHold = random.nextFloat() * 2.0f - 1.0f;
    noteAgeSamples = 0.0f;
    noteDriftCents = 0.0f;
    cutoffVariancePercent = 0.0f;
    cutoffDriftPercent = 0.0f;
    envTimeVariance = 1.0f;

    updateVoiceParameters();
}

float BassicAudioProcessor::SynthVoice::getParam (const std::atomic<float>* p, float fallback) const noexcept
{
    return p != nullptr ? p->load (std::memory_order_relaxed) : fallback;
}

void BassicAudioProcessor::SynthVoice::updateVoiceParameters()
{
    envAttackSecondsCurrent = getParam (envAttack, 0.0015f) * envTimeVariance;
    envDecaySecondsCurrent = getParam (envDecay, 0.24f) * envTimeVariance;
    envSustainLevelCurrent = getParam (envSustain, 0.62f);
    envReleaseSecondsCurrent = getParam (envRelease, 0.20f) * envTimeVariance;
    ampEnvelope.setParameters (envAttackSecondsCurrent, envDecaySecondsCurrent,
                               envSustainLevelCurrent, envReleaseSecondsCurrent);

    const float baseCutoff = getParam (filterCutoff, 2500.0f);
    const float baseRes = juce::jlimit (0.0f, 1.0f, getParam (filterResonance, 0.72f));
    ladderFilter.setCutoffFrequencyHz (juce::jlimit (20.0f, 20000.0f, baseCutoff));
    ladderFilter.setResonance (baseRes);
    ladderFilter.setDrive (1.0f + getParam (filterDrive, 0.28f) * 3.0f);
}

void BassicAudioProcessor::SynthVoice::startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*, int)
{
    juce::ignoreUnused (velocity);

    const float tuneSemis = getParam (tune, 0.0f);
    const int rangeIndex = juce::jlimit (0, 3, static_cast<int> (std::round (getParam (vcoRange, 1.0f))));
    static constexpr std::array<int, 4> rangeSemis { -12, 0, 12, 24 };
    const int midiNote = juce::jlimit (0, 127, midiNoteNumber + static_cast<int> (std::round (tuneSemis)) + rangeSemis[static_cast<size_t> (rangeIndex)]);

    noteDriftCents = juce::jmap (random.nextFloat(), -3.0f, 3.0f);
    cutoffVariancePercent = juce::jmap (random.nextFloat(), -2.0f, 2.0f);
    cutoffDriftPercent = cutoffVariancePercent;
    envTimeVariance = juce::jmap (random.nextFloat(), 0.98f, 1.02f);

    currentFrequency = noteToHz (midiNote) * semitonesToRatio (noteDriftCents / 100.0f);

    const bool portLegatoMode = static_cast<int> (std::round (getParam (portamentoMode, 1.0f))) == 1;
    const bool glideThisNote = (!portLegatoMode) || legatoTransition;
    const float glide = getParam (portamento, 0.0f);
    const double glideSeconds = juce::jmap<double> (glide, 0.0, 1.0, 0.001, 0.35);

    if (glideThisNote)
    {
        smoothedFrequency.reset (sampleRate, glideSeconds);
        if (! isVoiceActive())
            smoothedFrequency.setCurrentAndTargetValue (currentFrequency);
        else
            smoothedFrequency.setTargetValue (currentFrequency);
    }
    else
    {
        smoothedFrequency.setCurrentAndTargetValue (currentFrequency);
    }

    updateVoiceParameters();

    if (!legatoTransition)
    {
        phase = random.nextFloat();      // random phase per trigger
        subPhase = phase;
        ampEnvelope.noteOn (true);       // retrigger
    }

    noteAgeSamples = 0.0f;
    legatoTransition = false;
}

void BassicAudioProcessor::SynthVoice::stopNote (float, bool allowTailOff)
{
    const bool gateMode = getParam (vcaMode, 0.0f) > 0.5f;
    if (gateMode)
    {
        clearCurrentNote();
        ampEnvelope.reset();
        return;
    }

    if (allowTailOff)
        ampEnvelope.noteOff();
    else
    {
        clearCurrentNote();
        ampEnvelope.reset();
    }
}

void BassicAudioProcessor::SynthVoice::pitchWheelMoved (int) {}
void BassicAudioProcessor::SynthVoice::controllerMoved (int, int) {}

float BassicAudioProcessor::SynthVoice::polyBlep (float t, float dt) noexcept
{
    if (dt <= 0.0f)
        return 0.0f;

    if (t < dt)
    {
        const float x = t / dt;
        return x + x - x * x - 1.0f;
    }

    if (t > 1.0f - dt)
    {
        const float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }

    return 0.0f;
}

void BassicAudioProcessor::SynthVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    if (!isVoiceActive())
        return;

    updateVoiceParameters();

    const float localSaw = getParam (sawLevel, 0.75f);
    const float localSquare = getParam (squareLevel, 0.35f);
    const float localSub = getParam (subLevel, 0.30f);
    const float localNoise = getParam (noiseLevel, 0.06f);
    const float localLevel = getParam (masterLevel, 0.72f);
    const float localPwBase = juce::jlimit (0.10f, 0.90f, getParam (pulseWidth, 0.50f));
    const int localSubMode = (getParam (subOscMode, 0.0f) > 0.5f) ? 1 : 0;
    const float localLfoRate = getParam (lfoRate, 4.0f);
    const int localLfoWave = juce::jlimit (0, 2, static_cast<int> (std::round (getParam (lfoWaveform, 0.0f))));
    const float localLfoDelaySec = getParam (lfoDelay, 0.0f);
    const float localLfoPitch = getParam (lfoPitch, 0.0f);
    const float localLfoPwm = getParam (lfoPwm, 0.25f);
    const float localRes = juce::jlimit (0.0f, 1.0f, getParam (filterResonance, 0.72f));
    const float localFilterDrive = getParam (filterDrive, 0.28f);
    const float localFilterEnvAmt = getParam (filterEnvAmt, 0.55f);
    const float localFilterLfoMod = getParam (filterLfoMod, 0.15f);
    const float localFilterKeyTrack = getParam (filterKeyTrack, 0.25f);
    const float localFilterCutoff = getParam (filterCutoff, 2500.0f);
    const bool gateMode = getParam (vcaMode, 0.0f) > 0.5f;

    const float delaySamples = juce::jmax (1.0f, localLfoDelaySec * static_cast<float> (sampleRate));
    const float subRatio = localSubMode == 0 ? 0.5f : 0.25f;
    constexpr int oversampleFactor = 2;
    constexpr float oversampleScale = 1.0f / static_cast<float> (oversampleFactor);

    for (int i = 0; i < numSamples; ++i)
    {
        noteAgeSamples += 1.0f;

        const float lfoInc = localLfoRate / static_cast<float> (sampleRate);
        lfoPhase += lfoInc;
        if (lfoPhase >= 1.0f)
        {
            lfoPhase -= 1.0f;
            if (localLfoWave == 2)
                lfoSampleAndHold = random.nextFloat() * 2.0f - 1.0f;
        }

        float lfoRaw = 0.0f;
        if (localLfoWave == 0)
            lfoRaw = 2.0f * std::abs (2.0f * lfoPhase - 1.0f) - 1.0f;
        else if (localLfoWave == 1)
            lfoRaw = (lfoPhase < 0.5f) ? 1.0f : -1.0f;
        else
            lfoRaw = lfoSampleAndHold;

        const float lfoFade = localLfoDelaySec <= 0.0001f
            ? 1.0f
            : juce::jlimit (0.0f, 1.0f, noteAgeSamples / delaySamples);
        const float lfoValue = lfoRaw * lfoFade;

        const float pitchSemis = lfoValue * localLfoPitch * 4.0f;
        const float hz = smoothedFrequency.getNextValue() * semitonesToRatio (pitchSemis);
        const float dt = juce::jlimit (1.0e-6f, 0.49f, hz / static_cast<float> (sampleRate)) * oversampleScale;

        const float env = ampEnvelope.getNextSample();
        const float localPw = juce::jlimit (0.10f, 0.90f, localPwBase + (lfoValue * localLfoPwm * 0.38f));
        cutoffDriftPercent = juce::jlimit (-2.0f, 2.0f, cutoffDriftPercent + (random.nextFloat() * 2.0f - 1.0f) * 0.004f);
        const float baseCutoff = localFilterCutoff * (1.0f + cutoffDriftPercent * 0.01f);
        const float envMod = env * localFilterEnvAmt * 12000.0f;
        const float lfoMod = lfoValue * localFilterLfoMod * 5000.0f;
        const float midiFloat = 69.0f + 12.0f * std::log2 (juce::jmax (1.0f, hz) / 440.0f);
        const float keyNorm = juce::jlimit (0.0f, 1.0f, (midiFloat - 24.0f) / 72.0f);
        const float keyMod = keyNorm * localFilterKeyTrack * 7000.0f;
        const float cutoff = juce::jlimit (20.0f, 20000.0f, baseCutoff + envMod + lfoMod + keyMod);

        ladderFilter.setCutoffFrequencyHz (cutoff);
        ladderFilter.setResonance (localRes);
        ladderFilter.setDrive (1.0f + localFilterDrive * 3.0f);

        float filtered = 0.0f;
        for (int os = 0; os < oversampleFactor; ++os)
        {
            phase += dt;
            if (phase >= 1.0f)
                phase -= 1.0f;

            subPhase += dt * subRatio;
            if (subPhase >= 1.0f)
                subPhase -= 1.0f;

            float saw = (2.0f * phase) - 1.0f;
            saw -= polyBlep (phase, dt);

            float pulse = (phase < localPw) ? 1.0f : -1.0f;
            pulse += polyBlep (phase, dt);
            float t2 = phase - localPw;
            if (t2 < 0.0f)
                t2 += 1.0f;
            pulse -= polyBlep (t2, dt);

            const float sub = (subPhase < 0.5f) ? 1.0f : -1.0f;
            const float noise = random.nextFloat() * 2.0f - 1.0f;

            float mix = saw * localSaw + pulse * localSquare + sub * localSub + noise * localNoise;
            mix = std::tanh (mix * (1.0f + localFilterDrive * 1.4f));

            filtered += ladderFilter.processSample (mix, 0);
        }
        filtered *= oversampleScale;

        // Bass thinning at higher resonance.
        const float thinAmt = juce::jmap (juce::jlimit (0.65f, 1.0f, localRes), 0.65f, 1.0f, 0.0f, 0.50f);
        const float hp = bassThinFilter.processSample (0, filtered);
        filtered = filtered * (1.0f - thinAmt) + hp * thinAmt;

        const float amp = gateMode ? (isKeyDown() ? 1.0f : 0.0f) : env;
        const float sampleOut = filtered * amp * localLevel;

        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
            outputBuffer.addSample (ch, startSample + i, sampleOut);
    }

    if ((!gateMode && !ampEnvelope.isActive()) || (gateMode && !isKeyDown()))
        clearCurrentNote();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassicAudioProcessor();
}
