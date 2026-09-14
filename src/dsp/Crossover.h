#pragma once
#include <array>
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"

namespace ember
{
/**
    Splits one input into 1..6 phase-coherent bands.

    Two modes:
    - `MinimumPhaseLR4`: Linkwitz-Riley 4th order tree. Bands that skip a
      crossover are compensated with the matching 2nd-order all-pass sections so
      the band sum stays magnitude-flat. Zero latency.
    - `LinearPhase`: FIR band filters, constant group delay, reports latency.

    Summing all bands with no further processing must be transparent:
    +/- 0.01 dB flatness and < -100 dB residual against the dry signal
    (`tests/test_crossover.cpp` enforces this).

    Realtime contract: `process` allocates nothing. Frequency and band-count
    changes are applied smoothly by the caller (EmberEngine crossfades band
    count); `setCrossoverFrequencies` itself only updates coefficients.
*/
class Crossover
{
public:
    Crossover();
    ~Crossover();

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;

    /** Changing mode re-plans filters; call off the audio thread (it may allocate
        in LinearPhase mode). EmberEngine only calls this from prepareToPlay or
        from a parameter-change on the message thread with the engine suspended. */
    void setMode(CrossoverMode mode);

    /** 1..kMaxBands. Cheap, realtime-safe. */
    void setNumBands(int numBands) noexcept;
    int getNumBands() const noexcept;

    /** `freqs` holds `getNumBands() - 1` ascending edge frequencies in Hz.
        The caller guarantees ascending order and >= 1/3-octave separation. */
    void setCrossoverFrequencies(const float* freqs, int numEdges) noexcept;

    /** Split `input` (numChannels x numSamples) into `getNumBands()` buffers.
        `bandOut[b]` must already be sized >= numChannels x numSamples. */
    void process(const juce::AudioBuffer<float>& input,
                 std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut,
                 int numSamples) noexcept;

    /** Latency introduced by the current mode, in samples at the host rate. */
    int getLatencySamples() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Crossover)
};
} // namespace ember
