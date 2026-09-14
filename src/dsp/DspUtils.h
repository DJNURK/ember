#pragma once
#include <cmath>
#include <algorithm>
#include <juce_dsp/juce_dsp.h>

namespace ember::dsputil
{
/** Replace non-finite values with 0. Cheap insurance: one bad host buffer must
    not be able to poison a recursive stage for the rest of the session. */
inline float sanitise(float x) noexcept
{
    return std::isfinite(x) ? x : 0.0f;
}

inline bool isFinite(float x) noexcept
{
    return std::isfinite(x);
}

/** Rational tanh approximation. Max error ~2e-4 over [-5, 5], monotonic, and
    exactly saturating at +/-1 outside it — roughly 5x faster than std::tanh and
    indistinguishable after oversampling. */
inline float fastTanh(float x) noexcept
{
    if (x < -4.97f)
        return -1.0f;
    if (x > 4.97f)
        return 1.0f;
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    return num / den;
}

/** Classic cubic soft clip: linear below 1/3, curved to 2/3, hard above. */
inline float softClipCubic(float x) noexcept
{
    const float a = std::abs(x);
    if (a >= 1.0f)
        return x > 0.0f ? (2.0f / 3.0f) : (-2.0f / 3.0f);
    return x - (x * x * x) / 3.0f;
}

inline float hardClip(float x, float ceiling = 1.0f) noexcept
{
    return juce::jlimit(-ceiling, ceiling, x);
}

/** Asymmetric bias then tanh — the basic "tube" gesture. Even harmonics come
    from the bias, odd from the symmetric part. */
inline float biasedTanh(float x, float bias) noexcept
{
    return fastTanh(x + bias) - fastTanh(bias);
}

/** One-pole smoother / filter. `setTimeConstant` in seconds. */
class OnePole
{
public:
    void prepare(double sampleRate) noexcept { sr = sampleRate; }
    void setTimeConstant(float seconds) noexcept
    {
        coeff = seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (static_cast<float>(sr) * seconds));
    }
    void setCoefficient(float c) noexcept { coeff = c; }
    void reset(float value = 0.0f) noexcept { state = value; }
    float process(float x) noexcept
    {
        state = x + coeff * (state - x);
        return state;
    }
    float getState() const noexcept { return state; }

private:
    double sr{44100.0};
    float coeff{0.0f}, state{0.0f};
};

/** Topology-preserving state-variable filter (Zavalishin). Used for tone
    shaping and the feedback bandpass: stable under fast coefficient
    modulation, unlike a direct-form biquad. */
class SvfTPT
{
public:
    void prepare(double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }

    void setCutoffQ(float freqHz, float q) noexcept
    {
        const float f = juce::jlimit(10.0f, static_cast<float>(sr) * 0.49f, freqHz);
        g = std::tan(juce::MathConstants<float>::pi * f / static_cast<float>(sr));
        k = 1.0f / juce::jmax(0.025f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    struct Outputs
    {
        float lp, bp, hp;
    };

    Outputs process(float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return {v2, v1, x - k * v1 - v2};
    }

    float processBandpass(float x) noexcept { return process(x).bp; }

    /** Reset if the state has gone non-finite. Called once per control block. */
    void sanitiseState() noexcept
    {
        if (!(std::isfinite(ic1) && std::isfinite(ic2)))
            ic1 = ic2 = 0.0f;
    }

private:
    double sr{44100.0};
    float g{0.0f}, k{1.0f}, a1{0.0f}, a2{0.0f}, a3{0.0f};
    float ic1{0.0f}, ic2{0.0f};
};
} // namespace ember::dsputil
