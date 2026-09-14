#pragma once
#include <array>
#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"
#include "dsp/DspUtils.h"
#include "dsp/modulation/ModTypes.h"

namespace ember
{
/**
    The six modulation source generators.

    Shared contract for every source in this file:

    - `prepare (sampleRate)` takes the HOST sample rate (not a control rate) and
      allocates nothing beyond what the object already owns. Everything is a
      fixed-size member; no source ever touches the heap.
    - `tick (numSamples, ...)` advances the source by `numSamples` samples of
      audio time and returns its new value. It is called once per CONTROL BLOCK
      (`kControlBlockSize` = 32 samples) from the audio thread, so passing the
      real number of samples the block covered keeps rates exact even when the
      host hands us a 1-sample or 7-sample buffer. `numSamples <= 0` is legal
      and simply returns the current value without advancing.
    - `setParameters` is realtime-safe and is expected to be called from the
      audio thread once per control block, exactly like `EmberEngine::
      setParameters`. (Calling it from the message thread while audio is running
      races on a small POD; that is the same trade the rest of Ember makes, and
      the values are always finite and clamped, so the worst case is one control
      block of mixed old/new parameters.)
    - Output range follows `ModSourceInfo::bipolar`: XLfo is bipolar and returns
      [-1, +1]; every other source is unipolar and returns [0, 1]. Values are
      always finite — every recursive state is sanitised on the way out, so one
      NaN-infected host buffer cannot poison a source for the session.
    - `reset()` returns the source to its silent initial state and allocates
      nothing.
*/

// ============================================================================
/** One-pole smoother that advances by a whole block at a time.

    A `juce::SmoothedValue` ramps per sample; modulation sources only produce a
    value per control block, so the smoothing coefficient has to be derived from
    the block length instead. One `std::exp` per source per block (~32k/second
    for the whole modulation section) is far below the noise floor of the
    saturation stages. */
class ControlSmoother
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    }

    void reset (float initialValue = 0.0f) noexcept { value = dsputil::sanitise (initialValue); }

    /** Move `numSamples` of audio time towards `target` with a `timeMs`
        exponential. A non-positive time (or block) snaps. */
    float process (float target, float timeMs, int numSamples) noexcept
    {
        const float safeTarget = dsputil::sanitise (target);
        const float seconds    = juce::jmax (0.0f, timeMs) * 0.001f;

        if (seconds <= 0.0f || numSamples <= 0)
        {
            value = safeTarget;
            return value;
        }

        const float coeff = std::exp (-static_cast<float> (numSamples)
                                      / (static_cast<float> (sr) * seconds));
        value = dsputil::sanitise (safeTarget + coeff * (value - safeTarget));
        return value;
    }

    float getValue() const noexcept { return value; }

private:
    double sr { 44100.0 };
    float  value { 0.0f };
};

// ============================================================================
/** Maximum number of breakpoints in an XLFO shape. Fixed and preallocated: the
    shape editor may add and remove points freely, but the audio thread only
    ever reads a fully-formed, sorted array of at most this many. */
inline constexpr int kMaxLfoPoints = 32;

/** One breakpoint of an XLFO shape. `curve` shapes the segment that STARTS at
    this point and ends at the next one (the last point's segment wraps around
    to the first). */
struct LfoPoint
{
    float    position { 0.0f };                 ///< 0 .. 1 within the cycle
    float    value { 0.0f };                    ///< -1 .. +1
    ModCurve curve { ModCurve::Linear };
};

struct XLfoParams
{
    float rateHz { 1.0f };         ///< free-running rate, 0.01 .. 40 Hz
    bool  tempoSync { false };     ///< derive the phase from the host timeline
    float syncBeats { 4.0f };      ///< cycle length in quarter notes when synced
    float phaseOffset { 0.0f };    ///< 0 .. 1, applied at read time (phase stays continuous)
    int   steps { 0 };             ///< 0 or 1 = smooth; >= 2 snaps the phase to a grid
    float smoothingMs { 0.0f };    ///< output smoothing, 0 .. 500 ms
    float depth { 1.0f };          ///< 0 .. 1 output scale
};

/**
    Multi-point shape LFO. Bipolar: output is [-1, +1] * depth.

    Free-running or host-synced. When synced AND the transport is rolling the
    phase is DERIVED from the ppq position rather than integrated, so the shape
    stays locked to the timeline through loops, locates and tempo changes; when
    the transport is stopped it free-runs at the synced rate so the GUI still
    shows movement. Phase is kept in double precision and wrapped every block,
    so it never drifts and never loses resolution over a long session.
*/
class XLfo
{
public:
    static constexpr bool kBipolar = true;

    XLfo();

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const XLfoParams& newParams) noexcept;

    /** Replace the shape. MESSAGE THREAD ONLY: it writes the inactive half of a
        double buffer and publishes it with a release store, so the audio thread
        sees the whole new shape at once rather than a half-written one. Points
        are clamped and sorted here so the audio thread never has to.
        `numPoints` is clamped to [0, kMaxLfoPoints] and `points` must hold that
        many entries; fewer than two points produces a constant.

        (Two `setShape` calls less than one control block apart could in
        principle let the audio thread read the buffer being rewritten. Shapes
        are edited at human speed, and every stored point is clamped before it
        is written, so the worst case is one block of a blended shape — still
        finite, still within [-1, +1]. It is not worth a triple buffer.) */
    void setShape (const LfoPoint* points, int numPoints);

    /** Restore the default shape (a smooth cosine-like 3-point curve). Message
        thread only. */
    void setDefaultShape();

    int      getNumShapePoints() const noexcept;
    LfoPoint getShapePoint (int index) const noexcept;

    /** Advance and return the new value in [-1, +1]. Audio thread. */
    float tick (int numSamples, double bpm, double ppqPosition, bool isPlaying) noexcept;

    float getValue() const noexcept { return currentValue; }

    /** Value of the current shape at an arbitrary phase, for drawing the editor.
        Message thread; does not touch the running phase. */
    float evaluateShapeAt (float phase01) const noexcept;

private:
    struct Shape
    {
        std::array<LfoPoint, kMaxLfoPoints> points {};
        int                                 numPoints { 0 };
    };

    float evaluate (const Shape& shape, float phase01) const noexcept;

    std::array<Shape, 2> shapes {};
    std::atomic<int>     activeShape { 0 };

    XLfoParams      params {};
    ControlSmoother smoother;
    double          sr { 44100.0 };
    double          phase { 0.0 };      ///< 0 .. 1, before the phase offset
    float           currentValue { 0.0f };
};

// ============================================================================
enum class EgTriggerMode : int
{
    Transient = 0,   ///< fires when the detector crosses the threshold
    MidiNote,        ///< fires on MIDI note-on, releases when the last note lifts
    Count
};

struct EnvelopeGeneratorParams
{
    float         attackMs { 5.0f };
    float         decayMs { 120.0f };
    float         sustain { 0.7f };        ///< 0 .. 1
    float         releaseMs { 200.0f };
    float         threshold { 0.2f };      ///< 0 .. 1 detector level (Transient mode)
    int           detectorBand { -1 };     ///< band to watch, -1 = full range
    EgTriggerMode trigger { EgTriggerMode::Transient };
};

/**
    ADSR envelope, unipolar [0, 1].

    Segments are exponential with an overshooting target (attack aims past 1,
    release aims below 0), which is both the analog behaviour and the reason the
    envelope can never stall: each segment reaches its boundary in finite time
    regardless of the level it started from. That is what makes a retrigger in
    the middle of the release safe — the attack simply restarts from wherever
    the level currently is and still arrives at 1.

    The transient trigger is hysteretic: it arms again only after the detector
    has fallen to 70% of the threshold, so a signal sitting exactly on the
    threshold cannot machine-gun the envelope.
*/
class EnvelopeGenerator
{
public:
    static constexpr bool kBipolar = false;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const EnvelopeGeneratorParams& newParams) noexcept;

    /** MIDI gate. Audio thread, realtime-safe. Note counting is done here so a
        legato chord releases only when the LAST note lifts. */
    void noteOn() noexcept;
    void noteOff() noexcept;
    void allNotesOff() noexcept;

    /** Advance and return the new level in [0, 1]. `detectorValue` is the
        (already band-selected) 0..1 detector the engine measured this block. */
    float tick (int numSamples, float detectorValue) noexcept;

    float getValue() const noexcept { return level; }

    /** Which band the engine should measure for this envelope, -1 = full range. */
    int getDetectorBand() const noexcept { return params.detectorBand; }

private:
    enum class Stage : int { Idle = 0, Attack, Decay, Sustain, Release };

    EnvelopeGeneratorParams params {};
    double sr { 44100.0 };
    Stage  stage { Stage::Idle };
    float  level { 0.0f };
    int    heldNotes { 0 };
    bool   retriggerPending { false };
    bool   gate { false };
    bool   armed { true };
};

// ============================================================================
struct EnvelopeFollowerParams
{
    float attackMs { 10.0f };
    float releaseMs { 200.0f };
    int   band { -1 };            ///< band to follow, -1 = full range
    float gainDb { 0.0f };        ///< detector make-up before the dB mapping
    float floorDb { -60.0f };     ///< detector level that maps to 0
};

/**
    Sidechain envelope follower, unipolar [0, 1].

    The engine measures per-band RMS once per control block and hands the array
    in; the follower only selects one band (or the full-range sum) and smooths
    it, so no detector work is duplicated across the four followers.

    The mapping is logarithmic — `floorDb` maps to 0 and 0 dBFS maps to 1 —
    because a linear RMS reading spends almost all of its range in the top 6 dB
    and makes for a useless modulation source.
*/
class EnvelopeFollower
{
public:
    static constexpr bool kBipolar = false;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const EnvelopeFollowerParams& newParams) noexcept;

    /** `perBandRms` may be null / `numBands` may be 0; the follower then decays
        towards silence instead of misbehaving. */
    float tick (int numSamples, const float* perBandRms, int numBands) noexcept;

    float getValue() const noexcept { return level; }
    int   getBand() const noexcept { return params.band; }

private:
    EnvelopeFollowerParams params {};
    double sr { 44100.0 };
    float  level { 0.0f };
};

// ============================================================================
/** Which of the pad's two axes the single XY modulation source emits. The pad
    itself always keeps both smoothed values (`getX`/`getY`) for the GUI and for
    direct use by the processor; the flat source index defined in EmberTypes.h
    is one value, so it emits the axis selected here. */
enum class XyAxis : int { X = 0, Y, Radius, Count };

struct XyControllerParams
{
    float  x { 0.5f };             ///< 0 .. 1 pad position
    float  y { 0.5f };             ///< 0 .. 1 pad position
    float  smoothingMs { 30.0f };  ///< both axes, 0 .. 500 ms
    XyAxis axis { XyAxis::X };     ///< which value the mod source emits
};

/** XY pad source, unipolar [0, 1] on both axes. */
class XyController
{
public:
    static constexpr bool kBipolar = false;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const XyControllerParams& newParams) noexcept;

    /** Advance both axes; returns the value of the selected axis. */
    float tick (int numSamples) noexcept;

    float getX() const noexcept { return smoothedX.getValue(); }
    float getY() const noexcept { return smoothedY.getValue(); }
    float getValue() const noexcept { return currentValue; }

private:
    XyControllerParams params {};
    ControlSmoother    smoothedX, smoothedY;
    float              currentValue { 0.0f };
};

// ============================================================================
enum class MidiSourceKind : int
{
    Velocity = 0,     ///< velocity of the most recent note-on
    ControlChange,    ///< an arbitrary CC number
    ModWheel,         ///< CC 1, broken out because everyone wants it
    NoteNumber,       ///< note number of the most recent note-on, /127
    Count
};

/** Everything the engine's MIDI parser extracts, shared by all MIDI sources so
    the buffer is parsed exactly once per block. All values are 0..1. */
struct MidiState
{
    float                     velocity { 0.0f };
    float                     noteNumber { 0.0f };
    std::array<float, 128>    cc {};
    int                       heldNotes { 0 };

    void reset() noexcept
    {
        velocity   = 0.0f;
        noteNumber = 0.0f;
        heldNotes  = 0;
        cc.fill (0.0f);
    }
};

struct MidiSourceParams
{
    MidiSourceKind kind { MidiSourceKind::Velocity };
    int            ccNumber { 1 };        ///< 0 .. 127, used by ControlChange
    float          smoothingMs { 20.0f }; ///< 0 .. 500 ms
};

/** MIDI-derived source, unipolar [0, 1]. */
class MidiSource
{
public:
    static constexpr bool kBipolar = false;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const MidiSourceParams& newParams) noexcept;

    float tick (int numSamples, const MidiState& state) noexcept;
    float getValue() const noexcept { return smoother.getValue(); }

private:
    MidiSourceParams params {};
    ControlSmoother  smoother;
};

// ============================================================================
struct MacroParams
{
    float value { 0.0f };          ///< 0 .. 1
    float smoothingMs { 20.0f };   ///< 0 .. 500 ms
};

/** Macro knob, unipolar [0, 1]: a smoothed pass-through of its parameter, so a
    macro that is dragged in the GUI never steps its destinations. */
class MacroSource
{
public:
    static constexpr bool kBipolar = false;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParameters (const MacroParams& newParams) noexcept;

    float tick (int numSamples) noexcept;
    float getValue() const noexcept { return smoother.getValue(); }

private:
    MacroParams     params {};
    ControlSmoother smoother;
};
} // namespace ember
