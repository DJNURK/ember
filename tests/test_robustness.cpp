#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/EmberEngine.h"
#include "plugin/PluginProcessor.h"

using namespace ember;
using namespace embertest;

TEST_CASE("processBlock survives a buffer larger than the prepared maximum", "[robustness]")
{
    // maximumExpectedSamplesPerBlock is a hint, not a contract: hosts do hand
    // over more than they promised and validators deliberately do. Worse, a host
    // may prepare a maximum SMALLER than the 32-sample control block the
    // processor splits into, in which case the very first chunk overruns every
    // buffer the engine sized. That was a hard segfault.
    for (int prepared : {1, 8, 16, 31, 32, 64})
    {
        for (int actual : {1, 32, 64, 512, 2048})
        {
            EmberAudioProcessor proc;
            proc.prepareToPlay(44100.0, prepared);

            juce::AudioBuffer<float> buf(2, actual);
            fillWhiteNoise(buf, 0x9001u + static_cast<uint32_t>(actual), 0.3f);
            juce::MidiBuffer midi;

            INFO("prepared for " << prepared << ", called with " << actual);
            REQUIRE_NOTHROW(proc.processBlock(buf, midi));
            REQUIRE(allFinite(buf));
        }
    }
}

TEST_CASE("a single non-finite input sample does not disable the plugin", "[robustness]")
{
    // Everything downstream of the input holds recursive state - the
    // oversampler's half-band filters, the tone stack's IIRs, the feedback delay
    // - and none of it can recover on its own once its history is NaN. Before
    // this was guarded, one bad sample from a host dropped the output to -169 dB
    // and it never came back for the life of the instance.
    auto steadyRms = [](bool injectNaN)
    {
        EmberEngine engine;
        engine.prepare({44100.0, 32u, 2u});

        GlobalParams g;
        g.numBands = 1;
        g.autoGain = true;

        std::array<BandParams, kMaxBands> bands{};
        for (auto& p : bands)
            p.driveDb = 30.0f; // enough drive that auto-gain has real work to do

        double sumSq = 0.0;
        int counted = 0;

        for (int block = 0; block < 2000; ++block)
        {
            engine.setParameters(g, bands.data(), kMaxBands);

            juce::AudioBuffer<float> buf(2, 32);
            for (int i = 0; i < 32; ++i)
            {
                const auto t = static_cast<float>(block * 32 + i);
                const float v = 0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * t / 44100.0f);
                buf.getWritePointer(0)[i] = v;
                buf.getWritePointer(1)[i] = v;
            }

            if (injectNaN && block == 50)
                buf.getWritePointer(0)[7] = std::nanf("");

            engine.process(buf);

            if (block >= 1500) // measure only once well past the injection
                for (int i = 0; i < 32; ++i)
                {
                    const double o = buf.getSample(0, i);
                    sumSq += o * o;
                    ++counted;
                }
        }
        return static_cast<float>(std::sqrt(sumSq / juce::jmax(1, counted)));
    };

    const float clean = steadyRms(false);
    const float afterNaN = steadyRms(true);

    REQUIRE(clean > 0.01f);
    const float deviationDb =
        juce::Decibels::gainToDecibels(juce::jmax(1.0e-9f, afterNaN) / juce::jmax(1.0e-9f, clean));

    INFO("steady RMS: clean " << clean << ", after one NaN " << afterNaN << " (" << deviationDb << " dB)");
    REQUIRE(std::abs(deviationDb) < 1.0f);
}

TEST_CASE("a non-finite sample injected mid-session recovers through the plugin", "[robustness]")
{
    EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    juce::MidiBuffer midi;

    juce::AudioBuffer<float> buf(2, 256);
    auto runBlock = [&](bool poison)
    {
        fillWhiteNoise(buf, 0x4242u, 0.3f);
        if (poison)
        {
            buf.getWritePointer(0)[13] = std::numeric_limits<float>::infinity();
            buf.getWritePointer(1)[200] = std::nanf("");
        }
        proc.processBlock(buf, midi);
        return rms(buf, 0);
    };

    for (int i = 0; i < 50; ++i)
        runBlock(false);
    const float before = runBlock(false);

    runBlock(true); // the bad block

    for (int i = 0; i < 100; ++i)
        runBlock(false);
    const float after = runBlock(false);

    REQUIRE(before > 1.0e-4f);
    INFO("RMS before " << before << ", after recovery " << after);
    REQUIRE(after > before * 0.5f);
    REQUIRE(allFinite(buf));
}
