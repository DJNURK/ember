#pragma once
#include <array>
#include <memory>
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"
#include "dsp/BandParams.h"
#include "dsp/BandFx.h"
#include "dsp/StyleCalibrator.h"
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    One band of the multiband chain:

        dry tap ------------------------------------------------[ delay ]--+
           |                                                               |
        [ upsample -> style (applies drive) -> gain match -> feedback ]     |
           -> downsample -> DC block -> dynamics -> tone -> level/pan/width +-> mix

    Notes on two decisions that are easy to get wrong:

    - ALL styles are preallocated and prepared, one instance of each per band.
      Switching style is then a pointer swap plus a 20 ms crossfade between the
      old and new instance, which is both click-free and allocation-free. The
      obvious alternative — constructing the new style on demand — would
      allocate on the audio thread.

    - The dry path is delayed by the oversampler's EXACT, fractional latency
      using an interpolating delay line. Rounding to whole samples would leave
      up to half a sample of misalignment, which comb-filters the top octave as
      soon as the band mix is anywhere but fully wet.
*/
class BandChain
{
public:
    BandChain();
    ~BandChain();

    /** Allocates. Message/prepare thread only.

        `linearPhaseOversampling` selects the half-band filter family:
          - false (realtime): polyphase IIR. Minimum phase, low latency, and
            roughly 3% of a core cheaper across six bands at 4x.
          - true (offline render / HQ): equiripple FIR. Linear phase, so the
            delayed dry path lines up exactly and the dry/wet blends do not
            comb, at a noticeably higher cost. */
    void prepare(double hostSampleRate, int maxBlockSize, int numChannels, OversamplingFactor factor,
                 bool linearPhaseOversampling = false);
    void reset() noexcept;

    /** Control-rate parameter update. Realtime-safe. */
    void setParameters(const BandParams& p) noexcept;

    /** In place, at the host sample rate. Realtime-safe. */
    void process(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    /** Latency this band adds, in host-rate samples (fractional). Identical for
        every band at a given oversampling factor, so bands stay aligned. */
    float getLatencySamples() const noexcept { return latencySamples; }

    float getGainReductionDb() const noexcept { return dynamics.getGainReductionDb(); }

private:
    void applyLevelPanWidth(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    std::array<std::unique_ptr<SaturationStyle>, static_cast<size_t>(kNumStyles)> styles;
    SaturationStyle* currentStyle{nullptr};
    SaturationStyle* fadingStyle{nullptr};
    float styleFade{1.0f}; ///< 1 = fully on currentStyle
    float styleFadeStep{1.0f};

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    /** The oversampler is built with integer-latency compensation, so this
        delay is always a whole number of samples and needs no interpolation. A
        Lagrange interpolator here costs four multiply-adds per sample per
        channel to compute a fraction that is always zero. */
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay{256};

    DCBlocker dcBlocker;
    FeedbackLoop feedback;
    Dynamics dynamics;
    ToneStack tone;

    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> fadeBuffer;

    const StyleCalibrator* calibrator{nullptr};

    BandParams params;
    double hostRate{44100.0};
    double osRate{44100.0};
    int maxBlock{512};
    int channels{2};
    int osFactorMultiplier{1};
    float latencySamples{0.0f};

    juce::SmoothedValue<float> smoothedDriveDb, smoothedMix, smoothedLevelGain;
    juce::SmoothedValue<float> smoothedPan, smoothedWidth, smoothedCompGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandChain)
};
} // namespace ember
