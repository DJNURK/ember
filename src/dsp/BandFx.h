#pragma once
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"

namespace ember
{
/** First-order DC blocker (high-pass at ~5 Hz). Applied after every saturation
    stage so asymmetric and rectifying styles cannot pump DC into the band sum. */
class DCBlocker
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset() noexcept;
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

private:
    double coeff{0.9995};
    std::array<float, 2> x1{}, y1{};
};

/**
    Short resonant feedback path around the saturation stage.

    A fractional delay tuned to `frequency` plus a state-variable bandpass at the
    same frequency, fed back with `amount`. Stability is structural, not
    incidental: the loop gain is capped below unity at the bandpass peak and a
    `tanh` soft limiter plus a hard ceiling sit inside the loop, so the output
    stays bounded for any combination of settings and any input
    (`tests/test_feedback.cpp` runs 60 s of full-scale noise at the worst-case
    settings and asserts a bounded, finite peak).
*/
class FeedbackLoop
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;

    /** @param amount01  0 .. 1 (maps the 0-100 % parameter)
        @param frequency 20 .. 2000 Hz */
    void setParameters(float amount01, float frequency) noexcept;

    /** In-place. Bypasses itself cheaply when amount is 0. */
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

private:
    struct Impl;
    double sampleRate{44100.0};
    float amount{0.0f}, freq{200.0f};
    std::array<std::vector<float>, 2> delayLine;
    std::array<float, 2> svfIc1{}, svfIc2{};
    std::array<int, 2> writePos{};
    float g{0.0f}, k{0.0f}, a1{0.0f}, a2{0.0f}, a3{0.0f};
    float delaySamples{1.0f};
    int lineLength{0};
};

/**
    Single-knob bipolar dynamics.

    `amount` in [-1, +1]: positive = compression with program-dependent
    attack/release driven by a combined RMS + peak detector; negative =
    downward expansion / gating. Exactly 0 is a true bypass.
*/
class Dynamics
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;
    void setAmount(float bipolarAmount) noexcept; // -1 .. +1
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    /** Current gain reduction in dB (negative), for the GUI meter. */
    float getGainReductionDb() const noexcept { return gainReductionDb; }

private:
    double sampleRate{44100.0};
    float amount{0.0f};
    float rmsState{0.0f}, peakState{0.0f}, envState{1.0f};
    float gainReductionDb{0.0f};
    float attackCoeff{0.0f}, releaseCoeff{0.0f}, rmsCoeff{0.0f};
};

/** Post-saturation Low / Mid / High tone shaping, +/- 12 dB at fixed musical
    frequencies (low shelf 150 Hz, peak 1 kHz Q 0.7, high shelf 4 kHz). */
class ToneStack
{
public:
    /** Where the three bands sit, in Hz, and how sharp the mid bell is.

        These were fixed constants until the tone stage became a node editor:
        dragging a node horizontally has to move a real frequency, and there is
        no honest way to draw a curve the user can grab if the curve cannot
        move. Defaults are the values the constants held, so a preset saved
        before they existed loads unchanged. */
    struct Shape
    {
        float lowHz{150.0f};
        float midHz{1000.0f};
        float midQ{0.7f};
        float highHz{4000.0f};
    };

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;
    void setGainsDb(float lowDb, float midDb, float highDb) noexcept;

    /** Realtime-safe: recomputes coefficients only when something moved. */
    void setShape(const Shape& newShape) noexcept;

    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    /** The stage's magnitude response at `frequencyHz`, in decibels.

        Message-thread only, and deliberately not read from the live filters:
        it recomputes the same biquads from the current gains and shape so the
        drawn curve is the curve the filters implement, rather than a separate
        model of it that can drift. `test_eq.cpp` checks it against an offline
        sweep of the real processor. */
    [[nodiscard]] float magnitudeDbAt(float frequencyHz) const noexcept;

    [[nodiscard]] Shape getShape() const noexcept { return shape; }
    [[nodiscard]] float getLowDb() const noexcept { return lastLow; }
    [[nodiscard]] float getMidDb() const noexcept { return lastMid; }
    [[nodiscard]] float getHighDb() const noexcept { return lastHigh; }

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    double sampleRate{44100.0};
    float lastLow{0.0f}, lastMid{0.0f}, lastHigh{0.0f};
    Shape shape;
    std::array<std::array<Filter, 3>, 2> filters; // [channel][low, mid, high]
};
} // namespace ember
