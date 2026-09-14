#pragma once
#include <memory>
#include "dsp/EmberTypes.h"

namespace ember
{
/** Block-constant inputs to a saturation style.

    `drive` is the ONLY character control: per the spec each band exposes drive,
    not per-style knobs, so a style derives everything (bias, bit depth, fold
    count, filter movement) from the drive amount. */
struct StyleParams
{
    double sampleRate { 44100.0 }; ///< OVERSAMPLED rate — styles run inside the oversampler.
    float driveDb { 0.0f };        ///< 0 .. 40 dB, already smoothed.
    float driveLin { 1.0f };       ///< juce::Decibels::decibelsToGain (driveDb)
    float amount01 { 0.0f };       ///< driveDb / 40, clamped to [0, 1].
};

/**
    A saturation style: a bounded nonlinearity applied in place, at the
    oversampled rate, inside a band.

    Contract every implementation must honour:

    - `process` is called on the AUDIO THREAD. No allocation, no locks, no
      logging, no exceptions, no `juce::String`, no virtual dispatch per sample.
    - All state is allocated in `prepare` and sized for `maxBlockSize` samples
      and `numChannels` channels.
    - Output must be bounded for any finite input: a style may distort, but it
      may never produce a value outside +/- 8.0 or a non-finite value.
    - Styles are NOT responsible for output gain matching. The band chain
      applies a measured compensation from `StyleCalibrator`, so implementations
      should aim for musical character and leave level alone.
    - Styles are NOT responsible for DC removal. The band chain DC-blocks after
      the saturation stage, so asymmetric and rectifying styles are free to
      generate DC.
    - `process` must be deterministic: same input + same state => same output.
      This is what lets `StyleCalibrator` measure each style offline.
*/
class SaturationStyle
{
public:
    virtual ~SaturationStyle() = default;

    /** Allocate and size all state. Called off the audio thread.
        @param oversampledSampleRate  host rate * oversampling factor
        @param maxBlockSize           max samples per `process` call (oversampled)
        @param numChannels            1 or 2 */
    virtual void prepare(double oversampledSampleRate, int maxBlockSize, int numChannels) = 0;

    /** Return to the silent initial state. Must not allocate. */
    virtual void reset() noexcept = 0;

    /** Process in place. Audio thread; must be realtime-safe. */
    virtual void process(float* const* channelData, int numChannels, int numSamples,
                         const StyleParams& params) noexcept = 0;

    /** Stable display name, e.g. "Warm Tube". Must be a string literal. */
    virtual const char* getName() const noexcept = 0;

    /** Category used to group the style in the GUI picker. */
    virtual StyleCategory getCategory() const noexcept = 0;

    /** True for hard-edged shapers that rely on antiderivative anti-aliasing.
        Purely informational — used by the aliasing test to report coverage. */
    virtual bool usesAdaa() const noexcept { return false; }
};

/** Construct a style. Never returns null: an unknown id yields Clean Tube. */
std::unique_ptr<SaturationStyle> createSaturationStyle(StyleID id);

/** Display name without constructing the style. */
const char* getStyleName(StyleID id) noexcept;

/** Category without constructing the style. */
StyleCategory getStyleCategory(StyleID id) noexcept;

/** Human-readable category name, e.g. "Tube". */
const char* getCategoryName(StyleCategory c) noexcept;
} // namespace ember
