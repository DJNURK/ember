#include "dsp/modulation/ModulationEngine.h"
#include "dsp/DspUtils.h"
#include <algorithm>
#include <cmath>

namespace ember
{
namespace ModStateIds
{
const juce::Identifier modulation{"MODULATION"};
const juce::Identifier connection{"CONNECTION"};
const juce::Identifier source{"source"};
const juce::Identifier target{"target"};
const juce::Identifier amount{"amount"};
const juce::Identifier curve{"curve"};
const juce::Identifier smoothing{"smoothing"};
const juce::Identifier enabled{"enabled"};
} // namespace ModStateIds

namespace
{
/** Time and frequency parameters are modulated multiplicatively — a bipolar
    offset of +/-1 spans +/-`octaves` octaves — because an additive offset on a
    0.5 ms attack and on a 5 s attack cannot both be musical. */
float scaleByOctaves(float base, float offset, float octaves) noexcept
{
    if (std::abs(offset) < 1.0e-6f)
        return base;

    return base * std::exp2(octaves * offset);
}

// ---- triple-buffer slot word -----------------------------------------------
// bits 0-1: the buffer the editor may write, 2-3: the published one waiting to
// be picked up, 4-5: the one the audio thread is reading, bit 8: "pending holds
// something newer than read". The three indices are always a permutation of
// {0,1,2} because both hand-overs are swaps.
constexpr std::uint32_t kFreshBit = 1u << 8;

constexpr std::uint32_t writeSlot(std::uint32_t s) noexcept
{
    return s & 3u;
}
constexpr std::uint32_t pendingSlot(std::uint32_t s) noexcept
{
    return (s >> 2) & 3u;
}
constexpr std::uint32_t readSlot(std::uint32_t s) noexcept
{
    return (s >> 4) & 3u;
}

constexpr std::uint32_t packSlots(std::uint32_t write, std::uint32_t pending, std::uint32_t read, bool fresh) noexcept
{
    return write | (pending << 2) | (read << 4) | (fresh ? kFreshBit : 0u);
}
} // namespace

// ============================================================================
ModulationEngine::ModulationEngine()
{
    for (auto& plan : planStorage)
    {
        for (int i = 0; i < kNumModSources; ++i)
            plan.order[static_cast<size_t>(i)] = i;

        for (auto& row : plan.fieldTarget)
            row.fill(-1);

        plan.numConnections = 0;
    }

    // One plan for the audio thread, one for the editor, one spare in between.
    readPlanIndex = 0;
    writePlanIndex = 1;
    planSlots.store(packSlots(1u, 2u, 0u, false), std::memory_order_relaxed);

    for (auto& value : sourceValues)
        value.store(0.0f, std::memory_order_relaxed);

    smoothState.fill(0.0f);
}

ModulationEngine::~ModulationEngine() = default;

// ---------------------------------------------------------------- lifetime
void ModulationEngine::prepare(double sampleRate, [[maybe_unused]] int maxBlockSize, int numModulatableParams)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    numTargets = juce::jmax(0, numModulatableParams);

    offsets.assign(static_cast<size_t>(numTargets), 0.0f);

    // resize (not assign): registrations made before prepare survive.
    if (targetOwners.size() != static_cast<size_t>(numTargets))
        targetOwners.resize(static_cast<size_t>(numTargets));

    for (auto& lfo : lfos)
        lfo.prepare(sr);
    for (auto& eg : envGenerators)
        eg.prepare(sr);
    for (auto& ef : envFollowers)
        ef.prepare(sr);
    xyPad.prepare(sr);
    for (auto& ms : midiSources)
        ms.prepare(sr);
    for (auto& macro : macroSources)
        macro.prepare(sr);

    reset();
    rebuildPlan(); // smoothing coefficients depend on the sample rate
}

void ModulationEngine::reset() noexcept
{
    for (auto& lfo : lfos)
        lfo.reset();
    for (auto& eg : envGenerators)
        eg.reset();
    for (auto& ef : envFollowers)
        ef.reset();
    xyPad.reset();
    for (auto& ms : midiSources)
        ms.reset();
    for (auto& macro : macroSources)
        macro.reset();

    midiState.reset();
    smoothState.fill(0.0f);
    std::fill(offsets.begin(), offsets.end(), 0.0f);

    for (auto& value : sourceValues)
        value.store(0.0f, std::memory_order_relaxed);

    // A stale ppq left over from the previous transport position would make the
    // first synced block after a reset jump to the wrong phase.
    transportBpm = 120.0;
    transportPpq = 0.0;
    transportPlaying = false;
}

// -------------------------------------------------------- per-block inputs
void ModulationEngine::setTransport(double bpm, double ppqPosition, bool isPlaying) noexcept
{
    transportBpm = (std::isfinite(bpm) && bpm > 1.0) ? bpm : 120.0;
    transportPpq = std::isfinite(ppqPosition) ? ppqPosition : 0.0;
    transportPlaying = isPlaying;
}

void ModulationEngine::processMidi(const juce::MidiBuffer& midi) noexcept
{
    constexpr float kInv127 = 1.0f / 127.0f;

    for (const auto metadata : midi)
    {
        if (metadata.numBytes < 2 || metadata.data == nullptr)
            continue;

        const int status = metadata.data[0] & 0xF0;
        const int data1 = metadata.data[1] & 0x7F;
        const int data2 = metadata.numBytes > 2 ? (metadata.data[2] & 0x7F) : 0;

        if (status == 0x90 && data2 > 0)
        {
            midiState.velocity = static_cast<float>(data2) * kInv127;
            midiState.noteNumber = static_cast<float>(data1) * kInv127;

            if (midiState.heldNotes < 1024)
                ++midiState.heldNotes;

            for (auto& eg : envGenerators)
                eg.noteOn();
        }
        else if (status == 0x80 || (status == 0x90 && data2 == 0))
        {
            if (midiState.heldNotes > 0)
                --midiState.heldNotes;

            for (auto& eg : envGenerators)
                eg.noteOff();
        }
        else if (status == 0xB0)
        {
            midiState.cc[static_cast<size_t>(data1)] = static_cast<float>(data2) * kInv127;

            if (data1 == 120 || data1 == 123) // all sound off / all notes off
            {
                midiState.heldNotes = 0;

                for (auto& eg : envGenerators)
                    eg.allNotesOff();
            }
        }
    }
}

void ModulationEngine::setSourceParameters(const ModSourceParameters& newParams) noexcept
{
    baseParams = newParams;
}

void ModulationEngine::setXLfoParameters(int index, const XLfoParams& p) noexcept
{
    if (index >= 0 && index < kNumXLFOs)
        baseParams.lfos[static_cast<size_t>(index)] = p;
}

void ModulationEngine::setEnvelopeGeneratorParameters(int index, const EnvelopeGeneratorParams& p) noexcept
{
    if (index >= 0 && index < kNumEnvGenerators)
        baseParams.envGenerators[static_cast<size_t>(index)] = p;
}

void ModulationEngine::setEnvelopeFollowerParameters(int index, const EnvelopeFollowerParams& p) noexcept
{
    if (index >= 0 && index < kNumEnvFollowers)
        baseParams.envFollowers[static_cast<size_t>(index)] = p;
}

void ModulationEngine::setXyParameters(const XyControllerParams& p) noexcept
{
    baseParams.xy = p;
}

void ModulationEngine::setMidiSourceParameters(int index, const MidiSourceParams& p) noexcept
{
    if (index >= 0 && index < kNumMidiSources)
        baseParams.midi[static_cast<size_t>(index)] = p;
}

void ModulationEngine::setMacroParameters(int index, const MacroParams& p) noexcept
{
    if (index >= 0 && index < kNumMacros)
        baseParams.macros[static_cast<size_t>(index)] = p;
}

void ModulationEngine::setXLfoShape(int index, const LfoPoint* points, int numPoints)
{
    if (index >= 0 && index < kNumXLFOs)
        lfos[static_cast<size_t>(index)].setShape(points, numPoints);
}

const XLfo& ModulationEngine::getXLfo(int index) const noexcept
{
    const int safe = juce::jlimit(0, kNumXLFOs - 1, index);
    return lfos[static_cast<size_t>(safe)];
}

// ------------------------------------------------------------- evaluation
void ModulationEngine::updateControlBlock(const float* perBandRms, int numBands) noexcept
{
    updateControlBlock(perBandRms, numBands, kControlBlockSize);
}

void ModulationEngine::updateControlBlock(const float* perBandRms, int numBands, int numSamples) noexcept
{
    // Pick up a newer plan if the editor published one: a single swap of the
    // read and pending slots, so the editor can never be writing what we take.
    std::uint32_t expected = planSlots.load(std::memory_order_acquire);

    if ((expected & kFreshBit) != 0u)
    {
        std::uint32_t desired = 0u;

        do
        {
            desired = packSlots(writeSlot(expected),   // untouched
                                readSlot(expected),    // hand ours back
                                pendingSlot(expected), // take theirs
                                false);
        } while (
            !planSlots.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire));

        readPlanIndex = static_cast<int>(readSlot(desired));
    }

    const EvaluationPlan& plan = planStorage[static_cast<size_t>(readPlanIndex)];

    std::fill(offsets.begin(), offsets.end(), 0.0f);

    // The compiled coefficient assumes a nominal 32-sample block. A host that
    // hands over 1-sample buffers would otherwise run every connection smoother
    // 32x too fast, which turns a 5 ms glide into 0.16 ms of stepping; a
    // non-positive block must not advance the smoothers at all. Both are off
    // the fast path, so an exact 32-sample block still costs one int compare.
    const bool nominalBlock = (numSamples == kControlBlockSize);
    const bool advanceTime = (numSamples > 0);

    int connectionIndex = 0;

    for (int i = 0; i < kNumModSources; ++i)
    {
        const int flat = plan.order[static_cast<size_t>(i)];

        if (flat < 0 || flat >= kNumModSources)
            continue;

        const float value = tickSource(plan, flat, numSamples, perBandRms, numBands);
        sourceValues[static_cast<size_t>(flat)].store(value, std::memory_order_relaxed);

        // Connections are pre-sorted into the same order, so every routing out
        // of this source is the next contiguous run.
        while (connectionIndex < plan.numConnections &&
               plan.connections[static_cast<size_t>(connectionIndex)].sourceIndex == flat)
        {
            const RuntimeConnection& conn = plan.connections[static_cast<size_t>(connectionIndex)];
            ++connectionIndex;

            const float contribution = conn.enabled ? applyModCurve(value, conn.curve) * conn.amount : 0.0f;

            float& state = smoothState[static_cast<size_t>(juce::jlimit(0, kMaxModConnections - 1, conn.slot))];

            if (advanceTime)
            {
                float coeff;

                if (nominalBlock)
                    coeff = conn.smoothCoeff;
                else if (conn.smoothSeconds <= 0.0f)
                    coeff = 0.0f;
                else
                    coeff = std::exp(-static_cast<float>(numSamples) / (static_cast<float>(sr) * conn.smoothSeconds));

                state = dsputil::sanitise(contribution + coeff * (state - contribution));
            }

            if (conn.targetIndex >= 0 && conn.targetIndex < numTargets)
                offsets[static_cast<size_t>(conn.targetIndex)] += state;
        }
    }
}

float ModulationEngine::tickSource(const EvaluationPlan& plan, int flatIndex, int numSamples, const float* perBandRms,
                                   int numBands) noexcept
{
    const ModSourceInfo info = sourceInfoFromFlatIndex(flatIndex);
    const size_t i = static_cast<size_t>(info.indexWithinType);

    switch (info.type)
    {
    case ModSourceType::XLFO:
    {
        XLfoParams p = baseParams.lfos[i];
        const float rateOffset = fieldOffset(plan, flatIndex, ModSourceField::Rate);

        p.rateHz = scaleByOctaves(p.rateHz, rateOffset, 4.0f);
        p.syncBeats = scaleByOctaves(p.syncBeats, -rateOffset, 4.0f); // shorter cycle = faster
        p.phaseOffset += fieldOffset(plan, flatIndex, ModSourceField::Phase);
        p.depth += fieldOffset(plan, flatIndex, ModSourceField::Depth);
        p.smoothingMs += 500.0f * fieldOffset(plan, flatIndex, ModSourceField::Smoothing);

        lfos[i].setParameters(p);
        return lfos[i].tick(numSamples, transportBpm, transportPpq, transportPlaying);
    }

    case ModSourceType::EnvelopeGenerator:
    {
        EnvelopeGeneratorParams p = baseParams.envGenerators[i];

        p.attackMs = scaleByOctaves(p.attackMs, fieldOffset(plan, flatIndex, ModSourceField::Attack), 3.0f);
        p.decayMs = scaleByOctaves(p.decayMs, fieldOffset(plan, flatIndex, ModSourceField::Decay), 3.0f);
        p.releaseMs = scaleByOctaves(p.releaseMs, fieldOffset(plan, flatIndex, ModSourceField::Release), 3.0f);
        p.sustain += fieldOffset(plan, flatIndex, ModSourceField::Sustain);
        p.threshold += fieldOffset(plan, flatIndex, ModSourceField::Threshold);

        envGenerators[i].setParameters(p);
        return envGenerators[i].tick(numSamples, detectorFor(perBandRms, numBands, p.detectorBand));
    }

    case ModSourceType::EnvelopeFollower:
    {
        EnvelopeFollowerParams p = baseParams.envFollowers[i];

        p.attackMs = scaleByOctaves(p.attackMs, fieldOffset(plan, flatIndex, ModSourceField::Attack), 3.0f);
        p.releaseMs = scaleByOctaves(p.releaseMs, fieldOffset(plan, flatIndex, ModSourceField::Release), 3.0f);
        p.gainDb += 24.0f * fieldOffset(plan, flatIndex, ModSourceField::Depth);

        envFollowers[i].setParameters(p);
        return envFollowers[i].tick(numSamples, perBandRms, numBands);
    }

    case ModSourceType::XYController:
    {
        XyControllerParams p = baseParams.xy;

        // Both coordinates move with their own parameter's modulation; `axis`
        // decides which of them the source emits. Routing the offset to
        // whichever axis was selected instead meant "XY Y" was owned by nothing
        // and modulating "XY X" silently drove Y.
        p.x += fieldOffset(plan, flatIndex, ModSourceField::Value);
        p.y += fieldOffset(plan, flatIndex, ModSourceField::ValueY);

        p.smoothingMs += 500.0f * fieldOffset(plan, flatIndex, ModSourceField::Smoothing);

        xyPad.setParameters(p);
        return xyPad.tick(numSamples);
    }

    case ModSourceType::MidiSource:
    {
        MidiSourceParams p = baseParams.midi[i];
        p.smoothingMs += 500.0f * fieldOffset(plan, flatIndex, ModSourceField::Smoothing);

        midiSources[i].setParameters(p);

        // The raw value comes from the MIDI state rather than a parameter,
        // so a Value modulation is added after smoothing.
        const float value = midiSources[i].tick(numSamples, midiState);
        return juce::jlimit(0.0f, 1.0f, value + fieldOffset(plan, flatIndex, ModSourceField::Value));
    }

    case ModSourceType::Macro:
    {
        MacroParams p = baseParams.macros[i];
        p.value += fieldOffset(plan, flatIndex, ModSourceField::Value);
        p.smoothingMs += 500.0f * fieldOffset(plan, flatIndex, ModSourceField::Smoothing);

        macroSources[i].setParameters(p);
        return macroSources[i].tick(numSamples);
    }

    case ModSourceType::Count:
        break;
    }

    return 0.0f;
}

float ModulationEngine::fieldOffset(const EvaluationPlan& plan, int flatIndex, ModSourceField field) const noexcept
{
    if (flatIndex < 0 || flatIndex >= kNumModSources)
        return 0.0f;

    const int fieldIndex = static_cast<int>(field);

    if (fieldIndex < 0 || fieldIndex >= kNumModSourceFields)
        return 0.0f;

    const int targetIndex = plan.fieldTarget[static_cast<size_t>(flatIndex)][static_cast<size_t>(fieldIndex)];

    if (targetIndex < 0 || targetIndex >= static_cast<int>(offsets.size()))
        return 0.0f;

    return juce::jlimit(-1.0f, 1.0f, offsets[static_cast<size_t>(targetIndex)]);
}

float ModulationEngine::detectorFor(const float* perBandRms, int numBands, int band) const noexcept
{
    if (perBandRms == nullptr || numBands <= 0)
        return 0.0f;

    if (band >= 0 && band < numBands)
        return juce::jmax(0.0f, dsputil::sanitise(perBandRms[band]));

    float power = 0.0f;

    for (int b = 0; b < numBands; ++b)
    {
        const float v = dsputil::sanitise(perBandRms[b]);
        power += v * v;
    }

    return std::sqrt(power);
}

float ModulationEngine::getModulationOffset(int targetIndex) const noexcept
{
    if (targetIndex < 0 || targetIndex >= static_cast<int>(offsets.size()))
        return 0.0f;

    return juce::jlimit(-1.0f, 1.0f, offsets[static_cast<size_t>(targetIndex)]);
}

float ModulationEngine::getSourceValue(int flatSourceIndex) const noexcept
{
    if (flatSourceIndex < 0 || flatSourceIndex >= kNumModSources)
        return 0.0f;

    return sourceValues[static_cast<size_t>(flatSourceIndex)].load(std::memory_order_relaxed);
}

// ---------------------------------------------------- source-owned targets
void ModulationEngine::setTargetOwner(int targetIndex, int flatSourceIndex, ModSourceField field)
{
    if (targetIndex < 0)
        return;

    if (targetIndex >= static_cast<int>(targetOwners.size()))
        targetOwners.resize(static_cast<size_t>(targetIndex) + 1);

    const int fieldIndex = static_cast<int>(field);

    TargetOwner& owner = targetOwners[static_cast<size_t>(targetIndex)];
    owner.source = (flatSourceIndex >= 0 && flatSourceIndex < kNumModSources) ? flatSourceIndex : -1;
    owner.field = (fieldIndex >= 0 && fieldIndex < kNumModSourceFields) ? field : ModSourceField::Value;

    rebuildPlan();
}

void ModulationEngine::clearTargetOwners()
{
    for (auto& owner : targetOwners)
        owner = TargetOwner{};

    rebuildPlan();
}

int ModulationEngine::ownerOfTarget(int targetIndex) const noexcept
{
    if (targetIndex < 0 || targetIndex >= static_cast<int>(targetOwners.size()))
        return -1;

    return targetOwners[static_cast<size_t>(targetIndex)].source;
}

// -------------------------------------------------------- graph editing
ModConnection ModulationEngine::sanitised(const ModConnection& c) noexcept
{
    ModConnection out = c;

    out.amount = juce::jlimit(-1.0f, 1.0f, dsputil::sanitise(c.amount));
    out.smoothingMs = juce::jlimit(0.0f, 500.0f, dsputil::sanitise(c.smoothingMs));

    const int curveIndex = static_cast<int>(c.curve);

    if (curveIndex < 0 || curveIndex >= static_cast<int>(ModCurve::Count))
        out.curve = ModCurve::Linear;

    if (out.sourceIndex < 0 || out.sourceIndex >= kNumModSources)
        out.sourceIndex = -1;

    if (out.targetIndex < 0)
        out.targetIndex = -1;

    return out;
}

bool ModulationEngine::topologicalOrder(const ModConnection* conns, int numConns,
                                        std::array<int, kNumModSources>& outOrder) const
{
    // Adjacency over the 23 sources: an edge means "must be evaluated before".
    // 529 bools on the message thread's stack; never touched by audio.
    std::array<std::array<bool, kNumModSources>, kNumModSources> adjacency{};
    std::array<int, kNumModSources> inDegree{};

    for (int i = 0; i < numConns; ++i)
    {
        const ModConnection& c = conns[i];

        if (c.sourceIndex < 0 || c.sourceIndex >= kNumModSources)
            continue;

        // Disabled connections still count as edges: toggling `enabled` must
        // never be able to turn a legal graph into a cyclic one.
        const int owner = ownerOfTarget(c.targetIndex);

        if (owner < 0 || owner >= kNumModSources)
            continue;

        auto& edge = adjacency[static_cast<size_t>(c.sourceIndex)][static_cast<size_t>(owner)];

        if (!edge)
        {
            edge = true;
            ++inDegree[static_cast<size_t>(owner)];
        }
    }

    std::array<int, kNumModSources> queue{};
    int head = 0, tail = 0, count = 0;

    for (int n = 0; n < kNumModSources; ++n)
        if (inDegree[static_cast<size_t>(n)] == 0)
            queue[static_cast<size_t>(tail++)] = n;

    while (head < tail)
    {
        const int n = queue[static_cast<size_t>(head++)];
        outOrder[static_cast<size_t>(count++)] = n;

        for (int m = 0; m < kNumModSources; ++m)
        {
            if (!adjacency[static_cast<size_t>(n)][static_cast<size_t>(m)])
                continue;

            if (--inDegree[static_cast<size_t>(m)] == 0)
                queue[static_cast<size_t>(tail++)] = m;
        }
    }

    if (count != kNumModSources)
    {
        // A cycle: the nodes left over never reached in-degree zero.
        for (int n = 0; n < kNumModSources; ++n)
            outOrder[static_cast<size_t>(n)] = n;

        return false;
    }

    return true;
}

void ModulationEngine::rebuildPlan()
{
    EvaluationPlan& plan = planStorage[static_cast<size_t>(writePlanIndex)];

    // ---- target ownership -------------------------------------------------
    for (auto& row : plan.fieldTarget)
        row.fill(-1);

    for (int t = 0; t < static_cast<int>(targetOwners.size()); ++t)
    {
        const TargetOwner& owner = targetOwners[static_cast<size_t>(t)];

        if (owner.source < 0 || owner.source >= kNumModSources)
            continue;

        const int fieldIndex = static_cast<int>(owner.field);

        if (fieldIndex >= 0 && fieldIndex < kNumModSourceFields)
            plan.fieldTarget[static_cast<size_t>(owner.source)][static_cast<size_t>(fieldIndex)] = t;
    }

    // ---- dependency order -------------------------------------------------
    // A failure here means an owner was registered after the routings and
    // closed a cycle addConnection could not see. The graph still evaluates,
    // just in flat index order (see setTargetOwner's documentation).
    const bool acyclic = topologicalOrder(connections.data(), numConnections, plan.order);
    jassert(acyclic);
    juce::ignoreUnused(acyclic);

    std::array<int, kNumModSources> position{};

    for (int i = 0; i < kNumModSources; ++i)
        position[static_cast<size_t>(plan.order[static_cast<size_t>(i)])] = i;

    // ---- compile the connections -----------------------------------------
    int count = 0;

    for (int slot = 0; slot < numConnections; ++slot)
    {
        const ModConnection& c = connections[static_cast<size_t>(slot)];

        if (c.sourceIndex < 0 || c.sourceIndex >= kNumModSources)
            continue;

        if (c.targetIndex < 0 || c.targetIndex >= numTargets)
            continue; // outside the processor's table: kept in the graph, silent here

        RuntimeConnection compiled;
        compiled.slot = slot;
        compiled.sourceIndex = c.sourceIndex;
        compiled.targetIndex = c.targetIndex;
        compiled.amount = c.amount;
        compiled.curve = c.curve;
        compiled.enabled = c.enabled;
        compiled.smoothSeconds = juce::jmax(0.0f, c.smoothingMs) * 0.001f;
        compiled.smoothCoeff =
            (compiled.smoothSeconds <= 0.0f)
                ? 0.0f
                : std::exp(-static_cast<float>(kControlBlockSize) / (static_cast<float>(sr) * compiled.smoothSeconds));

        // Stable insertion sort by the source's position in the evaluation
        // order, so the audio thread can walk sources and routings together.
        int k = count;

        while (k > 0 && position[static_cast<size_t>(plan.connections[static_cast<size_t>(k - 1)].sourceIndex)] >
                            position[static_cast<size_t>(compiled.sourceIndex)])
        {
            plan.connections[static_cast<size_t>(k)] = plan.connections[static_cast<size_t>(k - 1)];
            --k;
        }

        plan.connections[static_cast<size_t>(k)] = compiled;
        ++count;
    }

    plan.numConnections = count;

    publishPlan();
}

void ModulationEngine::publishPlan()
{
    // Offer the finished plan and take the pending slot's buffer to write next:
    // one swap, so we always end up owning a buffer the audio thread is not
    // reading. If the audio thread never picked up our previous plan we simply
    // get it back — it was published, never read, and is ours to overwrite.
    std::uint32_t expected = planSlots.load(std::memory_order_relaxed);
    std::uint32_t desired = 0u;

    do
    {
        desired = packSlots(pendingSlot(expected), // take theirs
                            writeSlot(expected),   // offer ours
                            readSlot(expected),    // untouched
                            true);
    } while (!planSlots.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed));

    writePlanIndex = static_cast<int>(writeSlot(desired));
}

bool ModulationEngine::addConnection(const ModConnection& c)
{
    if (numConnections >= kMaxModConnections)
        return false;

    if (c.sourceIndex < 0 || c.sourceIndex >= kNumModSources || c.targetIndex < 0)
        return false;

    for (int i = 0; i < numConnections; ++i)
    {
        const ModConnection& existing = connections[static_cast<size_t>(i)];

        if (existing.sourceIndex == c.sourceIndex && existing.targetIndex == c.targetIndex)
            return false;
    }

    const size_t slot = static_cast<size_t>(numConnections);
    connections[slot] = sanitised(c);

    std::array<int, kNumModSources> probe{};

    if (!topologicalOrder(connections.data(), numConnections + 1, probe))
    {
        connections[slot] = ModConnection{};
        return false;
    }

    ++numConnections;
    rebuildPlan();
    return true;
}

void ModulationEngine::removeConnection(int slot)
{
    if (slot < 0 || slot >= numConnections)
        return;

    // Slots stay dense. The audio thread's per-slot smoother state follows the
    // slot, so routings above the removed one glide to their new contribution
    // over their own smoothing time instead of stepping.
    for (int i = slot; i < numConnections - 1; ++i)
        connections[static_cast<size_t>(i)] = connections[static_cast<size_t>(i + 1)];

    connections[static_cast<size_t>(numConnections - 1)] = ModConnection{};
    --numConnections;

    rebuildPlan();
}

void ModulationEngine::setConnection(int slot, const ModConnection& c)
{
    if (slot < 0 || slot >= numConnections)
        return;

    const ModConnection replacement = sanitised(c);

    for (int i = 0; i < numConnections; ++i)
    {
        if (i == slot)
            continue;

        const ModConnection& existing = connections[static_cast<size_t>(i)];

        if (existing.sourceIndex == replacement.sourceIndex && existing.targetIndex == replacement.targetIndex &&
            replacement.sourceIndex >= 0)
        {
            jassertfalse; // duplicate routing
            return;
        }
    }

    const ModConnection previous = connections[static_cast<size_t>(slot)];
    connections[static_cast<size_t>(slot)] = replacement;

    std::array<int, kNumModSources> probe{};

    if (!topologicalOrder(connections.data(), numConnections, probe))
    {
        connections[static_cast<size_t>(slot)] = previous;
        jassertfalse; // would have created a cycle
        return;
    }

    rebuildPlan();
}

void ModulationEngine::clearConnections()
{
    for (auto& c : connections)
        c = ModConnection{};

    numConnections = 0;
    rebuildPlan();
}

const ModConnection& ModulationEngine::getConnection(int slot) const noexcept
{
    if (slot < 0 || slot >= numConnections)
        return emptyConnection;

    return connections[static_cast<size_t>(slot)];
}

int ModulationEngine::getNumConnectionsForTarget(int targetIndex) const noexcept
{
    if (targetIndex < 0)
        return 0;

    int count = 0;

    for (int i = 0; i < numConnections; ++i)
        if (connections[static_cast<size_t>(i)].targetIndex == targetIndex)
            ++count;

    return count;
}

// --------------------------------------------------------------- state
juce::ValueTree ModulationEngine::toValueTree() const
{
    juce::ValueTree tree(ModStateIds::modulation);

    for (int i = 0; i < numConnections; ++i)
    {
        const ModConnection& c = connections[static_cast<size_t>(i)];

        juce::ValueTree child(ModStateIds::connection);
        child.setProperty(ModStateIds::source, c.sourceIndex, nullptr);
        child.setProperty(ModStateIds::target, c.targetIndex, nullptr);
        child.setProperty(ModStateIds::amount, static_cast<double>(c.amount), nullptr);
        child.setProperty(ModStateIds::curve, static_cast<int>(c.curve), nullptr);
        child.setProperty(ModStateIds::smoothing, static_cast<double>(c.smoothingMs), nullptr);
        child.setProperty(ModStateIds::enabled, c.enabled, nullptr);

        tree.appendChild(child, nullptr);
    }

    return tree;
}

void ModulationEngine::fromValueTree(const juce::ValueTree& tree)
{
    juce::ValueTree node = tree;

    if (!node.hasType(ModStateIds::modulation))
        node = tree.getChildWithName(ModStateIds::modulation);

    for (auto& c : connections)
        c = ModConnection{};

    numConnections = 0;

    if (node.isValid())
    {
        const int numChildren = node.getNumChildren();

        for (int i = 0; i < numChildren && numConnections < kMaxModConnections; ++i)
        {
            const juce::ValueTree child = node.getChild(i);

            if (!child.hasType(ModStateIds::connection))
                continue;

            const ModConnection defaults{};

            ModConnection c;
            c.sourceIndex = static_cast<int>(child.getProperty(ModStateIds::source, -1));
            c.targetIndex = static_cast<int>(child.getProperty(ModStateIds::target, -1));
            c.amount = static_cast<float>(static_cast<double>(child.getProperty(ModStateIds::amount, 0.0)));
            c.curve = static_cast<ModCurve>(static_cast<int>(child.getProperty(ModStateIds::curve, 0)));
            c.smoothingMs = static_cast<float>(static_cast<double>(
                child.getProperty(ModStateIds::smoothing, static_cast<double>(defaults.smoothingMs))));
            c.enabled = static_cast<bool>(child.getProperty(ModStateIds::enabled, true));

            if (c.sourceIndex < 0 || c.sourceIndex >= kNumModSources || c.targetIndex < 0)
                continue;

            bool duplicate = false;

            for (int k = 0; k < numConnections; ++k)
            {
                const ModConnection& existing = connections[static_cast<size_t>(k)];

                if (existing.sourceIndex == c.sourceIndex && existing.targetIndex == c.targetIndex)
                {
                    duplicate = true;
                    break;
                }
            }

            if (duplicate)
                continue;

            const size_t slot = static_cast<size_t>(numConnections);
            connections[slot] = sanitised(c);

            std::array<int, kNumModSources> probe{};

            if (!topologicalOrder(connections.data(), numConnections + 1, probe))
            {
                connections[slot] = ModConnection{}; // corrupt/hand-edited state: drop it
                continue;
            }

            ++numConnections;
        }
    }

    rebuildPlan();
}
} // namespace ember
