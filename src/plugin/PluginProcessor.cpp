#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "plugin/ParameterIDs.h"
#include "dsp/DspUtils.h"

namespace ember
{
namespace
{
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

    registerSourceParameterOwnership();
    buildParameterCache();
}

void EmberAudioProcessor::buildParameterCache()
{
    auto cache = [this](const juce::String& id)
    {
        CachedParam c;
        c.param = apvts.getParameter(id);
        c.modIndex = getModulationTargetIndex(id);
        jassert(c.param != nullptr); // a typo'd id would silently do nothing
        return c;
    };

    inGainCache = cache(pid::inputGain);
    outGainCache = cache(pid::outputGain);
    globalMixCache = cache(pid::globalMix);
    autoGainCache = cache(pid::autoGain);
    numBandsCache = cache(pid::numBands);
    stereoModeCache = cache(pid::stereoMode);

    for (int i = 0; i < kMaxCrossovers; ++i)
        crossoverCache[static_cast<size_t>(i)] = cache(pid::crossover(i));

    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& c = bandCache[static_cast<size_t>(b)];
        c.drive = cache(pid::drive(b));
        c.mix = cache(pid::bandMix(b));
        c.level = cache(pid::level(b));
        c.pan = cache(pid::pan(b));
        c.width = cache(pid::width(b));
        c.style = cache(pid::style(b));
        c.feedback = cache(pid::feedback(b));
        c.feedbackFreq = cache(pid::feedbackFreq(b));
        c.dynamics = cache(pid::dynamics(b));
        c.toneLow = cache(pid::toneLow(b));
        c.toneMid = cache(pid::toneMid(b));
        c.toneHigh = cache(pid::toneHigh(b));
        c.bypass = cache(pid::bypass(b));
        c.solo = cache(pid::solo(b));
    }

    for (int i = 0; i < kNumXLFOs; ++i)
    {
        auto& c = lfoCache[static_cast<size_t>(i)];
        c.rate = cache(pid::lfoRate(i));
        c.sync = cache(pid::lfoSync(i));
        c.phase = cache(pid::lfoPhase(i));
        c.smooth = cache(pid::lfoSmooth(i));
        c.steps = cache(pid::lfoSteps(i));
        c.depth = cache(pid::lfoDepth(i));
    }
    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        auto& c = egCache[static_cast<size_t>(i)];
        c.attack = cache(pid::egAttack(i));
        c.decay = cache(pid::egDecay(i));
        c.sustain = cache(pid::egSustain(i));
        c.release = cache(pid::egRelease(i));
        c.threshold = cache(pid::egThreshold(i));
        c.trigger = cache(pid::egTrigger(i));
    }
    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        auto& c = efCache[static_cast<size_t>(i)];
        c.attack = cache(pid::efAttack(i));
        c.release = cache(pid::efRelease(i));
        c.band = cache(pid::efBand(i));
        c.gain = cache(pid::efGain(i));
    }
    xyXCache = cache(pid::xyX);
    xyYCache = cache(pid::xyY);
    for (int i = 0; i < kNumMidiSources; ++i)
    {
        auto& c = midiCache[static_cast<size_t>(i)];
        c.type = cache(pid::midiType(i));
        c.cc = cache(pid::midiCC(i));
        c.smooth = cache(pid::midiSmooth(i));
    }
    for (int i = 0; i < kNumMacros; ++i)
        macroCache[static_cast<size_t>(i)] = cache(pid::macro(i));
}

void EmberAudioProcessor::registerSourceParameterOwnership()
{
    // A modulation source's own parameters can themselves be modulation
    // destinations. The engine applies that internally — it has to, because the
    // sources are evaluated in dependency order inside one control block — so it
    // needs to know which destination index corresponds to which (source, field)
    // pair. Registering it here is what makes "drag an LFO onto another LFO's
    // rate" work; without it the processor would push a base value that silently
    // overwrote the modulated one every block.
    modulation.clearTargetOwners();

    auto own = [this](const juce::String& id, ModSourceType type, int ordinal, ModSourceField field)
    {
        const int idx = getModulationTargetIndex(id);
        if (idx >= 0)
            modulation.setTargetOwner(idx, flatSourceIndex(type, ordinal), field);
    };

    for (int i = 0; i < kNumXLFOs; ++i)
    {
        own(pid::lfoRate(i), ModSourceType::XLFO, i, ModSourceField::Rate);
        own(pid::lfoPhase(i), ModSourceType::XLFO, i, ModSourceField::Phase);
        own(pid::lfoSmooth(i), ModSourceType::XLFO, i, ModSourceField::Smoothing);
        own(pid::lfoDepth(i), ModSourceType::XLFO, i, ModSourceField::Depth);
    }
    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        own(pid::egAttack(i), ModSourceType::EnvelopeGenerator, i, ModSourceField::Attack);
        own(pid::egDecay(i), ModSourceType::EnvelopeGenerator, i, ModSourceField::Decay);
        own(pid::egSustain(i), ModSourceType::EnvelopeGenerator, i, ModSourceField::Sustain);
        own(pid::egRelease(i), ModSourceType::EnvelopeGenerator, i, ModSourceField::Release);
        own(pid::egThreshold(i), ModSourceType::EnvelopeGenerator, i, ModSourceField::Threshold);
    }
    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        own(pid::efAttack(i), ModSourceType::EnvelopeFollower, i, ModSourceField::Attack);
        own(pid::efRelease(i), ModSourceType::EnvelopeFollower, i, ModSourceField::Release);
        own(pid::efGain(i), ModSourceType::EnvelopeFollower, i, ModSourceField::Depth);
    }
    own(pid::xyX, ModSourceType::XYController, 0, ModSourceField::Value);
    for (int i = 0; i < kNumMidiSources; ++i)
        own(pid::midiSmooth(i), ModSourceType::MidiSource, i, ModSourceField::Smoothing);
    for (int i = 0; i < kNumMacros; ++i)
        own(pid::macro(i), ModSourceType::Macro, i, ModSourceField::Value);
}

void EmberAudioProcessor::pushSourceParameters() noexcept
{
    // Base (un-modulated) values. Fields registered with setTargetOwner are
    // modulated by the engine itself, so applying modulation here too would
    // double it. Cached pointers only — this runs once per control block.
    for (int i = 0; i < kNumXLFOs; ++i)
    {
        const auto& c = lfoCache[static_cast<size_t>(i)];
        XLfoParams lp;
        lp.rateHz = juce::jlimit(0.01f, 40.0f, c.rate.raw());
        lp.tempoSync = c.sync.isOn();
        // There is no separate sync-division parameter, so a synced LFO carries
        // its division in the rate field at the 120 BPM reference, where a
        // quarter note is 2 Hz. The factory presets are written that way.
        lp.syncBeats = juce::jlimit(0.03125f, 64.0f, 2.0f / juce::jmax(0.01f, lp.rateHz));
        lp.phaseOffset = c.phase.raw() / 360.0f;
        lp.steps = static_cast<int>(std::lround(c.steps.raw()));
        lp.smoothingMs = juce::jlimit(0.0f, 500.0f, c.smooth.raw() * 5.0f);
        lp.depth = juce::jlimit(0.0f, 1.0f, c.depth.raw() * 0.01f);
        modulation.setXLfoParameters(i, lp);
    }

    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        const auto& c = egCache[static_cast<size_t>(i)];
        EnvelopeGeneratorParams ep;
        ep.attackMs = c.attack.raw();
        ep.decayMs = c.decay.raw();
        ep.sustain = juce::jlimit(0.0f, 1.0f, c.sustain.raw() * 0.01f);
        ep.releaseMs = c.release.raw();
        ep.threshold = juce::Decibels::decibelsToGain(c.threshold.raw());
        ep.detectorBand = -1;
        ep.trigger = static_cast<EgTriggerMode>(juce::jlimit(0, static_cast<int>(EgTriggerMode::Count) - 1,
                                                             static_cast<int>(std::lround(c.trigger.raw()))));
        modulation.setEnvelopeGeneratorParameters(i, ep);
    }

    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        const auto& c = efCache[static_cast<size_t>(i)];
        EnvelopeFollowerParams fp;
        fp.attackMs = c.attack.raw();
        fp.releaseMs = c.release.raw();
        // The parameter is 0 = Full Range, 1..6 = band; the engine wants -1 for
        // full range and a zero-based band index otherwise.
        fp.band = static_cast<int>(std::lround(c.band.raw())) - 1;
        fp.gainDb = c.gain.raw();
        modulation.setEnvelopeFollowerParameters(i, fp);
    }

    XyControllerParams xy;
    xy.x = juce::jlimit(0.0f, 1.0f, xyXCache.raw() * 0.01f);
    xy.y = juce::jlimit(0.0f, 1.0f, xyYCache.raw() * 0.01f);
    modulation.setXyParameters(xy);

    for (int i = 0; i < kNumMidiSources; ++i)
    {
        const auto& c = midiCache[static_cast<size_t>(i)];
        MidiSourceParams mp;
        mp.kind = static_cast<MidiSourceKind>(
            juce::jlimit(0, static_cast<int>(MidiSourceKind::Count) - 1, static_cast<int>(std::lround(c.type.raw()))));
        mp.ccNumber = juce::jlimit(0, 127, static_cast<int>(std::lround(c.cc.raw())));
        mp.smoothingMs = juce::jlimit(0.0f, 500.0f, c.smooth.raw());
        modulation.setMidiSourceParameters(i, mp);
    }

    for (int i = 0; i < kNumMacros; ++i)
    {
        MacroParams mp;
        mp.value = juce::jlimit(0.0f, 1.0f, macroCache[static_cast<size_t>(i)].raw() * 0.01f);
        modulation.setMacroParameters(i, mp);
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

    // Two independent hazards, both fixed by sizing the engine generously.
    //
    // 1. processBlock splits the host's buffer into control blocks of
    //    kControlBlockSize (32) samples. If the host prepares a SMALLER maximum
    //    than that - 16 is legal and real - every engine buffer is sized 16 and
    //    the first 32-sample chunk runs off the end of all of them.
    // 2. maximumExpectedSamplesPerBlock is a hint, not a contract. Hosts do hand
    //    over more than they promised, and validators deliberately do.
    //
    // So prepare for at least a full control block, and never let a chunk exceed
    // what was prepared.
    lastBlockSize = juce::jmax(kControlBlockSize, juce::jmax(1, maximumExpectedSamplesPerBlock));
    preparedBlockSize = lastBlockSize;

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

    // Everything here reads cached pointers: no id strings, no hashing, no
    // allocation. Modulation offsets are deltas in NORMALISED units, so they are
    // added in the normalised domain and converted back through the parameter's
    // own range — that keeps a given modulation amount meaning the same
    // fraction of travel on a logarithmic frequency control as on a linear one.
    auto value = [this](const CachedParam& c) noexcept
    {
        if (c.param == nullptr)
            return 0.0f;
        const auto& range = c.param->getNormalisableRange();
        if (c.modIndex < 0)
            return range.convertFrom0to1(c.param->getValue());
        const float offset = modulation.getModulationOffset(c.modIndex);
        if (offset == 0.0f)
            return range.convertFrom0to1(c.param->getValue());
        return range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, c.param->getValue() + offset));
    };

    globalParams.inputGainDb = value(inGainCache);
    globalParams.outputGainDb = value(outGainCache);
    globalParams.mix01 = value(globalMixCache) * 0.01f;
    globalParams.autoGain = autoGainCache.isOn();
    globalParams.numBands = juce::jlimit(kMinBands, kMaxBands, static_cast<int>(std::lround(value(numBandsCache))));
    globalParams.stereoMode =
        static_cast<StereoMode>(juce::jlimit(0, 1, static_cast<int>(std::lround(stereoModeCache.raw()))));

    for (int i = 0; i < kMaxCrossovers; ++i)
        globalParams.crossoverHz[i] = value(crossoverCache[static_cast<size_t>(i)]);

    // Keep the edges ascending and at least a third of an octave apart even if
    // modulation pushes two together: unordered edges would produce unstable
    // crossover coefficients.
    constexpr float kMinRatio = 1.26f;
    for (int i = 1; i < kMaxCrossovers; ++i)
        globalParams.crossoverHz[i] =
            juce::jmax(globalParams.crossoverHz[i], globalParams.crossoverHz[i - 1] * kMinRatio);

    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& c = bandCache[static_cast<size_t>(b)];
        auto& p = bandParams[static_cast<size_t>(b)];
        p.driveDb = value(c.drive);
        p.mix01 = value(c.mix) * 0.01f;
        p.levelDb = value(c.level);
        p.pan = value(c.pan) * 0.01f;
        p.width01 = value(c.width) * 0.01f;
        p.style = static_cast<StyleID>(juce::jlimit(0, kNumStyles - 1, static_cast<int>(std::lround(c.style.raw()))));
        p.feedback01 = value(c.feedback) * 0.01f;
        p.feedbackFreq = value(c.feedbackFreq);
        p.dynamics = value(c.dynamics) * 0.01f;
        p.toneLowDb = value(c.toneLow);
        p.toneMidDb = value(c.toneMid);
        p.toneHighDb = value(c.toneHigh);
        p.bypass = c.bypass.isOn();
        p.solo = c.solo.isOn();
    }
}

void EmberAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const auto blockStartTicks = juce::Time::getHighResolutionTicks();

    // Index by the buffer's OWN channel count, never by the bus layout's.
    // getArrayOfWritePointers() has exactly buffer.getNumChannels() entries, so
    // building a view with getTotalNumOutputChannels() reads past the end of
    // that array whenever a host hands over a narrower buffer than the layout
    // advertises - which hosts do, and which validators deliberately do. It is
    // undefined behaviour that happens to survive on some platforms and faults
    // on others.
    const int numCh = juce::jmin(getTotalNumOutputChannels(), buffer.getNumChannels());
    const int totalIn = juce::jmin(getTotalNumInputChannels(), numCh);

    for (int ch = totalIn; ch < numCh; ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // Scrub the INPUT, not just the output. Several stages downstream hold
    // recursive state that a single non-finite sample latches permanently - the
    // oversampler's half-band filters and the tone stack's IIRs have no way back
    // once their history is NaN - so one bad buffer from a host would silence
    // the plug-in for the rest of the session rather than for one block.
    // Measured before this guard: a single NaN at block 50 dropped the output to
    // -169 dB and it never recovered.
    //
    // Two comparisons per sample against the cost of a dead instance is not a
    // close call, and the specification asks for NaN/Inf guards explicitly.
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            d[i] = dsputil::sanitise(d[i]);
    }

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
        const int chunk = juce::jmin(juce::jmin(kControlBlockSize, preparedBlockSize), numSamples - offset);

        pushSourceParameters();
        modulation.updateControlBlock(bandRms.data(), globalParams.numBands, chunk);
        resolveParameters(chunk);
        engine.setParameters(globalParams, bandParams.data(), kMaxBands);

        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), numCh, offset, chunk);
        engine.process(view);

        for (int b = 0; b < kMaxBands; ++b)
            bandRms[static_cast<size_t>(b)] = engine.getBandLevel(b);

        offset += chunk;
    }

    // Final safety net: a non-finite sample reaching the host is far worse than
    // a moment of silence, and some hosts will disable a plugin that emits one.
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            d[i] = dsputil::sanitise(d[i]);
    }

    publishCpuLoad(blockStartTicks, numSamples);
}

void EmberAudioProcessor::publishCpuLoad(juce::int64 startTicks, int numSamples) noexcept
{
    const auto rate = getSampleRate();

    if (rate <= 0.0 || numSamples <= 0)
        return;

    const auto elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - startTicks);
    const auto available = static_cast<double>(numSamples) / rate;

    if (available <= 0.0 || ! std::isfinite(elapsed))
        return;

    const auto instant = static_cast<float>(100.0 * elapsed / available);

    // One-pole at roughly a second. A per-block figure swings by several
    // percent block to block and is unreadable; the peak still gets through
    // because the coefficient is asymmetric - it rises four times faster than
    // it falls, so a spike shows up rather than being averaged away.
    const auto previous = cpuLoadPercent.load(std::memory_order_relaxed);
    const float coefficient = instant > previous ? 0.08f : 0.02f;
    auto smoothed = previous + (instant - previous) * coefficient;

    if (! std::isfinite(smoothed))
        smoothed = 0.0f;

    cpuLoadPercent.store(juce::jlimit(0.0f, 999.0f, smoothed), std::memory_order_relaxed);
}

void EmberAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // supportsDoublePrecisionProcessing() is false, so hosts never call this.
    // It exists only because the base class declares the overload.
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(getTotalNumOutputChannels(), buffer.getNumChannels());
    for (int ch = juce::jmin(getTotalNumInputChannels(), channels); ch < channels; ++ch)
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
                return index < static_cast<int>(midiCCForParam.size())
                           ? midiCCForParam[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                           : -1;
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
                if (index < static_cast<int>(midiCCForParam.size()))
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
                            if (i < static_cast<int>(midiCCForParam.size()))
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
            const int cc = index < static_cast<int>(midiCCForParam.size())
                               ? midiCCForParam[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                               : -1;
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
                            if (index < static_cast<int>(midiCCForParam.size()))
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

float EmberAudioProcessor::getBandHeat(int band) const noexcept
{
    if (band < 0 || band >= kMaxBands)
        return 0.0f;

    // The engine hands back a normalised distortion residual, already 0..1 and
    // already immune to level, gain matching and auto-gain. It only needs a
    // curve: distortion figures crowd into the bottom of the range, so a square
    // root spreads the useful part of the scale where the eye can see it.
    const float residual = engine.getBandHeatRatio(band);

    // Distortion figures crowd into the bottom of the range, so a square root
    // spreads the useful part of the scale where the eye can actually see it.
    const float shaped = std::sqrt(juce::jlimit(0.0f, 1.0f, residual));

    // Weighted by drive. The residual measures everything that makes the band's
    // output differ from its input - tone EQ, dynamics, feedback and pan all
    // contribute - but heat is meant to read as saturation. Without this, a
    // band at zero drive with an EQ curve on it glows as brightly as one being
    // hammered, which is exactly the wrong story.
    const auto& p = bandParams[static_cast<size_t>(band)];
    const float driveWeight = juce::jlimit(0.0f, 1.0f, p.driveDb / 24.0f);

    return juce::jlimit(0.0f, 1.0f, shaped * driveWeight);
}

float EmberAudioProcessor::getGlobalHeat() const noexcept
{
    const int bands = juce::jlimit(kMinBands, kMaxBands, globalParams.numBands);

    float weighted = 0.0f;
    float weight = 0.0f;

    for (int b = 0; b < bands; ++b)
    {
        // Weight by the band's own level, or a silent band driven to the moon
        // would light the logo as brightly as the one doing the work.
        const float level = juce::jlimit(0.0f, 1.0f, engine.getBandLevel(b));
        weighted += getBandHeat(b) * level;
        weight += level;
    }

    return weight > 1.0e-6f ? juce::jlimit(0.0f, 1.0f, weighted / weight) : 0.0f;
}

juce::AudioProcessorEditor* EmberAudioProcessor::createEditor()
{
    return new EmberAudioProcessorEditor(*this);
}
} // namespace ember
