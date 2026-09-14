#pragma once
#include "dsp/EmberTypes.h"

namespace ember
{
/** Fully resolved per-band values for one control block: APVTS values with all
    modulation already applied. Plain data, copied by value onto the audio
    thread — no strings, no pointers, no allocation. */
struct BandParams
{
    float driveDb { 0.0f };        ///< 0 .. 40
    float mix01 { 1.0f };          ///< 0 .. 1
    float levelDb { 0.0f };        ///< -24 .. +24
    float pan { 0.0f };            ///< -1 .. +1
    float width01 { 1.0f };        ///< 0 .. 2 (1 = unchanged)
    StyleID style { StyleID::CleanTube };
    float feedback01 { 0.0f };     ///< 0 .. 1
    float feedbackFreq { 200.0f }; ///< 20 .. 2000 Hz
    float dynamics { 0.0f };       ///< -1 .. +1
    float toneLowDb { 0.0f };
    float toneMidDb { 0.0f };
    float toneHighDb { 0.0f };
    bool bypass { false };
    bool solo { false };
};

/** Resolved global values for one control block. */
struct GlobalParams
{
    float inputGainDb { 0.0f };
    float outputGainDb { 0.0f };
    float mix01 { 1.0f };
    bool autoGain { false };
    int numBands { 3 };
    float crossoverHz[kMaxCrossovers] { 120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f };
    OversamplingFactor oversampling { OversamplingFactor::x2 };
    CrossoverMode crossoverMode { CrossoverMode::MinimumPhaseLR4 };
    StereoMode stereoMode { StereoMode::Stereo };
};
} // namespace ember
