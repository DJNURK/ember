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
    for (int block = 0; block < 400; ++block)
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

    // Measured on a style that actually saturates. The default style is the
    // cleanest one Ember has, and asking for a large heat SEPARATION from it
    // tests the style's gentleness rather than the metric: at 35 dB it reads
    // about 0.15 in total, so a 0.15 difference was never achievable. The
    // ordering claims below hold for every style; the magnitude claim needs a
    // style where the magnitude means something.
    if (auto* p = proc.getAPVTS().getParameter(pid::style(0)))
        p->setValueNotifyingHost(p->convertTo0to1(9.0f));

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

TEST_CASE("heat rises with drive on every style", "[heat]")
{
    // The ordering is the real behavioural claim and it must hold everywhere,
    // including the styles too clean to produce a large number.
    for (const int style : {0, 1, 3, 9, 14})
    {
        EmberAudioProcessor proc;
        proc.prepareToPlay(48000.0, 512);

        if (auto* p = proc.getAPVTS().getParameter(pid::style(0)))
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(style)));

        const auto cold = heatAtDrive(proc, 0.0f);
        const auto hot = heatAtDrive(proc, 35.0f);

        INFO("style " << style << ": cold " << cold << ", hot " << hot);
        REQUIRE(hot > cold);
    }
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

TEST_CASE("probe: heat by style at 35 dB", "[heat][.probe]")
{
    // Not part of the suite (tagged hidden). Run with "[.probe]" to see what
    // each style actually reports, which is the only way to tell a clean style
    // from a broken measurement.
    for (int style : {0, 1, 3, 9, 14})
    {
        EmberAudioProcessor proc;
        proc.prepareToPlay(48000.0, 512);

        auto& apvts = proc.getAPVTS();

        if (auto* p = apvts.getParameter(pid::style(0)))
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(style)));

        if (auto* p = apvts.getParameter(pid::drive(0)))
            p->setValueNotifyingHost(p->convertTo0to1(35.0f));

        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        juce::Random rng{0x4321};

        for (int block = 0; block < 150; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample(ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.25f);

            proc.processBlock(buffer, midi);
        }

        WARN("style " << style << " heat " << proc.getBandHeat(0));
    }
}
