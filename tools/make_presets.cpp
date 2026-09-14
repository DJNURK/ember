// Generates the factory preset XML files from the design in
// resources/presets/MANIFEST.md.
//
// Presets are emitted from the REAL parameter layout rather than written by
// hand, so every id and every modulation target index is correct by
// construction instead of by proofreading. Concretely:
//
//   * every value below is stated in the parameter's own engineering units
//     (dB, %, Hz, or a choice INDEX) and is range-checked against the real
//     `NormalisableRange` before it is written;
//   * styles are looked up by NAME in `pid::styleChoices()`, so a renamed or
//     renumbered `StyleID` is a build-time failure here rather than a silent
//     change of character in every preset;
//   * modulation target indices are computed by walking `getParameters()` and
//     keeping the modulatable ones, exactly as
//     `EmberAudioProcessor::buildModulationTargetTable()` does;
//   * modulation source indices come from `flatSourceIndex()`, never a guess.
//
// Nothing is written to disk unless every preset validates, so a transcription
// slip cannot half-update the library.
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "dsp/EmberTypes.h"
#include "dsp/modulation/ModTypes.h"
#include "dsp/modulation/ModulationEngine.h"
#include "dsp/styles/SaturationStyle.h"
#include "plugin/ParameterIDs.h"

using namespace ember;

namespace
{
// ---------------------------------------------------------------- diagnostics
int errorCount = 0;

void fail(const juce::String& message)
{
    std::fprintf(stderr, "make_presets: ERROR: %s\n", message.toRawUTF8());
    ++errorCount;
}

// ------------------------------------------------------------------ the host
/** Minimal host for an APVTS built from the real layout — the same shape as
    `LayoutProbe` in tests/test_presets.cpp, so the tool and the test agree on
    what the plugin's parameter set actually is. */
class LayoutHost : public juce::AudioProcessor
{
public:
    LayoutHost() : apvts(*this, nullptr, "EMBER", pid::createParameterLayout()) {}

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "LayoutHost"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    juce::AudioProcessorValueTreeState apvts;
};

// ------------------------------------------------------------ preset "schema"
/** Sentinel for a manifest cell of "—": write the parameter's own default. */
constexpr float kUseDefault = -1.0e9f;

/** One row of a manifest per-band table, in the manifest's own column order:
    style, drive, mix, level, pan, width, feedback, feedback freq, dynamics,
    tone low, tone mid, tone high. */
struct BandSpec
{
    const char* style;
    float drive, mix, level, pan, width, feedback, feedbackFreq, dynamics, low, mid, high;
};

/** One `<source> -> <destination>, <amount>, <curve>` line. `amountPercent` is
    the manifest's signed percentage; it becomes `amount / 100` in the XML. */
struct ModSpec
{
    ModSourceType sourceType;
    int sourceOrdinal; ///< zero-based ("XLFO 2" in the manifest is 1)
    juce::String targetId;
    float amountPercent;
    ModCurve curve;
    float smoothingMs{5.0f};
};

/** A "Source setup" entry: any parameter written verbatim by id. */
struct Extra
{
    juce::String id;
    float value;
};

struct Preset
{
    juce::String name, category, fileName;
    std::vector<float> crossovers; ///< only the edges the preset uses
    OversamplingFactor os{OversamplingFactor::x4};
    OversamplingFactor osOffline{OversamplingFactor::Count}; ///< Count => follow `os`
    float globalMix{100.0f};
    bool autoGain{true};
    StereoMode stereo{StereoMode::Stereo};
    CrossoverMode xoverMode{CrossoverMode::MinimumPhaseLR4};
    DitherMode dither{DitherMode::Off};
    float inputGain{0.0f}, outputGain{0.0f};
    std::vector<BandSpec> bands;
    std::vector<Extra> extras;
    std::vector<ModSpec> mods;

    int numBands() const { return static_cast<int>(bands.size()); }
};

Preset makePreset(const char* fileName, const char* name, const char* category, std::vector<float> crossovers,
                  std::vector<BandSpec> bands)
{
    Preset p;
    p.fileName = fileName;
    p.name = name;
    p.category = category;
    p.crossovers = std::move(crossovers);
    p.bands = std::move(bands);
    return p;
}

// ------------------------------------------------- "Source setup" shorthands
void setEnvFollower(Preset& p, int i, int band, float attackMs, float releaseMs, float gainDb)
{
    // `band` follows the parameter's own convention: 0 == full range,
    // 1..kMaxBands == that (one-based) band, which is how the manifest writes it.
    p.extras.push_back({pid::efBand(i), static_cast<float>(band)});
    p.extras.push_back({pid::efAttack(i), attackMs});
    p.extras.push_back({pid::efRelease(i), releaseMs});
    p.extras.push_back({pid::efGain(i), gainDb});
}

void setEnvGenerator(Preset& p, int i, int triggerIndex, float thresholdDb, float attackMs, float decayMs,
                     float sustainPercent, float releaseMs)
{
    p.extras.push_back({pid::egTrigger(i), static_cast<float>(triggerIndex)});
    p.extras.push_back({pid::egThreshold(i), thresholdDb});
    p.extras.push_back({pid::egAttack(i), attackMs});
    p.extras.push_back({pid::egDecay(i), decayMs});
    p.extras.push_back({pid::egSustain(i), sustainPercent});
    p.extras.push_back({pid::egRelease(i), releaseMs});
}

void setXlfoCommon(Preset& p, int i, int steps, float depthPercent, float phaseDeg, float smoothPercent)
{
    // The manifest's "N-point <shape>" describes the XLFO shape editor, which
    // has no parameter in the layout yet. Its one storable aspect is the step
    // count, so stepped / gate / sample-and-hold shapes carry their point count
    // in `lfoSteps` and smooth shapes (triangle, sine-ish) carry 0 == off.
    p.extras.push_back({pid::lfoSteps(i), static_cast<float>(steps)});
    p.extras.push_back({pid::lfoDepth(i), depthPercent});
    p.extras.push_back({pid::lfoPhase(i), phaseDeg});
    p.extras.push_back({pid::lfoSmooth(i), smoothPercent});
}

void setXlfoFree(Preset& p, int i, float rateHz, int steps, float depthPercent, float phaseDeg, float smoothPercent)
{
    p.extras.push_back({pid::lfoSync(i), 0.0f}); // Free
    p.extras.push_back({pid::lfoRate(i), rateHz});
    setXlfoCommon(p, i, steps, depthPercent, phaseDeg, smoothPercent);
}

void setXlfoSynced(Preset& p, int i, float cycleBeats, int steps, float depthPercent, float phaseDeg,
                   float smoothPercent)
{
    // MANIFEST open gap: the layout has a boolean `lfoSync` and a free-running
    // `lfoRate` in Hz, and no parameter for the tempo division the manifest
    // asks for (1/16 … 4 bars). The convention adopted here is: store
    // `lfoSync` = Tempo Sync and store the division's rate at 120 BPM in
    // `lfoRate`, so the preset is already at the intended speed when the
    // transport is stopped or the host reports no tempo, and only needs the
    // division re-read once a division parameter exists.
    constexpr double kReferenceBpm = 120.0;
    const auto rateHz = static_cast<float>((kReferenceBpm / 60.0) / static_cast<double>(cycleBeats));

    p.extras.push_back({pid::lfoSync(i), 1.0f}); // Tempo Sync
    p.extras.push_back({pid::lfoRate(i), rateHz});
    setXlfoCommon(p, i, steps, depthPercent, phaseDeg, smoothPercent);
}

void setMacro(Preset& p, int i, float percent)
{
    p.extras.push_back({pid::macro(i), percent});
}

void setXy(Preset& p, float xPercent, float yPercent)
{
    p.extras.push_back({pid::xyX, xPercent});
    p.extras.push_back({pid::xyY, yPercent});
}

void setMidiSource(Preset& p, int i, int typeIndex, int cc, float smoothMs)
{
    p.extras.push_back({pid::midiType(i), static_cast<float>(typeIndex)});
    p.extras.push_back({pid::midiCC(i), static_cast<float>(cc)});
    p.extras.push_back({pid::midiSmooth(i), smoothMs});
}

// ------------------------------------------------------------------ the bank
/** The 35 presets of resources/presets/MANIFEST.md, transcribed verbatim.
    Band rows are in the manifest's fixed column order; `kUseDefault` is its
    "—". */
std::vector<Preset> buildPresetBank()
{
    std::vector<Preset> bank;
    constexpr float d = kUseDefault;

    // =================================================================== Drums
    {
        auto p = makePreset("Drums_01_Kick_Weight_and_Click.xml", "Kick Weight and Click", "Drums", {95.0f, 2200.0f},
                            {{"Transformer", 12, 80, 1.0f, 0, 100, 0, d, 25, 1.5f, 0.0f, -1.0f},
                             {"Warm Tube", 6, 45, -1.5f, 0, 100, 0, d, -15, -2.0f, -1.0f, 0.0f},
                             {"Bright Tape", 9, 60, 0.5f, 0, 100, 0, d, -30, 0.0f, 1.0f, 2.0f}});
        p.os = OversamplingFactor::x4;
        p.autoGain = true;
        setEnvFollower(p, 0, 1, 1.0f, 90.0f, 3.0f);
        p.mods.push_back({ModSourceType::EnvelopeFollower, 0, pid::drive(2), 18.0f, ModCurve::ExpoOut});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_02_Snare_Crack.xml", "Snare Crack", "Drums", {180.0f, 3500.0f},
                            {{"Warm Tube", 8, 55, 0.0f, 0, 100, 0, d, 15, 1.0f, 0.0f, 0.0f},
                             {"Crunch", 7, 35, -0.5f, 0, 100, 0, d, -20, -1.5f, 1.0f, 0.0f},
                             {"Bright Tape", 11, 65, 1.0f, 0, 100, 0, d, -35, 0.0f, 1.0f, 2.5f}});
        p.os = OversamplingFactor::x4;
        setEnvGenerator(p, 0, 0, -22.0f, 0.5f, 60.0f, 0.0f, 120.0f);
        p.mods.push_back({ModSourceType::EnvelopeGenerator, 0, pid::drive(2), 22.0f, ModCurve::ExpoOut});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_03_Room_Mic_Glue.xml", "Room Mic Glue", "Drums", {350.0f},
                            {{"Warm Tape", 10, 70, -1.0f, 0, 100, 0, d, 40, 1.0f, -1.0f, 0.0f},
                             {"Warm Tape", 14, 85, 0.0f, 0, 130, 0, d, 55, -1.0f, 1.5f, 1.0f}});
        p.os = OversamplingFactor::x2;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_04_Overhead_Sheen.xml", "Overhead Sheen", "Drums", {1800.0f},
                            {{"Subtle Tube", 5, 40, 0.0f, 0, 100, 0, d, 0, 0.0f, 0.0f, 0.0f},
                             {"Clean Tape", 7, 55, -0.5f, 0, 115, 0, d, 20, 0.0f, 0.0f, 1.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_05_Parallel_Smash_Bus.xml", "Parallel Smash Bus", "Drums", {220.0f},
                            {{"Crunch", 26, 100, -3.0f, 0, 100, 0, d, 70, 2.0f, 0.0f, -2.0f},
                             {"Lead", 30, 100, -4.0f, 0, 110, 0, d, 80, -3.0f, 2.0f, -1.0f}});
        p.os = OversamplingFactor::x8;
        p.globalMix = 35.0f;
        p.autoGain = false;   // the blend is the point
        setMacro(p, 0, 0.0f); // Macro 1 "Blend"
        p.mods.push_back({ModSourceType::Macro, 0, pid::globalMix, 50.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_06_Hat_and_Shaker_Polish.xml", "Hat and Shaker Polish", "Drums", {4000.0f},
                            {{"Clean Tube", 3, 25, 0.0f, 0, 100, 0, d, 0, 0.0f, 0.0f, 0.0f},
                             {"Shimmer", 6, 45, -0.5f, 0, 110, 0, d, -25, 0.0f, 0.0f, 1.5f}});
        p.os = OversamplingFactor::x8; // all the content is near Nyquist
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Drums_07_Kit_Sculpt_Six.xml", "Kit Sculpt Six", "Drums",
                            {60.0f, 130.0f, 400.0f, 1500.0f, 5000.0f},
                            {{"Clean Tube", 5, 30, 0.0f, 0, 90, 0, d, 25, 1.0f, 0.0f, 0.0f},
                             {"Transformer", 12, 70, 0.5f, 0, 95, 0, d, 20, 1.0f, 0.0f, 0.0f},
                             {"Warm Tube", 7, 40, -1.0f, 0, 100, 0, d, -10, -1.0f, 0.0f, 0.0f},
                             {"Subtle Tube", 4, 25, -1.5f, 0, 100, 0, d, -15, -1.5f, 0.0f, 0.0f},
                             {"Crunch", 10, 45, 0.5f, 0, 105, 0, d, -25, 0.0f, 1.5f, 0.0f},
                             {"Bright Tape", 8, 50, 0.0f, 0, 115, 0, d, -20, 0.0f, 0.0f, 1.5f}});
        p.os = OversamplingFactor::x8;
        setEnvFollower(p, 0, 2, 2.0f, 120.0f, 0.0f);
        p.mods.push_back({ModSourceType::EnvelopeFollower, 0, pid::drive(4), 20.0f, ModCurve::ExpoOut});
        bank.push_back(std::move(p));
    }

    // ==================================================================== Bass
    {
        auto p = makePreset("Bass_08_Sub_Anchor.xml", "Sub Anchor", "Bass", {70.0f, 700.0f},
                            {{"Clean Tube", 4, 20, 0.5f, 0, 70, 0, d, 30, 1.0f, 0.0f, 0.0f},
                             {"Transformer", 13, 75, 0.0f, 0, 100, 0, d, 10, -1.0f, 1.0f, 0.0f},
                             {"Warm Tube", 9, 55, 0.5f, 0, 100, 0, d, 0, 0.0f, 1.0f, 0.5f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Bass_09_Bass_Grind_Upper.xml", "Bass Grind Upper", "Bass", {90.0f, 900.0f},
                            {{"Subtle Tube", 3, 15, 1.0f, 0, 70, 0, d, 20, 0.5f, 0.0f, 0.0f},
                             {"Warm Tape", 11, 60, -0.5f, 0, 100, 0, d, 15, -2.0f, 1.0f, 0.0f},
                             {"Crunch", 22, 70, 2.0f, 0, 100, 0, d, -10, -2.0f, 2.0f, 1.0f}});
        p.os = OversamplingFactor::x8; // Crunch at 22 dB aliases badly below that
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Bass_10_Round_and_Warm.xml", "Round and Warm", "Bass", {500.0f},
                            {{"Warm Tube", 10, 65, 0.0f, 0, 85, 0, d, 25, 1.5f, 0.0f, -1.0f},
                             {"Warm Tape", 8, 50, -1.0f, 0, 100, 0, d, 10, 0.0f, 0.5f, -1.5f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Bass_11_Synth_Bass_Iron.xml", "Synth Bass Iron", "Bass", {120.0f},
                            {{"Transformer", 16, 90, 0.0f, 0, 80, 12, 55.0f, 35, 2.0f, 0.0f, -1.0f},
                             {"Transformer", 10, 60, -0.5f, 0, 100, 0, d, 0, 0.0f, 1.0f, 0.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Bass_12_Long_Sub_Bloom.xml", "Long Sub Bloom", "Bass", {110.0f},
                            {{"Clean Tube", 8, 45, 0.0f, 0, 60, 18, 45.0f, 60, 1.0f, 0.0f, 0.0f},
                             {"Smudge", 14, 55, -1.0f, 0, 100, 0, d, 20, -1.0f, 1.0f, -1.0f}});
        p.os = OversamplingFactor::x8;
        setEnvFollower(p, 0, 1, 5.0f, 400.0f, 0.0f);
        p.mods.push_back({ModSourceType::EnvelopeFollower, 0, pid::bandMix(1), -35.0f, ModCurve::SCurve, 60.0f});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Bass_13_Bass_Reamp_Bite.xml", "Bass Reamp Bite", "Bass", {100.0f, 1200.0f},
                            {{"Clean Tube", 5, 25, 1.0f, 0, 70, 0, d, 25, 1.5f, 0.0f, 0.0f},
                             {"Clean Amp", 18, 80, -1.0f, 0, 100, 0, d, 0, -1.0f, 1.5f, 0.0f},
                             {"Lead", 24, 65, 0.0f, 0, 100, 0, d, -15, -3.0f, 2.0f, -1.0f}});
        p.os = OversamplingFactor::x8;
        bank.push_back(std::move(p));
    }

    // ================================================================== Vocals
    {
        auto p = makePreset("Vocals_14_Vocal_Presence_Lift.xml", "Vocal Presence Lift", "Vocals", {200.0f, 2500.0f},
                            {{"Subtle Tube", 3, 20, -0.5f, 0, 100, 0, d, 15, -1.0f, 0.0f, 0.0f},
                             {"Warm Tube", 6, 40, 0.0f, 0, 100, 0, d, 20, 0.0f, 0.5f, 0.0f},
                             {"Clean Tube", 8, 50, 0.5f, 0, 100, 0, d, -10, 0.0f, 1.0f, 1.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Vocals_15_Intimate_Whisper.xml", "Intimate Whisper", "Vocals", {400.0f},
                            {{"Subtle Tube", 4, 25, 0.0f, 0, 100, 0, d, 35, -1.5f, 0.0f, 0.0f},
                             {"Breathe", 7, 35, 0.5f, 0, 105, 0, d, 40, 0.0f, 1.0f, 1.5f}});
        p.os = OversamplingFactor::x4;
        setEnvFollower(p, 1, 2, 8.0f, 250.0f, 0.0f);
        p.mods.push_back({ModSourceType::EnvelopeFollower, 1, pid::drive(1), -25.0f, ModCurve::ExpoOut, 30.0f});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Vocals_16_Rap_Vocal_Edge.xml", "Rap Vocal Edge", "Vocals", {180.0f, 3000.0f},
                            {{"Warm Tube", 5, 30, -1.0f, 0, 100, 0, d, 25, -2.0f, 0.0f, 0.0f},
                             {"Transformer", 12, 60, 0.5f, 0, 100, 0, d, 30, -1.0f, 2.0f, 0.0f},
                             {"Clean Tape", 9, 45, 0.0f, 0, 100, 0, d, -15, 0.0f, 1.0f, 1.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Vocals_17_Gritty_Lead_Vocal.xml", "Gritty Lead Vocal", "Vocals", {300.0f},
                            {{"Warm Tube", 7, 35, 0.0f, 0, 100, 0, d, 20, -1.0f, 0.0f, 0.0f},
                             {"Crunch", 20, 100, -2.0f, 0, 100, 0, d, 10, -2.0f, 2.0f, -1.0f}});
        p.os = OversamplingFactor::x8;
        p.globalMix = 45.0f;
        p.autoGain = false;
        setMacro(p, 1, 0.0f); // Macro 2 "Grit"
        p.mods.push_back({ModSourceType::Macro, 1, pid::globalMix, 45.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Vocals_18_Backing_Vocal_Bed.xml", "Backing Vocal Bed", "Vocals", {600.0f},
                            {{"Warm Tape", 8, 45, -1.0f, 0, 90, 0, d, 30, -1.0f, 0.0f, 0.0f},
                             {"Clean Tape", 10, 60, 0.0f, 0, 150, 0, d, 25, 0.0f, 0.5f, 1.0f}});
        p.os = OversamplingFactor::x4;
        p.stereo = StereoMode::MidSide;
        bank.push_back(std::move(p));
    }

    // ================================================================== Guitar
    {
        auto p = makePreset("Guitar_19_Clean_Electric_Sparkle.xml", "Clean Electric Sparkle", "Guitar", {900.0f},
                            {{"Clean Tube", 6, 40, 0.0f, 0, 100, 0, d, 20, 0.0f, 0.0f, 0.0f},
                             {"Bright Tape", 9, 55, 0.5f, 0, 110, 0, d, 0, 0.0f, 1.0f, 1.5f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Guitar_20_Crunch_Rhythm_Stack.xml", "Crunch Rhythm Stack", "Guitar", {140.0f, 1800.0f},
                            {{"Clean Tube", 4, 25, -1.5f, 0, 85, 0, d, 30, -2.0f, 0.0f, 0.0f},
                             {"Crunch", 24, 90, 0.0f, 0, 100, 0, d, 0, -1.0f, 2.0f, 0.0f},
                             {"Warm Tape", 12, 60, -1.0f, 0, 100, 0, d, -10, 0.0f, 0.0f, -2.0f}});
        p.os = OversamplingFactor::x8;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Guitar_21_Singing_Lead_Sustain.xml", "Singing Lead Sustain", "Guitar", {500.0f},
                            {{"Clean Amp", 14, 70, -1.0f, 0, 100, 0, d, 20, -1.0f, 1.0f, 0.0f},
                             {"Lead", 30, 100, 0.0f, 0, 100, 35, 780.0f, 45, -1.0f, 2.0f, -1.0f}});
        p.os = OversamplingFactor::x8;
        setXlfoFree(p, 0, 0.18f, 0, 100.0f, 0.0f, 40.0f); // 3-point triangle
        setMidiSource(p, 0, 1, 1, 80.0f);                 // type CC, CC 1 (mod wheel)
        p.mods.push_back({ModSourceType::XLFO, 0, pid::feedbackFreq(1), 12.0f, ModCurve::SCurve, 120.0f});
        p.mods.push_back({ModSourceType::MidiSource, 0, pid::feedback(1), 40.0f, ModCurve::ExpoIn});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Guitar_22_Acoustic_Body_and_Air.xml", "Acoustic Body and Air", "Guitar", {250.0f, 4000.0f},
                            {{"Subtle Tube", 3, 20, -0.5f, 0, 100, 0, d, 15, -1.5f, 0.0f, 0.0f},
                             {"Warm Tube", 6, 35, 0.0f, 0, 100, 0, d, 10, 0.0f, 0.5f, 0.0f},
                             {"Shimmer", 5, 30, 0.0f, 0, 110, 0, d, -20, 0.0f, 0.0f, 1.5f}});
        p.os = OversamplingFactor::x8;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Guitar_23_Amp_Room_Dirt.xml", "Amp Room Dirt", "Guitar", {700.0f},
                            {{"Smudge", 18, 75, -1.0f, 0, 90, 0, d, 35, 1.0f, 0.0f, -1.0f},
                             {"Rectify", 16, 55, -1.0f, 0, 70, 0, d, 25, -2.0f, 2.0f, -3.0f}});
        p.os = OversamplingFactor::x8; // Rectify is hard-edged
        bank.push_back(std::move(p));
    }

    // ================================================================= Mix Bus
    {
        auto p = makePreset("MixBus_24_Bus_Glue_Whisper.xml", "Bus Glue Whisper", "Mix Bus", {},
                            {{"Subtle Tube", 3, 22, 0.0f, 0, 100, 0, d, 12, 0.0f, 0.0f, 0.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("MixBus_25_Tape_Cohesion.xml", "Tape Cohesion", "Mix Bus", {120.0f, 5000.0f},
                            {{"Clean Tape", 4, 30, 0.0f, 0, 95, 0, d, 15, 0.5f, 0.0f, 0.0f},
                             {"Warm Tape", 5, 35, 0.0f, 0, 100, 0, d, 20, 0.0f, 0.0f, 0.0f},
                             {"Clean Tape", 6, 30, -0.5f, 0, 105, 0, d, 10, 0.0f, 0.0f, -0.5f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("MixBus_26_Console_Weight.xml", "Console Weight", "Mix Bus", {180.0f},
                            {{"Transformer", 6, 35, 0.0f, 0, 95, 0, d, 15, 0.5f, 0.0f, 0.0f},
                             {"Transformer", 4, 25, 0.0f, 0, 100, 0, d, 10, 0.0f, 0.5f, 0.0f}});
        p.os = OversamplingFactor::x4;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("MixBus_27_Wide_and_Quiet.xml", "Wide and Quiet", "Mix Bus", {300.0f},
                            {{"Subtle Tube", 2, 15, 0.0f, 0, 85, 0, d, 10, 0.0f, 0.0f, 0.0f},
                             {"Clean Tube", 5, 30, 0.0f, 0, 125, 0, d, 10, 0.0f, 0.0f, 0.5f}});
        p.os = OversamplingFactor::x4;
        p.stereo = StereoMode::MidSide;
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("MixBus_28_Master_Polish.xml", "Master Polish", "Mix Bus", {90.0f, 700.0f, 6000.0f},
                            {{"Clean Tube", 2, 15, 0.0f, 0, 100, 0, d, 5, 0.5f, 0.0f, 0.0f},
                             {"Subtle Tube", 3, 20, 0.0f, 0, 100, 0, d, 8, 0.0f, 0.0f, 0.0f},
                             {"Subtle Tube", 2, 18, 0.0f, 0, 100, 0, d, 8, 0.0f, 0.5f, 0.0f},
                             {"Clean Tape", 3, 15, -0.5f, 0, 100, 0, d, 5, 0.0f, 0.0f, -0.5f}});
        p.os = OversamplingFactor::x8;
        p.osOffline = OversamplingFactor::x16;    // the only preset that differs
        p.xoverMode = CrossoverMode::LinearPhase; // the only linear-phase preset
        p.dither = DitherMode::Triangular;        // the only dithered preset
        bank.push_back(std::move(p));
    }

    // ================================================================ Creative
    {
        auto p = makePreset("Creative_29_Broken_Radio.xml", "Broken Radio", "Creative", {400.0f, 3000.0f},
                            {{"Broken Tube", 20, 40, -6.0f, 0, 60, 0, d, -20, -6.0f, 0.0f, 0.0f},
                             {"Decimate", 18, 100, 0.0f, 0, 70, 0, d, 0, -3.0f, 4.0f, -2.0f},
                             {"Bitcrush", 12, 70, -5.0f, 0, 60, 0, d, 0, 0.0f, 0.0f, -6.0f}});
        p.os = OversamplingFactor::x2; // the artefacts are the effect
        p.autoGain = false;
        p.outputGain = 2.0f;
        setXlfoSynced(p, 1, 0.5f, 8, 100.0f, 0.0f, 0.0f); // XLFO 2, 1/8, 8-point stepped
        setMacro(p, 2, 0.0f);                             // Macro 3 "Signal Loss"
        p.mods.push_back({ModSourceType::XLFO, 1, pid::drive(1), 35.0f, ModCurve::Stepped});
        p.mods.push_back({ModSourceType::Macro, 2, pid::globalMix, 100.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Creative_30_Ring_of_Foldback.xml", "Ring of Foldback", "Creative", {600.0f},
                            {{"Warm Tube", 8, 40, 0.0f, 0, 100, 0, d, 0, 0.0f, 0.0f, 0.0f},
                             {"Foldback", 26, 85, -4.0f, 0, 120, 0, d, -30, -2.0f, 2.0f, -3.0f}});
        p.os = OversamplingFactor::x16; // folding is the worst aliaser in the plugin
        p.globalMix = 70.0f;
        p.autoGain = false;
        setXlfoSynced(p, 0, 1.0f, 0, 100.0f, 0.0f, 25.0f); // XLFO 1, 1/4, 4-point triangle
        setEnvFollower(p, 0, 2, 10.0f, 200.0f, 0.0f);
        p.mods.push_back({ModSourceType::XLFO, 0, pid::drive(1), 45.0f, ModCurve::SCurve});
        p.mods.push_back({ModSourceType::EnvelopeFollower, 0, pid::bandMix(1), -25.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Creative_31_Pulse_Gate_Grind.xml", "Pulse Gate Grind", "Creative", {250.0f},
                            {{"Hard Clip", 16, 60, -3.0f, 0, 100, 0, d, 0, 0.0f, 0.0f, -2.0f},
                             {"Crunch", 28, 100, -2.0f, 0, 110, 0, d, 0, -2.0f, 3.0f, 0.0f}});
        p.os = OversamplingFactor::x8;
        p.autoGain = false;
        setXlfoSynced(p, 2, 0.25f, 16, 60.0f, 0.0f, 5.0f); // XLFO 3, 1/16, 16-point gate
        setMacro(p, 3, 40.0f);                             // Macro 4 "Chop Depth"
        p.mods.push_back({ModSourceType::XLFO, 2, pid::level(1), -60.0f, ModCurve::Stepped});
        p.mods.push_back({ModSourceType::XLFO, 2, pid::level(0), -40.0f, ModCurve::Stepped});
        p.mods.push_back({ModSourceType::Macro, 3, pid::lfoDepth(2), 100.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Creative_32_Resonant_Howl.xml", "Resonant Howl", "Creative", {500.0f},
                            {{"Warm Tube", 12, 50, -2.0f, 0, 100, 25, 90.0f, 20, 0.0f, 0.0f, 0.0f},
                             {"Lead", 30, 90, -3.0f, 0, 110, 65, 620.0f, 30, -3.0f, 3.0f, -2.0f}});
        p.os = OversamplingFactor::x8;
        p.globalMix = 65.0f;
        p.autoGain = false;
        setXy(p, 35.0f, 50.0f);
        setMacro(p, 4, 0.0f); // Macro 5 "Howl"
        p.mods.push_back({ModSourceType::XYController, 0, pid::feedbackFreq(1), 80.0f, ModCurve::ExpoIn});
        p.mods.push_back({ModSourceType::Macro, 4, pid::feedback(1), 50.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p =
            makePreset("Creative_33_Vowel_Sweep.xml", "Vowel Sweep", "Creative", {250.0f, 700.0f, 1800.0f, 4000.0f},
                       {{"Warm Tube", 10, 50, -1.0f, 0, 100, 0, d, 10, 0.0f, 0.0f, 0.0f},
                        {"Rectify", 18, 75, -1.0f, 0, 100, 0, d, 15, -1.0f, 3.0f, -1.0f},
                        {"Rectify", 20, 80, -1.0f, 0, 100, 0, d, 20, -2.0f, 4.0f, -2.0f},
                        {"Smudge", 14, 60, -2.0f, 0, 105, 0, d, 0, -1.0f, 2.0f, -3.0f},
                        {"Clean Tape", 8, 35, -3.0f, 0, 100, 0, d, -10, 0.0f, 0.0f, -2.0f}});
        p.os = OversamplingFactor::x8;
        p.globalMix = 85.0f;
        p.autoGain = false;
        setXlfoSynced(p, 0, 2.0f, 0, 100.0f, 0.0f, 50.0f); // XLFO 1, 1/2, 5-point sine-ish
        setEnvGenerator(p, 0, 0, -26.0f, 2.0f, 120.0f, 20.0f, 200.0f);
        setMacro(p, 5, 50.0f); // Macro 6 "Sweep Rate"
        p.mods.push_back({ModSourceType::XLFO, 0, pid::level(1), 55.0f, ModCurve::SCurve});
        p.mods.push_back({ModSourceType::XLFO, 0, pid::level(2), -55.0f, ModCurve::SCurve});
        p.mods.push_back({ModSourceType::EnvelopeGenerator, 0, pid::drive(2), 25.0f, ModCurve::ExpoOut});
        p.mods.push_back({ModSourceType::Macro, 5, pid::lfoRate(0), 50.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Creative_34_Bit_Rot_Texture.xml", "Bit Rot Texture", "Creative", {300.0f, 2500.0f},
                            {{"Broken Tube", 9, 35, -1.0f, 0, 90, 0, d, 10, -2.0f, 0.0f, 0.0f},
                             {"Bitcrush", 14, 65, -2.0f, 0, 100, 0, d, 0, -2.0f, 1.0f, -2.0f},
                             {"Decimate", 16, 55, -4.0f, 0, 80, 0, d, 0, 0.0f, 0.0f, -5.0f}});
        p.os = OversamplingFactor::Off; // oversampling a bitcrusher defeats it
        p.autoGain = false;
        p.outputGain = 3.0f;
        setXlfoFree(p, 3, 0.7f, 8, 100.0f, 0.0f, 0.0f); // XLFO 4, 8-point sample-and-hold
        setMacro(p, 6, 25.0f);                          // Macro 7 "Rot"
        p.mods.push_back({ModSourceType::XLFO, 3, pid::bandMix(2), 50.0f, ModCurve::Stepped});
        p.mods.push_back({ModSourceType::Macro, 6, pid::drive(1), 100.0f, ModCurve::Linear});
        bank.push_back(std::move(p));
    }
    {
        auto p = makePreset("Creative_35_Shimmer_Haze.xml", "Shimmer Haze", "Creative", {200.0f, 2000.0f},
                            {{"Subtle Tube", 4, 25, -1.0f, 0, 80, 0, d, 25, -2.0f, 0.0f, 0.0f},
                             {"Breathe", 12, 55, 0.0f, 0, 130, 0, d, 40, 0.0f, 1.0f, 1.0f},
                             {"Shimmer", 16, 70, -1.0f, 0, 160, 0, d, -15, 0.0f, 0.0f, 2.0f}});
        p.os = OversamplingFactor::x8;
        p.globalMix = 75.0f;
        p.autoGain = false;
        setXlfoSynced(p, 1, 16.0f, 0, 80.0f, 0.0f, 80.0f); // XLFO 2, 4 bars, 4-point triangle
        setEnvFollower(p, 2, 2, 20.0f, 500.0f, 0.0f);
        p.mods.push_back({ModSourceType::XLFO, 1, pid::width(2), 35.0f, ModCurve::SCurve, 200.0f});
        p.mods.push_back({ModSourceType::EnvelopeFollower, 2, pid::bandMix(2), -30.0f, ModCurve::ExpoOut});
        bank.push_back(std::move(p));
    }

    return bank;
}

// --------------------------------------------------------------- crossovers
/** Default crossover edges as the layout declares them, used when a preset has
    no edges of its own to extend from. */
constexpr float kDefaultCrossoverHz[kMaxCrossovers] = {120.0f, 600.0f, 2500.0f, 6000.0f, 12000.0f};

/** Minimum spacing between adjacent crossover edges: a third of an octave. */
const float kMinEdgeRatio = std::pow(2.0f, 1.0f / 3.0f);

/** All five edges for a preset that uses only the first `used.size()` of them.

    A preset with fewer bands still stores every edge, and those edges must stay
    ascending and sanely spaced: the user who raises the band count afterwards
    gets real crossovers rather than a stack of coincident ones. Unused edges
    are spread geometrically from the last used edge up towards the top of the
    audible range, which keeps the ratio between neighbours well above the
    third-octave minimum. */
std::array<float, kMaxCrossovers> resolveCrossovers(const std::vector<float>& used)
{
    std::array<float, kMaxCrossovers> out{};

    const int numUsed = static_cast<int>(used.size());

    if (numUsed == 0)
    {
        for (int i = 0; i < kMaxCrossovers; ++i)
            out[static_cast<size_t>(i)] = kDefaultCrossoverHz[i];

        return out;
    }

    for (int i = 0; i < numUsed; ++i)
        out[static_cast<size_t>(i)] = used[static_cast<size_t>(i)];

    const int numFree = kMaxCrossovers - numUsed;

    if (numFree > 0)
    {
        constexpr double kTopHz = 19000.0; // just under the range's 20 kHz end
        const double last = used.back();
        const double ratio = std::pow(kTopHz / last, 1.0 / (numFree + 1));

        // Whole Hz: a generated placeholder edge has no business claiming
        // sub-hertz precision at 9 kHz, and it keeps the stored number exact.
        for (int j = 0; j < numFree; ++j)
            out[static_cast<size_t>(numUsed + j)] = static_cast<float>(std::round(last * std::pow(ratio, j + 1)));
    }

    return out;
}

/** Ascending, and at least a third of an octave apart. A transcription slip
    that breaks this must stop the build, not ship a preset whose crossovers
    cross over each other. */
void validateCrossovers(const Preset& preset, const std::array<float, kMaxCrossovers>& edges)
{
    for (int i = 1; i < kMaxCrossovers; ++i)
    {
        const float lower = edges[static_cast<size_t>(i - 1)];
        const float upper = edges[static_cast<size_t>(i)];

        if (!(upper > lower))
        {
            fail(preset.name + ": crossover edges are not ascending (" + juce::String(lower, 2) + " Hz then " +
                 juce::String(upper, 2) + " Hz)");
            continue;
        }

        if (upper / lower < kMinEdgeRatio - 1.0e-4f)
            fail(preset.name + ": crossover edges " + juce::String(lower, 2) + " Hz and " + juce::String(upper, 2) +
                 " Hz are closer than a third of an octave");
    }
}

// ----------------------------------------------------------- parameter setting
float defaultValueOf(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
{
    if (auto* param = apvts.getParameter(id))
        return param->convertFrom0to1(param->getDefaultValue());

    fail("no such parameter: " + id);
    return 0.0f;
}

/** A value the preset asked for, in the parameter's own units. Setting goes
    through the real parameter (which is what proves the id exists and the value
    is legal); the exact number is remembered here so the emitted XML can carry
    it verbatim instead of the value that survives a normalise round trip. */
using ExactValues = std::map<std::string, double>;

void setParam(juce::AudioProcessorValueTreeState& apvts, ExactValues& exact, const juce::String& presetName,
              const juce::String& id, float value)
{
    auto* param = apvts.getParameter(id);

    if (param == nullptr)
    {
        fail(presetName + ": no such parameter '" + id + "'");
        return;
    }

    const auto& range = param->getNormalisableRange();

    if (!std::isfinite(value) || value < range.start - 1.0e-4f || value > range.end + 1.0e-4f)
    {
        fail(presetName + ": " + id + " = " + juce::String(value, 3) + " is outside [" + juce::String(range.start, 3) +
             ", " + juce::String(range.end, 3) + "]");
        return;
    }

    const auto clamped = juce::jlimit(range.start, range.end, value);
    param->setValueNotifyingHost(param->convertTo0to1(clamped));
    exact[id.toStdString()] = static_cast<double>(clamped);
}

/** Removes the float dust that a normalise round trip leaves behind.

    APVTS stores `convertFrom0to1 (convertTo0to1 (v))`, so a 200 Hz default
    comes back as 199.9999847412109 and a 0 dB default as -2.68e-7. Both load to
    the identical setting, but a factory file should read as the number the
    manifest states, and a reader should not have to wonder whether the dust
    means something.

    Rounding is to the precision the parameter itself expresses: a control that
    steps in hundredths cannot mean more than two decimals, so -2.68e-7 is dust
    and belongs at zero. The skewed ranges (frequencies, times) declare no
    interval, and there six significant digits is already far finer than the
    control. The result is re-clamped, so tidying can never push a value out of
    range. */
/** `value` rounded to `decimals` places (negative rounds to tens, hundreds...).

    The power of ten is built by repeated multiplication rather than `std::pow`,
    which is not required to return an exact 100.0 for `pow (10, 2)` and whose
    last-bit error is enough to turn a tidy 9179.03 into 9179.030000000001. */
double roundToDecimals(double value, int decimals)
{
    double power = 1.0;

    for (int i = std::abs(decimals); --i >= 0;)
        power *= 10.0;

    return decimals >= 0 ? std::round(value * power) / power : std::round(value / power) * power;
}

double tidyValue(double value, const juce::NormalisableRange<float>& range)
{
    double tidied = value;

    if (!std::isfinite(tidied))
        return static_cast<double>(range.start);

    if (range.interval > 0.0f)
    {
        const auto decimals = static_cast<int>(std::ceil(-std::log10(static_cast<double>(range.interval))));
        tidied = roundToDecimals(tidied, juce::jmax(0, decimals));
    }
    else if (tidied != 0.0)
    {
        // Six significant digits, expressed as a decimal place count.
        const auto magnitude = static_cast<int>(std::floor(std::log10(std::abs(tidied))));
        tidied = roundToDecimals(tidied, 5 - magnitude);
    }

    return juce::jlimit(static_cast<double>(range.start), static_cast<double>(range.end), tidied);
}

/** The APVTS state, with every value tidied and every value the preset asked
    for restored verbatim. Operates on the copy, so no listener writes the
    round-tripped numbers back. */
juce::ValueTree tidiedState(juce::AudioProcessorValueTreeState& apvts, const ExactValues& exact)
{
    auto state = apvts.copyState();

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto child = state.getChild(i);
        const auto id = child.getProperty("id").toString();
        auto* param = apvts.getParameter(id);

        if (param == nullptr)
            continue;

        const auto& range = param->getNormalisableRange();
        const auto found = exact.find(id.toStdString());
        const double raw = found != exact.end() ? found->second : static_cast<double>(child.getProperty("value"));

        child.setProperty("value", tidyValue(raw, range), nullptr);
    }

    return state;
}

/** The style choice INDEX for a manifest style name, looked up in the real
    choice list rather than counted by hand. */
int styleIndexForName(const juce::String& presetName, const juce::String& styleName)
{
    static const juce::StringArray choices = pid::styleChoices();
    const int index = choices.indexOf(styleName);

    if (index < 0)
        fail(presetName + ": unknown style '" + styleName + "' (choices: " + choices.joinIntoString(", ") + ")");

    return juce::jmax(0, index);
}

// ------------------------------------------------------- modulation target map
/** Parameter id -> modulation target index, built exactly the way
    `EmberAudioProcessor::buildModulationTargetTable()` builds it: walk
    `getParameters()` in order and keep the modulatable ones; the target index
    is the position within that filtered list. */
std::map<std::string, int> buildModulationTargetIndices(const juce::AudioProcessor& processor)
{
    std::map<std::string, int> indices;
    int next = 0;

    for (auto* param : processor.getParameters())
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            if (pid::isModulatable(withID->paramID))
                indices[withID->paramID.toStdString()] = next++;

    return indices;
}

// ------------------------------------------------------------------- emission
/** Sets the whole preset on a fresh APVTS and returns the finished document. */
juce::String renderPreset(const Preset& preset, const std::map<std::string, int>& targetIndices)
{
    LayoutHost host;
    auto& apvts = host.apvts;
    const auto& name = preset.name;
    ExactValues exact;

    const auto set = [&apvts, &exact, &name](const juce::String& id, float value)
    { setParam(apvts, exact, name, id, value); };

    // ------------------------------------------------------------- global
    set(pid::inputGain, preset.inputGain);
    set(pid::outputGain, preset.outputGain);
    set(pid::globalMix, preset.globalMix);
    set(pid::autoGain, preset.autoGain ? 1.0f : 0.0f);
    set(pid::numBands, static_cast<float>(preset.numBands()));

    set(pid::osFactor, static_cast<float>(static_cast<int>(preset.os)));
    set(pid::osOffline, static_cast<float>(static_cast<int>(
                            preset.osOffline == OversamplingFactor::Count ? preset.os : preset.osOffline)));

    set(pid::xoverMode, static_cast<float>(static_cast<int>(preset.xoverMode)));
    set(pid::stereoMode, static_cast<float>(static_cast<int>(preset.stereo)));
    set(pid::ditherMode, static_cast<float>(static_cast<int>(preset.dither)));

    // --------------------------------------------------------- crossovers
    if (static_cast<int>(preset.crossovers.size()) != preset.numBands() - 1)
        fail(name + ": " + juce::String(preset.numBands()) + " bands need " + juce::String(preset.numBands() - 1) +
             " crossovers but the table gives " + juce::String(static_cast<int>(preset.crossovers.size())));

    const auto edges = resolveCrossovers(preset.crossovers);
    validateCrossovers(preset, edges);

    for (int i = 0; i < kMaxCrossovers; ++i)
        set(pid::crossover(i), edges[static_cast<size_t>(i)]);

    // -------------------------------------------------------------- bands
    // Bands the preset does not use keep the layout's defaults, which are
    // neutral (drive 0 dB, tone flat, no feedback): raising the band count
    // after loading opens a clean band rather than a leftover setting.
    for (int band = 0; band < preset.numBands(); ++band)
    {
        const auto& b = preset.bands[static_cast<size_t>(band)];

        set(pid::style(band), static_cast<float>(styleIndexForName(name, b.style)));
        set(pid::drive(band), b.drive);
        set(pid::bandMix(band), b.mix);
        set(pid::level(band), b.level);
        set(pid::pan(band), b.pan);
        set(pid::width(band), b.width);
        set(pid::feedback(band), b.feedback);

        // "—" in the FB (Hz) column: the control is inert, so write the
        // parameter's own default rather than an invented number.
        set(pid::feedbackFreq(band),
            b.feedbackFreq == kUseDefault ? defaultValueOf(apvts, pid::feedbackFreq(band)) : b.feedbackFreq);

        set(pid::dynamics(band), b.dynamics);
        set(pid::toneLow(band), b.low);
        set(pid::toneMid(band), b.mid);
        set(pid::toneHigh(band), b.high);
    }

    // ------------------------------------------------------ source setup
    for (const auto& extra : preset.extras)
        set(extra.id, extra.value);

    // ----------------------------------------------------------- document
    juce::XmlElement root("EMBER_PRESET");
    root.setAttribute("name", preset.name);
    root.setAttribute("category", preset.category);
    root.setAttribute("pluginVersion", EMBER_VERSION_STRING);

    // The APVTS state tree type is the source of truth for the parameter block
    // name, so it can never drift from what PresetManager looks for.
    if (auto stateXml = tidiedState(apvts, exact).createXml())
        root.addChildElement(stateXml.release());
    else
        fail(name + ": the APVTS state produced no XML");

    if (!preset.mods.empty())
    {
        if (static_cast<int>(preset.mods.size()) > kMaxModConnections)
            fail(name + ": more connections than the engine has slots");

        auto* modNode = root.createNewChildElement(ModStateIds::modulation.toString());

        for (const auto& m : preset.mods)
        {
            const int source = flatSourceIndex(m.sourceType, m.sourceOrdinal);

            if (source < 0 || source >= kNumModSources)
            {
                fail(name + ": modulation source ordinal " + juce::String(m.sourceOrdinal) +
                     " does not exist for its type");
                continue;
            }

            const auto found = targetIndices.find(m.targetId.toStdString());

            if (found == targetIndices.end())
            {
                fail(name + ": '" + m.targetId + "' is not a modulatable parameter");
                continue;
            }

            const double amount = static_cast<double>(m.amountPercent) / 100.0;

            if (amount < -1.0 || amount > 1.0)
            {
                fail(name + ": modulation amount " + juce::String(m.amountPercent, 1) + " % is outside +/-100 %");
                continue;
            }

            auto* connection = modNode->createNewChildElement(ModStateIds::connection.toString());
            connection->setAttribute(ModStateIds::source, source);
            connection->setAttribute(ModStateIds::target, found->second);
            connection->setAttribute(ModStateIds::amount, amount);
            connection->setAttribute(ModStateIds::curve, static_cast<int>(m.curve));
            connection->setAttribute(ModStateIds::smoothing, static_cast<double>(m.smoothingMs));
            connection->setAttribute(ModStateIds::enabled, 1);
        }
    }

    juce::XmlElement::TextFormat format;
    format.newLineChars = "\n";
    format.lineWrapLength = 200; // one element per line, as the spec shows

    return root.toString(format);
}

/** Every parameter at its default, so the browser has a clean starting point. */
juce::String renderInitPreset()
{
    LayoutHost host;

    juce::XmlElement root("EMBER_PRESET");
    root.setAttribute("name", "Init");
    root.setAttribute("category", "Factory");
    root.setAttribute("pluginVersion", EMBER_VERSION_STRING);

    if (auto stateXml = tidiedState(host.apvts, {}).createXml())
        root.addChildElement(stateXml.release());
    else
        fail("Init: the APVTS state produced no XML");

    juce::XmlElement::TextFormat format;
    format.newLineChars = "\n";
    format.lineWrapLength = 200;

    return root.toString(format);
}

// ------------------------------------------------------------ output location
juce::File resolveOutputDirectory(int argc, char** argv)
{
    if (argc > 1)
        return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[1]));

    // Walk up from the working directory, then from the executable, looking for
    // the manifest these presets are transcribed from.
    const juce::File starts[] = {
        juce::File::getCurrentWorkingDirectory(),
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()};

    for (const auto& start : starts)
    {
        juce::File dir = start;

        for (int depth = 0; depth < 10; ++depth)
        {
            const auto candidate = dir.getChildFile("resources").getChildFile("presets");

            if (candidate.getChildFile("MANIFEST.md").existsAsFile())
                return candidate;

            const auto parent = dir.getParentDirectory();

            if (parent == dir)
                break;

            dir = parent;
        }
    }

    return {};
}
} // namespace

int main(int argc, char** argv)
{
    const auto outputDir = resolveOutputDirectory(argc, argv);

    if (outputDir == juce::File())
    {
        std::fprintf(stderr, "make_presets: could not find resources/presets "
                             "(pass the output directory as the first argument)\n");
        return 1;
    }

    LayoutHost probe;
    const auto targetIndices = buildModulationTargetIndices(probe);

    if (targetIndices.empty())
    {
        std::fprintf(stderr, "make_presets: the layout has no modulatable parameters\n");
        return 1;
    }

    const auto bank = buildPresetBank();

    // Render and validate everything BEFORE touching the disk, so a bad
    // transcription cannot leave a half-updated library behind.
    std::vector<std::pair<juce::String, juce::String>> documents; // file name, xml
    juce::StringArray names, fileNames, categories;

    documents.emplace_back("Init.xml", renderInitPreset());
    names.add("Init");
    fileNames.add("Init.xml");
    categories.add("Factory");

    for (const auto& preset : bank)
    {
        if (names.contains(preset.name))
            fail("duplicate preset name: " + preset.name);

        if (fileNames.contains(preset.fileName))
            fail("duplicate preset file name: " + preset.fileName);

        names.add(preset.name);
        fileNames.add(preset.fileName);
        categories.addIfNotAlreadyThere(preset.category);

        documents.emplace_back(preset.fileName, renderPreset(preset, targetIndices));
    }

    if (errorCount > 0)
    {
        std::fprintf(stderr, "make_presets: %d error(s); nothing written\n", errorCount);
        return 1;
    }

    if (!outputDir.createDirectory())
    {
        std::fprintf(stderr, "make_presets: cannot create %s\n", outputDir.getFullPathName().toRawUTF8());
        return 1;
    }

    for (const auto& doc : documents)
    {
        const auto file = outputDir.getChildFile(doc.first);

        if (!file.replaceWithText(doc.second))
        {
            std::fprintf(stderr, "make_presets: cannot write %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }
    }

    std::printf("make_presets: wrote %d presets in %d categories to %s\n", static_cast<int>(documents.size()),
                categories.size(), outputDir.getFullPathName().toRawUTF8());

    return 0;
}
