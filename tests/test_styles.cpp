#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/styles/SaturationStyle.h"
#include "dsp/StyleCalibrator.h"

using namespace ember;
using namespace embertest;

namespace
{
constexpr double kRate = 96000.0;   // a representative oversampled rate
constexpr int kBlock = 1024;

StyleParams makeParams(double rate, float driveDb)
{
    StyleParams p;
    p.sampleRate = rate;
    p.driveDb = driveDb;
    p.driveLin = juce::Decibels::decibelsToGain(driveDb);
    p.amount01 = juce::jlimit(0.0f, 1.0f, driveDb / 40.0f);
    return p;
}
} // namespace

TEST_CASE("every style id constructs and reports its identity", "[styles]")
{
    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);
        auto style = createSaturationStyle(id);
        REQUIRE(style != nullptr);
        INFO("style index " << i);
        REQUIRE(juce::String(style->getName()) == juce::String(getStyleName(id)));
        REQUIRE(style->getCategory() == getStyleCategory(id));
    }
}

TEST_CASE("styles stay bounded and finite at every drive", "[styles][safety]")
{
    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);
        auto style = createSaturationStyle(id);
        style->prepare(kRate, kBlock, 2);
        style->reset();

        for (float driveDb : { 0.0f, 12.0f, 24.0f, 40.0f })
        {
            juce::AudioBuffer<float> buf(2, kBlock);
            // Deliberately hot input: hosts do send signals above 0 dBFS.
            fillWhiteNoise(buf, 0x900Du + static_cast<uint32_t>(i), 2.0f);

            auto params = makeParams(kRate, driveDb);
            style->process(buf.getArrayOfWritePointers(), 2, kBlock, params);

            INFO(getStyleName(id) << " at " << driveDb << " dB drive");
            REQUIRE(allFinite(buf));
            REQUIRE(peak(buf) <= 8.0f);
        }
    }
}

TEST_CASE("styles survive degenerate block sizes and mono", "[styles][safety]")
{
    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);
        auto style = createSaturationStyle(id);
        style->prepare(kRate, kBlock, 2);
        style->reset();

        auto params = makeParams(kRate, 20.0f);

        juce::AudioBuffer<float> one(2, 1);
        one.setSample(0, 0, 0.5f);
        one.setSample(1, 0, -0.5f);
        style->process(one.getArrayOfWritePointers(), 2, 1, params);
        INFO(getStyleName(id) << " with a single sample");
        REQUIRE(allFinite(one));

        juce::AudioBuffer<float> zero(2, 1);
        zero.clear();
        style->process(zero.getArrayOfWritePointers(), 2, 0, params);   // zero-length block
        REQUIRE(allFinite(zero));

        juce::AudioBuffer<float> mono(1, 256);
        fillWhiteNoise(mono, 0x77u, 0.5f);
        style->process(mono.getArrayOfWritePointers(), 1, 256, params);
        INFO(getStyleName(id) << " in mono");
        REQUIRE(allFinite(mono));
    }
}

TEST_CASE("silence in gives silence out", "[styles][safety]")
{
    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);

        // Decimate and Bitcrush quantise around zero and Shimmer carries an
        // internal oscillator, so they are allowed a small noise floor rather
        // than mathematical silence; everything else must be exactly quiet.
        const bool allowFloor = (id == StyleID::Decimate || id == StyleID::Bitcrush
                                 || id == StyleID::Shimmer || id == StyleID::Breathe);

        auto style = createSaturationStyle(id);
        style->prepare(kRate, kBlock, 2);
        style->reset();

        juce::AudioBuffer<float> buf(2, kBlock);
        buf.clear();
        auto params = makeParams(kRate, 30.0f);
        style->process(buf.getArrayOfWritePointers(), 2, kBlock, params);

        INFO(getStyleName(id));
        REQUIRE(allFinite(buf));
        REQUIRE(peak(buf) < (allowFloor ? 0.02f : 1.0e-4f));
    }
}

TEST_CASE("calibrated styles are loudness matched at 0 dB drive", "[styles][gainmatch]")
{
    // The spec requires every style to sit within +/-1 dB of the others at
    // 0 dB drive. StyleCalibrator measures the compensation; this test checks
    // that applying it actually lands inside the window.
    const auto& cal = StyleCalibrator::getForSampleRate(kRate);

    juce::AudioBuffer<float> reference(1, kBlock * 8);
    fillPinkNoise(reference, 0xC0FFEEu, -18.0f);
    const float inputRms = rms(reference, 0, kBlock, kBlock * 6);

    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);
        auto style = createSaturationStyle(id);
        style->prepare(kRate, reference.getNumSamples(), 1);
        style->reset();

        juce::AudioBuffer<float> buf(1, reference.getNumSamples());
        buf.makeCopyOf(reference);

        auto params = makeParams(kRate, 0.0f);
        style->process(buf.getArrayOfWritePointers(), 1, buf.getNumSamples(), params);
        buf.applyGain(cal.compensationGain(id, 0.0f));

        const float outRms = rms(buf, 0, kBlock, kBlock * 6);
        const float deviationDb = juce::Decibels::gainToDecibels(outRms / juce::jmax(1.0e-9f, inputRms));

        INFO(getStyleName(id) << " deviation " << deviationDb << " dB");
        REQUIRE(std::abs(deviationDb) <= 1.0f);
    }
}

TEST_CASE("gain matching holds across the drive range", "[styles][gainmatch]")
{
    // Not as tight as the 0 dB gate — heavy distortion genuinely changes the
    // crest factor — but the compensation must keep levels in a sane window so
    // that sweeping drive does not blow up the mix.
    const auto& cal = StyleCalibrator::getForSampleRate(kRate);

    juce::AudioBuffer<float> reference(1, kBlock * 8);
    fillPinkNoise(reference, 0xFEEDu, -18.0f);
    const float inputRms = rms(reference, 0, kBlock, kBlock * 6);

    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto id = static_cast<StyleID>(i);
        for (float driveDb : { 6.0f, 18.0f, 30.0f, 40.0f })
        {
            auto style = createSaturationStyle(id);
            style->prepare(kRate, reference.getNumSamples(), 1);
            style->reset();

            juce::AudioBuffer<float> buf(1, reference.getNumSamples());
            buf.makeCopyOf(reference);

            auto params = makeParams(kRate, driveDb);
            style->process(buf.getArrayOfWritePointers(), 1, buf.getNumSamples(), params);
            buf.applyGain(cal.compensationGain(id, driveDb));

            const float outRms = rms(buf, 0, kBlock, kBlock * 6);
            const float deviationDb = juce::Decibels::gainToDecibels(outRms / juce::jmax(1.0e-9f, inputRms));

            INFO(getStyleName(id) << " at " << driveDb << " dB drive: " << deviationDb << " dB");
            REQUIRE(std::abs(deviationDb) <= 3.0f);
        }
    }
}
