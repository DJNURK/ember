#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/BandChain.h"
#include "dsp/PolyphaseOversampler.h"
#include "dsp/styles/SaturationStyle.h"

using namespace ember;
using namespace embertest;

namespace
{
constexpr double kHostRate = 44100.0;
constexpr int kFftSize = 16384;

// A bin-centred fundamental: 3716 cycles in 16384 samples is 10001.2 Hz, near
// enough to the specified 10 kHz. Choosing it this way means the fundamental,
// every harmonic and every aliased image all land exactly on FFT bins, so the
// spectrum can be taken with a RECTANGULAR window and has no leakage at all.
//
// This matters more than it looks. Measuring with a Hann window instead puts
// the window's own sidelobes — about -50 dB a few bins from a strong peak —
// straight into the "alias" bins, and the measurement then reports roughly
// -51 dB no matter how good the anti-aliasing actually is. That is a property
// of the analysis, not of the plugin.
constexpr int kFundamentalBin = 3716;
constexpr double kFundamental = kFundamentalBin * kHostRate / kFftSize;

/** Worst non-harmonic component, in dB relative to the fundamental.

    At 44.1 kHz a 10 kHz fundamental has only one harmonic below Nyquist (the
    2nd, which a symmetric clipper does not produce anyway), so every other
    component in the output is an aliased image folded back down. */
float measureAliasFloorDb(const juce::AudioBuffer<float>& output)
{
    auto spec = magnitudeSpectrumDb(output.getReadPointer(0), kFftSize, /*applyWindow*/ false);

    float fundamentalDb = -300.0f;
    for (int b = kFundamentalBin - 1; b <= kFundamentalBin + 1; ++b)
        fundamentalDb = juce::jmax(fundamentalDb, spec[static_cast<size_t>(b)]);

    auto isExcluded = [](int bin)
    {
        if (bin <= 2)
            return true; // DC and the first bins
        if (std::abs(bin - kFundamentalBin) <= 2)
            return true; // fundamental
        if (std::abs(bin - 2 * kFundamentalBin) <= 2)
            return true; // 2nd harmonic, if any
        return false;
    };

    float worst = -300.0f;
    for (int bin = 0; bin < static_cast<int>(spec.size()); ++bin)
        if (!isExcluded(bin))
            worst = juce::jmax(worst, spec[static_cast<size_t>(bin)]);

    return worst - fundamentalDb;
}

float runAndMeasure(OversamplingFactor factor, StyleID style, float driveDb)
{
    BandChain chain;
    chain.prepare(kHostRate, kFftSize, 1, factor);
    chain.reset();

    BandParams p;
    p.style = style;
    p.driveDb = driveDb;
    p.mix01 = 1.0f;
    chain.setParameters(p);

    // Two warm-up blocks so parameter smoothing and the oversampler's filter
    // state are fully settled before the measured block.
    juce::AudioBuffer<float> buf(1, kFftSize);
    for (int i = 0; i < 3; ++i)
    {
        fillSine(buf, kHostRate, kFundamental, 0.5f);
        chain.process(buf, kFftSize);
    }
    return measureAliasFloorDb(buf);
}
} // namespace

TEST_CASE("hard clip at 16x oversampling keeps aliasing below -80 dBFS", "[aliasing]")
{
    // The specification's gate. Hard Clip is the worst case: a discontinuous
    // slope generating harmonics far above Nyquist, which the oversampler and
    // the ADAA shaper together have to keep from folding back into the band.
    const float aliasDb = runAndMeasure(OversamplingFactor::x16, StyleID::HardClip, 24.0f);
    INFO("worst alias component, relative to the fundamental: " << aliasDb << " dB");
    REQUIRE(aliasDb < -80.0f);
}

TEST_CASE("oversampling monotonically reduces aliasing", "[aliasing]")
{
    // Catches an oversampler that is silently doing nothing, and documents what
    // each factor actually buys.
    const float off = runAndMeasure(OversamplingFactor::Off, StyleID::HardClip, 24.0f);
    const float x2 = runAndMeasure(OversamplingFactor::x2, StyleID::HardClip, 24.0f);
    const float x4 = runAndMeasure(OversamplingFactor::x4, StyleID::HardClip, 24.0f);
    const float x16 = runAndMeasure(OversamplingFactor::x16, StyleID::HardClip, 24.0f);

    INFO("alias floor: off=" << off << "  2x=" << x2 << "  4x=" << x4 << "  16x=" << x16 << " dB");
    REQUIRE(x2 < off);
    REQUIRE(x4 < x2);
    REQUIRE(x16 < off - 40.0f);
}

TEST_CASE("the other hard-edged styles are also anti-aliased", "[aliasing]")
{
    for (auto style : {StyleID::Foldback, StyleID::Rectify, StyleID::Smudge})
    {
        const float aliasDb = runAndMeasure(OversamplingFactor::x16, style, 24.0f);
        INFO(getStyleName(style) << " alias floor: " << aliasDb << " dB");
        REQUIRE(aliasDb < -60.0f);
    }
}

TEST_CASE("the polyphase oversampler matches the JUCE one it replaced", "[aliasing][oversampling]")
{
    // The realtime oversampler is hand-written (see PolyphaseOversampler.h) so
    // that the two polyphase cascades and the two channels can run as four
    // lanes of one register instead of four separate passes. It is only allowed
    // to be faster: the filters, the recursion and the latency compensation are
    // meant to be JUCE's, to the bit.
    //
    // That is worth pinning down, because nothing else in the suite would
    // notice if it drifted. The alias gate above passes with 8 dB of margin and
    // would swallow a coefficient wrong in the seventh place; the dry/wet comb
    // that a half-sample of latency error produces is not measured anywhere.
    //
    // A hard clipper sits between the up and the down pass so the downsampling
    // filters see full-band content rather than a signal their own stopband has
    // already removed.
    for (int numChannels : {1, 2})
    {
        for (int numStages = 1; numStages <= 4; ++numStages)
        {
            constexpr int block = 512;

            PolyphaseOversampler mine;
            mine.prepare(numChannels, numStages, block);

            juce::dsp::Oversampling<float> theirs(static_cast<size_t>(numChannels), static_cast<size_t>(numStages),
                                                  juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true,
                                                  true);
            theirs.initProcessing(static_cast<size_t>(block));
            theirs.reset();

            INFO("channels = " << numChannels << ", stages = " << numStages);
            REQUIRE(mine.getLatencyInSamples() == static_cast<float>(theirs.getLatencyInSamples()));

            juce::AudioBuffer<float> a(numChannels, block), b(numChannels, block);
            float worst = 0.0f;

            for (int pass = 0; pass < 8; ++pass)
            {
                fillWhiteNoise(a, 0x5eedu + static_cast<uint32_t>(pass), 0.8f);
                b.makeCopyOf(a);

                auto blockA = juce::dsp::AudioBlock<float>(a);
                auto blockB = juce::dsp::AudioBlock<float>(b);

                auto upA = mine.processSamplesUp(juce::dsp::AudioBlock<const float>(blockA));
                auto upB = theirs.processSamplesUp(juce::dsp::AudioBlock<const float>(blockB));

                REQUIRE(upA.getNumSamples() == upB.getNumSamples());

                for (size_t ch = 0; ch < static_cast<size_t>(numChannels); ++ch)
                {
                    auto* pa = upA.getChannelPointer(ch);
                    auto* pb = upB.getChannelPointer(ch);

                    for (size_t i = 0; i < upA.getNumSamples(); ++i)
                    {
                        worst = juce::jmax(worst, std::abs(pa[i] - pb[i]));
                        pa[i] = juce::jlimit(-1.0f, 1.0f, pa[i]);
                        pb[i] = juce::jlimit(-1.0f, 1.0f, pb[i]);
                    }
                }

                mine.processSamplesDown(blockA);
                theirs.processSamplesDown(blockB);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < block; ++i)
                        worst = juce::jmax(worst, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
            }

            INFO("worst difference against juce::dsp::Oversampling: " << worst);
            REQUIRE(worst == 0.0f);
        }
    }
}
