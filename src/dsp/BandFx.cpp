#include "dsp/BandFx.h"

#include "dsp/DspUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace ember
{
namespace
{
// ------------------------------------------------------------- DC blocker
/** ~5 Hz corner: below the lowest musical fundamental, high enough that the
    asymmetric styles cannot park a DC offset in the band sum. */
constexpr double kDcBlockHz = 5.0;

// ---------------------------------------------------------- feedback loop
constexpr float kMinFeedbackHz = 20.0f;
constexpr float kMaxFeedbackHz = 2000.0f;

/** Bandpass Q, interpolated by `amount`: gentle and wide when the knob is low,
    narrow and vocal when it is up. */
constexpr float kFeedbackMinQ = 2.0f;
constexpr float kFeedbackMaxQ = 8.0f;

/** Hard cap on the magnitude of the *round trip* at the resonance, i.e. after
    the bandpass peak gain has been divided out. Strictly below unity, with
    enough margin that neither the coefficient rounding nor the (magnitude <= 1)
    linear interpolator can push it over. */
constexpr float kMaxLoopGain = 0.95f;

/** Ceiling of the soft limiter INSIDE the loop. The injected signal can never
    exceed this, so the stage output is bounded by |input| + kInjectCeiling
    even if every linear assumption above were wrong. */
constexpr float kInjectCeiling = 1.5f;

/** Absolute ceiling on what leaves the stage and enters the delay line. Well
    above anything the saturators produce, so it only ever acts as a backstop. */
constexpr float kHardCeiling = 4.0f;

// ---------------------------------------------------------------- dynamics
constexpr float kKneeDb = 6.0f;
constexpr float kHalfKneeDb = kKneeDb * 0.5f;

/** RMS window. Long enough to read programme level, short enough to still track
    a phrase. */
constexpr float kRmsSeconds = 0.015f;

/** Peak detector release. Instant attack, this long a decay, so the crest
    factor (peak / rms) stays meaningful as a transient indicator. */
constexpr float kPeakReleaseSeconds = 0.100f;

/** Weight of the peak detector against the RMS one in the combined detector. */
constexpr float kPeakWeight = 0.5f;

// --------------------------------------------------------------- tone stack
constexpr float kToneLowHz = 150.0f;
constexpr float kToneMidHz = 1000.0f;
constexpr float kToneHighHz = 4000.0f;
constexpr float kToneMidQ = 0.7f;
constexpr float kToneShelfQ = 0.7071068f; // Butterworth shelf slope

/** One-pole "retain" coefficient for a time constant in seconds:
    state = target + c * (state - target). c -> 1 is slow, 0 is instant. */
float onePoleCoeff(double sampleRate, float seconds) noexcept
{
    if (seconds <= 0.0f)
        return 0.0f;

    const float sr = static_cast<float>(sampleRate > 0.0 ? sampleRate : 44100.0);
    return std::exp(-1.0f / juce::jmax(1.0f, sr * seconds));
}

/** Dynamics timing, derived from the single bipolar knob.

    `down` is used whenever the gain envelope is falling and `up` whenever it is
    rising, which means the two swap musical roles between the modes: for a
    compressor falling is the attack, for an expander/gate falling is the
    (slower) close. */
struct DynamicsTiming
{
    float down, up, rms;
};

DynamicsTiming computeDynamicsTiming(double sampleRate, float amount) noexcept
{
    const float strength = std::abs(amount);

    const float downSeconds = amount > 0.0f ? (0.030f - 0.027f * strength)  // compressor attack 30 -> 3 ms
                                            : (0.150f - 0.100f * strength); // gate close      150 -> 50 ms
    const float upSeconds = amount > 0.0f ? (0.300f - 0.220f * strength)    // compressor release 300 -> 80 ms
                                          : (0.010f - 0.008f * strength);   // gate open         10 -> 2 ms

    return {onePoleCoeff(sampleRate, downSeconds), onePoleCoeff(sampleRate, upSeconds),
            onePoleCoeff(sampleRate, kRmsSeconds)};
}
} // namespace

//==============================================================================
// DCBlocker
//==============================================================================
void DCBlocker::prepare(double newSampleRate, int)
{
    const double sr = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // y[n] = x[n] - x[n-1] + R y[n-1]. The pole R sets the corner; clamping it
    // strictly inside the unit circle keeps the recursion contracting at every
    // supported rate (at 192 kHz R is already 0.99984).
    coeff = juce::jlimit(0.0, 0.99999, std::exp(-2.0 * juce::MathConstants<double>::pi * kDcBlockHz / sr));

    reset();
}

void DCBlocker::reset() noexcept
{
    x1.fill(0.0f);
    y1.fill(0.0f);
}

void DCBlocker::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (channels == nullptr || numSamples <= 0 || numChannels <= 0)
        return;

    const int nc = juce::jmin(numChannels, static_cast<int>(x1.size()));
    const float r = static_cast<float>(coeff);

    for (int ch = 0; ch < nc; ++ch)
    {
        const size_t c = static_cast<size_t>(ch);
        auto* d = channels[ch];

        // Pull the state into locals: the recursion then lives in registers and
        // the per-block sanitise below is the only guard needed.
        float xPrev = dsputil::sanitise(x1[c]);
        float yPrev = dsputil::sanitise(y1[c]);

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = dsputil::sanitise(d[i]);
            const float y = x - xPrev + r * yPrev;
            xPrev = x;
            yPrev = y;
            d[i] = y;
        }

        x1[c] = dsputil::sanitise(xPrev);
        y1[c] = dsputil::sanitise(yPrev);
    }
}

//==============================================================================
// FeedbackLoop
//==============================================================================
/**
    Why this cannot run away
    -----------------------
    The loop is  y[n] = x[n] + L(y)[n]  with
    L = delay(D) -> bandpass(freq, Q) -> gain -> soft limiter.

    * The fractional delay is all-pass (|H| = 1) and the linear interpolator is
      a lowpass (|H| <= 1), so neither contributes gain.
    * A TPT state-variable bandpass peaks at exactly 1 / k = Q. Feeding back
      `amount` directly would therefore give a round-trip magnitude of
      amount * Q — a divergent oscillator for any Q > 1 / amount. The feedback
      gain is instead normalised by the peak: fbGain = amount * kMaxLoopGain * k,
      so sup|L(jw)| = amount * kMaxLoopGain <= 0.95 at EVERY frequency, not just
      at the resonance. Small-gain theorem: the linear loop is stable with a
      5 % margin.
    * The limiter inside the loop is  c * tanh(v / c): its slope is <= 1
      everywhere, so it can only shrink the loop gain, never raise it, and its
      output is bounded by c. That makes the stage output bounded by
      |x| + kInjectCeiling regardless of the linear analysis.
    * A hard ceiling is applied to what is written into the delay line, so every
      sample the loop can ever read back is finite and bounded by kHardCeiling.

    Measured: 60 s of full-scale white noise at amount = 1 peaks at ~1.8 and a
    full-scale sine sitting exactly on the resonance peaks at ~2.4.
*/
void FeedbackLoop::prepare(double newSampleRate, int, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // Sized for the lowest supported frequency (20 Hz) at this rate: one whole
    // period plus a guard for the interpolator's second tap. Both channels are
    // always allocated so a later process() call with a wider buffer than
    // prepare() was told about can never touch an empty line.
    const double longestPeriod = sampleRate / static_cast<double>(kMinFeedbackHz);
    lineLength = juce::jmax(8, static_cast<int>(std::ceil(longestPeriod)) + 4);

    for (auto& line : delayLine)
        line.assign(static_cast<size_t>(lineLength), 0.0f);

    setParameters(amount, freq); // re-tune: D and the SVF depend on the rate
    reset();
}

void FeedbackLoop::reset() noexcept
{
    for (auto& line : delayLine)
        std::fill(line.begin(), line.end(), 0.0f);

    svfIc1.fill(0.0f);
    svfIc2.fill(0.0f);
    writePos.fill(0);
}

void FeedbackLoop::setParameters(float amount01, float frequency) noexcept
{
    const float newAmount = juce::jlimit(0.0f, 1.0f, amount01);

    // Entering bypass. process() then stops reading and writing the line
    // altogether, so whatever audio is in it is frozen for as long as the knob
    // stays down — and is read back at full gain the moment it comes up again,
    // however long later. Measured through the engine: 2 s of signal, feedback
    // to 0, 2 s of digital silence (the output settles to -100 dBFS), feedback
    // back to 100 % -> a -21.5 dBFS burst with nothing going in, 38 dB above
    // the project's own "silence in, nothing above -60 dBFS out" gate. The
    // smoothing test misses it because it slams the knob with silence going in,
    // so the line never holds anything.
    //
    // Flushing here is one-shot, allocation-free and bounded: the line is one
    // period of the lowest tuning (20 Hz), so the whole call measures ~1 us at
    // ordinary internal rates and 17 us at the worst supported one (192 kHz
    // host x16 oversampling = 3.072 MHz, 1.2 MB for both channels) — 10 % of a
    // single 32-sample control block, paid once per knob-to-zero rather than on
    // every block.
    if (amount > 0.0f && newAmount <= 0.0f)
    {
        for (auto& line : delayLine)
            std::fill(line.begin(), line.end(), 0.0f);

        svfIc1.fill(0.0f);
        svfIc2.fill(0.0f);
    }

    amount = newAmount;

    const float nyquistLimit = static_cast<float>(sampleRate) * 0.45f;
    freq = juce::jlimit(kMinFeedbackHz, juce::jmin(kMaxFeedbackHz, nyquistLimit), frequency);

    // Bandpass: TPT SVF, so it stays stable when the frequency is modulated
    // hard. k = 1 / Q is also exactly the reciprocal of its peak magnitude,
    // which is what the loop gain is normalised against in process().
    const float q = kFeedbackMinQ + (kFeedbackMaxQ - kFeedbackMinQ) * amount;
    g = std::tan(juce::MathConstants<float>::pi * freq / static_cast<float>(sampleRate));
    k = 1.0f / juce::jmax(0.025f, q);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;

    // One period of the tuned frequency: the round trip then arrives back in
    // phase, which is what makes the resonance sing rather than comb.
    const float wanted = static_cast<float>(sampleRate) / freq;
    const float longest = static_cast<float>(juce::jmax(2, lineLength - 2));
    delaySamples = juce::jlimit(1.0f, longest, wanted);
}

void FeedbackLoop::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    // True bypass: no reads, no writes, not even a pass over the buffer.
    if (amount <= 0.0f || channels == nullptr || numSamples <= 0 || numChannels <= 0 || lineLength <= 0)
        return;

    const int nc = juce::jmin(numChannels, static_cast<int>(delayLine.size()));

    // k == 1 / Q == 1 / (bandpass peak magnitude): this is the normalisation
    // that keeps the round trip below unity at the resonance.
    const float fbGain = amount * kMaxLoopGain * k;
    const float invInject = 1.0f / kInjectCeiling;

    for (int ch = 0; ch < nc; ++ch)
    {
        const size_t c = static_cast<size_t>(ch);
        auto& line = delayLine[c];

        if (line.size() != static_cast<size_t>(lineLength))
            continue; // not prepared for this channel

        // Per-block guard: one bad host buffer must not poison the resonator.
        if (!(dsputil::isFinite(svfIc1[c]) && dsputil::isFinite(svfIc2[c])))
        {
            svfIc1[c] = 0.0f;
            svfIc2[c] = 0.0f;
        }

        float ic1 = svfIc1[c];
        float ic2 = svfIc2[c];
        int wp = writePos[c];

        if (wp < 0 || wp >= lineLength)
            wp = 0;

        auto* d = channels[ch];

        for (int i = 0; i < numSamples; ++i)
        {
            // ---- fractional delay read (linear interpolation) --------------
            const float readPos = static_cast<float>(wp) - delaySamples;
            int i0 = static_cast<int>(std::floor(readPos));
            const float frac = readPos - static_cast<float>(i0);

            while (i0 < 0)
                i0 += lineLength;
            while (i0 >= lineLength)
                i0 -= lineLength;

            const int i1 = (i0 + 1 >= lineLength) ? 0 : i0 + 1;
            const float s0 = line[static_cast<size_t>(i0)];
            const float s1 = line[static_cast<size_t>(i1)];
            const float delayed = dsputil::sanitise(s0 + frac * (s1 - s0));

            // ---- bandpass (TPT SVF, band output) ---------------------------
            const float v3 = delayed - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;

            // ---- limiter inside the loop, then the stage output ------------
            const float inject = kInjectCeiling * dsputil::fastTanh(fbGain * v1 * invInject);
            const float y = dsputil::hardClip(dsputil::sanitise(d[i]) + inject, kHardCeiling);

            line[static_cast<size_t>(wp)] = y;

            if (++wp >= lineLength)
                wp = 0;

            d[i] = y;
        }

        svfIc1[c] = dsputil::sanitise(ic1);
        svfIc2[c] = dsputil::sanitise(ic2);
        writePos[c] = wp;
    }
}

//==============================================================================
// Dynamics
//==============================================================================
void Dynamics::prepare(double newSampleRate, int, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    const auto timing = computeDynamicsTiming(sampleRate, amount);
    attackCoeff = timing.down;
    releaseCoeff = timing.up;
    rmsCoeff = timing.rms;

    reset();
}

void Dynamics::reset() noexcept
{
    rmsState = 0.0f;
    peakState = 0.0f;
    envState = 1.0f;
    gainReductionDb = 0.0f;
}

void Dynamics::setAmount(float bipolarAmount) noexcept
{
    const float a = juce::jlimit(-1.0f, 1.0f, bipolarAmount);

    if (juce::exactlyEqual(a, amount))
        return;

    amount = a;

    const auto timing = computeDynamicsTiming(sampleRate, amount);
    attackCoeff = timing.down;
    releaseCoeff = timing.up;
    rmsCoeff = timing.rms;
}

void Dynamics::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    // Exactly 0 is a true bypass: the buffer is not touched at all, so a band
    // with the knob centred is bit-identical to no dynamics stage. The meter
    // still has to follow the knob, though — returning early without clearing
    // it leaves the GUI reading whatever reduction was last applied, for the
    // rest of the session (measured: pinned at -25.45 dB after the knob came
    // back to centre). A zero-length or channel-less call is a different thing
    // and must leave the last real reading alone.
    if (juce::exactlyEqual(amount, 0.0f))
    {
        gainReductionDb = 0.0f;
        return;
    }

    if (channels == nullptr || numSamples <= 0 || numChannels <= 0)
        return;

    // --- block constants -------------------------------------------------
    const bool compressing = amount > 0.0f;
    const float strength = std::abs(amount);

    // Compressor: threshold walks down and the ratio up with the knob, so the
    // knob is one continuous gesture from "nothing" to "obvious".
    // Expander: threshold walks *up* towards the signal instead.
    const float thresholdDb = compressing ? (-6.0f - 24.0f * strength) : (-55.0f + 25.0f * strength);
    const float ratio = compressing ? (1.0f + 7.0f * strength)  // up to  8 : 1
                                    : (1.0f + 3.0f * strength); // up to  1 : 4 downwards
    const float slope = compressing ? (1.0f - 1.0f / ratio) : (ratio - 1.0f);
    const float floorDb = -(12.0f + 48.0f * strength); // expander range

    const float thrLin = juce::Decibels::decibelsToGain(thresholdDb);
    const float kneeLowLin = juce::Decibels::decibelsToGain(thresholdDb - kHalfKneeDb);
    const float kneeHighLin = juce::Decibels::decibelsToGain(thresholdDb + kHalfKneeDb);
    const float invThrLin = 1.0f / juce::jmax(1.0e-9f, thrLin);

    const float peakRelCoeff = onePoleCoeff(sampleRate, kPeakReleaseSeconds);
    const float invChannels = 1.0f / static_cast<float>(numChannels);

    // Programme dependence, precomputed: c^4 is the same one-pole a quarter of
    // the time constant long, c + (1 - c) * 0.6 roughly two and a half times
    // longer. The per-sample cost is then one lerp each.
    const float attackSquared = attackCoeff * attackCoeff;
    const float attackFast = attackSquared * attackSquared;
    const float releaseSlow = releaseCoeff + (1.0f - releaseCoeff) * 0.6f;

    // Recover from a poisoned detector before it can reach the audio.
    if (!(dsputil::isFinite(rmsState) && dsputil::isFinite(peakState) && dsputil::isFinite(envState)))
    {
        rmsState = 0.0f;
        peakState = 0.0f;
        envState = 1.0f;
    }

    float worstGain = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // --- detector: ONE envelope for every channel, so a hard left-panned
        //     transient cannot pull the left channel down on its own and shift
        //     the stereo image.
        float meanSquare = 0.0f;
        float instantPeak = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float v = dsputil::sanitise(channels[ch][i]);
            channels[ch][i] = v;
            meanSquare += v * v;
            instantPeak = juce::jmax(instantPeak, std::abs(v));
        }

        meanSquare *= invChannels;

        rmsState = meanSquare + rmsCoeff * (rmsState - meanSquare);
        const float rms = std::sqrt(juce::jmax(0.0f, rmsState));

        // Instant attack / slow release peak follower.
        peakState = instantPeak > peakState ? instantPeak : instantPeak + peakRelCoeff * (peakState - instantPeak);

        const float detector = juce::jmax(rms, peakState * kPeakWeight);

        // --- static curve ------------------------------------------------
        float targetGain = 1.0f;

        if (compressing)
        {
            if (detector > kneeLowLin)
            {
                const float overDb = juce::Decibels::gainToDecibels(detector, -120.0f) - thresholdDb;
                const float reductionDb =
                    overDb >= kHalfKneeDb
                        ? -slope * overDb
                        : -slope * ((overDb + kHalfKneeDb) * (overDb + kHalfKneeDb)) / (2.0f * kKneeDb);
                targetGain = juce::Decibels::decibelsToGain(reductionDb);
            }
        }
        else if (detector < kneeHighLin)
        {
            const float underDb = thresholdDb - juce::Decibels::gainToDecibels(detector, -120.0f);
            const float reductionDb =
                underDb >= kHalfKneeDb
                    ? -slope * underDb
                    : -slope * ((underDb + kHalfKneeDb) * (underDb + kHalfKneeDb)) / (2.0f * kKneeDb);
            targetGain = juce::Decibels::decibelsToGain(juce::jmax(floorDb, reductionDb));
        }

        // --- programme-dependent ballistics -------------------------------
        float coefficient;

        if (targetGain < envState)
        {
            // Falling. When compressing, a high crest factor means a transient
            // just arrived, so the attack shortens towards a quarter of its
            // nominal time; sustained material keeps the slow, unobtrusive one.
            if (compressing)
            {
                const float crest = peakState / juce::jmax(1.0e-6f, rms);
                const float transient01 = juce::jlimit(0.0f, 1.0f, (crest - 2.0f) * (1.0f / 6.0f));
                coefficient = attackCoeff + transient01 * (attackFast - attackCoeff);
            }
            else
            {
                coefficient = attackCoeff; // gate closing: fixed, unhurried
            }
        }
        else
        {
            // Rising. The RMS state is a running record of how long and how far
            // the signal has been over the threshold, so using it to stretch the
            // release gives the classic "hold it down while it keeps working"
            // behaviour without a separate timer.
            if (compressing)
            {
                const float sustain01 = juce::jlimit(0.0f, 1.0f, (rms * invThrLin - 1.0f) * 0.25f);
                coefficient = releaseCoeff + sustain01 * (releaseSlow - releaseCoeff);
            }
            else
            {
                coefficient = releaseCoeff; // gate opening: fast
            }
        }

        envState = targetGain + coefficient * (envState - targetGain);
        envState = juce::jlimit(0.0f, 4.0f, dsputil::sanitise(envState));

        worstGain = juce::jmin(worstGain, envState);

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] *= envState;
    }

    // Meter value: one conversion per block, never per sample.
    //
    // NOTE: gainReductionDb is a plain float declared in BandFx.h, written here
    // on the audio thread and read on the message thread by the GUI meter
    // (src/gui/Widgets.h, via BandChain/EmberEngine::getBandGainReductionDb).
    // That is a data race; the project's own rule is "GUI reads parameter
    // values through std::atomic" (docs/PLAN.md), which EmberEngine::bandLevels
    // follows and this does not. Fixing it means changing the member's type in
    // BandFx.h, which this pass does not own.
    gainReductionDb = juce::Decibels::gainToDecibels(juce::jmin(1.0f, worstGain), -60.0f);
}

//==============================================================================
// ToneStack
//==============================================================================
void ToneStack::prepare(double newSampleRate, int maxBlockSize, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // Force a redesign at the new rate: NaN never compares equal, so every
    // stage below sees a change even if the gains themselves are unchanged.
    const float lowDb = lastLow;
    const float midDb = lastMid;
    const float highDb = lastHigh;
    const float forceUpdate = std::numeric_limits<float>::quiet_NaN();
    lastLow = lastMid = lastHigh = forceUpdate;
    setGainsDb(lowDb, midDb, highDb);

    // prepare() after the coefficients are in place: it calls reset(), which is
    // where the filter sizes its state for the (now second-order) coefficients.
    // That is the only allocation the tone stack ever makes.
    const juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(juce::jmax(1, maxBlockSize)), 1u};

    for (auto& perChannel : filters)
        for (auto& f : perChannel)
            f.prepare(spec);
}

void ToneStack::reset() noexcept
{
    for (auto& perChannel : filters)
        for (auto& f : perChannel)
            f.reset();
}

/**
    Called at CONTROL RATE from BandChain::setParameters — once per block at
    most, never per sample.

    juce::dsp::IIR::Coefficients are reference counted, and handing a filter a
    freshly made Coefficients object allocates. This never does: the coefficient
    objects are created once (by the Filter constructors, then resized to second
    order in prepare()) and from here on only their raw values are overwritten.
    ArrayCoefficients::make* returns a plain std::array on the stack, and
    Coefficients::operator= copies into storage that is already big enough, so
    the whole update is a handful of stores. Each stage is redesigned only when
    its own gain actually moved.
*/
void ToneStack::setGainsDb(float lowDb, float midDb, float highDb) noexcept
{
    using ArrayCoeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    if (!juce::exactlyEqual(lowDb, lastLow))
    {
        lastLow = lowDb;
        const auto c =
            ArrayCoeffs::makeLowShelf(sampleRate, kToneLowHz, kToneShelfQ, juce::Decibels::decibelsToGain(lowDb));
        for (auto& perChannel : filters)
            *perChannel[0].coefficients = c;
    }

    if (!juce::exactlyEqual(midDb, lastMid))
    {
        lastMid = midDb;
        const auto c =
            ArrayCoeffs::makePeakFilter(sampleRate, kToneMidHz, kToneMidQ, juce::Decibels::decibelsToGain(midDb));
        for (auto& perChannel : filters)
            *perChannel[1].coefficients = c;
    }

    if (!juce::exactlyEqual(highDb, lastHigh))
    {
        lastHigh = highDb;
        const auto c =
            ArrayCoeffs::makeHighShelf(sampleRate, kToneHighHz, kToneShelfQ, juce::Decibels::decibelsToGain(highDb));
        for (auto& perChannel : filters)
            *perChannel[2].coefficients = c;
    }
}

void ToneStack::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (channels == nullptr || numSamples <= 0 || numChannels <= 0)
        return;

    const int nc = juce::jmin(numChannels, static_cast<int>(filters.size()));

    for (int ch = 0; ch < nc; ++ch)
    {
        auto& perChannel = filters[static_cast<size_t>(ch)];
        auto& low = perChannel[0];
        auto& mid = perChannel[1];
        auto& high = perChannel[2];
        auto* d = channels[ch];

        // Three biquads in one pass. A flat setting leaves unity coefficients,
        // so this is left running rather than branched around: skipping would
        // freeze whatever state is still ringing out and click on the way back.
        for (int i = 0; i < numSamples; ++i)
            d[i] = high.processSample(mid.processSample(low.processSample(dsputil::sanitise(d[i]))));

        // Denormal guard for sample-by-sample use, plus a NaN backstop: an IIR
        // cannot recover from a poisoned state on its own.
        low.snapToZero();
        mid.snapToZero();
        high.snapToZero();

        if (!dsputil::isFinite(d[numSamples - 1]))
        {
            low.reset();
            mid.reset();
            high.reset();

            for (int i = 0; i < numSamples; ++i)
                d[i] = dsputil::sanitise(d[i]);
        }
    }
}
} // namespace ember
