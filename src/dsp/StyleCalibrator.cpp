#include "dsp/StyleCalibrator.h"

#include <juce_dsp/juce_dsp.h>

#include "dsp/DspUtils.h"
#include "dsp/styles/SaturationStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace ember
{
namespace
{
// ------------------------------------------------------------------ stimulus
/** Fixed seed. The whole point of measuring rather than hand-tuning is that the
    numbers are reproducible, so nothing here may depend on time, address space
    layout, threading or the host. Same seed => same stimulus => same table. */
constexpr std::uint32_t kNoiseSeed = 0xE3B12F5u;

/** Samples discarded before the measurement window opens, so that envelope
    followers, magnetisation memories and filter states have settled and are not
    measured mid-transient. ~10 ms at 96 kHz. */
constexpr int kWarmupSamples = 1024;

/** Measurement window. 4096 samples is ~43 ms at 96 kHz — long enough that the
    RMS estimate of a noise stimulus is stable to well under 0.1 dB, short
    enough that the whole 19 x 41 table builds in a few tens of milliseconds. */
constexpr int kMeasureSamples = 4096;

constexpr int kStimulusSamples = kWarmupSamples + kMeasureSamples;

/** Nominal operating level (-18 dBFS RMS, the usual alignment level). Gain
    compensation for a nonlinearity is only meaningful at a stated input level;
    this is the level the table is calibrated for. */
constexpr float kStimulusLevelDb = -18.0f;

/** A measured compensation outside this range means the style did something
    pathological at that drive; clamping keeps one bad measurement from turning
    into a +200 dB multiplier on the audio thread. */
constexpr float kMaxCompensationDb = 40.0f;

/** Output RMS at or below this counts as "silent" — unmeasurable, so the entry
    falls back to unity rather than to a huge boost. */
constexpr double kSilenceFloor = 1.0e-7;

/** dB between adjacent table entries (1.0 for the 0..40 dB / 41 point table). */
constexpr float kDriveStepDb =
    StyleCalibrator::kMaxDriveDb / static_cast<float> (StyleCalibrator::kNumDrivePoints - 1);

/** Marsaglia xorshift32. Deterministic, no library RNG involved: std::mt19937
    would also be portable but the distribution adaptors are not specified
    bit-for-bit across implementations, and this table must be. */
class Xorshift32
{
public:
    explicit Xorshift32 (std::uint32_t seed) noexcept
        : state (seed != 0u ? seed : 1u) {}

    std::uint32_t nextBits() noexcept
    {
        state = static_cast<std::uint32_t> (state ^ (state << 13));
        state = static_cast<std::uint32_t> (state ^ (state >> 17));
        state = static_cast<std::uint32_t> (state ^ (state << 5));
        return state;
    }

    /** Uniform in [-1, 1). */
    float nextBipolar() noexcept
    {
        return static_cast<float> (static_cast<std::int32_t> (nextBits())) / 2147483648.0f;
    }

private:
    std::uint32_t state;
};

/** Paul Kellet's economy pink filter: three one-poles summed, about -3 dB per
    octave. Pink is the right stimulus because its long-term spectrum matches
    music far better than white does, so a style whose character is
    frequency-dependent gets weighted the way programme material would weight
    it. The coefficients were fitted at 44.1 kHz; at higher rates the tilt
    simply extends, which is fine — "pink-ish" is all the measurement needs. */
class PinkFilter
{
public:
    float process (float white) noexcept
    {
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        return b0 + b1 + b2 + white * 0.1848f;
    }

private:
    float b0 { 0.0f }, b1 { 0.0f }, b2 { 0.0f };
};

/** Build the one stimulus every measurement reuses: DC-free pink-ish noise
    normalised to `kStimulusLevelDb` RMS. Off the audio thread; allocates. */
std::vector<float> makeStimulus()
{
    std::vector<float> signal (static_cast<std::size_t> (kStimulusSamples), 0.0f);

    Xorshift32 rng { kNoiseSeed };
    PinkFilter pink;

    // Run the pink filter in first, or the opening samples would be a ramp out
    // of silence rather than representative noise.
    for (int i = 0; i < 4096; ++i)
        (void) pink.process (rng.nextBipolar());

    double mean = 0.0;

    for (int i = 0; i < kStimulusSamples; ++i)
    {
        const float v = pink.process (rng.nextBipolar());
        signal[static_cast<std::size_t> (i)] = v;
        mean += static_cast<double> (v);
    }

    // Strip the residual DC the pink filter's long time constants leave behind:
    // asymmetric and rectifying styles would otherwise be measured against an
    // offset that no real signal has.
    mean /= static_cast<double> (kStimulusSamples);

    double sumSquares = 0.0;

    for (auto& v : signal)
    {
        v -= static_cast<float> (mean);
        sumSquares += static_cast<double> (v) * static_cast<double> (v);
    }

    const double rms = std::sqrt (sumSquares / static_cast<double> (kStimulusSamples));
    const double target = static_cast<double> (juce::Decibels::decibelsToGain (kStimulusLevelDb));
    const float scale = (rms > kSilenceFloor) ? static_cast<float> (target / rms) : 1.0f;

    for (auto& v : signal)
        v *= scale;

    return signal;
}

/** RMS of `data` over [start, start + count), in double so that a 4096-sample
    sum cannot lose precision. Returns 0 if anything in the window is not
    finite — the caller treats that as an unmeasurable style. */
double windowRms (const float* data, int start, int count) noexcept
{
    if (data == nullptr || count <= 0)
        return 0.0;

    double acc = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const double v = static_cast<double> (data[static_cast<std::size_t> (start + i)]);

        if (! std::isfinite (v))
            return 0.0;

        acc += v * v;
    }

    return std::sqrt (acc / static_cast<double> (count));
}

/** Drive an isolated instance of one style with the stimulus and report the
    gain, in dB, that restores the input RMS. Off the audio thread. */
float measureCompensationDb (StyleID id, float driveDb, double rate,
                             const std::vector<float>& stimulus,
                             std::vector<float>& scratch,
                             double inputRms)
{
    auto style = createSaturationStyle (id);

    if (style == nullptr)                       // the factory promises non-null
        return 0.0f;                            // but never trust it silently

    style->prepare (rate, kStimulusSamples, 1);
    style->reset();

    std::copy (stimulus.begin(), stimulus.end(), scratch.begin());

    StyleParams params;
    params.sampleRate = rate;
    params.driveDb    = driveDb;
    params.driveLin   = juce::Decibels::decibelsToGain (driveDb);
    params.amount01   = juce::jlimit (0.0f, 1.0f, driveDb / StyleCalibrator::kMaxDriveDb);

    float* channels[1] = { scratch.data() };
    style->process (channels, 1, kStimulusSamples, params);

    const double outRms = windowRms (scratch.data(), kWarmupSamples, kMeasureSamples);

    // Silent, or NaN/Inf somewhere in the window: there is nothing to match, so
    // leave the level alone instead of inventing a correction.
    if (! (outRms > kSilenceFloor) || ! (inputRms > kSilenceFloor))
        return 0.0f;

    const double db = 20.0 * std::log10 (inputRms / outRms);

    if (! std::isfinite (db))
        return 0.0f;

    return juce::jlimit (-kMaxCompensationDb, kMaxCompensationDb, static_cast<float> (db));
}
} // namespace

//==============================================================================
StyleCalibrator::StyleCalibrator (double oversampledSampleRate)
{
    // Oversampled rates run from 44.1 k (no oversampling) to 192 k x 16; the
    // clamp only guards against a host reporting nonsense during prepare.
    const double rate = juce::jlimit (8000.0, 1.0e7, std::isfinite (oversampledSampleRate)
                                                         ? oversampledSampleRate
                                                         : 44100.0);

    const std::vector<float> stimulus = makeStimulus();
    const double inputRms = windowRms (stimulus.data(), kWarmupSamples, kMeasureSamples);

    std::vector<float> scratch (static_cast<std::size_t> (kStimulusSamples), 0.0f);

    for (int s = 0; s < kNumStyles; ++s)
    {
        const auto id = static_cast<StyleID> (s);

        for (int d = 0; d < kNumDrivePoints; ++d)
        {
            const float driveDb = juce::jmin (kMaxDriveDb, static_cast<float> (d) * kDriveStepDb);

            tableDb[static_cast<std::size_t> (s)][static_cast<std::size_t> (d)]
                = measureCompensationDb (id, driveDb, rate, stimulus, scratch, inputRms);
        }
    }
}

//==============================================================================
float StyleCalibrator::compensationDb (StyleID id, float driveDb) const noexcept
{
    // Realtime-safe: two clamps, one table lookup, one lerp. No allocation, no
    // lock, no virtual call, nothing that depends on the style's identity
    // beyond an integer index.
    const int styleIndex = juce::jlimit (0, kNumStyles - 1, static_cast<int> (id));

    // jlimit propagates NaN, so screen it out before clamping rather than after.
    const float safeDrive = juce::jlimit (0.0f, kMaxDriveDb,
                                          std::isfinite (driveDb) ? driveDb : 0.0f);

    const float pos = safeDrive * (1.0f / kDriveStepDb);
    const int lower = juce::jlimit (0, kNumDrivePoints - 1, static_cast<int> (pos));
    const int upper = juce::jmin (lower + 1, kNumDrivePoints - 1);
    const float frac = juce::jlimit (0.0f, 1.0f, pos - static_cast<float> (lower));

    const auto& row = tableDb[static_cast<std::size_t> (styleIndex)];
    const float a = row[static_cast<std::size_t> (lower)];
    const float b = row[static_cast<std::size_t> (upper)];

    return a + frac * (b - a);
}

float StyleCalibrator::compensationGain (StyleID id, float driveDb) const noexcept
{
    // Called once per control block per band, not per sample, so the exp10 in
    // decibelsToGain is far below the noise floor of the cost budget. The
    // default -100 dB "minus infinity" floor is left alone deliberately: the
    // table is clamped to -40 dB, and passing that as the floor would turn the
    // clamped entries into silence instead of into a 0.01x gain.
    return juce::Decibels::decibelsToGain (compensationDb (id, driveDb));
}

//==============================================================================
const StyleCalibrator& StyleCalibrator::getForSampleRate (double oversampledSampleRate)
{
    // NOT REALTIME-SAFE. This allocates, takes a lock and, on a cache miss,
    // runs several million samples of DSP. It must only ever be called from
    // prepareToPlay / prepare() on the message or prepare thread — NEVER from
    // processBlock or any other audio-thread code.
    //
    // The lock exists because an offline renderer or a test can prepare several
    // instances concurrently; the cache is shared process-wide because the
    // table depends on nothing but the rate, and all six bands plus every
    // plugin instance want the same one.
    static juce::CriticalSection cacheLock;
    static std::map<int, std::unique_ptr<StyleCalibrator>> cache;

    // Key on the rounded rate: hosts hand back rates that differ in the last
    // float bit between calls, and an exact-equality key would miss the cache
    // and rebuild the table every time.
    const double clamped = juce::jlimit (8000.0, 1.0e7, std::isfinite (oversampledSampleRate)
                                                            ? oversampledSampleRate
                                                            : 44100.0);
    const int key = static_cast<int> (std::lround (clamped));

    const juce::ScopedLock lock (cacheLock);

    auto entry = cache.find (key);

    if (entry == cache.end())
    {
        // Not make_unique: the constructor is private, and only members of this
        // class may call it.
        std::unique_ptr<StyleCalibrator> built (new StyleCalibrator (static_cast<double> (key)));
        entry = cache.emplace (key, std::move (built)).first;
    }

    jassert (entry->second != nullptr);
    return *entry->second;
}
} // namespace ember
