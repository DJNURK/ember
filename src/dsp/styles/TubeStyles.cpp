#include "dsp/styles/TubeStyles.h"

#include <cmath>

namespace ember
{
namespace
{
// ---------------------------------------------------------------- safety rails
/** The driven signal is clamped here before any shaper. Every curve below is
    fully saturated long before this, so it costs no character; what it buys is a
    guarantee that `x * x * x` cannot overflow to infinity when a host hands us an
    absurd (but finite) buffer. */
constexpr float kDriveCeiling = 64.0f;

/** Hard contract from `SaturationStyle`: never outside +/- 8. Every style here
    is analytically bounded well inside it; this is the belt to that braces. */
constexpr float kOutputCeiling = 7.5f;

/** Linear drive is 0..40 dB (1..100) by spec; clamp with headroom. */
constexpr float kMaxDriveLin = 1000.0f;

/** Sanitise and bound one output sample. Non-finite becomes silence rather than
    a value that would propagate into the band sum. */
inline float finalise(float y) noexcept
{
    return juce::jlimit(-kOutputCeiling, kOutputCeiling, dsputil::sanitise(y));
}

/** Sanitise and bound one input sample before it reaches a recursive stage. */
inline float prepareInput(float x) noexcept
{
    return juce::jlimit(-kDriveCeiling, kDriveCeiling, dsputil::sanitise(x));
}

// ---------------------------------------------------------------- valve curve
/**
    The shared valve gesture.

    `inner` scales the signal into the shaper and `invInner` scales it back out,
    which selects how much of the curve a unity-level signal actually uses: small
    inner = nearly linear, large inner = hard. `biasedTanh` subtracts `tanh(bias)`
    so the curve passes exactly through the origin and the asymmetry shows up as
    harmonics rather than as a DC step.
*/
inline float valveShape(float driven, float bias, float inner, float invInner) noexcept
{
    return dsputil::biasedTanh(driven * inner, bias) * invInner;
}

// ---------------------------------------------------------------- dead zone
/**
    Smooth crossover dead zone: `x^3 / (x^2 + t^2)`.

    Approaches `x` for |x| >> t and `x^3 / t^2` inside the zone, is monotonic
    (derivative `(x^4 + 3 x^2 t^2) / (x^2 + t^2)^2` is never negative) and has
    continuous derivatives of every order, so unlike the piecewise
    `sign(x) * max(0, |x| - t)` it does not need antiderivative anti-aliasing.
    `threshold` is kept strictly positive by the caller, so the denominator can
    never be zero.
*/
inline float deadZone(float x, float threshold) noexcept
{
    const float x2 = x * x;
    return x * (x2 / (x2 + threshold * threshold));
}

// ---------------------------------------------------------------- style tuning
// Clean Tube
constexpr float kCleanBias     = 0.18f;
constexpr float kCleanInner    = 0.75f;
constexpr float kCleanInvInner = 1.0f / kCleanInner;

// Warm Tube
constexpr float kWarmShelfHz     = 220.0f;
constexpr float kWarmShelfQ      = 0.6f;
/** Low-band boost at 0 dB drive: DC gain is `1 + kWarmShelfMin` (+2.3 dB).

    This one is gain-match critical, not just taste. `StyleCalibrator` measures
    every style's compensation on FLAT noise, but the spec's "+/-1 dB between
    styles at 0 dB drive" is judged on PROGRAMME-like (pink) material, which has
    roughly a third of its energy under this shelf's corner where flat noise has
    almost none. A scalar trim cannot close that gap — the calibrator cancels
    any scalar exactly — so the only lever is how much spectral tilt the style
    adds at unity drive. At 0.45 (+3.2 dB) the measured deviation is 1.04 dB and
    the gate fails; at 0.30 it is 0.67 dB. The shelf at FULL drive is unchanged:
    `kWarmShelfRange` was raised by the same amount, so the endpoint is still
    1.30 and the thick, bass-driven character at high drive is exactly as it
    was. Raise this and re-run "[gainmatch]" before assuming it is free. */
constexpr float kWarmShelfMin    = 0.30f;
constexpr float kWarmShelfRange  = 1.00f;   ///< to 1.30 at 40 dB drive (~+7.2 dB total)
constexpr float kWarmBiasBase    = 0.30f;
constexpr float kWarmBiasRange   = 0.25f;
constexpr float kWarmInner       = 0.9f;
constexpr float kWarmInvInner    = 1.0f / kWarmInner;
constexpr float kWarmSqueeze     = 0.8f;    ///< second, gentler stage: rounds the knee further
constexpr float kWarmInvSqueeze  = 1.0f / kWarmSqueeze;

// Subtle Tube
/** Drive is scaled in dB, not linearly: `driveLin^0.35` is the same as using
    only 35 % of the knob's decibels, so the top of the range lands around 14 dB
    and the curve never leaves its gentle region. A linear fraction would still
    be 20x at the top and clip like everything else. */
constexpr float kSubtleDriveExp   = 0.35f;
constexpr float kSubtleBias       = 0.10f;
constexpr float kSubtleInner      = 0.5f;
constexpr float kSubtleInvInner   = 1.0f / kSubtleInner;

// Broken Tube
constexpr float kBrokenBiasBase   = 0.12f;
constexpr float kBrokenLfoDepth   = 0.30f;
constexpr float kBrokenEnvDepth   = 0.45f;
constexpr float kBrokenDriftMin   = 0.35f;  ///< drift depth at 0 dB drive, scaled up with amount
constexpr float kBrokenDriftRange = 0.65f;
constexpr double kBrokenLfoHz     = 0.13;   ///< one wander every ~7.7 s
constexpr float kBrokenEnvFastSec = 0.050f;
constexpr float kBrokenEnvSlowSec = 0.400f;
constexpr float kBrokenCrossBase  = 0.020f; ///< dead zone half-width, relative to band level
constexpr float kBrokenCrossRange = 0.060f;
constexpr float kBrokenCrossMin   = 1.0e-4f;
} // namespace

// ==================================================================== base
void TubeStyleBase::prepareBase(double oversampledSampleRate, int numChannels) noexcept
{
    jassert(numChannels >= 1 && numChannels <= kMaxChannels);
    sampleRate = oversampledSampleRate > 0.0 ? oversampledSampleRate : 44100.0;
    preparedChannels = juce::jlimit(1, kMaxChannels, numChannels);
}

int TubeStyleBase::usableChannels(int numChannels) noexcept
{
    jassert(numChannels <= kMaxChannels);
    return juce::jmin(numChannels, kMaxChannels);
}

float TubeStyleBase::clampedDrive(const StyleParams& params) noexcept
{
    return juce::jlimit(0.0f, kMaxDriveLin, dsputil::sanitise(params.driveLin));
}

float TubeStyleBase::clampedAmount(const StyleParams& params) noexcept
{
    return juce::jlimit(0.0f, 1.0f, dsputil::sanitise(params.amount01));
}

// ==================================================================== Clean Tube
void CleanTubeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    prepareBase(oversampledSampleRate, numChannels);
    reset();
}

void CleanTubeStyle::reset() noexcept
{
    // Memoryless curve: nothing to clear.
}

void CleanTubeStyle::process(float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    const float drive = clampedDrive(params);
    const int channels = usableChannels(numChannels);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* const samples = channelData[ch];
        if (samples == nullptr)
            continue;

        for (int n = 0; n < numSamples; ++n)
        {
            const float driven = prepareInput(prepareInput(samples[n]) * drive);
            samples[n] = finalise(valveShape(driven, kCleanBias, kCleanInner, kCleanInvInner));
        }
    }
}

// ==================================================================== Warm Tube
void WarmTubeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    prepareBase(oversampledSampleRate, numChannels);

    for (auto& filter : shelfLowpass)
    {
        filter.prepare(sampleRate);
        filter.setCutoffQ(kWarmShelfHz, kWarmShelfQ);
    }

    reset();
}

void WarmTubeStyle::reset() noexcept
{
    for (auto& filter : shelfLowpass)
        filter.reset();
}

void WarmTubeStyle::process(float* const* channelData, int numChannels, int numSamples,
                            const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    const float drive = clampedDrive(params);
    const float amount = clampedAmount(params);
    const int channels = usableChannels(numChannels);

    // Shelf gain expressed as "how much low-pass to add on top of the dry path":
    // DC gain is 1 + shelfMix, everything above the corner passes at unity.
    const float shelfMix = kWarmShelfMin + kWarmShelfRange * amount;
    const float bias = kWarmBiasBase + kWarmBiasRange * amount;

    for (int ch = 0; ch < channels; ++ch)
    {
        float* const samples = channelData[ch];
        if (samples == nullptr)
            continue;

        auto& filter = shelfLowpass[static_cast<size_t>(ch)];
        filter.sanitiseState();

        for (int n = 0; n < numSamples; ++n)
        {
            const float in = prepareInput(samples[n]);
            const float shelved = in + shelfMix * filter.process(in).lp;
            const float driven = prepareInput(shelved * drive);

            const float stage1 = valveShape(driven, bias, kWarmInner, kWarmInvInner);
            const float stage2 = dsputil::fastTanh(stage1 * kWarmSqueeze) * kWarmInvSqueeze;

            samples[n] = finalise(stage2);
        }
    }
}

// ==================================================================== Subtle Tube
void SubtleTubeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    prepareBase(oversampledSampleRate, numChannels);
    reset();
}

void SubtleTubeStyle::reset() noexcept
{
    // Memoryless curve: nothing to clear.
}

void SubtleTubeStyle::process(float* const* channelData, int numChannels, int numSamples,
                              const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    // Only a slice of the drive knob reaches the shaper, so 40 dB here lands
    // around 14 dB. One `pow` per block, none per sample.
    const float drive = std::pow(clampedDrive(params), kSubtleDriveExp);
    const int channels = usableChannels(numChannels);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* const samples = channelData[ch];
        if (samples == nullptr)
            continue;

        for (int n = 0; n < numSamples; ++n)
        {
            const float driven = prepareInput(prepareInput(samples[n]) * drive);
            samples[n] = finalise(valveShape(driven, kSubtleBias, kSubtleInner, kSubtleInvInner));
        }
    }
}

// ==================================================================== Broken Tube
void BrokenTubeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    prepareBase(oversampledSampleRate, numChannels);

    for (auto& env : envFast)
    {
        env.prepare(sampleRate);
        env.setTimeConstant(kBrokenEnvFastSec);
    }

    for (auto& env : envSlow)
    {
        env.prepare(sampleRate);
        env.setTimeConstant(kBrokenEnvSlowSec);
    }

    lfoInc = 2.0 * kBrokenLfoHz / sampleRate;   // phase spans 2 units per cycle

    reset();
}

void BrokenTubeStyle::reset() noexcept
{
    for (auto& env : envFast)
        env.reset();

    for (auto& env : envSlow)
        env.reset();

    lfoPhase = 0.0;
}

void BrokenTubeStyle::process(float* const* channelData, int numChannels, int numSamples,
                              const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    const float drive = clampedDrive(params);
    const float amount = clampedAmount(params);
    const int channels = usableChannels(numChannels);

    const float driftDepth = kBrokenDriftMin + kBrokenDriftRange * amount;
    const float lfoDepth = kBrokenLfoDepth * driftDepth;
    const float envDepth = kBrokenEnvDepth * driftDepth;

    // Dead zone half-width relative to the band signal. Applying it before the
    // drive gain is identical to applying it after with a threshold of
    // `drive * threshold`, so the fault stays level-relative and audible at
    // every drive setting; doing it this way just saves a multiply.
    const float threshold = juce::jmax(kBrokenCrossMin, kBrokenCrossBase + kBrokenCrossRange * amount);

    float* samples[kMaxChannels] {};

    for (int ch = 0; ch < channels; ++ch)
    {
        samples[ch] = channelData[ch];

        // One bad buffer must not be able to park the drift off in NaN for the
        // rest of the session.
        const size_t idx = static_cast<size_t>(ch);
        if (! (dsputil::isFinite(envFast[idx].getState()) && dsputil::isFinite(envSlow[idx].getState())))
        {
            envFast[idx].reset();
            envSlow[idx].reset();
        }
    }

    if (! std::isfinite(lfoPhase))
        lfoPhase = 0.0;

    // Samples outer, channels inner: the oscillator is the valve's own supply
    // wandering, so both channels see the same phase.
    for (int n = 0; n < numSamples; ++n)
    {
        lfoPhase += lfoInc;
        if (lfoPhase >= 1.0)
            lfoPhase -= 2.0;

        // Parabolic sine: 4p(1 - |p|) over p in [-1, 1). Peak error ~0.06 against
        // a real sine, which no one can hear at 0.13 Hz, and it costs no call.
        const float phase = static_cast<float>(lfoPhase);
        const float lfo = 4.0f * phase * (1.0f - std::abs(phase));

        for (int ch = 0; ch < channels; ++ch)
        {
            float* const channel = samples[ch];
            if (channel == nullptr)
                continue;

            const size_t idx = static_cast<size_t>(ch);

            const float in = prepareInput(channel[n]);
            const float crossed = deadZone(in, threshold) * drive;

            // Heavily smoothed: two cascaded one-poles, so the drift creeps
            // rather than tracking the waveform.
            const float envelope = envSlow[idx].process(envFast[idx].process(std::abs(crossed)));
            const float envTerm = envelope / (1.0f + envelope);   // bounded in [0, 1)

            const float bias = kBrokenBiasBase + lfoDepth * lfo + envDepth * envTerm;

            channel[n] = finalise(dsputil::biasedTanh(crossed, bias));
        }
    }
}
} // namespace ember
