#pragma once
#include <vector>
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    Smudge — half-wave rectification crossfaded against the driven signal.

    The blend ratio rides the drive amount: at the bottom of the range the
    output is essentially the (lightly saturated) dry signal, and at the top it
    is almost pure half-wave rectification — the negative half collapses and the
    positive half is left standing, which reads as a thick, gated smear rather
    than a clean octave.

    Half-wave rectification has a slope discontinuity at zero, so the whole
    shaper (dry term included) runs through the first-order antiderivative
    anti-aliasing kernel in Adaa.h. Keeping the dry term inside the same kernel
    matters: ADAA costs half a sample of group delay, and mixing a delayed wet
    path against an undelayed dry path would comb the top octave.

    The style deliberately generates a large DC offset — that is what half-wave
    rectification is. The band chain DC-blocks downstream, so nothing is done
    about it here.
*/
class SmudgeStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Smudge"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Rectify; }
    bool usesAdaa() const noexcept override { return true; }

private:
    /** Previous driven input sample, one per channel — the ADAA kernel's state. */
    std::vector<float> adaaState;
};

/**
    Rectify — full-wave rectification crossfaded against the driven signal.

    Same gesture as Smudge but taken all the way: the blend curve rises faster
    and reaches a pure |x| at maximum drive, which doubles the apparent
    frequency and snarls an octave above the source.

    The crossfade passes through half-wave on its way there — (1 - b) * x +
    b * |x| is exactly half-wave rectification at b = 0.5 — so the knob sweeps
    dry, through Smudge territory, into the full octave-up.

    Bounded by the same tanh output stage and anti-aliased by the same
    first-order ADAA kernel; DC is left for the band chain to remove.
*/
class RectifyStyle final : public SaturationStyle
{
public:
    void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(float* const* channelData, int numChannels, int numSamples,
                 const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Rectify"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Rectify; }
    bool usesAdaa() const noexcept override { return true; }

private:
    /** Previous driven input sample, one per channel — the ADAA kernel's state. */
    std::vector<float> adaaState;
};
} // namespace ember
