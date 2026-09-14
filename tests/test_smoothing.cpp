#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/EmberEngine.h"

using namespace ember;
using namespace embertest;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

GlobalParams defaultGlobal(int numBands = 4)
{
    GlobalParams g;
    g.numBands = numBands;
    g.oversampling = OversamplingFactor::x2;
    return g;
}
} // namespace

TEST_CASE("slamming parameters over silence produces no audible click", "[smoothing]")
{
    // The spec's smoothing gate: automate every continuous parameter as
    // violently as a host can, with silence going in, and nothing above
    // -60 dBFS may come out. Any zipper noise or discontinuity shows up here.
    EmberEngine engine;
    engine.prepare({ kRate, static_cast<juce::uint32>(kBlock), 2 });

    std::array<BandParams, kMaxBands> bands;
    auto global = defaultGlobal();

    juce::AudioBuffer<float> buf(2, kBlock);
    float worst = 0.0f;

    for (int step = 0; step < 400; ++step)
    {
        // Alternate every parameter between its extremes on every block.
        const bool hi = (step % 2) == 0;
        for (int b = 0; b < kMaxBands; ++b)
        {
            auto& p = bands[static_cast<size_t>(b)];
            p.driveDb = hi ? 40.0f : 0.0f;
            p.mix01 = hi ? 1.0f : 0.0f;
            p.levelDb = hi ? 24.0f : -24.0f;
            p.pan = hi ? 1.0f : -1.0f;
            p.width01 = hi ? 2.0f : 0.0f;
            p.feedback01 = hi ? 1.0f : 0.0f;
            p.feedbackFreq = hi ? 2000.0f : 20.0f;
            p.dynamics = hi ? 1.0f : -1.0f;
            p.toneLowDb = hi ? 12.0f : -12.0f;
            p.toneMidDb = hi ? -12.0f : 12.0f;
            p.toneHighDb = hi ? 12.0f : -12.0f;
        }
        global.inputGainDb = hi ? 24.0f : -24.0f;
        global.outputGainDb = hi ? 24.0f : -24.0f;
        global.mix01 = hi ? 1.0f : 0.0f;

        engine.setParameters(global, bands.data(), kMaxBands);

        buf.clear();                       // silence in
        engine.process(buf);

        REQUIRE(allFinite(buf));
        worst = juce::jmax(worst, peak(buf));
    }

    const float worstDb = juce::Decibels::gainToDecibels(worst + 1.0e-12f);
    INFO("loudest artefact while slamming parameters over silence: " << worstDb << " dBFS");
    REQUIRE(worstDb < -60.0f);
}

TEST_CASE("changing style over silence produces no click", "[smoothing]")
{
    EmberEngine engine;
    engine.prepare({ kRate, static_cast<juce::uint32>(kBlock), 2 });

    std::array<BandParams, kMaxBands> bands;
    auto global = defaultGlobal(3);

    juce::AudioBuffer<float> buf(2, kBlock);
    float worst = 0.0f;

    for (int i = 0; i < kNumStyles * 4; ++i)
    {
        for (auto& p : bands)
        {
            p.style = static_cast<StyleID>(i % kNumStyles);
            p.driveDb = 20.0f;
        }
        engine.setParameters(global, bands.data(), kMaxBands);
        buf.clear();
        engine.process(buf);
        REQUIRE(allFinite(buf));
        worst = juce::jmax(worst, peak(buf));
    }

    const float worstDb = juce::Decibels::gainToDecibels(worst + 1.0e-12f);
    INFO("loudest artefact while sweeping styles over silence: " << worstDb << " dBFS");
    REQUIRE(worstDb < -60.0f);
}

TEST_CASE("changing band count is click-free on sustained material", "[smoothing]")
{
    // Band-count changes are the hardest click to avoid because the band
    // boundaries themselves move. The engine crossfades the crossover outputs
    // so the sum stays continuous; this test looks for a step discontinuity in
    // the output of a steady sine while the count changes underneath it.
    EmberEngine engine;
    engine.prepare({ kRate, static_cast<juce::uint32>(kBlock), 2 });

    std::array<BandParams, kMaxBands> bands;
    for (auto& p : bands) { p.driveDb = 0.0f; p.mix01 = 1.0f; }

    auto global = defaultGlobal(1);
    engine.setParameters(global, bands.data(), kMaxBands);

    juce::AudioBuffer<float> buf(2, kBlock);
    double phase = 0.0;
    const double w = juce::MathConstants<double>::twoPi * 1000.0 / kRate;

    float prevSample = 0.0f;
    float worstJump = 0.0f;
    bool started = false;

    for (int step = 0; step < 200; ++step)
    {
        if (step % 20 == 0)
        {
            global.numBands = 1 + (step / 20) % kMaxBands;
            engine.setParameters(global, bands.data(), kMaxBands);
        }

        for (int i = 0; i < kBlock; ++i)
        {
            const float v = 0.5f * static_cast<float>(std::sin(phase));
            phase += w;
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }

        engine.process(buf);
        REQUIRE(allFinite(buf));

        if (step > 20)   // let latency and startup transients pass
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = buf.getSample(0, i);
                if (started)
                    worstJump = juce::jmax(worstJump, std::abs(s - prevSample));
                prevSample = s;
                started = true;
            }
        }
    }

    // A 1 kHz sine at 48 kHz moves at most ~0.066 per sample at 0.5 amplitude;
    // allow generous headroom for the crossover's phase response but still
    // catch a real step discontinuity.
    INFO("worst sample-to-sample jump across band-count changes: " << worstJump);
    REQUIRE(worstJump < 0.25f);
}
