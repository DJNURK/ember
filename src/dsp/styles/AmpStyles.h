#pragma once
#include "dsp/EmberTypes.h"
#include "dsp/DspUtils.h"
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
namespace ampdetail
{
inline constexpr int kMaxAmpChannels = 2;
inline constexpr int kMaxAmpStages   = 4;

/**
    The fixed voicing of one amp cascade.

    Written once by the owning style's constructor and read-only afterwards, so
    the three amp styles share a single per-sample kernel with no virtual
    dispatch and no per-style branching inside the loop.

    Each stage is `trim * drive^exp` of gain into a bounded soft clip, followed
    by inter-stage tone shaping. The exponents sum to roughly one (slightly more
    for the hotter voices), so the small-signal gain of the whole cascade tracks
    the drive control while every stage shares the saturation work — that is what
    makes a four-stage cascade compress and sustain rather than simply square
    the waveform.
*/
struct AmpVoicing
{
    int numStages { 2 };

    float stageTrim     [kMaxAmpStages] { 1.0f, 1.0f, 1.0f, 1.0f };
    float stageDriveExp [kMaxAmpStages] { 0.5f, 0.5f, 0.0f, 0.0f };

    /** Asymmetry of the stage shaper at full drive; 0 = symmetric. */
    float biasAtFullDrive { 0.0f };

    /** Inter-stage mid shaping: a resonant band lifted back into the signal.
        The mix interpolates from `midMixMin` at 0 dB drive to `midMixMax` at
        40 dB, so the voice gets more forward the harder it is pushed. */
    float midHz      { 800.0f };
    float midQ       { 0.7f };
    float midMixMin  { 0.10f };
    float midMixMax  { 0.20f };

    /** Inter-stage one-pole high-pass, to keep the lows from turning to mush as
        the stages pile up. `lowCutHz` <= 0 disables it entirely. */
    float lowCutHz     { 0.0f };
    float lowCutAmount { 0.0f };   ///< 0 .. 1 depth of the high-pass

    /** Final one-pole roll-off standing in for the speaker/output stage. */
    float postLowpassHz { 14000.0f };
};

/** Per-channel, per-stage recursive state. */
struct AmpStageState
{
    dsputil::SvfTPT  mid;
    dsputil::OnePole lowCut;
};

/** All recursive state for one channel. Fixed size: nothing is ever allocated. */
struct AmpChannelState
{
    AmpStageState    stages[kMaxAmpStages];
    dsputil::OnePole post;
};
} // namespace ampdetail

/**
    Shared implementation of the three amp voices: a cascade of bounded
    soft-clipping stages with tone shaping between them.

    Every stage clips to +/-1 before the next stage's gain is applied, so the
    cascade cannot run away however large the drive or the input: the only
    unbounded-looking operation, the pre-gain multiply, always feeds a shaper
    that saturates. The inter-stage filters add at most a fixed, small factor on
    top of that, and a final guard clamp keeps the documented +/-8 contract true
    by construction.

    Derived classes only fill in `voicing` and return a name.
*/
class AmpStyleBase : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    StyleCategory getCategory() const noexcept override { return StyleCategory::Amp; }

protected:
    ampdetail::AmpVoicing voicing {};

private:
    /** Recomputes every filter coefficient. Called from `prepare`, and from
        `process` only when the sample rate has actually changed — never per
        sample. Realtime-safe (no allocation, no locks). */
    void updateCoefficients(double sampleRate) noexcept;

    ampdetail::AmpChannelState channels[ampdetail::kMaxAmpChannels] {};
    double preparedSampleRate { 44100.0 };
};

/** Two gentle stages and mild mid shaping: stays clean until pushed hard. */
class CleanAmpStyle final : public AmpStyleBase
{
public:
    CleanAmpStyle();
    const char* getName() const noexcept override { return "Clean Amp"; }
};

/** Three stages, more gain each, a mid-forward voice and enough asymmetry to bite. */
class CrunchStyle final : public AmpStyleBase
{
public:
    CrunchStyle();
    const char* getName() const noexcept override { return "Crunch"; }
};

/** Four stages, the most gain, a pronounced mid push and a tight low end:
    the compressed, sustaining voice. */
class LeadStyle final : public AmpStyleBase
{
public:
    LeadStyle();
    const char* getName() const noexcept override { return "Lead"; }
};
} // namespace ember
