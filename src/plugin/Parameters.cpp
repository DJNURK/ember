#include "plugin/ParameterIDs.h"
#include "dsp/styles/SaturationStyle.h"

#include <cmath>
#include <memory>

namespace ember::pid
{
namespace
{
// ---------------------------------------------------------------------------
// Local helpers. Everything here has internal linkage; none of it is reachable
// from the audio thread — parameter construction and ID lookup are message
// thread only, the audio thread reads cached atomics instead.
// ---------------------------------------------------------------------------

/** Version hint carried by every juce::ParameterID. Bump ONLY if a parameter's
    meaning changes in a way that must invalidate existing host automation. */
constexpr int kVersionHint = 1;

/** Clamp a caller-supplied ordinal so a bad index yields a deterministic ID
    instead of undefined behaviour. The assertion fires in debug builds. */
int safeIndex (int index, int numItems) noexcept
{
    jassert (juce::isPositiveAndBelow (index, numItems));
    return juce::jlimit (0, numItems - 1, index);
}

/** "lfo" + (0 -> 1) + "Rate" == "lfo1Rate". Indices are zero-based in code and
    one-based in every ID and display name, as ParameterIDs.h specifies. */
juce::String makeId (const char* prefix, int zeroBasedIndex, const char* suffix = "")
{
    return juce::String (prefix) + juce::String (zeroBasedIndex + 1) + suffix;
}

/** "LFO" + (0 -> 1) + "Rate" == "LFO 1 Rate". */
juce::String makeName (const char* prefix, int zeroBasedIndex, const char* suffix)
{
    return juce::String (prefix) + " " + juce::String (zeroBasedIndex + 1) + " " + suffix;
}

/** A logarithmic range whose midpoint on the control lands on `centre`.

    juce::NormalisableRange maps value -> normalised as pow(proportion, skew),
    so placing `centre` at 0.5 means pow(p, skew) == 0.5, i.e.
    skew = log(0.5) / log(p) with p the linear proportion of `centre`. Derived,
    never a guessed magic number. */
juce::NormalisableRange<float> logRange (float start, float end, float centre, float interval = 0.0f)
{
    jassert (end > start && centre > start && centre < end);

    const auto proportion = (centre - start) / (end - start);
    const auto skew       = std::log (0.5f) / std::log (proportion);

    return { start, end, interval, skew };
}

// Shared linear ranges. Gains and percentages stay linear on purpose.
const juce::NormalisableRange<float> gainRange     { -24.0f,  24.0f, 0.01f };
const juce::NormalisableRange<float> driveRange    {   0.0f,  40.0f, 0.01f };
const juce::NormalisableRange<float> percentRange  {   0.0f, 100.0f, 0.1f  };
const juce::NormalisableRange<float> bipolarRange  { -100.0f, 100.0f, 0.1f };
const juce::NormalisableRange<float> widthRange    {   0.0f, 200.0f, 0.1f  };
const juce::NormalisableRange<float> toneRange     { -12.0f,  12.0f, 0.01f };
const juce::NormalisableRange<float> phaseRange    {   0.0f, 360.0f, 0.1f  };
const juce::NormalisableRange<float> thresholdRange { -60.0f,  0.0f, 0.1f  };

/** Default crossover edges, ascending, spread over the audible range. */
constexpr float kDefaultCrossoverHz[kMaxCrossovers] = { 120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f };

/** Every continuous parameter that modulation may target.

    Built from the very same ID builders the layout uses, so the two can never
    drift apart, and no fragile string parsing is involved: membership is exact
    by construction rather than inferred from a prefix or suffix. */
juce::StringArray buildModulatableIds()
{
    juce::StringArray ids;
    ids.ensureStorageAllocated (160);

    // Global continuous controls (autoGain / osFactor / osOffline / xoverMode /
    // stereoMode / numBands / dither are bool, choice or int -> not targets).
    ids.add (inputGain);
    ids.add (outputGain);
    ids.add (globalMix);

    for (int i = 0; i < kMaxCrossovers; ++i)
        ids.add (crossover (i));

    for (int band = 0; band < kMaxBands; ++band)
    {
        ids.add (drive (band));
        ids.add (bandMix (band));
        ids.add (level (band));
        ids.add (pan (band));
        ids.add (width (band));
        ids.add (feedback (band));
        ids.add (feedbackFreq (band));
        ids.add (dynamics (band));
        ids.add (toneLow (band));
        ids.add (toneMid (band));
        ids.add (toneHigh (band));
        // style (choice), bypass and solo (bool) are deliberately absent.
    }

    for (int i = 0; i < kNumXLFOs; ++i)
    {
        ids.add (lfoRate (i));
        ids.add (lfoPhase (i));
        ids.add (lfoSmooth (i));
        ids.add (lfoDepth (i));
        // lfoSync (bool) and lfoSteps (int) are deliberately absent.
    }

    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        ids.add (egAttack (i));
        ids.add (egDecay (i));
        ids.add (egSustain (i));
        ids.add (egRelease (i));
        ids.add (egThreshold (i));
        // egTrigger (choice) is deliberately absent.
    }

    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        ids.add (efAttack (i));
        ids.add (efRelease (i));
        ids.add (efGain (i));
        // efBand (int) is deliberately absent.
    }

    ids.add (xyX);
    ids.add (xyY);

    for (int i = 0; i < kNumMidiSources; ++i)
        ids.add (midiSmooth (i));   // midiType (choice) and midiCC (int) are not targets.

    for (int i = 0; i < kNumMacros; ++i)
        ids.add (macro (i));

    return ids;
}
} // namespace

// --------------------------------------------------------------------- IDs
juce::String crossover (int i)    { return makeId ("xover", safeIndex (i, kMaxCrossovers)); }

juce::String drive (int band)        { return makeId ("band", safeIndex (band, kMaxBands), "Drive"); }
juce::String bandMix (int band)      { return makeId ("band", safeIndex (band, kMaxBands), "Mix"); }
juce::String level (int band)        { return makeId ("band", safeIndex (band, kMaxBands), "Level"); }
juce::String pan (int band)          { return makeId ("band", safeIndex (band, kMaxBands), "Pan"); }
juce::String width (int band)        { return makeId ("band", safeIndex (band, kMaxBands), "Width"); }
juce::String style (int band)        { return makeId ("band", safeIndex (band, kMaxBands), "Style"); }
juce::String feedback (int band)     { return makeId ("band", safeIndex (band, kMaxBands), "Feedback"); }
juce::String feedbackFreq (int band) { return makeId ("band", safeIndex (band, kMaxBands), "FeedbackFreq"); }
juce::String dynamics (int band)     { return makeId ("band", safeIndex (band, kMaxBands), "Dynamics"); }
juce::String toneLow (int band)      { return makeId ("band", safeIndex (band, kMaxBands), "ToneLow"); }
juce::String toneMid (int band)      { return makeId ("band", safeIndex (band, kMaxBands), "ToneMid"); }
juce::String toneHigh (int band)     { return makeId ("band", safeIndex (band, kMaxBands), "ToneHigh"); }
juce::String bypass (int band)       { return makeId ("band", safeIndex (band, kMaxBands), "Bypass"); }
juce::String solo (int band)         { return makeId ("band", safeIndex (band, kMaxBands), "Solo"); }

juce::String lfoRate (int i)   { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Rate"); }
juce::String lfoSync (int i)   { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Sync"); }
juce::String lfoPhase (int i)  { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Phase"); }
juce::String lfoSmooth (int i) { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Smooth"); }
juce::String lfoSteps (int i)  { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Steps"); }
juce::String lfoDepth (int i)  { return makeId ("lfo", safeIndex (i, kNumXLFOs), "Depth"); }

juce::String egAttack (int i)    { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Attack"); }
juce::String egDecay (int i)     { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Decay"); }
juce::String egSustain (int i)   { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Sustain"); }
juce::String egRelease (int i)   { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Release"); }
juce::String egThreshold (int i) { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Threshold"); }
juce::String egTrigger (int i)   { return makeId ("eg", safeIndex (i, kNumEnvGenerators), "Trigger"); }

juce::String efAttack (int i)  { return makeId ("ef", safeIndex (i, kNumEnvFollowers), "Attack"); }
juce::String efRelease (int i) { return makeId ("ef", safeIndex (i, kNumEnvFollowers), "Release"); }
juce::String efBand (int i)    { return makeId ("ef", safeIndex (i, kNumEnvFollowers), "Band"); }
juce::String efGain (int i)    { return makeId ("ef", safeIndex (i, kNumEnvFollowers), "Gain"); }

juce::String midiType (int i)   { return makeId ("midi", safeIndex (i, kNumMidiSources), "Type"); }
juce::String midiCC (int i)     { return makeId ("midi", safeIndex (i, kNumMidiSources), "CC"); }
juce::String midiSmooth (int i) { return makeId ("midi", safeIndex (i, kNumMidiSources), "Smooth"); }

juce::String macro (int i)      { return makeId ("macro", safeIndex (i, kNumMacros)); }

// ----------------------------------------------------------------- choices
juce::StringArray styleChoices()
{
    juce::StringArray choices;
    choices.ensureStorageAllocated (kNumStyles);

    for (int i = 0; i < kNumStyles; ++i)
        choices.add (getStyleName (static_cast<StyleID> (i)));

    return choices;
}

juce::StringArray oversamplingChoices() { return { "Off", "2x", "4x", "8x", "16x" }; }
juce::StringArray stereoModeChoices()   { return { "Stereo", "Mid-Side" }; }
juce::StringArray crossoverModeChoices() { return { "Minimum Phase", "Linear Phase" }; }
juce::StringArray ditherChoices()       { return { "Off", "Rectangular", "Triangular" }; }
juce::StringArray lfoSyncChoices()      { return { "Free", "Tempo Sync" }; }
juce::StringArray egTriggerChoices()    { return { "Input Transient", "MIDI Note" }; }
juce::StringArray midiTypeChoices()     { return { "Velocity", "CC", "Mod Wheel", "Note Number" }; }

// ------------------------------------------------------------------ layout
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto addFloat = [&layout] (const juce::String& paramId, const juce::String& name,
                                     const juce::NormalisableRange<float>& range,
                                     float defaultValue, const char* label)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { paramId, kVersionHint }, name, range, defaultValue,
            juce::AudioParameterFloatAttributes().withLabel (label)));
    };

    const auto addBool = [&layout] (const juce::String& paramId, const juce::String& name,
                                    bool defaultValue,
                                    juce::AudioParameterBoolAttributes attributes = {})
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { paramId, kVersionHint }, name, defaultValue, std::move (attributes)));
    };

    const auto addChoice = [&layout] (const juce::String& paramId, const juce::String& name,
                                      const juce::StringArray& choices, int defaultIndex)
    {
        jassert (juce::isPositiveAndBelow (defaultIndex, choices.size()));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { paramId, kVersionHint }, name, choices, defaultIndex));
    };

    const auto addInt = [&layout] (const juce::String& paramId, const juce::String& name,
                                   int minValue, int maxValue, int defaultValue,
                                   juce::AudioParameterIntAttributes attributes = {})
    {
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId, kVersionHint }, name, minValue, maxValue, defaultValue,
            std::move (attributes)));
    };

    // ------------------------------------------------------------- global
    addFloat (inputGain,  "Input Gain",  gainRange,    0.0f,   "dB");
    addFloat (outputGain, "Output Gain", gainRange,    0.0f,   "dB");
    addFloat (globalMix,  "Mix",         percentRange, 100.0f, "%");
    addBool  (autoGain,   "Auto Gain",   false);

    addChoice (osFactor,   "Oversampling",         oversamplingChoices(),
               static_cast<int> (OversamplingFactor::x2));
    addChoice (osOffline,  "Oversampling Offline", oversamplingChoices(),
               static_cast<int> (OversamplingFactor::x8));
    addChoice (xoverMode,  "Crossover Mode",       crossoverModeChoices(),
               static_cast<int> (CrossoverMode::MinimumPhaseLR4));
    addChoice (stereoMode, "Stereo Mode",          stereoModeChoices(),
               static_cast<int> (StereoMode::Stereo));
    addChoice (ditherMode, "Dither",               ditherChoices(),
               static_cast<int> (DitherMode::Triangular));

    addInt (numBands, "Band Count", kMinBands, kMaxBands, 3);

    // ---------------------------------------------------------- crossovers
    const auto crossoverRange = logRange (20.0f, 20000.0f, 1000.0f);

    for (int i = 0; i < kMaxCrossovers; ++i)
        addFloat (crossover (i), makeName ("Crossover", i, "Freq"), crossoverRange,
                  kDefaultCrossoverHz[i], "Hz");

    // ------------------------------------------------------------- bands
    const auto feedbackFreqRange = logRange (20.0f, 2000.0f, 200.0f);
    const auto allStyles         = styleChoices();

    const auto addBandParams = [&] (int band)
    {
        addFloat (drive (band),        makeName ("Band", band, "Drive"),  driveRange,   0.0f,   "dB");
        addFloat (bandMix (band),      makeName ("Band", band, "Mix"),    percentRange, 100.0f, "%");
        addFloat (level (band),        makeName ("Band", band, "Level"),  gainRange,    0.0f,   "dB");
        addFloat (pan (band),          makeName ("Band", band, "Pan"),    bipolarRange, 0.0f,   "");
        addFloat (width (band),        makeName ("Band", band, "Width"),  widthRange,   100.0f, "%");

        addChoice (style (band),       makeName ("Band", band, "Style"),  allStyles,
                   static_cast<int> (StyleID::CleanTube));

        addFloat (feedback (band),     makeName ("Band", band, "Feedback"),      percentRange,      0.0f,   "%");
        addFloat (feedbackFreq (band), makeName ("Band", band, "Feedback Freq"), feedbackFreqRange, 200.0f, "Hz");
        addFloat (dynamics (band),     makeName ("Band", band, "Dynamics"),      bipolarRange,      0.0f,   "%");

        addFloat (toneLow (band),      makeName ("Band", band, "Tone Low"),  toneRange, 0.0f, "dB");
        addFloat (toneMid (band),      makeName ("Band", band, "Tone Mid"),  toneRange, 0.0f, "dB");
        addFloat (toneHigh (band),     makeName ("Band", band, "Tone High"), toneRange, 0.0f, "dB");

        addBool (bypass (band), makeName ("Band", band, "Bypass"), false);
        addBool (solo (band),   makeName ("Band", band, "Solo"),   false);
    };

    for (int band = 0; band < kMaxBands; ++band)
        addBandParams (band);

    // ------------------------------------------------------------- XLFOs
    const auto lfoRateRange = logRange (0.01f, 40.0f, 2.0f);
    const auto syncNames    = lfoSyncChoices();

    const auto addLfoParams = [&] (int i)
    {
        addFloat (lfoRate (i), makeName ("LFO", i, "Rate"), lfoRateRange, 2.0f, "Hz");

        addBool (lfoSync (i), makeName ("LFO", i, "Sync"), false,
                 juce::AudioParameterBoolAttributes().withStringFromValueFunction (
                     [names = syncNames] (bool value, int) { return names[value ? 1 : 0]; }));

        addFloat (lfoPhase (i),  makeName ("LFO", i, "Phase"),  phaseRange,   0.0f, "deg");
        addFloat (lfoSmooth (i), makeName ("LFO", i, "Smooth"), percentRange, 0.0f, "%");

        // 0 steps == smooth (stepping off); 1..32 quantises the shape.
        addInt (lfoSteps (i), makeName ("LFO", i, "Steps"), 0, 32, 0,
                juce::AudioParameterIntAttributes().withStringFromValueFunction (
                    [] (int value, int) { return value == 0 ? juce::String ("Off") : juce::String (value); }));

        addFloat (lfoDepth (i), makeName ("LFO", i, "Depth"), percentRange, 100.0f, "%");
    };

    for (int i = 0; i < kNumXLFOs; ++i)
        addLfoParams (i);

    // ------------------------------------------------- envelope generators
    const auto egAttackRange  = logRange (0.1f, 2000.0f, 20.0f);
    const auto egDecayRange   = logRange (1.0f, 5000.0f, 200.0f);
    const auto egReleaseRange = logRange (1.0f, 5000.0f, 200.0f);
    const auto triggerNames   = egTriggerChoices();

    const auto addEgParams = [&] (int i)
    {
        addFloat (egAttack (i),    makeName ("Env Gen", i, "Attack"),    egAttackRange,  10.0f,  "ms");
        addFloat (egDecay (i),     makeName ("Env Gen", i, "Decay"),     egDecayRange,   200.0f, "ms");
        addFloat (egSustain (i),   makeName ("Env Gen", i, "Sustain"),   percentRange,   50.0f,  "%");
        addFloat (egRelease (i),   makeName ("Env Gen", i, "Release"),   egReleaseRange, 300.0f, "ms");
        addFloat (egThreshold (i), makeName ("Env Gen", i, "Threshold"), thresholdRange, -24.0f, "dB");

        addChoice (egTrigger (i), makeName ("Env Gen", i, "Trigger"), triggerNames, 0);
    };

    for (int i = 0; i < kNumEnvGenerators; ++i)
        addEgParams (i);

    // -------------------------------------------------- envelope followers
    const auto efAttackRange  = logRange (0.1f, 500.0f, 10.0f);
    const auto efReleaseRange = logRange (1.0f, 2000.0f, 100.0f);

    const auto addEfParams = [&] (int i)
    {
        addFloat (efAttack (i),  makeName ("Env Follower", i, "Attack"),  efAttackRange,  5.0f,   "ms");
        addFloat (efRelease (i), makeName ("Env Follower", i, "Release"), efReleaseRange, 100.0f, "ms");

        // 0 == full range sidechain; 1..kMaxBands listen to that band only.
        addInt (efBand (i), makeName ("Env Follower", i, "Band"), 0, kMaxBands, 0,
                juce::AudioParameterIntAttributes().withStringFromValueFunction (
                    [] (int value, int)
                    {
                        return value == 0 ? juce::String ("Full Range")
                                          : juce::String ("Band ") + juce::String (value);
                    }));

        addFloat (efGain (i), makeName ("Env Follower", i, "Gain"), gainRange, 0.0f, "dB");
    };

    for (int i = 0; i < kNumEnvFollowers; ++i)
        addEfParams (i);

    // ------------------------------------------------------ XY controller
    addFloat (xyX, "XY X", percentRange, 50.0f, "%");
    addFloat (xyY, "XY Y", percentRange, 50.0f, "%");

    // ------------------------------------------------------ MIDI sources
    const auto midiSmoothRange = logRange (0.0f, 500.0f, 50.0f);
    const auto midiNames       = midiTypeChoices();

    const auto addMidiParams = [&] (int i)
    {
        addChoice (midiType (i), makeName ("MIDI", i, "Type"), midiNames, 0);
        addInt    (midiCC (i),   makeName ("MIDI", i, "CC"),   0, 127, 1);
        addFloat  (midiSmooth (i), makeName ("MIDI", i, "Smooth"), midiSmoothRange, 20.0f, "ms");
    };

    for (int i = 0; i < kNumMidiSources; ++i)
        addMidiParams (i);

    // ------------------------------------------------------------ macros
    for (int i = 0; i < kNumMacros; ++i)
        addFloat (macro (i), makeName ("Macro", i, "").trim(), percentRange, 0.0f, "%");

    return layout;
}

// ------------------------------------------------------------ modulation
bool isModulatable (const juce::String& id)
{
    // The table is generated once, on first use, from the same ID builders the
    // layout uses — so this is an exact membership test over the real parameter
    // set, not a prefix/suffix guess that could silently accept a typo. Built
    // lazily because it must not run before static init of the ID helpers;
    // message thread only, never called from process().
    static const juce::StringArray modulatableIds = buildModulatableIds();

    return modulatableIds.contains (id);
}
} // namespace ember::pid
