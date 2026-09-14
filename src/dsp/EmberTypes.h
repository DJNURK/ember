#pragma once
#include <cstdint>
#include <cstddef>

namespace ember
{
// ---------------------------------------------------------------- topology
inline constexpr int kMaxBands        = 6;
inline constexpr int kMaxCrossovers   = kMaxBands - 1;   // 5
inline constexpr int kMinBands        = 1;

/** Modulation and parameter smoothing run once per control block, then ramp
    linearly across it. 32 samples @ 44.1 kHz = 0.73 ms — fast enough for
    envelope followers, cheap enough to be free. */
inline constexpr int kControlBlockSize = 32;

/** Spec requires >= 50 simultaneous modulation connections; 64 slots are
    preallocated so the graph never allocates on the audio thread. */
inline constexpr int kMaxModConnections = 64;

/** Click-free band-count / style changes crossfade over this long. */
inline constexpr double kCrossfadeSeconds = 0.020;

// ---------------------------------------------------------------- styles
/** Saturation styles. The numeric order is the order shown in the GUI and the
    order stored in state — APPEND ONLY, never renumber, or saved sessions and
    presets will silently change character. */
enum class StyleID : int
{
    // Tube
    CleanTube = 0,
    WarmTube,
    SubtleTube,
    BrokenTube,
    // Tape
    CleanTape,
    WarmTape,
    BrightTape,
    // Transformer
    Transformer,
    // Amp
    CleanAmp,
    Crunch,
    Lead,
    // Smudge / Rectify
    Smudge,
    Rectify,
    // Foldback / Destroy
    Foldback,
    HardClip,
    Decimate,
    // Bitcrusher
    Bitcrush,
    // FX
    Shimmer,
    Breathe,

    Count
};

inline constexpr int kNumStyles = static_cast<int>(StyleID::Count);   // 19

enum class StyleCategory : int
{
    Tube = 0, Tape, Transformer, Amp, Rectify, Destroy, Crush, FX, Count
};

// ---------------------------------------------------------------- global
enum class OversamplingFactor : int { Off = 0, x2, x4, x8, x16, Count };

/** Oversampling factor as a multiplier (1, 2, 4, 8, 16). */
inline constexpr int oversamplingMultiplier(OversamplingFactor f) noexcept
{
    return 1 << static_cast<int>(f);
}

enum class StereoMode : int { Stereo = 0, MidSide, Count };

enum class CrossoverMode : int { MinimumPhaseLR4 = 0, LinearPhase, Count };

enum class DitherMode : int { Off = 0, Rectangular, Triangular, Count };

// ---------------------------------------------------------------- modulation
enum class ModSourceType : int
{
    XLFO = 0,         // multi-point shape editor, host-sync or free
    EnvelopeGenerator, // ADSR, transient- or MIDI-triggered
    EnvelopeFollower,  // band-selectable sidechain detector
    XYController,
    MidiSource,
    Macro,
    Count
};

inline constexpr int kNumXLFOs          = 4;
inline constexpr int kNumEnvGenerators  = 2;
inline constexpr int kNumEnvFollowers   = 4;
inline constexpr int kNumXYControllers  = 1;
inline constexpr int kNumMidiSources    = 4;
inline constexpr int kNumMacros         = 8;

inline constexpr int kNumModSources =
    kNumXLFOs + kNumEnvGenerators + kNumEnvFollowers + kNumXYControllers + kNumMidiSources + kNumMacros; // 23

/** Shape applied to a connection's [0,1] or [-1,1] source value. */
enum class ModCurve : int { Linear = 0, ExpoIn, ExpoOut, SCurve, Stepped, Count };

// ---------------------------------------------------------------- spectrum
inline constexpr int kSpectrumFFTOrder = 11;               // 2048
inline constexpr int kSpectrumFFTSize  = 1 << kSpectrumFFTOrder;
inline constexpr int kSpectrumBins     = kSpectrumFFTSize / 2;
} // namespace ember
