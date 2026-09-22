#pragma once
#include "dsp/EmberTypes.h"

namespace ember
{
/** Fully resolved per-band values for one control block: APVTS values with all
    modulation already applied. Plain data, copied by value onto the audio
    thread — no strings, no pointers, no allocation. */
struct BandParams
{
    float driveDb{0.0f}; ///< 0 .. 40
    float mix01{1.0f};   ///< 0 .. 1
    float levelDb{0.0f}; ///< -24 .. +24
    float pan{0.0f};     ///< -1 .. +1
    float width01{1.0f}; ///< 0 .. 2 (1 = unchanged)
    StyleID style{StyleID::CleanTube};
    float feedback01{0.0f};     ///< 0 .. 1
    float feedbackFreq{200.0f}; ///< 20 .. 2000 Hz
    float dynamics{0.0f};       ///< -1 .. +1
    float toneLowDb{0.0f};
    float toneMidDb{0.0f};
    float toneHighDb{0.0f};

    // Where the tone stage's three nodes sit. Defaults are the values these
    // were as fixed constants, so a preset written before they were parameters
    // loads with an identical response.
    float toneLowHz{150.0f};
    float toneMidHz{1000.0f};
    float toneMidQ{0.7f};
    float toneHighHz{4000.0f};

    /** Tone before the saturator instead of after it.

        Pre-EQ changes what the style is fed and therefore which harmonics it
        generates; post-EQ shapes what came out. They are different instruments,
        and the difference is most of the point of having the switch. */
    bool tonePreSaturation{false};

    /** Tone stage off entirely, without losing the node positions. */
    bool toneBypass{false};
    bool bypass{false};
    bool solo{false};
};

/** Resolved global values for one control block. */
struct GlobalParams
{
    float inputGainDb{0.0f};
    float outputGainDb{0.0f};
    float mix01{1.0f};
    bool autoGain{false};
    int numBands{3};
    float crossoverHz[kMaxCrossovers]{120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f};
    OversamplingFactor oversampling{OversamplingFactor::x2};
    CrossoverMode crossoverMode{CrossoverMode::MinimumPhaseLR4};
    StereoMode stereoMode{StereoMode::Stereo};

    /** Dither for the Bitcrush style's quantiser.

        This parameter existed, saved and restored, and was read by nothing:
        Bitcrush always used its own triangular default. It is wired up now,
        which makes it the control it always claimed to be. */
    DitherMode dither{DitherMode::Triangular};
};
} // namespace ember
