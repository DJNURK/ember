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
    measured mid-transient.

    Settling is a property of TIME, not of a sample count, so this cannot be a
    constant: the table is built at the OVERSAMPLED rate, which spans 44.1 kHz
    (no oversampling) to 3.072 MHz (192 kHz host at 16x). A fixed 4096 samples
    is 92.9 ms at 44.1 kHz but only 1.33 ms at 3.072 MHz, and a style whose
    state moves in tens of milliseconds is then measured entirely inside its own
    attack transient. Measured on this repository's styles, that is worth up to
    5.7 dB: Warm Tape's magnetisation memory at 40 dB drive wants +6.87 dB of
    compensation at 3.072 MHz and a 4096-sample warm-up reports +1.22 dB.
    Lengthening only the warm-up converges monotonically onto the long-run
    value (+1.22, +2.13, +3.38, +4.86, +6.11, +6.71, +6.88 dB as it doubles),
    which is what identifies the warm-up rather than the window as the cause.

    So: hold the warm-up DURATION that 96 kHz got, floored at the historical
    4096 samples and capped so the build cost stays bounded. Rates at or below
    96 kHz are unchanged, bit for bit. */
constexpr int kMinWarmupSamples = 4096;
constexpr double kWarmupSeconds = 4096.0 / 96000.0; // 42.7 ms

/** Ceiling on the warm-up, purely to bound build time. 131072 samples is
    exactly `kWarmupSeconds` at 3.072 MHz — the highest oversampled rate the
    plugin can reach — so within the shipped configuration space the cap never
    actually binds; it only stops an absurd `oversampledSampleRate` from turning
    prepare into an unbounded amount of work. */
constexpr int kMaxWarmupSamples = 131072;

/** Measurement window, in samples. Long enough that the estimate no longer
    depends much on which stretch of noise it saw — halving it to 4096 doubles
    the spread of the gain match across independent test signals, from 0.29 dB
    to 0.66 dB.

    Unlike the warm-up this one is legitimately a sample count rather than a
    duration: the stimulus is flat noise, so 32768 samples carry the same number
    of independent observations whatever the rate. 32768 is where the
    measurement stops moving, which makes the table a property of the style
    rather than of one noise burst, and is what lets tests/test_styles.cpp
    verify it with an independently seeded signal.

    The one style this does not fully serve is Decimate, whose sample-and-hold
    collapses the window to a handful of independent values at high drive: it
    scatters by about 1 dB at 96 kHz and 3.8 dB at 3.072 MHz, and only a window
    8x longer brings that under 0.5 dB. Buying it would multiply an already
    ~0.3 s build by ten, so it is left as a documented limit of the estimator
    rather than paid for by every other style. */
constexpr int kMeasureSamples = 32768;

/** Warm-up for one oversampled rate: `kWarmupSeconds` of settling time, never
    shorter than the historical floor and never longer than the cost cap. */
int warmupSamplesForRate(double rate) noexcept
{
    const double byDuration = std::ceil(rate * kWarmupSeconds);

    return static_cast<int>(
        juce::jlimit(static_cast<double>(kMinWarmupSamples), static_cast<double>(kMaxWarmupSamples),
                     std::isfinite(byDuration) ? byDuration : static_cast<double>(kMinWarmupSamples)));
}

/** Nominal operating level (-18 dBFS RMS, the usual alignment level). Gain
    compensation for a nonlinearity is only meaningful at a stated input level;
    this is the level the table is calibrated for. Styles whose output level is
    strongly non-monotonic in input level — Foldback above all, where the fold
    count changes with amplitude — are only matched near this level, which is
    inherent to the shape and not something a table can fix. */
constexpr float kStimulusLevelDb = -18.0f;

/** A measured compensation outside this range means the style did something
    pathological at that drive; clamping keeps one bad measurement from turning
    into a +200 dB multiplier on the audio thread. */
constexpr float kMaxCompensationDb = 40.0f;

/** Output RMS at or below this counts as "silent" — unmeasurable, so the entry
    falls back to unity rather than to a huge boost. */
constexpr double kSilenceFloor = 1.0e-7;

/** dB between adjacent table entries (1.0 for the 0..40 dB / 41 point table). */
constexpr float kDriveStepDb = StyleCalibrator::kMaxDriveDb / static_cast<float>(StyleCalibrator::kNumDrivePoints - 1);

/** Marsaglia xorshift32. Deterministic, no library RNG involved: std::mt19937
    would also be portable but the distribution adaptors are not specified
    bit-for-bit across implementations, and this table must be. */
class Xorshift32
{
public:
    explicit Xorshift32(std::uint32_t seed) noexcept : state(seed != 0u ? seed : 1u) {}

    std::uint32_t nextBits() noexcept
    {
        state = static_cast<std::uint32_t>(state ^ (state << 13));
        state = static_cast<std::uint32_t>(state ^ (state >> 17));
        state = static_cast<std::uint32_t>(state ^ (state << 5));
        return state;
    }

    /** Uniform in [-1, 1). */
    float nextBipolar() noexcept { return static_cast<float>(static_cast<std::int32_t>(nextBits())) / 2147483648.0f; }

private:
    std::uint32_t state;
};

/** Build the one stimulus every measurement reuses: DC-free flat-spectrum noise
    normalised to `kStimulusLevelDb` RMS. Off the audio thread; allocates.

    A note on why this is flat and not pink. Pink matches the long-term spectrum
    of programme material, so it is the better stimulus in the abstract — but
    the table is built at the OVERSAMPLED rate, where pink puts essentially all
    of its energy in the bottom fraction of the band and the measurement stops
    seeing what a style does above a few kHz. That is not a cosmetic difference:
    measured on this repository's styles at 96 kHz, calibrating on pink instead
    of flat noise moves the required compensation by 2.6 dB for the ADAA shapers
    (Hard Clip, Foldback — first-order ADAA is a two-point average, which costs
    3 dB on a flat spectrum and nothing on a pink one) and by 3.2 to 4.7 dB for
    the amp styles, whose post-lowpass sits at 7.5 to 9 kHz. Both the spec's
    "+/-1 dB between styles at 0 dB drive" and the unit tests that check it
    judge loudness on full-band noise, and pink-calibrated entries miss that
    window by up to 4.7 dB.

    Flat is also the only assumption-free choice: the calibrator is handed one
    number, the oversampled rate, and cannot tell 96 kHz of host rate with no
    oversampling (where the band really is full of signal up to Nyquist) from
    48 kHz at 2x (where it is not). Weighting the whole band evenly measures the
    style over its entire operating range rather than over a guess about how
    much of that range the host will actually excite. */
std::vector<float> makeStimulus(int numSamples)
{
    std::vector<float> signal(static_cast<std::size_t>(juce::jmax(1, numSamples)), 0.0f);

    Xorshift32 rng{kNoiseSeed};

    double mean = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float v = rng.nextBipolar();
        signal[static_cast<std::size_t>(i)] = v;
        mean += static_cast<double>(v);
    }

    // Strip the residual DC of a finite noise burst: asymmetric and rectifying
    // styles would otherwise be measured against an offset no real signal has.
    mean /= static_cast<double>(numSamples);

    double sumSquares = 0.0;

    for (auto& v : signal)
    {
        v -= static_cast<float>(mean);
        sumSquares += static_cast<double>(v) * static_cast<double>(v);
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(numSamples));
    const double target = static_cast<double>(juce::Decibels::decibelsToGain(kStimulusLevelDb));
    const float scale = (rms > kSilenceFloor) ? static_cast<float>(target / rms) : 1.0f;

    for (auto& v : signal)
        v *= scale;

    return signal;
}

/** RMS of `data` over [start, start + count), accumulated in double so that a
    several-thousand-sample sum cannot lose precision. Returns 0 if anything in
    the window is not finite — the caller treats that as an unmeasurable style. */
double windowRms(const float* data, int start, int count) noexcept
{
    if (data == nullptr || count <= 0)
        return 0.0;

    double acc = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const double v = static_cast<double>(data[static_cast<std::size_t>(start + i)]);

        if (!std::isfinite(v))
            return 0.0;

        acc += v * v;
    }

    return std::sqrt(acc / static_cast<double>(count));
}

/** Drive an isolated instance of one style with the stimulus and report the
    gain, in dB, that restores the input RMS. Off the audio thread. */
float measureCompensationDb(StyleID id, float driveDb, double rate, const std::vector<float>& stimulus,
                            std::vector<float>& scratch, int warmupSamples, double inputRms)
{
    auto style = createSaturationStyle(id);

    if (style == nullptr) // the factory promises non-null
        return 0.0f;      // but never trust it silently

    const int stimulusSamples = static_cast<int>(stimulus.size());

    style->prepare(rate, stimulusSamples, 1);
    style->reset();

    std::copy(stimulus.begin(), stimulus.end(), scratch.begin());

    StyleParams params;
    params.sampleRate = rate;
    params.driveDb = driveDb;
    params.driveLin = juce::Decibels::decibelsToGain(driveDb);
    params.amount01 = juce::jlimit(0.0f, 1.0f, driveDb / StyleCalibrator::kMaxDriveDb);

    float* channels[1] = {scratch.data()};
    style->process(channels, 1, stimulusSamples, params);

    const double outRms = windowRms(scratch.data(), warmupSamples, kMeasureSamples);

    // Silent, or NaN/Inf somewhere in the window: there is nothing to match, so
    // leave the level alone instead of inventing a correction.
    if (!(outRms > kSilenceFloor) || !(inputRms > kSilenceFloor))
        return 0.0f;

    const double db = 20.0 * std::log10(inputRms / outRms);

    if (!std::isfinite(db))
        return 0.0f;

    return juce::jlimit(-kMaxCompensationDb, kMaxCompensationDb, static_cast<float>(db));
}
} // namespace

//==============================================================================
StyleCalibrator::StyleCalibrator(double oversampledSampleRate)
{
    // Oversampled rates run from 44.1 k (no oversampling) to 192 k x 16; the
    // clamp only guards against a host reporting nonsense during prepare.
    const double rate =
        juce::jlimit(8000.0, 1.0e7, std::isfinite(oversampledSampleRate) ? oversampledSampleRate : 44100.0);

    // Warm-up scales with the rate so that every rate gets the same settling
    // TIME; the measurement window does not, because it is counted in
    // independent observations of flat noise. See the constants above.
    const int warmupSamples = warmupSamplesForRate(rate);
    const int stimulusSamples = warmupSamples + kMeasureSamples;

    const std::vector<float> stimulus = makeStimulus(stimulusSamples);
    const double inputRms = windowRms(stimulus.data(), warmupSamples, kMeasureSamples);

    std::vector<float> scratch(static_cast<std::size_t>(stimulusSamples), 0.0f);

    for (int s = 0; s < kNumStyles; ++s)
    {
        const auto id = static_cast<StyleID>(s);

        for (int d = 0; d < kNumDrivePoints; ++d)
        {
            const float driveDb = juce::jmin(kMaxDriveDb, static_cast<float>(d) * kDriveStepDb);

            tableDb[static_cast<std::size_t>(s)][static_cast<std::size_t>(d)] =
                measureCompensationDb(id, driveDb, rate, stimulus, scratch, warmupSamples, inputRms);
        }
    }
}

//==============================================================================
float StyleCalibrator::compensationDb(StyleID id, float driveDb) const noexcept
{
    // Realtime-safe: two clamps, one table lookup, one lerp. No allocation, no
    // lock, no virtual call, nothing that depends on the style's identity
    // beyond an integer index.
    const int styleIndex = juce::jlimit(0, kNumStyles - 1, static_cast<int>(id));

    // jlimit propagates NaN, so screen NaN out before clamping rather than
    // after. Only NaN: jlimit handles the infinities correctly on its own
    // (+Inf -> 40 dB, -Inf -> 0 dB), and folding them in with NaN would send
    // +Inf to the 0 dB entry instead — which is not the neutral answer it looks
    // like, because at high oversampled rates the 0 dB entry is a large BOOST
    // for the amp styles (+22 dB for Clean Amp at 3.072 MHz) where the 40 dB
    // entry is close to unity.
    const float safeDrive = juce::jlimit(0.0f, kMaxDriveDb, std::isnan(driveDb) ? 0.0f : driveDb);

    const float pos = safeDrive * (1.0f / kDriveStepDb);
    const int lower = juce::jlimit(0, kNumDrivePoints - 1, static_cast<int>(pos));
    const int upper = juce::jmin(lower + 1, kNumDrivePoints - 1);
    const float frac = juce::jlimit(0.0f, 1.0f, pos - static_cast<float>(lower));

    const auto& row = tableDb[static_cast<std::size_t>(styleIndex)];
    const float a = row[static_cast<std::size_t>(lower)];
    const float b = row[static_cast<std::size_t>(upper)];

    return a + frac * (b - a);
}

float StyleCalibrator::compensationGain(StyleID id, float driveDb) const noexcept
{
    // Called once per control block per band, not per sample, so the exp10 in
    // decibelsToGain is far below the noise floor of the cost budget. The
    // default -100 dB "minus infinity" floor is left alone deliberately: the
    // table is clamped to -40 dB, and passing that as the floor would turn the
    // clamped entries into silence instead of into a 0.01x gain.
    return juce::Decibels::decibelsToGain(compensationDb(id, driveDb));
}

//==============================================================================
const StyleCalibrator& StyleCalibrator::getForSampleRate(double oversampledSampleRate)
{
    // NOT REALTIME-SAFE. This allocates, takes a lock and, on a cache miss,
    // runs 19 x 41 = 779 measurements of (warm-up + 32768) samples each. That
    // is 28.7 M samples and about 0.28 s on an M-series core at 96 kHz, rising
    // with the warm-up to roughly 1.2 s at 3.072 MHz — NOT the "few ms" the
    // header claims. It must only ever be called from prepareToPlay /
    // prepare() on the message or prepare thread — NEVER from processBlock or
    // any other audio-thread code, and callers should expect prepare to block
    // for that long the first time a given oversampled rate is seen.
    //
    // The lock exists because an offline renderer or a test can prepare several
    // instances concurrently; the cache is shared process-wide because the
    // table depends on nothing but the rate, and all six bands plus every
    // plugin instance want the same one. It is deliberately held across the
    // build so two threads racing on the same new rate build the table once.
    //
    // std::map, not unordered_map or a vector: callers keep the returned
    // reference for the lifetime of their prepare (BandChain stores a pointer),
    // and a node-based container never moves an element when a later rate is
    // added.
    static juce::CriticalSection cacheLock;
    static std::map<int, std::unique_ptr<StyleCalibrator>> cache;

    // Key on the rounded rate: hosts hand back rates that differ in the last
    // float bit between calls, and an exact-equality key would miss the cache
    // and rebuild the table every time.
    const double clamped =
        juce::jlimit(8000.0, 1.0e7, std::isfinite(oversampledSampleRate) ? oversampledSampleRate : 44100.0);
    const int key = static_cast<int>(std::lround(clamped));

    const juce::ScopedLock lock(cacheLock);

    auto entry = cache.find(key);

    if (entry == cache.end())
    {
        // Not make_unique: the constructor is private, and only members of this
        // class may call it.
        std::unique_ptr<StyleCalibrator> built(new StyleCalibrator(static_cast<double>(key)));
        entry = cache.emplace(key, std::move(built)).first;
    }

    jassert(entry->second != nullptr);
    return *entry->second;
}
} // namespace ember
