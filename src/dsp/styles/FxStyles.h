#pragma once

#include <array>
#include <cmath>

#include "dsp/EmberTypes.h"
#include "dsp/DspUtils.h"
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    Helpers shared by the FX family.

    Everything here is header-inline, branch-light and allocation-free: the
    per-sample loops in FxStyles.cpp call these on the audio thread.
*/
namespace fxdetail
{
/** These styles are used inside a band, which is mono or stereo. */
inline constexpr int kMaxChannels = 2;

/** Final safety net applied to every sample an FX style writes: finite, and
    inside the +/- 8 bound the SaturationStyle contract guarantees. */
inline float finish(float y) noexcept
{
    return dsputil::hardClip(dsputil::sanitise(y), 8.0f);
}

inline float clampAmount(float amount01) noexcept
{
    return juce::jlimit(0.0f, 1.0f, dsputil::sanitise(amount01));
}

/** 0.001 .. 316 covers -60 .. +50 dB: far wider than the 0..40 dB spec range,
    but it means a corrupt parameter cannot turn into an infinite pre-gain. */
inline float clampDrive(float driveLin) noexcept
{
    return juce::jlimit(0.001f, 316.0f, dsputil::sanitise(driveLin));
}

/** Coefficient of a one-pole lowpass at `cutoffHz`; the cutoff is clamped into
    a safe range so no host sample rate can produce an unstable pole. */
inline float onePoleCoeff(float cutoffHz, double rate) noexcept
{
    const float sr = juce::jmax(8000.0f, static_cast<float>(rate));
    const float fc = juce::jlimit(5.0f, sr * 0.45f, cutoffHz);
    return std::exp(-juce::MathConstants<float>::twoPi * fc / sr);
}

/** Coefficient of a one-pole smoother with time constant `seconds`. */
inline float timeCoeff(float seconds, double rate) noexcept
{
    const float sr = juce::jmax(8000.0f, static_cast<float>(rate));
    return std::exp(-1.0f / (juce::jmax(1.0e-5f, seconds) * sr));
}

/** Clear a smoother whose state has gone non-finite. Called once per block, so
    a single bad host buffer cannot poison a recursive stage for good. */
inline void sanitiseOnePole(dsputil::OnePole& p) noexcept
{
    if (! dsputil::isFinite(p.getState()))
        p.reset();
}

/** Fold any finite phase back into [0, 1). Used once per block on restored
    state; the inner loop uses the cheaper conditional wrap below. */
inline float wrapPhase(float phase) noexcept
{
    const float p = dsputil::sanitise(phase);
    if (p >= 0.0f && p < 1.0f)
        return p;

    const float wrapped = p - std::floor(p);
    return (wrapped >= 0.0f && wrapped < 1.0f) ? wrapped : 0.0f;
}

/**
    sin(2 * pi * phase) for a phase measured in turns, to within ~1.1e-3.

    The classic two-term parabolic sine: a parabola through (0, 0), (0.5, 1),
    (1, 0) plus one self-correcting refinement pass. It costs a handful of
    multiplies against roughly 100 cycles for `std::sin`, which matters because
    this runs per sample per channel at up to 16x oversampling.

    The result is bounded by exactly 1 for any phase in [0, 1]: the parabola
    peaks at 1, and the refinement term `0.225 * (y * |y| - y)` is <= 0 there.
*/
inline float fastSineTurns(float phase01) noexcept
{
    const float p = 2.0f * phase01 - 1.0f;                     // [-1, 1)
    const float y = 4.0f * p * (1.0f - std::abs(p));           // ~= sin(pi * p)
    const float refined = y + 0.225f * (y * std::abs(y) - y);
    return -refined;                                           // sin(pi*p + pi)
}
} // namespace fxdetail

//==============================================================================
/**
    Shimmer — envelope-tracked ring modulation, mixed back over the dry band.

    Pure ring modulation is atonal: the sum and difference sidebands of a fixed
    carrier sit in no key at all. Three things keep this one musical.

    - It is amplitude modulation, not ring modulation. The output is
      `sat + depth * high * osc`, i.e. the carrier term survives at full level
      and the ring product is *added* rather than substituted. At `depth = 0`
      the stage is exactly the (saturated) dry band, and the sidebands only ever
      sit underneath it.
    - Only the top of the band is modulated. A fixed one-pole split at 700 Hz
      takes the complement `sat - lowpass(sat)` as the modulated path, so weight
      and pitch stay where they were and the sparkle lands above them.
    - The carrier pitch tracks the material. A bounded, drive-independent
      envelope follower lifts the oscillator from a resting ~1.2 kHz into the
      sparkle region as the band gets louder, so the artefact moves with the
      performance instead of sitting on top of it as a static whistle.

    Drive is the only control the spec gives it, and it does three things: it
    sets the blend `depth`, it sets how far the envelope is allowed to lift the
    carrier (35% of the span at 0 dB drive, all of it at 40 dB), and it sets the
    carrier's character, waveshaping the sine progressively closer to a square
    (more sidebands, harder sparkle) as it is pushed.

    The phase accumulator lives in per-channel member state and is wrapped, not
    recomputed from a block-relative index, so the carrier is continuous across
    block boundaries at any block size — a discontinuity there would click on
    every buffer. The two channels are detuned by ~2.4 cents worth and start a
    quarter-turn apart, which spreads the shimmer rather than collapsing it to
    the centre.
*/
class ShimmerStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override       { return "Shimmer"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::FX; }

private:
    struct ShimmerChannel
    {
        dsputil::OnePole env;    ///< bounded amplitude tracker -> carrier pitch
        dsputil::OnePole tilt;   ///< 700 Hz split; only the complement is modulated
        float phase { 0.0f };    ///< carrier phase in turns, always in [0, 1)
    };

    double sampleRate { 44100.0 };
    std::array<ShimmerChannel, fxdetail::kMaxChannels> state {};
};

//==============================================================================
/**
    Breathe — an envelope-swept resonant filter wrapped around a saturator.

    An attack/release follower (4 ms / 160 ms — fast enough to catch a transient,
    slow enough that the recovery is audible as a breath) drives the cutoff of a
    resonant tilt filter ahead of the shaper, and of a resonant bandpass behind
    it. Loud material throws the filter open and the band brightens and bites;
    as the material decays the filter closes again, so the band inhales and
    exhales with the performance.

    The pre-filter is a *tilt*, not a plain lowpass: the output is
    `lp + through * (x - lp)`. Because the TPT SVF satisfies `x == lp + k*bp + hp`
    exactly, `x - lp` is the exact complement of the lowpass, so at `through = 1`
    the filter is bit-transparent regardless of Q, and at `through = 0.15` the
    band above the cutoff is pushed down ~16 dB but never silenced. That matters
    because this style runs inside *any* of the six bands: a plain sweeping
    lowpass would simply mute a 10 kHz band.

    Drive controls how far the sweep travels (2.5 up to 5.5 octaves above the
    220 Hz resting cutoff), how far the band closes when quiet, the resonance,
    and how hard the stage itself is driven. The shaper is `biasedTanh`, whose
    asymmetry grows with drive for a slightly vocal even-harmonic colour; the DC
    that produces is the band chain's business, not this style's.

    `SvfTPT` is specified here rather than a direct-form biquad precisely
    because these coefficients move: a topology-preserving SVF stays stable and
    zipper-free under modulation, where a biquad's direct-form state has no
    meaning after its coefficients change. Coefficients are recomputed once per
    `kControlBlockSize` (32-sample) control block, not per sample, and the
    cutoff is clamped into [20 Hz, 0.45 * sampleRate] before it ever reaches the
    filter, so no envelope excursion and no sample rate can push it into or past
    Nyquist.
*/
class BreatheStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override       { return "Breathe"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::FX; }

private:
    struct BreatheChannel
    {
        dsputil::SvfTPT sweep;   ///< resonant tilt ahead of the shaper
        dsputil::SvfTPT body;    ///< resonant bandpass behind it
        float env { 0.0f };      ///< attack/release follower, bounded to [0, 1]
    };

    double sampleRate { 44100.0 };
    std::array<BreatheChannel, fxdetail::kMaxChannels> state {};
};
} // namespace ember
