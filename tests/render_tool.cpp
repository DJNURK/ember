// Renders test signals through the Ember engine and writes WAVs to
// test-renders/, so the manual checklist in docs/TESTING.md can be completed
// against real audio instead of by ear alone. Also prints a short analysis of
// each render so CI logs carry the numbers.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include "dsp/EmberEngine.h"

using namespace ember;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

void writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat format;
    // JUCE 8 takes the stream by reference to a unique_ptr and claims ownership
    // only on success, so the local must be an OutputStream pointer.
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream().release());
    if (stream == nullptr)
        return;

    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate(sampleRate)
                             .withNumChannels(buffer.getNumChannels())
                             .withBitsPerSample(24);

    if (auto writer = format.createWriterFor(stream, options))
        writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

void makeSweep(juce::AudioBuffer<float>& buf, double sampleRate, double f0, double f1)
{
    const int n = buf.getNumSamples();
    const double k = std::log(f1 / f0) / n;
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f = f0 * std::exp(k * i);
        phase += juce::MathConstants<double>::twoPi * f / sampleRate;
        const float v = 0.5f * static_cast<float>(std::sin(phase));
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.setSample(ch, i, v);
    }
}

void makeDrumLikeImpulses(juce::AudioBuffer<float>& buf, double sampleRate)
{
    buf.clear();
    const int n = buf.getNumSamples();
    for (int hit = 0; hit * static_cast<int>(sampleRate * 0.25) < n; ++hit)
    {
        const int start = hit * static_cast<int>(sampleRate * 0.25);
        const int len = juce::jmin(static_cast<int>(sampleRate * 0.2), n - start);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sampleRate;
            const float env = static_cast<float>(std::exp(-t * 25.0));
            const float body = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 65.0 * t));
            const float snap = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 1800.0 * t))
                             * static_cast<float>(std::exp(-t * 180.0));
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                buf.setSample(ch, start + i, 0.7f * env * (body + 0.4f * snap));
        }
    }
}

float peakOf(const juce::AudioBuffer<float>& b)
{
    float p = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i) p = juce::jmax(p, std::abs(b.getSample(ch, i)));
    return p;
}

float rmsOf(const juce::AudioBuffer<float>& b)
{
    double acc = 0.0;
    int count = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i) { acc += static_cast<double>(b.getSample(ch, i)) * b.getSample(ch, i); ++count; }
    return static_cast<float>(std::sqrt(acc / juce::jmax(1, count)));
}

/** Returns the engine latency used for the render, so callers can align the
    result against the source. */
int renderThrough(const juce::AudioBuffer<float>& source, juce::AudioBuffer<float>& dest,
                  const GlobalParams& g, const std::array<BandParams, kMaxBands>& bands)
{
    EmberEngine engine;
    engine.prepare({ kRate, static_cast<juce::uint32>(kBlock), 2 });
    engine.setOversamplingFactor(g.oversampling);
    engine.setCrossoverMode(g.crossoverMode);
    engine.setParameters(g, bands.data(), kMaxBands);

    dest.setSize(source.getNumChannels(), source.getNumSamples());
    dest.makeCopyOf(source);

    juce::AudioBuffer<float> chunk(source.getNumChannels(), kBlock);
    for (int pos = 0; pos < dest.getNumSamples(); pos += kBlock)
    {
        const int n = juce::jmin(kBlock, dest.getNumSamples() - pos);
        chunk.clear();
        for (int ch = 0; ch < dest.getNumChannels(); ++ch)
            chunk.copyFrom(ch, 0, dest, ch, pos, n);
        juce::AudioBuffer<float> view(chunk.getArrayOfWritePointers(), chunk.getNumChannels(), n);
        engine.process(view);
        for (int ch = 0; ch < dest.getNumChannels(); ++ch)
            dest.copyFrom(ch, pos, chunk, ch, 0, n);
    }
    return engine.getLatencySamples();
}

/** Residual of `processed` against `source` delayed by `latency`, in dB
    relative to the source. This is what "transparent at unity settings"
    actually means, and it is worth measuring rather than asserting. */
double nullDepthDb(const juce::AudioBuffer<float>& source, const juce::AudioBuffer<float>& processed,
                   int latency)
{
    const int start = latency + 4096;
    const int count = source.getNumSamples() - start - 4096;
    if (count <= 0)
        return 0.0;

    double residual = 0.0, reference = 0.0;
    for (int ch = 0; ch < source.getNumChannels(); ++ch)
    {
        for (int i = 0; i < count; ++i)
        {
            const double ref = source.getSample(ch, start + i - latency);
            const double d = processed.getSample(ch, start + i) - ref;
            residual += d * d;
            reference += ref * ref;
        }
    }
    return 10.0 * std::log10((residual / juce::jmax(1.0e-30, reference)) + 1.0e-30);
}
} // namespace

int main(int argc, char** argv)
{
    juce::File outDir = argc > 1 ? juce::File(juce::String(argv[1]))
                                 : juce::File::getCurrentWorkingDirectory().getChildFile("test-renders");
    outDir.createDirectory();

    const int seconds = 4;
    const int n = static_cast<int>(kRate) * seconds;

    juce::AudioBuffer<float> sweep(2, n), drums(2, n), noise(2, n);
    makeSweep(sweep, kRate, 20.0, 20000.0);
    makeDrumLikeImpulses(drums, kRate);
    {
        uint32_t s = 0xC0DEu;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                noise.setSample(ch, i, static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f * 0.25f);
            }
    }

    writeWav(outDir.getChildFile("00-source-sweep.wav"), sweep, kRate);
    writeWav(outDir.getChildFile("00-source-drums.wav"), drums, kRate);
    writeWav(outDir.getChildFile("00-source-noise.wav"), noise, kRate);

    std::printf("\n%-40s %10s %10s %12s\n", "render", "peak", "rms dB", "null vs dry");
    std::printf("-------------------------------------------------------------\n");

    auto renderCase = [&](const char* name, const juce::AudioBuffer<float>& src,
                          const GlobalParams& g, const std::array<BandParams, kMaxBands>& bands,
                          bool measureNull = false)
    {
        juce::AudioBuffer<float> out;
        const int latency = renderThrough(src, out, g, bands);
        writeWav(outDir.getChildFile(juce::String(name) + ".wav"), out, kRate);

        if (measureNull)
            std::printf("%-40s %10.3f %10.2f %9.1f dB\n", name, peakOf(out),
                        juce::Decibels::gainToDecibels(rmsOf(out) + 1.0e-12f),
                        nullDepthDb(src, out, latency));
        else
            std::printf("%-40s %10.3f %10.2f\n", name, peakOf(out),
                        juce::Decibels::gainToDecibels(rmsOf(out) + 1.0e-12f));
    };

    // 1. Transparency of the dry path through the crossover.
    //
    //    Note what this does and does not claim. Drive at 0 dB is NOT a bypass:
    //    every style still applies its transfer curve, so a "unity" render is
    //    the sound of that style at its gentlest, not the input. What must be
    //    transparent is the band split itself, which is measured by taking all
    //    band mixes fully dry and nulling against the latency-aligned input.
    //
    //    In linear-phase mode that null should be very deep. In minimum-phase
    //    mode it will NOT be, and that is correct rather than a defect: a
    //    Linkwitz-Riley crossover sums to an allpass, so the magnitude is flat
    //    but the phase is rotated. The magnitude flatness is what
    //    tests/test_crossover.cpp asserts to 0.01 dB.
    {
        GlobalParams g; g.numBands = 4; g.oversampling = OversamplingFactor::x2;
        g.crossoverMode = CrossoverMode::LinearPhase;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands) { p.driveDb = 0.0f; p.mix01 = 0.0f; }
        renderCase("01-crossover-dry-linearphase-noise", noise, g, bands, true);

        g.crossoverMode = CrossoverMode::MinimumPhaseLR4;
        renderCase("01-crossover-dry-minphase-noise", noise, g, bands, true);
    }

    // 2. Gentlest setting of the default style, for reference by ear.
    {
        GlobalParams g; g.numBands = 4; g.oversampling = OversamplingFactor::x2;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands) { p.driveDb = 0.0f; p.mix01 = 1.0f; }
        renderCase("01-lowest-drive-sweep", sweep, g, bands);
        renderCase("01-lowest-drive-noise", noise, g, bands);
    }

    // 2. One render per saturation style, single band, moderate drive.
    for (int i = 0; i < kNumStyles; ++i)
    {
        GlobalParams g; g.numBands = 1; g.oversampling = OversamplingFactor::x4;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands) { p.style = static_cast<StyleID>(i); p.driveDb = 18.0f; p.mix01 = 1.0f; }
        juce::String name = juce::String::formatted("02-style-%02d-", i)
                          + juce::String(getStyleName(static_cast<StyleID>(i))).replaceCharacter(' ', '-').toLowerCase();
        renderCase(name.toRawUTF8(), drums, g, bands);
    }

    // 3. Feedback character sweep.
    for (float fb : { 0.3f, 0.6f, 0.9f })
    {
        GlobalParams g; g.numBands = 3; g.oversampling = OversamplingFactor::x4;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands)
        {
            p.style = StyleID::WarmTube; p.driveDb = 20.0f; p.feedback01 = fb; p.feedbackFreq = 320.0f;
        }
        renderCase(juce::String::formatted("03-feedback-%02d", static_cast<int>(fb * 100)).toRawUTF8(),
                   drums, g, bands);
    }

    // 4. Dynamics either side of zero.
    for (float dyn : { -1.0f, -0.5f, 0.5f, 1.0f })
    {
        GlobalParams g; g.numBands = 3;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands) { p.style = StyleID::CleanTape; p.driveDb = 10.0f; p.dynamics = dyn; }
        renderCase(juce::String::formatted("04-dynamics-%+03d", static_cast<int>(dyn * 100)).toRawUTF8(),
                   drums, g, bands);
    }

    // 5. Mid/side mode and auto-gain.
    {
        GlobalParams g; g.numBands = 3; g.stereoMode = StereoMode::MidSide; g.autoGain = true;
        std::array<BandParams, kMaxBands> bands;
        for (auto& p : bands) { p.style = StyleID::Transformer; p.driveDb = 24.0f; }
        renderCase("05-midside-autogain", drums, g, bands);
    }

    std::printf("-------------------------------------------------------------\n");
    std::printf("wrote renders to %s\n\n", outDir.getFullPathName().toRawUTF8());
    return 0;
}
