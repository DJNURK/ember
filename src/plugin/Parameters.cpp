#include "plugin/ParameterIDs.h"
#include "dsp/styles/SaturationStyle.h"

#include <array>
#include <cmath>
#include <memory>

namespace ember::pid
{
namespace
{
// ---------------------------------------------------------------------------
// Local helpers, all internal linkage. Layout construction and the choice
// tables are message thread only. The ID builders are NOT: the processor
// resolves its parameters through them once per control block on the audio
// thread, so every ID string is built once into an IdTable and thereafter only
// copied (a refcount bump), never allocated.
// ---------------------------------------------------------------------------

/** Version hint carried by every juce::ParameterID. Bump ONLY if a parameter's
    meaning changes in a way that must invalidate existing host automation. */
constexpr int kVersionHint = 1;

/** A boolean parameter that reports back what it actually is.

    juce::AudioParameterBool::setValue stores the incoming normalised float
    verbatim, so setValue(0.82f) leaves getValue() returning 0.82 even though the
    parameter means "true". The APVTS round trip does not preserve that: the
    state stores the DENORMALISED value, which snaps to 1, so saving and
    reloading turns 0.82 into 1.0 and the parameter looks as though it was not
    restored. pluginval's state test sets random normalised values and catches
    exactly this at strictness 10.

    AudioParameterBool::setValue is private, so it cannot simply be wrapped.
    This is the same parameter with one behavioural difference: the value is
    snapped on the way in, so what you read back is what the parameter is and
    the save/restore round trip is exact. */
class EmberBoolParameter final : public juce::RangedAudioParameter
{
public:
    EmberBoolParameter(const juce::ParameterID& parameterId, const juce::String& parameterName, bool defaultValue,
                       juce::AudioParameterBoolAttributes attributes = {})
        : juce::RangedAudioParameter(parameterId, parameterName,
                                     attributes.getAudioProcessorParameterWithIDAttributes()),
          range(0.0f, 1.0f, 1.0f), value(defaultValue ? 1.0f : 0.0f), defaultNormalised(defaultValue ? 1.0f : 0.0f),
          stringFromBool(attributes.getStringFromValueFunction()),
          boolFromString(attributes.getValueFromStringFunction())
    {
    }

    bool get() const noexcept { return value.load() >= 0.5f; }
    explicit operator bool() const noexcept { return get(); }

    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }
    float getValue() const override { return value.load(); }
    void setValue(float newValue) override { value.store(newValue >= 0.5f ? 1.0f : 0.0f); }
    float getDefaultValue() const override { return defaultNormalised; }
    int getNumSteps() const override { return 2; }
    bool isDiscrete() const override { return true; }
    bool isBoolean() const override { return true; }

    juce::String getText(float normalised, int maximumLength) const override
    {
        const bool b = normalised >= 0.5f;
        auto text = stringFromBool != nullptr ? stringFromBool(b, maximumLength) : (b ? "On" : "Off");
        return maximumLength > 0 ? text.substring(0, maximumLength) : text;
    }

    float getValueForText(const juce::String& text) const override
    {
        if (boolFromString != nullptr)
            return boolFromString(text) ? 1.0f : 0.0f;
        return text.equalsIgnoreCase("on") || text.equalsIgnoreCase("true") || text.getFloatValue() >= 0.5f ? 1.0f
                                                                                                            : 0.0f;
    }

private:
    juce::NormalisableRange<float> range;
    std::atomic<float> value;
    float defaultNormalised;
    std::function<juce::String(bool, int)> stringFromBool;
    std::function<bool(const juce::String&)> boolFromString;
};

/** Clamp a caller-supplied ordinal so a bad index yields a deterministic ID
    instead of undefined behaviour. The assertion fires in debug builds. */
int safeIndex(int index, int numItems) noexcept
{
    jassert(juce::isPositiveAndBelow(index, numItems));
    return juce::jlimit(0, numItems - 1, index);
}

/** "lfo" + (0 -> 1) + "Rate" == "lfo1Rate". Indices are zero-based in code and
    one-based in every ID and display name, as ParameterIDs.h specifies. */
juce::String makeId(const char* prefix, int zeroBasedIndex, const char* suffix = "")
{
    return juce::String(prefix) + juce::String(zeroBasedIndex + 1) + suffix;
}

/** "LFO" + (0 -> 1) + "Rate" == "LFO 1 Rate". */
juce::String makeName(const char* prefix, int zeroBasedIndex, const char* suffix)
{
    return juce::String(prefix) + " " + juce::String(zeroBasedIndex + 1) + " " + suffix;
}

/** An immutable table of the ids "<prefix><1..kCount><suffix>", built once on
    first use and then only ever copied.

    The declared signatures hand a juce::String back by value, and the processor
    resolves every global, crossover and band parameter through these builders
    once per 32-sample control block -- that is, on the AUDIO THREAD.
    Constructing a juce::String allocates; copying one is an atomic refcount
    bump on a buffer this table keeps alive for the life of the process. Each id
    is therefore built exactly once, off the audio thread (the layout is created
    in the processor's constructor and touches every builder), which removes the
    only heap allocation this file could put in an audio-thread path. */
template<int kCount>
class IdTable
{
public:
    IdTable(const char* prefix, const char* suffix)
    {
        for (int i = 0; i < kCount; ++i)
            entries[static_cast<size_t>(i)] = makeId(prefix, i, suffix);
    }

    /** Shares the cached buffer: no allocation, and a bad index still clamps. */
    const juce::String& operator[](int index) const noexcept
    {
        return entries[static_cast<size_t>(safeIndex(index, kCount))];
    }

private:
    std::array<juce::String, static_cast<size_t>(kCount)> entries;
};

/** A logarithmic range whose midpoint on the control lands on `centre`.

    juce::NormalisableRange maps value -> normalised as pow(proportion, skew),
    so placing `centre` at 0.5 means pow(p, skew) == 0.5, i.e.
    skew = log(0.5) / log(p) with p the linear proportion of `centre`. Derived,
    never a guessed magic number. */
juce::NormalisableRange<float> logRange(float start, float end, float centre, float interval = 0.0f)
{
    jassert(end > start && centre > start && centre < end);

    const auto proportion = (centre - start) / (end - start);
    const auto skew = std::log(0.5f) / std::log(proportion);

    return {start, end, interval, skew};
}

// Shared linear ranges. Gains and percentages stay linear on purpose.
const juce::NormalisableRange<float> gainRange{-24.0f, 24.0f, 0.01f};
const juce::NormalisableRange<float> driveRange{0.0f, 40.0f, 0.01f};
const juce::NormalisableRange<float> percentRange{0.0f, 100.0f, 0.1f};
const juce::NormalisableRange<float> bipolarRange{-100.0f, 100.0f, 0.1f};
const juce::NormalisableRange<float> widthRange{0.0f, 200.0f, 0.1f};
const juce::NormalisableRange<float> toneRange{-12.0f, 12.0f, 0.01f};
const juce::NormalisableRange<float> phaseRange{0.0f, 360.0f, 0.1f};
const juce::NormalisableRange<float> thresholdRange{-60.0f, 0.0f, 0.1f};

/** Default crossover edges, ascending, spread over the audible range. */
constexpr float kDefaultCrossoverHz[kMaxCrossovers] = {120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f};

/** Every continuous parameter that modulation may target.

    Built from the very same ID builders the layout uses, so the two can never
    drift apart, and no fragile string parsing is involved: membership is exact
    by construction rather than inferred from a prefix or suffix. */
juce::StringArray buildModulatableIds()
{
    juce::StringArray ids;
    ids.ensureStorageAllocated(160);

    // Global continuous controls (autoGain / osFactor / osOffline / xoverMode /
    // stereoMode / numBands / dither are bool, choice or int -> not targets).
    ids.add(inputGain);
    ids.add(outputGain);
    ids.add(globalMix);

    for (int i = 0; i < kMaxCrossovers; ++i)
        ids.add(crossover(i));

    for (int band = 0; band < kMaxBands; ++band)
    {
        ids.add(drive(band));
        ids.add(bandMix(band));
        ids.add(level(band));
        ids.add(pan(band));
        ids.add(width(band));
        ids.add(feedback(band));
        ids.add(feedbackFreq(band));
        ids.add(dynamics(band));
        ids.add(toneLow(band));
        ids.add(toneMid(band));
        ids.add(toneHigh(band));
        // style (choice), bypass and solo (bool) are deliberately absent.
    }

    for (int i = 0; i < kNumXLFOs; ++i)
    {
        ids.add(lfoRate(i));
        ids.add(lfoPhase(i));
        ids.add(lfoSmooth(i));
        ids.add(lfoDepth(i));
        // lfoSync (bool) and lfoSteps (int) are deliberately absent.
    }

    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        ids.add(egAttack(i));
        ids.add(egDecay(i));
        ids.add(egSustain(i));
        ids.add(egRelease(i));
        ids.add(egThreshold(i));
        // egTrigger (choice) is deliberately absent.
    }

    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        ids.add(efAttack(i));
        ids.add(efRelease(i));
        ids.add(efGain(i));
        // efBand (int) is deliberately absent.
    }

    ids.add(xyX);
    ids.add(xyY);

    for (int i = 0; i < kNumMidiSources; ++i)
        ids.add(midiSmooth(i)); // midiType (choice) and midiCC (int) are not targets.

    for (int i = 0; i < kNumMacros; ++i)
        ids.add(macro(i));

    return ids;
}
} // namespace

// --------------------------------------------------------------------- IDs
juce::String crossover(int i)
{
    static const IdTable<kMaxCrossovers> t("xover", "");
    return t[i];
}

juce::String drive(int band)
{
    static const IdTable<kMaxBands> t("band", "Drive");
    return t[band];
}
juce::String bandMix(int band)
{
    static const IdTable<kMaxBands> t("band", "Mix");
    return t[band];
}
juce::String level(int band)
{
    static const IdTable<kMaxBands> t("band", "Level");
    return t[band];
}
juce::String pan(int band)
{
    static const IdTable<kMaxBands> t("band", "Pan");
    return t[band];
}
juce::String width(int band)
{
    static const IdTable<kMaxBands> t("band", "Width");
    return t[band];
}
juce::String style(int band)
{
    static const IdTable<kMaxBands> t("band", "Style");
    return t[band];
}
juce::String feedback(int band)
{
    static const IdTable<kMaxBands> t("band", "Feedback");
    return t[band];
}
juce::String feedbackFreq(int band)
{
    static const IdTable<kMaxBands> t("band", "FeedbackFreq");
    return t[band];
}
juce::String dynamics(int band)
{
    static const IdTable<kMaxBands> t("band", "Dynamics");
    return t[band];
}
juce::String toneLow(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneLow");
    return t[band];
}
juce::String toneMid(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneMid");
    return t[band];
}
const char* const xyAxis = "xyAxis";

juce::String toneHigh(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneHigh");
    return t[band];
}

juce::String toneLowHz(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneLowHz");
    return t[band];
}

juce::String toneMidHz(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneMidHz");
    return t[band];
}

juce::String toneMidQ(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneMidQ");
    return t[band];
}

juce::String toneHighHz(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneHighHz");
    return t[band];
}

juce::String tonePre(int band)
{
    static const IdTable<kMaxBands> t("band", "TonePre");
    return t[band];
}

juce::String toneBypass(int band)
{
    static const IdTable<kMaxBands> t("band", "ToneBypass");
    return t[band];
}
juce::String bypass(int band)
{
    static const IdTable<kMaxBands> t("band", "Bypass");
    return t[band];
}
juce::String solo(int band)
{
    static const IdTable<kMaxBands> t("band", "Solo");
    return t[band];
}

juce::String lfoRate(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Rate");
    return t[i];
}
juce::String lfoSync(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Sync");
    return t[i];
}
juce::String lfoPhase(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Phase");
    return t[i];
}
juce::String lfoSmooth(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Smooth");
    return t[i];
}
juce::String lfoSteps(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Steps");
    return t[i];
}
juce::String lfoDepth(int i)
{
    static const IdTable<kNumXLFOs> t("lfo", "Depth");
    return t[i];
}

juce::String egAttack(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Attack");
    return t[i];
}
juce::String egDecay(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Decay");
    return t[i];
}
juce::String egSustain(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Sustain");
    return t[i];
}
juce::String egRelease(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Release");
    return t[i];
}
juce::String egThreshold(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Threshold");
    return t[i];
}
juce::String egTrigger(int i)
{
    static const IdTable<kNumEnvGenerators> t("eg", "Trigger");
    return t[i];
}

juce::String efAttack(int i)
{
    static const IdTable<kNumEnvFollowers> t("ef", "Attack");
    return t[i];
}
juce::String efRelease(int i)
{
    static const IdTable<kNumEnvFollowers> t("ef", "Release");
    return t[i];
}
juce::String efBand(int i)
{
    static const IdTable<kNumEnvFollowers> t("ef", "Band");
    return t[i];
}
juce::String efGain(int i)
{
    static const IdTable<kNumEnvFollowers> t("ef", "Gain");
    return t[i];
}

juce::String midiType(int i)
{
    static const IdTable<kNumMidiSources> t("midi", "Type");
    return t[i];
}
juce::String midiCC(int i)
{
    static const IdTable<kNumMidiSources> t("midi", "CC");
    return t[i];
}
juce::String midiSmooth(int i)
{
    static const IdTable<kNumMidiSources> t("midi", "Smooth");
    return t[i];
}

juce::String macro(int i)
{
    static const IdTable<kNumMacros> t("macro", "");
    return t[i];
}

// ----------------------------------------------------------------- choices
juce::StringArray styleChoices()
{
    juce::StringArray choices;
    choices.ensureStorageAllocated(kNumStyles);

    for (int i = 0; i < kNumStyles; ++i)
        choices.add(getStyleName(static_cast<StyleID>(i)));

    return choices;
}

juce::StringArray oversamplingChoices()
{
    return {"Off", "2x", "4x", "8x", "16x"};
}
juce::StringArray stereoModeChoices()
{
    return {"Stereo", "Mid-Side"};
}
juce::StringArray crossoverModeChoices()
{
    return {"Minimum Phase", "Linear Phase"};
}
juce::StringArray ditherChoices()
{
    return {"Off", "Rectangular", "Triangular"};
}
juce::StringArray lfoSyncChoices()
{
    return {"Free", "Tempo Sync"};
}
juce::StringArray egTriggerChoices()
{
    return {"Input Transient", "MIDI Note"};
}
juce::StringArray midiTypeChoices()
{
    return {"Velocity", "CC", "Mod Wheel", "Note Number"};
}

// ------------------------------------------------------------------ layout
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto addFloat = [&layout](const juce::String& paramId, const juce::String& name,
                                    const juce::NormalisableRange<float>& range, float defaultValue, const char* label)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{paramId, kVersionHint}, name, range,
                                                               defaultValue,
                                                               juce::AudioParameterFloatAttributes().withLabel(label)));
    };

    const auto addBool = [&layout](const juce::String& paramId, const juce::String& name, bool defaultValue,
                                   juce::AudioParameterBoolAttributes attributes = {})
    {
        layout.add(std::make_unique<EmberBoolParameter>(juce::ParameterID{paramId, kVersionHint}, name, defaultValue,
                                                        std::move(attributes)));
    };

    const auto addChoice = [&layout](const juce::String& paramId, const juce::String& name,
                                     const juce::StringArray& choices, int defaultIndex)
    {
        jassert(juce::isPositiveAndBelow(defaultIndex, choices.size()));
        layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{paramId, kVersionHint}, name, choices,
                                                                defaultIndex));
    };

    const auto addInt = [&layout](const juce::String& paramId, const juce::String& name, int minValue, int maxValue,
                                  int defaultValue, juce::AudioParameterIntAttributes attributes = {})
    {
        layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{paramId, kVersionHint}, name, minValue,
                                                             maxValue, defaultValue, std::move(attributes)));
    };

    // ------------------------------------------------------------- global
    addFloat(inputGain, "Input Gain", gainRange, 0.0f, "dB");
    addFloat(outputGain, "Output Gain", gainRange, 0.0f, "dB");
    addFloat(globalMix, "Mix", percentRange, 100.0f, "%");
    addBool(autoGain, "Auto Gain", false);

    addChoice(osFactor, "Oversampling", oversamplingChoices(), static_cast<int>(OversamplingFactor::x2));
    addChoice(osOffline, "Oversampling Offline", oversamplingChoices(), static_cast<int>(OversamplingFactor::x8));
    addChoice(xoverMode, "Crossover Mode", crossoverModeChoices(), static_cast<int>(CrossoverMode::MinimumPhaseLR4));
    addChoice(stereoMode, "Stereo Mode", stereoModeChoices(), static_cast<int>(StereoMode::Stereo));
    addChoice(ditherMode, "Dither", ditherChoices(), static_cast<int>(DitherMode::Triangular));

    // An XY pad has two dimensions but a modulation source emits one value, so
    // this picks which. Y was stored and smoothed but never emitted before
    // this existed - the axis defaulted to X and nothing could change it.
    addChoice(xyAxis, "XY Axis", {"X", "Y"}, 0);

    addInt(numBands, "Band Count", kMinBands, kMaxBands, 3);

    // ---------------------------------------------------------- crossovers
    const auto crossoverRange = logRange(20.0f, 20000.0f, 1000.0f);

    for (int i = 0; i < kMaxCrossovers; ++i)
        addFloat(crossover(i), makeName("Crossover", i, "Freq"), crossoverRange, kDefaultCrossoverHz[i], "Hz");

    // ------------------------------------------------------------- bands
    const auto feedbackFreqRange = logRange(20.0f, 2000.0f, 200.0f);
    const auto allStyles = styleChoices();

    const auto addBandParams = [&](int band)
    {
        addFloat(drive(band), makeName("Band", band, "Drive"), driveRange, 0.0f, "dB");
        addFloat(bandMix(band), makeName("Band", band, "Mix"), percentRange, 100.0f, "%");
        addFloat(level(band), makeName("Band", band, "Level"), gainRange, 0.0f, "dB");
        addFloat(pan(band), makeName("Band", band, "Pan"), bipolarRange, 0.0f, "");
        addFloat(width(band), makeName("Band", band, "Width"), widthRange, 100.0f, "%");

        addChoice(style(band), makeName("Band", band, "Style"), allStyles, static_cast<int>(StyleID::CleanTube));

        addFloat(feedback(band), makeName("Band", band, "Feedback"), percentRange, 0.0f, "%");
        addFloat(feedbackFreq(band), makeName("Band", band, "Feedback Freq"), feedbackFreqRange, 200.0f, "Hz");
        addFloat(dynamics(band), makeName("Band", band, "Dynamics"), bipolarRange, 0.0f, "%");

        addFloat(toneLow(band), makeName("Band", band, "Tone Low"), toneRange, 0.0f, "dB");
        addFloat(toneMid(band), makeName("Band", band, "Tone Mid"), toneRange, 0.0f, "dB");
        addFloat(toneHigh(band), makeName("Band", band, "Tone High"), toneRange, 0.0f, "dB");

        // Node positions. The defaults are the values these were as fixed
        // constants, so a session saved before they existed restores the
        // identical response rather than snapping to a range midpoint.
        addFloat(toneLowHz(band), makeName("Band", band, "Tone Low Freq"), logRange(20.0f, 1000.0f, 150.0f), 150.0f,
                 "Hz");
        addFloat(toneMidHz(band), makeName("Band", band, "Tone Mid Freq"), logRange(100.0f, 8000.0f, 1000.0f), 1000.0f,
                 "Hz");
        addFloat(toneMidQ(band), makeName("Band", band, "Tone Mid Q"), logRange(0.2f, 6.0f, 1.0f), 0.7f, "");
        addFloat(toneHighHz(band), makeName("Band", band, "Tone High Freq"), logRange(1000.0f, 18000.0f, 4000.0f),
                 4000.0f, "Hz");

        addBool(tonePre(band), makeName("Band", band, "Tone Pre"), false,
                juce::AudioParameterBoolAttributes().withStringFromValueFunction(
                    [](bool value, int) { return juce::String(value ? "Pre" : "Post"); }));

        addBool(toneBypass(band), makeName("Band", band, "Tone Bypass"), false);

        addBool(bypass(band), makeName("Band", band, "Bypass"), false);
        addBool(solo(band), makeName("Band", band, "Solo"), false);
    };

    for (int band = 0; band < kMaxBands; ++band)
        addBandParams(band);

    // ------------------------------------------------------------- XLFOs
    const auto lfoRateRange = logRange(0.01f, 40.0f, 2.0f);
    const auto syncNames = lfoSyncChoices();

    const auto addLfoParams = [&](int i)
    {
        addFloat(lfoRate(i), makeName("LFO", i, "Rate"), lfoRateRange, 2.0f, "Hz");

        addBool(lfoSync(i), makeName("LFO", i, "Sync"), false,
                juce::AudioParameterBoolAttributes().withStringFromValueFunction([names = syncNames](bool value, int)
                                                                                 { return names[value ? 1 : 0]; }));

        addFloat(lfoPhase(i), makeName("LFO", i, "Phase"), phaseRange, 0.0f, "deg");
        addFloat(lfoSmooth(i), makeName("LFO", i, "Smooth"), percentRange, 0.0f, "%");

        // 0 steps == smooth (stepping off); 1..32 quantises the shape.
        addInt(lfoSteps(i), makeName("LFO", i, "Steps"), 0, 32, 0,
               juce::AudioParameterIntAttributes().withStringFromValueFunction(
                   [](int value, int) { return value == 0 ? juce::String("Off") : juce::String(value); }));

        addFloat(lfoDepth(i), makeName("LFO", i, "Depth"), percentRange, 100.0f, "%");
    };

    for (int i = 0; i < kNumXLFOs; ++i)
        addLfoParams(i);

    // ------------------------------------------------- envelope generators
    const auto egAttackRange = logRange(0.1f, 2000.0f, 20.0f);
    const auto egDecayRange = logRange(1.0f, 5000.0f, 200.0f);
    const auto egReleaseRange = logRange(1.0f, 5000.0f, 200.0f);
    const auto triggerNames = egTriggerChoices();

    const auto addEgParams = [&](int i)
    {
        addFloat(egAttack(i), makeName("Env Gen", i, "Attack"), egAttackRange, 10.0f, "ms");
        addFloat(egDecay(i), makeName("Env Gen", i, "Decay"), egDecayRange, 200.0f, "ms");
        addFloat(egSustain(i), makeName("Env Gen", i, "Sustain"), percentRange, 50.0f, "%");
        addFloat(egRelease(i), makeName("Env Gen", i, "Release"), egReleaseRange, 300.0f, "ms");
        addFloat(egThreshold(i), makeName("Env Gen", i, "Threshold"), thresholdRange, -24.0f, "dB");

        addChoice(egTrigger(i), makeName("Env Gen", i, "Trigger"), triggerNames, 0);
    };

    for (int i = 0; i < kNumEnvGenerators; ++i)
        addEgParams(i);

    // -------------------------------------------------- envelope followers
    const auto efAttackRange = logRange(0.1f, 500.0f, 10.0f);
    const auto efReleaseRange = logRange(1.0f, 2000.0f, 100.0f);

    const auto addEfParams = [&](int i)
    {
        addFloat(efAttack(i), makeName("Env Follower", i, "Attack"), efAttackRange, 5.0f, "ms");
        addFloat(efRelease(i), makeName("Env Follower", i, "Release"), efReleaseRange, 100.0f, "ms");

        // 0 == full range sidechain; 1..kMaxBands listen to that band only.
        addInt(efBand(i), makeName("Env Follower", i, "Band"), 0, kMaxBands, 0,
               juce::AudioParameterIntAttributes().withStringFromValueFunction(
                   [](int value, int)
                   { return value == 0 ? juce::String("Full Range") : juce::String("Band ") + juce::String(value); }));

        addFloat(efGain(i), makeName("Env Follower", i, "Gain"), gainRange, 0.0f, "dB");
    };

    for (int i = 0; i < kNumEnvFollowers; ++i)
        addEfParams(i);

    // ------------------------------------------------------ XY controller
    addFloat(xyX, "XY X", percentRange, 50.0f, "%");
    addFloat(xyY, "XY Y", percentRange, 50.0f, "%");

    // ------------------------------------------------------ MIDI sources
    const auto midiSmoothRange = logRange(0.0f, 500.0f, 50.0f);
    const auto midiNames = midiTypeChoices();

    const auto addMidiParams = [&](int i)
    {
        addChoice(midiType(i), makeName("MIDI", i, "Type"), midiNames, 0);
        addInt(midiCC(i), makeName("MIDI", i, "CC"), 0, 127, 1);
        addFloat(midiSmooth(i), makeName("MIDI", i, "Smooth"), midiSmoothRange, 20.0f, "ms");
    };

    for (int i = 0; i < kNumMidiSources; ++i)
        addMidiParams(i);

    // ------------------------------------------------------------ macros
    for (int i = 0; i < kNumMacros; ++i)
        addFloat(macro(i), makeName("Macro", i, "").trim(), percentRange, 0.0f, "%");

    return layout;
}

// ------------------------------------------------------------ modulation
bool isModulatable(const juce::String& id)
{
    // The table is generated once, on first use, from the same ID builders the
    // layout uses — so this is an exact membership test over the real parameter
    // set, not a prefix/suffix guess that could silently accept a typo. Built
    // lazily because it must not run before static init of the ID helpers;
    // message thread only, never called from process().
    static const juce::StringArray modulatableIds = buildModulatableIds();

    return modulatableIds.contains(id);
}
} // namespace ember::pid
