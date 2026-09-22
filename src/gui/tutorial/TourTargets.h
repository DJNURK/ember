#pragma once
#include <juce_core/juce_core.h>

namespace ember::gui::tutorial
{
/**
    The names a tour can point at.

    A tour is data - a JSON file naming a control and saying something about it
    - so it needs a vocabulary of names that outlives any particular layout.
    These are those names. A component claims one with `setComponentID`, and the
    engine finds it by walking the editor's tree.

    WHY A REGISTRY RATHER THAN RAW STRINGS
    --------------------------------------
    A tour that points at "band.1.drive" when the component calls itself
    "band1Drive" fails silently: the spotlight lands nowhere and the step looks
    broken rather than missing. Both halves naming the same constant makes that
    a compile error on one side and a test failure on the other.

    `test_tutorials` walks every shipped tour and asserts each target resolves
    to a component that actually exists in a constructed editor, so a rename
    that breaks a tour cannot reach a release.

    THE SHAPE OF A NAME
    -------------------
    Dotted, lowercase, most general part first: `band.3.drive`, `header.preset`,
    `display.crossover.1`. Indices are one-based in the name because they are
    one-based on screen, and a tour is written by someone reading the interface
    rather than the source.
*/
namespace TourTargets
{
//==============================================================================
// Header
inline constexpr const char* logo = "header.logo";
inline constexpr const char* presetName = "header.preset";
inline constexpr const char* presetPrevious = "header.preset.previous";
inline constexpr const char* presetNext = "header.preset.next";
inline constexpr const char* bandCount = "header.bands";
inline constexpr const char* oversampling = "header.os";
inline constexpr const char* crossoverMode = "header.crossover.mode";
inline constexpr const char* stereoMode = "header.stereo";
inline constexpr const char* compareA = "header.compare.a";
inline constexpr const char* compareB = "header.compare.b";
inline constexpr const char* undo = "header.undo";
inline constexpr const char* redo = "header.redo";
inline constexpr const char* presetsButton = "header.presets";
inline constexpr const char* modButton = "header.mod";
inline constexpr const char* helpButton = "header.help";
inline constexpr const char* overflow = "header.more";

//==============================================================================
// Display
inline constexpr const char* display = "display";
inline constexpr const char* displayMode = "display.mode";
inline constexpr const char* analyserSettings = "display.analyser";

/** Crossover handle `edge`, zero-based in code and one-based in the name. */
inline juce::String crossover(int edge)
{
    return "display.crossover." + juce::String(edge + 1);
}

/** Band region `band` on the display. */
inline juce::String bandRegion(int band)
{
    return "display.band." + juce::String(band + 1);
}

//==============================================================================
// Band modules
inline juce::String band(int b)
{
    return "band." + juce::String(b + 1);
}

inline juce::String bandControl(int b, const juce::String& control)
{
    return band(b) + "." + control;
}

inline juce::String bandDrive(int b) { return bandControl(b, "drive"); }
inline juce::String bandMix(int b) { return bandControl(b, "mix"); }
inline juce::String bandLevel(int b) { return bandControl(b, "level"); }
inline juce::String bandPan(int b) { return bandControl(b, "pan"); }
inline juce::String bandWidth(int b) { return bandControl(b, "width"); }
inline juce::String bandStyle(int b) { return bandControl(b, "style"); }
inline juce::String bandFeedback(int b) { return bandControl(b, "feedback"); }
inline juce::String bandFeedbackFreq(int b) { return bandControl(b, "feedbackfreq"); }
inline juce::String bandDynamics(int b) { return bandControl(b, "dynamics"); }
inline juce::String bandTone(int b) { return bandControl(b, "tone"); }
inline juce::String bandBypass(int b) { return bandControl(b, "bypass"); }
inline juce::String bandSolo(int b) { return bandControl(b, "solo"); }

//==============================================================================
// Modulation
inline constexpr const char* modPanel = "mod";
inline constexpr const char* modSources = "mod.sources";
inline constexpr const char* modMatrix = "mod.matrix";

inline juce::String modSource(int index)
{
    return "mod.source." + juce::String(index + 1);
}

//==============================================================================
// Footer
inline constexpr const char* footerInput = "footer.input";
inline constexpr const char* footerOutput = "footer.output";
inline constexpr const char* footerMix = "footer.mix";
inline constexpr const char* footerAutoGain = "footer.autogain";
inline constexpr const char* footerCpu = "footer.cpu";
} // namespace TourTargets
} // namespace ember::gui::tutorial
