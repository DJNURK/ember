#include "dsp/PolyphaseOversampler.h"

#include <algorithm>
#include <cmath>

namespace ember
{
namespace
{
constexpr int kMaxStages = 4; // 2x .. 16x

/** The half-band filter set for a given number of 2x stages.

    The design depends only on the number of stages — not on the sample rate —
    so it is computed once per stage count for the whole process and shared by
    every band. That is not just tidiness: `designIIRLowpassHalfBandPolyphase-
    AllpassMethod` is an elliptic design with an iterative root search, and the
    old code paid for it once per band per `prepareToPlay`. */
struct HalfBandDesign
{
    struct StageCoeffs
    {
        std::vector<float> up, down;
        int directUp{0}, directDown{0};
    };

    std::vector<StageCoeffs> stages;
    float fractionalDelay{0.0f};
    float totalLatency{0.0f};
};

/** Pack one polyphase structure the way `juce::dsp::Oversampling` packs it:
    every direct-path section, then every delayed-path section EXCEPT the first,
    which is the branch's bare unit delay and is applied by the loop itself.

    `numDirect` is derived from the total the way JUCE derives it — arithmetically,
    not from `directPath.size()`. The two agree for every design this call can
    return, but deriving it the same way means the split cannot drift even if a
    future JUCE changes the structure. */
void packStructure(const juce::dsp::FilterDesign<float>::IIRPolyphaseAllpassStructure& s,
                   std::vector<float>& out, int& numDirect)
{
    out.clear();

    for (int i = 0; i < s.directPath.size(); ++i)
        out.push_back(s.directPath.getObjectPointer(i)->coefficients[0]);

    for (int i = 1; i < s.delayedPath.size(); ++i)
        out.push_back(s.delayedPath.getObjectPointer(i)->coefficients[0]);

    const int total = static_cast<int>(out.size());
    numDirect = total - total / 2;
}

HalfBandDesign buildDesign(int numStages)
{
    HalfBandDesign d;
    d.stages.resize(static_cast<size_t>(numStages));

    for (int n = 0; n < numStages; ++n)
    {
        // The same transition widths and stopband targets JUCE uses for
        // `isMaximumQuality == true`. Reproduced rather than referenced because
        // they are baked into the body of the Oversampling constructor.
        const float twUp = 0.10f * (n == 0 ? 0.5f : 1.0f);
        const float twDown = 0.12f * (n == 0 ? 0.5f : 1.0f);
        const float gainUp = -90.0f + 10.0f * static_cast<float>(n);
        const float gainDown = -75.0f + 10.0f * static_cast<float>(n);

        auto& stage = d.stages[static_cast<size_t>(n)];
        packStructure(juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod(twUp, gainUp),
                      stage.up, stage.directUp);
        packStructure(juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod(twDown, gainDown),
                      stage.down, stage.directDown);
    }

    // Latency is read straight out of a throwaway `juce::dsp::Oversampling`
    // rather than recomputed. JUCE derives it from the phase response of the
    // equivalent high-order IIR, which is a page of polynomial algebra that
    // would be a liability to transcribe: any drift would misalign the dry path
    // against the wet one and comb the top octave. `initProcessing(1)` is what
    // computes the fractional part, and sizes the throwaway's buffers for a
    // single sample.
    juce::dsp::Oversampling<float> reference(1, static_cast<size_t>(numStages),
                                             juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    reference.initProcessing(1);
    d.totalLatency = reference.getLatencyInSamples();
    reference.setUsingIntegerLatency(false);
    d.fractionalDelay = d.totalLatency - reference.getLatencyInSamples();

    return d;
}

/** Thread-safe by the usual magic-static rule, and only ever reached from
    `prepare`, which is message-thread only. */
const HalfBandDesign& getDesign(int numStages)
{
    static const std::array<HalfBandDesign, kMaxStages> designs{buildDesign(1), buildDesign(2), buildDesign(3),
                                                                buildDesign(4)};
    return designs[static_cast<size_t>(juce::jlimit(1, kMaxStages, numStages) - 1)];
}

//==============================================================================
// The kernels. `NumCh` is a template parameter so the per-channel loops unroll
// completely and the two channels' chains end up interleaved in the schedule,
// which is the whole point of the exercise.
//==============================================================================

/** One 2x upsampling stage: `n` input samples in, `2 * n` out.
    State is laid out section-major: `state[NumCh * section + channel]`. */
template <int NumCh>
void stageUp(const float* const* in, float* const* out, int n, const float* coeffs, int numSections, int numDirect,
             float* state) noexcept
{
    const int numDelayed = numSections - numDirect;
    const int common = std::min(numDirect, numDelayed);

    for (int i = 0; i < n; ++i)
    {
        float a[NumCh], b[NumCh];

        for (int k = 0; k < NumCh; ++k)
        {
            a[k] = in[k][i];
            b[k] = a[k];
        }

        // Both cascades, both channels, one section at a time: 2 * NumCh
        // independent dependency chains sharing one chain's worth of latency.
        for (int s = 0; s < common; ++s)
        {
            const float ad = coeffs[s];
            const float ae = coeffs[numDirect + s];
            float* sd = state + NumCh * s;
            float* se = state + NumCh * (numDirect + s);

            for (int k = 0; k < NumCh; ++k)
            {
                const float od = ad * a[k] + sd[k];
                const float oe = ae * b[k] + se[k];
                sd[k] = a[k] - ad * od;
                se[k] = b[k] - ae * oe;
                a[k] = od;
                b[k] = oe;
            }
        }

        // Whichever cascade is longer finishes on its own. The two differ by at
        // most one section.
        for (int s = common; s < numDirect; ++s)
        {
            const float ad = coeffs[s];
            float* sd = state + NumCh * s;

            for (int k = 0; k < NumCh; ++k)
            {
                const float od = ad * a[k] + sd[k];
                sd[k] = a[k] - ad * od;
                a[k] = od;
            }
        }

        for (int s = common; s < numDelayed; ++s)
        {
            const float ae = coeffs[numDirect + s];
            float* se = state + NumCh * (numDirect + s);

            for (int k = 0; k < NumCh; ++k)
            {
                const float oe = ae * b[k] + se[k];
                se[k] = b[k] - ae * oe;
                b[k] = oe;
            }
        }

        for (int k = 0; k < NumCh; ++k)
        {
            out[k][2 * i] = a[k];
            out[k][2 * i + 1] = b[k];
        }
    }
}

/** One 2x downsampling stage: `2 * n` samples in `in`, `n` out. */
template <int NumCh>
void stageDown(const float* const* in, float* const* out, int n, const float* coeffs, int numSections, int numDirect,
               float* state, float* delayState) noexcept
{
    const int numDelayed = numSections - numDirect;
    const int common = std::min(numDirect, numDelayed);

    float held[NumCh];
    for (int k = 0; k < NumCh; ++k)
        held[k] = delayState[k];

    for (int i = 0; i < n; ++i)
    {
        float a[NumCh], b[NumCh];

        for (int k = 0; k < NumCh; ++k)
        {
            a[k] = in[k][2 * i];
            b[k] = in[k][2 * i + 1];
        }

        for (int s = 0; s < common; ++s)
        {
            const float ad = coeffs[s];
            const float ae = coeffs[numDirect + s];
            float* sd = state + NumCh * s;
            float* se = state + NumCh * (numDirect + s);

            for (int k = 0; k < NumCh; ++k)
            {
                const float od = ad * a[k] + sd[k];
                const float oe = ae * b[k] + se[k];
                sd[k] = a[k] - ad * od;
                se[k] = b[k] - ae * oe;
                a[k] = od;
                b[k] = oe;
            }
        }

        for (int s = common; s < numDirect; ++s)
        {
            const float ad = coeffs[s];
            float* sd = state + NumCh * s;

            for (int k = 0; k < NumCh; ++k)
            {
                const float od = ad * a[k] + sd[k];
                sd[k] = a[k] - ad * od;
                a[k] = od;
            }
        }

        for (int s = common; s < numDelayed; ++s)
        {
            const float ae = coeffs[numDirect + s];
            float* se = state + NumCh * (numDirect + s);

            for (int k = 0; k < NumCh; ++k)
            {
                const float oe = ae * b[k] + se[k];
                se[k] = b[k] - ae * oe;
                b[k] = oe;
            }
        }

        // The delayed branch's bare unit delay, then the half-band sum.
        for (int k = 0; k < NumCh; ++k)
        {
            out[k][i] = (held[k] + a[k]) * 0.5f;
            held[k] = b[k];
        }
    }

    for (int k = 0; k < NumCh; ++k)
        delayState[k] = held[k];
}

/** Matches `juce::util::snapToZero`, which JUCE applies to the allpass state
    after every block so a decaying tail cannot leave the filters running on
    denormals for the rest of the session. */
void snapStateToZero(std::vector<float>& state) noexcept
{
    for (auto& v : state)
        juce::dsp::util::snapToZero(v);
}
} // namespace

//==============================================================================
void PolyphaseOversampler::prepare(int numChannels, int numStages, int maxBlockSize)
{
    channels = juce::jlimit(1, 2, numChannels);
    maxBlock = juce::jmax(1, maxBlockSize);
    numStages = juce::jlimit(0, kMaxStages, numStages);

    stages.clear();
    factor = 1;
    totalLatency = 0.0f;
    fractionalDelay = 0.0f;
    ready = false;

    if (numStages <= 0)
        return;

    const auto& design = getDesign(numStages);
    stages.resize(static_cast<size_t>(numStages));

    int rateMultiplier = 1;

    for (int n = 0; n < numStages; ++n)
    {
        auto& stage = stages[static_cast<size_t>(n)];
        const auto& d = design.stages[static_cast<size_t>(n)];

        stage.coeffsUp = d.up;
        stage.coeffsDown = d.down;
        stage.directUp = d.directUp;
        stage.directDown = d.directDown;

        stage.stateUp.assign(stage.coeffsUp.size() * static_cast<size_t>(channels), 0.0f);
        stage.stateDown.assign(stage.coeffsDown.size() * static_cast<size_t>(channels), 0.0f);

        rateMultiplier *= 2;
        stage.buffer.setSize(channels, maxBlock * rateMultiplier, false, false, true);
    }

    factor = rateMultiplier;
    totalLatency = design.totalLatency;
    fractionalDelay = design.fractionalDelay;

    fractionalDelayLine.prepare(
        {0.0, static_cast<juce::uint32>(maxBlock), static_cast<juce::uint32>(channels)});
    fractionalDelayLine.setDelay(fractionalDelay);

    ready = true;
    reset();
}

void PolyphaseOversampler::reset() noexcept
{
    for (auto& s : stages)
    {
        std::fill(s.stateUp.begin(), s.stateUp.end(), 0.0f);
        std::fill(s.stateDown.begin(), s.stateDown.end(), 0.0f);
        s.delayDown.fill(0.0f);
        s.buffer.clear();
    }

    fractionalDelayLine.reset();
}

juce::dsp::AudioBlock<float> PolyphaseOversampler::processSamplesUp(
    const juce::dsp::AudioBlock<const float>& input) noexcept
{
    if (!ready || stages.empty())
        return {};

    const int numCh = juce::jmin(channels, static_cast<int>(input.getNumChannels()));
    int n = static_cast<int>(input.getNumSamples());

    if (numCh <= 0 || n <= 0)
        return {};

    const float* inPtrs[2] = {nullptr, nullptr};
    float* outPtrs[2] = {nullptr, nullptr};

    for (int ch = 0; ch < numCh; ++ch)
        inPtrs[ch] = input.getChannelPointer(static_cast<size_t>(ch));

    for (size_t s = 0; s < stages.size(); ++s)
    {
        auto& stage = stages[s];

        for (int ch = 0; ch < numCh; ++ch)
            outPtrs[ch] = stage.buffer.getWritePointer(ch);

        const int sections = static_cast<int>(stage.coeffsUp.size());

        if (numCh == 2)
            stageUp<2>(inPtrs, outPtrs, n, stage.coeffsUp.data(), sections, stage.directUp, stage.stateUp.data());
        else
            stageUp<1>(inPtrs, outPtrs, n, stage.coeffsUp.data(), sections, stage.directUp, stage.stateUp.data());

        snapStateToZero(stage.stateUp);

        n *= 2;
        for (int ch = 0; ch < numCh; ++ch)
            inPtrs[ch] = outPtrs[ch];
    }

    return juce::dsp::AudioBlock<float>(stages.back().buffer)
        .getSubsetChannelBlock(0, static_cast<size_t>(numCh))
        .getSubBlock(0, static_cast<size_t>(n));
}

void PolyphaseOversampler::processSamplesDown(juce::dsp::AudioBlock<float>& output) noexcept
{
    if (!ready || stages.empty())
        return;

    const int numCh = juce::jmin(channels, static_cast<int>(output.getNumChannels()));
    const int baseSamples = static_cast<int>(output.getNumSamples());

    if (numCh <= 0 || baseSamples <= 0)
        return;

    const float* inPtrs[2] = {nullptr, nullptr};
    float* outPtrs[2] = {nullptr, nullptr};

    // Stage s reads its own 2x buffer and writes into stage s-1's; the first
    // stage writes into the caller's block.
    int n = baseSamples;
    for (size_t s = 0; s + 1 < stages.size(); ++s)
        n *= 2;

    for (int s = static_cast<int>(stages.size()) - 1; s >= 0; --s)
    {
        auto& stage = stages[static_cast<size_t>(s)];

        for (int ch = 0; ch < numCh; ++ch)
        {
            inPtrs[ch] = stage.buffer.getReadPointer(ch);
            outPtrs[ch] = s > 0 ? stages[static_cast<size_t>(s - 1)].buffer.getWritePointer(ch)
                                : output.getChannelPointer(static_cast<size_t>(ch));
        }

        const int sections = static_cast<int>(stage.coeffsDown.size());

        if (numCh == 2)
            stageDown<2>(inPtrs, outPtrs, n, stage.coeffsDown.data(), sections, stage.directDown,
                         stage.stateDown.data(), stage.delayDown.data());
        else
            stageDown<1>(inPtrs, outPtrs, n, stage.coeffsDown.data(), sections, stage.directDown,
                         stage.stateDown.data(), stage.delayDown.data());

        snapStateToZero(stage.stateDown);

        n /= 2;
    }

    if (fractionalDelay > 0.0f)
    {
        auto context = juce::dsp::ProcessContextReplacing<float>(output);
        fractionalDelayLine.process(context);
    }
}
} // namespace ember
