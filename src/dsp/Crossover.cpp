#include "dsp/Crossover.h"

#include "dsp/DspUtils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace ember
{
namespace
{
// ------------------------------------------------------------------ limits
constexpr float kMinCrossoverHz = 20.0f;
constexpr float kMaxCrossoverHz = 20000.0f;

/** Crossovers are forced at least this far apart (ratio) so a caller that hands
    us a descending or duplicated list still yields a usable, stable filter set. */
/** One third of an octave between adjacent edges.

    This was 1.02 - about 1/35 of an octave - while the GUI enforced 1.26 on a
    drag and the manual promised "about a third of an octave so bands cannot
    collapse into each other". Both were true of the mouse and neither was true
    of automation or modulation, which could drive two edges almost on top of
    each other and produce a band a few hertz wide.

    The engine is the authority on what the engine will accept, so it enforces
    the number the interface has been claiming. */
constexpr float kMinCrossoverRatio = 1.26f;

/**
    `juce::dsp::LinkwitzRileyFilter<float>` written out.

    The same topology-preserving structure, the same coefficient update and the
    same order of operations, so the output is bit-identical. What it is not is
    a call: JUCE instantiates that filter in a translation unit of its own, so
    every splitter sample and every all-pass sample went through a real function
    call that could neither be inlined nor scheduled against its neighbours. A
    six-band split is five splitter calls per sample plus ten all-pass passes
    per channel over the block, and the crossover was 13% of the engine.

    The state is held per channel here rather than inside the filter so both
    channels of a stereo pair can be driven through the same section in one
    iteration — the splitter tree is a chain of five dependent sections, about
    thirty operations deep, and a single channel leaves the core waiting on it.
*/
struct LrTpt
{
    void prepare(double newSampleRate, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        state.assign(static_cast<size_t>(juce::jmax(1, numChannels)) * 4u, 0.0f);
        update();
    }

    void setCutoffFrequency(float newCutoffHz) noexcept
    {
        cutoffHz = newCutoffHz;
        update();
    }

    void reset() noexcept { std::fill(state.begin(), state.end(), 0.0f); }

    void snapToZero() noexcept
    {
        for (auto& v : state)
            juce::dsp::util::snapToZero(v);
    }

    float* channelState(int channel) noexcept { return state.data() + 4u * static_cast<size_t>(channel); }

    /** The band-split output pair: the 4th-order lowpass and the complementary
        highpass that sums with it to this filter's own 2nd-order all-pass. */
    void processLowHigh(float* s, float x, float& low, float& high) const noexcept
    {
        const float yH = (x - (R2 + g) * s[0] - s[1]) * h;
        const float yB = g * yH + s[0];
        s[0] = g * yH + yB;
        const float yL = g * yB + s[1];
        s[1] = g * yB + yL;

        const float yH2 = (yL - (R2 + g) * s[2] - s[3]) * h;
        const float yB2 = g * yH2 + s[2];
        s[2] = g * yH2 + yB2;
        const float yL2 = g * yB2 + s[3];
        s[3] = g * yB2 + yL2;

        low = yL2;
        high = yL - R2 * yB + yH - yL2;
    }

    /** That 2nd-order all-pass on its own, for the phase compensation. Only the
        first section's state is used, exactly as JUCE's all-pass mode does. */
    float processAllpass(float* s, float x) const noexcept
    {
        const float yH = (x - (R2 + g) * s[0] - s[1]) * h;
        const float yB = g * yH + s[0];
        s[0] = g * yH + yB;
        const float yL = g * yB + s[1];
        s[1] = g * yB + yL;

        return yL - R2 * yB + yH;
    }

private:
    void update() noexcept
    {
        g = static_cast<float>(std::tan(juce::MathConstants<double>::pi * static_cast<double>(cutoffHz) / sampleRate));
        R2 = static_cast<float>(std::sqrt(2.0));
        h = static_cast<float>(1.0 / (1.0 + static_cast<double>(R2 * g) + static_cast<double>(g * g)));
    }

    std::vector<float> state;
    double sampleRate{44100.0};
    float cutoffHz{2000.0f};
    float g{0.0f}, R2{0.0f}, h{0.0f};
};

/** Linear-phase prototypes are ~10 ms per side. Long enough for a usable
    transition width down to ~60 Hz, short enough that the reported latency
    stays inside what every host comfortably compensates. */
constexpr double kFirHalfSeconds = 0.010;
constexpr int kMinFirHalfLength = 192;
constexpr int kMaxFirHalfLength = 4096;

/** Kaiser beta ~8.6 gives roughly a -90 dB stopband, well under the -100 dB
    residual the null test asks of the *sum* (which is exact by construction). */
constexpr double kKaiserBeta = 8.6;

/** sin(pi x) / (pi x), with the removable singularity filled in. */
double sincPi(double x) noexcept
{
    if (std::abs(x) < 1.0e-12)
        return 1.0;

    const double px = juce::MathConstants<double>::pi * x;
    return std::sin(px) / px;
}

/** Zeroth-order modified Bessel function of the first kind (Kaiser window). */
double besselI0(double x) noexcept
{
    const double half = 0.5 * x;
    double term = 1.0;
    double sum = 1.0;

    for (int k = 1; k < 64; ++k)
    {
        term *= half / static_cast<double>(k);
        const double squared = term * term;
        sum += squared;

        if (squared < sum * 1.0e-20)
            break;
    }

    return sum;
}

/** Smallest FFT order whose transform size is >= `minimumSize`. */
int fftOrderFor(int minimumSize) noexcept
{
    int order = 4;

    while ((1 << order) < minimumSize && order < 18)
        ++order;

    return order;
}
} // namespace

//==============================================================================
/**
    MinimumPhaseLR4
    ---------------
    Serial LR4 tree: crossover j splits the running "high" signal into band j
    (its low output) and the signal that feeds crossover j + 1.

    A JUCE LinkwitzRileyFilter produces low + high == its own 2nd-order all-pass
    A_j, so the split is lossless *at that node*. Band j however never sees
    A_{j+1} .. A_{n-1}, while every later band does — a naive cascade therefore
    sums with a phase mismatch and a deep notch. Each band is run through the
    matching all-pass sections for the crossovers it skipped, which makes the
    total transfer function A_0 * A_1 * ... * A_{n-1} for *every* path:

        T_k(y)   = P_{k+1}(LP_k y) + T_{k+1}(HP_k y)
                 = P_{k+1}(LP_k y + HP_k y) = P_{k+1}(A_k y) = P_k(y)

    i.e. the band sum is an all-pass of the input — magnitude perfectly flat,
    zero latency.

    LinearPhase
    -----------
    Only the n-1 lowpass prototypes are actually convolved; the bands are formed
    by differencing them against each other and against the delayed dry signal:

        band 0     = LP_0
        band j     = LP_j - LP_{j-1}
        band n-1   = delayed - LP_{n-2}

    The sum telescopes to the delayed dry signal exactly, so flatness does not
    depend on the quality of the FIR design at all — only band separation does.
    Convolution is uniform overlap-add against a shared forward transform of the
    input, so the reported latency is exactly the prototype's own (L - 1) / 2.
*/
struct Crossover::Impl
{
    // ------------------------------------------------------------- topology
    double sampleRate{44100.0};
    int maxBlockSize{0};
    int numChannels{0};
    bool prepared{false};

    CrossoverMode mode{CrossoverMode::MinimumPhaseLR4};
    int numBands{1};

    std::array<float, static_cast<size_t>(kMaxCrossovers)> freqs{{100.0f, 300.0f, 900.0f, 2500.0f, 7000.0f}};

    /** What the filters and FIR prototypes are actually tuned to right now, so
        an unchanged edge never pays for a redesign. */
    std::array<float, static_cast<size_t>(kMaxCrossovers)> appliedFreqs{{}};

    /** Edges whose linear-phase prototype no longer matches `appliedFreqs`.
        Only ever set while MinimumPhaseLR4 is the active mode, where the
        prototypes are not read by anything; flushed by prepare() and by the
        switch into LinearPhase, both of which are off-the-audio-thread calls. */
    std::array<bool, static_cast<size_t>(kMaxCrossovers)> firDirty{{}};

    // ------------------------------------------------------------ LR4 state
    using LrFilter = LrTpt;

    std::array<LrFilter, static_cast<size_t>(kMaxCrossovers)> splitters;

    /** `allpass[b][j]` compensates band `b` for crossover `j` (only j > b used). */
    std::array<std::array<LrFilter, static_cast<size_t>(kMaxCrossovers)>, static_cast<size_t>(kMaxBands)> allpass;

    // --------------------------------------------------- linear-phase state
    std::unique_ptr<juce::dsp::FFT> fft;
    int firLength{0};  ///< odd
    int firLatency{0}; ///< (firLength - 1) / 2
    int fftSize{0};
    int hopSize{0};    ///< fftSize - firLength + 1
    int tailLength{0}; ///< firLength - 1

    std::vector<double> window;       ///< firLength Kaiser coefficients
    std::vector<double> tapScratch;   ///< firLength design taps
    std::vector<float> designScratch; ///< 2 * fftSize, design side only
    std::vector<float> irSpectra;     ///< kMaxCrossovers * 2 * fftSize
    std::vector<float> fwdScratch;    ///< 2 * fftSize, audio thread
    std::vector<float> mulScratch;    ///< 2 * fftSize, audio thread
    std::vector<float> olaTail;       ///< kMaxCrossovers * numChannels * tailLength
    std::vector<float> delayRing;     ///< numChannels * firLatency
    std::vector<int> delayPos;        ///< numChannels

    // ----------------------------------------------------------------------
    void prepare(double newSampleRate, int newMaxBlockSize, int newNumChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        maxBlockSize = juce::jmax(1, newMaxBlockSize);
        numChannels = juce::jlimit(1, 32, newNumChannels);

        for (auto& f : splitters)
            f.prepare(sampleRate, numChannels);

        for (auto& row : allpass)
            for (auto& f : row)
                f.prepare(sampleRate, numChannels);

        // Both modes are planned up front so setMode() never has to build
        // anything while the engine is live.
        planLinearPhase();
        prepared = true;

        // Re-clamp and re-apply: the new rate may have moved the ceiling, and
        // the FIR prototypes have to be designed against the new rate.
        applyFrequencies(true);
        refreshPrototypes();
        resetState();
    }

    /** Bring every stale linear-phase prototype back in sync with the applied
        edge frequencies. Off the audio thread only: prepare() and the switch
        into LinearPhase, which the header already documents as expensive. */
    void refreshPrototypes() noexcept
    {
        if (!prepared)
            return;

        for (int i = 0; i < kMaxCrossovers; ++i)
        {
            const size_t idx = static_cast<size_t>(i);

            if (!firDirty[idx])
                continue;

            designLowpass(i, appliedFreqs[idx]);
            firDirty[idx] = false;
        }
    }

    void resetState() noexcept
    {
        for (auto& f : splitters)
            f.reset();

        for (auto& row : allpass)
            for (auto& f : row)
                f.reset();

        std::fill(olaTail.begin(), olaTail.end(), 0.0f);
        std::fill(delayRing.begin(), delayRing.end(), 0.0f);
        std::fill(delayPos.begin(), delayPos.end(), 0);
    }

    // --------------------------------------------------------- linear phase
    void planLinearPhase()
    {
        const int half = juce::jlimit(kMinFirHalfLength, kMaxFirHalfLength,
                                      static_cast<int>(std::lround(sampleRate * kFirHalfSeconds)));

        firLength = 2 * half + 1;
        firLatency = half;
        tailLength = firLength - 1;

        const int order = fftOrderFor(2 * firLength);
        fftSize = 1 << order;
        hopSize = juce::jmax(1, fftSize - firLength + 1);

        fft = std::make_unique<juce::dsp::FFT>(order);

        const size_t spectrumFloats = static_cast<size_t>(2 * fftSize);

        window.assign(static_cast<size_t>(firLength), 0.0);
        tapScratch.assign(static_cast<size_t>(firLength), 0.0);
        designScratch.assign(spectrumFloats, 0.0f);
        fwdScratch.assign(spectrumFloats, 0.0f);
        mulScratch.assign(spectrumFloats, 0.0f);
        irSpectra.assign(static_cast<size_t>(kMaxCrossovers) * spectrumFloats, 0.0f);

        olaTail.assign(static_cast<size_t>(kMaxCrossovers) * static_cast<size_t>(numChannels) *
                           static_cast<size_t>(tailLength),
                       0.0f);

        delayRing.assign(static_cast<size_t>(numChannels) * static_cast<size_t>(firLatency), 0.0f);
        delayPos.assign(static_cast<size_t>(numChannels), 0);

        // Kaiser window, computed once and reused by every prototype design.
        const double denom = besselI0(kKaiserBeta);
        const double centre = static_cast<double>(half);

        for (int n = 0; n < firLength; ++n)
        {
            const double r = (static_cast<double>(n) - centre) / centre;
            const double arg = 1.0 - r * r;
            window[static_cast<size_t>(n)] = besselI0(kKaiserBeta * std::sqrt(arg > 0.0 ? arg : 0.0)) / denom;
        }
    }

    /** Window-method linear-phase lowpass, normalised to unity DC gain, then
        transformed once into the frequency domain. Never called from process().
        Allocation-free: everything it touches was sized in planLinearPhase(). */
    void designLowpass(int index, float cutoffHz) noexcept
    {
        if (fft == nullptr || firLength <= 0)
            return;

        jassert(index >= 0 && index < kMaxCrossovers);

        const double fcNorm = juce::jlimit(1.0e-5, 0.4999, static_cast<double>(cutoffHz) / sampleRate);
        const double centre = 0.5 * static_cast<double>(firLength - 1);

        double* taps = tapScratch.data();
        const double* win = window.data();
        double sum = 0.0;

        for (int n = 0; n < firLength; ++n)
        {
            const double t = static_cast<double>(n) - centre;
            const double v = 2.0 * fcNorm * sincPi(2.0 * fcNorm * t) * win[n];
            taps[n] = v;
            sum += v;
        }

        if (std::abs(sum) > 1.0e-12)
        {
            const double norm = 1.0 / sum;
            for (int n = 0; n < firLength; ++n)
                taps[n] *= norm;
        }

        float* scratch = designScratch.data();
        std::fill(designScratch.begin(), designScratch.end(), 0.0f);

        for (int n = 0; n < firLength; ++n)
            scratch[n] = static_cast<float>(taps[n]);

        fft->performRealOnlyForwardTransform(scratch, true);

        float* dest = irSpectra.data() + static_cast<size_t>(index) * static_cast<size_t>(2 * fftSize);
        std::copy(scratch, scratch + 2 * fftSize, dest);
    }

    // ------------------------------------------------------------ frequency
    /** Clamp defensively, force strictly ascending, then push each edge into
        every filter that uses it. A bad caller therefore cannot produce an
        unstable coefficient set, and an edge that did not actually move never
        pays for a filter update or (in LinearPhase) a prototype redesign.
        `force` re-applies everything — used after prepare(), where the sample
        rate and therefore the whole design has moved. */
    void applyFrequencies(bool force) noexcept
    {
        const float ceilingHz = juce::jmin(kMaxCrossoverHz, static_cast<float>(sampleRate * 0.49));
        const float floorHz = juce::jmin(kMinCrossoverHz, ceilingHz);

        for (int i = 0; i < kMaxCrossovers; ++i)
        {
            const size_t idx = static_cast<size_t>(i);
            float f = freqs[idx];

            if (!std::isfinite(f))
                f = floorHz;

            f = juce::jlimit(floorHz, ceilingHz, f);

            if (i > 0)
                f = juce::jlimit(floorHz, ceilingHz, juce::jmax(f, freqs[idx - 1] * kMinCrossoverRatio));

            freqs[idx] = f;

            if (!force && std::abs(f - appliedFreqs[idx]) <= 1.0e-4f)
                continue;

            appliedFreqs[idx] = f;
            splitters[idx].setCutoffFrequency(f);

            for (auto& row : allpass)
                row[idx].setCutoffFrequency(f);

            // A prototype redesign is a full window-method FIR plus a forward
            // FFT — tens of microseconds per edge. EmberEngine drives this
            // setter from processBlock() once per control block, so paying for
            // it in MinimumPhaseLR4 mode, where no prototype is ever read, is
            // pure audio-thread waste (at 192 kHz, five moving edges cost more
            // than a whole 32-sample control block). Defer it instead: the
            // switch into LinearPhase flushes whatever went stale.
            if (!prepared)
                continue;

            if (mode == CrossoverMode::LinearPhase)
            {
                designLowpass(i, f);
                firDirty[idx] = false;
            }
            else
            {
                firDirty[idx] = true;
            }
        }
    }

    // -------------------------------------------------------------- process
    /** The splitter tree for `NumCh` channels starting at `firstCh`. */
    template<int NumCh>
    void splitPass(const juce::AudioBuffer<float>& input, std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut,
                   int numSamples, int firstCh) noexcept
    {
        const int nb = numBands;
        const int nx = nb - 1;

        const float* src[NumCh];
        float* dst[static_cast<size_t>(kMaxBands)][NumCh];
        float* st[static_cast<size_t>(kMaxCrossovers)][NumCh];

        for (int k = 0; k < NumCh; ++k)
        {
            src[k] = input.getReadPointer(firstCh + k);

            for (int b = 0; b < nb; ++b)
                dst[b][k] = bandOut[static_cast<size_t>(b)].getWritePointer(firstCh + k);

            for (int j = 0; j < nx; ++j)
                st[j][k] = splitters[static_cast<size_t>(j)].channelState(firstCh + k);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            float x[NumCh];

            for (int k = 0; k < NumCh; ++k)
                x[k] = dsputil::sanitise(src[k][i]);

            for (int j = 0; j < nx; ++j)
            {
                const auto& f = splitters[static_cast<size_t>(j)];

                for (int k = 0; k < NumCh; ++k)
                {
                    float lo = 0.0f, hi = 0.0f;
                    f.processLowHigh(st[j][k], x[k], lo, hi);
                    dst[j][k][i] = lo;
                    x[k] = hi;
                }
            }

            for (int k = 0; k < NumCh; ++k)
                dst[nb - 1][k][i] = x[k];
        }
    }

    /** Phase compensation: band b skipped every crossover after b. */
    template<int NumCh>
    void allpassPass(std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut, int numSamples, int firstCh) noexcept
    {
        const int nx = numBands - 1;

        for (int b = 0; b < nx; ++b)
        {
            float* d[NumCh];

            for (int k = 0; k < NumCh; ++k)
                d[k] = bandOut[static_cast<size_t>(b)].getWritePointer(firstCh + k);

            for (int j = b + 1; j < nx; ++j)
            {
                auto& ap = allpass[static_cast<size_t>(b)][static_cast<size_t>(j)];
                float* st[NumCh];

                for (int k = 0; k < NumCh; ++k)
                    st[k] = ap.channelState(firstCh + k);

                for (int i = 0; i < numSamples; ++i)
                    for (int k = 0; k < NumCh; ++k)
                        d[k][i] = ap.processAllpass(st[k], d[k][i]);
            }
        }
    }

    void processLR4(const juce::AudioBuffer<float>& input, std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut,
                    int numSamples, int nCh) noexcept
    {
        const int nx = numBands - 1;

        // Channels are taken in pairs: every filter state is per channel, so the
        // result is unchanged, but the second channel fills the gaps in the
        // first one's dependency chain.
        for (int ch = 0; ch < nCh; ch += 2)
        {
            if (nCh - ch >= 2)
            {
                splitPass<2>(input, bandOut, numSamples, ch);
                allpassPass<2>(bandOut, numSamples, ch);
            }
            else
            {
                splitPass<1>(input, bandOut, numSamples, ch);
                allpassPass<1>(bandOut, numSamples, ch);
            }
        }

        for (int j = 0; j < nx; ++j)
            splitters[static_cast<size_t>(j)].snapToZero();

        for (int b = 0; b < nx; ++b)
            for (int j = b + 1; j < nx; ++j)
                allpass[static_cast<size_t>(b)][static_cast<size_t>(j)].snapToZero();
    }

    /** One overlap-add convolution pass for crossover `j`, channel `ch`. */
    void convolveChunk(int j, int ch, const float* spectrum, float* dest, int chunk) noexcept
    {
        const size_t spectrumFloats = static_cast<size_t>(2 * fftSize);
        const float* ir = irSpectra.data() + static_cast<size_t>(j) * spectrumFloats;
        float* work = mulScratch.data();

        const int numBins = fftSize / 2; // bins 0 .. fftSize/2 inclusive

        for (int k = 0; k <= numBins; ++k)
        {
            const float ar = spectrum[2 * k];
            const float ai = spectrum[2 * k + 1];
            const float br = ir[2 * k];
            const float bi = ir[2 * k + 1];

            work[2 * k] = ar * br - ai * bi;
            work[2 * k + 1] = ar * bi + ai * br;
        }

        fft->performRealOnlyInverseTransform(work);

        float* tail =
            olaTail.data() + (static_cast<size_t>(j) * static_cast<size_t>(numChannels) + static_cast<size_t>(ch)) *
                                 static_cast<size_t>(tailLength);

        const int overlap = juce::jmin(chunk, tailLength);

        for (int i = 0; i < overlap; ++i)
            dest[i] = work[i] + tail[i];

        for (int i = overlap; i < chunk; ++i)
            dest[i] = work[i];

        // New tail: what is left of the old one, plus this block's own overhang.
        // Written forwards, reading strictly ahead of the write cursor.
        for (int i = 0; i < tailLength; ++i)
        {
            const int shifted = i + chunk;
            float v = work[shifted];

            if (shifted < tailLength)
                v += tail[shifted];

            tail[i] = v;
        }
    }

    void processLinearPhase(const juce::AudioBuffer<float>& input,
                            std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut, int numSamples, int nCh) noexcept
    {
        const int nb = numBands;
        const int nx = nb - 1;

        for (int ch = 0; ch < nCh; ++ch)
        {
            const float* src = input.getReadPointer(ch);

            // ---- dry, delayed by exactly the reported latency -> top band
            float* dry = bandOut[static_cast<size_t>(nb - 1)].getWritePointer(ch);

            if (firLatency > 0)
            {
                float* ring = delayRing.data() + static_cast<size_t>(ch) * static_cast<size_t>(firLatency);
                int p = delayPos[static_cast<size_t>(ch)];

                for (int i = 0; i < numSamples; ++i)
                {
                    const float in = dsputil::sanitise(src[i]);
                    dry[i] = ring[p];
                    ring[p] = in;

                    if (++p >= firLatency)
                        p = 0;
                }

                delayPos[static_cast<size_t>(ch)] = p;
            }
            else
            {
                for (int i = 0; i < numSamples; ++i)
                    dry[i] = dsputil::sanitise(src[i]);
            }

            // ---- lowpass prototypes, uniform overlap-add
            int pos = 0;

            while (pos < numSamples)
            {
                const int chunk = juce::jmin(hopSize, numSamples - pos);
                float* fwd = fwdScratch.data();

                for (int i = 0; i < chunk; ++i)
                    fwd[i] = dsputil::sanitise(src[pos + i]);

                std::fill(fwd + chunk, fwd + 2 * fftSize, 0.0f);
                fft->performRealOnlyForwardTransform(fwd, true);

                for (int j = 0; j < nx; ++j)
                    convolveChunk(j, ch, fwd, bandOut[static_cast<size_t>(j)].getWritePointer(ch) + pos, chunk);

                pos += chunk;
            }

            // ---- difference adjacent lowpasses, top band last-to-first so the
            //      in-place subtraction never reads an already-modified band.
            for (int b = nb - 1; b >= 1; --b)
            {
                float* hiBand = bandOut[static_cast<size_t>(b)].getWritePointer(ch);
                const float* loBand = bandOut[static_cast<size_t>(b - 1)].getReadPointer(ch);

                for (int i = 0; i < numSamples; ++i)
                    hiBand[i] -= loBand[i];
            }
        }
    }

    void process(const juce::AudioBuffer<float>& input, std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut,
                 int numSamples) noexcept
    {
        const int nb = juce::jlimit(kMinBands, kMaxBands, numBands);

        int ns = juce::jmin(numSamples, input.getNumSamples());

        // Before prepare() there is no filter state to bound the channel count
        // against, and `numChannels` is still 0 — clamping to it would return
        // early and leave the caller's band buffers holding whatever was in
        // them, which is exactly what the pass-through fallback below exists to
        // avoid. Fall back to the input's own channel count until prepared.
        int nCh = prepared ? juce::jmin(input.getNumChannels(), numChannels) : input.getNumChannels();

        for (int b = 0; b < nb; ++b)
        {
            const auto& buf = bandOut[static_cast<size_t>(b)];
            ns = juce::jmin(ns, buf.getNumSamples());
            nCh = juce::jmin(nCh, buf.getNumChannels());
        }

        if (ns <= 0 || nCh <= 0)
            return;

        if (!prepared)
        {
            for (int b = 0; b < nb; ++b)
                bandOut[static_cast<size_t>(b)].clear(0, ns);

            for (int ch = 0; ch < nCh; ++ch)
                bandOut[0].copyFrom(ch, 0, input, ch, 0, ns);

            return;
        }

        if (nb == 1 && mode == CrossoverMode::MinimumPhaseLR4)
        {
            for (int ch = 0; ch < nCh; ++ch)
                bandOut[0].copyFrom(ch, 0, input, ch, 0, ns);

            return;
        }

        if (mode == CrossoverMode::LinearPhase)
            processLinearPhase(input, bandOut, ns, nCh);
        else
            processLR4(input, bandOut, ns, nCh);

        // One bad host buffer must not be able to poison a recursive stage for
        // the rest of the session: a NaN in an LR4 state is permanent, so scrub
        // and restart rather than emitting silence-plus-NaN forever.
        bool poisoned = false;

        for (int b = 0; b < nb && !poisoned; ++b)
            for (int ch = 0; ch < nCh; ++ch)
                if (!std::isfinite(bandOut[static_cast<size_t>(b)].getSample(ch, ns - 1)))
                {
                    poisoned = true;
                    break;
                }

        if (poisoned)
        {
            resetState();

            for (int b = 0; b < nb; ++b)
                bandOut[static_cast<size_t>(b)].clear(0, ns);
        }
    }
};

//==============================================================================
Crossover::Crossover() : impl(std::make_unique<Impl>()) {}
Crossover::~Crossover() = default;

void Crossover::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    impl->prepare(sampleRate, maxBlockSize, numChannels);
}

void Crossover::reset() noexcept
{
    impl->resetState();
}

void Crossover::setMode(CrossoverMode mode)
{
    if (mode == impl->mode)
        return;

    impl->mode = (mode == CrossoverMode::LinearPhase) ? CrossoverMode::LinearPhase : CrossoverMode::MinimumPhaseLR4;

    // Any prototype that went stale while the LR4 path was live is rebuilt
    // here, where the header already sanctions the cost.
    if (impl->mode == CrossoverMode::LinearPhase)
        impl->refreshPrototypes();

    impl->resetState();
}

void Crossover::setNumBands(int numBands) noexcept
{
    impl->numBands = juce::jlimit(kMinBands, kMaxBands, numBands);
}

int Crossover::getNumBands() const noexcept
{
    return impl->numBands;
}

float Crossover::getCrossoverFrequency(int edge) const noexcept
{
    if (edge < 0 || edge >= kMaxCrossovers)
        return 0.0f;

    return impl->freqs[static_cast<size_t>(edge)];
}

void Crossover::setCrossoverFrequencies(const float* freqs, int numEdges) noexcept
{
    if (freqs == nullptr)
        return;

    const int n = juce::jlimit(0, kMaxCrossovers, numEdges);

    if (n <= 0)
        return;

    for (int i = 0; i < n; ++i)
        impl->freqs[static_cast<size_t>(i)] = freqs[i];

    impl->applyFrequencies(false);
}

void Crossover::process(const juce::AudioBuffer<float>& input, std::array<juce::AudioBuffer<float>, kMaxBands>& bandOut,
                        int numSamples) noexcept
{
    impl->process(input, bandOut, numSamples);
}

int Crossover::getLatencySamples() const noexcept
{
    return impl->mode == CrossoverMode::LinearPhase ? impl->firLatency : 0;
}
} // namespace ember
