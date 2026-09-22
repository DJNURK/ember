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
    with the same arguments, the allpass recursion is the same expression in the
    same order, and the same fractional delay makes the total latency a whole
    number of samples. What changes is only the shape of the loop.

    Why it is worth having at all
    -----------------------------
    A half-band stage is two cascades of first-order allpass sections, one per
    polyphase branch. Each section is

        out = a * in + v;   v = in - a * out;

    and the next section's input is this section's output, so a cascade is a
    chain of dependent multiply-adds — three or four of them for these designs.
    The kernel is bound by multiply-add LATENCY, not by arithmetic throughput.
    JUCE runs one channel to completion before starting the next, which leaves
    the direct and the delayed cascade as the only two independent chains the
    core can find, and most of the FP pipes idle.

    Here the two cascades and the two channels are FOUR LANES of one register:

        [ ch0 direct, ch0 delayed, ch1 direct, ch1 delayed ]

    so a stereo frame costs one cascade's worth of latency instead of four. The
    two cascades differ by at most one section; the shorter one is padded with
    alpha = 1, which is an exact identity for a section whose state starts at
    zero (out = in, and the state stays exactly zero), so the padding changes no
    bit of the result.

    Mono runs the lower pair of the same lanes scalar. It shares the layout, so
    a block that arrives with fewer channels than the last one cannot scramble
    the filter state, and lanes 2 and 3 are simply never touched.

    How much of this is the SIMD, honestly
    --------------------------------------
    Almost none of it. Writing the same four lanes as four scalar chains in one
    loop measured 4.50% of a core on the six-band / 4x benchmark against the
    register form's 4.42%, and 17.3% against 16.9% at 16x — the vector registers
    are worth about 1.5%, the chains in flight are worth the rest. That is what
    a latency-bound kernel looks like: once four chains are interleaved the core
    is issue-limited again, and a wider register has nothing left to fill.
    The register form is kept because it is also the simpler code — one padded
    loop instead of a common run plus two remainders — and because it holds up
    better where the work grows, at 16x and at 96 kHz.

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

    /** Upsamples `input` into the internal buffer and returns a view of it.
        Realtime-safe. */
    juce::dsp::AudioBlock<float> processSamplesUp(const juce::dsp::AudioBlock<const float>& input) noexcept;

    /** Downsamples the internal buffer back into `output`. Realtime-safe. */
    void processSamplesDown(juce::dsp::AudioBlock<float>& output) noexcept;

    /** Sections per cascade after padding. The widest of these designs is four;
        the ceiling is generous so the storage can be fixed-size and aligned. */
    static constexpr int kMaxSections = 8;

private:
    struct Stage
    {
        /** Four lanes per section, in the order described above, and their
            negatives: the state update is `in - a * out`, and feeding `-a` to a
            fused multiply-add is what keeps that one rounding rather than two.
            See `cascade`. */
        alignas(16) std::array<float, 4 * kMaxSections> coeffsUp{}, coeffsDown{};
        alignas(16) std::array<float, 4 * kMaxSections> negCoeffsUp{}, negCoeffsDown{};
        alignas(16) std::array<float, 4 * kMaxSections> stateUp{}, stateDown{};
        int sectionsUp{0}, sectionsDown{0};

        /** The delayed branch's bare unit delay, per channel, used on the way
            back down. */
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
