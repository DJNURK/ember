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

    `numDirect` is derived from the total the way JUCE derives it —
    arithmetically, not from `directPath.size()`. The two agree for every design
    this call can return, but deriving it the same way means the split cannot
    drift even if a future JUCE changes the structure. */
void packStructure(const juce::dsp::FilterDesign<float>::IIRPolyphaseAllpassStructure& s, std::vector<float>& out,
                   int& numDirect)
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
    // computes the fractional part, and sizes the throwaway for a single sample.
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

/** Lay one packed cascade pair out four lanes wide:
    [ch0 direct, ch0 delayed, ch1 direct, ch1 delayed] per section.

    The delayed cascade is never longer than the direct one and is at most one
    section shorter; the gap is filled with alpha = 1, which is an exact
    identity for a section whose state is zero — out = 1 * in + 0 = in, and the
    new state is in - 1 * in = 0, so it stays zero forever. */
void buildLanes(const std::vector<float>& coeffs, int numDirect, float* lanes, float* negLanes,
                int& numSections) noexcept
{
    const int numDelayed = static_cast<int>(coeffs.size()) - numDirect;
    numSections = juce::jmin(PolyphaseOversampler::kMaxSections, juce::jmax(numDirect, numDelayed));
    jassert(numSections == juce::jmax(numDirect, numDelayed));

    for (int s = 0; s < numSections; ++s)
    {
        const float direct = s < numDirect ? coeffs[static_cast<size_t>(s)] : 1.0f;
        const float delayed = s < numDelayed ? coeffs[static_cast<size_t>(numDirect + s)] : 1.0f;

        lanes[4 * s + 0] = direct;
        lanes[4 * s + 1] = delayed;
        lanes[4 * s + 2] = direct;
        lanes[4 * s + 3] = delayed;

        for (int l = 0; l < 4; ++l)
            negLanes[4 * s + l] = -lanes[4 * s + l];
    }
}

/** Push four lanes through the cascade, in place.

    `v`, `alpha`, `negAlpha` and `state` are all 16-byte aligned, and the
    per-section offsets stay aligned because the stride is a whole register.

    `multiplyAdd` rather than `a * x + st`, and `-a` rather than a subtraction,
    because the two are not the same number. JUCE's scalar filter writes
    `alpha * input + v` and `input - alpha * output` as single expressions, which
    the compiler contracts into fused multiply-adds — one rounding each.
    SIMDRegister's `operator*` and `operator+` are separate inlined calls, so
    writing the same algebra with them rounds twice and drifts from JUCE by
    about 3.6e-7 on a full-scale signal. Harmless, but the equivalence test in
    tests/test_aliasing.cpp is a much better guard when it can demand zero. */
inline void cascade(float* v, const float* alpha, const float* negAlpha, float* state, int numSections) noexcept
{
#if JUCE_USE_SIMD
    using Vec = juce::dsp::SIMDRegister<float>;

    auto x = Vec::fromRawArray(v);

    for (int s = 0; s < numSections; ++s)
    {
        const auto a = Vec::fromRawArray(alpha + 4 * s);
        const auto negA = Vec::fromRawArray(negAlpha + 4 * s);
        const auto st = Vec::fromRawArray(state + 4 * s);

        const auto o = Vec::multiplyAdd(st, a, x);                  // st + a * x
        Vec::multiplyAdd(x, negA, o).copyToRawArray(state + 4 * s); // x - a * o
        x = o;
    }

    x.copyToRawArray(v);
#else
    juce::ignoreUnused(negAlpha);

    // Four independent chains, scalar. Slower than the register form, but the
    // same arithmetic and the same dependency structure, so still a long way
    // ahead of running one channel at a time.
    for (int s = 0; s < numSections; ++s)
    {
        for (int l = 0; l < 4; ++l)
        {
            const float a = alpha[4 * s + l];
            const float o = a * v[l] + state[4 * s + l];
            state[4 * s + l] = v[l] - a * o;
            v[l] = o;
        }
    }
#endif
}

/** The two lanes a mono instance needs, scalar.

    Mono could just as well run the four-lane form with the upper pair carrying
    zeros, and at the same cost. It does not, because the vector multiply-add
    rounds differently from the scalar one, and keeping mono on the scalar
    expression keeps it bit-identical to what JUCE produces. Lanes 2 and 3 of
    the shared state are simply never touched, so mono and stereo blocks cannot
    disturb each other. */
inline void cascadeMono(float* v, const float* alpha, float* state, int numSections) noexcept
{
    for (int s = 0; s < numSections; ++s)
    {
        for (int l = 0; l < 2; ++l)
        {
            const float a = alpha[4 * s + l];
            const float o = a * v[l] + state[4 * s + l];
            state[4 * s + l] = v[l] - a * o;
            v[l] = o;
        }
    }
}

/** One 2x upsampling stage: `n` samples in, `2 * n` out. */
template<bool Stereo>
void stageUp(const float* const* in, float* const* out, int n, const float* alpha, const float* negAlpha,
             int numSections, float* state) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x0 = in[0][i];
        const float x1 = Stereo ? in[1][i] : 0.0f;

        alignas(16) float v[4] = {x0, x0, x1, x1};

        if constexpr (Stereo)
            cascade(v, alpha, negAlpha, state, numSections);
        else
            cascadeMono(v, alpha, state, numSections);

        out[0][2 * i] = v[0];
        out[0][2 * i + 1] = v[1];

        if constexpr (Stereo)
        {
            out[1][2 * i] = v[2];
            out[1][2 * i + 1] = v[3];
        }
    }
}

/** One 2x downsampling stage: `2 * n` samples in, `n` out. */
template<bool Stereo>
void stageDown(const float* const* in, float* const* out, int n, const float* alpha, const float* negAlpha,
               int numSections, float* state, float* delayState) noexcept
{
    float held0 = delayState[0];
    float held1 = delayState[1];

    for (int i = 0; i < n; ++i)
    {
        alignas(16) float v[4] = {in[0][2 * i], in[0][2 * i + 1], Stereo ? in[1][2 * i] : 0.0f,
                                  Stereo ? in[1][2 * i + 1] : 0.0f};

        if constexpr (Stereo)
            cascade(v, alpha, negAlpha, state, numSections);
        else
            cascadeMono(v, alpha, state, numSections);

        // The delayed branch's bare unit delay, then the half-band sum.
        out[0][i] = (held0 + v[0]) * 0.5f;
        held0 = v[1];

        if constexpr (Stereo)
        {
            out[1][i] = (held1 + v[2]) * 0.5f;
            held1 = v[3];
        }
    }

    delayState[0] = held0;
    delayState[1] = held1;
}

/** Matches `juce::dsp::util::snapToZero`, which JUCE applies to the allpass
    state after every block so a decaying tail cannot leave the filters running
    on denormals for the rest of the session. */
void snapStateToZero(float* state, int numSections) noexcept
{
    for (int i = 0; i < 4 * numSections; ++i)
        juce::dsp::util::snapToZero(state[i]);
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
    compAlpha = 0.0f;
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

        buildLanes(d.up, d.directUp, stage.coeffsUp.data(), stage.negCoeffsUp.data(), stage.sectionsUp);
        buildLanes(d.down, d.directDown, stage.coeffsDown.data(), stage.negCoeffsDown.data(), stage.sectionsDown);

        rateMultiplier *= 2;
        stage.buffer.setSize(channels, maxBlock * rateMultiplier, false, false, true);
    }

    factor = rateMultiplier;
    totalLatency = design.totalLatency;
    fractionalDelay = design.fractionalDelay;

    // JUCE's Thiran path always lands on delayInt == 0 for a delay in
    // (0.618, 1.618], so the whole line collapses to this one coefficient.
    compAlpha = (1.0f - fractionalDelay) / (1.0f + fractionalDelay);

    ready = true;
    reset();
}

void PolyphaseOversampler::reset() noexcept
{
    for (auto& s : stages)
    {
        s.stateUp.fill(0.0f);
        s.stateDown.fill(0.0f);
        s.delayDown.fill(0.0f);
        s.buffer.clear();
    }

    compPrevIn.fill(0.0f);
    compPrevOut.fill(0.0f);
}

juce::dsp::AudioBlock<float>
PolyphaseOversampler::processSamplesUp(const juce::dsp::AudioBlock<const float>& input) noexcept
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

    for (auto& stage : stages)
    {
        for (int ch = 0; ch < numCh; ++ch)
            outPtrs[ch] = stage.buffer.getWritePointer(ch);

        if (numCh == 2)
            stageUp<true>(inPtrs, outPtrs, n, stage.coeffsUp.data(), stage.negCoeffsUp.data(), stage.sectionsUp,
                          stage.stateUp.data());
        else
            stageUp<false>(inPtrs, outPtrs, n, stage.coeffsUp.data(), stage.negCoeffsUp.data(), stage.sectionsUp,
                           stage.stateUp.data());

        snapStateToZero(stage.stateUp.data(), stage.sectionsUp);

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
    // stage writes into the caller's block. `n` is the stage's OUTPUT count.
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

        if (numCh == 2)
            stageDown<true>(inPtrs, outPtrs, n, stage.coeffsDown.data(), stage.negCoeffsDown.data(), stage.sectionsDown,
                            stage.stateDown.data(), stage.delayDown.data());
        else
            stageDown<false>(inPtrs, outPtrs, n, stage.coeffsDown.data(), stage.negCoeffsDown.data(),
                             stage.sectionsDown, stage.stateDown.data(), stage.delayDown.data());

        snapStateToZero(stage.stateDown.data(), stage.sectionsDown);

        n /= 2;
    }

    if (fractionalDelay > 0.0f)
    {
        const float alpha = compAlpha;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = output.getChannelPointer(static_cast<size_t>(ch));
            float prevIn = compPrevIn[static_cast<size_t>(ch)];
            float prevOut = compPrevOut[static_cast<size_t>(ch)];

            for (int i = 0; i < baseSamples; ++i)
            {
                const float x = d[i];
                const float y = prevIn + alpha * (x - prevOut);
                prevIn = x;
                prevOut = y;
                d[i] = y;
            }

            compPrevIn[static_cast<size_t>(ch)] = prevIn;
            compPrevOut[static_cast<size_t>(ch)] = prevOut;
        }
    }
}
} // namespace ember
