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
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;
    void setGainsDb(float lowDb, float midDb, float highDb) noexcept;
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

private:
    /** Normalise a `juce::dsp::IIR::ArrayCoefficients` design into one section,
        by the same division by a0 that `IIR::Coefficients` would do. */
    void setSection(size_t index, const std::array<float, 6>& design) noexcept;

    double sampleRate{44100.0};
    float lastLow{0.0f}, lastMid{0.0f}, lastHigh{0.0f};

    /** b0, b1, b2, a1, a2 for the low shelf, the mid peak and the high shelf.
        The three sections are the same for both channels, which is why they are
        held here rather than inside a per-channel filter object: the hot loop
        then reads one set of fifteen floats and runs both channels against it.

        This is `juce::dsp::IIR::Filter`'s transposed direct form II written out.
        Keeping the JUCE object meant an out-of-line call per sample per section
        — six per stereo frame — each of which re-read the coefficient order
        through a `getFilterOrder` stub before doing three multiply-adds. */
    std::array<std::array<float, 5>, 3> coeffs{
        {{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f}}}};

    std::array<std::array<std::array<float, 2>, 3>, 2> state{}; // [channel][section][order]
};
} // namespace ember
