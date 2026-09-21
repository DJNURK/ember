#pragma once
#include <array>
#include <vector>
#include <juce_dsp/juce_dsp.h>

namespace ember
{
/**
    Half-band polyphase IIR oversampler.

    Functionally a drop-in replacement for `juce::dsp::Oversampling<float>`
    constructed with `filterHalfBandPolyphaseIIR`, maximum quality and integer
    latency: the filters are designed by the same `juce::dsp::FilterDesign` call
    with the same arguments, the allpass recursion is written out in the same
    order, and the same Thiran delay makes the total latency a whole number of
    samples. What changes is only the shape of the loop.

    Why it is worth having at all
    -----------------------------
    A polyphase half-band stage is two cascades of first-order allpass sections,
    one per polyphase branch. Each section is

        out = a * in + v;   v = in - a * out;

    and the next section's input is this section's output, so a cascade is a
    chain of dependent multiply-adds. The kernel is therefore bound by FMA
    LATENCY, not by arithmetic throughput: on an M2 a five-section cascade is
    about twenty cycles deep but only needs about five cycles of issue.

    JUCE runs one channel to completion before starting the next, which leaves
    the direct and the delayed cascade — two independent chains — as the only
    parallelism the core can find, and the other two thirds of the FP pipes
    idle. Interleaving the channels into the same inner loop puts FOUR
    independent chains in flight for the same chain depth, so the same
    wall-clock latency covers twice the work. Measured on the six-band / 4x
    benchmark that is worth about a quarter of the whole engine.

    Going wider than four does NOT follow: with four chains the kernel is
    already back to issue-limited, so replacing them with one four-lane SIMD
    register buys nothing (measured, see docs/STATUS.md). More would need more
    independent streams, i.e. batching the bands, which the per-band structure
    of the chain does not offer.

    Restrictions relative to `juce::dsp::Oversampling`: float only, one or two
    channels, polyphase IIR only, maximum quality only, integer latency always.
*/
class PolyphaseOversampler
{
public:
    PolyphaseOversampler() = default;

    /** Designs the filters and allocates the buffers. Message/prepare thread
        only — this allocates.

        @param numChannels   1 or 2
        @param numStages     number of cascaded 2x stages (1..4)
        @param maxBlockSize  the largest block, at the BASE rate, that
                             `processSamplesUp` will be given */
    void prepare(int numChannels, int numStages, int maxBlockSize);

    /** Returns to the silent state. Does not allocate. */
    void reset() noexcept;

    /** Whole number of base-rate samples of latency, as with JUCE's integer
        latency mode. Valid after `prepare`. */
    float getLatencyInSamples() const noexcept { return totalLatency; }

    int getFactor() const noexcept { return factor; }

    /** Upsamples in to the internal buffer and returns a view of it.
        Realtime-safe. */
    juce::dsp::AudioBlock<float> processSamplesUp(const juce::dsp::AudioBlock<const float>& input) noexcept;

    /** Downsamples the internal buffer back into `output`. Realtime-safe. */
    void processSamplesDown(juce::dsp::AudioBlock<float>& output) noexcept;

private:
    struct Stage
    {
        /** Allpass coefficients, direct-path sections first then delayed-path,
            exactly as `juce::dsp::Oversampling` packs them. */
        std::vector<float> coeffsUp, coeffsDown;
        int directUp{0}, directDown{0};

        /** One value per section per channel, section-major so both channels'
            state for a section is one contiguous pair. */
        std::vector<float> stateUp, stateDown;
        std::array<float, 2> delayDown{};

        juce::AudioBuffer<float> buffer; ///< this stage's output, at 2x its input rate
    };

    std::vector<Stage> stages;

    /** The integer-latency compensator. JUCE builds this out of a
        `DelayLine<float, Thiran>` whose delay is always between 0.618 and 1.618
        samples, which reduces to a bare first-order allpass:

            y[n] = x[n-1] + alpha * (x[n] - y[n-1]),  alpha = (1-f) / (1+f)

        — the ring buffer, the modulo and the two `getSample` calls around it
        all fall away. Written out, it is the same three floating-point
        operations in the same order, so the result is bit-identical. */
    std::array<float, 2> compPrevIn{}, compPrevOut{};
    float compAlpha{0.0f};
    float fractionalDelay{0.0f};
    float totalLatency{0.0f};
    int channels{2};
    int factor{1};
    int maxBlock{512};
    bool ready{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PolyphaseOversampler)
};
} // namespace ember
