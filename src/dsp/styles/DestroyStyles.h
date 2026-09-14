#pragma once
#include <array>
#include <cstdint>
#include "dsp/styles/SaturationStyle.h"

namespace ember
{
/**
    Foldback — triangular wavefolder (category Destroy).

    Drive is applied as a monotonic pre-gain, so raising it pushes the signal
    further past the fold threshold and the triangle wraps more times: below
    +/-1 the transfer curve is the identity, above it the output folds back and
    keeps folding, one extra fold per 2.0 of input.

    The pre-gain is `driveLin ^ 0.65` rather than `driveLin` itself, which
    spends the knob over 1x .. ~20x (up to ten folds of a full-scale signal)
    instead of 1x .. 100x. Past roughly ten folds the character stops changing
    and only alias energy accumulates, so the compressed map costs nothing
    audible and buys ~26 dB of alias rejection at the top of the range. See
    the rationale at `kFoldDriveExponent` in the .cpp.

    A raw wavefolder is one of the most aggressive alias generators in audio —
    every fold is a slope discontinuity — so the shaper runs through
    `ember::adaa::process1` first-order antiderivative anti-aliasing rather than
    being evaluated point-wise.

    Output is bounded to +/-1 by the fold itself.
*/
class FoldbackStyle final : public SaturationStyle
{
public:
    void prepare (double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process (float* const* channelData, int numChannels, int numSamples,
                  const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Foldback"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Destroy; }
    bool usesAdaa() const noexcept override { return true; }

private:
    static constexpr int kMaxChannels = 2;

    std::array<float, kMaxChannels> adaaState {};   ///< previous (pre-gained) input per channel
    int activeChannels { kMaxChannels };
};

/**
    Hard Clip — straight hard clipping at +/-1 (category Destroy).

    Drive is the pre-gain into the clipper; the ceiling is fixed, so the drive
    knob is exactly the clipping ratio. Nothing is rounded, soft-kneed or
    gain-matched: the band chain applies the measured compensation afterwards.

    The clip runs through `ember::adaa::process1` with `hardClipF`/`hardClipF1`,
    which buys roughly 30 dB of alias rejection over a point-wise clip at the
    same oversampling factor.

    Output is bounded to +/-1.
*/
class HardClipStyle final : public SaturationStyle
{
public:
    void prepare (double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process (float* const* channelData, int numChannels, int numSamples,
                  const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Hard Clip"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Destroy; }
    bool usesAdaa() const noexcept override { return true; }

private:
    static constexpr int kMaxChannels = 2;

    std::array<float, kMaxChannels> adaaState {};
    int activeChannels { kMaxChannels };
};

/**
    Decimate — sample-and-hold rate reduction (category Destroy).

    Drive maps to the hold rate, sliding geometrically from 200 kHz (inaudible:
    every image lands far above the band edge, so 0 drive is effectively
    transparent at every oversampling factor) down to 300 Hz at full drive.

    The hold interval is driven by a phase accumulator, so it is fractional and
    completely independent of the block size — a 1-sample block and a
    1024-sample block produce the same output. The accumulator is shared by both
    channels so left and right always latch on the same sample, which keeps the
    stereo image intact.

    Sample-and-hold IS an aliasing effect; that is the whole character here, so
    nothing about it is anti-aliased. It is only made deterministic, block-size
    invariant and click-free at the parameter extremes.
*/
class DecimateStyle final : public SaturationStyle
{
public:
    void prepare (double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process (float* const* channelData, int numChannels, int numSamples,
                  const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Decimate"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Destroy; }

private:
    static constexpr int kMaxChannels = 2;

    std::array<float, kMaxChannels> held {};
    double sampleRate { 44100.0 };
    float phase { 1.0f };            ///< >= 1 forces a latch on the very first sample
    int activeChannels { kMaxChannels };
};

/**
    Bitcrush — bit-depth reduction plus rate reduction, with dither (category Crush).

    Drive maps to word length: 16 bits at 0 dB down to 2 bits at full drive,
    fractional depths included, with a gentler rate reduction (200 kHz down to
    1.5 kHz) layered underneath and a modest push into the quantiser so quiet
    material still reaches the code steps.

    Quantisation is mid-tread rounding of a signal first clamped to the
    quantiser's full-scale range, and the result is clamped back to that same
    range, so neither rounding nor dither can ever push a code outside the
    pre-quantisation range.

    Dither is selectable (`setDitherMode`) and defaults to triangular (TPDF).
    Its amplitude fades out below ~7 bits, where a full-LSB dither would be
    louder than the programme material.

    The noise source is a per-channel xorshift32 owned by this class and
    re-seeded in `reset()`, so the style stays bit-for-bit deterministic for
    `StyleCalibrator`.
*/
class BitcrushStyle final : public SaturationStyle
{
public:
    void prepare (double oversampledSampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process (float* const* channelData, int numChannels, int numSamples,
                  const StyleParams& params) noexcept override;

    const char* getName() const noexcept override { return "Bitcrush"; }
    StyleCategory getCategory() const noexcept override { return StyleCategory::Crush; }

    /** Dither type. Not realtime-safe to change mid-block; call from prepare. */
    void setDitherMode (DitherMode mode) noexcept { ditherMode = mode; }
    DitherMode getDitherMode() const noexcept { return ditherMode; }

private:
    static constexpr int kMaxChannels = 2;

    std::array<float, kMaxChannels> held {};
    std::array<std::uint32_t, kMaxChannels> rngState {};
    double sampleRate { 44100.0 };
    float phase { 1.0f };
    DitherMode ditherMode { DitherMode::Triangular };
    int activeChannels { kMaxChannels };
};
} // namespace ember
