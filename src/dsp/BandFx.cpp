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
// The tone bands' frequencies and the mid's Q are now per-band parameters;
// their former fixed values live on as ToneStack::Shape's defaults, so a preset
// written before they existed loads with exactly the old response.
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

namespace
{
/** The feedback stage for `NumCh` channels at once.

    Both channels share one delay tuning, so the read index, its fraction and
    the wrap are the same for both: computing them once instead of once per
    channel is free work removed. The rest is per-channel state, and running the
    two channels in the same iteration also gives the core two independent
    copies of the resonator's recurrence and of `fastTanh`'s division to overlap
    — the loop is short enough that a channel-at-a-time version leaves most of
    that latency exposed.

    Arithmetic per channel is unchanged, operation for operation, so the output
    is bit-identical to the one-channel-at-a-time form. */
template<int NumCh>
void feedbackKernel(float* const* d, float* const* lines, int numSamples, int lineLength, float delaySamples, float a1,
                    float a2, float a3, float fbGain, float invInject, float* ic1State, float* ic2State,
                    int& writePosition) noexcept
{
    float ic1[NumCh], ic2[NumCh];

    for (int k = 0; k < NumCh; ++k)
    {
        ic1[k] = ic1State[k];
        ic2[k] = ic2State[k];
    }

    int wp = writePosition;

    for (int i = 0; i < numSamples; ++i)
    {
        // ---- fractional delay read (linear interpolation) ------------------
        const float readPos = static_cast<float>(wp) - delaySamples;
        int i0 = static_cast<int>(std::floor(readPos));
        const float frac = readPos - static_cast<float>(i0);

        while (i0 < 0)
            i0 += lineLength;
        while (i0 >= lineLength)
            i0 -= lineLength;

        const int i1 = (i0 + 1 >= lineLength) ? 0 : i0 + 1;

        for (int k = 0; k < NumCh; ++k)
        {
            const float s0 = lines[k][i0];
            const float s1 = lines[k][i1];
            const float delayed = dsputil::sanitise(s0 + frac * (s1 - s0));

            // ---- bandpass (TPT SVF, band output) ---------------------------
            const float v3 = delayed - ic2[k];
            const float v1 = a1 * ic1[k] + a2 * v3;
            const float v2 = ic2[k] + a2 * ic1[k] + a3 * v3;
            ic1[k] = 2.0f * v1 - ic1[k];
            ic2[k] = 2.0f * v2 - ic2[k];

            // ---- limiter inside the loop, then the stage output ------------
            const float inject = kInjectCeiling * dsputil::fastTanh(fbGain * v1 * invInject);
            const float y = dsputil::hardClip(dsputil::sanitise(d[k][i]) + inject, kHardCeiling);

            lines[k][wp] = y;
            d[k][i] = y;
        }

        if (++wp >= lineLength)
            wp = 0;
    }

    for (int k = 0; k < NumCh; ++k)
    {
        ic1State[k] = dsputil::sanitise(ic1[k]);
        ic2State[k] = dsputil::sanitise(ic2[k]);
    }

    writePosition = wp;
}
} // namespace

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

    const size_t wanted = static_cast<size_t>(lineLength);
    const bool stereoReady = nc >= 2 && delayLine[0].size() == wanted && delayLine[1].size() == wanted;

    // Per-block guard: one bad host buffer must not poison the resonator.
    for (int ch = 0; ch < nc; ++ch)
    {
        const size_t c = static_cast<size_t>(ch);

        if (!(dsputil::isFinite(svfIc1[c]) && dsputil::isFinite(svfIc2[c])))
        {
            svfIc1[c] = 0.0f;
            svfIc2[c] = 0.0f;
        }

        if (writePos[c] < 0 || writePos[c] >= lineLength)
            writePos[c] = 0;
    }

    if (stereoReady)
    {
        // Both channels always advance the write position together, so one
        // position covers them; it is written back to both so the per-channel
        // fallback below can pick up wherever this left off.
        float* lines[2] = {delayLine[0].data(), delayLine[1].data()};
        int wp = writePos[0];
        feedbackKernel<2>(channels, lines, numSamples, lineLength, delaySamples, a1, a2, a3, fbGain, invInject,
                          svfIc1.data(), svfIc2.data(), wp);
        writePos[0] = wp;
        writePos[1] = wp;
        return;
    }

    for (int ch = 0; ch < nc; ++ch)
    {
        const size_t c = static_cast<size_t>(ch);

        if (delayLine[c].size() != wanted)
            continue; // not prepared for this channel

        float* lines[1] = {delayLine[c].data()};
        float* data[1] = {channels[ch]};
        feedbackKernel<1>(data, lines, numSamples, lineLength, delaySamples, a1, a2, a3, fbGain, invInject, &svfIc1[c],
                          &svfIc2[c], writePos[c]);
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
void ToneStack::prepare(double newSampleRate, int, int)
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

    reset();
}

void ToneStack::reset() noexcept
{
    for (auto& perChannel : state)
        for (auto& section : perChannel)
            section.fill(0.0f);
}

/** The normalisation `juce::dsp::IIR::Coefficients` applies when a six-element
    design is assigned to it: divide through by a0, and drop a0 itself. A zero
    a0 yields all-zero coefficients, exactly as JUCE's does. */
void ToneStack::setSection(size_t index, const std::array<float, 6>& design) noexcept
{
    const float a0 = design[3];
    const float a0Inv = juce::approximatelyEqual(a0, 0.0f) ? 0.0f : 1.0f / a0;

    auto& c = coeffs[index];
    c[0] = design[0] * a0Inv;
    c[1] = design[1] * a0Inv;
    c[2] = design[2] * a0Inv;
    c[3] = design[4] * a0Inv;
    c[4] = design[5] * a0Inv;
}

/**
    Called at CONTROL RATE from BandChain::setParameters - once per block at
    most, never per sample.

    Nothing here allocates: `ArrayCoefficients::make*` returns a plain
    std::array on the stack and the normalised result is stored into fixed
    members. Each section is redesigned only when its own gain actually moved.
*/
void ToneStack::setGainsDb(float lowDb, float midDb, float highDb) noexcept
{
    using ArrayCoeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    if (!juce::exactlyEqual(lowDb, lastLow))
    {
        lastLow = lowDb;
        setSection(
            0, ArrayCoeffs::makeLowShelf(sampleRate, shape.lowHz, kToneShelfQ, juce::Decibels::decibelsToGain(lowDb)));
    }

    if (!juce::exactlyEqual(midDb, lastMid))
    {
        lastMid = midDb;
        setSection(
            1, ArrayCoeffs::makePeakFilter(sampleRate, shape.midHz, shape.midQ, juce::Decibels::decibelsToGain(midDb)));
    }

    if (!juce::exactlyEqual(highDb, lastHigh))
    {
        lastHigh = highDb;

        setSection(2, ArrayCoeffs::makeHighShelf(sampleRate, shape.highHz, kToneShelfQ,
                                                 juce::Decibels::decibelsToGain(highDb)));
    }
}

void ToneStack::setShape(const Shape& newShape) noexcept
{
    using ArrayCoeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    // Frequencies are clamped below Nyquist: a node dragged to the top of a
    // band's range at 44.1 kHz would otherwise ask for a shelf above half the
    // sample rate, which produces coefficients that are not merely wrong but
    // unstable.
    const auto limit = static_cast<float>(sampleRate * 0.49);

    Shape clamped;
    clamped.lowHz = juce::jlimit(20.0f, limit, newShape.lowHz);
    clamped.midHz = juce::jlimit(20.0f, limit, newShape.midHz);
    clamped.midQ = juce::jlimit(0.1f, 8.0f, newShape.midQ);
    clamped.highHz = juce::jlimit(20.0f, limit, newShape.highHz);

    if (juce::exactlyEqual(clamped.lowHz, shape.lowHz) && juce::exactlyEqual(clamped.midHz, shape.midHz) &&
        juce::exactlyEqual(clamped.midQ, shape.midQ) && juce::exactlyEqual(clamped.highHz, shape.highHz))
        return;

    shape = clamped;

    // A shape change invalidates all three, so rebuild rather than relying on
    // setGainsDb's "only if the gain moved" shortcut - which would leave the
    // filters on the old frequencies until the user also turned a gain knob.
    const auto low =
        ArrayCoeffs::makeLowShelf(sampleRate, shape.lowHz, kToneShelfQ, juce::Decibels::decibelsToGain(lastLow));
    const auto mid =
        ArrayCoeffs::makePeakFilter(sampleRate, shape.midHz, shape.midQ, juce::Decibels::decibelsToGain(lastMid));
    const auto high =
        ArrayCoeffs::makeHighShelf(sampleRate, shape.highHz, kToneShelfQ, juce::Decibels::decibelsToGain(lastHigh));

    setSection(0, low);
    setSection(1, mid);
    setSection(2, high);
}

float ToneStack::magnitudeDbAt(float frequencyHz) const noexcept
{
    using ArrayCoeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    if (sampleRate <= 0.0 || frequencyHz <= 0.0f)
        return 0.0f;

    // Rebuilt from the same design calls the processing path uses, so the drawn
    // curve cannot drift from the filters. Reading the live filters' own
    // coefficients would be cheaper but they are mutated from the audio thread.
    const auto low =
        ArrayCoeffs::makeLowShelf(sampleRate, shape.lowHz, kToneShelfQ, juce::Decibels::decibelsToGain(lastLow));
    const auto mid =
        ArrayCoeffs::makePeakFilter(sampleRate, shape.midHz, shape.midQ, juce::Decibels::decibelsToGain(lastMid));
    const auto high =
        ArrayCoeffs::makeHighShelf(sampleRate, shape.highHz, kToneShelfQ, juce::Decibels::decibelsToGain(lastHigh));

    const auto magnitude = [this, frequencyHz](const std::array<float, 6>& c)
    {
        // c is {b0, b1, b2, a0, a1, a2} with a0 normalised to 1 by JUCE.
        const auto w = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;
        const std::complex<double> z{std::cos(-w), std::sin(-w)};
        const auto z2 = z * z;

        const auto numerator =
            static_cast<double>(c[0]) + static_cast<double>(c[1]) * z + static_cast<double>(c[2]) * z2;
        const auto denominator =
            static_cast<double>(c[3]) + static_cast<double>(c[4]) * z + static_cast<double>(c[5]) * z2;

        const auto d = std::abs(denominator);

        return d > 1.0e-12 ? std::abs(numerator) / d : 1.0;
    };

    const auto total = magnitude(low) * magnitude(mid) * magnitude(high);

    return static_cast<float>(juce::Decibels::gainToDecibels(juce::jmax(1.0e-6, total)));
}

namespace
{
/** Three transposed-direct-form-II biquads in series, `NumCh` channels at a
    time. Each section's output feeds the next, so one channel on its own is a
    chain of three dependent multiply-adds with nothing to fill the gaps;
    running both channels in the same iteration covers two chains in the time of
    one. The per-channel arithmetic is `juce::dsp::IIR::Filter`'s, in the same
    order. */
template<int NumCh>
void toneKernel(float* const* d, int numSamples, const std::array<std::array<float, 5>, 3>& coeffs,
                std::array<std::array<std::array<float, 2>, 3>, 2>& state) noexcept
{
    float s[NumCh][3][2];

    for (int k = 0; k < NumCh; ++k)
        for (int n = 0; n < 3; ++n)
        {
            s[k][n][0] = state[static_cast<size_t>(k)][static_cast<size_t>(n)][0];
            s[k][n][1] = state[static_cast<size_t>(k)][static_cast<size_t>(n)][1];
        }

    for (int i = 0; i < numSamples; ++i)
    {
        float x[NumCh];

        for (int k = 0; k < NumCh; ++k)
            x[k] = dsputil::sanitise(d[k][i]);

        for (int n = 0; n < 3; ++n)
        {
            const auto& c = coeffs[static_cast<size_t>(n)];

            for (int k = 0; k < NumCh; ++k)
            {
                const float y = (c[0] * x[k]) + s[k][n][0];
                s[k][n][0] = (c[1] * x[k]) - (c[3] * y) + s[k][n][1];
                s[k][n][1] = (c[2] * x[k]) - (c[4] * y);
                x[k] = y;
            }
        }

        for (int k = 0; k < NumCh; ++k)
            d[k][i] = x[k];
    }

    for (int k = 0; k < NumCh; ++k)
        for (int n = 0; n < 3; ++n)
        {
            // Denormal guard for sample-by-sample use, as JUCE's filter applies
            // after every block.
            juce::dsp::util::snapToZero(s[k][n][0]);
            juce::dsp::util::snapToZero(s[k][n][1]);
            state[static_cast<size_t>(k)][static_cast<size_t>(n)][0] = s[k][n][0];
            state[static_cast<size_t>(k)][static_cast<size_t>(n)][1] = s[k][n][1];
        }
}
} // namespace

void ToneStack::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (channels == nullptr || numSamples <= 0 || numChannels <= 0)
        return;

    const int nc = juce::jmin(numChannels, static_cast<int>(state.size()));

    // A flat setting leaves unity coefficients, so the sections are left running
    // rather than branched around: skipping would freeze whatever state is still
    // ringing out and click on the way back.
    if (nc >= 2)
        toneKernel<2>(channels, numSamples, coeffs, state);
    else
        toneKernel<1>(channels, numSamples, coeffs, state);

    // NaN backstop: an IIR cannot recover from a poisoned state on its own.
    for (int ch = 0; ch < nc; ++ch)
    {
        if (!dsputil::isFinite(channels[ch][numSamples - 1]))
        {
            reset();

            for (int c2 = 0; c2 < nc; ++c2)
                for (int i = 0; i < numSamples; ++i)
                    channels[c2][i] = dsputil::sanitise(channels[c2][i]);

            break;
        }
    }
}

} // namespace ember
