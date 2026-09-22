#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "dsp/EmberTypes.h"
#include "dsp/modulation/ModTypes.h"
#include "dsp/modulation/ModSources.h"

namespace ember
{
/**
    A parameter of a modulation SOURCE that may itself be a modulation
    destination (LFO rate modulated by a macro, envelope attack modulated by
    another envelope, and so on).

    The engine has no idea what a `targetIndex` means — it is an index into the
    processor's table of modulatable parameters. `setTargetOwner` is how the
    processor tells the engine "this target is source S's rate", which is what
    lets the engine (a) order the evaluation so S sees the modulated value in
    the SAME control block and (b) reject routings that would form a cycle.

    Targets that are NOT registered as a source parameter are ordinary DSP
    destinations: the engine just accumulates their offset.
*/
enum class ModSourceField : int
{
    Rate = 0,  ///< XLFO rate / sync division (applied as +/- 4 octaves)
    Phase,     ///< XLFO phase offset (additive, wraps)
    Depth,     ///< XLFO depth, follower gain (additive, clamped)
    Smoothing, ///< output smoothing time (additive, +/- 500 ms)
    Attack,    ///< EG / follower attack (applied as +/- 3 octaves of time)
    Decay,     ///< EG decay (+/- 3 octaves)
    Sustain,   ///< EG sustain level (additive)
    Release,   ///< EG / follower release (+/- 3 octaves)
    Threshold, ///< EG transient threshold (additive)
    Value,     ///< macro / XY / MIDI output value (additive)
    ValueY,    ///< the XY pad's second coordinate (additive)
    Count
};

inline constexpr int kNumModSourceFields = static_cast<int>(ModSourceField::Count);

/** Every source's base (un-modulated) parameters in one struct, so the
    processor can push the whole APVTS snapshot in a single realtime-safe call
    once per control block. The individual setters exist for GUI-driven partial
    updates. */
struct ModSourceParameters
{
    std::array<XLfoParams, kNumXLFOs> lfos{};
    std::array<EnvelopeGeneratorParams, kNumEnvGenerators> envGenerators{};
    std::array<EnvelopeFollowerParams, kNumEnvFollowers> envFollowers{};
    XyControllerParams xy{};
    std::array<MidiSourceParams, kNumMidiSources> midi{};
    std::array<MacroParams, kNumMacros> macros{};
};

/** ValueTree identifiers for the serialised graph. Public so the preset manager
    and the state tests can reach them without string literals. */
namespace ModStateIds
{
extern const juce::Identifier modulation; ///< parent node
extern const juce::Identifier connection; ///< one child per routing
extern const juce::Identifier source;
extern const juce::Identifier target;
extern const juce::Identifier amount;
extern const juce::Identifier curve;
extern const juce::Identifier smoothing;
extern const juce::Identifier enabled;
} // namespace ModStateIds

/**
    Ember's modulation matrix: 23 sources, up to `kMaxModConnections` (64)
    routings, evaluated once per control block.

    ---------------------------------------------------------------- rate
    EVERYTHING here runs at CONTROL RATE. `updateControlBlock` is called once
    per `kControlBlockSize` (32) samples and produces ONE offset per target for
    that block. There is deliberately no per-sample interpolation in this class:
    the BAND CHAIN (and the processor's parameter snapshot) is responsible for
    ramping the value it reads from `getModulationOffset` across the block, the
    same way it already ramps smoothed parameters. Doing it here would mean
    either a buffer per target or per-sample work for 64 routings, and the band
    chain has to ramp its parameters anyway.

    If the host hands over a block that is not a multiple of 32 samples, call
    the three-argument overload with the real sample count so LFO rates and
    envelope times stay exact for 1-sample and 7-sample buffers alike. Zero and
    negative counts are legal and simply do not advance time.

    ---------------------------------------------------------------- threading
    Audio thread:   setTransport, processMidi, the setParameters family,
                    updateControlBlock, getModulationOffset, getSourceValue.
                    None of these allocate, lock, log, throw, or touch a
                    juce::String.
    Message thread: addConnection / removeConnection / setConnection /
                    clearConnections / setTargetOwner / setXLfoShape /
                    toValueTree / fromValueTree.

    Graph edits never block the audio thread and never let it observe a
    half-edited graph. The editor owns a dense master array of connections and
    compiles it into an `EvaluationPlan` — connections sorted into dependency
    order, plus the topological source order and the target-owner map.

    The plan reaches the audio thread through a lock-free triple buffer. Three
    preallocated plans and ONE atomic word holding their roles — write slot,
    pending slot, read slot, plus a "fresh" flag. The editor fills the write
    slot and swaps write <-> pending with a single compare-exchange; the audio
    thread, when the flag is set, swaps read <-> pending with a single
    compare-exchange at the top of the block. Because each side's hand-over is
    one atomic operation, the three roles are always a permutation of the three
    buffers: the editor's buffer is never the one being read, neither side ever
    waits or allocates, and no edit rate can make them collide.

    (Two things this replaced, both of which ThreadSanitizer catches within
    seconds: a plain double buffer — the audio thread cannot atomically load
    the live index AND mark it in use, so an edit landing in that window
    scribbles on the plan being read; and a pointer mailbox where the audio
    thread takes the new plan and hands the old one back as two separate
    atomics — between them it holds two buffers and the editor can be left with
    none.)

    ---------------------------------------------------------------- ordering
    A source may modulate another source's parameter. `addConnection` therefore
    runs a real topological sort (Kahn) over the 23 sources and REJECTS any
    routing that would close a cycle — including a source modulating itself.
    Disabled connections still count as edges, so toggling `enabled` can never
    turn a legal graph into a cyclic one. The resulting order is computed once,
    at edit time, and merely replayed per block: evaluation is a single linear
    pass over the sources with a linear pass over the sorted connections.

    ---------------------------------------------------------------- amounts
    Each connection applies `applyModCurve (sourceValue, curve) * amount` to its
    target, and the contribution is smoothed with a per-connection one-pole
    (`smoothingMs`). Because the contribution is what gets smoothed, disabling a
    connection ramps it out instead of stepping. Offsets from several
    connections sum, and the total is clamped to [-1, +1]: it is a delta in
    NORMALISED target units, so the consumer adds it to the parameter's
    normalised value and clamps to [0, 1].
*/
class ModulationEngine
{
public:
    ModulationEngine();
    ~ModulationEngine();

    // ------------------------------------------------------------ lifetime
    /** Allocates. Message / prepare thread only.
        @param sampleRate           host sample rate
        @param maxBlockSize         largest block the host will ask for (unused
                                    by the engine — it keeps no per-sample
                                    buffers — but part of the standard contract)
        @param numModulatableParams size of the processor's target table; any
                                    connection pointing outside it is kept in
                                    the graph but contributes nothing. */
    void prepare(double sampleRate, int maxBlockSize, int numModulatableParams);

    /** Silence every source and offset. Does not touch the graph. */
    void reset() noexcept;

    // ------------------------------------------------------ per-block inputs
    /** Host transport, for tempo-synced XLFOs. Audio thread. A stopped or
        absent transport is fine: synced LFOs then free-run at the synced rate. */
    void setTransport(double bpm, double ppqPosition, bool isPlaying) noexcept;

    /** Parse the block's MIDI into the shared MIDI state and gate the
        MIDI-triggered envelopes. Audio thread; reads raw bytes, so no
        juce::MidiMessage is ever constructed. */
    void processMidi(const juce::MidiBuffer& midi) noexcept;

    /** Push every source's base parameters at once. Audio thread. */
    void setSourceParameters(const ModSourceParameters& newParams) noexcept;

    /** Partial pushes. Out-of-range indices are ignored. Audio thread. */
    void setXLfoParameters(int index, const XLfoParams& p) noexcept;
    void setEnvelopeGeneratorParameters(int index, const EnvelopeGeneratorParams& p) noexcept;
    void setEnvelopeFollowerParameters(int index, const EnvelopeFollowerParams& p) noexcept;
    void setXyParameters(const XyControllerParams& p) noexcept;
    void setMidiSourceParameters(int index, const MidiSourceParams& p) noexcept;
    void setMacroParameters(int index, const MacroParams& p) noexcept;

    /** Replace an XLFO's shape. MESSAGE THREAD ONLY (double-buffered inside
        XLfo, see ModSources.h). */
    void setXLfoShape(int index, const LfoPoint* points, int numPoints);

    /** Read-only access for the shape editor / GUI drawing. Out-of-range
        indices clamp to the first LFO, so this never returns a dangling ref. */
    const XLfo& getXLfo(int index) const noexcept;

    // -------------------------------------------------------- evaluation
    /** Advance every source by one nominal control block (kControlBlockSize
        samples) and recompute all offsets. Audio thread.
        @param perBandRms  one RMS value per active band, measured by the
                           engine's caller; may be null.
        @param numBands    number of valid entries in `perBandRms`. */
    void updateControlBlock(const float* perBandRms, int numBands) noexcept;

    /** As above, but advancing by exactly `numSamples` samples of audio time.
        Use this when the host's block size is not a multiple of 32 so rates
        stay exact. `numSamples <= 0` evaluates without advancing time. */
    void updateControlBlock(const float* perBandRms, int numBands, int numSamples) noexcept;

    /** Summed, smoothed, curve-shaped offset for a target, in normalised units,
        clamped to [-1, +1]. Valid for the control block that was last
        evaluated; the caller ramps it across the block. Out-of-range indices
        return 0. Audio thread. */
    float getModulationOffset(int targetIndex) const noexcept;

    /** Last value of a source, for the GUI: [-1, +1] for bipolar sources
        (XLFO), [0, 1] for the rest — see `sourceInfoFromFlatIndex`. Lock-free
        from any thread. */
    float getSourceValue(int flatSourceIndex) const noexcept;

    // ------------------------------------------------- source-owned targets
    /** Register `targetIndex` as `field` of modulation source `flatSourceIndex`
        so that source-to-source modulation is dependency-ordered. Pass
        `flatSourceIndex < 0` to unregister. Message thread; rebuilds the plan.

        Register ownership BEFORE loading or building the graph: `addConnection`
        can only reject a cycle it can see, so registering an owner afterwards
        could in principle close one. If that happens the engine keeps every
        routing and falls back to flat (index) evaluation order, which is still
        finite and stable — a source-to-source routing then simply lands one
        control block late. */
    void setTargetOwner(int targetIndex, int flatSourceIndex, ModSourceField field);

    /** Forget every registration made with `setTargetOwner`. Message thread. */
    void clearTargetOwners();

    // ------------------------------------------------------ graph editing
    /** Append a routing. Returns false (and changes nothing) if the table is
        full, the source or target index is invalid, the same source -> target
        pair already exists, or the routing would create a cycle.
        Message thread. */
    bool addConnection(const ModConnection& c);

    /** Remove slot `slot`; later slots shift down by one so slots stay dense.
        Out-of-range slots are ignored. Message thread. */
    void removeConnection(int slot);

    /** Replace slot `slot`. A replacement that would create a cycle or
        duplicate an existing routing is rejected and the old connection is
        kept (an assertion fires in debug). Message thread. */
    void setConnection(int slot, const ModConnection& c);

    /** Remove every routing. Message thread. */
    void clearConnections();

    int getNumConnections() const noexcept { return numConnections; }

    /** Slots are dense: valid range is [0, getNumConnections()). An
        out-of-range slot returns a reference to a shared empty connection. */
    const ModConnection& getConnection(int slot) const noexcept;

    /** How many routings currently point at a target — for the GUI's
        "modulated" ring. Message thread. */
    int getNumConnectionsForTarget(int targetIndex) const noexcept;

    // ------------------------------------------------------------- state
    /** The whole graph as a `ModStateIds::modulation` node with one
        `connection` child per routing, in slot order. Message thread. */
    juce::ValueTree toValueTree() const;

    /** Replace the whole graph. Accepts either the modulation node itself or a
        parent containing one. Tolerant by design: missing properties fall back
        to the `ModConnection` defaults, out-of-range values are clamped,
        unknown children and properties are ignored, and routings that would
        form a cycle are dropped rather than throwing — an old or hand-edited
        state must never take the plugin down. Message thread. */
    void fromValueTree(const juce::ValueTree& tree);

private:
    /** One routing, compiled for the audio thread: no strings, no branches for
        validity, and the smoothing coefficient already turned into a one-pole
        multiplier for the nominal control block. */
    struct RuntimeConnection
    {
        int slot{-1}; ///< editor slot, so smoother state follows the routing
        int sourceIndex{-1};
        int targetIndex{-1};
        float amount{0.0f};
        ModCurve curve{ModCurve::Linear};
        float smoothCoeff{0.0f};   ///< one-pole multiplier for a NOMINAL 32-sample block
        float smoothSeconds{0.0f}; ///< the time constant itself, for odd block lengths
        bool enabled{false};
    };

    /** Immutable snapshot the audio thread evaluates. Built by the editor. */
    struct EvaluationPlan
    {
        std::array<RuntimeConnection, kMaxModConnections> connections{};
        int numConnections{0};
        /** All kNumModSources sources in dependency order. */
        std::array<int, kNumModSources> order{};
        /** fieldTarget[source][field] = target index, or -1. */
        std::array<std::array<int, kNumModSourceFields>, kNumModSources> fieldTarget{};
    };

    // ---- audio thread
    float tickSource(const EvaluationPlan& plan, int flatIndex, int numSamples, const float* perBandRms,
                     int numBands) noexcept;
    float fieldOffset(const EvaluationPlan& plan, int flatIndex, ModSourceField field) const noexcept;
    float detectorFor(const float* perBandRms, int numBands, int band) const noexcept;

    // ---- message thread
    void rebuildPlan();
    void publishPlan();
    bool topologicalOrder(const ModConnection* conns, int numConns, std::array<int, kNumModSources>& outOrder) const;
    int ownerOfTarget(int targetIndex) const noexcept;
    static ModConnection sanitised(const ModConnection& c) noexcept;

    // ---- sources
    std::array<XLfo, kNumXLFOs> lfos;
    std::array<EnvelopeGenerator, kNumEnvGenerators> envGenerators;
    std::array<EnvelopeFollower, kNumEnvFollowers> envFollowers;
    XyController xyPad;
    std::array<MidiSource, kNumMidiSources> midiSources;
    std::array<MacroSource, kNumMacros> macroSources;

    ModSourceParameters baseParams{};
    MidiState midiState{};

    // ---- graph (message thread owns these)
    std::array<ModConnection, kMaxModConnections> connections{};
    int numConnections{0};
    ModConnection emptyConnection{};

    struct TargetOwner
    {
        int source{-1};
        ModSourceField field{ModSourceField::Value};
    };

    std::vector<TargetOwner> targetOwners;

    // ---- plan hand-off (see the class comment: lock-free triple buffer)
    std::array<EvaluationPlan, 3> planStorage{};
    /** Packed slot roles: bits 0-1 write, 2-3 pending, 4-5 read, bit 8 "fresh". */
    std::atomic<std::uint32_t> planSlots{0};
    int readPlanIndex{0};  ///< audio thread only
    int writePlanIndex{1}; ///< message thread only

    // ---- audio-thread state
    std::vector<float> offsets;
    std::array<float, kMaxModConnections> smoothState{};
    std::array<std::atomic<float>, kNumModSources> sourceValues{};

    double transportBpm{120.0};
    double transportPpq{0.0};
    bool transportPlaying{false};

    double sr{44100.0};
    int numTargets{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulationEngine)
};
} // namespace ember
