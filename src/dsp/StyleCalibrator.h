#pragma once
#include <array>
#include "dsp/EmberTypes.h"

namespace ember
{
/**
    Measured per-style output gain compensation.

    Every style is driven with a fixed, deterministic pink-noise burst at 1 dB
    drive steps from 0 to 40 dB; the RMS deviation from the input is stored and
    interpolated at runtime. The band chain applies the result after the
    saturation stage, which is what makes two spec requirements true by
    construction instead of by hand-fitted constants:

      - consistent perceived loudness across styles at 0 dB drive, and
      - no level jump when the style changes.

    The table depends only on the oversampled sample rate, so it is built once
    per rate, cached, and shared by all six bands. Building is NOT realtime-safe
    (it allocates and runs ~1.5 M samples of DSP, a few ms); call
    `getForSampleRate` from `prepareToPlay`, never from `processBlock`.
*/
class StyleCalibrator
{
public:
    static constexpr int kNumDrivePoints = 41; // 0..40 dB inclusive, 1 dB apart
    static constexpr float kMaxDriveDb = 40.0f;

    /** Build or fetch the cached table for this rate. Message/prepare thread only. */
    static const StyleCalibrator& getForSampleRate(double oversampledSampleRate);

    /** Gain to apply after the style, in dB. Realtime-safe, allocation-free. */
    float compensationDb(StyleID id, float driveDb) const noexcept;

    /** Linear equivalent of `compensationDb`. Realtime-safe. */
    float compensationGain(StyleID id, float driveDb) const noexcept;

private:
    explicit StyleCalibrator(double oversampledSampleRate);
    std::array<std::array<float, kNumDrivePoints>, kNumStyles> tableDb{};
};
} // namespace ember
