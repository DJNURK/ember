// The EQ curve the interface draws has to be the curve the filters implement.
//
// A node editor is a promise: drag this node here and the sound changes that
// way. If the drawn response is a separate model of the filters rather than a
// reading of them, the promise quietly breaks - usually at the edges, where
// bilinear-transform frequency warping pulls a shelf away from where the maths
// says it should be. So the model is checked against a measured sweep of the
// real filter, not against a second implementation of the same formula.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/BandFx.h"
#include "TestHelpers.h"
#include "plugin/ParameterIDs.h"
#include "plugin/PluginProcessor.h"

using namespace ember;
using namespace embertest;

namespace
{
/** Measures a prepared ToneStack's actual gain at one frequency by running a
    sine through it and comparing settled RMS in to RMS out. */
float measuredGainDb(ToneStack& tone, double sampleRate, float frequencyHz)
{
    constexpr int blockSize = 512;
    const int settleBlocks = 40;
    const int measureBlocks = 20;

    std::vector<float> buffer(static_cast<size_t>(blockSize));
    float* channels[1] = {buffer.data()};

    double phase = 0.0;
    const double increment = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;

    double inSum = 0.0;
    double outSum = 0.0;

    for (int block = 0; block < settleBlocks + measureBlocks; ++block)
    {
        double blockInSum = 0.0;

        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = static_cast<float>(std::sin(phase));
            phase += increment;

            if (phase > juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            buffer[static_cast<size_t>(i)] = sample;
            blockInSum += static_cast<double>(sample) * sample;
        }

        tone.process(channels, 1, blockSize);

        if (block < settleBlocks)
            continue; // IIR transients would read as gain error

        double blockOutSum = 0.0;

        for (int i = 0; i < blockSize; ++i)
            blockOutSum += static_cast<double>(buffer[static_cast<size_t>(i)]) * buffer[static_cast<size_t>(i)];

        inSum += blockInSum;
        outSum += blockOutSum;
    }

    if (inSum <= 0.0)
        return 0.0f;

    return static_cast<float>(juce::Decibels::gainToDecibels(std::sqrt(outSum / inSum)));
}
} // namespace

TEST_CASE("the drawn tone curve matches the filters it describes", "[eq]")
{
    constexpr double sampleRate = 48000.0;

    struct Setting
    {
        const char* what;
        float lowDb, midDb, highDb;
        ToneStack::Shape shape;
    };

    const Setting settings[] = {
        {"flat", 0.0f, 0.0f, 0.0f, {}},
        {"defaults, boosted", 4.0f, -3.0f, 6.0f, {}},
        {"low shelf moved up", 6.0f, 0.0f, 0.0f, {400.0f, 1000.0f, 0.7f, 4000.0f}},
        {"narrow mid bell", 0.0f, 9.0f, 0.0f, {150.0f, 2500.0f, 4.5f, 4000.0f}},
        {"wide mid cut", 0.0f, -9.0f, 0.0f, {150.0f, 700.0f, 0.3f, 4000.0f}},
        {"high shelf low", 0.0f, 0.0f, -8.0f, {150.0f, 1000.0f, 0.7f, 1500.0f}},
        {"everything at once", -5.0f, 7.0f, -4.0f, {90.0f, 3000.0f, 2.0f, 9000.0f}},
    };

    // Spread across the audible range, including the extremes where the
    // bilinear transform's frequency warping is worst.
    const float probes[] = {30.0f,   60.0f,   120.0f,  250.0f,   500.0f,  1000.0f,
                            2000.0f, 4000.0f, 8000.0f, 12000.0f, 16000.0f};

    for (const auto& setting : settings)
    {
        for (const auto probe : probes)
        {
            ToneStack tone;
            tone.prepare(sampleRate, 512, 1);
            tone.setShape(setting.shape);
            tone.setGainsDb(setting.lowDb, setting.midDb, setting.highDb);
            tone.reset();

            const auto drawn = tone.magnitudeDbAt(probe);
            const auto measured = measuredGainDb(tone, sampleRate, probe);

            INFO(setting.what << " at " << probe << " Hz: drawn " << drawn << " dB, measured " << measured << " dB");

            // The acceptance criterion is +/-0.5 dB between what the interface
            // claims and what the plugin does.
            REQUIRE_THAT(drawn, Catch::Matchers::WithinAbs(measured, 0.5));
        }
    }
}

TEST_CASE("a flat tone stack is genuinely flat", "[eq]")
{
    ToneStack tone;
    tone.prepare(48000.0, 512, 1);
    tone.setGainsDb(0.0f, 0.0f, 0.0f);

    for (const float probe : {30.0f, 200.0f, 1000.0f, 5000.0f, 15000.0f})
    {
        INFO("at " << probe << " Hz");
        REQUIRE_THAT(tone.magnitudeDbAt(probe), Catch::Matchers::WithinAbs(0.0, 0.01));
    }
}

TEST_CASE("the shape defaults reproduce the old fixed response", "[eq]")
{
    // The frequencies were constants before they became parameters. A preset
    // written then must load with exactly the response it was saved with, so
    // the defaults have to be the old constants to the last digit.
    const ToneStack::Shape defaults;

    REQUIRE(defaults.lowHz == 150.0f);
    REQUIRE(defaults.midHz == 1000.0f);
    REQUIRE(defaults.midQ == 0.7f);
    REQUIRE(defaults.highHz == 4000.0f);
}

TEST_CASE("node frequencies are clamped below Nyquist", "[eq]")
{
    // A node dragged to the top of its range at 44.1 kHz would otherwise ask
    // for a shelf above half the sample rate: not merely wrong, unstable.
    ToneStack tone;
    tone.prepare(44100.0, 512, 1);

    tone.setShape({20000.0f, 30000.0f, 0.7f, 40000.0f});
    tone.setGainsDb(6.0f, 6.0f, 6.0f);

    const auto shape = tone.getShape();
    const auto limit = static_cast<float>(44100.0 * 0.49);

    REQUIRE(shape.lowHz <= limit);
    REQUIRE(shape.midHz <= limit);
    REQUIRE(shape.highHz <= limit);

    // And the filters must still be producing finite audio.
    std::vector<float> buffer(512, 0.5f);
    float* channels[1] = {buffer.data()};
    tone.process(channels, 1, 512);

    for (const auto sample : buffer)
        REQUIRE(std::isfinite(sample));
}

TEST_CASE("a band's tone nodes are used where the parameter puts them", "[eq][presets]")
{
    // 2.1.1 briefly clamped each node into the band's own frequency span, on
    // the reasoning that a node outside its band shapes nothing. The reasoning
    // is right and the clamp is still wrong, for a reason only visible against
    // real content: no preset stores a node frequency, so all 36 factory
    // presets load at the defaults - 150 Hz, 1 kHz, 4 kHz - and under the
    // default crossovers two of those three fall outside each band.
    //
    // Outside the band a node is harmless: a 150 Hz low shelf on a band that
    // starts at 600 Hz is flat across everything the band carries. Clamped to
    // 600 Hz it lands at full strength on the band's own content. 34 of the 36
    // factory presets set a tone gain, so 34 of them quietly changed voicing.
    //
    // The invariant, therefore: where a node sits must not depend on where the
    // crossovers are.
    EmberAudioProcessor processor;

    auto set = [&processor](const juce::String& id, float value)
    {
        auto* p = processor.getAPVTS().getParameter(id);
        REQUIRE(p != nullptr);
        p->setValueNotifyingHost(p->getNormalisableRange().convertTo0to1(value));
    };

    // Three bands, split at 120 and 600 Hz, so the top band runs from 600 Hz up
    // and the default 150 Hz low shelf sits below it.
    set(pid::numBands, 3.0f);
    set(pid::crossover(0), 120.0f);
    set(pid::crossover(1), 600.0f);

    // Nothing but the tone stage should colour the probe.
    for (int b = 0; b < 3; ++b)
    {
        set(pid::drive(b), 0.0f);
        set(pid::bandMix(b), 100.0f);
    }

    set(pid::autoGain, 0.0f);

    const double sampleRate = 48000.0;
    const int blockSize = 512;
    const float probeHz = 900.0f; // inside the top band, above 600 Hz

    auto measureDb = [&](float shelfDb, float shelfHz)
    {
        for (int b = 0; b < 6; ++b)
        {
            set(pid::toneLow(b), shelfDb);
            set(pid::toneLowHz(b), shelfHz);
        }

        processor.prepareToPlay(sampleRate, blockSize);

        juce::AudioBuffer<float> buf(2, blockSize);
        juce::MidiBuffer midi;
        float out = 0.0f;

        // Settle first: gain smoothers and the crossover both need a few blocks
        // before the measurement means anything.
        for (int i = 0; i < 40; ++i)
        {
            fillSine(buf, sampleRate, probeHz, 0.25f);
            processor.processBlock(buf, midi);

            if (i >= 30)
                out = juce::jmax(out, rms(buf));
        }

        return juce::Decibels::gainToDecibels(out / 0.25f * std::sqrt(2.0f));
    };

    const auto flat = measureDb(0.0f, 150.0f);
    const auto asWritten = measureDb(-6.0f, 150.0f);
    const auto asClamped = measureDb(-6.0f, 600.0f);

    INFO("900 Hz: flat " << flat << " dB, shelf at 150 Hz " << asWritten << " dB, shelf at the 600 Hz band edge "
                         << asClamped << " dB");

    // A 150 Hz low shelf is flat at 900 Hz, two and a half octaves above its
    // corner, so the band sounds the same as with no shelf at all.
    REQUIRE_THAT(asWritten, Catch::Matchers::WithinAbs(flat, 0.2));

    // Moved to the band edge it is emphatically not flat there - which is both
    // what the clamp did and why it mattered. Asserted so the first REQUIRE
    // cannot pass by measuring nothing: a broken harness would report both as
    // flat.
    REQUIRE(flat - asClamped > 0.8);
}
