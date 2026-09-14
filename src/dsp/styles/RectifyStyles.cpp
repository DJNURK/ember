#include "dsp/styles/RectifyStyles.h"
#include "dsp/Adaa.h"
#include "dsp/DspUtils.h"
#include <algorithm>

namespace ember
{
namespace
{
/** Hard ceiling on the driven signal before it reaches the shaper.

    The output stage saturates long before this, so the clamp is inaudible; it
    exists so that a pathological host buffer (1e30 and friends) cannot push the
    squared antiderivative past float range and hand the ADAA kernel an
    infinity. */
constexpr float kDrivenClamp = 64.0f;

/** 40 dB of drive is 100x; a little headroom above that is plenty. */
constexpr float kMaxDriveLin = 128.0f;

// ---------------------------------------------------------------- Smudge tuning
/** Leaving a sliver of dry at full drive keeps some weight under the smear. */
constexpr float kSmudgeMaxBlend = 0.92f;
/** Quadratic-ish blend law: nearly clean for the first third of the range. */
constexpr float kSmudgeBlendBase  = 0.35f;
constexpr float kSmudgeInScale  = 0.80f;
constexpr float kSmudgeOutScale = 1.25f;   // unity small-signal gain, bounded to +/-1.25

// ---------------------------------------------------------------- Rectify tuning
/** Reaches pure |x| at the top of the drive range. */
constexpr float kRectifyMaxBlend = 1.0f;
/** Rises faster than Smudge — past half-wave (b = 0.5) by a third of the range. */
constexpr float kRectifyBlendBase  = 0.55f;
constexpr float kRectifyInScale  = 0.70f;
constexpr float kRectifyOutScale = 1.55f;  // bounded to +/-1.55

/** blend(amount) = maxBlend * amount * (base + (1 - base) * amount): zero at
    zero drive, `maxBlend` at full drive, with the knee set by `base`. */
constexpr float blendForAmount(float amount, float maxBlend, float base) noexcept
{
    return maxBlend * amount * (base + (1.0f - base) * amount);
}

/** Shared inner loop.

    `rectF` / `rectF1` are the rectifier and its antiderivative from Adaa.h,
    passed as empty lambdas so they inline — no indirect call per sample. The
    dry term is folded into the shaper (and its antiderivative) rather than
    summed afterwards, so the linear and rectified paths share one and the same
    half-sample ADAA group delay and cannot comb against each other. */
template <typename RectFn, typename RectF1Fn>
inline void processRectifier(float* const* channelData, int numChannels, int numSamples,
                             std::vector<float>& state, float drive, float blend,
                             float inScale, float outScale,
                             RectFn rectF, RectF1Fn rectF1) noexcept
{
    const int channels = juce::jmin(numChannels, static_cast<int>(state.size()));

    // Everything the ADAA kernel evaluates is double, deliberately.
    // `adaa::process1` divides a difference of two nearly-equal antiderivative
    // values by the step between samples; in float32 that cancellation injects
    // more broadband noise than the aliasing the kernel exists to remove, so
    // process1 widens the subtraction and the quotient to double. Taking a
    // float here would undo that: the antiderivative reaches ~2000 at full
    // drive, where a float ulp is 2.4e-4, and rounding it on the way in throws
    // away exactly the bits process1 is trying to preserve. The blend weights
    // are double for the same reason.
    const double blendD = static_cast<double>(blend);
    const double dryD   = 1.0 - blendD;

    const auto shaper = [dryD, blendD, rectF](double v) noexcept
    {
        return dryD * v + blendD * rectF(v);
    };

    const auto antiderivative = [dryD, blendD, rectF1](double v) noexcept
    {
        return dryD * (0.5 * v * v) + blendD * rectF1(v);
    };

    for (int ch = 0; ch < channels; ++ch)
    {
        float* const data = channelData[ch];
        if (data == nullptr)
            continue;

        // One bad buffer must not poison the kernel for the rest of the session.
        float z = dsputil::sanitise(state[static_cast<size_t>(ch)]);

        for (int n = 0; n < numSamples; ++n)
        {
            const float driven = juce::jlimit(-kDrivenClamp, kDrivenClamp,
                                              dsputil::sanitise(data[n]) * drive);
            const float shaped = adaa::process1(driven, z, shaper, antiderivative);

            // fastTanh saturates exactly at +/-1 outside +/-4.97, so the output
            // is hard-bounded by outScale for any finite input.
            data[n] = dsputil::sanitise(outScale * dsputil::fastTanh(inScale * shaped));
        }

        state[static_cast<size_t>(ch)] = dsputil::sanitise(z);
    }
}
} // namespace

// ================================================================ Smudge
void SmudgeStyle::prepare(double oversampledSampleRate, int maxBlockSize, int numChannels)
{
    juce::ignoreUnused(oversampledSampleRate, maxBlockSize);
    jassert(numChannels >= 1 && numChannels <= 2);

    adaaState.assign(static_cast<size_t>(juce::jmax(1, numChannels)), 0.0f);
}

void SmudgeStyle::reset() noexcept
{
    std::fill(adaaState.begin(), adaaState.end(), 0.0f);
}

void SmudgeStyle::process(float* const* channelData, int numChannels, int numSamples,
                          const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0 || adaaState.empty())
        return;

    const float amount = juce::jlimit(0.0f, 1.0f, dsputil::sanitise(params.amount01));
    const float drive  = juce::jlimit(1.0f, kMaxDriveLin, dsputil::sanitise(params.driveLin));
    const float blend  = blendForAmount(amount, kSmudgeMaxBlend, kSmudgeBlendBase);

    processRectifier(channelData, numChannels, numSamples, adaaState, drive, blend,
                     kSmudgeInScale, kSmudgeOutScale,
                     [](double v) noexcept { return adaa::halfRectifyF(v); },
                     [](double v) noexcept { return adaa::halfRectifyF1(v); });
}

// ================================================================ Rectify
void RectifyStyle::prepare(double oversampledSampleRate, int maxBlockSize, int numChannels)
{
    juce::ignoreUnused(oversampledSampleRate, maxBlockSize);
    jassert(numChannels >= 1 && numChannels <= 2);

    adaaState.assign(static_cast<size_t>(juce::jmax(1, numChannels)), 0.0f);
}

void RectifyStyle::reset() noexcept
{
    std::fill(adaaState.begin(), adaaState.end(), 0.0f);
}

void RectifyStyle::process(float* const* channelData, int numChannels, int numSamples,
                           const StyleParams& params) noexcept
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0 || adaaState.empty())
        return;

    const float amount = juce::jlimit(0.0f, 1.0f, dsputil::sanitise(params.amount01));
    const float drive  = juce::jlimit(1.0f, kMaxDriveLin, dsputil::sanitise(params.driveLin));
    const float blend  = blendForAmount(amount, kRectifyMaxBlend, kRectifyBlendBase);

    processRectifier(channelData, numChannels, numSamples, adaaState, drive, blend,
                     kRectifyInScale, kRectifyOutScale,
                     [](double v) noexcept { return adaa::rectifyF(v); },
                     [](double v) noexcept { return adaa::rectifyF1(v); });
}
} // namespace ember
