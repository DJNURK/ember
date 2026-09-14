#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "plugin/ParameterIDs.h"
#include "dsp/DspUtils.h"

namespace ember
{
namespace
{
/** Read a parameter's current value with modulation applied.

    Modulation offsets are deltas in NORMALISED units, so the offset is added in
    the normalised domain and converted back through the parameter's own range.
    That keeps a modulation amount meaning the same fraction of travel on a
    logarithmic frequency control as on a linear gain control. */
inline float resolveValue(const juce::RangedAudioParameter* p, float modOffset) noexcept
{
    const auto& range = p->getNormalisableRange();
    if (modOffset == 0.0f)
        return range.convertFrom0to1(p->getValue());
    const float n = juce::jlimit(0.0f, 1.0f, p->getValue() + modOffset);
    return range.convertFrom0to1(n);
}

inline int choiceIndex(const juce::RangedAudioParameter* p) noexcept
{
    if (auto* c = dynamic_cast<const juce::AudioParameterChoice*>(p))
        return c->getIndex();
    return static_cast<int>(std::lround(p->getNormalisableRange().convertFrom0to1(p->getValue())));
}

} // namespace

EmberAudioProcessor::EmberAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, &undoManager, "EMBER", pid::createParameterLayout())
{
    buildModulationTargetTable();

    for (auto& cc : midiCCForParam)
        cc.store(-1, std::memory_order_relaxed);

    presetManager = std::make_unique<PresetManager>(
        apvts, [this] { return modulation.toValueTree(); },
        [this](const juce::ValueTree& t) { modulation.fromValueTree(t); });

    // Any parameter change marks the preset dirty so the GUI can show it.
    for (auto* p : getParameters())
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            apvts.addParameterListener(withID->paramID, this);
}

EmberAudioProcessor::~EmberAudioProcessor()
{
    for (auto* p : getParameters())
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            apvts.removeParameterListener(withID->paramID, this);
}

void EmberAudioProcessor::buildModulationTargetTable()
{
    modTargetIDs.clear();
    modTargetValues.clear();
    modTargetIndexByID.clear();

    for (auto* p : getParameters())
    {
        auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p);
        if (withID == nullptr || !pid::isModulatable(withID->paramID))
            continue;

        modTargetIndexByID.set(withID->paramID, modTargetIDs.size());
        modTargetIDs.add(withID->paramID);
    }
}

int EmberAudioProcessor::getModulationTargetIndex(const juce::String& parameterID) const
{
    return modTargetIndexByID.contains(parameterID) ? modTargetIndexByID[parameterID] : -1;
}

juce::String EmberAudioProcessor::getModulationTargetID(int index) const
{
    return juce::isPositiveAndBelow(index, modTargetIDs.size()) ? modTargetIDs[index] : juce::String();
}

float EmberAudioProcessor::getModulationDepth(const juce::String& parameterID) const
{
    const int idx = getModulationTargetIndex(parameterID);
    return idx < 0 ? 0.0f : modulation.getModulationOffset(idx);
}

void EmberAudioProcessor::parameterChanged(const juce::String&, float)
{
    if (presetManager != nullptr)
        presetManager->markModified();
}

void EmberAudioProcessor::setSelectedBand(int band) noexcept
{
    selectedBand.store(juce::jlimit(0, kMaxBands - 1, band), std::memory_order_relaxed);
}

void EmberAudioProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    lastSampleRate = sampleRate;
    lastBlockSize = juce::jmax(1, maximumExpectedSamplesPerBlock);

    juce::dsp::ProcessSpec spec{};
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(lastBlockSize);
    spec.numChannels = static_cast<juce::uint32>(juce::jmax(1, getTotalNumOutputChannels()));

    // Offline rendering gets its own oversampling factor AND the linear-phase
    // FIR filters: when nobody is waiting on latency or CPU, there is no reason
    // not to take the better-sounding path. Realtime uses the cheaper
    // minimum-phase IIR. This is the specification's "separate offline-render
    // setting", applied to quality as well as to factor.
    const bool offline = isNonRealtime();
    const auto factor = static_cast<OversamplingFactor>(
        juce::jlimit(0, static_cast<int>(OversamplingFactor::Count) - 1,
                     choiceIndex(apvts.getParameter(offline ? pid::osOffline : pid::osFactor))));
    globalParams.oversampling = factor;

    engine.setOversamplingQuality(offline);
    engine.setOversamplingFactor(factor);
    engine.prepare(spec);
    engine.setCrossoverMode(static_cast<CrossoverMode>(
        juce::jlimit(0, static_cast<int>(CrossoverMode::Count) - 1, choiceIndex(apvts.getParameter(pid::xoverMode)))));

    modulation.prepare(sampleRate, lastBlockSize, modTargetIDs.size());
    modulation.reset();

    setLatencySamples(engine.getLatencySamples());
    bandRms.fill(0.0f);
    controlCounter = 0;
}

void EmberAudioProcessor::releaseResources() {}

bool EmberAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

double EmberAudioProcessor::getTailLengthSeconds() const
{
    // The resonant feedback path can ring for a while after the input stops.
    return 2.0;
}

void EmberAudioProcessor::resolveParameters(int numSamples) noexcept
{
    juce::ignoreUnused(numSamples);

    auto mod = [this](const juce::String& id) noexcept
    {
        const int idx = modTargetIndexByID.contains(id) ? modTargetIndexByID[id] : -1;
        return idx < 0 ? 0.0f : modulation.getModulationOffset(idx);
    };
    auto value = [this, &mod](const juce::String& id) noexcept
    {
        auto* p = apvts.getParameter(id);
        return p == nullptr ? 0.0f : resolveValue(p, mod(id));
    };
    auto raw = [this](const juce::String& id) noexcept
    {
        auto* p = apvts.getParameter(id);
        return p == nullptr ? 0.0f : p->getValue();
    };

    globalParams.inputGainDb = value(pid::inputGain);
    globalParams.outputGainDb = value(pid::outputGain);
    globalParams.mix01 = value(pid::globalMix) * 0.01f;
    globalParams.autoGain = raw(pid::autoGain) >= 0.5f;
    globalParams.numBands = juce::jlimit(kMinBands, kMaxBands, static_cast<int>(std::lround(value(pid::numBands))));
    globalParams.stereoMode =
        static_cast<StereoMode>(juce::jlimit(0, 1, choiceIndex(apvts.getParameter(pid::stereoMode))));

    for (int i = 0; i < kMaxCrossovers; ++i)
        globalParams.crossoverHz[i] = value(pid::crossover(i));

    // Keep the edges ascending and at least a third of an octave apart, even if
    // modulation pushes two of them together: unordered edges would produce
    // unstable crossover coefficients.
    constexpr float kMinRatio = 1.26f; // one third of an octave
    for (int i = 1; i < kMaxCrossovers; ++i)
        globalParams.crossoverHz[i] =
            juce::jmax(globalParams.crossoverHz[i], globalParams.crossoverHz[i - 1] * kMinRatio);

    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& p = bandParams[static_cast<size_t>(b)];
        p.driveDb = value(pid::drive(b));
        p.mix01 = value(pid::bandMix(b)) * 0.01f;
        p.levelDb = value(pid::level(b));
        p.pan = value(pid::pan(b)) * 0.01f;
        p.width01 = value(pid::width(b)) * 0.01f;
        p.style = static_cast<StyleID>(juce::jlimit(0, kNumStyles - 1, choiceIndex(apvts.getParameter(pid::style(b)))));
        p.feedback01 = value(pid::feedback(b)) * 0.01f;
        p.feedbackFreq = value(pid::feedbackFreq(b));
        p.dynamics = value(pid::dynamics(b)) * 0.01f;
        p.toneLowDb = value(pid::toneLow(b));
        p.toneMidDb = value(pid::toneMid(b));
        p.toneHighDb = value(pid::toneHigh(b));
        p.bypass = raw(pid::bypass(b)) >= 0.5f;
        p.solo = raw(pid::solo(b)) >= 0.5f;
    }
}

void EmberAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();
    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // Transport, for tempo-synced modulation.
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            modulation.setTransport(pos->getBpm().orFallback(120.0), pos->getPpqPosition().orFallback(0.0),
                                    pos->getIsPlaying());
        }
    }

    modulation.processMidi(midi);
    applyMidiMappings(midi);

    // Process in control-rate chunks so modulation actually moves within a
    // block. The band chains ramp their own smoothed parameters across each
    // chunk, which is the per-sample interpolation the specification asks for.
    int offset = 0;
    while (offset < numSamples)
    {
        const int chunk = juce::jmin(kControlBlockSize, numSamples - offset);

        modulation.updateControlBlock(bandRms.data(), globalParams.numBands, chunk);
        resolveParameters(chunk);
        engine.setParameters(globalParams, bandParams.data(), kMaxBands);

        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), totalOut, offset, chunk);
        engine.process(view);

        for (int b = 0; b < kMaxBands; ++b)
            bandRms[static_cast<size_t>(b)] = engine.getBandLevel(b);

        offset += chunk;
    }

    // Final safety net: a non-finite sample reaching the host is far worse than
    // a moment of silence, and some hosts will disable a plugin that emits one.
    for (int ch = 0; ch < totalOut; ++ch)
    {
        auto* d = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            d[i] = dsputil::sanitise(d[i]);
    }
}

void EmberAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // supportsDoublePrecisionProcessing() is false, so hosts never call this.
    // It exists only because the base class declares the overload.
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());
}

// ---------------------------------------------------------------- MIDI learn
void EmberAudioProcessor::beginMidiLearn(const juce::String& parameterID)
{
    const juce::ScopedLock sl(midiLearnLock);
    midiLearnParameterID = parameterID;
    midiLearnActive.store(true, std::memory_order_release);
}

void EmberAudioProcessor::cancelMidiLearn()
{
    const juce::ScopedLock sl(midiLearnLock);
    midiLearnParameterID.clear();
    midiLearnActive.store(false, std::memory_order_release);
}

juce::String EmberAudioProcessor::getMidiLearnParameterID() const
{
    const juce::ScopedLock sl(midiLearnLock);
    return midiLearnParameterID;
}

int EmberAudioProcessor::getMidiCCForParameter(const juce::String& parameterID) const
{
    int index = 0;
    for (auto* p : getParameters())
    {
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            if (withID->paramID == parameterID)
                return midiCCForParam[static_cast<size_t>(index)].load(std::memory_order_relaxed);
        ++index;
    }
    return -1;
}

void EmberAudioProcessor::clearMidiMapping(const juce::String& parameterID)
{
    int index = 0;
    for (auto* p : getParameters())
    {
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            if (withID->paramID == parameterID)
            {
                midiCCForParam[static_cast<size_t>(index)].store(-1, std::memory_order_relaxed);
                return;
            }
        ++index;
    }
}

void EmberAudioProcessor::clearAllMidiMappings()
{
    for (auto& cc : midiCCForParam)
        cc.store(-1, std::memory_order_relaxed);
}

void EmberAudioProcessor::applyMidiMappings(const juce::MidiBuffer& midi)
{
    if (midi.isEmpty())
        return;

    const auto& params = getParameters();

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (!msg.isController())
            continue;

        const int cc = msg.getControllerNumber();
        const float normalised = static_cast<float>(msg.getControllerValue()) / 127.0f;

        // Completing a learn assignment: bind this CC to the armed parameter.
        if (midiLearnActive.load(std::memory_order_acquire))
        {
            juce::String target;
            {
                const juce::ScopedTryLock stl(midiLearnLock);
                if (stl.isLocked())
                    target = midiLearnParameterID;
            }
            if (target.isNotEmpty())
            {
                for (int i = 0; i < params.size(); ++i)
                    if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(params[i]))
                        if (withID->paramID == target)
                            midiCCForParam[static_cast<size_t>(i)].store(cc, std::memory_order_relaxed);
                midiLearnActive.store(false, std::memory_order_release);
            }
        }

        for (int i = 0; i < params.size() && i < static_cast<int>(midiCCForParam.size()); ++i)
        {
            if (midiCCForParam[static_cast<size_t>(i)].load(std::memory_order_relaxed) != cc)
                continue;
            // setValueNotifyingHost is not realtime-safe in general, but this is
            // the standard JUCE route for MIDI-driven parameter changes and is
            // bounded by the number of mapped parameters, which is small.
            params[i]->setValueNotifyingHost(normalised);
        }
    }
}

// ---------------------------------------------------------------- A/B compare
void EmberAudioProcessor::storeToSlot(int slot)
{
    (slot == 0 ? slotA : slotB) = captureFullState();
}

void EmberAudioProcessor::recallSlot(int slot)
{
    const auto& tree = (slot == 0 ? slotA : slotB);
    if (tree.isValid())
        restoreFullState(tree);
}

void EmberAudioProcessor::setActiveSlot(int slot)
{
    slot = slot == 0 ? 0 : 1;
    if (slot == activeSlot)
        return;
    storeToSlot(activeSlot); // remember where we were
    activeSlot = slot;
    recallSlot(activeSlot);
}

void EmberAudioProcessor::copyCurrentSlotToOther()
{
    const auto current = captureFullState();
    (activeSlot == 0 ? slotB : slotA) = current;
}

// ---------------------------------------------------------------- editor size
void EmberAudioProcessor::setEditorBounds(int width, int height)
{
    editorWidth.store(juce::jlimit(800, 3000, width), std::memory_order_relaxed);
    editorHeight.store(juce::jlimit(480, 2000, height), std::memory_order_relaxed);
}

juce::Rectangle<int> EmberAudioProcessor::getEditorBounds() const
{
    return {editorWidth.load(std::memory_order_relaxed), editorHeight.load(std::memory_order_relaxed)};
}

// ---------------------------------------------------------------- programs
int EmberAudioProcessor::getNumPrograms()
{
    return juce::jmax(1, presetManager != nullptr ? presetManager->getAllPresets().size() : 1);
}

int EmberAudioProcessor::getCurrentProgram()
{
    return 0;
}

void EmberAudioProcessor::setCurrentProgram(int index)
{
    if (presetManager == nullptr)
        return;
    const auto& all = presetManager->getAllPresets();
    if (juce::isPositiveAndBelow(index, all.size()))
        presetManager->loadPreset(all.getReference(index));
}

const juce::String EmberAudioProcessor::getProgramName(int index)
{
    if (presetManager == nullptr)
        return "Default";
    const auto& all = presetManager->getAllPresets();
    return juce::isPositiveAndBelow(index, all.size()) ? all.getReference(index).name : juce::String("Default");
}

void EmberAudioProcessor::changeProgramName(int, const juce::String&) {}

// ---------------------------------------------------------------- state
juce::ValueTree EmberAudioProcessor::captureFullState() const
{
    // apvts.copyState() is non-const (it flushes pending parameter values into
    // the tree first), so take the const_cast here rather than dropping const
    // from every caller.
    auto& mutableApvts = const_cast<juce::AudioProcessorValueTreeState&>(apvts);

    juce::ValueTree tree("EMBER_STATE");
    tree.setProperty("version", EMBER_VERSION_STRING, nullptr);
    tree.setProperty("selectedBand", selectedBand.load(std::memory_order_relaxed), nullptr);
    tree.setProperty("editorWidth", editorWidth.load(std::memory_order_relaxed), nullptr);
    tree.setProperty("editorHeight", editorHeight.load(std::memory_order_relaxed), nullptr);

    tree.addChild(mutableApvts.copyState().createCopy(), -1, nullptr);

    auto mod = modulation.toValueTree();
    if (mod.isValid())
        tree.addChild(mod.createCopy(), -1, nullptr);

    // MIDI mappings, stored by parameter ID so they survive a parameter being
    // added or reordered in a later version.
    juce::ValueTree midiTree("MIDI_MAP");
    int index = 0;
    for (auto* p : getParameters())
    {
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
        {
            const int cc = midiCCForParam[static_cast<size_t>(index)].load(std::memory_order_relaxed);
            if (cc >= 0)
            {
                juce::ValueTree entry("MAP");
                entry.setProperty("param", withID->paramID, nullptr);
                entry.setProperty("cc", cc, nullptr);
                midiTree.addChild(entry, -1, nullptr);
            }
        }
        ++index;
    }
    tree.addChild(midiTree, -1, nullptr);
    return tree;
}

void EmberAudioProcessor::restoreFullState(const juce::ValueTree& tree)
{
    if (!tree.isValid())
        return;

    if (auto params = tree.getChildWithName(apvts.state.getType()); params.isValid())
        apvts.replaceState(params.createCopy());

    modulation.fromValueTree(tree.getChildWithName(ModStateIds::modulation));

    clearAllMidiMappings();
    if (auto midiTree = tree.getChildWithName("MIDI_MAP"); midiTree.isValid())
    {
        for (int i = 0; i < midiTree.getNumChildren(); ++i)
        {
            const auto entry = midiTree.getChild(i);
            const juce::String paramID = entry.getProperty("param").toString();
            const int cc = static_cast<int>(entry.getProperty("cc", -1));
            if (paramID.isNotEmpty() && cc >= 0)
            {
                int index = 0;
                for (auto* p : getParameters())
                {
                    if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
                        if (withID->paramID == paramID)
                            midiCCForParam[static_cast<size_t>(index)].store(cc, std::memory_order_relaxed);
                    ++index;
                }
            }
        }
    }

    selectedBand.store(juce::jlimit(0, kMaxBands - 1, static_cast<int>(tree.getProperty("selectedBand", 0))),
                       std::memory_order_relaxed);
    editorWidth.store(juce::jlimit(800, 3000, static_cast<int>(tree.getProperty("editorWidth", 1100))),
                      std::memory_order_relaxed);
    editorHeight.store(juce::jlimit(480, 2000, static_cast<int>(tree.getProperty("editorHeight", 640))),
                       std::memory_order_relaxed);
}

void EmberAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = captureFullState().createXml())
        copyXmlToBinary(*xml, destData);
}

void EmberAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        restoreFullState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* EmberAudioProcessor::createEditor()
{
    return new EmberAudioProcessorEditor(*this);
}
} // namespace ember

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ember::EmberAudioProcessor();
}
