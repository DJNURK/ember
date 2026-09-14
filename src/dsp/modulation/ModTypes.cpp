#include "dsp/modulation/ModTypes.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace ember
{
namespace
{
/** The flat source layout, in order. Changing the ORDER of this table changes
    every saved modulation routing, so it is append-only in the same sense as
    StyleID: the block order is part of the state format. */
struct SourceBlockInfo
{
    ModSourceType type;
    int count;
    bool bipolar;
    const char* displayName;
};

constexpr std::array<SourceBlockInfo, 6> kSourceBlocks{
    {{ModSourceType::XLFO, kNumXLFOs, true, "XLFO"},
     {ModSourceType::EnvelopeGenerator, kNumEnvGenerators, false, "Env"},
     {ModSourceType::EnvelopeFollower, kNumEnvFollowers, false, "Follow"},
     {ModSourceType::XYController, kNumXYControllers, false, "XY"},
     {ModSourceType::MidiSource, kNumMidiSources, false, "MIDI"},
     {ModSourceType::Macro, kNumMacros, false, "Macro"}}};

/** Compile-time proof that the table and the header's total agree: if someone
    adds a source type to EmberTypes.h and forgets this table, the build stops
    here instead of silently mis-indexing every saved preset. */
constexpr int blockTotal() noexcept
{
    int total = 0;
    for (const auto& b : kSourceBlocks)
        total += b.count;
    return total;
}

static_assert(blockTotal() == kNumModSources, "kSourceBlocks must cover every modulation source");
static_assert(kSourceBlocks.size() == static_cast<size_t>(ModSourceType::Count),
              "kSourceBlocks must list every ModSourceType exactly once");
} // namespace

int flatSourceIndex(ModSourceType type, int ordinal) noexcept
{
    int base = 0;

    for (const auto& block : kSourceBlocks)
    {
        if (block.type == type)
            return (ordinal >= 0 && ordinal < block.count) ? base + ordinal : -1;

        base += block.count;
    }

    return -1;
}

ModSourceInfo sourceInfoFromFlatIndex(int flatIndex) noexcept
{
    ModSourceInfo info{};

    if (flatIndex < 0)
        return info;

    int remaining = flatIndex;

    for (const auto& block : kSourceBlocks)
    {
        if (remaining < block.count)
        {
            info.type = block.type;
            info.indexWithinType = remaining;
            info.bipolar = block.bipolar;
            return info;
        }

        remaining -= block.count;
    }

    // Out of range: the default-constructed info (Macro 0, unipolar) is a safe,
    // never-null answer. Callers that care validate the index themselves.
    return info;
}

juce::String modSourceDisplayName(int flatIndex)
{
    if (flatIndex < 0 || flatIndex >= kNumModSources)
        return "None";

    const auto info = sourceInfoFromFlatIndex(flatIndex);

    for (const auto& block : kSourceBlocks)
        if (block.type == info.type)
            return juce::String(block.displayName) + " " + juce::String(info.indexWithinType + 1);

    return "None";
}

float applyModCurve(float value, ModCurve curve) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    // Curves are defined on the magnitude so that a bipolar source keeps its
    // sign and stays symmetric about zero; a unipolar source is unaffected by
    // the sign handling because its magnitude is the value.
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    const float magnitude = std::min(std::abs(value), 1.0f);

    switch (curve)
    {
    case ModCurve::Linear:
        break;

    case ModCurve::ExpoIn:
        // Slow start, steep finish.
        return sign * magnitude * magnitude * magnitude;

    case ModCurve::ExpoOut:
    {
        // Steep start, slow finish (mirror of ExpoIn).
        const float inverse = 1.0f - magnitude;
        return sign * (1.0f - inverse * inverse * inverse);
    }

    case ModCurve::SCurve:
        // Smoothstep: flat at both ends, steepest in the middle.
        return sign * (magnitude * magnitude * (3.0f - 2.0f * magnitude));

    case ModCurve::Stepped:
    {
        // Quantise to 8 evenly spaced levels including both 0 and 1.
        constexpr float levels = 7.0f; // 8 levels => 7 intervals
        return sign * (std::round(magnitude * levels) / levels);
    }

    case ModCurve::Count:
        break;
    }

    return sign * magnitude;
}
} // namespace ember
