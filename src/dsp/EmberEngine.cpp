#include "dsp/EmberEngine.h"
#include "dsp/DspUtils.h"

namespace ember
{
EmberEngine::EmberEngine() = default;
EmberEngine::~EmberEngine() = default;

void EmberEngine::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    maxBlockSize = juce::jmax(1, static_cast<int>(spec.maximumBlockSize));
    numChannels = juce::jlimit(1, 2, static_cast<int>(spec.numChannels));

    crossoverA.prepare(sampleRate, maxBlockSize, numChannels);
    crossoverB.prepare(sampleRate, maxBlockSize, numChannels);
    crossoverA.setMode(globalParams.crossoverMode);
    crossoverB.setMode(globalParams.crossoverMode);
    activeCrossover = &crossoverA;
    previousCrossover = nullptr;

    for (auto& b : bands)
        b.prepare(sampleRate, maxBlockSize, numChannels, osFactor, linearPhaseOversampling);

    for (auto& buf : bandBuffers)
        buf.setSize(numChannels, maxBlockSize, false, false, true);
    for (auto& buf : altBandBuffers)
        buf.setSize(numChannels, maxBlockSize, false, false, true);

    dryBuffer.setSize(numChannels, maxBlockSize, false, false, true);
    sumBuffer.setSize(numChannels, maxBlockSize, false, false, true);
    msBuffer.setSize(numChannels, maxBlockSize, false, false, true);

    const int bandLatency = static_cast<int>(std::ceil(bands[0].getLatencySamples()));
    latencySamples = bandLatency + activeCrossover->getLatencySamples();

    globalDryDelay.prepare(
        {sampleRate, static_cast<juce::uint32>(maxBlockSize), static_cast<juce::uint32>(numChannels)});
    globalDryDelay.setMaximumDelayInSamples(juce::jmax(8, latencySamples + 8));
    globalDryDelay.setDelay(static_cast<float>(latencySamples));

    const double smoothSeconds = 0.02;
    smoothedInputGain.reset(sampleRate, smoothSeconds);
    smoothedOutputGain.reset(sampleRate, smoothSeconds);
    smoothedGlobalMix.reset(sampleRate, smoothSeconds);
    smoothedAutoGain.reset(sampleRate, 0.2);
    smoothedInputGain.setCurrentAndTargetValue(1.0f);
    smoothedOutputGain.setCurrentAndTargetValue(1.0f);
    smoothedGlobalMix.setCurrentAndTargetValue(1.0f);
    smoothedAutoGain.setCurrentAndTargetValue(1.0f);

    for (auto& g : bandGateGain)
    {
        g.reset(sampleRate, smoothSeconds);
        g.setCurrentAndTargetValue(1.0f);
    }

    bandFadeStep = static_cast<float>(1.0 / juce::jmax(1.0, kCrossfadeSeconds * sampleRate));

    // ~300 ms integration for the auto-gain RMS comparison: long enough to
    // track programme loudness, short enough to follow an arrangement change.
    autoGainCoeff = static_cast<float>(std::exp(-1.0 / (0.3 * sampleRate)));

    for (int i = 0; i < kSpectrumFFTSize; ++i)
        window[static_cast<size_t>(i)] =
            0.5f * (1.0f - std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i) /
                                    static_cast<float>(kSpectrumFFTSize - 1)));

    reset();
}

void EmberEngine::reset()
{
    crossoverA.reset();
    crossoverB.reset();
    for (auto& b : bands)
        b.reset();
    for (auto& buf : bandBuffers)
        buf.clear();
    for (auto& buf : altBandBuffers)
        buf.clear();
    dryBuffer.clear();
    sumBuffer.clear();
    msBuffer.clear();
    globalDryDelay.reset();

    inputAccum.fill(0.0f);
    outputAccum.fill(0.0f);
    accumIndex = 0;
    dryRms = wetRms = 0.0f;
    bandFade = 1.0f;
    previousCrossover = nullptr;
    appliedNumBands = -1; // force the next setParameters to re-send

    for (auto& l : bandLevels)
        l.store(0.0f, std::memory_order_relaxed);
}

void EmberEngine::setOversamplingQuality(bool linearPhase)
{
    if (linearPhase == linearPhaseOversampling)
        return;
    linearPhaseOversampling = linearPhase;
    for (auto& b : bands)
        b.prepare(sampleRate, maxBlockSize, numChannels, osFactor, linearPhaseOversampling);

    const int bandLatency = static_cast<int>(std::ceil(bands[0].getLatencySamples()));
    latencySamples = bandLatency + activeCrossover->getLatencySamples();
    globalDryDelay.setMaximumDelayInSamples(juce::jmax(8, latencySamples + 8));
    globalDryDelay.setDelay(static_cast<float>(latencySamples));
}

void EmberEngine::setOversamplingFactor(OversamplingFactor factor)
{
    if (factor == osFactor)
        return;
    osFactor = factor;
    for (auto& b : bands)
        b.prepare(sampleRate, maxBlockSize, numChannels, osFactor, linearPhaseOversampling);

    const int bandLatency = static_cast<int>(std::ceil(bands[0].getLatencySamples()));
    latencySamples = bandLatency + activeCrossover->getLatencySamples();
    globalDryDelay.setMaximumDelayInSamples(juce::jmax(8, latencySamples + 8));
    globalDryDelay.setDelay(static_cast<float>(latencySamples));
}

void EmberEngine::setCrossoverMode(CrossoverMode mode)
{
    crossoverA.setMode(mode);
    crossoverB.setMode(mode);
    globalParams.crossoverMode = mode;

    const int bandLatency = static_cast<int>(std::ceil(bands[0].getLatencySamples()));
    latencySamples = bandLatency + activeCrossover->getLatencySamples();
    globalDryDelay.setMaximumDelayInSamples(juce::jmax(8, latencySamples + 8));
    globalDryDelay.setDelay(static_cast<float>(latencySamples));
}

int EmberEngine::getLatencySamples() const noexcept
{
    return latencySamples;
}

float EmberEngine::getBandGainReductionDb(int band) const noexcept
{
    return band >= 0 && band < kMaxBands ? bands[static_cast<size_t>(band)].getGainReductionDb() : 0.0f;
}

float EmberEngine::getBandLevel(int band) const noexcept
{
    return band >= 0 && band < kMaxBands ? bandLevels[static_cast<size_t>(band)].load(std::memory_order_relaxed) : 0.0f;
}

void EmberEngine::setParameters(const GlobalParams& global, const BandParams* bandsIn, int numBandParams) noexcept
{
    const int requested = juce::jlimit(kMinBands, kMaxBands, global.numBands);

    // Only touch the crossover when something actually changed. Re-sending the
    // same frequencies is not free: in linear-phase mode each edge costs a FIR
    // design and a forward FFT, and this runs once per 32-sample control block
    // on the audio thread.
    const int numEdges = juce::jmax(0, requested - 1);
    bool edgesMoved = (requested != appliedNumBands);
    for (int i = 0; i < numEdges && !edgesMoved; ++i)
        edgesMoved = std::abs(global.crossoverHz[i] - appliedCrossoverHz[i]) > 1.0e-3f;

    if (requested != activeNumBands)
    {
        // Start a band-count crossfade: the current crossover becomes the
        // "previous" one and a fresh configuration takes over.
        previousCrossover = activeCrossover;
        previousNumBands = activeNumBands;
        activeCrossover = (activeCrossover == &crossoverA) ? &crossoverB : &crossoverA;
        activeCrossover->setNumBands(requested);
        activeCrossover->setCrossoverFrequencies(global.crossoverHz, numEdges);
        activeNumBands = requested;
        bandFade = 0.0f;
        edgesMoved = true;
    }
    else if (edgesMoved)
    {
        activeCrossover->setNumBands(requested);
        activeCrossover->setCrossoverFrequencies(global.crossoverHz, numEdges);
    }

    if (edgesMoved)
    {
        appliedNumBands = requested;
        for (int i = 0; i < kMaxCrossovers; ++i)
            appliedCrossoverHz[i] = global.crossoverHz[i];
    }

    globalParams = global;

    smoothedInputGain.setTargetValue(juce::Decibels::decibelsToGain(juce::jlimit(-24.0f, 24.0f, global.inputGainDb)));
    smoothedOutputGain.setTargetValue(juce::Decibels::decibelsToGain(juce::jlimit(-24.0f, 24.0f, global.outputGainDb)));
    smoothedGlobalMix.setTargetValue(juce::jlimit(0.0f, 1.0f, global.mix01));

    const int n = juce::jmin(numBandParams, kMaxBands);
    bool anySolo = false;
    for (int b = 0; b < n; ++b)
        anySolo = anySolo || bandsIn[b].solo;

    for (int b = 0; b < n; ++b)
    {
        bandParams[static_cast<size_t>(b)] = bandsIn[b];
        bands[static_cast<size_t>(b)].setParameters(bandsIn[b]);

        // Solo mutes every non-soloed band; without any solo, all bands pass.
        // This is a gain ramp rather than a hard mute so it cannot click.
        const bool audible = (b < activeNumBands) && (!anySolo || bandsIn[b].solo);
        bandGateGain[static_cast<size_t>(b)].setTargetValue(audible ? 1.0f : 0.0f);
    }
    for (int b = n; b < kMaxBands; ++b)
        bandGateGain[static_cast<size_t>(b)].setTargetValue(0.0f);
}

void EmberEngine::process(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    const int numCh = juce::jmin(numChannels, buffer.getNumChannels());

    // Scrub the input here as well as in the plug-in wrapper: this is a public
    // entry point that the tests, the benchmark and the offline render tool all
    // drive directly, and everything downstream holds recursive state that a
    // single non-finite sample latches for good.
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            d[i] = dsputil::sanitise(d[i]);
    }

    // ---- input gain ----
    {
        float* p[2] = {nullptr, nullptr};
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            p[ch] = buffer.getWritePointer(ch);

        const float g0 = smoothedInputGain.getCurrentValue();
        smoothedInputGain.skip(numSamples);
        const float g1 = smoothedInputGain.getCurrentValue();
        const float dg = (g1 - g0) / static_cast<float>(juce::jmax(1, numSamples));

        float g = g0;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
                p[ch][i] *= g;
            g += dg;
        }
    }

    // ---- dry tap for the global mix, delayed by the plugin's own latency ----
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    if (latencySamples > 0)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = dryBuffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                globalDryDelay.pushSample(ch, d[i]);
                d[i] = globalDryDelay.popSample(ch);
            }
        }
    }

    // ---- mid/side encode ----
    const bool midSide = (globalParams.stereoMode == StereoMode::MidSide) && numCh >= 2;
    if (midSide)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            const float s = 0.5f * (l[i] - r[i]);
            l[i] = m;
            r[i] = s;
        }
    }

    // ---- band split (with band-count crossfade) ----
    activeCrossover->process(buffer, bandBuffers, numSamples);

    const bool fading = (bandFade < 1.0f) && (previousCrossover != nullptr);
    if (fading)
    {
        previousCrossover->process(buffer, altBandBuffers, numSamples);

        // Every band advances through the same fade ramp, so the end point is
        // the same for all of them and can be computed once.
        const float fadeAtEnd = bandFade + bandFadeStep * static_cast<float>(numSamples);
        const int fadeBands = juce::jmax(activeNumBands, previousNumBands);
        for (int b = 0; b < fadeBands; ++b)
        {
            auto& dst = bandBuffers[static_cast<size_t>(b)];
            const bool inNew = b < activeNumBands;
            const bool inOld = b < previousNumBands;
            float f = bandFade;
            for (int i = 0; i < numSamples; ++i)
            {
                const float ff = juce::jlimit(0.0f, 1.0f, f);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    const float nv = inNew ? dst.getSample(ch, i) : 0.0f;
                    const float ov = inOld ? altBandBuffers[static_cast<size_t>(b)].getSample(ch, i) : 0.0f;
                    dst.getWritePointer(ch)[i] = ff * nv + (1.0f - ff) * ov;
                }
                f += bandFadeStep;
            }
        }
        bandFade = juce::jlimit(0.0f, 1.0f, fadeAtEnd);
        if (bandFade >= 1.0f)
            previousCrossover = nullptr;
    }

    // ---- per-band processing and sum ----
    sumBuffer.clear();
    const int bandsToRun = fading ? juce::jmax(activeNumBands, previousNumBands) : activeNumBands;

    for (int b = 0; b < bandsToRun; ++b)
    {
        auto& bb = bandBuffers[static_cast<size_t>(b)];
        bands[static_cast<size_t>(b)].process(bb, numSamples);

        float peak = 0.0f;
        auto& gate = bandGateGain[static_cast<size_t>(b)];

        const float gg0 = gate.getCurrentValue();
        gate.skip(numSamples);
        const float gg1 = gate.getCurrentValue();
        const float dgg = (gg1 - gg0) / static_cast<float>(juce::jmax(1, numSamples));

        const float* src[2] = {nullptr, nullptr};
        float* dst[2] = {nullptr, nullptr};
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
        {
            src[ch] = bb.getReadPointer(ch);
            dst[ch] = sumBuffer.getWritePointer(ch);
        }

        float g = gg0;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float v = src[ch][i] * g;
                dst[ch][i] += v;
                peak = juce::jmax(peak, std::abs(v));
            }
            g += dgg;
        }
        bandLevels[static_cast<size_t>(b)].store(peak, std::memory_order_relaxed);
    }
    for (int b = bandsToRun; b < kMaxBands; ++b)
    {
        bandGateGain[static_cast<size_t>(b)].skip(numSamples);
        bandLevels[static_cast<size_t>(b)].store(0.0f, std::memory_order_relaxed);
    }

    // ---- mid/side decode ----
    if (midSide)
    {
        auto* m = sumBuffer.getWritePointer(0);
        auto* s = sumBuffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float l = m[i] + s[i];
            const float r = m[i] - s[i];
            m[i] = l;
            s[i] = r;
        }
    }

    // ---- auto-gain: slow RMS match of the wet path back to the dry path ----
    if (globalParams.autoGain)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float dAcc = 0.0f, wAcc = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float d = dryBuffer.getSample(ch, i);
                const float w = sumBuffer.getSample(ch, i);
                dAcc += d * d;
                wAcc += w * w;
            }
            dryRms = dAcc + autoGainCoeff * (dryRms - dAcc);
            wetRms = wAcc + autoGainCoeff * (wetRms - wAcc);
        }
        // These followers are recursive, so a single non-finite sample would
        // otherwise latch them at NaN for the rest of the session and silence
        // the plug-in permanently - measured at -169 dB and never recovering.
        if (!(std::isfinite(dryRms) && std::isfinite(wetRms)))
            dryRms = wetRms = 0.0f;

        // Only trust the ratio once there is something to measure, and clamp it
        // so a near-silent wet path cannot ask for enormous make-up gain.
        const float target =
            (wetRms > 1.0e-9f && dryRms > 1.0e-9f) ? juce::jlimit(0.25f, 4.0f, std::sqrt(dryRms / wetRms)) : 1.0f;
        smoothedAutoGain.setTargetValue(target);
    }
    else
    {
        smoothedAutoGain.setTargetValue(1.0f);
    }

    // ---- global mix, auto-gain, output gain ----
    {
        float* out[2] = {nullptr, nullptr};
        const float* wetp[2] = {nullptr, nullptr};
        const float* dryp[2] = {nullptr, nullptr};
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
        {
            out[ch] = buffer.getWritePointer(ch);
            wetp[ch] = sumBuffer.getReadPointer(ch);
            dryp[ch] = dryBuffer.getReadPointer(ch);
        }

        const float inv = 1.0f / static_cast<float>(juce::jmax(1, numSamples));
        const float ag0 = smoothedAutoGain.getCurrentValue();
        const float mx0 = smoothedGlobalMix.getCurrentValue();
        const float og0 = smoothedOutputGain.getCurrentValue();
        smoothedAutoGain.skip(numSamples);
        smoothedGlobalMix.skip(numSamples);
        smoothedOutputGain.skip(numSamples);
        const float dAg = (smoothedAutoGain.getCurrentValue() - ag0) * inv;
        const float dMx = (smoothedGlobalMix.getCurrentValue() - mx0) * inv;
        const float dOg = (smoothedOutputGain.getCurrentValue() - og0) * inv;

        float ag = ag0, mix = mx0, og = og0;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float wet = wetp[ch][i] * ag;
                out[ch][i] = (mix * wet + (1.0f - mix) * dryp[ch][i]) * og;
            }
            ag += dAg;
            mix += dMx;
            og += dOg;
        }
    }

    if (spectrumEnabled.load(std::memory_order_relaxed))
        pushSpectrum(dryBuffer, buffer, numSamples);
}

void EmberEngine::pushSpectrum(const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out,
                               int numSamples) noexcept
{
    const int numCh = juce::jmin(numChannels, out.getNumChannels());
    for (int i = 0; i < numSamples; ++i)
    {
        float mi = 0.0f, mo = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            mi += in.getSample(ch, i);
            mo += out.getSample(ch, i);
        }
        const float inv = 1.0f / static_cast<float>(juce::jmax(1, numCh));
        inputAccum[static_cast<size_t>(accumIndex)] = mi * inv;
        outputAccum[static_cast<size_t>(accumIndex)] = mo * inv;
        if (++accumIndex >= kSpectrumFFTSize)
        {
            accumIndex = 0;
            accumulateSpectrum(inputAccum.data(), outputAccum.data(), kSpectrumFFTSize);
        }
    }
}

void EmberEngine::accumulateSpectrum(const float* input, const float* output, int numSamples) noexcept
{
    auto analyse = [this, numSamples](const float* src, std::array<float, kSpectrumBins>& dest)
    {
        std::fill(fftScratch.begin(), fftScratch.end(), 0.0f);
        for (int i = 0; i < numSamples; ++i)
            fftScratch[static_cast<size_t>(i)] = src[i] * window[static_cast<size_t>(i)];

        fft.performFrequencyOnlyForwardTransform(fftScratch.data(), true);

        const float norm = 2.0f / static_cast<float>(kSpectrumFFTSize);
        for (int bin = 0; bin < kSpectrumBins; ++bin)
        {
            const float mag = fftScratch[static_cast<size_t>(bin)] * norm;
            dest[static_cast<size_t>(bin)] = juce::jmax(-120.0f, juce::Decibels::gainToDecibels(mag + 1.0e-12f));
        }
    };

    analyse(input, scratchFrame.inputDb);
    analyse(output, scratchFrame.outputDb);
    spectrumFifo.push(scratchFrame);
}
} // namespace ember
