#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/EmberTypes.h"

namespace ember::pid
{
// ------------------------------------------------------------------ global
inline constexpr const char* inputGain = "inGain";
inline constexpr const char* outputGain = "outGain";
inline constexpr const char* globalMix = "mix";
inline constexpr const char* autoGain = "autoGain";
inline constexpr const char* osFactor = "osFactor";
inline constexpr const char* osOffline = "osOffline";
inline constexpr const char* xoverMode = "xoverMode";   // minimum phase / linear phase
inline constexpr const char* stereoMode = "stereoMode"; // stereo / mid-side
inline constexpr const char* numBands = "numBands";
inline constexpr const char* ditherMode = "dither";

/** Crossover frequency for edge `i` in [0, kMaxCrossovers). */
juce::String crossover(int i);

// ------------------------------------------------------------------ per band
// `band` is zero-based in code; the ID and display name are one-based.
juce::String drive(int band);
juce::String bandMix(int band);
juce::String level(int band);
juce::String pan(int band);
juce::String width(int band);
juce::String style(int band);
juce::String feedback(int band);
juce::String feedbackFreq(int band);
juce::String dynamics(int band);
juce::String toneLow(int band);
juce::String toneMid(int band);
juce::String toneHigh(int band);

// The tone stage's node positions and routing. These were fixed constants
// until the tone stage became a draggable node editor.
juce::String toneLowHz(int band);
juce::String toneMidHz(int band);
juce::String toneMidQ(int band);
juce::String toneHighHz(int band);
juce::String tonePre(int band);

/** Which of the XY pad's two axes the modulation source emits. */
extern const char* const xyAxis;
juce::String toneBypass(int band);
juce::String bypass(int band);
juce::String solo(int band);

// ------------------------------------------------------------------ modulation sources
juce::String lfoRate(int i);
juce::String lfoSync(int i);
juce::String lfoPhase(int i);
juce::String lfoSmooth(int i);
juce::String lfoSteps(int i);
juce::String lfoDepth(int i);

juce::String egAttack(int i);
juce::String egDecay(int i);
juce::String egSustain(int i);
juce::String egRelease(int i);
juce::String egThreshold(int i);
juce::String egTrigger(int i);

juce::String efAttack(int i);
juce::String efRelease(int i);
juce::String efBand(int i);
juce::String efGain(int i);

inline constexpr const char* xyX = "xyX";
inline constexpr const char* xyY = "xyY";

juce::String midiType(int i);
juce::String midiCC(int i);
juce::String midiSmooth(int i);

juce::String macro(int i);

// ------------------------------------------------------------------ choice strings
juce::StringArray styleChoices();
juce::StringArray oversamplingChoices();
juce::StringArray stereoModeChoices();
juce::StringArray crossoverModeChoices();
juce::StringArray ditherChoices();
juce::StringArray lfoSyncChoices();
juce::StringArray egTriggerChoices();
juce::StringArray midiTypeChoices();

/** Every automatable parameter in the plugin. */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** True if `id` names a continuous parameter that modulation may target.
    Bypass/solo/choice parameters are not valid modulation destinations. */
bool isModulatable(const juce::String& id);
} // namespace ember::pid
