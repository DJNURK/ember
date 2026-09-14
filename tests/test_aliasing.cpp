#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/BandChain.h"
#include "dsp/styles/SaturationStyle.h"

using namespace ember;
using namespace embertest;

namespace
{
constexpr double kHostRate = 44100.0;
constexpr int kFftSize = 16384;

/** Energy in every bin that is neither the fundamental nor one of its true
    harmonics — i.e. the aliased components folded back down the spectrum. */
float measureAliasFloorDb(const juce::AudioBuffer<float>& output, double sampleRate, double fundamental)
{
    auto spec = magnitudeSpectrumDb(output.getReadPointer(0), kFftSize);
    const double binHz = sampleRate / kFftSize;

    // Reference level = the fundamental's bin.
    const int fundBin = static_cast<int>(std::round(fundamental / binHz));
    float fundamentalDb = -200.0f;
    for (int b = juce::jmax(0, fundBin - 3); b <= fundBin + 3 && b < static_cast<int>(spec.size()); ++b)
        fundamentalDb = juce::jmax(fundamentalDb, spec[static_cast<size_t>(b)]);

    auto isHarmonicOrFundamental = [&](int bin)
    {
        const double f = bin * binHz;
        for (int h = 1; h <= 40; ++h)
        {
            const double hf = fundamental * h;
            if (hf > sampleRate * 0.5) break;
            if (std::abs(f - hf) < binHz * 4.0) return true;
        }
        return false;
    };

    float worstAliasDb = -200.0f;
    for (int bin = 4; bin < static_cast<int>(spec.size()) - 4; ++bin)
    {
        if (isHarmonicOrFundamental(bin)) continue;
        worstAliasDb = juce::jmax(worstAliasDb, spec[static_cast<size_t>(bin)]);
    }
    return worstAliasDb - fundamentalDb;   // relative to the fundamental
}
} // namespace

TEST_CASE("hard clip at 16x oversampling keeps aliasing below -80 dBFS", "[aliasing]")
{
    // The spec's gate: a 10 kHz sine at 44.1 kHz through the hard-clip style at
    // 16x oversampling must show alias components below -80 dBFS. At 44.1 kHz a
    // 10 kHz fundamental has only two harmonics below Nyquist, so essentially
    // every other component in the output is an alias — this is a demanding and
    // honest measurement of the oversampler plus the ADAA shaper together.
    BandChain chain;
    chain.prepare(kHostRate, kFftSize, 1, OversamplingFactor::x16);
    chain.reset();

    BandParams p;
    p.style = StyleID::HardClip;
    p.driveDb = 24.0f;
    p.mix01 = 1.0f;
    p.levelDb = 0.0f;
    chain.setParameters(p);

    juce::AudioBuffer<float> buf(1, kFftSize);
    fillSine(buf, kHostRate, 10000.0, 0.5f);

    // Let smoothing settle before the measured block.
    {
        juce::AudioBuffer<float> warm(1, kFftSize);
        fillSine(warm, kHostRate, 10000.0, 0.5f);
        chain.process(warm, kFftSize);
    }
    chain.process(buf, kFftSize);

    REQUIRE(allFinite(buf));

    const float aliasDb = measureAliasFloorDb(buf, kHostRate, 10000.0);
    INFO("worst alias component relative to fundamental: " << aliasDb << " dB");
    REQUIRE(aliasDb < -80.0f);
}

TEST_CASE("oversampling meaningfully reduces aliasing", "[aliasing]")
{
    // A relative check that catches an oversampler that is silently doing
    // nothing: 16x must be clearly better than no oversampling at all.
    auto measure = [](OversamplingFactor f)
    {
        BandChain chain;
        chain.prepare(kHostRate, kFftSize, 1, f);
        chain.reset();
        BandParams p;
        p.style = StyleID::HardClip;
        p.driveDb = 24.0f;
        chain.setParameters(p);

        juce::AudioBuffer<float> warm(1, kFftSize), buf(1, kFftSize);
        fillSine(warm, kHostRate, 10000.0, 0.5f);
        fillSine(buf, kHostRate, 10000.0, 0.5f);
        chain.process(warm, kFftSize);
        chain.process(buf, kFftSize);
        return measureAliasFloorDb(buf, kHostRate, 10000.0);
    };

    const float off = measure(OversamplingFactor::Off);
    const float x16 = measure(OversamplingFactor::x16);
    INFO("alias floor: off = " << off << " dB, 16x = " << x16 << " dB");
    REQUIRE(x16 < off - 20.0f);
}
