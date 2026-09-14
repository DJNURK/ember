#pragma once

#include <array>
#include <cmath>

#include "dsp/DspUtils.h"
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    Helpers shared by the tape / transformer family.

    Everything here is header-inline, branch-light and allocation-free: the
    per-sample loops in TapeStyles.cpp call these on the audio thread.
*/
namespace tapedetail
{
/** These styles are used inside a band, which is mono or stereo. */
inline constexpr int kMaxChannels = 2;

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
    if (!dsputil::isFinite(p.getState()))
        p.reset();
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

/** Bound one input sample before it reaches a recursive stage.

    `sanitise` alone is not enough for a filter that has gain: a *finite* host
    sample near the float maximum can still overflow the first multiply to
    infinity, and inf - inf is NaN, which then sits in the filter state for the
    rest of the block. +/- 64 is ~+36 dBFS — far above anything a band can
    legitimately carry, and every shaper in this file is fully saturated long
    before it, so the bound costs no character while making overflow
    impossible. (Same rail, and the same reasoning, as TubeStyles.) */
inline float clampInput(float x) noexcept
{
    return dsputil::hardClip(dsputil::sanitise(x), 64.0f);
}

/** Final safety net applied to every sample a style writes: finite, and inside
    the +/- 8 bound the SaturationStyle contract guarantees. */
inline float finish(float y) noexcept
{
    return dsputil::hardClip(dsputil::sanitise(y), 8.0f);
}
} // namespace tapedetail

//==============================================================================
/**
    Clean Tape — soft symmetric saturation plus progressive high-frequency loss.

    The curve is odd-symmetric (a scaled `tanh`), so it generates odd harmonics
    and no DC. On top of that sits a first-order high shelf whose depth and
    corner both close as drive rises: at zero drive the shelf gain is exactly
    unity and the stage is bit-transparent apart from the shaper, and by full
    drive roughly 11 dB of top end has gone — tape loses HF when you push it.
*/
class CleanTapeStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Clean Tape"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Tape; }

private:
    double sampleRate{44100.0};
    std::array<dsputil::OnePole, tapedetail::kMaxChannels> rolloff{};
};

//==============================================================================
/**
    Warm Tape — hysteresis-flavoured saturation with memory.

    Not a Jiles-Atherton model: the magnetisation term is approximated by a
    *smoothed, bounded* image of the recent output, subtracted from the shaper
    input. Two properties make that unconditionally stable rather than
    incidentally stable:

      - the fed-back quantity is a `tanh` output, so |m| <= 1 no matter what the
        input does, and the shaper argument is therefore always bounded; and
      - the feedback path is a one-pole lowpass with loop gain kept below 0.35,
        so the linearised return difference `1 + B*H(z)` has a real part >= 1
        (a one-pole lowpass has a strictly positive-real frequency response),
        i.e. the closed loop can only attenuate — it cannot peak or ring.

    The lag between input and the magnetisation term opens a loop in the
    transfer curve (the hysteresis gesture). A program-dependent gain term
    driven by a bounded envelope then rides the *output* of the curve, so it
    adds to the saturation rather than backing the signal out of it: Warm Tape
    is measurably more compressed than Clean Tape at every drive setting.
*/
class WarmTapeStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Warm Tape"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Tape; }

private:
    struct TapeChannel
    {
        dsputil::OnePole level; ///< bounded envelope: program-dependent squash
        dsputil::OnePole mag;   ///< bounded magnetisation memory
        dsputil::OnePole tone;  ///< high-frequency loss
    };

    double sampleRate{44100.0};
    std::array<TapeChannel, tapedetail::kMaxChannels> state{};
};

//==============================================================================
/**
    Bright Tape — pre-emphasis, shaper, de-emphasis.

    The emphasis pair is a first-order shelf and its exact algebraic inverse,
    both built from the same two coefficients `a` (pole) and `b` (zero):

        pre (z) = A * (1 - b z^-1) / (1 - a z^-1),   A = (1 - a) / (1 - b)
        de  (z) = (1 / A) * (1 - a z^-1) / (1 - b z^-1)

    `pre * de == 1` identically, and both are stable because 0 < a < b < 1. The
    de-emphasis has an impulse response whose l1 norm is exactly 1, so it can
    never expand the bounded shaper output; the pre-emphasis has an l1 norm of
    `2 * shelf - 1` (at most 9), which is why its input is bounded rather than
    only sanitised — see `tapedetail::clampInput`. Highs hit the curve first
    because they arrive up to ~14 dB hotter; at low drive the shaper is
    near-linear and the pair nulls, so the tone stays neutral while the
    character changes.
*/
class BrightTapeStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Bright Tape"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Tape; }

private:
    /** Direct-form-1 state for the emphasis pair. */
    struct EmphasisChannel
    {
        float preX1{0.0f}, preY1{0.0f};
        float deX1{0.0f}, deY1{0.0f};
    };

    double sampleRate{44100.0};
    std::array<EmphasisChannel, tapedetail::kMaxChannels> state{};
};

//==============================================================================
/**
    Transformer — low-frequency saturation with a bass bloom.

    A TPT state-variable filter splits the band at 200 Hz; because the SVF
    satisfies `x == lp + k*bp + hp` exactly, taking the complement `x - lp` as
    the upper path makes the split perfectly reconstructing, so at low drive the
    stage is effectively flat. The lows are pushed up to ~9 dB harder into a
    symmetric (odd-harmonic) `tanh` than the highs and saturate long before
    them, and a gentle resonant bandpass at 95 Hz lifts the saturated lows to
    give the core its bloom.
*/
class TransformerStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Transformer"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Transformer; }

private:
    struct XfmrChannel
    {
        dsputil::SvfTPT split; ///< 200 Hz lowpass; the complement is the upper path
        dsputil::SvfTPT bloom; ///< 95 Hz resonant lift on the saturated lows
    };

    double sampleRate{44100.0};
    std::array<XfmrChannel, tapedetail::kMaxChannels> state{};
};
} // namespace ember
