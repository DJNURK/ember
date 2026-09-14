#pragma once

#include <array>
#include "dsp/EmberTypes.h"
#include "dsp/DspUtils.h"
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    Shared plumbing for the four valve styles.

    All of them are built from the same gesture: bias the signal, push it through
    a soft symmetric saturator, then subtract the bias' own DC contribution
    (`dsputil::biasedTanh`). The bias is what makes a valve read as a valve — it
    breaks the odd-only symmetry of a plain `tanh` and produces the second
    harmonic — and subtracting `tanh(bias)` keeps the residual DC small so the
    band's DC blocker has almost nothing left to do.

    This base owns nothing that runs per sample: it only holds the prepared rate,
    the prepared channel count and the small helpers that clamp the incoming
    block-constant parameters into a range the shapers are provably bounded over.
*/
class TubeStyleBase : public SaturationStyle
{
public:
    StyleCategory getCategory() const noexcept override { return StyleCategory::Tube; }

protected:
    /** Per-channel state is sized for this many channels. The contract says 1 or
        2; a hypothetical third channel is passed through untouched rather than
        indexing off the end of the state arrays. */
    static constexpr int kMaxChannels = 2;

    /** Common `prepare` bookkeeping. Off the audio thread, allocates nothing. */
    void prepareBase(double oversampledSampleRate, int numChannels) noexcept;

    /** Channels we actually have state for. */
    static int usableChannels(int numChannels) noexcept;

    /** Smoothed linear drive, sanitised and clamped to a sane range. */
    static float clampedDrive(const StyleParams& params) noexcept;

    /** `amount01` sanitised and clamped to [0, 1]. */
    static float clampedAmount(const StyleParams& params) noexcept;

    double sampleRate{44100.0};
    int preparedChannels{kMaxChannels};
};

/**
    Clean Tube — soft asymmetric `tanh`.

    A small FIXED bias sits in front of the shaper: it is not derived from drive,
    so the harmonic recipe (a steady sprinkle of second over the odd series)
    stays the same from 0 to 40 dB and only the depth changes. The inner gain is
    below unity so unity drive is genuinely gentle and it takes real drive to get
    dirty.

    Stateless: the curve is memoryless, so there is nothing to size per channel
    and nothing that can be poisoned by a bad buffer.
*/
class CleanTubeStyle final : public TubeStyleBase
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Clean Tube"; }
};

/**
    Warm Tube — thicker, more compressed.

    Two departures from Clean Tube:
      - a gentle low shelf INTO the stage (built from a TPT state-variable low
        pass, so it stays stable and well behaved at every rate), which makes
        bass drive the nonlinearity harder than the top end does;
      - a deeper, drive-dependent bias plus a second soft stage after the
        shaper, which rounds the knee further and reads as compression.

    There is deliberately no matching de-emphasis after the stage: the point of
    the shelf is that the low end comes out saturated and thick, not neutral.
*/
class WarmTubeStyle final : public TubeStyleBase
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Warm Tube"; }

private:
    std::array<dsputil::SvfTPT, kMaxChannels> shelfLowpass{};
};

/**
    Subtle Tube — mix-bus glue.

    The drive knob is scaled well down and the shaper is used with a small inner
    gain, which keeps the operating point in the nearly linear part of the curve:
    a couple of dB of peak rounding and mostly low-order harmonics, with the
    bias small enough that the second harmonic is a tint rather than a colour.

    Stateless, like Clean Tube.
*/
class SubtleTubeStyle final : public TubeStyleBase
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Subtle Tube"; }
};

/**
    Broken Tube — a valve that is not well.

    Two faults, layered:

    (a) BIAS DRIFT. The operating point wanders instead of sitting still. It is
        driven by a heavily smoothed (two cascaded one-poles) envelope of the
        driven signal plus a very slow internal oscillator, so the asymmetry —
        and with it the second-harmonic content — breathes with the programme
        and also on its own. The oscillator phase is accumulated in `double`:
        at 192 kHz x16 the per-sample increment is smaller than a `float` ULP
        near the wrap point, and a float accumulator would simply stall there.

    (b) CROSSOVER DISTORTION. A dead zone around zero, as in a push-pull stage
        whose output devices are biased too cold. It is the smooth rational
        dead zone `x^3 / (x^2 + t^2)` rather than the textbook
        `sign(x) * max(0, |x| - t)`: that one is continuous in value but has a
        slope discontinuity at the edges of the zone, which sprays aliases even
        oversampled. The rational form is C-infinity and monotonic, so it needs
        no ADAA at all — hence `usesAdaa()` stays false.

    The dead zone threshold is proportional to drive, which makes the artifact
    level-relative: it stays audible across the whole drive range instead of
    vanishing as soon as the stage is pushed.
*/
class BrokenTubeStyle final : public TubeStyleBase
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Broken Tube"; }

private:
    std::array<dsputil::OnePole, kMaxChannels> envFast{};
    std::array<dsputil::OnePole, kMaxChannels> envSlow{};
    double lfoPhase{0.0}; ///< [-1, 1), one full cycle per 2 units
    double lfoInc{0.0};
};
} // namespace ember
