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

void BandChain::prepare(double hostSampleRate, int maxBlockSize, int numChannels, OversamplingFactor factor)
{
    hostRate = hostSampleRate;
    maxBlock = juce::jmax(1, maxBlockSize);
    channels = juce::jlimit(1, 2, numChannels);
    osFactorMultiplier = oversamplingMultiplier(factor);

    const int numStages = static_cast<int>(factor);   // Off=0, x2=1, x4=2, x8=3, x16=4

    if (numStages > 0)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            static_cast<size_t>(channels), static_cast<size_t>(numStages),
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true,   // maximum quality
            true);  // integer latency where possible
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
    dryDelay.prepare({ hostRate, static_cast<juce::uint32>(maxBlock), static_cast<juce::uint32>(channels) });
    dryDelay.setMaximumDelayInSamples(juce::jmax(8, static_cast<int>(std::ceil(latencySamples)) + 8));
    dryDelay.setDelay(latencySamples);

    dryBuffer.setSize(channels, maxBlock, false, false, true);
    fadeBuffer.setSize(channels, osBlock, false, false, true);

    const double smoothSeconds = 0.02;
    for (auto* sv : { &smoothedDriveDb, &smoothedMix, &smoothedLevelGain,
                      &smoothedPan, &smoothedWidth, &smoothedCompGain })
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

    feedback.setParameters(juce::jlimit(0.0f, 1.0f, p.feedback01),
                           juce::jlimit(20.0f, 2000.0f, p.feedbackFreq));
    dynamics.setAmount(juce::jlimit(-1.0f, 1.0f, p.dynamics));
    tone.setGainsDb(juce::jlimit(-12.0f, 12.0f, p.toneLowDb),
                    juce::jlimit(-12.0f, 12.0f, p.toneMidDb),
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
        return;
    }

    StyleParams sp;
    sp.sampleRate = osRate;
    sp.driveDb = smoothedDriveDb.getCurrentValue();
    sp.driveLin = juce::Decibels::decibelsToGain(sp.driveDb);
    sp.amount01 = juce::jlimit(0.0f, 1.0f, sp.driveDb / 40.0f);
    smoothedDriveDb.skip(numSamples);

    // ---- oversampled nonlinear section ----
    juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(),
                                       static_cast<size_t>(numCh),
                                       static_cast<size_t>(numSamples));

    if (oversampler != nullptr)
    {
        auto up = oversampler->processSamplesUp(block);
        const int n = static_cast<int>(up.getNumSamples());
        auto** ptrs = up.getChannelPointers();

        if (fadingStyle != nullptr && styleFade < 1.0f)
        {
            // Run both styles and crossfade. Only happens for ~20 ms after a
            // style change, so the doubled cost is not a steady-state concern.
            for (int ch = 0; ch < numCh; ++ch)
                juce::FloatVectorOperations::copy(fadeBuffer.getWritePointer(ch), ptrs[ch], n);

            currentStyle->process(ptrs, numCh, n, sp);
            auto* fadePtrs = fadeBuffer.getArrayOfWritePointers();
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
            for (int i = 0; i < n; ++i) { d[i] *= gCur; gCur += inc; }
        }

        oversampler->processSamplesDown(block);
    }
    else
    {
        auto** ptrs = buffer.getArrayOfWritePointers();
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
            for (int i = 0; i < numSamples; ++i) { d[i] *= gCur; gCur += inc; }
        }
        if (fadingStyle != nullptr)
        {
            styleFade = juce::jlimit(0.0f, 1.0f, styleFade + styleFadeStep * static_cast<float>(numSamples));
            if (styleFade >= 1.0f)
                fadingStyle = nullptr;
        }
    }

    // ---- base-rate post section ----
    auto** post = buffer.getArrayOfWritePointers();
    dcBlocker.process(post, numCh, numSamples);
    dynamics.process(post, numCh, numSamples);
    tone.process(post, numCh, numSamples);

    applyLevelPanWidth(buffer, numSamples);

    // ---- band dry/wet ----
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = smoothedMix.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* w = buffer.getWritePointer(ch);
            const float dry = dryBuffer.getSample(ch, i);
            w[i] = dsputil::sanitise(mix * w[i] + (1.0f - mix) * dry);
        }
    }
}

void BandChain::applyLevelPanWidth(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    const int numCh = juce::jmin(channels, buffer.getNumChannels());

    if (numCh >= 2)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float gain = smoothedLevelGain.getNextValue();
            const float pan = smoothedPan.getNextValue();
            const float width = smoothedWidth.getNextValue();

            // Mid/side width first, then constant-power pan.
            const float mid = 0.5f * (l[i] + r[i]);
            const float side = 0.5f * (l[i] - r[i]) * width;
            float a = mid + side;
            float b = mid - side;

            // Constant-power pan, normalised so the centre position is unity:
            // cos(pi/4) * sqrt2 == 1.
            const float theta = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            const float gl = std::cos(theta) * juce::MathConstants<float>::sqrt2;
            const float gr = std::sin(theta) * juce::MathConstants<float>::sqrt2;
            l[i] = a * gl * gain;
            r[i] = b * gr * gain;
        }
    }
    else if (numCh == 1)
    {
        auto* m = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const float gain = smoothedLevelGain.getNextValue();
            smoothedPan.getNextValue();
            smoothedWidth.getNextValue();
            m[i] *= gain;
        }
    }
}
} // namespace ember
