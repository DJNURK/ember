#pragma once
#include <array>
#include <memory>
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"
#include "dsp/BandParams.h"
#include "dsp/BandFx.h"
#include "dsp/DspUtils.h"
#include "dsp/PolyphaseOversampler.h"
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

    - The dry path is delayed by the oversampler's EXACT latency. Leaving even
      half a sample of misalignment there comb-filters the top octave as soon as
      the band mix is anywhere but fully wet. Both oversamplers are built with
      integer-latency compensation, so that exact figure is a whole number and
      the delay needs no interpolation at all.
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

    /** The frequency range this band covers, so the tone stage can keep its
        nodes inside it.

        The GUI clamps a dragged node to the band, but automation and modulation
        write the parameter directly and were not clamped at all - a node could
        sit two bands away, shaping a range this stage does not touch and
        drawing a curve the plugin does not produce. Clamping here covers every
        route to the parameter rather than just the mouse. */
    void setBandSpanHz(float lowHz, float highHz) noexcept;

    /** Dither for the Bitcrush style's quantiser. Realtime-safe: it writes a
        mode on one style object and touches nothing else. */
    void setDitherMode(DitherMode) noexcept;

    /** In place, at the host sample rate. Realtime-safe. */
    void process(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    /** Latency this band adds, in host-rate samples. A whole number, because
        both oversamplers compensate to one. Identical for every band at a given
        oversampling factor, so bands stay aligned. */
    float getLatencySamples() const noexcept { return latencySamples; }

    float getGainReductionDb() const noexcept { return dynamics.getGainReductionDb(); }

    /** How much of this band's output is no longer a scaled copy of its input:
        0 for a clean gain stage at any gain, rising towards 1 as the band
        distorts. A normalised distortion residual, not a level ratio - Ember
        gain-matches its styles, so levels cannot reveal saturation.

        A measurement for the visualiser and nothing else: taken from values the
        dry/wet loop already holds in registers, altering no sample and adding
        no branch to the processing path. Visual smoothing happens GUI-side, so
        the audio thread stores a raw figure and stops there. */
    float getHeatRatio() const noexcept { return heatRatio.load(std::memory_order_relaxed); }

private:
    void applyLevelPanWidth(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void publishHeat(float inIn, float inOut, float outOut) noexcept;

    std::atomic<float> heatRatio{0.0f};
    float spanLowHz{20.0f}, spanHighHz{20000.0f};

    std::array<std::unique_ptr<SaturationStyle>, static_cast<size_t>(kNumStyles)> styles;
    SaturationStyle* currentStyle{nullptr};
    SaturationStyle* fadingStyle{nullptr};
    float styleFade{1.0f}; ///< 1 = fully on currentStyle
    float styleFadeStep{1.0f};

    /** Two oversampler implementations, one live at a time.

        The realtime path uses `PolyphaseOversampler`, which is JUCE's polyphase
        IIR filter — the same design, bit for bit — run four lanes at a time
        instead of a channel at a time; the offline / HQ path keeps JUCE's own
        equiripple FIR, which only runs where CPU does not matter and so is not
        worth reimplementing. `usingPolyphase` says which one is active. */
    PolyphaseOversampler polyphase;
    std::unique_ptr<juce::dsp::Oversampling<float>> firOversampler;
    bool usingPolyphase{false};
    bool hasOversampling{false};
    /** Both oversamplers are built with integer-latency compensation, so this
        delay is always a whole number of samples: no interpolator, and no
        fractional arithmetic to compute a fraction that is always zero. */
    dsputil::IntegerDelay dryDelay;

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
