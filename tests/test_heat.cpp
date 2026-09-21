// Heat is the redesign's signature interaction: the harder a band is driven,
// the more that part of the interface glows. It is only worth anything if the
// number actually tracks saturation rather than level, so these tests drive the
// real processor and read what the GUI would read.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "TestHelpers.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ParameterIDs.h"

using namespace ember;

namespace
{
/** Runs noise through the processor and returns band 0's settled heat. */
float heatAtDrive(EmberAudioProcessor& proc, float driveDb)
{
    auto& apvts = proc.getAPVTS();

    if (auto* p = apvts.getParameter(pid::drive(0)))
        p->setValueNotifyingHost(p->convertTo0to1(driveDb));

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng{0x4321};

    // Long enough for the drive smoother and the band's own smoothers to
    // settle; heat read mid-ramp would just measure the smoother.
    for (int block = 0; block < 120; ++block)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.25f);

        proc.processBlock(buffer, midi);
    }

    return proc.getBandHeat(0);
}
} // namespace

TEST_CASE("heat rises with drive", "[heat]")
{
    EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    const auto cold = heatAtDrive(proc, 0.0f);
    const auto warm = heatAtDrive(proc, 15.0f);
    const auto hot = heatAtDrive(proc, 35.0f);

    INFO("heat at 0 dB = " << cold << ", 15 dB = " << warm << ", 35 dB = " << hot);

    REQUIRE(cold >= 0.0f);
    REQUIRE(cold <= 1.0f);
    REQUIRE(warm > cold);
    REQUIRE(hot > warm);

    // The three renders the acceptance criteria ask for have to look different,
    // not merely differ in the last decimal.
    REQUIRE(hot - cold > 0.15f);
}

TEST_CASE("heat stays in range for every band and never goes non-finite", "[heat]")
{
    EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    auto& apvts = proc.getAPVTS();

    if (auto* p = apvts.getParameter(pid::numBands))
        p->setValueNotifyingHost(1.0f); // all six bands

    for (int b = 0; b < kMaxBands; ++b)
        if (auto* p = apvts.getParameter(pid::drive(b)))
            p->setValueNotifyingHost(p->convertTo0to1(30.0f));

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng{0x99};

    for (int block = 0; block < 200; ++block)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.4f);

        proc.processBlock(buffer, midi);

        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto h = proc.getBandHeat(b);
            INFO("band " << b << " heat " << h << " at block " << block);
            REQUIRE(std::isfinite(h));
            REQUIRE(h >= 0.0f);
            REQUIRE(h <= 1.0f);
        }
    }
}

TEST_CASE("silence does not make a band flicker", "[heat]")
{
    // Dividing two near-zero RMS values swings violently, which would show as a
    // silent plugin twitching. The measurement floors out instead.
    EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < 60; ++block)
    {
        buffer.clear();
        proc.processBlock(buffer, midi);
        REQUIRE(proc.getBandHeat(0) <= 0.05f);
    }
}

TEST_CASE("a bypassed band is cold however hard it is driven", "[heat]")
{
    EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    auto& apvts = proc.getAPVTS();

    if (auto* p = apvts.getParameter(pid::drive(0)))
        p->setValueNotifyingHost(p->convertTo0to1(35.0f));

    if (auto* p = apvts.getParameter(pid::bypass(0)))
        p->setValueNotifyingHost(1.0f);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng{0x7};

    for (int block = 0; block < 80; ++block)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.3f);

        proc.processBlock(buffer, midi);
    }

    REQUIRE(proc.getBandHeat(0) <= 0.05f);
}
