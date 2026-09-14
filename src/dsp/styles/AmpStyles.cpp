#include "dsp/styles/AmpStyles.h"

#include <cmath>

namespace ember
{
namespace
{
/** Absolute guard on the returned sample. The cascade cannot reach this in
    practice (every stage clips to +/-1 and the tone shaping adds a small fixed
    factor on top), so the clamp is pure insurance for the +/-8 contract. */
constexpr float kOutputGuard = 6.0f;

/** Drive is clamped before it reaches `std::pow`, so a rogue value from the host
    can never turn a stage gain into inf or NaN. 0..40 dB is 1 .. 100 linear. */
constexpr float kMinDriveLin = 0.0625f;
constexpr float kMaxDriveLin = 128.0f;
constexpr float kMaxStageGain = 64.0f;

/** `OnePole::setTimeConstant` wants seconds; a one-pole's -3 dB cutoff sits at
    1 / (2 pi tau). */
inline float onePoleTimeConstantFor(float cutoffHz) noexcept
{
    return 1.0f / (juce::MathConstants<float>::twoPi * juce::jmax(1.0f, cutoffHz));
}

/** Once-per-control-block insurance: one bad host buffer must not be able to
    poison a recursive stage for the rest of the session. */
inline void sanitiseChannelState(ampdetail::AmpChannelState& cs, int numStages) noexcept
{
    for (int s = 0; s < numStages; ++s)
    {
        cs.stages[s].mid.sanitiseState();

        if (! dsputil::isFinite(cs.stages[s].lowCut.getState()))
            cs.stages[s].lowCut.reset();
    }

    if (! dsputil::isFinite(cs.post.getState()))
        cs.post.reset();
}
} // namespace

// ====================================================================== base
void AmpStyleBase::prepare(double oversampledSampleRate,
                           [[maybe_unused]] int maxBlockSize,
                           [[maybe_unused]] int numChannels)
{
    jassert(oversampledSampleRate > 0.0);
    jassert(maxBlockSize >= 0);
    jassert(numChannels >= 1 && numChannels <= ampdetail::kMaxAmpChannels);

    // Nothing is sized by the block length: all state is per channel and per
    // stage, fixed at compile time, so `prepare` never allocates.
    updateCoefficients(oversampledSampleRate);
    reset();
}

void AmpStyleBase::reset() noexcept
{
    for (auto& cs : channels)
    {
        for (auto& stage : cs.stages)
        {
            stage.mid.reset();
            stage.lowCut.reset();
        }

        cs.post.reset();
    }
}

void AmpStyleBase::updateCoefficients(double sampleRate) noexcept
{
    const double sr = (sampleRate >= 8000.0 && sampleRate <= 1.0e7) ? sampleRate : 44100.0;
    const float nyquistLimit = static_cast<float>(sr) * 0.45f;

    const float midF  = juce::jlimit(20.0f,  nyquistLimit, voicing.midHz);
    const float lowF  = juce::jlimit(10.0f,  nyquistLimit,
                                     voicing.lowCutHz > 0.0f ? voicing.lowCutHz : 20.0f);
    const float postF = juce::jlimit(200.0f, nyquistLimit, voicing.postLowpassHz);

    const float lowTau  = onePoleTimeConstantFor(lowF);
    const float postTau = onePoleTimeConstantFor(postF);

    for (auto& cs : channels)
    {
        for (auto& stage : cs.stages)
        {
            stage.mid.prepare(sr);
            stage.mid.setCutoffQ(midF, voicing.midQ);

            stage.lowCut.prepare(sr);
            stage.lowCut.setTimeConstant(lowTau);
        }

        cs.post.prepare(sr);
        cs.post.setTimeConstant(postTau);
    }

    // Remember the rate that was ASKED for, not the clamped fallback: `process`
    // compares against this to decide whether the rate has changed. Storing the
    // fallback instead would make an out-of-range or zero rate compare unequal
    // on every single block, rebuilding every coefficient and — because
    // `SvfTPT::prepare` resets — wiping the mid filters' state once per block
    // for the rest of the session. A non-finite request cannot be compared at
    // all, so it falls back to the rate the coefficients were actually built for.
    preparedSampleRate = std::isfinite(sampleRate) ? sampleRate : sr;
}

void AmpStyleBase::process(float* const* channelData, int numChannels, int numSamples,
                           const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    // Coefficients depend only on the rate, so they are rebuilt on a change and
    // never touched per sample.
    if (std::abs(params.sampleRate - preparedSampleRate) > 1.0)
        updateCoefficients(params.sampleRate);

    const int numCh     = juce::jmin(numChannels, ampdetail::kMaxAmpChannels);
    const int numStages = juce::jlimit(1, ampdetail::kMaxAmpStages, voicing.numStages);

    // ------------------------------------------------- block-rate parameters
    const float sanitisedDrive = dsputil::sanitise(params.driveLin);
    const float driveLin = juce::jlimit(kMinDriveLin, kMaxDriveLin,
                                        sanitisedDrive > 0.0f ? sanitisedDrive : 1.0f);
    const float amount = juce::jlimit(0.0f, 1.0f, dsputil::sanitise(params.amount01));

    float stageGain[ampdetail::kMaxAmpStages] { 1.0f, 1.0f, 1.0f, 1.0f };

    for (int s = 0; s < numStages; ++s)
        stageGain[s] = juce::jlimit(0.01f, kMaxStageGain,
                                    voicing.stageTrim[s] * std::pow(driveLin, voicing.stageDriveExp[s]));

    const float bias   = voicing.biasAtFullDrive * amount;
    const float midMix = voicing.midMixMin + (voicing.midMixMax - voicing.midMixMin) * amount;
    const float lowCutDepth = juce::jlimit(0.0f, 1.0f, voicing.lowCutAmount);

    const bool useBias   = bias > 1.0e-4f;
    const bool useLowCut = voicing.lowCutHz > 0.0f && lowCutDepth > 1.0e-4f;

    // ----------------------------------------------------------- the cascade
    for (int ch = 0; ch < numCh; ++ch)
    {
        float* const data = channelData[ch];

        if (data == nullptr)
            continue;

        auto& cs = channels[ch];

        for (int offset = 0; offset < numSamples; offset += kControlBlockSize)
        {
            const int n = juce::jmin(numSamples - offset, kControlBlockSize);

            for (int i = 0; i < n; ++i)
            {
                float x = dsputil::sanitise(data[offset + i]);

                for (int s = 0; s < numStages; ++s)
                {
                    auto& stage = cs.stages[s];

                    // Gain into a bounded shaper: |x| <= 1 (+ the bias offset)
                    // leaving every stage, whatever came in.
                    x *= stageGain[s];
                    x = useBias ? dsputil::biasedTanh(x, bias) : dsputil::fastTanh(x);

                    // Inter-stage voicing: lift the mids, then optionally thin
                    // the lows so the next stage is not asked to saturate mud.
                    x += midMix * stage.mid.process(x).bp;

                    if (useLowCut)
                        x -= lowCutDepth * stage.lowCut.process(x);
                }

                x = cs.post.process(x);

                data[offset + i] = juce::jlimit(-kOutputGuard, kOutputGuard, dsputil::sanitise(x));
            }

            sanitiseChannelState(cs, numStages);
        }
    }
}

// ================================================================= Clean Amp
CleanAmpStyle::CleanAmpStyle()
{
    // Two gentle stages. The trims sit below unity so 0 dB drive is genuinely
    // clean — the cascade only starts to round once drive pushes it there.
    voicing.numStages = 2;

    voicing.stageTrim[0] = 0.85f;
    voicing.stageTrim[1] = 0.80f;
    voicing.stageTrim[2] = 1.0f;
    voicing.stageTrim[3] = 1.0f;

    voicing.stageDriveExp[0] = 0.58f;
    voicing.stageDriveExp[1] = 0.42f;   // sum 1.00: small-signal gain tracks drive
    voicing.stageDriveExp[2] = 0.0f;
    voicing.stageDriveExp[3] = 0.0f;

    voicing.biasAtFullDrive = 0.0f;     // symmetric: odd harmonics only

    voicing.midHz     = 900.0f;
    voicing.midQ      = 0.55f;
    voicing.midMixMin = 0.06f;
    voicing.midMixMax = 0.18f;

    voicing.lowCutHz     = 0.0f;        // full-range low end
    voicing.lowCutAmount = 0.0f;

    voicing.postLowpassHz = 13000.0f;
}

// ==================================================================== Crunch
CrunchStyle::CrunchStyle()
{
    // Three stages, hotter trims, a mid-forward tone stack and a little
    // asymmetry for the even-harmonic bite.
    voicing.numStages = 3;

    voicing.stageTrim[0] = 1.15f;
    voicing.stageTrim[1] = 1.05f;
    voicing.stageTrim[2] = 1.00f;
    voicing.stageTrim[3] = 1.0f;

    voicing.stageDriveExp[0] = 0.40f;
    voicing.stageDriveExp[1] = 0.36f;
    voicing.stageDriveExp[2] = 0.34f;   // sum 1.10: a touch hotter than drive
    voicing.stageDriveExp[3] = 0.0f;

    voicing.biasAtFullDrive = 0.20f;

    voicing.midHz     = 1100.0f;
    voicing.midQ      = 0.85f;
    voicing.midMixMin = 0.20f;
    voicing.midMixMax = 0.45f;

    // Gentle tightening between stages. Three stages compound it, so the depth
    // per stage stays small: about -5 dB at 40 Hz overall, -2 dB at 100 Hz.
    voicing.lowCutHz     = 70.0f;
    voicing.lowCutAmount = 0.25f;

    voicing.postLowpassHz = 9000.0f;
}

// ====================================================================== Lead
LeadStyle::LeadStyle()
{
    // Four stages and the highest gain, so the cascade is already compressing
    // well before the drive control runs out — that is the sustain. The low end
    // is high-passed hard between stages to keep it from turning to mush.
    voicing.numStages = 4;

    voicing.stageTrim[0] = 1.30f;
    voicing.stageTrim[1] = 1.20f;
    voicing.stageTrim[2] = 1.10f;
    voicing.stageTrim[3] = 1.05f;

    voicing.stageDriveExp[0] = 0.34f;
    voicing.stageDriveExp[1] = 0.30f;
    voicing.stageDriveExp[2] = 0.29f;
    voicing.stageDriveExp[3] = 0.27f;   // sum 1.20

    voicing.biasAtFullDrive = 0.14f;

    voicing.midHz     = 1400.0f;
    voicing.midQ      = 1.05f;
    voicing.midMixMin = 0.32f;
    voicing.midMixMax = 0.60f;

    // Four stages compound the high-pass, so a shallow shelf per stage is
    // already a firm tightening: about -11 dB at 40 Hz, -5 dB at 100 Hz, and
    // barely 2 dB by 200 Hz. Deeper than this deletes a low band outright.
    voicing.lowCutHz     = 90.0f;
    voicing.lowCutAmount = 0.35f;

    voicing.postLowpassHz = 7500.0f;
}
} // namespace ember
