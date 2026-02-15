#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr float kOverlayLiftDb = -6.0f;

inline float normToDb (float norm) noexcept
{
    return -96.0f + juce::jlimit (0.0f, 1.0f, norm) * 96.0f;
}
} // namespace

DreamAudioProcessor::DreamAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, juce::Identifier ("DreamAnalyzer"), createParameterLayout())
{
}

DreamAudioProcessor::~DreamAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout DreamAudioProcessor::createParameterLayout()
{
    return {};
}

const juce::String DreamAudioProcessor::getName() const { return JucePlugin_Name; }
bool DreamAudioProcessor::acceptsMidi() const { return false; }
bool DreamAudioProcessor::producesMidi() const { return false; }
bool DreamAudioProcessor::isMidiEffect() const { return false; }
double DreamAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int DreamAudioProcessor::getNumPrograms() { return 1; }
int DreamAudioProcessor::getCurrentProgram() { return 0; }
void DreamAudioProcessor::setCurrentProgram (int index) { juce::ignoreUnused (index); }
const juce::String DreamAudioProcessor::getProgramName (int index) { juce::ignoreUnused (index); return {}; }
void DreamAudioProcessor::changeProgramName (int index, const juce::String& newName) { juce::ignoreUnused (index, newName); }

void DreamAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate.store (sampleRate);
    updateSpectrumLayout (sampleRate);
    fftMagnitudeToDbScale = computeFftMagnitudeScale();

    lufsHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 60.0f);
    lufsHighShelf.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sampleRate, 1500.0f, 0.7071f, juce::Decibels::decibelsToGain (4.0f));
    updateSoloBandFilters (sampleRate);
    lufsHighPass.reset();
    lufsHighShelf.reset();
    resetSoloBandFilters();
    resetResonanceSuppressor();
    lufsWeightedEnergySum = 0.0;
    lufsWeightedSampleCount = 0.0;
    rmsSmoothedDb = -96.0f;
    rmsDb.store (-96.0f);
    lufsIntegrated.store (-96.0f);

    fifoIndex = 0;
    std::fill (fifo.begin(), fifo.end(), 0.0f);
    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::fill (smoothedSpectrum.begin(), smoothedSpectrum.end(), 0.0f);
    for (auto& v : spectrumData)
        v.store (0.0f);
    for (auto& v : oscilloscopeData)
        v.store (0.0f);
    for (auto& v : oscilloscopeDataRight)
        v.store (0.0f);
    oscilloscopeLastBin = -1;
    oscilloscopeQuarterPositionSamples = 0.0;
    oscilloscopeLastLengthMode = oscilloscopeLengthMode.load (std::memory_order_relaxed);
}

void DreamAudioProcessor::releaseResources() {}

bool DreamAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;

    return true;
}

void DreamAudioProcessor::updateSpectrumLayout (double sampleRate) noexcept
{
    spectrumBinPosition = buildSpectrumBinPositions (sampleRate);

    const float nyquist = static_cast<float> (sampleRate * 0.5);
    const float minFreq = 20.0f;
    const float maxFreq = juce::jlimit (minFreq + 1.0f, nyquist, 20000.0f);
    const float ratio = maxFreq / minFreq;
    for (int i = 0; i < spectrumBins; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (spectrumBins - 1);
        spectrumBinFrequencyHz[static_cast<size_t> (i)] = minFreq * std::pow (ratio, t);
    }
}

std::array<float, DreamAudioProcessor::spectrumBins> DreamAudioProcessor::buildSpectrumBinPositions (double sampleRate) noexcept
{
    std::array<float, spectrumBins> positions {};
    const float nyquist = static_cast<float> (sampleRate * 0.5);
    const float minFreq = 20.0f;
    const float maxFreq = juce::jlimit (minFreq + 1.0f, nyquist, 20000.0f);
    const float ratio = maxFreq / minFreq;
    const float fftBinHz = static_cast<float> (sampleRate / static_cast<double> (fftSize));
    const float maxIndex = static_cast<float> ((fftSize / 2) - 2);

    for (int i = 0; i < spectrumBins; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (spectrumBins - 1);
        const float f = minFreq * std::pow (ratio, t);
        const float fftBin = f / fftBinHz;
        positions[static_cast<size_t> (i)] = juce::jlimit (1.0f, maxIndex, fftBin);
    }

    return positions;
}

float DreamAudioProcessor::computeFftMagnitudeScale()
{
    std::array<float, fftSize> windowTable {};
    juce::dsp::WindowingFunction<float>::fillWindowingTables (
        windowTable.data(),
        fftSize,
        juce::dsp::WindowingFunction<float>::hann,
        true);

    double windowSum = 0.0;
    for (const auto w : windowTable)
        windowSum += static_cast<double> (w);

    const double coherentGain = windowSum / static_cast<double> (fftSize);
    const double safeGain = juce::jmax (coherentGain, 1.0e-9);
    return static_cast<float> (2.0 / (static_cast<double> (fftSize) * safeGain));
}

void DreamAudioProcessor::pushAnalyserSample (float sample) noexcept
{
    fifo[static_cast<size_t> (fifoIndex)] = sample;
    ++fifoIndex;

    if (fifoIndex >= fftSize)
    {
        buildSpectrumFrame();
        fifoIndex = 0;
    }
}

void DreamAudioProcessor::buildSpectrumFrame() noexcept
{
    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::copy (fifo.begin(), fifo.end(), fftData.begin());
    window.multiplyWithWindowingTable (fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const int maxIndex = (fftSize / 2) - 1;
    auto readMagnitude = [&] (float fftBin) -> float
    {
        const float clamped = juce::jlimit (1.0f, static_cast<float> (maxIndex - 1), fftBin);
        const int index = static_cast<int> (clamped);
        const float frac = clamped - static_cast<float> (index);
        const float magA = fftData[static_cast<size_t> (index)];
        const float magB = fftData[static_cast<size_t> (juce::jmin (index + 1, maxIndex))];
        return juce::jmax (0.0f, magA + frac * (magB - magA));
    };

    for (int i = 0; i < spectrumBins; ++i)
    {
        const float pos = spectrumBinPosition[static_cast<size_t> (i)];
        const float centerMag = readMagnitude (pos);
        const float leftMag = readMagnitude (pos - 0.5f);
        const float rightMag = readMagnitude (pos + 0.5f);
        const float blendedMag = centerMag * 0.60f + leftMag * 0.20f + rightMag * 0.20f;

        const float scaledMag = blendedMag * fftMagnitudeToDbScale;
        const float dB = juce::Decibels::gainToDecibels (scaledMag, -120.0f);
        const float normalized = juce::jlimit (0.0f, 1.0f, juce::jmap (dB, -96.0f, 0.0f, 0.0f, 1.0f));

        float& smoothed = smoothedSpectrum[static_cast<size_t> (i)];
        if (normalized >= smoothed)
            smoothed = smoothed * 0.25f + normalized * 0.75f;
        else
            smoothed = smoothed * 0.90f + normalized * 0.10f;

        spectrumData[static_cast<size_t> (i)].store (smoothed, std::memory_order_relaxed);
    }
}

void DreamAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    bool hasHostPpq = false;
    bool hostTransportIsPlaying = false;
    double hostPpq = 0.0;
    double hostQuarterNotesPerBar = 4.0;
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            if (auto bpm = position->getBpm())
                currentTempoBpm.store (static_cast<float> (*bpm), std::memory_order_relaxed);
            hostTransportIsPlaying = position->getIsPlaying();
            if (auto ppq = position->getPpqPosition())
            {
                hostPpq = *ppq;
                hasHostPpq = hostTransportIsPlaying;
            }
            if (auto ts = position->getTimeSignature())
            {
                const int numerator = juce::jmax (1, static_cast<int> (ts->numerator));
                const int denominator = juce::jmax (1, static_cast<int> (ts->denominator));
                hostQuarterNotesPerBar = static_cast<double> (numerator) * (4.0 / static_cast<double> (denominator));
            }
        }
    }

    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    for (int ch = totalNumInputChannels; ch < totalNumOutputChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    applyResonanceSuppressorToBuffer (buffer);

    const float* inL = buffer.getReadPointer (0);
    const float* inR = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : inL;
    const int lengthMode = oscilloscopeLengthMode.load (std::memory_order_relaxed);
    if (lengthMode != oscilloscopeLastLengthMode)
    {
        oscilloscopeLastLengthMode = lengthMode;
        oscilloscopeLastBin = -1;
        oscilloscopeQuarterPositionSamples = 0.0;
        for (auto& v : oscilloscopeData)
            v.store (0.0f, std::memory_order_relaxed);
        for (auto& v : oscilloscopeDataRight)
            v.store (0.0f, std::memory_order_relaxed);
    }

    const float bpm = juce::jlimit (30.0f, 300.0f, currentTempoBpm.load (std::memory_order_relaxed));
    const double samplesPerQuarter = juce::jmax (
        1.0,
        currentSampleRate.load() * (60.0 / static_cast<double> (bpm)));
    const double cycleQuarterNotes = (lengthMode == 0)
        ? 1.0
        : juce::jlimit (1.0, 16.0, hostQuarterNotesPerBar);
    const double samplesPerCycle = samplesPerQuarter * cycleQuarterNotes;

    if (hasHostPpq)
    {
        double phaseInCycle = std::fmod (hostPpq, cycleQuarterNotes);
        if (phaseInCycle < 0.0)
            phaseInCycle += cycleQuarterNotes;
        oscilloscopeQuarterPositionSamples = (phaseInCycle / cycleQuarterNotes) * samplesPerCycle;
    }

    double sumSquares = 0.0;
    double weightedSumSquares = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mono = 0.5f * (inL[i] + inR[i]);
        const float oscSampleLeft = juce::jlimit (-1.0f, 1.0f, inL[i]);
        const float oscSampleRight = juce::jlimit (-1.0f, 1.0f, inR[i]);
        pushAnalyserSample (mono);

        const double phase = juce::jlimit (0.0, 0.999999, oscilloscopeQuarterPositionSamples / samplesPerCycle);
        const int bin = juce::jlimit (
            0,
            oscilloscopeSamples - 1,
            static_cast<int> (phase * static_cast<double> (oscilloscopeSamples)));

        oscilloscopeData[static_cast<size_t> (bin)].store (oscSampleLeft, std::memory_order_relaxed);
        oscilloscopeDataRight[static_cast<size_t> (bin)].store (oscSampleRight, std::memory_order_relaxed);
        oscilloscopeLastBin = bin;
        oscilloscopeQuarterPositionSamples += 1.0;
        while (oscilloscopeQuarterPositionSamples >= samplesPerCycle)
            oscilloscopeQuarterPositionSamples -= samplesPerCycle;
        sumSquares += static_cast<double> (mono * mono);

        float weighted = lufsHighPass.processSample (mono);
        weighted = lufsHighShelf.processSample (weighted);
        weightedSumSquares += static_cast<double> (weighted * weighted);
    }

    const float blockRms = static_cast<float> (std::sqrt (sumSquares / static_cast<double> (numSamples)));
    const float blockRmsDb = juce::Decibels::gainToDecibels (blockRms, -96.0f);
    rmsSmoothedDb = rmsSmoothedDb * 0.82f + blockRmsDb * 0.18f;
    rmsDb.store (rmsSmoothedDb, std::memory_order_relaxed);

    lufsWeightedEnergySum += weightedSumSquares;
    lufsWeightedSampleCount += static_cast<double> (numSamples);

    const double integratedMs = lufsWeightedEnergySum / juce::jmax (1.0, lufsWeightedSampleCount);
    const float integratedLufs = juce::Decibels::gainToDecibels (
        static_cast<float> (std::sqrt (integratedMs)), -96.0f) - 0.691f;
    lufsIntegrated.store (integratedLufs, std::memory_order_relaxed);

    applySoloBandToBuffer (buffer);
}

bool DreamAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* DreamAudioProcessor::createEditor()
{
    return new DreamAudioProcessorEditor (*this);
}

void DreamAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void DreamAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

std::array<float, DreamAudioProcessor::spectrumBins> DreamAudioProcessor::getSpectrumSnapshot() const
{
    std::array<float, spectrumBins> out {};
    for (int i = 0; i < spectrumBins; ++i)
        out[static_cast<size_t> (i)] = spectrumData[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    return out;
}

std::array<float, DreamAudioProcessor::spectrumBins> DreamAudioProcessor::getReferenceSpectrumSnapshot() const
{
    std::array<float, spectrumBins> out {};
    for (int i = 0; i < spectrumBins; ++i)
        out[static_cast<size_t> (i)] = referenceSpectrumData[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    return out;
}

std::array<float, DreamAudioProcessor::oscilloscopeSamples> DreamAudioProcessor::getOscilloscopeSnapshot() const
{
    std::array<float, oscilloscopeSamples> out {};
    for (int i = 0; i < oscilloscopeSamples; ++i)
        out[static_cast<size_t> (i)] = oscilloscopeData[static_cast<size_t> (i)].load (std::memory_order_relaxed);

    return out;
}

std::array<float, DreamAudioProcessor::oscilloscopeSamples> DreamAudioProcessor::getOscilloscopeSnapshotRight() const
{
    std::array<float, oscilloscopeSamples> out {};
    for (int i = 0; i < oscilloscopeSamples; ++i)
        out[static_cast<size_t> (i)] = oscilloscopeDataRight[static_cast<size_t> (i)].load (std::memory_order_relaxed);

    return out;
}

void DreamAudioProcessor::setOscilloscopeLengthMode (int mode) noexcept
{
    oscilloscopeLengthMode.store (mode == 0 ? 0 : 1, std::memory_order_relaxed);
}

int DreamAudioProcessor::getOscilloscopeLengthMode() const noexcept
{
    return oscilloscopeLengthMode.load (std::memory_order_relaxed);
}

void DreamAudioProcessor::setSoloBand (int bandIndex) noexcept
{
    const int clamped = (bandIndex >= 0 && bandIndex <= 3) ? bandIndex : -1;
    const int previous = soloBand.exchange (clamped, std::memory_order_relaxed);
    if (previous != clamped)
        resetSoloBandFilters();
}

double DreamAudioProcessor::getCurrentAnalysisSampleRate() const noexcept
{
    return currentSampleRate.load();
}

float DreamAudioProcessor::getRmsDb() const noexcept
{
    return rmsDb.load (std::memory_order_relaxed);
}

float DreamAudioProcessor::getLufsIntegrated() const noexcept
{
    return lufsIntegrated.load (std::memory_order_relaxed);
}

void DreamAudioProcessor::updateSoloBandFilters (double sampleRate) noexcept
{
    const float safeSampleRate = juce::jmax (1000.0f, static_cast<float> (sampleRate));
    const float maxCutoff = safeSampleRate * 0.45f;
    const float edgeLow = juce::jmin (200.0f, maxCutoff);
    const float edgeMid = juce::jmin (2000.0f, maxCutoff);
    const float edgeHigh = juce::jmin (5000.0f, maxCutoff);

    for (int ch = 0; ch < 2; ++ch)
    {
        soloHighPass200[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeHighPass (safeSampleRate, edgeLow, 0.7071f);
        soloLowPass200[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeLowPass (safeSampleRate, edgeLow, 0.7071f);

        soloHighPass2k[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeHighPass (safeSampleRate, edgeMid, 0.7071f);
        soloLowPass2k[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeLowPass (safeSampleRate, edgeMid, 0.7071f);

        soloHighPass5k[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeHighPass (safeSampleRate, edgeHigh, 0.7071f);
        soloLowPass5k[static_cast<size_t> (ch)].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeLowPass (safeSampleRate, edgeHigh, 0.7071f);
    }
}

void DreamAudioProcessor::resetSoloBandFilters() noexcept
{
    for (auto& filter : soloHighPass200)
        filter.reset();
    for (auto& filter : soloLowPass200)
        filter.reset();
    for (auto& filter : soloHighPass2k)
        filter.reset();
    for (auto& filter : soloLowPass2k)
        filter.reset();
    for (auto& filter : soloHighPass5k)
        filter.reset();
    for (auto& filter : soloLowPass5k)
        filter.reset();
}

void DreamAudioProcessor::applySoloBandToBuffer (juce::AudioBuffer<float>& buffer) noexcept
{
    const int activeBand = soloBand.load (std::memory_order_relaxed);
    if (activeBand < 0 || activeBand > 3)
        return;

    const int channels = juce::jmin (2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    for (int ch = 0; ch < channels; ++ch)
    {
        float* data = buffer.getWritePointer (ch);
        auto& hp200 = soloHighPass200[static_cast<size_t> (ch)];
        auto& lp200 = soloLowPass200[static_cast<size_t> (ch)];
        auto& hp2k = soloHighPass2k[static_cast<size_t> (ch)];
        auto& lp2k = soloLowPass2k[static_cast<size_t> (ch)];
        auto& hp5k = soloHighPass5k[static_cast<size_t> (ch)];
        auto& lp5k = soloLowPass5k[static_cast<size_t> (ch)];

        for (int i = 0; i < samples; ++i)
        {
            float sample = data[i];
            switch (activeBand)
            {
                case 0: // Low: <= 200 Hz
                    sample = lp200.processSample (sample);
                    break;

                case 1: // Low-mid: 200 Hz .. 2 kHz
                    sample = hp200.processSample (sample);
                    sample = lp2k.processSample (sample);
                    break;

                case 2: // High-mid: 2 kHz .. 5 kHz
                    sample = hp2k.processSample (sample);
                    sample = lp5k.processSample (sample);
                    break;

                case 3: // High: >= 5 kHz
                    sample = hp5k.processSample (sample);
                    break;

                default:
                    break;
            }

            data[i] = sample;
        }
    }
}

void DreamAudioProcessor::resetResonanceSuppressor() noexcept
{
    const double sampleRate = juce::jmax (1000.0, currentSampleRate.load());

    for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
    {
        auto& band = resonanceBands[static_cast<size_t> (bandIndex)];
        const float t = resonanceSuppressorBands > 1
            ? static_cast<float> (bandIndex) / static_cast<float> (resonanceSuppressorBands - 1)
            : 0.0f;
        const float startHz = 120.0f;
        const float endHz = 9000.0f;
        band.currentFrequencyHz = startHz * std::pow (endHz / startHz, t);
        band.currentGainDb = 0.0f;
        band.currentQ = 5.0f;

        auto coeff = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
            sampleRate,
            band.currentFrequencyHz,
            band.currentQ,
            juce::Decibels::decibelsToGain (band.currentGainDb));

        for (auto& filter : band.filters)
        {
            filter.reset();
            filter.coefficients = coeff;
        }

        resonanceBandFrequencyUi[static_cast<size_t> (bandIndex)].store (band.currentFrequencyHz, std::memory_order_relaxed);
        resonanceBandGainUi[static_cast<size_t> (bandIndex)].store (0.0f, std::memory_order_relaxed);
    }
}

void DreamAudioProcessor::updateResonanceSuppressorTargets (int numSamples) noexcept
{
    const double sampleRate = juce::jmax (1000.0, currentSampleRate.load());
    const float overlayLevelDb = juce::jlimit (-23.0f, 0.0f, resonanceOverlayLevelDb.load (std::memory_order_relaxed));
    const float overlayWidthDb = juce::jlimit (3.0f, 18.0f, resonanceOverlayWidthDb.load (std::memory_order_relaxed));
    const float overlayTiltDb = juce::jlimit (-24.0f, 24.0f, resonanceOverlayTiltDb.load (std::memory_order_relaxed));
    constexpr float warningStartDb = 0.08f;
    constexpr float redStartDb = 3.0f;
    const float halfWidthDb = 0.5f * overlayWidthDb;

    std::array<float, spectrumBins> thresholdUpperDb {};
    float maxUpperDb = -std::numeric_limits<float>::infinity();

    for (int i = 0; i < spectrumBins; ++i)
    {
        const size_t idx = static_cast<size_t> (i);
        const float referenceNorm = referenceSpectrumData[idx].load (std::memory_order_relaxed);
        const float freqHz = juce::jmax (20.0f, spectrumBinFrequencyHz[idx]);
        const float octaveFrom1k = std::log2 (freqHz / 1000.0f);
        const float centerDb = normToDb (referenceNorm) + (overlayTiltDb * octaveFrom1k) + kOverlayLiftDb;
        const float upperDb = centerDb + halfWidthDb;
        thresholdUpperDb[idx] = upperDb;
        maxUpperDb = juce::jmax (maxUpperDb, upperDb);
    }

    if (! std::isfinite (maxUpperDb))
        maxUpperDb = -24.0f;

    const float alignToZeroDb = -24.0f - maxUpperDb;
    for (int i = 0; i < spectrumBins; ++i)
        thresholdUpperDb[static_cast<size_t> (i)] += alignToZeroDb + overlayLevelDb;

    struct Candidate
    {
        int bin = 0;
        float exceedDb = 0.0f;
        float score = 0.0f;
    };

    std::array<Candidate, spectrumBins> candidates {};
    int candidateCount = 0;
    for (int i = 1; i < spectrumBins - 1; ++i)
    {
        const size_t idx = static_cast<size_t> (i);
        const float frequencyHz = juce::jmax (20.0f, spectrumBinFrequencyHz[idx]);
        const float octaveFrom1k = std::log2 (frequencyHz / 1000.0f);
        const float currentDb = normToDb (smoothedSpectrum[idx]) + (overlayTiltDb * octaveFrom1k);
        const float exceedDb = currentDb - thresholdUpperDb[idx];

        const float prevFreqHz = juce::jmax (20.0f, spectrumBinFrequencyHz[static_cast<size_t> (i - 1)]);
        const float prevOctaveFrom1k = std::log2 (prevFreqHz / 1000.0f);
        const float prevDb = (normToDb (smoothedSpectrum[static_cast<size_t> (i - 1)]) + (overlayTiltDb * prevOctaveFrom1k))
            - thresholdUpperDb[static_cast<size_t> (i - 1)];

        const float nextFreqHz = juce::jmax (20.0f, spectrumBinFrequencyHz[static_cast<size_t> (i + 1)]);
        const float nextOctaveFrom1k = std::log2 (nextFreqHz / 1000.0f);
        const float nextDb = (normToDb (smoothedSpectrum[static_cast<size_t> (i + 1)]) + (overlayTiltDb * nextOctaveFrom1k))
            - thresholdUpperDb[static_cast<size_t> (i + 1)];

        const float highAssist = juce::jmap (
            juce::jlimit (0.0f, 1.0f, (std::log2 (frequencyHz / 600.0f) + 1.0f) / 4.0f),
            0.0f,
            1.0f,
            0.0f,
            0.12f);
        if (exceedDb <= warningStartDb)
            continue;
        if (exceedDb < prevDb || exceedDb < nextDb)
            continue;

        candidates[static_cast<size_t> (candidateCount++)] = { i, exceedDb, exceedDb + highAssist };
    }

    std::sort (candidates.begin(),
               candidates.begin() + candidateCount,
               [] (const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::array<float, resonanceSuppressorBands> targetFrequencyHz {};
    std::array<float, resonanceSuppressorBands> targetGainDb {};
    std::array<float, resonanceSuppressorBands> targetQ {};
    std::array<float, resonanceSuppressorBands> selectedFreqHz {};
    int selectedCount = 0;

    for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
    {
        const auto& band = resonanceBands[static_cast<size_t> (bandIndex)];
        targetFrequencyHz[static_cast<size_t> (bandIndex)] = band.currentFrequencyHz;
        targetGainDb[static_cast<size_t> (bandIndex)] = 0.0f;
        targetQ[static_cast<size_t> (bandIndex)] = band.currentQ;
    }

    auto canSelectFrequency = [&selectedFreqHz, &selectedCount] (float frequencyHz) -> bool
    {
        for (int s = 0; s < selectedCount; ++s)
        {
            const float octaveDistance = std::abs (std::log2 (frequencyHz / juce::jmax (20.0f, selectedFreqHz[static_cast<size_t> (s)])));
            if (octaveDistance < 0.16f)
                return false;
        }
        return true;
    };

    auto assignCandidateToSlot = [&] (const Candidate& candidate)
    {
        if (selectedCount >= resonanceSuppressorBands)
            return;

        const float frequencyHz = juce::jmax (20.0f, spectrumBinFrequencyHz[static_cast<size_t> (candidate.bin)]);
        if (! canSelectFrequency (frequencyHz))
            return;

        float reductionDb = 0.0f;
        if (candidate.exceedDb <= redStartDb)
        {
            const float lowerSpan = juce::jmax (0.25f, redStartDb - warningStartDb);
            const float t = juce::jlimit (0.0f, 1.0f, (candidate.exceedDb - warningStartDb) / lowerSpan);
            reductionDb = juce::jmap (t, 0.0f, 3.0f);
        }
        else
        {
            reductionDb = 3.0f + (candidate.exceedDb - redStartDb) * 1.35f;
        }

        const int slot = selectedCount++;
        selectedFreqHz[static_cast<size_t> (slot)] = frequencyHz;
        targetFrequencyHz[static_cast<size_t> (slot)] = frequencyHz;
        targetGainDb[static_cast<size_t> (slot)] = -juce::jlimit (0.0f, 18.0f, reductionDb);
        targetQ[static_cast<size_t> (slot)] = juce::jlimit (2.0f, 14.0f, 4.0f + candidate.exceedDb * 1.1f);
    };

    for (int i = 0; i < candidateCount && selectedCount < resonanceSuppressorBands; ++i)
    {
        const auto& candidate = candidates[static_cast<size_t> (i)];
        assignCandidateToSlot (candidate);
    }

    const float blockDurationSec = static_cast<float> (numSamples / sampleRate);
    const bool hasWarningCandidates = selectedCount > 0;
    const float attackCoeff = std::exp (-blockDurationSec / 0.03f);
    const float releaseCoeff = std::exp (-blockDurationSec / (hasWarningCandidates ? 0.22f : 0.04f));
    const float paramCoeff = std::exp (-blockDurationSec / 0.10f);
    const float maxFrequency = juce::jmax (120.0f, static_cast<float> (sampleRate * 0.45));

    for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
    {
        auto& band = resonanceBands[static_cast<size_t> (bandIndex)];
        const float nextGainDb = targetGainDb[static_cast<size_t> (bandIndex)];
        const float gainCoeff = (nextGainDb < band.currentGainDb) ? attackCoeff : releaseCoeff;
        band.currentGainDb = gainCoeff * band.currentGainDb + (1.0f - gainCoeff) * nextGainDb;
        band.currentFrequencyHz = paramCoeff * band.currentFrequencyHz
            + (1.0f - paramCoeff) * juce::jlimit (40.0f, maxFrequency, targetFrequencyHz[static_cast<size_t> (bandIndex)]);
        band.currentQ = paramCoeff * band.currentQ
            + (1.0f - paramCoeff) * juce::jlimit (1.5f, 16.0f, targetQ[static_cast<size_t> (bandIndex)]);

        auto coeff = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
            sampleRate,
            band.currentFrequencyHz,
            band.currentQ,
            juce::Decibels::decibelsToGain (band.currentGainDb));

        for (auto& filter : band.filters)
            filter.coefficients = coeff;

        resonanceBandFrequencyUi[static_cast<size_t> (bandIndex)].store (band.currentFrequencyHz, std::memory_order_relaxed);
        resonanceBandGainUi[static_cast<size_t> (bandIndex)].store (band.currentGainDb, std::memory_order_relaxed);
    }
}

void DreamAudioProcessor::applyResonanceSuppressorToBuffer (juce::AudioBuffer<float>& buffer) noexcept
{
    const bool enabled = resonanceSuppressorEnabled.load (std::memory_order_relaxed);
    const bool hasReference = hasReferenceSpectrum.load (std::memory_order_relaxed);
    if (! enabled || ! hasReference)
    {
        for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
            resonanceBandGainUi[static_cast<size_t> (bandIndex)].store (0.0f, std::memory_order_relaxed);
        return;
    }

    const int channels = juce::jmin (2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    updateResonanceSuppressorTargets (samples);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* data = buffer.getWritePointer (ch);
        for (int i = 0; i < samples; ++i)
        {
            float sample = data[i];
            for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
            {
                const auto& band = resonanceBands[static_cast<size_t> (bandIndex)];
                if (band.currentGainDb < -0.05f)
                    sample = resonanceBands[static_cast<size_t> (bandIndex)].filters[static_cast<size_t> (ch)].processSample (sample);
            }
            data[i] = sample;
        }
    }
}

bool DreamAudioProcessor::buildSmoothPresetFromFolder (const juce::File& folder, juce::String& outMessage, int smoothingAmount)
{
    if (! folder.isDirectory())
    {
        outMessage = "Selected path is not a folder.";
        return false;
    }

    const int smoothingAmountClamped = juce::jlimit (0, 16, smoothingAmount);

    constexpr int maxAudioFilesToAnalyse = 160;
    constexpr int maxCandidateFilesToScan = 30000;
    constexpr double maxSecondsPerFileToAnalyse = 120.0;

    auto isSupportedAudioFile = [] (const juce::File& file)
    {
        const auto ext = file.getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg"
            || ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".wma";
    };

    juce::Array<juce::File> audioFiles;
    int candidateFilesScanned = 0;
    bool hitCandidateFileLimit = false;
    bool hitAudioFileLimit = false;

    for (const auto& entry : juce::RangedDirectoryIterator (folder,
                                                            true,
                                                            "*",
                                                            juce::File::findFiles,
                                                            juce::File::FollowSymlinks::no))
    {
        ++candidateFilesScanned;
        if (candidateFilesScanned > maxCandidateFilesToScan)
        {
            hitCandidateFileLimit = true;
            break;
        }

        const auto file = entry.getFile();
        if (isSupportedAudioFile (file))
        {
            audioFiles.add (file);
            if (audioFiles.size() >= maxAudioFilesToAnalyse)
            {
                hitAudioFileLimit = true;
                break;
            }
        }
    }

    if (audioFiles.isEmpty())
    {
        outMessage = "No supported audio files were found.";
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::array<double, spectrumBins> accumulatedPerFileAverage {};
    int filesAnalysed = 0;

    constexpr int analysisFftOrder = 12;
    constexpr int analysisFftSize = 1 << analysisFftOrder;
    constexpr int analysisLinearBins = (analysisFftSize / 2) + 1;
    constexpr int analysisHopSize = analysisFftSize / 4;

    juce::dsp::FFT localFft { analysisFftOrder };
    juce::dsp::WindowingFunction<float> localWindow {
        analysisFftSize,
        juce::dsp::WindowingFunction<float>::hann,
        true
    };
    std::array<float, analysisFftSize> localFifo {};
    std::array<float, analysisFftSize * 2> localFftData {};

    auto computeLocalMagnitudeScale = []()
    {
        std::array<float, analysisFftSize> windowTable {};
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            windowTable.data(),
            analysisFftSize,
            juce::dsp::WindowingFunction<float>::hann,
            true);

        double windowSum = 0.0;
        for (const auto w : windowTable)
            windowSum += static_cast<double> (w);

        const double coherentGain = windowSum / static_cast<double> (analysisFftSize);
        const double safeGain = juce::jmax (coherentGain, 1.0e-9);
        return static_cast<float> (2.0 / (static_cast<double> (analysisFftSize) * safeGain));
    };
    const float localMagnitudeScale = computeLocalMagnitudeScale();

    for (const auto& file : audioFiles)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            continue;

        const int channelsToRead = juce::jmax (1, juce::jmin (2, static_cast<int> (reader->numChannels)));
        constexpr int readBlockSize = 4096;
        juce::AudioBuffer<float> readBuffer (channelsToRead, readBlockSize);

        std::fill (localFifo.begin(), localFifo.end(), 0.0f);
        std::array<double, analysisLinearBins> fileLinearPowerAccum {};
        std::array<float, spectrumBins> fileCurve {};

        auto analyseFrame = [&]()
        {
            std::fill (localFftData.begin(), localFftData.end(), 0.0f);
            std::copy (localFifo.begin(), localFifo.end(), localFftData.begin());
            localWindow.multiplyWithWindowingTable (localFftData.data(), analysisFftSize);
            localFft.performFrequencyOnlyForwardTransform (localFftData.data());

            for (int i = 0; i < analysisLinearBins; ++i)
            {
                const float mag = juce::jmax (0.0f, localFftData[static_cast<size_t> (i)]) * localMagnitudeScale;
                fileLinearPowerAccum[static_cast<size_t> (i)] += static_cast<double> (mag * mag);
            }
        };

        int localFifoIndex = 0;
        std::int64_t position = 0;
        const auto fileSamplesToAnalyse = juce::jmin<std::int64_t> (
            reader->lengthInSamples,
            static_cast<std::int64_t> (reader->sampleRate * maxSecondsPerFileToAnalyse));
        int fileFramesAnalysed = 0;

        while (position < fileSamplesToAnalyse)
        {
            const int samplesToRead = static_cast<int> (
                juce::jmin<std::int64_t> (readBlockSize, fileSamplesToAnalyse - position));
            readBuffer.clear();
            reader->read (&readBuffer, 0, samplesToRead, position, true, true);

            const float* left = readBuffer.getReadPointer (0);
            const float* right = channelsToRead > 1 ? readBuffer.getReadPointer (1) : left;

            for (int i = 0; i < samplesToRead; ++i)
            {
                const float mono = channelsToRead > 1 ? 0.5f * (left[i] + right[i]) : left[i];
                localFifo[static_cast<size_t> (localFifoIndex)] = mono;
                ++localFifoIndex;

                if (localFifoIndex >= analysisFftSize)
                {
                    analyseFrame();
                    ++fileFramesAnalysed;

                    std::move (localFifo.begin() + analysisHopSize, localFifo.end(), localFifo.begin());
                    localFifoIndex = analysisFftSize - analysisHopSize;
                }
            }

            position += samplesToRead;
        }

        if (localFifoIndex > (analysisFftSize / 2))
        {
            for (int i = localFifoIndex; i < analysisFftSize; ++i)
                localFifo[static_cast<size_t> (i)] = 0.0f;
            analyseFrame();
            ++fileFramesAnalysed;
        }

        if (fileFramesAnalysed <= 0)
            continue;

        const float minFreq = 20.0f;
        const float maxFreq = juce::jlimit (minFreq + 1.0f, static_cast<float> (reader->sampleRate * 0.5), 20000.0f);
        const float ratio = maxFreq / minFreq;
        const float analysisBinHz = static_cast<float> (reader->sampleRate / static_cast<double> (analysisFftSize));

        for (int i = 0; i < spectrumBins; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (spectrumBins - 1);
            const float freq = minFreq * std::pow (ratio, t);
            const float binPos = freq / analysisBinHz;
            const float clamped = juce::jlimit (
                0.0f,
                static_cast<float> (analysisLinearBins - 1),
                binPos);
            const int idxA = static_cast<int> (clamped);
            const int idxB = juce::jmin (idxA + 1, analysisLinearBins - 1);
            const float frac = clamped - static_cast<float> (idxA);

            const double powA = fileLinearPowerAccum[static_cast<size_t> (idxA)] / static_cast<double> (fileFramesAnalysed);
            const double powB = fileLinearPowerAccum[static_cast<size_t> (idxB)] / static_cast<double> (fileFramesAnalysed);
            const double interpPower = juce::jmax (0.0, powA + (powB - powA) * static_cast<double> (frac));

            const float amplitude = static_cast<float> (std::sqrt (juce::jmax (interpPower, 1.0e-20)));
            const float dB = juce::Decibels::gainToDecibels (amplitude, -120.0f);
            fileCurve[static_cast<size_t> (i)] = juce::jlimit (
                0.0f,
                1.0f,
                juce::jmap (dB, -96.0f, 0.0f, 0.0f, 1.0f));
        }

        const int smoothingPasses = juce::jlimit (0, 40, smoothingAmountClamped * 2);
        const float targetSide = juce::jmap (
            static_cast<float> (smoothingAmountClamped),
            0.0f,
            16.0f,
            0.04f,
            0.34f);
        const float targetCenter = juce::jmax (0.08f, 1.0f - 2.0f * targetSide);
        const float normalizer = juce::jmax (1.0e-6f, targetCenter + 2.0f * targetSide);
        const float sideWeight = targetSide / normalizer;
        const float centerWeight = targetCenter / normalizer;

        auto smoothCurve = [sideWeight, centerWeight] (std::array<float, spectrumBins>& curve)
        {
            std::array<float, spectrumBins> work {};
            for (int i = 0; i < spectrumBins; ++i)
            {
                const int leftIdx = juce::jlimit (0, spectrumBins - 1, i - 1);
                const int rightIdx = juce::jlimit (0, spectrumBins - 1, i + 1);
                const float left = curve[static_cast<size_t> (leftIdx)];
                const float center = curve[static_cast<size_t> (i)];
                const float right = curve[static_cast<size_t> (rightIdx)];
                work[static_cast<size_t> (i)] = left * sideWeight + center * centerWeight + right * sideWeight;
            }
            curve = work;
        };

        for (int pass = 0; pass < smoothingPasses; ++pass)
            smoothCurve (fileCurve);

        for (int i = 0; i < spectrumBins; ++i)
            accumulatedPerFileAverage[static_cast<size_t> (i)] += static_cast<double> (fileCurve[static_cast<size_t> (i)]);

        ++filesAnalysed;
    }

    if (filesAnalysed <= 0)
    {
        outMessage = "No analyzable frames were produced from the selected files.";
        return false;
    }

    std::array<float, spectrumBins> averagedPerFile {};
    for (int i = 0; i < spectrumBins; ++i)
        averagedPerFile[static_cast<size_t> (i)] = static_cast<float> (
            accumulatedPerFileAverage[static_cast<size_t> (i)] / static_cast<double> (filesAnalysed));

    for (int i = 0; i < spectrumBins; ++i)
    {
        const float v = juce::jlimit (0.0f, 1.0f, averagedPerFile[static_cast<size_t> (i)]);
        referenceSpectrumData[static_cast<size_t> (i)].store (v, std::memory_order_relaxed);
    }

    hasReferenceSpectrum.store (true, std::memory_order_relaxed);
    referenceSpectrumRevision.fetch_add (1, std::memory_order_relaxed);

    juce::String truncationNote;
    if (hitAudioFileLimit || hitCandidateFileLimit)
        truncationNote = " (limited scan)";

    outMessage = "Smooth preset built from "
        + juce::String (filesAnalysed)
        + " file(s), smoothing "
        + juce::String (smoothingAmountClamped)
        + truncationNote
        + ".";
    return true;
}

bool DreamAudioProcessor::hasReferenceSpectrumData() const noexcept
{
    return hasReferenceSpectrum.load (std::memory_order_relaxed);
}

std::uint32_t DreamAudioProcessor::getReferenceSpectrumRevision() const noexcept
{
    return referenceSpectrumRevision.load (std::memory_order_relaxed);
}

void DreamAudioProcessor::clearReferenceSpectrum() noexcept
{
    for (auto& v : referenceSpectrumData)
        v.store (0.0f, std::memory_order_relaxed);
    hasReferenceSpectrum.store (false, std::memory_order_relaxed);
    referenceSpectrumRevision.fetch_add (1, std::memory_order_relaxed);
}

void DreamAudioProcessor::setReferenceSpectrumFromUi (const std::array<float, spectrumBins>& bins, bool hasData) noexcept
{
    bool incomingHasData = hasData;
    if (incomingHasData)
    {
        incomingHasData = std::any_of (bins.begin(), bins.end(),
                                       [] (float v) { return std::isfinite (v) && v > 1.0e-6f; });
    }

    const bool previousHasData = hasReferenceSpectrum.load (std::memory_order_relaxed);
    bool changed = (incomingHasData != previousHasData);

    std::array<float, spectrumBins> nextBins {};
    for (int i = 0; i < spectrumBins; ++i)
    {
        const float next = incomingHasData
            ? juce::jlimit (0.0f, 1.0f, std::isfinite (bins[static_cast<size_t> (i)]) ? bins[static_cast<size_t> (i)] : 0.0f)
            : 0.0f;
        nextBins[static_cast<size_t> (i)] = next;

        const float current = referenceSpectrumData[static_cast<size_t> (i)].load (std::memory_order_relaxed);
        if (! changed && std::abs (current - next) > 1.0e-5f)
            changed = true;
    }

    if (! changed)
        return;

    for (int i = 0; i < spectrumBins; ++i)
        referenceSpectrumData[static_cast<size_t> (i)].store (nextBins[static_cast<size_t> (i)], std::memory_order_relaxed);

    hasReferenceSpectrum.store (incomingHasData, std::memory_order_relaxed);
    referenceSpectrumRevision.fetch_add (1, std::memory_order_relaxed);
}

void DreamAudioProcessor::setResonanceSuppressorConfig (bool enabled,
                                                        float overlayLevelDb,
                                                        float overlayWidthDb,
                                                        float tiltDb) noexcept
{
    resonanceSuppressorEnabled.store (enabled, std::memory_order_relaxed);
    resonanceOverlayLevelDb.store (juce::jlimit (-23.0f, 0.0f, overlayLevelDb), std::memory_order_relaxed);
    resonanceOverlayWidthDb.store (juce::jlimit (3.0f, 18.0f, overlayWidthDb), std::memory_order_relaxed);
    resonanceOverlayTiltDb.store (juce::jlimit (-24.0f, 24.0f, tiltDb), std::memory_order_relaxed);

    if (! enabled)
    {
        for (int bandIndex = 0; bandIndex < resonanceSuppressorBands; ++bandIndex)
            resonanceBandGainUi[static_cast<size_t> (bandIndex)].store (0.0f, std::memory_order_relaxed);
    }
}

std::array<float, 6> DreamAudioProcessor::getResonanceSuppressorFrequencySnapshot() const noexcept
{
    std::array<float, 6> out {};
    for (int i = 0; i < resonanceSuppressorBands; ++i)
        out[static_cast<size_t> (i)] = resonanceBandFrequencyUi[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    return out;
}

std::array<float, 6> DreamAudioProcessor::getResonanceSuppressorGainSnapshot() const noexcept
{
    std::array<float, 6> out {};
    for (int i = 0; i < resonanceSuppressorBands; ++i)
        out[static_cast<size_t> (i)] = resonanceBandGainUi[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    return out;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DreamAudioProcessor();
}
