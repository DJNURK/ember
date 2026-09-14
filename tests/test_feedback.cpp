#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/BandFx.h"
#include "dsp/BandChain.h"

using namespace ember;
using namespace embertest;

TEST_CASE("feedback loop stays bounded for 60 s of full-scale noise", "[feedback][stability]")
{
    // The spec's stability gate. The feedback path is a resonant loop, so an
    // under-damped design diverges slowly and only shows up after tens of
    // seconds — which is exactly why this test runs a full minute rather than a
    // few blocks, and sweeps the worst-case settings.
    constexpr double rate = 48000.0;
    constexpr int block = 512;
    const int totalBlocks = static_cast<int>((60.0 * rate) / block);

    for (float amount : { 1.0f, 0.95f, 0.75f })
    {
        for (float freq : { 20.0f, 200.0f, 2000.0f })
        {
            FeedbackLoop fb;
            fb.prepare(rate, block, 2);
            fb.reset();
            fb.setParameters(amount, freq);

            juce::AudioBuffer<float> buf(2, block);
            float worst = 0.0f;

            for (int i = 0; i < totalBlocks; ++i)
            {
                fillWhiteNoise(buf, 0x1000u + static_cast<uint32_t>(i), 1.0f);
                fb.process(buf.getArrayOfWritePointers(), 2, block);
                if (! allFinite(buf))
                {
                    INFO("non-finite output at amount " << amount << ", freq " << freq
                         << ", block " << i);
                    REQUIRE(false);
                }
                worst = juce::jmax(worst, peak(buf));
            }

            INFO("amount = " << amount << ", freq = " << freq << ", peak = " << worst);
            REQUIRE(worst < 8.0f);
            REQUIRE(std::isfinite(worst));
        }
    }
}

TEST_CASE("feedback at zero is a true bypass", "[feedback]")
{
    constexpr double rate = 48000.0;
    constexpr int block = 512;

    FeedbackLoop fb;
    fb.prepare(rate, block, 2);
    fb.reset();
    fb.setParameters(0.0f, 200.0f);

    juce::AudioBuffer<float> buf(2, block), copy(2, block);
    fillWhiteNoise(buf, 0x2222u, 0.5f);
    copy.makeCopyOf(buf);

    fb.process(buf.getArrayOfWritePointers(), 2, block);

    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < block; ++i)
            maxDiff = juce::jmax(maxDiff, std::abs(buf.getSample(ch, i) - copy.getSample(ch, i)));
    REQUIRE(maxDiff < 1.0e-7f);
}

TEST_CASE("the whole band chain stays bounded under sustained abuse", "[feedback][stability]")
{
    // Feedback plus a destructive style plus maximum drive is the worst
    // combination the plugin can be put in; it must still not run away.
    constexpr double rate = 48000.0;
    constexpr int block = 256;
    const int totalBlocks = static_cast<int>((20.0 * rate) / block);

    BandChain chain;
    chain.prepare(rate, block, 2, OversamplingFactor::x4);
    chain.reset();

    BandParams p;
    p.style = StyleID::Foldback;
    p.driveDb = 40.0f;
    p.feedback01 = 1.0f;
    p.feedbackFreq = 80.0f;
    p.dynamics = 1.0f;
    p.toneLowDb = 12.0f;
    p.toneHighDb = 12.0f;
    p.levelDb = 24.0f;
    chain.setParameters(p);

    juce::AudioBuffer<float> buf(2, block);
    float worst = 0.0f;
    for (int i = 0; i < totalBlocks; ++i)
    {
        fillWhiteNoise(buf, 0x3333u + static_cast<uint32_t>(i), 1.0f);
        chain.process(buf, block);
        REQUIRE(allFinite(buf));
        worst = juce::jmax(worst, peak(buf));
    }
    INFO("worst peak through the full chain: " << worst);
    REQUIRE(worst < 64.0f);
}
