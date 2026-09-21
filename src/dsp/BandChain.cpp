#include "dsp/BandChain.h"
#include "dsp/DspUtils.h"

namespace ember
{
BandChain::BandChain()
{
    for (int i = 0; i < kNumStyles; ++i)
        styles[static_cast<size_t>(i)] = createSaturationStyle(static_cast<StyleID>(i));

    currentStyle = styles[0].get();
}

BandChain::~BandChain() = default;

void BandChain::prepare(double hostSampleRate, int maxBlockSize, int numChannels, OversamplingFactor factor,
                        bool linearPhaseOversampling)
{
    hostRate = hostSampleRate;
    maxBlock = juce::jmax(1, maxBlockSize);
    channels = juce::jlimit(1, 2, numChannels);
    osFactorMultiplier = oversamplingMultiplier(factor);

    const int numStages = static_cast<int>(factor); // Off=0, x2=1, x4=2, x8=3, x16=4

    if (numStages > 0)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            static_cast<size_t>(channels), static_cast<size_t>(numStages),
            linearPhaseOversampling ? juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple
                                    : juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            true,  // maximum quality
            true); // integer latency where possible
        oversampler->initProcessing(static_cast<size_t>(maxBlock));
        oversampler->reset();
        latencySamples = static_cast<float>(oversampler->getLatencyInSamples());
    }
    else
    {
        oversampler.reset();
        latencySamples = 0.0f;
    }

    osRate = hostRate * osFactorMultiplier;

    // Every style is prepared up front so a style change is a pointer swap
    // rather than an allocation on the audio thread.
    const int osBlock = maxBlock * osFactorMultiplier;
    for (auto& s : styles)
        s->prepare(osRate, osBlock, channels);

    calibrator = &StyleCalibrator::getForSampleRate(osRate);

    dcBlocker.prepare(hostRate, channels);
    feedback.prepare(osRate, osBlock, channels);
    dynamics.prepare(hostRate, maxBlock, channels);
    tone.prepare(hostRate, maxBlock, channels);

    // Order matters: prepare() establishes the channel count, and
    // setMaximumDelayInSamples resizes while keeping it, so preparing second
    // would leave the line sized for zero channels.
    dryDelay.prepare({hostRate, static_cast<juce::uint32>(maxBlock), static_cast<juce::uint32>(channels)});
    dryDelay.setMaximumDelayInSamples(juce::jmax(8, static_cast<int>(std::ceil(latencySamples)) + 8));
    dryDelay.setDelay(latencySamples);

    dryBuffer.setSize(channels, maxBlock, false, false, true);
    fadeBuffer.setSize(channels, osBlock, false, false, true);

    const double smoothSeconds = 0.02;
    for (auto* sv :
         {&smoothedDriveDb, &smoothedMix, &smoothedLevelGain, &smoothedPan, &smoothedWidth, &smoothedCompGain})
        sv->reset(hostRate, smoothSeconds);

    smoothedDriveDb.setCurrentAndTargetValue(params.driveDb);
    smoothedMix.setCurrentAndTargetValue(params.mix01);
    smoothedLevelGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(params.levelDb));
    smoothedPan.setCurrentAndTargetValue(params.pan);
    smoothedWidth.setCurrentAndTargetValue(params.width01);
    smoothedCompGain.setCurrentAndTargetValue(1.0f);

    styleFadeStep = static_cast<float>(1.0 / juce::jmax(1.0, kCrossfadeSeconds * hostRate));

    reset();
}

void BandChain::reset() noexcept
{
    for (auto& s : styles)
        s->reset();

    if (oversampler != nullptr)
        oversampler->reset();

    dcBlocker.reset();
    feedback.reset();
    dynamics.reset();
    tone.reset();
    dryDelay.reset();
    dryBuffer.clear();
    fadeBuffer.clear();

    fadingStyle = nullptr;
    styleFade = 1.0f;
}

void BandChain::setParameters(const BandParams& p) noexcept
{
    // A style change starts a crossfade between the outgoing and incoming
    // instance. Both already exist, so nothing is allocated here.
    if (p.style != params.style)
    {
        auto* next = styles[static_cast<size_t>(juce::jlimit(0, kNumStyles - 1, static_cast<int>(p.style)))].get();
        if (next != currentStyle)
        {
            fadingStyle = currentStyle;
            currentStyle = next;
            styleFade = 0.0f;
        }
    }

    params = p;

    smoothedDriveDb.setTargetValue(juce::jlimit(0.0f, 40.0f, p.driveDb));
    smoothedMix.setTargetValue(juce::jlimit(0.0f, 1.0f, p.mix01));
    smoothedLevelGain.setTargetValue(juce::Decibels::decibelsToGain(juce::jlimit(-24.0f, 24.0f, p.levelDb)));
    smoothedPan.setTargetValue(juce::jlimit(-1.0f, 1.0f, p.pan));
    smoothedWidth.setTargetValue(juce::jlimit(0.0f, 2.0f, p.width01));

    if (calibrator != nullptr)
        smoothedCompGain.setTargetValue(calibrator->compensationGain(p.style, p.driveDb));

    feedback.setParameters(juce::jlimit(0.0f, 1.0f, p.feedback01), juce::jlimit(20.0f, 2000.0f, p.feedbackFreq));
    dynamics.setAmount(juce::jlimit(-1.0f, 1.0f, p.dynamics));
    tone.setGainsDb(juce::jlimit(-12.0f, 12.0f, p.toneLowDb), juce::jlimit(-12.0f, 12.0f, p.toneMidDb),
                    juce::jlimit(-12.0f, 12.0f, p.toneHighDb));
}

void BandChain::process(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const int numCh = juce::jmin(channels, buffer.getNumChannels());

    // ---- dry tap, delayed by the oversampler's exact fractional latency ----
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    if (latencySamples > 0.0f)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = dryBuffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                dryDelay.pushSample(ch, d[i]);
                d[i] = dryDelay.popSample(ch);
            }
        }
    }

    // A fully bypassed band still has to come out with the same latency as its
    // neighbours, otherwise the band sum combs. The dry path above already
    // carries that delay, so bypass is just "use the dry path".
    if (params.bypass)
    {
        for (int ch = 0; ch < numCh; ++ch)
            buffer.copyFrom(ch, 0, dryBuffer, ch, 0, numSamples);
        applyLevelPanWidth(buffer, numSamples);
        heatRatio.store(0.0f, std::memory_order_relaxed); // adds nothing, so it is cold
        return;
    }

    StyleParams sp;
    sp.sampleRate = osRate;
    sp.driveDb = smoothedDriveDb.getCurrentValue();
    sp.driveLin = juce::Decibels::decibelsToGain(sp.driveDb);
    sp.amount01 = juce::jlimit(0.0f, 1.0f, sp.driveDb / 40.0f);
    smoothedDriveDb.skip(numSamples);

    // ---- oversampled nonlinear section ----
    juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(), static_cast<size_t>(numCh),
                                       static_cast<size_t>(numSamples));

    if (oversampler != nullptr)
    {
        auto up = oversampler->processSamplesUp(block);
        const int n = static_cast<int>(up.getNumSamples());

        // AudioBlock exposes channels one at a time; the style interface wants
        // an array of pointers, so gather them into a fixed-size local.
        float* osPtrs[2] = {nullptr, nullptr};
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            osPtrs[ch] = up.getChannelPointer(static_cast<size_t>(ch));
        float* const* ptrs = osPtrs;

        if (fadingStyle != nullptr && styleFade < 1.0f)
        {
            // Run both styles and crossfade. Only happens for ~20 ms after a
            // style change, so the doubled cost is not a steady-state concern.
            for (int ch = 0; ch < numCh; ++ch)
                juce::FloatVectorOperations::copy(fadeBuffer.getWritePointer(ch), ptrs[ch], n);

            currentStyle->process(ptrs, numCh, n, sp);
            float* const* fadePtrs = fadeBuffer.getArrayOfWritePointers();
            fadingStyle->process(fadePtrs, numCh, n, sp);

            float fade = styleFade;
            const float step = styleFadeStep / static_cast<float>(osFactorMultiplier);
            for (int i = 0; i < n; ++i)
            {
                const float f = juce::jlimit(0.0f, 1.0f, fade);
                for (int ch = 0; ch < numCh; ++ch)
                    ptrs[ch][i] = f * ptrs[ch][i] + (1.0f - f) * fadePtrs[ch][i];
                fade += step;
            }
            styleFade = juce::jlimit(0.0f, 1.0f, fade);
            if (styleFade >= 1.0f)
                fadingStyle = nullptr;
        }
        else
        {
            currentStyle->process(ptrs, numCh, n, sp);
        }

        feedback.process(ptrs, numCh, n);

        // Ramp the gain-match compensation across the block. Stepping it once
        // per block would click when drive is automated quickly, because a
        // 512-sample block is a large fraction of the 20 ms smoothing time.
        const float compStart = smoothedCompGain.getCurrentValue();
        smoothedCompGain.skip(numSamples);
        const float compEnd = smoothedCompGain.getCurrentValue();
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = ptrs[ch];
            const float inc = (compEnd - compStart) / static_cast<float>(juce::jmax(1, n));
            float gCur = compStart;
            for (int i = 0; i < n; ++i)
            {
                d[i] *= gCur;
                gCur += inc;
            }
        }

        oversampler->processSamplesDown(block);
    }
    else
    {
        float* const* ptrs = buffer.getArrayOfWritePointers();
        currentStyle->process(ptrs, numCh, numSamples, sp);
        feedback.process(ptrs, numCh, numSamples);
        const float compStart = smoothedCompGain.getCurrentValue();
        smoothedCompGain.skip(numSamples);
        const float compEnd = smoothedCompGain.getCurrentValue();
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = ptrs[ch];
            const float inc = (compEnd - compStart) / static_cast<float>(juce::jmax(1, numSamples));
            float gCur = compStart;
            for (int i = 0; i < numSamples; ++i)
            {
                d[i] *= gCur;
                gCur += inc;
            }
        }
        if (fadingStyle != nullptr)
        {
            styleFade = juce::jlimit(0.0f, 1.0f, styleFade + styleFadeStep * static_cast<float>(numSamples));
            if (styleFade >= 1.0f)
                fadingStyle = nullptr;
        }
    }

    // A style or the feedback path can generate a non-finite value internally,
    // and the oversampler's half-band filters and the tone stack's IIRs have no
    // way back once their history is poisoned. Scanning the block costs a
    // comparison per sample - no write, no branch misprediction in the common
    // case - and turns a permanently dead band into one lost block.
    {
        bool finite = true;
        for (int ch = 0; ch < numCh && finite; ++ch)
        {
            const auto* d = buffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                if (!std::isfinite(d[i]))
                {
                    finite = false;
                    break;
                }
        }

        if (!finite)
        {
            buffer.clear();
            reset();
            return;
        }
    }

    // ---- base-rate post section ----
    float* const* post = buffer.getArrayOfWritePointers();
    dcBlocker.process(post, numCh, numSamples);
    dynamics.process(post, numCh, numSamples);
    tone.process(post, numCh, numSamples);

    applyLevelPanWidth(buffer, numSamples);

    // ---- band dry/wet ----
    // Pointers hoisted out of the sample loop: getWritePointer/getSample per
    // sample per channel is a measurable cost in a six-band chain.
    {
        float* wet[2] = {nullptr, nullptr};
        const float* dry[2] = {nullptr, nullptr};
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
        {
            wet[ch] = buffer.getWritePointer(ch);
            dry[ch] = dryBuffer.getReadPointer(ch);
        }

        const float m0 = smoothedMix.getCurrentValue();
        smoothedMix.skip(numSamples);
        const float m1 = smoothedMix.getCurrentValue();
        const float dm = (m1 - m0) / static_cast<float>(juce::jmax(1, numSamples));

        // Heat is measured here because this loop already holds the band's
        // input and its final output in registers: three multiply-adds per
        // sample on one channel, no extra loads, no sample altered.
        //
        // It measures the DISTORTION RESIDUAL, not a level ratio. Ember
        // gain-matches every style - the calibrator holds output level to
        // within 0.1 dB of input across the whole drive range - so output-over-
        // input RMS sits at 1.0 however hard a band is driven, and would report
        // a screaming band as stone cold. What actually changes is how much of
        // the output is no longer a scaled copy of the input.
        float inIn = 0.0f;
        float inOut = 0.0f;
        float outOut = 0.0f;

        float mix = m0;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
                wet[ch][i] = mix * wet[ch][i] + (1.0f - mix) * dry[ch][i];

            const float in = dry[0][i];
            const float out = wet[0][i];
            inIn += in * in;
            inOut += in * out;
            outOut += out * out;

            mix += dm;
        }

        publishHeat(inIn, inOut, outOut);
    }
}

void BandChain::publishHeat(float inIn, float inOut, float outOut) noexcept
{
    // Least-squares fit the output as a scaled copy of the input, then ask how
    // much energy is left over. alpha = <in,out>/<in,in> is the best linear
    // explanation of the output; everything the fit cannot account for is
    // harmonic content the band added.
    //
    //   residual = <out,out> - <in,out>^2 / <in,in>
    //   heat     = sqrt(residual / <out,out>)
    //
    // That is a normalised distortion figure: 0 when the band is a clean gain
    // stage at any gain, rising towards 1 as the output stops resembling the
    // input. It is immune to the gain matching, to auto-gain, and to the band's
    // own level and pan.
    float ratio = 0.0f;

    // Below the noise floor the fit is meaningless - dividing near-zero
    // energies swings violently and a silent plugin would flicker.
    if (inIn > 1.0e-9f && outOut > 1.0e-9f)
    {
        const float residual = outOut - (inOut * inOut) / inIn;
        ratio = std::sqrt(juce::jmax(0.0f, residual) / outOut);
    }

    if (! std::isfinite(ratio))
        ratio = 0.0f;

    heatRatio.store(juce::jlimit(0.0f, 1.0f, ratio), std::memory_order_relaxed);
}

void BandChain::applyLevelPanWidth(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    const int numCh = juce::jmin(channels, buffer.getNumChannels());
    if (numCh <= 0 || numSamples <= 0)
        return;

    // These three values are all smoothed, so instead of evaluating the pan law
    // per sample — two transcendental calls each — evaluate it once at each end
    // of the block and interpolate the resulting gains. Over a 20 ms smoothing
    // ramp the difference from the exact curve is inaudible, and it takes the
    // trig out of the inner loop entirely.
    auto panGains = [](float pan, float& gl, float& gr) noexcept
    {
        const float theta = (juce::jlimit(-1.0f, 1.0f, pan) + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        gl = std::cos(theta) * juce::MathConstants<float>::sqrt2; // unity at centre
        gr = std::sin(theta) * juce::MathConstants<float>::sqrt2;
    };

    const float lv0 = smoothedLevelGain.getCurrentValue();
    const float pan0 = smoothedPan.getCurrentValue();
    const float wd0 = smoothedWidth.getCurrentValue();
    smoothedLevelGain.skip(numSamples);
    smoothedPan.skip(numSamples);
    smoothedWidth.skip(numSamples);
    const float lv1 = smoothedLevelGain.getCurrentValue();
    const float pan1 = smoothedPan.getCurrentValue();
    const float wd1 = smoothedWidth.getCurrentValue();

    const float inv = 1.0f / static_cast<float>(juce::jmax(1, numSamples));

    if (numCh >= 2)
    {
        float gl0 = 1.0f, gr0 = 1.0f, gl1 = 1.0f, gr1 = 1.0f;
        panGains(pan0, gl0, gr0);
        panGains(pan1, gl1, gr1);

        const float dLv = (lv1 - lv0) * inv;
        const float dWd = (wd1 - wd0) * inv;
        const float dGl = (gl1 - gl0) * inv;
        const float dGr = (gr1 - gr0) * inv;

        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);

        float lv = lv0, wd = wd0, gl = gl0, gr = gr0;
        for (int i = 0; i < numSamples; ++i)
        {
            // Mid/side width first, then constant-power pan.
            const float mid = 0.5f * (l[i] + r[i]);
            const float side = 0.5f * (l[i] - r[i]) * wd;
            l[i] = (mid + side) * gl * lv;
            r[i] = (mid - side) * gr * lv;
            lv += dLv;
            wd += dWd;
            gl += dGl;
            gr += dGr;
        }
    }
    else
    {
        auto* m = buffer.getWritePointer(0);
        const float dLv = (lv1 - lv0) * inv;
        float lv = lv0;
        for (int i = 0; i < numSamples; ++i)
        {
            m[i] *= lv;
            lv += dLv;
        }
    }
}
} // namespace ember
