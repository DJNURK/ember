#include "dsp/styles/DestroyStyles.h"

#include <cmath>

#include "dsp/Adaa.h"
#include "dsp/DspUtils.h"

namespace ember
{
namespace
{
// ---------------------------------------------------------------- shared limits
/** Hardest pre-gain any Destroy style will apply. 40 dB is the spec maximum
    (x100); the headroom above it only exists so a wild `driveLin` from a
    misbehaving modulation source cannot turn into an unbounded fold count. */
constexpr float kMaxPreGain = 128.0f;

/** Pure safety net on the wavefolder's input: a band that somehow arrives
    enormous still folds a bounded number of times instead of grinding through
    thousands of triangle periods per sample. With the drive mapping below this
    needs a band level above +/-12 to engage, so normal material never sees it. */
constexpr float kMaxFoldInput = 256.0f;

/**
    Fold-count exponent.

    A straight `driveLin` pre-gain would put 100x into the folder at +40 dB —
    fifty folds, which is not a wavefolder any more, it is a noise source: past
    roughly ten folds every further fold sounds the same and only adds alias
    energy. `driveLin ^ 0.65` keeps the mapping monotonic and still "further
    past the threshold = more folds", but spends the knob over 1x .. ~20x
    (up to ten folds of a full-scale signal), which is the range where the
    character actually changes, and buys ~26 dB of alias rejection at the top.
*/
constexpr float kFoldDriveExponent = 0.65f;

/** ADAA fallback threshold for the folder. Wider than the hard-clip default:
    the folder's antiderivative swings over a full unit per fold, so dividing
    its difference by a smaller step than this amplifies rounding noise faster
    than it removes aliasing. Below the threshold the kernel evaluates the
    shaper at the segment midpoint, which is exact in the limit. */
constexpr float kFoldAdaaEps = 1.0e-4f;

/** Rate-reduction end points, in Hz, shared by Decimate and Bitcrush.
    200 kHz is the "off" end: every sample-and-hold image sits above 175 kHz,
    so it is stripped by the oversampler's decimation filter at every factor,
    and at 1x-4x the increment simply clamps to 1.0 (true bypass). */
constexpr double kHoldRateTopHz = 200000.0;

// ---------------------------------------------------------------- wavefolder
/**
    Odd triangular fold, built on `ember::adaa::foldF`.

    `foldF` is written in terms of `std::abs(x)` and is therefore an even
    function; used directly it rectifies everything below the fold threshold
    instead of passing it through. The fold a wavefolder needs is the odd
    extension, which is what the sign flip below produces: identity on
    [-1, 1], then folding.
*/
double foldShaper (double x) noexcept
{
    const double y = adaa::foldF (x);
    return x < 0.0 ? -y : y;
}

/**
    Antiderivative of `foldShaper`, i.e. the F1 that `adaa::process1` needs.

    `adaa::foldF1` is the antiderivative of the triangle over a *single* period
    and drops the 4-per-period accumulation term, so it is only correct for
    |x| < 3 (verified numerically against a fine Riemann sum: it diverges
    linearly beyond the first fold). Restoring that term and folding it back
    into the phase gives the closed form used here:

        F1(x) = H(t) + 1/2 - t,   t = (|x| + 1) mod 4

    where H is `adaa::foldF1`'s piecewise-quadratic partial integral. Written
    this way there is no large-minus-large cancellation at all, and the phase
    reduction is done in double so that a deeply driven signal (|x| up to 64)
    still yields an F1 accurate to a float ulp — which matters, because
    `process1` divides the difference of two F1 values by a step that can be as
    small as `kFoldAdaaEps`.

    F1 is even, as the antiderivative of an odd function must be.
*/
double foldAntideriv (double x) noexcept
{
    const double a = std::abs (x);
    const double u = a + 1.0;
    const double t = u - 4.0 * std::floor (u * 0.25);        // t in [0, 4)
    const double h = t <= 2.0 ? 0.5 * t * t
                              : 4.0 * t - 0.5 * t * t - 4.0; // integral of the triangle
    // Stays in double all the way out: rounding the result to float first costs
    // ~6e-8 per evaluation, and `process1` divides the DIFFERENCE of two of
    // these by a step as small as `kFoldAdaaEps`, which amplifies that to
    // 5.3e-4 (-65 dB) of broadband error — measured against the double form.
    return h + 0.5 - t;
}

// ---------------------------------------------------------------- helpers
/** The hard ceiling `softBound` asymptotes to. Anything already produced by
    `softBound` is inside this, so re-clamping to it is a no-op — which is what
    lets the per-block state guard be idempotent. */
constexpr float kSoftBoundCeiling = 6.0f;

/** Output guard. Exactly transparent (and slope-continuous) up to +/-2, soft
    above it, hard-bounded at +/-6 — comfortably inside the +/-8 the style
    contract allows for any finite input. */
float softBound (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 2.0f)
        return x;

    const float s = x < 0.0f ? -1.0f : 1.0f;
    return s * (2.0f + 4.0f * dsputil::fastTanh ((a - 2.0f) * 0.25f));
}

/** Per-block sample-and-hold phase increment: the hold rate slides
    geometrically from `kHoldRateTopHz` to `bottomHz` as `amount` goes 0 -> 1,
    with `skew` > 1 keeping the low-drive end gentle. Always in (0, 1], so a
    hold can never be shorter than one sample. */
float holdIncrement (double sr, float amount, double bottomHz, double skew) noexcept
{
    const float clamped = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (amount));
    const double a = std::pow (static_cast<double> (clamped), skew);
    const double hz = kHoldRateTopHz * std::pow (bottomHz / kHoldRateTopHz, a);
    const double inc = hz / juce::jmax (1.0, sr);

    return std::isfinite (inc) ? static_cast<float> (juce::jlimit (1.0e-6, 1.0, inc)) : 1.0f;
}

/** xorshift32. Deterministic, allocation-free, period 2^32 - 1. */
std::uint32_t nextRandom (std::uint32_t& s) noexcept
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/** Uniform in [0, 1). 24 bits, exactly representable. */
float nextUniform (std::uint32_t& s) noexcept
{
    return static_cast<float> (nextRandom (s) >> 8) * (1.0f / 16777216.0f);
}

/** One dither sample in LSB units: RPDF spans 1 LSB, TPDF spans 2 LSB. */
float ditherSample (DitherMode mode, std::uint32_t& s) noexcept
{
    switch (mode)
    {
        case DitherMode::Rectangular:
            return nextUniform (s) - 0.5f;

        case DitherMode::Triangular:
        {
            // Sequenced deliberately: both draws mutate the generator state.
            const float u1 = nextUniform (s);
            const float u2 = nextUniform (s);
            return u1 + u2 - 1.0f;
        }

        case DitherMode::Off:
        case DitherMode::Count:
        default:
            return 0.0f;
    }
}

/** Deterministic, non-zero xorshift seed for a channel. */
std::uint32_t seedForChannel (int channel) noexcept
{
    const auto s = 0x9e3779b9u + 0x85ebca6bu * static_cast<std::uint32_t> (channel);
    return s != 0u ? s : 0x12345678u;
}

/** Clamp the block-constant pre-gain and reject anything non-finite. */
float safePreGain (float driveLin) noexcept
{
    return juce::jlimit (0.0f, kMaxPreGain, dsputil::sanitise (driveLin));
}

/** True when the caller handed us something we can safely write to. */
bool buffersUsable (float* const* channelData, int numChannels, int numSamples) noexcept
{
    if (channelData == nullptr || numSamples <= 0 || numChannels <= 0)
        return false;

    for (int ch = 0; ch < numChannels; ++ch)
        if (channelData[ch] == nullptr)
            return false;

    return true;
}
} // namespace

// ================================================================ Foldback
void FoldbackStyle::prepare (double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    juce::ignoreUnused (oversampledSampleRate);
    activeChannels = juce::jlimit (1, kMaxChannels, numChannels);
    reset();
}

void FoldbackStyle::reset() noexcept
{
    adaaState.fill (0.0f);
}

void FoldbackStyle::process (float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    const int nCh = juce::jmin (numChannels, activeChannels, kMaxChannels);

    if (! buffersUsable (channelData, nCh, numSamples))
        return;

    const float gain = std::pow (safePreGain (params.driveLin), kFoldDriveExponent);

    for (int ch = 0; ch < nCh; ++ch)
    {
        const auto idx = static_cast<size_t> (ch);
        float* const data = channelData[ch];
        float state = dsputil::sanitise (adaaState[idx]);

        for (int n = 0; n < numSamples; ++n)
        {
            const float driven = dsputil::sanitise (data[n]) * gain;
            const float x = juce::jlimit (-kMaxFoldInput, kMaxFoldInput, driven);
            data[n] = dsputil::sanitise (adaa::process1 (x, state, foldShaper, foldAntideriv, kFoldAdaaEps));
        }

        adaaState[idx] = dsputil::sanitise (state);
    }
}

// ================================================================ Hard Clip
void HardClipStyle::prepare (double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    juce::ignoreUnused (oversampledSampleRate);
    activeChannels = juce::jlimit (1, kMaxChannels, numChannels);
    reset();
}

void HardClipStyle::reset() noexcept
{
    adaaState.fill (0.0f);
}

void HardClipStyle::process (float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    const int nCh = juce::jmin (numChannels, activeChannels, kMaxChannels);

    if (! buffersUsable (channelData, nCh, numSamples))
        return;

    const float gain = safePreGain (params.driveLin);

    for (int ch = 0; ch < nCh; ++ch)
    {
        const auto idx = static_cast<size_t> (ch);
        float* const data = channelData[ch];
        float state = dsputil::sanitise (adaaState[idx]);

        for (int n = 0; n < numSamples; ++n)
        {
            const float x = dsputil::sanitise (data[n]) * gain;
            data[n] = dsputil::sanitise (adaa::process1 (x, state, adaa::hardClipF, adaa::hardClipF1));
        }

        adaaState[idx] = dsputil::sanitise (state);
    }
}

// ================================================================ Decimate
void DecimateStyle::prepare (double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    sampleRate = oversampledSampleRate > 0.0 ? oversampledSampleRate : 44100.0;
    activeChannels = juce::jlimit (1, kMaxChannels, numChannels);
    reset();
}

void DecimateStyle::reset() noexcept
{
    held.fill (0.0f);
    phase = 1.0f;   // latch on the first sample of the first block
}

void DecimateStyle::process (float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    const int nCh = juce::jmin (numChannels, activeChannels, kMaxChannels);

    if (! buffersUsable (channelData, nCh, numSamples))
        return;

    // 200 kHz (transparent) down to 300 Hz, skewed so the first half of the
    // knob stays usable rather than instantly granular.
    const float increment = holdIncrement (sampleRate, params.amount01, 300.0, 1.3);

    if (! (std::isfinite (phase) && phase >= 0.0f && phase <= 1.0f))
        phase = 1.0f;

    // Idempotent guard. `softBound` is NOT idempotent above +/-2 — re-applying
    // it once per block would walk a held value back towards 2 a little further
    // every block, which both drifts a sample that is supposed to be frozen and
    // makes the output depend on how the host chops the buffer (BandChain
    // already calls process() in control-block-sized chunks). Bounding happens
    // once, where the sample is latched; here we only reject garbage.
    for (auto& h : held)
        h = juce::jlimit (-kSoftBoundCeiling, kSoftBoundCeiling, dsputil::sanitise (h));

    for (int n = 0; n < numSamples; ++n)
    {
        phase += increment;
        const bool latch = phase >= 1.0f;

        if (latch)
            phase -= std::floor (phase);

        for (int ch = 0; ch < nCh; ++ch)
        {
            const auto idx = static_cast<size_t> (ch);
            float* const data = channelData[ch];

            if (latch)
                held[idx] = softBound (dsputil::sanitise (data[n]));

            data[n] = held[idx];
        }
    }
}

// ================================================================ Bitcrush
void BitcrushStyle::prepare (double oversampledSampleRate, [[maybe_unused]] int maxBlockSize, int numChannels)
{
    sampleRate = oversampledSampleRate > 0.0 ? oversampledSampleRate : 44100.0;
    activeChannels = juce::jlimit (1, kMaxChannels, numChannels);
    reset();
}

void BitcrushStyle::reset() noexcept
{
    held.fill (0.0f);
    phase = 1.0f;

    for (int ch = 0; ch < kMaxChannels; ++ch)
        rngState[static_cast<size_t> (ch)] = seedForChannel (ch);
}

void BitcrushStyle::process (float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    const int nCh = juce::jmin (numChannels, activeChannels, kMaxChannels);

    if (! buffersUsable (channelData, nCh, numSamples))
        return;

    const float amount = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.amount01));

    // 16 bits clean down to 2 bits destroyed; fractional depths are fine, the
    // step count is just 2^(bits-1) for a +/-1 full-scale signal.
    const float bits = 16.0f - 14.0f * amount;
    const float levels = std::exp2 (bits - 1.0f);
    const float invLevels = 1.0f / levels;

    // A full-LSB dither is louder than the programme once the word gets very
    // short, so fade it out between 7 and 3 bits instead of switching it off
    // at a threshold (a switch would click under drive modulation).
    const float ditherGain = juce::jlimit (0.0f, 1.0f, (bits - 3.0f) * 0.25f);

    // Rate reduction sits underneath the quantiser and stays gentler than
    // Decimate: it colours the crush rather than replacing it.
    const float increment = holdIncrement (sampleRate, amount, 1500.0, 1.5);

    // Push into the quantiser as drive rises (0 .. +12 dB) so quiet band
    // material still reaches the code steps instead of sitting in one LSB.
    const float gain = 1.0f + 3.0f * amount;

    if (! (std::isfinite (phase) && phase >= 0.0f && phase <= 1.0f))
        phase = 1.0f;

    for (auto& h : held)
        h = juce::jlimit (-1.0f, 1.0f, dsputil::sanitise (h));

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        auto& s = rngState[static_cast<size_t> (ch)];
        if (s == 0u)
            s = seedForChannel (ch);
    }

    for (int n = 0; n < numSamples; ++n)
    {
        phase += increment;
        const bool latch = phase >= 1.0f;

        if (latch)
            phase -= std::floor (phase);

        for (int ch = 0; ch < nCh; ++ch)
        {
            const auto idx = static_cast<size_t> (ch);
            float* const data = channelData[ch];

            if (latch)
            {
                // Clamp first: the quantiser's range is +/-1, and clamping
                // before rounding is what makes the "never outside the
                // pre-quantisation range" guarantee below hold.
                const float x = juce::jlimit (-1.0f, 1.0f, dsputil::sanitise (data[n]) * gain);

                // Digital black stays black: there is no quantisation error to
                // decorrelate, and six bands each hissing into silence is not a
                // feature. Everything else gets the dither.
                float q = 0.0f;

                if (std::abs (x) > 0.0f)
                {
                    const float d = ditherGain * ditherSample (ditherMode, rngState[idx]);
                    q = std::round (x * levels + d) * invLevels;
                }

                held[idx] = juce::jlimit (-1.0f, 1.0f, q);
            }

            data[n] = held[idx];
        }
    }
}
} // namespace ember
