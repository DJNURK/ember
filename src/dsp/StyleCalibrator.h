#pragma once
#include <array>
#include "dsp/EmberTypes.h"

namespace ember
{
/**
    Measured per-style output gain compensation.

    Every style is driven with a fixed, deterministic FLAT-noise burst at
    -18 dBFS RMS, at 1 dB drive steps from 0 to 40 dB; the RMS deviation from the
    input is stored and interpolated at runtime.

    The reference is flat rather than pink on purpose. Several styles roll off
    above a few kHz, and pink noise carries little energy there, so a
    pink-calibrated table measures a different quantity than the full-band
    loudness the specification's gate is about — for the amp styles the two
    differ by up to 4.7 dB. Flat is also the assumption-free choice: the
    calibrator is handed only the oversampled rate and cannot tell a host
    running at 96 kHz with no oversampling from 48 kHz at 2x. The band chain applies the result after the
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
