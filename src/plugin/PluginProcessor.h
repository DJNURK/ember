#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "dsp/EmberEngine.h"
#include "dsp/modulation/ModulationEngine.h"
#include "plugin/PresetManager.h"

namespace ember
{
/**
    The plugin wrapper: parameters, state, presets, MIDI learn, A/B compare and
    undo, plus the thin layer that resolves APVTS values + modulation into the
    plain parameter structs `EmberEngine` consumes.

    Threading:
      - `processBlock` owns the engine and the modulation engine's evaluation.
      - The GUI reads metering through atomics and spectrum frames through a
        wait-free FIFO; it never takes a lock the audio thread can contend on.
      - Preset, undo and modulation-graph EDITING happen on the message thread;
        the modulation engine hands the new graph to the audio thread without
        blocking it.
*/
class EmberAudioProcessor : public juce::AudioProcessor, private juce::AudioProcessorValueTreeState::Listener
{
public:
    EmberAudioProcessor();
    ~EmberAudioProcessor() override;

    // ---------------------------------------------------------- AudioProcessor
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Ember"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;
    bool supportsDoublePrecisionProcessing() const override { return false; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---------------------------------------------------------------- for the GUI
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    PresetManager& getPresetManager() noexcept { return *presetManager; }
    ModulationEngine& getModulationEngine() noexcept { return modulation; }
    SpectrumFifo& getSpectrumFifo() noexcept { return engine.getSpectrumFifo(); }

    /** The editor turns spectrum analysis on while it is open and off when it
        closes, so instances with no window pay nothing for FFTs. */
    void setSpectrumAnalysisEnabled(bool shouldAnalyse) noexcept { engine.setSpectrumEnabled(shouldAnalyse); }

    /** Peak output level of a band, 0..1, for the band "heat" overlay. */
    float getBandLevel(int band) const noexcept { return engine.getBandLevel(band); }
    float getBandGainReductionDb(int band) const noexcept { return engine.getBandGainReductionDb(band); }

    /** Which band the GUI has selected. Persisted with the plugin state. */
    int getSelectedBand() const noexcept { return selectedBand.load(std::memory_order_relaxed); }
    void setSelectedBand(int band) noexcept;

    // ---------------------------------------------------------------- modulation
    /** Modulation destinations are addressed by index on the audio thread. These
        map between the stable parameter IDs the GUI knows and those indices. */
    int getNumModulationTargets() const noexcept { return modTargetIDs.size(); }
    int getModulationTargetIndex(const juce::String& parameterID) const;
    juce::String getModulationTargetID(int index) const;

    /** Total modulation depth currently applied to a parameter, in normalised
        units, so a knob can draw its modulation range ring. */
    float getModulationDepth(const juce::String& parameterID) const;

    // ---------------------------------------------------------------- MIDI learn
    void beginMidiLearn(const juce::String& parameterID);
    void cancelMidiLearn();
    bool isMidiLearning() const noexcept { return midiLearnActive.load(std::memory_order_relaxed); }
    juce::String getMidiLearnParameterID() const;
    /** -1 when the parameter has no mapping. */
    int getMidiCCForParameter(const juce::String& parameterID) const;
    void clearMidiMapping(const juce::String& parameterID);
    void clearAllMidiMappings();

    // ---------------------------------------------------------------- A/B compare
    void storeToSlot(int slot);
    void recallSlot(int slot);
    void copyCurrentSlotToOther();
    int getActiveSlot() const noexcept { return activeSlot; }
    void setActiveSlot(int slot);

    // ---------------------------------------------------------------- editor size
    void setEditorBounds(int width, int height);
    juce::Rectangle<int> getEditorBounds() const;

private:
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void buildModulationTargetTable();
    void registerSourceParameterOwnership();
    void buildParameterCache();
    void pushSourceParameters() noexcept;

    /** One parameter, resolved once at construction.

        `resolveParameters` and `pushSourceParameters` run once per 32-sample
        control block — about 1500 times a second — and touch well over a
        hundred parameters each time. Looking those up by ID on the audio thread
        means a `juce::String` copy and a hash per parameter per block. It is not
        an allocation, but it measured at 1.8 percentage points of a core on top
        of a 7.5 % engine, so the pointers and modulation indices are resolved
        once here and the audio thread only ever dereferences. */
    struct CachedParam
    {
        juce::RangedAudioParameter* param{nullptr};
        int modIndex{-1};

        float raw() const noexcept
        {
            return param == nullptr ? 0.0f : param->getNormalisableRange().convertFrom0to1(param->getValue());
        }
        float normalised() const noexcept { return param == nullptr ? 0.0f : param->getValue(); }
        bool isOn() const noexcept { return param != nullptr && param->getValue() >= 0.5f; }
    };

    struct BandCache
    {
        CachedParam drive, mix, level, pan, width, style, feedback, feedbackFreq, dynamics;
        CachedParam toneLow, toneMid, toneHigh, bypass, solo;
    };
    struct LfoCache
    {
        CachedParam rate, sync, phase, smooth, steps, depth;
    };
    struct EgCache
    {
        CachedParam attack, decay, sustain, release, threshold, trigger;
    };
    struct EfCache
    {
        CachedParam attack, release, band, gain;
    };
    struct MidiCache
    {
        CachedParam type, cc, smooth;
    };

    std::array<BandCache, kMaxBands> bandCache{};
    std::array<CachedParam, kMaxCrossovers> crossoverCache{};
    CachedParam inGainCache, outGainCache, globalMixCache, autoGainCache, numBandsCache, stereoModeCache;
    std::array<LfoCache, kNumXLFOs> lfoCache{};
    std::array<EgCache, kNumEnvGenerators> egCache{};
    std::array<EfCache, kNumEnvFollowers> efCache{};
    CachedParam xyXCache, xyYCache;
    std::array<MidiCache, kNumMidiSources> midiCache{};
    std::array<CachedParam, kNumMacros> macroCache{};
    void resolveParameters(int numSamples) noexcept;
    void applyMidiMappings(const juce::MidiBuffer& midi);
    juce::ValueTree captureFullState() const;
    void restoreFullState(const juce::ValueTree& tree);

    juce::AudioProcessorValueTreeState apvts;
    juce::UndoManager undoManager{30000, 100};
    EmberEngine engine;
    ModulationEngine modulation;
    std::unique_ptr<PresetManager> presetManager;

    /** Cached raw parameter pointers so `processBlock` never does a string
        lookup. Index matches `modTargetIDs` for modulatable parameters. */
    juce::StringArray modTargetIDs;
    std::vector<std::atomic<float>*> modTargetValues;
    juce::HashMap<juce::String, int> modTargetIndexByID;

    std::array<BandParams, kMaxBands> bandParams;
    GlobalParams globalParams;

    // MIDI learn: parameter index -> CC number, -1 for unmapped.
    std::array<std::atomic<int>, 512> midiCCForParam{};
    std::atomic<bool> midiLearnActive{false};
    juce::String midiLearnParameterID;
    juce::CriticalSection midiLearnLock;

    juce::ValueTree slotA, slotB;
    int activeSlot{0};

    std::atomic<int> selectedBand{0};
    std::atomic<int> editorWidth{1100}, editorHeight{640};

    int controlCounter{0};
    std::array<float, kMaxBands> bandRms{};

    double lastSampleRate{44100.0};
    int lastBlockSize{512};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberAudioProcessor)
};
} // namespace ember
