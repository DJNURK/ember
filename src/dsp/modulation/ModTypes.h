#pragma once
#include <juce_core/juce_core.h>
#include "dsp/EmberTypes.h"

namespace ember
{
/**
    One modulation routing: source -> destination, with an amount, a response
    curve and a smoothing time.

    `targetIndex` is an index into the engine's table of modulatable parameters,
    NOT a parameter ID string. IDs are resolved to indices once, on the message
    thread, when the connection is created or state is loaded — the audio thread
    never touches a `juce::String`.

    A destination may be another source's parameter, so the graph is evaluated
    in dependency order. Cycles are rejected at edit time by `addConnection`.
*/
struct ModConnection
{
    int sourceIndex{-1}; ///< 0 .. kNumModSources-1; -1 = empty slot
    int targetIndex{-1}; ///< index into the modulatable-parameter table
    float amount{0.0f};  ///< bipolar, -1 .. +1, in normalised target units
    ModCurve curve{ModCurve::Linear};
    float smoothingMs{5.0f}; ///< 0 .. 500
    bool enabled{true};

    bool isActive() const noexcept { return sourceIndex >= 0 && targetIndex >= 0 && enabled; }
};

/** Identifies a modulation source for the GUI and for state. */
struct ModSourceInfo
{
    ModSourceType type{ModSourceType::Macro};
    int indexWithinType{0};
    bool bipolar{false}; ///< true => source range is [-1, +1], else [0, 1]
};

/** Flat source index <-> (type, ordinal). The flat order is:
    XLFO 0..3, EG 0..1, EnvFollower 0..3, XY 0, MIDI 0..3, Macro 0..7. */
int flatSourceIndex(ModSourceType type, int ordinal) noexcept;
ModSourceInfo sourceInfoFromFlatIndex(int flatIndex) noexcept;

/** Display name, e.g. "XLFO 2", "Macro 5". Message thread only. */
juce::String modSourceDisplayName(int flatIndex);

/** Apply a response curve to a value already in [0,1] (unipolar sources) or
    [-1,1] (bipolar sources). Realtime-safe and branch-cheap. */
float applyModCurve(float value, ModCurve curve) noexcept;
} // namespace ember
