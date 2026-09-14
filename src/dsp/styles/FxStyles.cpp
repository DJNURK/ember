#include "dsp/styles/FxStyles.h"

#include <algorithm>

namespace ember
{
namespace
{
/** Hard ceiling on the driven signal before it reaches a recursive stage.

    The output stages saturate long before this, so the clamp is inaudible; it
    exists so that a pathological host buffer (1e30 and friends) cannot hand a
    filter an infinity that no amount of per-block sanitising can undo. */
constexpr float kDrivenClamp = 64.0f;

// ---------------------------------------------------------------- Shimmer
/** Bounded, drive-independent detector gain. Tracking the *input* rather than
    the driven signal keeps the carrier moving with the performance at every
    drive setting, instead of parking at the top of its range once the shaper
    saturates. */
constexpr float kShimmerDetectGain = 1.6f;
constexpr float kShimmerEnvSeconds = 0.030f;   ///< carrier glide, not a transient tracker
constexpr float kShimmerSplitHz    = 700.0f;   ///< below this the band is left alone
constexpr float kShimmerBaseHz     = 1200.0f;  ///< resting carrier pitch
constexpr float kShimmerSpanHz     = 3200.0f;  ///< envelope lift on top of the base
constexpr float kShimmerMinHz      = 200.0f;   ///< floor after clamping
constexpr float kShimmerDepthMin   = 0.10f;    ///< blend at 0 dB drive
constexpr float kShimmerDepthMax   = 0.85f;    ///< ... and at 40 dB
constexpr float kShimmerEdgeMin    = 0.30f;    ///< carrier waveshaping: ~pure sine
constexpr float kShimmerEdgeRange  = 3.30f;    ///< ... up to a softly squared carrier
/** Highest carrier increment in turns per sample; 0.45 keeps it under Nyquist
    even before the explicit frequency clamp. */
constexpr float kShimmerMaxInc = 0.45f;

/** ~2.4 cents of detune between the channels, so the sparkle spreads instead of
    collapsing to the phantom centre. */
constexpr float kShimmerDetune[fxdetail::kMaxChannels] = { 1.0f, 1.0014f };
/** A quarter turn apart at reset, for the same reason. */
constexpr float kShimmerInitialPhase[fxdetail::kMaxChannels] = { 0.0f, 0.25f };

// ---------------------------------------------------------------- Breathe
constexpr float kBreatheDetectGain    = 2.0f;
constexpr float kBreatheAttackSeconds = 0.004f;   ///< catches the transient
constexpr float kBreatheRelSeconds    = 0.160f;   ///< ... and exhales slowly
constexpr float kBreatheBaseHz        = 220.0f;   ///< resting cutoff (envelope at zero)
constexpr float kBreatheSweepOctaves  = 5.5f;     ///< full travel at full drive
constexpr float kBreatheDepthMin      = 0.45f;    ///< fraction of that travel at 0 dB drive
constexpr float kBreatheDepthRange    = 0.55f;
constexpr float kBreatheCloseMin      = 0.25f;    ///< how far the tilt shuts at 0 dB drive
constexpr float kBreatheCloseRange    = 0.60f;    ///< ... max 0.85, i.e. -16 dB, never mute
constexpr float kBreatheSweepQMin     = 0.60f;
constexpr float kBreatheSweepQRange   = 2.60f;    ///< max Q 3.2: audible, nowhere near self-osc
constexpr float kBreatheBodyQ         = 2.50f;
constexpr float kBreatheResMixMin     = 0.08f;
constexpr float kBreatheResMixRange   = 0.22f;    ///< max 0.30 of a Q=2.5 bandpass
constexpr float kBreatheBiasMax       = 0.25f;    ///< even-harmonic colour at full drive
constexpr float kBreatheMinCutoffHz   = 20.0f;    ///< spec floor
constexpr float kBreatheNyquistFrac   = 0.45f;    ///< spec ceiling

/** Common entry guard: returns the number of channels to touch, or 0 if there
    is nothing to do. Keeps every `process` honest about empty and oversized
    buffers without repeating the same four lines. */
inline int usableChannels(float* const* channelData, int numChannels, int numSamples) noexcept
{
    if (channelData == nullptr || numSamples <= 0 || numChannels <= 0)
        return 0;

    jassert(numChannels <= fxdetail::kMaxChannels);
    return juce::jmin(numChannels, fxdetail::kMaxChannels);
}
} // namespace

//==============================================================================
void ShimmerStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                           [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= fxdetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);

    for (auto& s : state)
    {
        s.env.prepare(sampleRate);
        s.tilt.prepare(sampleRate);
    }

    reset();
}

void ShimmerStyle::reset() noexcept
{
    for (size_t ch = 0; ch < state.size(); ++ch)
    {
        auto& s = state[ch];
        s.env.reset();
        s.tilt.reset();
        s.phase = kShimmerInitialPhase[ch];
    }
}

void ShimmerStyle::process(float* const* channelData, int numChannels, int numSamples,
                           const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0);   // prepare() owns the rate

    const float amount = fxdetail::clampAmount(params.amount01);
    const float drive  = fxdetail::clampDrive(params.driveLin);

    // Drive's two jobs: how much ring product is blended back, and how hard the
    // carrier itself is shaped. `edgeNorm` renormalises the shaped carrier so it
    // still peaks at exactly 1 — the output bound depends on that.
    const float depth    = kShimmerDepthMin + (kShimmerDepthMax - kShimmerDepthMin) * amount;
    const float edge     = kShimmerEdgeMin + kShimmerEdgeRange * amount;
    const float edgeNorm = 1.0f / juce::jmax(1.0e-3f, dsputil::fastTanh(edge));

    const float envCoeff  = fxdetail::timeCoeff(kShimmerEnvSeconds, sampleRate);
    const float tiltCoeff = fxdetail::onePoleCoeff(kShimmerSplitHz, sampleRate);
    const float invSr     = 1.0f / static_cast<float>(sampleRate);
    const float maxHz     = juce::jmax(kShimmerMinHz, static_cast<float>(sampleRate) * 0.45f);
    const float spanHz    = kShimmerSpanHz * (0.35f + 0.65f * amount);

    for (int ch = 0; ch < chans; ++ch)
    {
        float* const data = channelData[ch];
        if (data == nullptr)
            continue;

        auto& s = state[static_cast<size_t>(ch)];
        fxdetail::sanitiseOnePole(s.env);
        fxdetail::sanitiseOnePole(s.tilt);
        s.env.setCoefficient(envCoeff);
        s.tilt.setCoefficient(tiltCoeff);

        // Restored from member state, never rebuilt from a block-relative index:
        // that is what makes the carrier continuous across block boundaries.
        float phase = fxdetail::wrapPhase(s.phase);
        const float baseHz = kShimmerBaseHz * kShimmerDetune[static_cast<size_t>(ch)];

        for (int start = 0; start < numSamples; start += kControlBlockSize)
        {
            const int blockEnd = juce::jmin(numSamples, start + kControlBlockSize);

            // Carrier pitch is refreshed once per control block from the envelope
            // as it stood at the block boundary; a 30 ms glide cannot move far
            // enough in 32 samples for that to be audible as stepping.
            const float freq = juce::jlimit(kShimmerMinHz, maxHz,
                                            baseHz + spanHz * s.env.getState());
            const float inc  = juce::jlimit(0.0f, kShimmerMaxInc, freq * invSr);

            for (int i = start; i < blockEnd; ++i)
            {
                const float x   = dsputil::sanitise(data[i]);
                const float sat = dsputil::fastTanh(x * drive);          // |sat| <= 1

                // Detector runs on the raw input, so the carrier tracks the
                // performance rather than the shaper's saturation point.
                s.env.process(std::abs(dsputil::fastTanh(x * kShimmerDetectGain)));

                const float low  = s.tilt.process(sat);
                const float high = sat - low;                            // |high| <= 2

                const float osc = edgeNorm * dsputil::fastTanh(edge * fxdetail::fastSineTurns(phase));

                phase += inc;
                if (phase >= 1.0f)
                    phase -= 1.0f;

                // |sat| + depth * |high| * |osc| <= 1 + 0.85 * 2 = 2.7.
                data[i] = fxdetail::finish(sat + depth * high * osc);
            }
        }

        s.phase = fxdetail::wrapPhase(phase);
    }
}

//==============================================================================
void BreatheStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                           [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= fxdetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);

    for (auto& s : state)
    {
        s.sweep.prepare(sampleRate);
        s.body.prepare(sampleRate);
        s.sweep.setCutoffQ(kBreatheBaseHz, kBreatheSweepQMin);
        s.body.setCutoffQ(kBreatheBaseHz, kBreatheBodyQ);
    }

    reset();
}

void BreatheStyle::reset() noexcept
{
    for (auto& s : state)
    {
        s.sweep.reset();
        s.body.reset();
        s.env = 0.0f;
    }
}

void BreatheStyle::process(float* const* channelData, int numChannels, int numSamples,
                           const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0);

    const float amount = fxdetail::clampAmount(params.amount01);
    const float drive  = fxdetail::clampDrive(params.driveLin);

    const float attackCoeff  = fxdetail::timeCoeff(kBreatheAttackSeconds, sampleRate);
    const float releaseCoeff = fxdetail::timeCoeff(kBreatheRelSeconds, sampleRate);

    // Drive sets how far the filter travels, how far it shuts, how resonant it
    // is, and how asymmetric the shaper gets.
    const float sweepDepth = kBreatheDepthMin + kBreatheDepthRange * amount;
    const float through    = 1.0f - (kBreatheCloseMin + kBreatheCloseRange * amount);
    const float sweepQ     = kBreatheSweepQMin + kBreatheSweepQRange * amount;
    const float resMix     = kBreatheResMixMin + kBreatheResMixRange * amount;
    const float bias       = kBreatheBiasMax * amount;
    const float octaves    = kBreatheSweepOctaves * sweepDepth;

    // Hard spec bound: the cutoff may never leave [20 Hz, 0.45 * sampleRate].
    const float maxHz = juce::jmax(kBreatheMinCutoffHz,
                                   static_cast<float>(sampleRate) * kBreatheNyquistFrac);

    for (int ch = 0; ch < chans; ++ch)
    {
        float* const data = channelData[ch];
        if (data == nullptr)
            continue;

        auto& s = state[static_cast<size_t>(ch)];
        float env = juce::jlimit(0.0f, 1.0f, dsputil::sanitise(s.env));

        for (int start = 0; start < numSamples; start += kControlBlockSize)
        {
            const int blockEnd = juce::jmin(numSamples, start + kControlBlockSize);

            // Once per control block, never per sample: recomputing a tangent and
            // four coefficients 705,600 times a second would cost more than the
            // rest of the style put together, and the SVF is topology-preserving
            // precisely so that stepping them here is inaudible.
            s.sweep.sanitiseState();
            s.body.sanitiseState();

            const float cutoff = juce::jlimit(kBreatheMinCutoffHz, maxHz,
                                              kBreatheBaseHz * std::exp2(octaves * env));
            s.sweep.setCutoffQ(cutoff, sweepQ);
            s.body.setCutoffQ(cutoff, kBreatheBodyQ);

            for (int i = start; i < blockEnd; ++i)
            {
                const float x      = dsputil::sanitise(data[i]);
                const float driven = juce::jlimit(-kDrivenClamp, kDrivenClamp, x * drive);

                // Bounded, drive-independent detector: the band breathes with the
                // material, and drive only decides how deeply it breathes.
                const float det   = std::abs(dsputil::fastTanh(x * kBreatheDetectGain));
                const float coeff = det > env ? attackCoeff : releaseCoeff;
                env = det + coeff * (env - det);

                // Tilt, not a plain lowpass: `driven - lp` is the exact complement
                // of the SVF lowpass, so `through == 1` reconstructs `driven`
                // bit-for-bit and no band is ever muted outright.
                const float lp     = s.sweep.process(driven).lp;
                const float tilted = lp + through * (driven - lp);

                // Asymmetric bounded shaper: |sat| <= 1 + |tanh(bias)| < 1.25.
                const float sat = dsputil::biasedTanh(tilted, bias);

                // Resonance behind the stage as well as in front of it, which is
                // what gives the sweep its vocal quality.
                const float res = s.body.process(sat).bp;

                data[i] = fxdetail::finish(sat + resMix * res);
            }
        }

        s.env = juce::jlimit(0.0f, 1.0f, dsputil::sanitise(env));
    }
}
} // namespace ember
