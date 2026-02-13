#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

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
    lufsHighPass.reset();
    lufsHighShelf.reset();
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
    double hostPpq = 0.0;
    double hostQuarterNotesPerBar = 4.0;
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            if (auto bpm = position->getBpm())
                currentTempoBpm.store (static_cast<float> (*bpm), std::memory_order_relaxed);
            if (auto ppq = position->getPpqPosition())
            {
                hostPpq = *ppq;
                hasHostPpq = true;
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
        const float oscSample = juce::jlimit (-1.0f, 1.0f, inL[i]);
        pushAnalyserSample (mono);

        const double phase = juce::jlimit (0.0, 0.999999, oscilloscopeQuarterPositionSamples / samplesPerCycle);
        const int bin = juce::jlimit (
            0,
            oscilloscopeSamples - 1,
            static_cast<int> (phase * static_cast<double> (oscilloscopeSamples)));

        oscilloscopeData[static_cast<size_t> (bin)].store (oscSample, std::memory_order_relaxed);
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

void DreamAudioProcessor::setOscilloscopeLengthMode (int mode) noexcept
{
    oscilloscopeLengthMode.store (mode == 0 ? 0 : 1, std::memory_order_relaxed);
}

int DreamAudioProcessor::getOscilloscopeLengthMode() const noexcept
{
    return oscilloscopeLengthMode.load (std::memory_order_relaxed);
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

bool DreamAudioProcessor::buildSmoothPresetFromFolder (const juce::File& folder, juce::String& outMessage)
{
    if (! folder.isDirectory())
    {
        outMessage = "Selected path is not a folder.";
        return false;
    }

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

    std::array<double, spectrumBins> accumulated {};
    std::int64_t totalFrames = 0;
    int filesAnalysed = 0;

    juce::dsp::FFT localFft { fftOrder };
    juce::dsp::WindowingFunction<float> localWindow { fftSize, juce::dsp::WindowingFunction<float>::hann, true };
    std::array<float, fftSize> localFifo {};
    std::array<float, fftSize * 2> localFftData {};
    const float localMagnitudeScale = computeFftMagnitudeScale();

    auto analyseFrame = [&] (const std::array<float, spectrumBins>& binPositions)
    {
        std::fill (localFftData.begin(), localFftData.end(), 0.0f);
        std::copy (localFifo.begin(), localFifo.end(), localFftData.begin());
        localWindow.multiplyWithWindowingTable (localFftData.data(), fftSize);
        localFft.performFrequencyOnlyForwardTransform (localFftData.data());

        const int maxIndex = (fftSize / 2) - 1;
        auto readMagnitude = [&] (float fftBin) -> float
        {
            const float clamped = juce::jlimit (1.0f, static_cast<float> (maxIndex - 1), fftBin);
            const int index = static_cast<int> (clamped);
            const float frac = clamped - static_cast<float> (index);
            const float magA = localFftData[static_cast<size_t> (index)];
            const float magB = localFftData[static_cast<size_t> (juce::jmin (index + 1, maxIndex))];
            return juce::jmax (0.0f, magA + frac * (magB - magA));
        };

        for (int i = 0; i < spectrumBins; ++i)
        {
            const float pos = binPositions[static_cast<size_t> (i)];
            const float centerMag = readMagnitude (pos);
            const float leftMag = readMagnitude (pos - 0.5f);
            const float rightMag = readMagnitude (pos + 0.5f);
            const float blendedMag = centerMag * 0.60f + leftMag * 0.20f + rightMag * 0.20f;

            const float scaledMag = blendedMag * localMagnitudeScale;
            const float dB = juce::Decibels::gainToDecibels (scaledMag, -120.0f);
            const float normalized = juce::jlimit (0.0f, 1.0f, juce::jmap (dB, -96.0f, 0.0f, 0.0f, 1.0f));
            accumulated[static_cast<size_t> (i)] += static_cast<double> (normalized);
        }

        ++totalFrames;
    };

    for (const auto& file : audioFiles)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            continue;

        const auto binPositions = buildSpectrumBinPositions (reader->sampleRate);
        const int channelsToRead = juce::jmax (1, juce::jmin (2, static_cast<int> (reader->numChannels)));
        constexpr int readBlockSize = 4096;
        juce::AudioBuffer<float> readBuffer (channelsToRead, readBlockSize);

        std::fill (localFifo.begin(), localFifo.end(), 0.0f);
        int localFifoIndex = 0;
        std::int64_t position = 0;
        const auto fileSamplesToAnalyse = juce::jmin<std::int64_t> (
            reader->lengthInSamples,
            static_cast<std::int64_t> (reader->sampleRate * maxSecondsPerFileToAnalyse));
        bool fileContributed = false;

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

                if (localFifoIndex >= fftSize)
                {
                    analyseFrame (binPositions);
                    localFifoIndex = 0;
                    fileContributed = true;
                }
            }

            position += samplesToRead;
        }

        if (localFifoIndex > (fftSize / 2))
        {
            for (int i = localFifoIndex; i < fftSize; ++i)
                localFifo[static_cast<size_t> (i)] = 0.0f;
            analyseFrame (binPositions);
            fileContributed = true;
        }

        if (fileContributed)
            ++filesAnalysed;
    }

    if (totalFrames <= 0)
    {
        outMessage = "No analyzable frames were produced from the selected files.";
        return false;
    }

    std::array<float, spectrumBins> averaged {};
    for (int i = 0; i < spectrumBins; ++i)
        averaged[static_cast<size_t> (i)] = static_cast<float> (accumulated[static_cast<size_t> (i)] / static_cast<double> (totalFrames));

    auto applySmoothingPass = [] (const std::array<float, spectrumBins>& input)
    {
        std::array<float, spectrumBins> output {};
        for (int i = 0; i < spectrumBins; ++i)
        {
            float weightedSum = 0.0f;
            float weightTotal = 0.0f;
            for (int offset = -4; offset <= 4; ++offset)
            {
                const int idx = juce::jlimit (0, spectrumBins - 1, i + offset);
                const float distance = static_cast<float> (offset * offset);
                const float weight = std::exp (-distance / 6.0f);
                weightedSum += input[static_cast<size_t> (idx)] * weight;
                weightTotal += weight;
            }
            output[static_cast<size_t> (i)] = weightTotal > 0.0f ? (weightedSum / weightTotal) : input[static_cast<size_t> (i)];
        }
        return output;
    };

    auto smoothed = applySmoothingPass (averaged);
    smoothed = applySmoothingPass (smoothed);

    for (int i = 0; i < spectrumBins; ++i)
    {
        const float v = juce::jlimit (0.0f, 1.0f, smoothed[static_cast<size_t> (i)]);
        referenceSpectrumData[static_cast<size_t> (i)].store (v, std::memory_order_relaxed);
    }

    hasReferenceSpectrum.store (true, std::memory_order_relaxed);
    referenceSpectrumRevision.fetch_add (1, std::memory_order_relaxed);

    juce::String truncationNote;
    if (hitAudioFileLimit || hitCandidateFileLimit)
        truncationNote = " (limited scan)";

    outMessage = "Smooth preset built from "
        + juce::String (filesAnalysed)
        + " file(s)." + truncationNote;
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DreamAudioProcessor();
}
