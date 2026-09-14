#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

namespace embertest
{
/** Deterministic white noise — a fixed seed so every test run and every machine
    sees the same signal and failures are reproducible. */
inline void fillWhiteNoise(juce::AudioBuffer<float>& buf, uint32_t seed = 0x1234567u, float amplitude = 0.25f)
{
    uint32_t s = seed;
    auto next = [&s]() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f;
    };
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        s = seed + static_cast<uint32_t>(ch) * 7919u;
        auto* d = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            d[i] = next() * amplitude;
    }
}

inline void fillSine(juce::AudioBuffer<float>& buf, double sampleRate, double freq, float amplitude = 0.5f)
{
    const double w = juce::MathConstants<double>::twoPi * freq / sampleRate;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        auto* d = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            d[i] = amplitude * static_cast<float>(std::sin(w * i));
    }
}

inline float rms(const juce::AudioBuffer<float>& buf, int channel = 0, int start = 0, int num = -1)
{
    if (num < 0) num = buf.getNumSamples() - start;
    if (num <= 0) return 0.0f;
    double acc = 0.0;
    const auto* d = buf.getReadPointer(channel);
    for (int i = 0; i < num; ++i) acc += static_cast<double>(d[start + i]) * d[start + i];
    return static_cast<float>(std::sqrt(acc / num));
}

inline float peak(const juce::AudioBuffer<float>& buf)
{
    float p = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            p = juce::jmax(p, std::abs(buf.getSample(ch, i)));
    return p;
}

inline bool allFinite(const juce::AudioBuffer<float>& buf)
{
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            if (! std::isfinite(buf.getSample(ch, i))) return false;
    return true;
}

/** Magnitude spectrum in dB of `signal`, length must be a power of two. */
inline std::vector<float> magnitudeSpectrumDb(const float* signal, int fftSize, bool applyWindow = true)
{
    const int order = static_cast<int>(std::round(std::log2(static_cast<double>(fftSize))));
    juce::dsp::FFT fft(order);
    std::vector<float> scratch(static_cast<size_t>(fftSize) * 2, 0.0f);
    for (int i = 0; i < fftSize; ++i)
    {
        const float w = applyWindow
            ? 0.5f * (1.0f - std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i)
                                      / static_cast<float>(fftSize - 1)))
            : 1.0f;
        scratch[static_cast<size_t>(i)] = signal[i] * w;
    }
    fft.performFrequencyOnlyForwardTransform(scratch.data(), true);

    std::vector<float> out(static_cast<size_t>(fftSize / 2));
    for (int i = 0; i < fftSize / 2; ++i)
        out[static_cast<size_t>(i)] =
            juce::Decibels::gainToDecibels(scratch[static_cast<size_t>(i)] + 1.0e-20f);
    return out;
}

/** Transfer function of a process, measured by comparing output and input
    spectra of the same noise burst. Returns dB per bin. */
inline std::vector<float> transferFunctionDb(const float* input, const float* output, int fftSize)
{
    auto a = magnitudeSpectrumDb(input, fftSize);
    auto b = magnitudeSpectrumDb(output, fftSize);
    std::vector<float> out(a.size());
    for (size_t i = 0; i < a.size(); ++i)
        out[i] = b[i] - a[i];
    return out;
}
} // namespace embertest
