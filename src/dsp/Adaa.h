#pragma once
#include <cmath>
#include <algorithm>

namespace ember::adaa
{
/**
    First-order antiderivative anti-aliasing.

    A hard-edged shaper f(x) aliases badly because its output is discontinuous in
    slope. ADAA replaces the point evaluation with the average of f over the
    segment between consecutive samples, computed from f's antiderivative F1:

        y[n] = (F1(x[n]) - F1(x[n-1])) / (x[n] - x[n-1])

    That is ~20-30 dB less aliasing at a given oversampling factor, which is what
    lets hard clip, the wavefolder and the rectifiers meet the < -80 dBFS alias
    gate without running at 16x.

    The division is ill-conditioned when consecutive samples are close, so below
    `eps` it falls back to evaluating f at the midpoint — the limit of the
    expression as the difference goes to zero.

    `state` must be one float per channel, owned by the caller and cleared in
    `reset()`.
*/
template <typename ShaperFn, typename AntiderivFn>
inline float process1(float x, float& state, ShaperFn f, AntiderivFn F1, float eps = 1.0e-5f) noexcept
{
    const float xPrev = state;
    state = x;
    const float diff = x - xPrev;

    if (std::abs(diff) < eps)
        return f(0.5f * (x + xPrev));

    const float y = (F1(x) - F1(xPrev)) / diff;
    return std::isfinite(y) ? y : f(0.5f * (x + xPrev));
}

// ---------------------------------------------------------------- hard clip
inline float hardClipF(float x) noexcept { return std::clamp(x, -1.0f, 1.0f); }

/** Antiderivative of hard clip: x^2/2 inside, |x| - 1/2 outside. */
inline float hardClipF1(float x) noexcept
{
    return std::abs(x) < 1.0f ? 0.5f * x * x : std::abs(x) - 0.5f;
}

// ---------------------------------------------------------------- full-wave rectifier
inline float rectifyF(float x) noexcept { return std::abs(x); }
inline float rectifyF1(float x) noexcept { return 0.5f * x * std::abs(x); }

// ---------------------------------------------------------------- half-wave rectifier
inline float halfRectifyF(float x) noexcept { return x > 0.0f ? x : 0.0f; }
inline float halfRectifyF1(float x) noexcept { return x > 0.0f ? 0.5f * x * x : 0.0f; }

// ---------------------------------------------------------------- triangular wavefolder
/** Folds x into [-1, 1] as a triangle wave of period 4. */
inline float foldF(float x) noexcept
{
    const float t = std::fmod(std::abs(x) + 1.0f, 4.0f);
    return (t <= 2.0f ? t : 4.0f - t) - 1.0f;
}

/** Antiderivative of the triangular fold. Piecewise quadratic, continuous. */
inline float foldF1(float x) noexcept
{
    const float s = x < 0.0f ? -1.0f : 1.0f;
    const float a = std::abs(x);
    const float period = std::floor((a + 1.0f) / 4.0f);
    const float t = (a + 1.0f) - 4.0f * period;   // t in [0, 4)
    // Integral of the triangle over one period is zero, so only the partial
    // period contributes; the -1 offset in foldF contributes -a.
    float partial;
    if (t <= 2.0f)
        partial = 0.5f * t * t;
    else
        partial = 2.0f - 0.5f * (4.0f - t) * (4.0f - t) + 2.0f;
    return s * (partial - 0.5f - a);
}
} // namespace ember::adaa
