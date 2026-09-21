#include "dsp/styles/TapeStyles.h"

namespace ember
{
namespace
{
// ---------------------------------------------------------------- Clean Tape
/** Slightly under unity so the knee enters gently — "clean" means the curve is
    audible as compression well before it is audible as distortion. */
constexpr float kCleanCurve = 0.85f;
constexpr float kCleanShelfHiHz = 9000.0f; ///< shelf corner at zero drive
constexpr float kCleanShelfLoHz = 3000.0f; ///< shelf corner at full drive
constexpr float kCleanShelfDepth = 0.72f;  ///< HF retained at full drive = 1 - this

// ---------------------------------------------------------------- Warm Tape
constexpr float kWarmLevelSeconds = 0.012f; ///< envelope memory (program dependent)
constexpr float kWarmMagSeconds = 0.0007f;  ///< magnetisation memory (~230 Hz)
constexpr float kWarmSquashMin = 0.60f;     ///< level-dependent gain reduction at 0 drive
constexpr float kWarmSquashRange = 2.40f;
constexpr float kWarmMagMin = 0.15f; ///< feedback depth; max 0.35 keeps loop gain < 1
constexpr float kWarmMagRange = 0.20f;
constexpr float kWarmShelfHiHz = 7000.0f;
constexpr float kWarmShelfLoHz = 2800.0f;
constexpr float kWarmShelfDepth = 0.78f;

// ---------------------------------------------------------------- Bright Tape
constexpr float kBrightPoleHz = 3000.0f;  ///< top of the emphasis shelf
constexpr float kBrightShelfMin = 2.0f;   ///< +6 dB of pre-emphasis at zero drive
constexpr float kBrightShelfRange = 3.0f; ///< ... rising to +14 dB at full drive

// ---------------------------------------------------------------- Transformer
constexpr float kXfmrSplitHz = 200.0f;
constexpr float kXfmrSplitQ = 0.5f;
constexpr float kXfmrBloomHz = 95.0f;
constexpr float kXfmrBloomQ = 0.9f;
constexpr float kXfmrPushMin = 1.15f;   ///< lows always run a little hotter: +1.2 dB
constexpr float kXfmrPushRange = 1.65f; ///< ... up to +9 dB at full drive
constexpr float kXfmrHighKnee = 0.7f;   ///< highs saturate ~12 dB later than the lows
constexpr float kXfmrBloomMin = 0.18f;
constexpr float kXfmrBloomRange = 0.42f;

/** Common entry guard: returns the number of channels to touch, or 0 if there
    is nothing to do. Keeps every `process` honest about empty and oversized
    buffers without repeating the same four lines. */
inline int usableChannels(float* const* channelData, int numChannels, int numSamples) noexcept
{
    if (channelData == nullptr || numSamples <= 0 || numChannels <= 0)
        return 0;

    jassert(numChannels <= tapedetail::kMaxChannels);
    return juce::jmin(numChannels, tapedetail::kMaxChannels);
}
} // namespace

/**
    Why these kernels take the channels together
    --------------------------------------------
    A tape shaper's per-sample chain is a `fastTanh` — which ends in a float
    division — feeding a one-pole whose state is the next sample's input. That
    is twenty-odd cycles of pure dependency and only a handful of cycles of
    actual issue, so a loop that finishes one channel before starting the other
    leaves most of the core idle. The two channels share every coefficient and
    touch no common state, so putting both in the same iteration covers two
    chains in the time of one. The arithmetic per channel is untouched.

    The smoothers are copied into locals for the duration: they live in arrays
    the audio pointers could in principle alias, and without the copy the
    compiler has to reload the filter state from memory on every sample.
*/
namespace
{
template <int NumCh>
void cleanTapeKernel(float* const* data, int numSamples, float drive, float retain,
                     dsputil::OnePole* rolloff) noexcept
{
    dsputil::OnePole lp[NumCh];

    for (int k = 0; k < NumCh; ++k)
        lp[k] = rolloff[k];

    for (int i = 0; i < numSamples; ++i)
    {
        for (int k = 0; k < NumCh; ++k)
        {
            const float x = dsputil::sanitise(data[k][i]);

            // Odd-symmetric soft curve: no bias, so no even harmonics and no DC.
            const float shaped = dsputil::fastTanh(x * drive);

            const float low = lp[k].process(shaped);
            data[k][i] = tapedetail::finish(low + retain * (shaped - low));
        }
    }

    for (int k = 0; k < NumCh; ++k)
        rolloff[k] = lp[k];
}
} // namespace

//==============================================================================
void CleanTapeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                             [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= tapedetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);

    for (auto& lp : rolloff)
        lp.prepare(sampleRate);

    reset();
}

void CleanTapeStyle::reset() noexcept
{
    for (auto& lp : rolloff)
        lp.reset();
}

void CleanTapeStyle::process(float* const* channelData, int numChannels, int numSamples,
                             const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0); // prepare() owns the rate

    const float amount = tapedetail::clampAmount(params.amount01);
    const float drive = tapedetail::clampDrive(params.driveLin) * kCleanCurve;

    // The HF shelf closes progressively: unity gain at zero drive (so the stage
    // is transparent above the shaper), ~-11 dB and a lower corner at full drive.
    const float coeff =
        tapedetail::onePoleCoeff(kCleanShelfHiHz - (kCleanShelfHiHz - kCleanShelfLoHz) * amount, sampleRate);
    const float retain = 1.0f - kCleanShelfDepth * amount;

    for (int ch = 0; ch < chans; ++ch)
    {
        auto& lp = rolloff[static_cast<size_t>(ch)];
        tapedetail::sanitiseOnePole(lp);
        lp.setCoefficient(coeff);
    }

    if (chans >= 2 && channelData[0] != nullptr && channelData[1] != nullptr)
    {
        cleanTapeKernel<2>(channelData, numSamples, drive, retain, rolloff.data());
        return;
    }

    for (int ch = 0; ch < chans; ++ch)
    {
        if (channelData[ch] == nullptr)
            continue;

        float* one[1] = {channelData[ch]};
        cleanTapeKernel<1>(one, numSamples, drive, retain, &rolloff[static_cast<size_t>(ch)]);
    }
}

namespace
{
template <int NumCh>
void warmTapeKernel(float* const* data, int numSamples, float drive, float magAmt, float squash, float retain,
                    dsputil::OnePole* level, dsputil::OnePole* mag, dsputil::OnePole* tone) noexcept
{
    dsputil::OnePole lv[NumCh], mg[NumCh], tn[NumCh];

    for (int k = 0; k < NumCh; ++k)
    {
        lv[k] = level[k];
        mg[k] = mag[k];
        tn[k] = tone[k];
    }

    for (int i = 0; i < numSamples; ++i)
    {
        for (int k = 0; k < NumCh; ++k)
        {
            const float x = dsputil::sanitise(data[k][i]);

            // One-sample-delayed magnetisation memory breaks the algebraic loop;
            // it is itself bounded by +/-1 because it smooths a tanh output.
            const float shaped = dsputil::fastTanh(x * drive - magAmt * mg[k].getState());
            mg[k].process(shaped);

            // Gain rides the curve's own output, so the squash adds to the
            // saturation instead of backing the signal out of it: Warm Tape is
            // more compressed than Clean Tape at every drive setting. The
            // detector sees a bounded signal, so the divisor is always >= 1.
            const float env = lv[k].process(std::abs(shaped));
            const float squashed = shaped / (1.0f + squash * env);

            const float low = tn[k].process(squashed);
            data[k][i] = tapedetail::finish(low + retain * (squashed - low));
        }
    }

    for (int k = 0; k < NumCh; ++k)
    {
        level[k] = lv[k];
        mag[k] = mg[k];
        tone[k] = tn[k];
    }
}
} // namespace

//==============================================================================
void WarmTapeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                            [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= tapedetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);

    for (auto& s : state)
    {
        s.level.prepare(sampleRate);
        s.mag.prepare(sampleRate);
        s.tone.prepare(sampleRate);
    }

    reset();
}

void WarmTapeStyle::reset() noexcept
{
    for (auto& s : state)
    {
        s.level.reset();
        s.mag.reset();
        s.tone.reset();
    }
}

void WarmTapeStyle::process(float* const* channelData, int numChannels, int numSamples,
                            const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0);

    const float amount = tapedetail::clampAmount(params.amount01);
    const float drive = tapedetail::clampDrive(params.driveLin);

    const float levelCoeff = tapedetail::timeCoeff(kWarmLevelSeconds, sampleRate);
    const float magCoeff = tapedetail::timeCoeff(kWarmMagSeconds, sampleRate);
    const float toneCoeff =
        tapedetail::onePoleCoeff(kWarmShelfHiHz - (kWarmShelfHiHz - kWarmShelfLoHz) * amount, sampleRate);
    const float retain = 1.0f - kWarmShelfDepth * amount;
    const float squash = kWarmSquashMin + kWarmSquashRange * amount;
    const float magAmt = kWarmMagMin + kWarmMagRange * amount; // <= 0.35

    for (int ch = 0; ch < chans; ++ch)
    {
        auto& s = state[static_cast<size_t>(ch)];
        tapedetail::sanitiseOnePole(s.level);
        tapedetail::sanitiseOnePole(s.mag);
        tapedetail::sanitiseOnePole(s.tone);

        s.level.setCoefficient(levelCoeff);
        s.mag.setCoefficient(magCoeff);
        s.tone.setCoefficient(toneCoeff);
    }

    if (chans >= 2 && channelData[0] != nullptr && channelData[1] != nullptr)
    {
        // A channel's three smoothers live in one struct, so a pair of channels
        // is not contiguous per smoother. Gather them, run, scatter back.
        dsputil::OnePole lv[2] = {state[0].level, state[1].level};
        dsputil::OnePole mg[2] = {state[0].mag, state[1].mag};
        dsputil::OnePole tn[2] = {state[0].tone, state[1].tone};

        warmTapeKernel<2>(channelData, numSamples, drive, magAmt, squash, retain, lv, mg, tn);

        for (size_t k = 0; k < 2; ++k)
        {
            state[k].level = lv[k];
            state[k].mag = mg[k];
            state[k].tone = tn[k];
        }

        return;
    }

    for (int ch = 0; ch < chans; ++ch)
    {
        if (channelData[ch] == nullptr)
            continue;

        auto& s = state[static_cast<size_t>(ch)];
        float* one[1] = {channelData[ch]};
        warmTapeKernel<1>(one, numSamples, drive, magAmt, squash, retain, &s.level, &s.mag, &s.tone);
    }
}

//==============================================================================
void BrightTapeStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                              [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= tapedetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);
    reset();
}

void BrightTapeStyle::reset() noexcept
{
    for (auto& s : state)
        s = EmphasisChannel{};
}

void BrightTapeStyle::process(float* const* channelData, int numChannels, int numSamples,
                              const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0);

    const float amount = tapedetail::clampAmount(params.amount01);
    const float drive = tapedetail::clampDrive(params.driveLin);

    // Both filters are designed from the SAME two coefficients, so they are
    // genuine inverses rather than two independently tuned shelves.
    const float shelf = kBrightShelfMin + kBrightShelfRange * amount;
    const float a = tapedetail::onePoleCoeff(kBrightPoleHz, sampleRate);         // pole of pre
    const float b = tapedetail::onePoleCoeff(kBrightPoleHz / shelf, sampleRate); // zero of pre
    const float shelfGain = (1.0f - a) / juce::jmax(1.0e-6f, 1.0f - b);          // == A, > 1
    const float invGain = 1.0f / juce::jmax(1.0e-6f, shelfGain);

    const float preB0 = shelfGain, preB1 = -shelfGain * b, preA1 = a; // HF boost, unity at DC
    const float deB0 = invGain, deB1 = -invGain * a, deA1 = b;        // exact inverse

    for (int ch = 0; ch < chans; ++ch)
    {
        float* data = channelData[ch];
        if (data == nullptr)
            continue;

        auto& s = state[static_cast<size_t>(ch)];
        if (!(dsputil::isFinite(s.preX1) && dsputil::isFinite(s.preY1) && dsputil::isFinite(s.deX1) &&
              dsputil::isFinite(s.deY1)))
            s = EmphasisChannel{};

        for (int i = 0; i < numSamples; ++i)
        {
            // Bounded, not merely sanitised: the pre-emphasis has gain (its l1
            // norm is 2*shelf - 1, so at most 9), and an unbounded finite input
            // would overflow `preB0 * x` to infinity. The next sample then adds
            // +inf to -inf, NaN lands in preY1, and — because recursive state is
            // only sanitised at the block boundary — the whole rest of the block
            // comes out silent. With |x| <= 64 the state is bounded by 576 and
            // that cannot happen.
            const float x = tapedetail::clampInput(data[i]);

            const float pre = preB0 * x + preB1 * s.preX1 + preA1 * s.preY1;
            s.preX1 = x;
            s.preY1 = pre;

            // Symmetric curve; small-signal gain is exactly `drive`, so at low
            // drive the emphasis pair nulls and the response stays flat.
            const float sat = dsputil::fastTanh(pre * drive);

            const float de = deB0 * sat + deB1 * s.deX1 + deA1 * s.deY1;
            s.deX1 = sat;
            s.deY1 = de;

            data[i] = tapedetail::finish(de);
        }
    }
}

//==============================================================================
void TransformerStyle::prepare(double oversampledSampleRate, [[maybe_unused]] int maxBlockSize,
                               [[maybe_unused]] int numChannels)
{
    jassert(numChannels >= 1 && numChannels <= tapedetail::kMaxChannels);
    sampleRate = juce::jmax(8000.0, oversampledSampleRate);

    for (auto& s : state)
    {
        s.split.prepare(sampleRate);
        s.bloom.prepare(sampleRate);
        s.split.setCutoffQ(kXfmrSplitHz, kXfmrSplitQ);
        s.bloom.setCutoffQ(kXfmrBloomHz, kXfmrBloomQ);
    }

    reset();
}

void TransformerStyle::reset() noexcept
{
    for (auto& s : state)
    {
        s.split.reset();
        s.bloom.reset();
    }
}

void TransformerStyle::process(float* const* channelData, int numChannels, int numSamples,
                               const StyleParams& params) noexcept
{
    const int chans = usableChannels(channelData, numChannels, numSamples);
    if (chans == 0)
        return;

    jassert(std::abs(params.sampleRate - sampleRate) < 1.0);

    const float amount = tapedetail::clampAmount(params.amount01);
    const float drive = tapedetail::clampDrive(params.driveLin);

    const float lowDrive = drive * (kXfmrPushMin + kXfmrPushRange * amount); // lows pushed hardest
    const float hiDrive = drive * kXfmrHighKnee;
    const float hiScale = 1.0f / kXfmrHighKnee;
    const float bloomAmt = kXfmrBloomMin + kXfmrBloomRange * amount;

    for (int ch = 0; ch < chans; ++ch)
    {
        float* data = channelData[ch];
        if (data == nullptr)
            continue;

        auto& s = state[static_cast<size_t>(ch)];
        s.split.sanitiseState();
        s.bloom.sanitiseState();

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = dsputil::sanitise(data[i]);

            // x == lp + k*bp + hp for the TPT SVF, so the complement of the
            // lowpass is an exactly reconstructing upper path.
            const float lows = s.split.process(x).lp;
            const float highs = x - lows;

            // Symmetric shapers: odd harmonics, no bias, no DC. The lows hit the
            // knee at 1 / lowDrive, the highs not until 1.43 / drive — 4 dB
            // apart at zero drive, 12 dB apart wide open.
            const float lowSat = dsputil::fastTanh(lows * lowDrive);
            const float hiSat = hiScale * dsputil::fastTanh(highs * hiDrive);

            // Mild resonant lift on the saturated core: the bass "bloom".
            const float bloom = s.bloom.processBandpass(lowSat);

            data[i] = tapedetail::finish(lowSat + bloomAmt * bloom + hiSat);
        }
    }
}
} // namespace ember
