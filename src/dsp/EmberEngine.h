#pragma once
#include <array>
#include <juce_dsp/juce_dsp.h>
#include "dsp/EmberTypes.h"
#include "dsp/BandParams.h"
#include "dsp/BandChain.h"
#include "dsp/Crossover.h"
#include "dsp/SpectrumFifo.h"

namespace ember
{
/**
    The whole audio graph below the plugin wrapper.

    Input gain -> [M/S encode] -> crossover -> 6 band chains -> sum ->
    [M/S decode] -> auto-gain -> global dry/wet -> output gain.

    Parameters arrive already resolved: the processor layer applies modulation
    and hands down plain `GlobalParams` / `BandParams`, so nothing here needs to
    know about APVTS, modulation or the GUI.

    Band-count changes are click-free by construction. Two crossovers are kept;
    on a change the engine crossfades each band's INPUT from the old split to
    the new one over ~20 ms. Because a crossover is transparent — the bands sum
    back to the input — the total at fade position f is

        f * sum(newBands) + (1 - f) * sum(oldBands) = input

    for every f, so the sum never jumps even though the band boundaries move.
    Bands that do not exist in a configuration simply contribute silence and
    fade in or out continuously.
*/
class EmberEngine
{
public:
    EmberEngine();
    ~EmberEngine();

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Message/prepare thread only — reallocates the oversamplers. */
    void setOversamplingFactor(OversamplingFactor factor);

    /** Select the oversampling filter family. See BandChain::prepare.
        Message/prepare thread only — it reallocates the oversamplers. */
    void setOversamplingQuality(bool linearPhase);
    void setCrossoverMode(CrossoverMode mode);

    /** Control-rate update. Realtime-safe. */
    void setParameters(const GlobalParams& global, const BandParams* bands, int numBandParams) noexcept;

    void process(juce::AudioBuffer<float>& buffer) noexcept;

    /** Total latency in samples, for `setLatencySamples`. */
    int getLatencySamples() const noexcept;

    SpectrumFifo& getSpectrumFifo() noexcept { return spectrumFifo; }

    /** The FFT is skipped entirely while no editor is open. A session can hold
        dozens of instances with the window closed, and analysing spectra nobody
        is looking at is the easiest CPU in the plugin to give back. */
    void setSpectrumEnabled(bool shouldAnalyse) noexcept
    {
        spectrumEnabled.store(shouldAnalyse, std::memory_order_relaxed);
    }

    float getBandGainReductionDb(int band) const noexcept;

    /** Peak level of each band's output, for the GUI band "heat" overlay. */
    float getBandLevel(int band) const noexcept;

private:
    void pushSpectrum(const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out, int numSamples) noexcept;
    void accumulateSpectrum(const float* input, const float* output, int numSamples) noexcept;

    std::array<BandChain, kMaxBands> bands;
    Crossover crossoverA, crossoverB;
    Crossover* activeCrossover{&crossoverA};
    Crossover* previousCrossover{nullptr};

    std::array<juce::AudioBuffer<float>, kMaxBands> bandBuffers;
    std::array<juce::AudioBuffer<float>, kMaxBands> altBandBuffers;
    juce::AudioBuffer<float> dryBuffer, sumBuffer, msBuffer;

    /** Whole-sample latency, as in BandChain: no interpolation needed. */
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> globalDryDelay{8192};

    // Band-count crossfade
    float bandFade{1.0f};
    float bandFadeStep{1.0f};
    int activeNumBands{3};
    int previousNumBands{3};

    /** Last crossover edges actually pushed into the filters. Frequencies are
        only re-sent when one genuinely moves: in linear-phase mode a re-send
        costs a FIR redesign plus an FFT per edge on the calling thread, which
        is the audio thread here, and at 192 kHz that is more than a whole
        32-sample control block's budget. A static crossover must cost nothing. */
    float appliedCrossoverHz[kMaxCrossovers]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int appliedNumBands{-1};

    std::array<juce::SmoothedValue<float>, kMaxBands> bandGateGain; // solo / bypass-to-silence
    std::array<std::atomic<float>, kMaxBands> bandLevels{};

    juce::SmoothedValue<float> smoothedInputGain, smoothedOutputGain, smoothedGlobalMix, smoothedAutoGain;

    // Auto-gain: slow RMS comparison of the dry and wet paths.
    float dryRms{0.0f}, wetRms{0.0f};
    float autoGainCoeff{0.0f};

    // Spectrum
    juce::dsp::FFT fft{kSpectrumFFTOrder};
    std::array<float, kSpectrumFFTSize> window{};
    std::array<float, kSpectrumFFTSize> inputAccum{}, outputAccum{};
    std::array<float, 2 * kSpectrumFFTSize> fftScratch{};
    int accumIndex{0};
    SpectrumFifo spectrumFifo;
    std::atomic<bool> spectrumEnabled{false};
    SpectrumFrame scratchFrame;

    GlobalParams globalParams;
    std::array<BandParams, kMaxBands> bandParams;

    double sampleRate{44100.0};
    int maxBlockSize{512};
    int numChannels{2};
    OversamplingFactor osFactor{OversamplingFactor::x2};
    bool linearPhaseOversampling{false};
    int latencySamples{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberEngine)
};
} // namespace ember
