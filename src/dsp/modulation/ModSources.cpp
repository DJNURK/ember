#include "dsp/modulation/ModSources.h"
#include <algorithm>
#include <cmath>

namespace ember
{
namespace
{
/** Wrap into [0, 1). Non-finite input collapses to 0 rather than poisoning the
    phase for the rest of the session. */
double wrap01 (double x) noexcept
{
    if (! std::isfinite (x))
        return 0.0;

    x -= std::floor (x);
    return (x < 0.0 || x >= 1.0) ? 0.0 : x;
}

float wrap01f (float x) noexcept
{
    if (! std::isfinite (x))
        return 0.0f;

    x -= std::floor (x);
    return (x < 0.0f || x >= 1.0f) ? 0.0f : x;
}

/** Exponential coefficient for moving over `numSamples` with a time constant of
    `seconds`. 0 means "snap". */
float blockCoefficient (double sampleRate, float seconds, int numSamples) noexcept
{
    if (seconds <= 0.0f || numSamples <= 0 || sampleRate <= 0.0)
        return 0.0f;

    return std::exp (-static_cast<float> (numSamples) / (static_cast<float> (sampleRate) * seconds));
}

ModCurve validCurve (ModCurve c) noexcept
{
    const int i = static_cast<int> (c);
    return (i >= 0 && i < static_cast<int> (ModCurve::Count)) ? c : ModCurve::Linear;
}

/** Attack aims past 1 and release aims below 0 so that both segments always
    cross their boundary in finite time, whatever level they started from. */
constexpr float kAttackTarget  = 1.25f;
constexpr float kReleaseTarget = -0.03f;
constexpr float kSegmentEps    = 1.0e-4f;
} // namespace

// ============================================================================
// XLfo
// ============================================================================
XLfo::XLfo()
{
    // Fill BOTH halves of the double buffer so the audio thread always has a
    // complete shape, whichever half is live.
    setDefaultShape();
    setDefaultShape();
}

void XLfo::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    smoother.prepare (sr);
    reset();
}

void XLfo::reset() noexcept
{
    phase        = 0.0;
    currentValue = 0.0f;
    smoother.reset (0.0f);
}

void XLfo::setParameters (const XLfoParams& newParams) noexcept
{
    params             = newParams;
    params.rateHz      = juce::jlimit (0.001f, 400.0f, dsputil::sanitise (params.rateHz));
    params.syncBeats   = juce::jlimit (0.0156f, 64.0f, dsputil::sanitise (params.syncBeats));
    params.phaseOffset = wrap01f (params.phaseOffset);
    params.steps       = juce::jlimit (0, 64, params.steps);
    params.smoothingMs = juce::jlimit (0.0f, 500.0f, dsputil::sanitise (params.smoothingMs));
    params.depth       = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.depth));
}

void XLfo::setShape (const LfoPoint* points, int numPoints)
{
    const int active = activeShape.load (std::memory_order_relaxed);
    const int target = 1 - active;

    Shape& dst = shapes[static_cast<size_t> (target)];
    const int count = (points != nullptr) ? juce::jlimit (0, kMaxLfoPoints, numPoints) : 0;

    for (int i = 0; i < count; ++i)
    {
        LfoPoint p = points[i];
        p.position = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (p.position));
        p.value    = juce::jlimit (-1.0f, 1.0f, dsputil::sanitise (p.value));
        p.curve    = validCurve (p.curve);
        dst.points[static_cast<size_t> (i)] = p;
    }

    dst.numPoints = count;

    // Insertion sort by position: at most 32 elements, message thread, and it
    // means the audio thread can assume sorted points and scan linearly.
    for (int i = 1; i < count; ++i)
    {
        const LfoPoint key = dst.points[static_cast<size_t> (i)];
        int j = i - 1;

        while (j >= 0 && dst.points[static_cast<size_t> (j)].position > key.position)
        {
            dst.points[static_cast<size_t> (j + 1)] = dst.points[static_cast<size_t> (j)];
            --j;
        }

        dst.points[static_cast<size_t> (j + 1)] = key;
    }

    activeShape.store (target, std::memory_order_release);
}

void XLfo::setDefaultShape()
{
    // Two points with S-curve segments: a smooth, cosine-like bipolar cycle.
    const std::array<LfoPoint, 2> defaultShape { {
        { 0.0f,  1.0f, ModCurve::SCurve },
        { 0.5f, -1.0f, ModCurve::SCurve }
    } };

    setShape (defaultShape.data(), static_cast<int> (defaultShape.size()));
}

int XLfo::getNumShapePoints() const noexcept
{
    return shapes[static_cast<size_t> (activeShape.load (std::memory_order_acquire))].numPoints;
}

LfoPoint XLfo::getShapePoint (int index) const noexcept
{
    const Shape& shape = shapes[static_cast<size_t> (activeShape.load (std::memory_order_acquire))];

    if (index < 0 || index >= shape.numPoints)
        return {};

    return shape.points[static_cast<size_t> (index)];
}

float XLfo::evaluateShapeAt (float phase01) const noexcept
{
    const Shape& shape = shapes[static_cast<size_t> (activeShape.load (std::memory_order_acquire))];
    return evaluate (shape, wrap01f (phase01));
}

float XLfo::evaluate (const Shape& shape, float phase01) const noexcept
{
    const int n = shape.numPoints;

    if (n <= 0)
        return 0.0f;

    if (n == 1)
        return shape.points[0].value;

    int   i = 0;
    int   j = 0;
    float segStart = 0.0f;
    float segEnd   = 0.0f;

    if (phase01 < shape.points[0].position)
    {
        // Inside the segment that wraps from the last point back to the first.
        i        = n - 1;
        j        = 0;
        segStart = shape.points[static_cast<size_t> (i)].position - 1.0f;
        segEnd   = shape.points[0].position;
    }
    else
    {
        for (int k = n - 1; k >= 0; --k)
        {
            if (shape.points[static_cast<size_t> (k)].position <= phase01)
            {
                i = k;
                break;
            }
        }

        j        = (i + 1) % n;
        segStart = shape.points[static_cast<size_t> (i)].position;
        segEnd   = (i + 1 < n) ? shape.points[static_cast<size_t> (j)].position
                               : shape.points[0].position + 1.0f;
    }

    const float span = segEnd - segStart;

    if (span <= 1.0e-6f)
        return shape.points[static_cast<size_t> (j)].value;

    const float t      = juce::jlimit (0.0f, 1.0f, (phase01 - segStart) / span);
    const float shaped = applyModCurve (t, shape.points[static_cast<size_t> (i)].curve);
    const float a      = shape.points[static_cast<size_t> (i)].value;
    const float b      = shape.points[static_cast<size_t> (j)].value;

    return a + (b - a) * shaped;
}

float XLfo::tick (int numSamples, double bpm, double ppqPosition, bool isPlaying) noexcept
{
    const double elapsed = numSamples > 0 ? static_cast<double> (numSamples) : 0.0;

    if (params.tempoSync)
    {
        const double beats = juce::jmax (1.0e-3, static_cast<double> (params.syncBeats));

        if (isPlaying && std::isfinite (ppqPosition))
        {
            // Locked to the timeline: loops and locates land on the same phase.
            phase = wrap01 (ppqPosition / beats);
        }
        else
        {
            const double tempo = (std::isfinite (bpm) && bpm > 1.0) ? bpm : 120.0;
            phase = wrap01 (phase + ((tempo / 60.0) / beats) * elapsed / sr);
        }
    }
    else
    {
        phase = wrap01 (phase + static_cast<double> (params.rateHz) * elapsed / sr);
    }

    float readPhase = wrap01f (static_cast<float> (phase) + params.phaseOffset);

    if (params.steps >= 2)
    {
        const float grid = static_cast<float> (params.steps);
        readPhase = juce::jlimit (0.0f, 1.0f, std::floor (readPhase * grid) / grid);
    }

    const Shape& shape = shapes[static_cast<size_t> (activeShape.load (std::memory_order_acquire))];
    const float  raw   = evaluate (shape, readPhase) * params.depth;

    currentValue = juce::jlimit (-1.0f, 1.0f, smoother.process (raw, params.smoothingMs, numSamples));
    return currentValue;
}

// ============================================================================
// EnvelopeGenerator
// ============================================================================
void EnvelopeGenerator::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void EnvelopeGenerator::reset() noexcept
{
    stage            = Stage::Idle;
    level            = 0.0f;
    heldNotes        = 0;
    retriggerPending = false;
    gate             = false;
    armed            = true;
}

void EnvelopeGenerator::setParameters (const EnvelopeGeneratorParams& newParams) noexcept
{
    params           = newParams;
    params.attackMs  = juce::jlimit (0.0f, 10000.0f, dsputil::sanitise (params.attackMs));
    params.decayMs   = juce::jlimit (0.0f, 10000.0f, dsputil::sanitise (params.decayMs));
    params.sustain   = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.sustain));
    params.releaseMs = juce::jlimit (0.0f, 20000.0f, dsputil::sanitise (params.releaseMs));
    params.threshold = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.threshold));

    if (static_cast<int> (params.trigger) < 0
        || static_cast<int> (params.trigger) >= static_cast<int> (EgTriggerMode::Count))
        params.trigger = EgTriggerMode::Transient;
}

void EnvelopeGenerator::noteOn() noexcept
{
    if (heldNotes < 1024)
        ++heldNotes;

    retriggerPending = true;
}

void EnvelopeGenerator::noteOff() noexcept
{
    if (heldNotes > 0)
        --heldNotes;
}

void EnvelopeGenerator::allNotesOff() noexcept
{
    heldNotes = 0;
}

float EnvelopeGenerator::tick (int numSamples, float detectorValue) noexcept
{
    const float detector = juce::jlimit (0.0f, 4.0f, dsputil::sanitise (detectorValue));

    // ---- gate -------------------------------------------------------------
    bool retrigger = false;

    switch (params.trigger)
    {
        case EgTriggerMode::Transient:
        {
            const float openAt  = params.threshold;
            const float closeAt = params.threshold * 0.7f;

            if (! gate && armed && detector >= openAt)
            {
                gate      = true;
                armed     = false;
                retrigger = true;
            }
            else if (gate && detector < closeAt)
            {
                gate = false;
            }

            if (! gate && detector < closeAt)
                armed = true;

            retriggerPending = false;   // MIDI notes are ignored in this mode
            break;
        }

        case EgTriggerMode::MidiNote:
        {
            gate = heldNotes > 0;

            if (retriggerPending)
            {
                retrigger        = true;
                gate             = true;
                retriggerPending = false;
            }
            break;
        }

        case EgTriggerMode::Count:
        default:
            break;
    }

    if (retrigger || (gate && (stage == Stage::Idle || stage == Stage::Release)))
        stage = Stage::Attack;
    else if (! gate && stage != Stage::Idle && stage != Stage::Release)
        stage = Stage::Release;

    // ---- segment ----------------------------------------------------------
    switch (stage)
    {
        case Stage::Idle:
            level = 0.0f;
            break;

        case Stage::Attack:
        {
            const float coeff = blockCoefficient (sr, params.attackMs * 0.001f, numSamples);
            level = kAttackTarget + coeff * (level - kAttackTarget);

            if (level >= 1.0f - kSegmentEps)
            {
                level = 1.0f;
                stage = Stage::Decay;
            }
            break;
        }

        case Stage::Decay:
        {
            const float coeff = blockCoefficient (sr, params.decayMs * 0.001f, numSamples);
            level = params.sustain + coeff * (level - params.sustain);

            if (level <= params.sustain + kSegmentEps)
            {
                level = params.sustain;
                stage = Stage::Sustain;
            }
            break;
        }

        case Stage::Sustain:
            level = params.sustain;
            break;

        case Stage::Release:
        {
            const float coeff = blockCoefficient (sr, params.releaseMs * 0.001f, numSamples);
            level = kReleaseTarget + coeff * (level - kReleaseTarget);

            if (level <= kSegmentEps)
            {
                level = 0.0f;
                stage = Stage::Idle;
            }
            break;
        }
    }

    level = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (level));
    return level;
}

// ============================================================================
// EnvelopeFollower
// ============================================================================
void EnvelopeFollower::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void EnvelopeFollower::reset() noexcept
{
    level = 0.0f;
}

void EnvelopeFollower::setParameters (const EnvelopeFollowerParams& newParams) noexcept
{
    params           = newParams;
    params.attackMs  = juce::jlimit (0.0f, 1000.0f, dsputil::sanitise (params.attackMs));
    params.releaseMs = juce::jlimit (0.0f, 5000.0f, dsputil::sanitise (params.releaseMs));
    params.band      = juce::jlimit (-1, kMaxBands - 1, params.band);
    params.gainDb    = juce::jlimit (-24.0f, 48.0f, dsputil::sanitise (params.gainDb));
    params.floorDb   = juce::jlimit (-120.0f, -6.0f, dsputil::sanitise (params.floorDb));
}

float EnvelopeFollower::tick (int numSamples, const float* perBandRms, int numBands) noexcept
{
    float rms = 0.0f;

    if (perBandRms != nullptr && numBands > 0)
    {
        if (params.band >= 0 && params.band < numBands)
        {
            rms = dsputil::sanitise (perBandRms[params.band]);
        }
        else
        {
            // Full range: power sum of every band.
            float power = 0.0f;

            for (int b = 0; b < numBands; ++b)
            {
                const float v = dsputil::sanitise (perBandRms[b]);
                power += v * v;
            }

            rms = std::sqrt (power);
        }
    }

    rms = juce::jmax (0.0f, rms) * juce::Decibels::decibelsToGain (params.gainDb);

    const float floorDb = params.floorDb;
    const float db      = juce::Decibels::gainToDecibels (rms, floorDb);
    const float target  = juce::jlimit (0.0f, 1.0f, (db - floorDb) / juce::jmax (6.0f, -floorDb));

    const float seconds = (target > level ? params.attackMs : params.releaseMs) * 0.001f;
    const float coeff   = blockCoefficient (sr, seconds, numSamples);

    level = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (target + coeff * (level - target)));
    return level;
}

// ============================================================================
// XyController
// ============================================================================
void XyController::prepare (double sampleRate) noexcept
{
    smoothedX.prepare (sampleRate);
    smoothedY.prepare (sampleRate);
    reset();
}

void XyController::reset() noexcept
{
    smoothedX.reset (params.x);
    smoothedY.reset (params.y);
    currentValue = 0.0f;
}

void XyController::setParameters (const XyControllerParams& newParams) noexcept
{
    params             = newParams;
    params.x           = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.x));
    params.y           = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.y));
    params.smoothingMs = juce::jlimit (0.0f, 500.0f, dsputil::sanitise (params.smoothingMs));

    if (static_cast<int> (params.axis) < 0
        || static_cast<int> (params.axis) >= static_cast<int> (XyAxis::Count))
        params.axis = XyAxis::X;
}

float XyController::tick (int numSamples) noexcept
{
    const float x = smoothedX.process (params.x, params.smoothingMs, numSamples);
    const float y = smoothedY.process (params.y, params.smoothingMs, numSamples);

    switch (params.axis)
    {
        case XyAxis::X:
            currentValue = x;
            break;

        case XyAxis::Y:
            currentValue = y;
            break;

        case XyAxis::Radius:
        {
            const float dx = x - 0.5f;
            const float dy = y - 0.5f;
            currentValue = std::sqrt (dx * dx + dy * dy) * 2.0f;
            break;
        }

        case XyAxis::Count:
        default:
            currentValue = x;
            break;
    }

    currentValue = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (currentValue));
    return currentValue;
}

// ============================================================================
// MidiSource
// ============================================================================
void MidiSource::prepare (double sampleRate) noexcept
{
    smoother.prepare (sampleRate);
    reset();
}

void MidiSource::reset() noexcept
{
    smoother.reset (0.0f);
}

void MidiSource::setParameters (const MidiSourceParams& newParams) noexcept
{
    params             = newParams;
    params.ccNumber    = juce::jlimit (0, 127, params.ccNumber);
    params.smoothingMs = juce::jlimit (0.0f, 500.0f, dsputil::sanitise (params.smoothingMs));

    if (static_cast<int> (params.kind) < 0
        || static_cast<int> (params.kind) >= static_cast<int> (MidiSourceKind::Count))
        params.kind = MidiSourceKind::Velocity;
}

float MidiSource::tick (int numSamples, const MidiState& state) noexcept
{
    float raw = 0.0f;

    switch (params.kind)
    {
        case MidiSourceKind::Velocity:
            raw = state.velocity;
            break;

        case MidiSourceKind::ControlChange:
            raw = state.cc[static_cast<size_t> (juce::jlimit (0, 127, params.ccNumber))];
            break;

        case MidiSourceKind::ModWheel:
            raw = state.cc[1];
            break;

        case MidiSourceKind::NoteNumber:
            raw = state.noteNumber;
            break;

        case MidiSourceKind::Count:
        default:
            break;
    }

    return juce::jlimit (0.0f, 1.0f, smoother.process (juce::jlimit (0.0f, 1.0f, raw),
                                                       params.smoothingMs, numSamples));
}

// ============================================================================
// MacroSource
// ============================================================================
void MacroSource::prepare (double sampleRate) noexcept
{
    smoother.prepare (sampleRate);
    reset();
}

void MacroSource::reset() noexcept
{
    smoother.reset (params.value);
}

void MacroSource::setParameters (const MacroParams& newParams) noexcept
{
    params             = newParams;
    params.value       = juce::jlimit (0.0f, 1.0f, dsputil::sanitise (params.value));
    params.smoothingMs = juce::jlimit (0.0f, 500.0f, dsputil::sanitise (params.smoothingMs));
}

float MacroSource::tick (int numSamples) noexcept
{
    return juce::jlimit (0.0f, 1.0f, smoother.process (params.value, params.smoothingMs, numSamples));
}
} // namespace ember
