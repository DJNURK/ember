#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "TestHelpers.h"
#include "dsp/Crossover.h"

using namespace ember;
using namespace embertest;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kFftSize = 8192;

/** Run noise through the crossover and sum every band back together. */
juce::AudioBuffer<float> splitAndSum(Crossover& xo, const juce::AudioBuffer<float>& input, int numBands)
{
    const int numCh = input.getNumChannels();
    const int total = input.getNumSamples();

    std::array<juce::AudioBuffer<float>, kMaxBands> bandBuffers;
    for (auto& b : bandBuffers)
        b.setSize(numCh, kBlock, false, false, true);

    juce::AudioBuffer<float> summed(numCh, total);
    summed.clear();

    juce::AudioBuffer<float> chunk(numCh, kBlock);

    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = juce::jmin(kBlock, total - pos);
        for (int ch = 0; ch < numCh; ++ch)
            chunk.copyFrom(ch, 0, input, ch, pos, n);

        xo.process(chunk, bandBuffers, n);

        for (int b = 0; b < numBands; ++b)
            for (int ch = 0; ch < numCh; ++ch)
                summed.addFrom(ch, pos, bandBuffers[static_cast<size_t>(b)], ch, 0, n);
    }
    return summed;
}

const float kFreqs[kMaxCrossovers] = {120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f};
} // namespace

TEST_CASE("crossover band sum is magnitude-flat for every band count", "[crossover][null]")
{
    // A Linkwitz-Riley crossover sums to an ALLPASS, not to unity: flat in
    // magnitude, but with 360 degrees of phase rotation per crossover. So the
    // meaningful gate in minimum-phase mode is magnitude flatness, which is
    // what "phase-coherent summing" actually buys you. The true time-domain
    // null is tested separately in linear-phase mode below.
    //
    // Measured from the impulse response so the result is the exact transfer
    // function rather than a windowed approximation.
    constexpr int kIrLength = 32768; // 0.68 s at 48 kHz: long enough to decay

    for (int numBands = 1; numBands <= kMaxBands; ++numBands)
    {
        Crossover xo;
        xo.prepare(kRate, kBlock, 1);
        xo.setMode(CrossoverMode::MinimumPhaseLR4);
        xo.setNumBands(numBands);
        xo.setCrossoverFrequencies(kFreqs, numBands - 1);
        xo.reset();

        juce::AudioBuffer<float> impulse(1, kIrLength);
        impulse.clear();
        impulse.setSample(0, 0, 1.0f);

        auto ir = splitAndSum(xo, impulse, numBands);
        REQUIRE(allFinite(ir));

        // Confirm the response really has decayed, otherwise the FFT would be
        // truncating and the flatness number would be meaningless.
        float tailPeak = 0.0f;
        for (int i = kIrLength - 2048; i < kIrLength; ++i)
            tailPeak = juce::jmax(tailPeak, std::abs(ir.getSample(0, i)));
        INFO("numBands = " << numBands << ", impulse response tail peak = " << tailPeak);
        REQUIRE(tailPeak < 1.0e-6f);

        auto mag = impulseResponseMagnitudeDb(ir.getReadPointer(0), kIrLength);

        const int firstBin = static_cast<int>(30.0 / (kRate / kIrLength));
        const int lastBin = static_cast<int>(18000.0 / (kRate / kIrLength));

        float worst = 0.0f;
        int worstBin = firstBin;
        for (int bin = firstBin; bin < lastBin; ++bin)
        {
            const float dev = std::abs(mag[static_cast<size_t>(bin)]);
            if (dev > worst)
            {
                worst = dev;
                worstBin = bin;
            }
        }

        INFO("numBands = " << numBands << ", worst magnitude deviation = " << worst << " dB at "
                           << (worstBin * kRate / kIrLength) << " Hz");
        REQUIRE(worst < 0.01f);
    }
}

TEST_CASE("single band passes through untouched", "[crossover]")
{
    Crossover xo;
    xo.prepare(kRate, kBlock, 2);
    xo.setMode(CrossoverMode::MinimumPhaseLR4);
    xo.setNumBands(1);
    xo.reset();

    juce::AudioBuffer<float> input(2, kBlock * 4);
    fillWhiteNoise(input, 0xABCDEFu);

    auto summed = splitAndSum(xo, input, 1);

    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < input.getNumSamples(); ++i)
            maxDiff = juce::jmax(maxDiff, std::abs(input.getSample(ch, i) - summed.getSample(ch, i)));

    REQUIRE(maxDiff < 1.0e-6f);
}

TEST_CASE("linear phase crossover nulls against the delayed input", "[crossover][null][linearphase]")
{
    for (int numBands = 2; numBands <= kMaxBands; ++numBands)
    {
        Crossover xo;
        xo.prepare(kRate, kBlock, 2);
        xo.setMode(CrossoverMode::LinearPhase);
        xo.setNumBands(numBands);
        xo.setCrossoverFrequencies(kFreqs, numBands - 1);
        xo.reset();

        const int latency = xo.getLatencySamples();
        const int total = kFftSize * 4;

        juce::AudioBuffer<float> input(1, total);
        fillWhiteNoise(input, 0x5150u, 0.25f);

        auto summed = splitAndSum(xo, input, numBands);

        // Linear phase means constant group delay, so the sum should be the
        // input delayed by exactly the reported latency. Anything else is a bug
        // in the latency report, which would misalign the plugin in the host.
        const int start = latency + 1024;
        const int count = total - start - 1024;
        REQUIRE(count > kFftSize);

        double residual = 0.0, reference = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double d =
                static_cast<double>(summed.getSample(0, start + i)) - input.getSample(0, start + i - latency);
            residual += d * d;
            reference +=
                static_cast<double>(input.getSample(0, start + i - latency)) * input.getSample(0, start + i - latency);
        }
        const double residualDb = 10.0 * std::log10((residual / juce::jmax(1.0e-30, reference)) + 1.0e-30);

        INFO("numBands = " << numBands << ", latency = " << latency << ", residual = " << residualDb << " dB");
        REQUIRE(residualDb < -100.0);
    }
}

TEST_CASE("the reported edges are the clamped ones, not the requested ones", "[crossover]")
{
    // Anything that needs to know where a band begins and ends - the tone
    // stage's span, most of all - has to ask the crossover rather than reuse
    // the frequencies it passed in. The two are the same until automation
    // drives two edges together, and that is precisely the case where the
    // request describes a band that does not exist: a few hertz wide, when the
    // filters have actually placed the edge a third of an octave up.
    //
    // Clamping tone nodes into the band as REQUESTED would put them outside the
    // band as BUILT, which is the failure the span was added to prevent.
    ember::Crossover crossover;
    crossover.prepare(48000.0, 512, 2);
    crossover.setNumBands(4);

    // Three edges, the upper two asked for almost on top of the first.
    const float requested[3] = {1000.0f, 1001.0f, 1002.0f};
    crossover.setCrossoverFrequencies(requested, 3);

    const auto e0 = crossover.getCrossoverFrequency(0);
    const auto e1 = crossover.getCrossoverFrequency(1);
    const auto e2 = crossover.getCrossoverFrequency(2);

    INFO("edges: " << e0 << ", " << e1 << ", " << e2);

    // A third of an octave is 2^(1/3) = 1.2599. Allow for float rounding only.
    REQUIRE(e1 >= e0 * 1.2598f);
    REQUIRE(e2 >= e1 * 1.2598f);

    // And the reported value is what the caller would otherwise have assumed,
    // so the getter is not merely echoing the request back.
    REQUIRE(e1 > requested[1]);
    REQUIRE(e2 > requested[2]);

    // Out of range asks answer 0 rather than reading past the array.
    REQUIRE(crossover.getCrossoverFrequency(-1) == 0.0f);
    REQUIRE(crossover.getCrossoverFrequency(ember::kMaxCrossovers) == 0.0f);
}
