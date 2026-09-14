// Offline CPU benchmark. Not a unit test: it prints a realtime-factor report
// that CI captures, and fails only if the plugin cannot keep up with realtime.
#include <juce_dsp/juce_dsp.h>
#include <chrono>
#include <cstdio>
#include "dsp/EmberEngine.h"

using namespace ember;

namespace
{
struct Result
{
    double secondsOfAudio;
    double secondsElapsed;
    double percentOfOneCore;
};

Result run(int numBands, OversamplingFactor os, double sampleRate, int blockSize, double seconds)
{
    EmberEngine engine;
    engine.prepare({ sampleRate, static_cast<juce::uint32>(blockSize), 2 });
    engine.setOversamplingFactor(os);

    std::array<BandParams, kMaxBands> bands;
    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& p = bands[static_cast<size_t>(b)];
        p.style = static_cast<StyleID>(b % kNumStyles);
        p.driveDb = 18.0f;
        p.mix01 = 1.0f;
        p.feedback01 = 0.3f;
        p.feedbackFreq = 250.0f;
        p.dynamics = 0.4f;
        p.toneLowDb = 2.0f;
        p.toneHighDb = -2.0f;
    }

    GlobalParams g;
    g.numBands = numBands;
    g.oversampling = os;
    g.autoGain = true;
    engine.setParameters(g, bands.data(), kMaxBands);

    juce::AudioBuffer<float> buf(2, blockSize);
    uint32_t s = 0x1234u;
    auto noise = [&s]() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f * 0.25f;
    };

    const int totalBlocks = static_cast<int>((seconds * sampleRate) / blockSize);

    // Warm up so caches and smoothers are settled before timing.
    for (int i = 0; i < 50; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < blockSize; ++j) buf.setSample(ch, j, noise());
        engine.process(buf);
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < totalBlocks; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < blockSize; ++j) buf.setSample(ch, j, noise());
        engine.process(buf);
    }
    const auto end = std::chrono::steady_clock::now();

    const double elapsed = std::chrono::duration<double>(end - start).count();
    const double audio = (totalBlocks * static_cast<double>(blockSize)) / sampleRate;
    return { audio, elapsed, 100.0 * elapsed / audio };
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("\nEmber offline CPU benchmark\n");
    std::printf("---------------------------------------------------------------\n");
    std::printf("%-42s %10s\n", "configuration", "%% of core");

    struct Case { const char* name; int bands; OversamplingFactor os; double rate; int block; };
    const Case cases[] = {
        { "stereo 48k, 6 bands, 4x  (spec target <= 3%)", 6, OversamplingFactor::x4,  48000.0, 512 },
        { "stereo 48k, 6 bands, 2x",                      6, OversamplingFactor::x2,  48000.0, 512 },
        { "stereo 48k, 6 bands, off",                     6, OversamplingFactor::Off, 48000.0, 512 },
        { "stereo 48k, 3 bands, 4x",                      3, OversamplingFactor::x4,  48000.0, 512 },
        { "stereo 96k, 6 bands, 4x",                      6, OversamplingFactor::x4,  96000.0, 512 },
        { "stereo 48k, 6 bands, 16x",                     6, OversamplingFactor::x16, 48000.0, 512 },
        { "stereo 48k, 6 bands, 4x, 64-sample blocks",    6, OversamplingFactor::x4,  48000.0, 64  },
    };

    double headline = 0.0;
    for (const auto& c : cases)
    {
        const auto r = run(c.bands, c.os, c.rate, c.block, 10.0);
        std::printf("%-42s %9.2f%%\n", c.name, r.percentOfOneCore);
        if (headline == 0.0) headline = r.percentOfOneCore;
    }
    std::printf("---------------------------------------------------------------\n");
    std::printf("headline (6 bands, 4x, 48k): %.2f%% of one core\n\n", headline);

    // Fail only if we cannot sustain realtime at all — the 3% spec target is
    // reported rather than enforced, because CI runners vary by an order of
    // magnitude and a hard gate there would be noise, not signal.
    if (headline >= 100.0)
    {
        std::printf("FAIL: cannot sustain realtime\n");
        return 1;
    }
    return 0;
}
