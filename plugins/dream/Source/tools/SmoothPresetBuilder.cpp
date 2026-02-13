#include <array>
#include <cmath>
#include <cstdint>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

namespace
{
constexpr int spectrumBins = 256;
constexpr int fftOrder = 11;
constexpr int fftSize = 1 << fftOrder;
constexpr int maxAudioFilesToAnalyse = 160;
constexpr int maxCandidateFilesToScan = 30000;
constexpr double maxSecondsPerFileToAnalyse = 120.0;

float computeFftMagnitudeScale()
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

std::array<float, spectrumBins> buildSpectrumBinPositions (double sampleRate) noexcept
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

struct PresetBuildResult
{
    bool success = false;
    int filesAnalysed = 0;
    bool hitAudioFileLimit = false;
    bool hitCandidateFileLimit = false;
    std::array<float, spectrumBins> bins {};
    juce::String message;
};

PresetBuildResult buildSmoothPresetFromFolder (const juce::File& folder)
{
    PresetBuildResult result;

    if (! folder.isDirectory())
    {
        result.message = "Selected path is not a folder.";
        return result;
    }

    auto isSupportedAudioFile = [] (const juce::File& file)
    {
        const auto ext = file.getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg"
            || ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".wma";
    };

    juce::Array<juce::File> audioFiles;
    int candidateFilesScanned = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (folder,
                                                            true,
                                                            "*",
                                                            juce::File::findFiles,
                                                            juce::File::FollowSymlinks::no))
    {
        ++candidateFilesScanned;
        if (candidateFilesScanned > maxCandidateFilesToScan)
        {
            result.hitCandidateFileLimit = true;
            break;
        }

        const auto file = entry.getFile();
        if (isSupportedAudioFile (file))
        {
            audioFiles.add (file);
            if (audioFiles.size() >= maxAudioFilesToAnalyse)
            {
                result.hitAudioFileLimit = true;
                break;
            }
        }
    }

    if (audioFiles.isEmpty())
    {
        result.message = "No supported audio files were found.";
        return result;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::array<double, spectrumBins> accumulated {};
    std::int64_t totalFrames = 0;

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
            ++result.filesAnalysed;
    }

    if (totalFrames <= 0)
    {
        result.message = "No analyzable frames were produced from the selected files.";
        return result;
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
        result.bins[static_cast<size_t> (i)] = juce::jlimit (0.0f, 1.0f, smoothed[static_cast<size_t> (i)]);

    result.success = true;
    result.message = "ok";
    return result;
}

void printResult (const juce::String& name, const juce::String& path, const PresetBuildResult& result)
{
    juce::String out;
    out << "{";
    out << "\"name\":\"" << name << "\",";
    out << "\"path\":" << juce::JSON::toString (juce::var (path)) << ",";
    out << "\"success\":" << (result.success ? "true" : "false") << ",";
    out << "\"filesAnalysed\":" << result.filesAnalysed << ",";
    out << "\"limited\":" << ((result.hitAudioFileLimit || result.hitCandidateFileLimit) ? "true" : "false") << ",";
    out << "\"message\":" << juce::JSON::toString (juce::var (result.message)) << ",";
    out << "\"bins\":[";

    for (int i = 0; i < spectrumBins; ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::String (result.bins[static_cast<size_t> (i)], 8);
    }

    out << "]}";
    std::cout << out << std::endl;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ignoreUnused (argc, argv);

    if (argc < 3 || ((argc - 1) % 2) != 0)
    {
        std::cout << "Usage: dream_smooth_preset_builder <name1> <folder1> [<name2> <folder2> ...]\n";
        return 1;
    }

    for (int i = 1; i + 1 < argc; i += 2)
    {
        const juce::String name = argv[i];
        const juce::String folderPath = argv[i + 1];
        const auto result = buildSmoothPresetFromFolder (juce::File (folderPath));
        printResult (name, folderPath, result);
    }

    return 0;
}
