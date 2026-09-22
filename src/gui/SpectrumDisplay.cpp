#include "gui/SpectrumDisplay.h"
#include "gui/EmberTheme.h"
#include "gui/tutorial/TourAnchor.h"
#include "gui/tutorial/TourTargets.h"
#include <limits>

#include <cmath>

#include "plugin/ParameterIDs.h"

namespace ember::gui
{
namespace
{
/** The frequency gridlines. Deliberately sparse: one per half decade, each one
    labelled, and nothing in between — a spectrum is easier to read over a few
    landmarks than over graph paper. */
struct FrequencyGridLine
{
    float hz;
    const char* label;
};

constexpr FrequencyGridLine kFrequencyGrid[] = {{30.0f, "30"},   {100.0f, "100"}, {300.0f, "300"},
                                                {1000.0f, "1k"}, {3000.0f, "3k"}, {10000.0f, "10k"}};

constexpr float kLevelGrid[] = {0.0f, -20.0f, -40.0f, -60.0f, -80.0f};

/** One third of an octave: the same minimum edge separation the engine enforces
    in `resolveParameters`, so a dragged edge never lands somewhere the DSP will
    silently move it away from. */
constexpr float kMinCrossoverRatio = 1.26f;

/** How far below the displayed floor a curve rests when there is no signal, so
    it stays pinned to the bottom edge instead of hovering just above it. */
constexpr float kCurveFloorMargin = 6.0f;

juce::String formatFrequency(float hz)
{
    if (hz >= 10000.0f)
        return juce::String(hz / 1000.0f, 1) + " kHz";

    if (hz >= 1000.0f)
        return juce::String(hz / 1000.0f, 2) + " kHz";

    if (hz >= 100.0f)
        return juce::String(juce::roundToInt(hz)) + " Hz";

    return juce::String(hz, 1) + " Hz";
}

/** Resizes a smoothed curve, carrying the shape it already has across to the
    new width. Resizing the window then costs nothing visible, where clearing
    would flash the display empty on every drag of the window edge. */
void resampleCurve(std::vector<float>& curve, int newSize, float fallback)
{
    const auto target = static_cast<size_t>(juce::jmax(0, newSize));

    if (curve.size() == target)
        return;

    std::vector<float> resampled(target, fallback);

    if (!curve.empty() && target > 0)
    {
        const auto lastSource = static_cast<float>(curve.size() - 1);
        const auto lastTarget = static_cast<float>(target - 1);

        for (size_t i = 0; i < target; ++i)
        {
            const float proportion = target > 1 ? static_cast<float>(i) / lastTarget : 0.0f;
            const auto source = static_cast<size_t>(juce::roundToInt(proportion * lastSource));
            resampled[i] = curve[juce::jmin(source, curve.size() - 1)];
        }
    }

    curve.swap(resampled);
}
} // namespace

//==============================================================================
SpectrumDisplay::SpectrumDisplay(EmberAudioProcessor& processorToUse) : processor(processorToUse)
{
    setComponentID(tutorial::TourTargets::display);

    // The mode selector, the analyser gear and the crossover handles are
    // painted rather than built from components, so each gets an anchor the
    // tour can point at.
    addAndMakeVisible(modeAnchor);
    addAndMakeVisible(gearAnchor);

    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        crossoverAnchors[static_cast<size_t>(i)] =
            std::make_unique<tutorial::TourAnchor>(tutorial::TourTargets::crossover(i));
        addAndMakeVisible(crossoverAnchors[static_cast<size_t>(i)].get());
    }

    setOpaque(false);
    setWantsKeyboardFocus(false);

    auto& state = processor.getAPVTS();

    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        const auto index = static_cast<size_t>(i);
        crossoverIds[index] = pid::crossover(i);
        crossoverParams[index] = state.getParameter(crossoverIds[index]);
        crossoverHz[index] = 1000.0f;
        modulatedHz[index] = 1000.0f;
        jassert(crossoverParams[index] != nullptr);
    }

    for (int band = 0; band < kMaxBands; ++band)
    {
        const auto index = static_cast<size_t>(band);
        driveIds[index] = pid::drive(band);
        driveParams[index] = state.getParameter(driveIds[index]);
        jassert(driveParams[index] != nullptr);
    }

    numBandsParam = state.getParameter(pid::numBands);
    jassert(numBandsParam != nullptr);

    logFrequencyRatio = std::log(maxFrequency / minFrequency);
    updateSmoothingCoefficients();
    refreshFromProcessor();

    startTimerHz(refreshRateHz);
}

SpectrumDisplay::~SpectrumDisplay()
{
    stopTimer();

    // A window closed with the mouse still down on a divider never delivers the
    // mouseUp that would have closed the gesture. Left open, the host keeps that
    // crossover flagged as being written by the user: in several DAWs that
    // latches the automation lane in touch/write mode for good. Close it here so
    // destruction can never leak a gesture.
    if (draggedDivider >= 0 && draggedDivider < kMaxCrossovers)
        if (auto* parameter = crossoverParams[static_cast<size_t>(draggedDivider)])
            parameter->endChangeGesture();
}

//==============================================================================
void SpectrumDisplay::setRefreshRateHz(int hz)
{
    refreshRateHz = juce::jlimit(15, 60, hz);
    updateSmoothingCoefficients();
    startTimerHz(refreshRateHz);
}

void SpectrumDisplay::setFrequencyRange(float lowHz, float highHz)
{
    minFrequency = juce::jmax(1.0f, lowHz);
    maxFrequency = juce::jmax(minFrequency * 2.0f, highHz);
    logFrequencyRatio = std::log(maxFrequency / minFrequency);
    repaint();
}

void SpectrumDisplay::setDecibelRange(float lowDb, float highDb)
{
    minDecibels = juce::jmin(lowDb, highDb - 12.0f);
    maxDecibels = juce::jmax(highDb, minDecibels + 12.0f);
    repaint();
}

void SpectrumDisplay::refreshFromProcessor()
{
    refreshParameters();
    refreshHeat();
    repaint();
}

//==============================================================================
float SpectrumDisplay::xForFrequency(float hz) const noexcept
{
    const float clamped = juce::jlimit(minFrequency, maxFrequency, hz);
    const float proportion = std::log(clamped / minFrequency) / logFrequencyRatio;

    return plotArea.getX() + plotArea.getWidth() * proportion;
}

float SpectrumDisplay::frequencyForX(float x) const noexcept
{
    if (plotArea.getWidth() < 1.0f)
        return minFrequency;

    const float proportion = (x - plotArea.getX()) / plotArea.getWidth();

    return minFrequency * std::exp(logFrequencyRatio * proportion);
}

float SpectrumDisplay::yForDecibels(float decibels) const noexcept
{
    const float span = juce::jmax(1.0f, maxDecibels - minDecibels);
    const float proportion = (juce::jlimit(minDecibels, maxDecibels, decibels) - minDecibels) / span;

    return plotArea.getBottom() - plotArea.getHeight() * proportion;
}

//==============================================================================
void SpectrumDisplay::resized()
{
    const float scale = currentUiScale();
    auto area = getLocalBounds().toFloat().reduced(juce::jmax(1.0f, 1.5f * scale));

    if (area.getWidth() < 4.0f || area.getHeight() < 4.0f)
    {
        // `reduced` on a component smaller than the inset produces a rectangle
        // with negative extents, whose getRight() is left of its getX(). Nothing
        // downstream is prepared for that, so collapse it to an empty rectangle
        // at the same origin instead of propagating an inside-out one.
        plotArea = area.withSize(juce::jmax(0.0f, area.getWidth()), juce::jmax(0.0f, area.getHeight()));
        axisArea = {};
        rebuildColumns();
        return;
    }

    // The frequency scale gets exactly the strip its type needs, and never more
    // than a fifth of the height, so a short display keeps its curve.
    const float labelHeight = EmberFonts::sizeFor(EmberFonts::Role::micro, scale) * 1.9f;
    const float axisHeight = juce::jlimit(6.0f, juce::jmax(6.0f, area.getHeight() * 0.2f), labelHeight);

    axisArea = area.removeFromBottom(axisHeight);
    plotArea = area;

    rebuildColumns();

    // Anchors follow the regions they stand for, so a spotlight lands on the
    // control rather than near it.
    const auto anchorScale = currentUiScale();
    modeAnchor.setBounds(modeSelectorBounds(anchorScale).toNearestInt());
    gearAnchor.setBounds(gearBounds(anchorScale).toNearestInt());

    for (int i = 0; i < kMaxCrossovers; ++i)
        if (auto& anchor = crossoverAnchors[static_cast<size_t>(i)])
        {
            const auto x = i < numBands - 1 ? xForFrequency(crossoverHz[static_cast<size_t>(i)]) : -1000.0f;
            anchor->setBounds(juce::Rectangle<float>(18.0f, plotArea.getHeight())
                                  .withCentre({x, plotArea.getCentreY()})
                                  .toNearestInt());
        }
}

void SpectrumDisplay::rebuildColumns()
{
    // One screen column per pixel at High: the curve then has exactly as much
    // detail as the display can show, at any window size. Low and Medium
    // aggregate more bins per column, which is what makes a busy signal read
    // as a shape rather than as a hedge.
    //
    // Resolution is a display choice, not an FFT size: kSpectrumFFTSize is
    // fixed in the engine, and making it variable would mean reallocating the
    // analyser's buffers from a menu click while the audio thread is reading
    // them. Aggregating more bins per column gives the same visible result
    // with none of that risk.
    constexpr float columnsPerPixel[] = {0.25f, 0.5f, 1.0f};
    const auto resolution = juce::jlimit(0, 2, processor.getAnalyserSettings().resolution);

    const int columns = juce::jlimit(2, 8192, juce::roundToInt(plotArea.getWidth() * columnsPerPixel[resolution]));
    const float floorDb = minDecibels - kCurveFloorMargin;

    resampleCurve(inputCurve, columns, floorDb);
    resampleCurve(outputCurve, columns, floorDb);
}

void SpectrumDisplay::updateSmoothingCoefficients()
{
    // Per-column one-pole smoothing, specified in seconds and converted for the
    // current frame rate, so changing the rate does not change the feel: a fast
    // attack keeps transients honest, a slow release stops the strobing that
    // raw FFT frames produce.
    const auto rate = static_cast<float>(refreshRateHz);

    // Averaging is a release-time choice: the attack stays fast at every
    // setting, because a slow attack hides transients, which is never what
    // "slow averaging" is asked for.
    constexpr float releaseSeconds[] = {0.12f, 0.32f, 0.90f};
    const auto averaging = juce::jlimit(0, 2, processor.getAnalyserSettings().averaging);

    attackCoefficient = 1.0f - std::exp(-1.0f / (rate * 0.020f));
    releaseCoefficient = 1.0f - std::exp(-1.0f / (rate * releaseSeconds[averaging]));
}

void SpectrumDisplay::updateCachedFonts(float scale)
{
    if (std::abs(scale - cachedFontScale) <= 1.0e-4f)
        return;

    cachedFontScale = scale;
    microFont = EmberFonts::get(EmberFonts::Role::micro, scale);
    valueFont = EmberFonts::get(EmberFonts::Role::value, scale);
}

void SpectrumDisplay::repaintPlot()
{
    if (plotArea.getWidth() < 1.0f || plotArea.getHeight() < 1.0f)
    {
        repaint();
        return;
    }

    repaint(plotArea.getSmallestIntegerContainer().expanded(1));
}

float SpectrumDisplay::currentUiScale() const
{
    if (const auto* lf = dynamic_cast<const EmberLookAndFeel*>(&getLookAndFeel()))
        return lf->getUiScale();

    // Standalone (a unit-drawing harness, say): the analyser is a little over
    // half the editor's height, so infer a plausible editor from our own.
    return EmberFonts::scaleFor(static_cast<float>(getHeight()) / 0.55f);
}

//==============================================================================
void SpectrumDisplay::timerCallback()
{
    const bool parametersMoved = refreshParameters();
    const bool heatMoved = refreshHeat();
    const bool curveMoved = refreshSpectrum();

    // Everything these three can move is inside the plot; the frequency scale
    // and the well only change on a resize.
    if (parametersMoved || heatMoved || curveMoved)
        repaintPlot();
}

bool SpectrumDisplay::refreshParameters()
{
    bool changed = false;

    if (numBandsParam != nullptr)
    {
        const int bands = juce::jlimit(kMinBands, kMaxBands,
                                       juce::roundToInt(numBandsParam->convertFrom0to1(numBandsParam->getValue())));

        if (bands != numBands)
        {
            numBands = bands;
            changed = true;
        }
    }

    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        const auto index = static_cast<size_t>(i);
        auto* parameter = crossoverParams[index];

        if (parameter == nullptr)
            continue;

        float hz = juce::jlimit(minFrequency, maxFrequency, parameter->convertFrom0to1(parameter->getValue()));

        // Keep the edges ascending and a third of an octave apart exactly as the
        // engine does, so a region boundary is always a real band boundary.
        if (i > 0)
            hz = juce::jlimit(minFrequency, maxFrequency, juce::jmax(hz, crossoverHz[index - 1] * kMinCrossoverRatio));

        const float modulated = modulatedFrequencyFor(i, hz);

        if (std::abs(hz - crossoverHz[index]) > 0.01f || std::abs(modulated - modulatedHz[index]) > 0.01f)
            changed = true;

        crossoverHz[index] = hz;
        modulatedHz[index] = modulated;
    }

    int selection = juce::jlimit(0, kMaxBands - 1, processor.getSelectedBand());

    if (selection >= numBands)
    {
        // The band count shrank under the selection: move it and tell the editor,
        // rather than highlighting a band that no longer exists.
        selection = numBands - 1;
        setSelectedBandAndNotify(selection);
        changed = true;
    }
    else if (selection != selectedBand)
    {
        selectedBand = selection;
        changed = true;
    }

    return changed;
}

bool SpectrumDisplay::refreshHeat()
{
    bool changed = false;

    // The design asks for 30 ms attack and 400 ms release. Deriving the
    // coefficients from the live refresh rate keeps that true when the rate is
    // lowered - otherwise a 15 Hz refresh would make heat four times slower
    // than specified, which reads as sluggish rather than as smooth.
    const auto rate = static_cast<float>(juce::jmax(1, refreshRateHz));
    const float attack = 1.0f - std::exp(-1.0f / (0.030f * rate));
    const float release = 1.0f - std::exp(-1.0f / (0.400f * rate));

    for (int band = 0; band < kMaxBands; ++band)
    {
        const auto index = static_cast<size_t>(band);

        // Measured harmonic energy, not a proxy. The old estimate multiplied
        // the drive parameter by the band's output level, which lit a band that
        // was merely loud and missed one saturating quietly.
        const float target = band < numBands ? processor.getBandHeat(band) : 0.0f;

        float& heat = bandHeat[index];
        const float previous = heat;

        heat += (target - heat) * (target > heat ? attack : release);

        if (std::abs(heat - previous) > 0.002f)
            changed = true;
    }

    return changed;
}

bool SpectrumDisplay::refreshSpectrum()
{
    // Freeze holds the last frame rather than stopping the timer: the meters,
    // heat and crossover handles all still have to move.
    if (processor.getAnalyserSettings().freeze)
        return false;

    const int columns = static_cast<int>(inputCurve.size());

    if (columns < 2 || outputCurve.size() != inputCurve.size() || plotArea.getWidth() < 2.0f)
        return false;

    // Wait-free: false simply means the engine has not published a frame yet, so
    // there is nothing to fade in or out — keep what is on screen.
    if (!processor.getSpectrumFifo().readLatest(frame))
        return false;

    // The first frame makes the curves drawable at all, so it always needs a
    // repaint even if it moves nothing: a silent input leaves every column
    // already at the floor, and without this the flat curve would not appear
    // until something else happened to dirty the component.
    const bool firstFrame = !hasFrame;

    hasFrame = true;

    const double hostRate = processor.getSampleRate();
    const auto sampleRate = static_cast<float>(hostRate > 0.0 ? hostRate : 44100.0);
    const float binsPerHz = static_cast<float>(kSpectrumFFTSize) / sampleRate;
    const float columnWidth = plotArea.getWidth() / static_cast<float>(columns);
    const float floorDb = minDecibels - kCurveFloorMargin;

    const auto advance = [this](float& value, float target)
    {
        const float coefficient = target > value ? attackCoefficient : releaseCoefficient;
        const float previous = value;
        value += (target - value) * coefficient;
        return std::abs(value - previous);
    };

    float largestMove = 0.0f;

    // Tilt lifts the top end so that programme material - which falls off with
    // frequency - reads as roughly flat. Referenced at 1 kHz, so the middle of
    // the display does not move as the tilt changes and only the ends rotate
    // around it.
    const float tiltDbPerOctave = processor.getAnalyserSettings().tiltDbPerOctave();

    for (int column = 0; column < columns; ++column)
    {
        const float left = plotArea.getX() + static_cast<float>(column) * columnWidth;
        const float centreHz = frequencyForX(left + columnWidth * 0.5f);
        const float firstBin = frequencyForX(left) * binsPerHz;
        const float lastBin = frequencyForX(left + columnWidth) * binsPerHz;

        const float tilt =
            tiltDbPerOctave > 0.0f && centreHz > 0.0f ? tiltDbPerOctave * std::log2(centreHz / 1000.0f) : 0.0f;

        const auto index = static_cast<size_t>(column);
        const float inputTarget = juce::jmax(floorDb, aggregateBins(frame.inputDb, firstBin, lastBin) + tilt);
        const float outputTarget = juce::jmax(floorDb, aggregateBins(frame.outputDb, firstBin, lastBin) + tilt);

        largestMove = juce::jmax(largestMove, advance(inputCurve[index], inputTarget));
        largestMove = juce::jmax(largestMove, advance(outputCurve[index], outputTarget));
    }

    return firstFrame || largestMove > 0.02f;
}

float SpectrumDisplay::aggregateBins(const std::array<float, static_cast<size_t>(kSpectrumBins)>& bins, float firstBin,
                                     float lastBin) noexcept
{
    // Bin 0 is DC and never belongs on a 20 Hz .. 20 kHz axis.
    const auto highest = static_cast<float>(kSpectrumBins - 1);
    const float low = juce::jlimit(1.0f, highest, juce::jmin(firstBin, lastBin));
    const float high = juce::jlimit(1.0f, highest, juce::jmax(firstBin, lastBin));

    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::floor(high));

    if (last <= first)
    {
        // Low end of the axis: a column is narrower than a bin, so interpolate
        // between neighbours rather than letting the curve step from bin to bin.
        const int next = juce::jmin(first + 1, kSpectrumBins - 1);
        const float fraction = low - static_cast<float>(first);

        return juce::jmap(fraction, bins[static_cast<size_t>(first)], bins[static_cast<size_t>(next)]);
    }

    // High end: many bins per column. Take the peak — an average of a column
    // that is mostly noise floor buries exactly the narrow peaks a spectrum is
    // being read for.
    float peak = bins[static_cast<size_t>(first)];

    for (int bin = first + 1; bin <= last; ++bin)
        peak = juce::jmax(peak, bins[static_cast<size_t>(bin)]);

    return peak;
}

//==============================================================================
void SpectrumDisplay::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float scale = currentUiScale();
    const float corner = juce::jmax(2.0f, 4.0f * scale);

    updateCachedFonts(scale);

    EmberLookAndFeel::drawWell(g, bounds, corner);

    if (plotArea.getWidth() < 8.0f || plotArea.getHeight() < 8.0f)
        return;

    juce::Graphics::ScopedSaveState saved(g);

    clipPath.clear();
    clipPath.addRoundedRectangle(bounds.reduced(1.0f), corner);
    g.reduceClipRegion(clipPath);

    paintBandRegions(g, scale);
    paintGrid(g, scale);

    if (displayMode != DisplayMode::eq)
        paintSpectra(g, scale);

    if (displayMode != DisplayMode::spectrum)
        paintEqOverlay(g, scale);

    paintCrossovers(g, scale);
    paintDragReadout(g, scale);
    paintMouseFrequency(g, scale);
    paintEqNodes(g, scale);
    paintGear(g, scale);
    paintModeSelector(g, scale);
}

void SpectrumDisplay::setDisplayMode(DisplayMode mode)
{
    if (displayMode == mode)
        return;

    displayMode = mode;
    repaint();
}

juce::Rectangle<float> SpectrumDisplay::modeSelectorBounds(float scale) const
{
    if (plotArea.getWidth() < 260.0f)
        return {};

    const float height = juce::jlimit(14.0f, 22.0f, 17.0f * scale);

    // Measured from the widest label, not guessed. Guessing truncated "BOTH"
    // to "B" - the same mistake the EQ panel's chips made, and the same fix.
    const auto font = EmberFonts::get(EmberFonts::Role::micro, scale);
    const auto widest = juce::jmax(juce::GlyphArrangement::getStringWidth(font, "SPEC"),
                                   juce::GlyphArrangement::getStringWidth(font, "BOTH"));
    const float width = (widest + 12.0f) * 3.0f;

    return {plotArea.getRight() - width - 6.0f * scale, plotArea.getY() + 6.0f * scale, width, height};
}

int SpectrumDisplay::modeSegmentAt(juce::Point<float> position, float scale) const
{
    const auto bounds = modeSelectorBounds(scale);

    if (bounds.isEmpty() || !bounds.contains(position))
        return -1;

    const auto segment = static_cast<int>((position.x - bounds.getX()) / (bounds.getWidth() / 3.0f));

    return juce::jlimit(0, 2, segment);
}

void SpectrumDisplay::paintModeSelector(juce::Graphics& g, float scale) const
{
    const auto bounds = modeSelectorBounds(scale);

    if (bounds.isEmpty())
        return;

    const auto& tk = EmberTheme::tokens();
    const float segmentWidth = bounds.getWidth() / 3.0f;
    const char* labels[] = {"SPEC", "EQ", "BOTH"};

    g.setColour(tk.bgDeep.withAlpha(0.85f));
    g.fillRoundedRectangle(bounds, bounds.getHeight() * 0.3f);

    for (int i = 0; i < 3; ++i)
    {
        const auto cell = juce::Rectangle<float>(bounds.getX() + static_cast<float>(i) * segmentWidth, bounds.getY(),
                                                 segmentWidth, bounds.getHeight());
        const bool on = static_cast<int>(displayMode) == i;

        if (on)
        {
            g.setColour(tk.ember.withAlpha(0.22f));
            g.fillRoundedRectangle(cell.reduced(1.0f), bounds.getHeight() * 0.28f);
        }

        g.setFont(EmberFonts::get(EmberFonts::Role::micro, scale));
        g.setColour(on ? tk.text : (i == hoveredModeSegment ? tk.textDim.brighter(0.3f) : tk.textDim));
        g.drawText(labels[i], cell, juce::Justification::centred, false);
    }

    g.setColour(tk.panelEdge);
    g.drawRoundedRectangle(bounds.reduced(0.5f), bounds.getHeight() * 0.3f, 1.0f);
}

//==============================================================================
namespace
{
constexpr float kEqOverlayRangeDb = 12.0f;
constexpr float kEqNodeRadius = 5.0f;
} // namespace

float SpectrumDisplay::yForEqDecibels(float db) const
{
    const auto t =
        (kEqOverlayRangeDb - juce::jlimit(-kEqOverlayRangeDb, kEqOverlayRangeDb, db)) / (2.0f * kEqOverlayRangeDb);
    return plotArea.getY() + t * plotArea.getHeight();
}

float SpectrumDisplay::eqDecibelsForY(float y) const
{
    if (plotArea.getHeight() < 1.0f)
        return 0.0f;

    const auto t = juce::jlimit(0.0f, 1.0f, (y - plotArea.getY()) / plotArea.getHeight());
    return kEqOverlayRangeDb - t * 2.0f * kEqOverlayRangeDb;
}

juce::RangedAudioParameter* SpectrumDisplay::eqNodeGain(int band, int node) const
{
    auto& state = processor.getAPVTS();

    switch (node)
    {
    case 0:
        return state.getParameter(pid::toneLow(band));
    case 1:
        return state.getParameter(pid::toneMid(band));
    case 2:
        return state.getParameter(pid::toneHigh(band));
    default:
        break;
    }

    return nullptr;
}

juce::RangedAudioParameter* SpectrumDisplay::eqNodeFreq(int band, int node) const
{
    auto& state = processor.getAPVTS();

    switch (node)
    {
    case 0:
        return state.getParameter(pid::toneLowHz(band));
    case 1:
        return state.getParameter(pid::toneMidHz(band));
    case 2:
        return state.getParameter(pid::toneHighHz(band));
    default:
        break;
    }

    return nullptr;
}

juce::Point<float> SpectrumDisplay::eqNodePosition(int band, int node) const
{
    auto* gain = eqNodeGain(band, node);
    auto* freq = eqNodeFreq(band, node);

    if (gain == nullptr || freq == nullptr)
        return {};

    return {xForFrequency(freq->convertFrom0to1(freq->getValue())),
            yForEqDecibels(gain->convertFrom0to1(gain->getValue()))};
}

SpectrumDisplay::EqNodeRef SpectrumDisplay::eqNodeAt(juce::Point<float> position) const
{
    if (displayMode == DisplayMode::spectrum)
        return {};

    EqNodeRef best;
    float bestDistance = kEqNodeRadius * 3.0f;

    for (int band = 0; band < numBands; ++band)
    {
        for (int node = 0; node < 3; ++node)
        {
            const auto distance = position.getDistanceFrom(eqNodePosition(band, node));

            // Ties go to the selected band. Six bands' curves cross often, and
            // without this the node you grab depends on rounding rather than on
            // which band you are working in.
            const bool better =
                distance < bestDistance || (best.isValid() && band == selectedBand && best.band != selectedBand &&
                                            distance < bestDistance + kEqNodeRadius);

            if (better)
            {
                bestDistance = juce::jmin(bestDistance, distance);
                best = {band, node};
            }
        }
    }

    return best;
}

void SpectrumDisplay::paintEqNodes(juce::Graphics& g, float scale) const
{
    if (displayMode == DisplayMode::spectrum)
        return;

    const auto& tk = EmberTheme::tokens();

    for (int band = 0; band < numBands; ++band)
    {
        for (int node = 0; node < 3; ++node)
        {
            const EqNodeRef ref{band, node};
            const bool active = ref == hoveredEqNode || ref == draggedEqNode;
            const auto centre = eqNodePosition(band, node);
            const auto radius = (active ? kEqNodeRadius * 1.3f : kEqNodeRadius) * scale;

            if (!plotArea.contains(centre))
                continue;

            if (active && !EmberTheme::reduceMotion())
                GlowCache::draw(g, centre, radius * 2.6f, tk.tubeGlow, 0.45f);

            const auto disc = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);

            g.setColour(tk.bgDeep.withAlpha(0.85f));
            g.fillEllipse(disc);
            g.setColour(band == selectedBand ? tk.ember : tk.ember.withAlpha(0.5f));
            g.fillEllipse(disc.reduced(radius * 0.45f));
            g.setColour(tk.panelEdge.brighter(active ? 0.35f : 0.0f));
            g.drawEllipse(disc.reduced(0.5f), 1.0f);
        }
    }
}

void SpectrumDisplay::paintEqOverlay(juce::Graphics& g, float scale)
{
    const auto& tk = EmberTheme::tokens();
    auto& state = processor.getAPVTS();

    const auto read = [&state](const juce::String& id, float fallback)
    {
        if (auto* v = state.getRawParameterValue(id))
            return v->load(std::memory_order_relaxed);

        return fallback;
    };

    // The EQ plot uses its own +/-12 dB scale rather than the analyser's dB
    // range: a 6 dB shelf drawn against an 80 dB spectrum scale is a flat line,
    // which is worse than not drawing it.
    constexpr float kEqRangeDb = 12.0f;
    const auto yForEqDb = [this](float db)
    {
        const auto t = (kEqRangeDb - juce::jlimit(-kEqRangeDb, kEqRangeDb, db)) / (2.0f * kEqRangeDb);
        return plotArea.getY() + t * plotArea.getHeight();
    };

    constexpr int kPoints = 200;
    combinedPath.clear();

    std::array<float, kPoints> combinedDb{};
    float weightTotal = 0.0f;

    for (int b = 0; b < numBands; ++b)
    {
        const auto index = static_cast<size_t>(b);

        toneDrawing[index].setShape({read(pid::toneLowHz(b), 150.0f), read(pid::toneMidHz(b), 1000.0f),
                                     read(pid::toneMidQ(b), 0.7f), read(pid::toneHighHz(b), 4000.0f)});

        const bool bypassed = read(pid::toneBypass(b), 0.0f) > 0.5f;

        toneDrawing[index].setGainsDb(bypassed ? 0.0f : read(pid::toneLow(b), 0.0f),
                                      bypassed ? 0.0f : read(pid::toneMid(b), 0.0f),
                                      bypassed ? 0.0f : read(pid::toneHigh(b), 0.0f));

        float bandLow = 20.0f;
        float bandHigh = 20000.0f;
        processor.getBandSpanHz(b, bandLow, bandHigh);

        auto& path = tonePaths[index];
        path.clear();

        // Weighted by the band's level: the combined line is what the plugin is
        // doing to the signal, and a band with nothing in it is not doing
        // anything to it however its curve is set.
        const auto weight = juce::jlimit(0.0f, 1.0f, processor.getBandLevel(b));
        weightTotal += weight;

        for (int i = 0; i < kPoints; ++i)
        {
            const auto t = static_cast<float>(i) / static_cast<float>(kPoints - 1);
            const auto hz = minFrequency * std::pow(maxFrequency / minFrequency, t);
            const auto db = toneDrawing[index].magnitudeDbAt(hz);

            combinedDb[static_cast<size_t>(i)] += db * weight;

            const auto x = plotArea.getX() + t * plotArea.getWidth();
            const auto y = yForEqDb(db);

            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }

        // Only the in-band stretch is drawn: this stage shapes this band alone.
        const auto left = xForFrequency(bandLow);
        const auto right = xForFrequency(bandHigh);

        juce::Graphics::ScopedSaveState save{g};
        g.reduceClipRegion(
            juce::Rectangle<float>(left, plotArea.getY(), juce::jmax(0.0f, right - left), plotArea.getHeight())
                .toNearestInt());

        const auto heat = juce::jlimit(0.0f, 1.0f, bandHeat[index]);
        const auto colour = heat > 0.02f ? tk.heatTint(heat) : tk.ember;

        if (heat > 0.02f && !EmberTheme::reduceMotion())
        {
            g.setColour(tk.tubeGlow.withAlpha(heat * 0.35f));
            g.strokePath(path, juce::PathStrokeType(4.0f * scale, juce::PathStrokeType::curved));
        }

        g.setColour(colour.withAlpha(b == selectedBand ? 0.95f : 0.6f));
        g.strokePath(path,
                     juce::PathStrokeType((b == selectedBand ? 2.0f : 1.4f) * scale, juce::PathStrokeType::curved));
    }

    // ---- the combined response --------------------------------------------
    if (weightTotal > 1.0e-4f)
    {
        for (int i = 0; i < kPoints; ++i)
        {
            const auto t = static_cast<float>(i) / static_cast<float>(kPoints - 1);
            const auto x = plotArea.getX() + t * plotArea.getWidth();
            const auto y = yForEqDb(combinedDb[static_cast<size_t>(i)] / weightTotal);

            if (i == 0)
                combinedPath.startNewSubPath(x, y);
            else
                combinedPath.lineTo(x, y);
        }

        juce::Graphics::ScopedSaveState save{g};
        g.reduceClipRegion(plotArea.toNearestInt());

        if (!EmberTheme::reduceMotion())
        {
            g.setColour(tk.signal.withAlpha(0.25f));
            g.strokePath(combinedPath, juce::PathStrokeType(6.0f * scale, juce::PathStrokeType::curved));
        }

        g.setColour(tk.signal);
        g.strokePath(combinedPath, juce::PathStrokeType(2.4f * scale, juce::PathStrokeType::curved));
    }
}

void SpectrumDisplay::paintBandRegions(juce::Graphics& g, float scale) const
{
    const float tabHeight = juce::jmax(2.0f, 2.5f * scale);
    const auto& font = microFont;
    const float labelHeight = font.getHeight();

    for (int band = 0; band < numBands; ++band)
    {
        const auto region = regionForBand(band);

        if (region.getWidth() < 1.0f)
            continue;

        const bool selected = band == selectedBand;
        const bool hovered = band == hoveredBand && draggedDivider < 0 && hoveredDivider < 0;
        const auto colour = EmberColours::band(band);
        const float heat = bandHeat[static_cast<size_t>(band)];

        // Intensity carries the band's heat, and the selected band is lifted
        // clear of the others whatever it is doing — but a region is a wash the
        // curve has to stay readable through, never a block of colour.
        const float alpha =
            (selected ? 0.12f : 0.045f) + heat * (selected ? 0.15f : 0.11f) + (hovered && !selected ? 0.03f : 0.0f);

        g.setGradientFill(juce::ColourGradient(colour.withAlpha(alpha), region.getCentreX(), region.getY(),
                                               colour.withAlpha(alpha * 0.3f), region.getCentreX(), region.getBottom(),
                                               false));
        g.fillRect(region);

        // The tab is what actually marks the selection: full strength and twice
        // the height, where an unselected band's only glows with its heat.
        const float thisTabHeight = selected ? tabHeight * 1.8f : tabHeight;
        g.setColour(colour.withAlpha(selected ? 1.0f : juce::jlimit(0.0f, 1.0f, 0.32f + heat * 0.5f)));
        g.fillRect(region.withHeight(thisTabHeight));

        if (selected)
        {
            g.setColour(colour.withAlpha(0.22f));
            g.drawRect(region.reduced(0.5f), 1.0f);
        }

        if (region.getWidth() > labelHeight * 1.8f && region.getHeight() > labelHeight * 4.0f)
        {
            const auto labelArea =
                juce::Rectangle<float>(region.getX() + 4.0f * scale, region.getY() + thisTabHeight + 2.0f * scale,
                                       region.getWidth() - 8.0f * scale, labelHeight);

            g.setFont(font);
            g.setColour(selected ? colour.brighter(0.3f) : colour.withAlpha(0.55f));
            g.drawText(juce::String(band + 1), labelArea, juce::Justification::topLeft, false);
        }
    }
}

void SpectrumDisplay::paintGrid(juce::Graphics& g, float scale) const
{
    const auto& font = microFont;
    const float labelHeight = font.getHeight();

    g.setFont(font);

    if (plotArea.getHeight() > labelHeight * 6.0f)
    {
        const float labelWidth = 30.0f * scale;

        for (const float decibels : kLevelGrid)
        {
            if (decibels < minDecibels || decibels > maxDecibels)
                continue;

            const float y = yForDecibels(decibels);

            EmberLookAndFeel::drawHairline(g, juce::Rectangle<float>(plotArea.getX(), y, plotArea.getWidth(), 1.0f),
                                           EmberColours::outline().withAlpha(decibels < -0.5f ? 0.42f : 0.7f));

            if (plotArea.getWidth() > labelWidth * 3.0f)
            {
                const auto textArea = juce::Rectangle<float>(plotArea.getRight() - labelWidth - 3.0f * scale,
                                                             y - labelHeight - 1.0f, labelWidth, labelHeight);

                g.setColour(EmberColours::textDisabled().withAlpha(0.6f));
                g.drawText(juce::String(juce::roundToInt(decibels)), textArea, juce::Justification::centredRight,
                           false);
            }
        }
    }

    float lastLabelRight = -1.0e6f;
    const bool roomForLabels = axisArea.getHeight() >= labelHeight;

    for (const auto& line : kFrequencyGrid)
    {
        if (line.hz <= minFrequency || line.hz >= maxFrequency)
            continue;

        const float x = xForFrequency(line.hz);

        EmberLookAndFeel::drawHairline(g, juce::Rectangle<float>(x, plotArea.getY(), 1.0f, plotArea.getHeight()),
                                       EmberColours::outline().withAlpha(0.48f), true);

        if (!roomForLabels)
            continue;

        const juce::String text(line.label);
        const float width = juce::GlyphArrangement::getStringWidth(font, text) + 10.0f * scale;
        const auto textArea = juce::Rectangle<float>(x - width * 0.5f, axisArea.getY(), width, axisArea.getHeight());

        // Skip a label rather than let two collide on a narrow window.
        if (textArea.getX() <= lastLabelRight || textArea.getX() < 0.0f ||
            textArea.getRight() > static_cast<float>(getWidth()))
            continue;

        g.setColour(EmberColours::textDisabled());
        g.drawText(text, textArea, juce::Justification::centred, false);
        lastLabelRight = textArea.getRight();
    }
}

void SpectrumDisplay::paintSpectra(juce::Graphics& g, float scale)
{
    if (!hasFrame || inputCurve.size() < 2 || outputCurve.size() < 2)
        return;

    // `curvePath` and `fillPath` are members so the several thousand coordinates
    // a curve costs are written into storage that was allocated once, not into a
    // Path built and thrown away 50 times a second.
    buildCurvePath(curvePath, inputCurve);

    if (!curvePath.isEmpty())
    {
        // The input is a body, not a line: a filled shape the output curve can
        // be read against without the two ever being confused. Built by walking
        // the curve a second time rather than copying the stroke path, because
        // copying a Path allocates and walking it does not.
        buildCurvePath(fillPath, inputCurve);
        fillPath.lineTo(plotArea.getRight(), plotArea.getBottom() + 2.0f);
        fillPath.lineTo(plotArea.getX(), plotArea.getBottom() + 2.0f);
        fillPath.closeSubPath();

        g.setGradientFill(juce::ColourGradient(EmberColours::textSecondary().withAlpha(0.20f), plotArea.getCentreX(),
                                               plotArea.getY(), EmberColours::textSecondary().withAlpha(0.05f),
                                               plotArea.getCentreX(), plotArea.getBottom(), false));
        g.fillPath(fillPath);

        g.setColour(EmberColours::textSecondary().withAlpha(0.34f));
        g.strokePath(curvePath, juce::PathStrokeType(juce::jmax(1.0f, 0.9f * scale), juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    buildCurvePath(curvePath, outputCurve);

    if (!curvePath.isEmpty())
    {
        g.setColour(EmberColours::accentGlow().withAlpha(0.16f));
        g.strokePath(curvePath, juce::PathStrokeType(juce::jmax(2.5f, 3.4f * scale), juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));

        g.setColour(EmberColours::accent().withAlpha(0.94f));
        g.strokePath(curvePath, juce::PathStrokeType(juce::jmax(1.0f, 1.5f * scale), juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }
}

void SpectrumDisplay::buildCurvePath(juce::Path& path, const std::vector<float>& curve) const
{
    // Cleared first and unconditionally: these paths are reused between frames,
    // so an early return must leave an empty path, never last frame's curve.
    path.clear();

    const int columns = static_cast<int>(curve.size());

    if (columns < 2 || plotArea.getWidth() < 2.0f)
        return;

    path.preallocateSpace((columns + 6) * 3);

    const float columnWidth = plotArea.getWidth() / static_cast<float>(columns);

    path.startNewSubPath(plotArea.getX(), yForDecibels(curve.front()));

    for (int column = 0; column < columns; ++column)
        path.lineTo(plotArea.getX() + (static_cast<float>(column) + 0.5f) * columnWidth,
                    yForDecibels(curve[static_cast<size_t>(column)]));

    path.lineTo(plotArea.getRight(), yForDecibels(curve.back()));
}

void SpectrumDisplay::paintCrossovers(juce::Graphics& g, float scale) const
{
    const int dividers = numBands - 1;
    const float handleWidth = juce::jmax(7.0f, 9.0f * scale);
    const float handleHeight = juce::jmin(juce::jmax(14.0f, 20.0f * scale), plotArea.getHeight() * 0.5f);

    for (int i = 0; i < dividers; ++i)
    {
        const auto index = static_cast<size_t>(i);
        const float x = xForFrequency(crossoverHz[index]);
        const bool highlighted = i == draggedDivider || (i == hoveredDivider && draggedDivider < 0);

        // Where modulation has moved the real edge, show it: the handle stays on
        // the value a drag would edit.
        if (std::abs(modulatedHz[index] - crossoverHz[index]) > 0.5f)
        {
            const float modulatedX = xForFrequency(modulatedHz[index]);

            g.setColour(EmberColours::accent().withAlpha(0.32f));
            g.fillRect(juce::Rectangle<float>(modulatedX - 0.5f, plotArea.getY(), 1.0f, plotArea.getHeight()));
        }

        const float thickness = highlighted ? juce::jmax(1.5f, 1.6f * scale) : juce::jmax(1.0f, 1.1f * scale);

        // A dark halo under a light line: the edge then reads with the same
        // weight over a bright band region as it does over the empty top of the
        // display, which a single flat line does not.
        g.setColour(EmberColours::backgroundDeep().withAlpha(0.5f));
        g.fillRect(juce::Rectangle<float>(x - thickness * 0.5f - 1.0f, plotArea.getY(), thickness + 2.0f,
                                          plotArea.getHeight()));

        g.setColour(highlighted ? EmberColours::accent() : EmberColours::textSecondary().withAlpha(0.72f));
        g.fillRect(juce::Rectangle<float>(x - thickness * 0.5f, plotArea.getY(), thickness, plotArea.getHeight()));

        if (handleHeight < 8.0f)
            continue;

        const auto handle =
            juce::Rectangle<float>(handleWidth, handleHeight)
                .withCentre(juce::Point<float>(x, plotArea.getBottom() - handleHeight * 0.5f - 2.0f * scale));

        g.setColour(highlighted ? EmberColours::accent() : EmberColours::outlineStrong());
        g.fillRoundedRectangle(handle, handleWidth * 0.45f);
        g.setColour(highlighted ? EmberColours::accent().brighter(0.3f)
                                : EmberColours::textSecondary().withAlpha(0.85f));
        g.drawRoundedRectangle(handle.reduced(0.5f), handleWidth * 0.45f, 1.0f);

        const float gripOffset = handleWidth * 0.2f;
        const float gripHeight = handleHeight * 0.4f;
        const float gripTop = handle.getCentreY() - gripHeight * 0.5f;

        g.setColour(highlighted ? EmberColours::backgroundDeep().withAlpha(0.8f)
                                : EmberColours::textPrimary().withAlpha(0.7f));
        g.fillRect(juce::Rectangle<float>(x - gripOffset - 0.5f, gripTop, 1.0f, gripHeight));
        g.fillRect(juce::Rectangle<float>(x + gripOffset - 0.5f, gripTop, 1.0f, gripHeight));
    }
}

namespace
{
/** The note nearest a frequency, as a name plus octave.

    Engineers think in notes as often as in hertz - "the bass sits around E1"
    is more useful than "41 Hz" when deciding where a crossover goes - and the
    two together cost one line along the bottom of the plot. */
juce::String noteNameFor(float hz)
{
    if (hz <= 0.0f)
        return {};

    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    // A4 = 440 Hz is MIDI note 69.
    const auto midi = 69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0);
    const auto rounded = juce::roundToInt(midi);

    if (rounded < 0 || rounded > 127)
        return {};

    return juce::String(names[rounded % 12]) + juce::String(rounded / 12 - 1);
}
} // namespace

void SpectrumDisplay::paintMouseFrequency(juce::Graphics& g, float scale) const
{
    if (!mouseInPlot || draggedDivider >= 0 || plotArea.getWidth() < 120.0f)
        return;

    const auto& tk = EmberTheme::tokens();
    const auto hz = frequencyForX(lastMousePosition.x);
    const auto note = noteNameFor(hz);

    juce::String text = note.isNotEmpty() ? note + "  " : juce::String();
    text += hz >= 1000.0f ? juce::String(hz / 1000.0f, 2) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";

    const auto font = EmberFonts::get(EmberFonts::Role::micro, scale);
    g.setFont(font);

    const auto width = juce::GlyphArrangement::getStringWidth(font, text) + 10.0f;
    const auto height = font.getHeight() + 4.0f;

    auto box =
        juce::Rectangle<float>(width, height).withCentre({lastMousePosition.x, plotArea.getBottom() - height * 0.75f});

    box.setX(juce::jlimit(plotArea.getX(), juce::jmax(plotArea.getX(), plotArea.getRight() - width), box.getX()));

    g.setColour(tk.bgDeep.withAlpha(0.8f));
    g.fillRoundedRectangle(box, height * 0.4f);
    g.setColour(tk.textDim);
    g.drawText(text, box, juce::Justification::centred, false);
}

void SpectrumDisplay::paintDragReadout(juce::Graphics& g, float scale) const
{
    if (draggedDivider < 0 || draggedDivider >= kMaxCrossovers)
        return;

    const auto& font = valueFont;
    const juce::String text = formatFrequency(crossoverHz[static_cast<size_t>(draggedDivider)]);
    const float boxWidth = juce::GlyphArrangement::getStringWidth(font, text) + 16.0f * scale;
    const float boxHeight = font.getHeight() + 8.0f * scale;

    if (boxWidth + 8.0f > plotArea.getWidth() || boxHeight > plotArea.getHeight())
        return;

    const float x = xForFrequency(crossoverHz[static_cast<size_t>(draggedDivider)]);

    auto box = juce::Rectangle<float>(boxWidth, boxHeight)
                   .withCentre(juce::Point<float>(x, plotArea.getY() + boxHeight * 0.5f + 9.0f * scale));

    box.setX(juce::jlimit(plotArea.getX() + 2.0f, plotArea.getRight() - boxWidth - 2.0f, box.getX()));

    const float corner = juce::jmax(2.0f, 3.0f * scale);

    g.setColour(EmberColours::backgroundDeep().withAlpha(0.94f));
    g.fillRoundedRectangle(box, corner);
    g.setColour(EmberColours::accent().withAlpha(0.85f));
    g.drawRoundedRectangle(box.reduced(0.5f), corner, 1.0f);

    g.setFont(font);
    g.setColour(EmberColours::textPrimary());
    g.drawText(text, box, juce::Justification::centred, false);
}

//==============================================================================
juce::Rectangle<float> SpectrumDisplay::regionForBand(int band) const
{
    if (band < 0 || band >= numBands)
        return {};

    const float left = band == 0 ? plotArea.getX() : xForFrequency(crossoverHz[static_cast<size_t>(band - 1)]);
    const float right =
        band == numBands - 1 ? plotArea.getRight() : xForFrequency(crossoverHz[static_cast<size_t>(band)]);

    return {left, plotArea.getY(), juce::jmax(0.0f, right - left), plotArea.getHeight()};
}

int SpectrumDisplay::bandAt(juce::Point<float> position) const
{
    if (plotArea.getWidth() < 1.0f)
        return -1;

    for (int band = 0; band < numBands; ++band)
    {
        const auto region = regionForBand(band);

        if (position.x >= region.getX() && position.x < region.getRight())
            return band;
    }

    // Past the last edge (or exactly on the right border): the top band owns it.
    return position.x >= plotArea.getX() ? numBands - 1 : -1;
}

int SpectrumDisplay::dividerAt(juce::Point<float> position) const
{
    const float tolerance = dividerTolerance();
    int nearest = -1;
    float nearestDistance = tolerance;

    for (int i = 0; i < numBands - 1; ++i)
    {
        const float distance = std::abs(position.x - xForFrequency(crossoverHz[static_cast<size_t>(i)]));

        if (distance <= nearestDistance)
        {
            nearest = i;
            nearestDistance = distance;
        }
    }

    return nearest;
}

float SpectrumDisplay::dividerTolerance() const
{
    return juce::jmax(4.0f, 6.0f * currentUiScale());
}

float SpectrumDisplay::clampCrossover(int index, float hz) const
{
    float low = minFrequency;
    float high = maxFrequency;

    if (index > 0)
        low = juce::jmax(low, crossoverHz[static_cast<size_t>(index - 1)] * kMinCrossoverRatio);

    if (index + 1 < numBands - 1)
        high = juce::jmin(high, crossoverHz[static_cast<size_t>(index + 1)] / kMinCrossoverRatio);

    // Neighbours squeezed together from automation: sit exactly between them
    // rather than asserting on an inverted range.
    if (low > high)
        return std::sqrt(low * high);

    return juce::jlimit(low, high, hz);
}

float SpectrumDisplay::modulatedFrequencyFor(int index, float baseHz) const
{
    if (index < 0 || index >= kMaxCrossovers)
        return baseHz;

    auto* parameter = crossoverParams[static_cast<size_t>(index)];

    if (parameter == nullptr)
        return baseHz;

    // Crossovers are modulation destinations. The divider itself stays on the
    // value the user edits; where modulation has moved the real edge, that is
    // drawn as a ghost beside it.
    const float offset = processor.getModulationDepth(crossoverIds[static_cast<size_t>(index)]);

    if (std::abs(offset) <= 1.0e-4f)
        return baseHz;

    // The ghost's whole job is to say where the edge REALLY is, so it has to
    // come from the engine rather than be recomputed here. Applying the offset
    // to the parameter and stopping there missed the third-of-an-octave spacing
    // the engine enforces on top, so the marker could sit a third of an octave
    // from the filter it was pointing at - and two ghosts could be drawn in the
    // opposite order to the edges they describe.
    const float applied = processor.getAppliedCrossoverHz(index);

    if (applied > 0.0f)
        return juce::jlimit(minFrequency, maxFrequency, applied);

    const float normalised = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(baseHz) + offset);

    return juce::jlimit(minFrequency, maxFrequency, parameter->convertFrom0to1(normalised));
}

void SpectrumDisplay::setCrossover(int index, float hz)
{
    if (index < 0 || index >= kMaxCrossovers)
        return;

    auto* parameter = crossoverParams[static_cast<size_t>(index)];

    if (parameter == nullptr)
        return;

    const float target = clampCrossover(index, hz);
    const float normalised = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(target));

    // Through the parameter, not the value tree: this is what puts the move in
    // front of the host's automation and the undo manager.
    //
    // Only when it actually differs, though. A drag delivers a mouseDrag for
    // every mouse event, most of which land on the pixel the last one did, and
    // an unchanged setValueNotifyingHost still round-trips through the host and
    // still runs the processor's parameterChanged — which marks the preset
    // dirty. Clamping against a neighbour makes this common: pushing an edge
    // into its neighbour produces the same clamped value on every event.
    if (std::abs(normalised - parameter->getValue()) > 1.0e-6f)
        parameter->setValueNotifyingHost(normalised);

    // Move the displayed state with it rather than waiting for the next timer
    // tick, or a drag repaints its edge in the new place and its ghost in the
    // old one.
    crossoverHz[static_cast<size_t>(index)] = target;
    modulatedHz[static_cast<size_t>(index)] = modulatedFrequencyFor(index, target);
}

void SpectrumDisplay::updateHover(juce::Point<float> position)
{
    const int divider = dividerAt(position);
    const int band = divider >= 0 ? -1 : bandAt(position);

    if (divider == hoveredDivider && band == hoveredBand)
        return;

    hoveredDivider = divider;
    hoveredBand = band;

    setMouseCursor(divider >= 0
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : (band >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
    repaintPlot();
}

void SpectrumDisplay::selectBandAt(juce::Point<float> position)
{
    const int band = bandAt(position);

    if (band < 0)
        return;

    setSelectedBandAndNotify(band);
    repaintPlot();
}

void SpectrumDisplay::setSelectedBandAndNotify(int band)
{
    selectedBand = juce::jlimit(0, kMaxBands - 1, band);
    processor.setSelectedBand(selectedBand);

    if (onBandSelected != nullptr)
        onBandSelected(selectedBand);
}

//==============================================================================
void SpectrumDisplay::mouseMove(const juce::MouseEvent& e)
{
    if (const auto segment = modeSegmentAt(e.position, currentUiScale()); segment != hoveredModeSegment)
    {
        hoveredModeSegment = segment;
        repaint();
    }

    lastMousePosition = e.position;
    mouseInPlot = plotArea.contains(e.position);

    if (const auto node = eqNodeAt(e.position); !(node == hoveredEqNode))
    {
        hoveredEqNode = node;

        if (onEqNodeHovered != nullptr)
            onEqNodeHovered(node.isValid() ? node.band : -1);

        repaintPlot();
    }

    updateHover(e.position);
}

void SpectrumDisplay::mouseEnter(const juce::MouseEvent& e)
{
    updateHover(e.position);
}

void SpectrumDisplay::mouseExit(const juce::MouseEvent&)
{
    if (hoveredDivider < 0 && hoveredBand < 0)
        return;

    hoveredDivider = -1;
    hoveredBand = -1;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaintPlot();
}

void SpectrumDisplay::mouseDown(const juce::MouseEvent& e)
{
    // The mode selector takes the click before anything else: it sits over the
    // plot, and a crossover drag starting on it would be a trap.
    if (gearBounds(currentUiScale()).contains(e.position))
    {
        showAnalyserMenu();
        return;
    }

    if (const auto segment = modeSegmentAt(e.position, currentUiScale()); segment >= 0)
    {
        setDisplayMode(static_cast<DisplayMode>(segment));
        return;
    }

    if (e.mods.isPopupMenu())
    {
        showDisplayMenu(e.position);
        return;
    }

    // A node takes the click before a crossover does: they can sit within a few
    // pixels of each other, and the node is the smaller target, so the larger
    // one must not shadow it.
    if (const auto node = eqNodeAt(e.position); node.isValid())
    {
        draggedEqNode = node;

        if (auto* p = eqNodeGain(node.band, node.node))
            p->beginChangeGesture();

        if (auto* p = eqNodeFreq(node.band, node.node))
            p->beginChangeGesture();

        if (node.band != selectedBand && onBandSelected != nullptr)
            onBandSelected(node.band);

        repaintPlot();
        return;
    }

    const int divider = dividerAt(e.position);

    if (divider >= 0)
    {
        draggedDivider = divider;
        dragFine = e.mods.isShiftDown();
        dragStartX = e.position.x;
        dragStartHz = crossoverHz[static_cast<size_t>(divider)];

        if (auto* parameter = crossoverParams[static_cast<size_t>(divider)])
            parameter->beginChangeGesture();

        repaintPlot();
        return;
    }

    selectBandAt(e.position);
}

void SpectrumDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedEqNode.isValid())
    {
        float bandLow = 20.0f;
        float bandHigh = 20000.0f;
        processor.getBandSpanHz(draggedEqNode.band, bandLow, bandHigh);

        if (auto* p = eqNodeGain(draggedEqNode.band, draggedEqNode.node))
            p->setValueNotifyingHost(p->convertTo0to1(eqDecibelsForY(e.position.y)));

        // Clamped to the band, for the same reason the module's panel clamps:
        // a node outside its band would draw a response that stage cannot
        // produce there.
        if (auto* p = eqNodeFreq(draggedEqNode.band, draggedEqNode.node))
            p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(bandLow, bandHigh, frequencyForX(e.position.x))));

        repaintPlot();
        return;
    }

    if (draggedDivider < 0 || plotArea.getWidth() < 2.0f)
        return;

    // Shift is fine control; re-anchoring on the way in and out of it means the
    // edge never jumps when the modifier changes mid-drag.
    const bool fine = e.mods.isShiftDown();

    if (fine != dragFine)
    {
        dragFine = fine;
        dragStartX = e.position.x;
        dragStartHz = crossoverHz[static_cast<size_t>(draggedDivider)];
    }

    // The drag is exponential because the axis is: a pixel is worth the same
    // musical interval wherever on the display it happens.
    const float delta = (e.position.x - dragStartX) * (fine ? 0.22f : 1.0f);
    const float hz = dragStartHz * std::exp(logFrequencyRatio * delta / plotArea.getWidth());

    // Alt snaps to musical anchor points. The list is the octave-ish spacing
    // engineers already think in, not a uniform grid: 250 Hz and 500 Hz matter,
    // 350 Hz does not.
    float target = hz;

    if (e.mods.isAltDown())
    {
        static constexpr float kSnapPoints[] = {60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};

        float best = target;
        float bestDistance = std::numeric_limits<float>::max();

        for (const auto point : kSnapPoints)
        {
            // Nearest in log distance, because the axis is logarithmic and
            // "nearest in Hz" would snap almost everything to 8 kHz.
            const auto distance = std::abs(std::log(target / point));

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = point;
            }
        }

        target = best;
    }

    target = juce::jlimit(minFrequency, maxFrequency, target);

    if (e.mods.isCommandDown())
    {
        // Link: every crossover moves by the same musical interval, so the band
        // layout keeps its proportions and only shifts up or down.
        const auto ratio = target / juce::jmax(1.0f, crossoverHz[static_cast<size_t>(draggedDivider)]);

        for (int i = 0; i < kMaxCrossovers; ++i)
            if (i != draggedDivider)
                setCrossover(i, juce::jlimit(minFrequency, maxFrequency, crossoverHz[static_cast<size_t>(i)] * ratio));
    }

    setCrossover(draggedDivider, target);
    repaintPlot();
}

juce::Rectangle<float> SpectrumDisplay::gearBounds(float scale) const
{
    const auto selector = modeSelectorBounds(scale);

    if (selector.isEmpty())
        return {};

    const auto size = selector.getHeight();

    return {selector.getX() - size - 4.0f * scale, selector.getY(), size, size};
}

void SpectrumDisplay::paintGear(juce::Graphics& g, float scale) const
{
    const auto bounds = gearBounds(scale);

    if (bounds.isEmpty())
        return;

    const auto& tk = EmberTheme::tokens();

    g.setColour(tk.bgDeep.withAlpha(0.85f));
    g.fillRoundedRectangle(bounds, bounds.getHeight() * 0.3f);
    g.setColour(tk.panelEdge);
    g.drawRoundedRectangle(bounds.reduced(0.5f), bounds.getHeight() * 0.3f, 1.0f);

    // A gear drawn as a ring with teeth: at 17 px a literal cog is mush, and
    // this reads as "settings" at any size.
    const auto centre = bounds.getCentre();
    const auto radius = bounds.getWidth() * 0.26f;

    g.setColour(tk.textDim);
    g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre), 1.4f);

    for (int i = 0; i < 6; ++i)
    {
        const auto angle = juce::MathConstants<float>::twoPi * static_cast<float>(i) / 6.0f;
        const auto inner = centre.getPointOnCircumference(radius * 1.15f, angle);
        const auto outer = centre.getPointOnCircumference(radius * 1.7f, angle);
        g.drawLine({inner, outer}, 1.4f);
    }
}

void SpectrumDisplay::showAnalyserMenu()
{
    auto settings = processor.getAnalyserSettings();

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());

    const auto addChoice = [this](juce::PopupMenu& parent, const juce::String& title, const juce::StringArray& names,
                                  int current, std::function<void(int)> apply)
    {
        juce::PopupMenu sub;

        for (int i = 0; i < names.size(); ++i)
            sub.addItem(names[i], true, i == current,
                        [this, apply, i]
                        {
                            apply(i);
                            repaintPlot();
                        });

        parent.addSubMenu(title, sub);
    };

    addChoice(menu, "Resolution", {"Low", "Medium", "High"}, settings.resolution,
              [this](int v)
              {
                  auto s = processor.getAnalyserSettings();
                  s.resolution = v;
                  processor.setAnalyserSettings(s);
                  rebuildColumns();
              });

    addChoice(menu, "Averaging", {"Fast", "Medium", "Slow"}, settings.averaging,
              [this](int v)
              {
                  auto s = processor.getAnalyserSettings();
                  s.averaging = v;
                  processor.setAnalyserSettings(s);
                  updateSmoothingCoefficients();
              });

    addChoice(menu, "Tilt", {"Flat", "3 dB / octave", "4.5 dB / octave"}, settings.tiltIndex,
              [this](int v)
              {
                  auto s = processor.getAnalyserSettings();
                  s.tiltIndex = v;
                  processor.setAnalyserSettings(s);
              });

    menu.addSeparator();

    menu.addItem("Freeze", true, settings.freeze,
                 [this]
                 {
                     auto s = processor.getAnalyserSettings();
                     s.freeze = !s.freeze;
                     processor.setAnalyserSettings(s);
                 });

    menu.addItem("Peak hold", true, settings.peakHold,
                 [this]
                 {
                     auto s = processor.getAnalyserSettings();
                     s.peakHold = !s.peakHold;
                     processor.setAnalyserSettings(s);
                     repaintPlot();
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMinimumWidth(170));
}

void SpectrumDisplay::showDisplayMenu(juce::Point<float> position)
{
    const auto hz = frequencyForX(position.x);

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());

    const int active = numBands;
    const bool canAdd = active < kMaxBands;

    menu.addItem("Add band at " + (hz >= 1000.0f ? juce::String(hz / 1000.0f, 2) + " kHz"
                                                 : juce::String(juce::roundToInt(hz)) + " Hz"),
                 canAdd, false,
                 [this, hz]
                 {
                     // Raising the band count adds an edge at the top; moving it
                     // to the click is what makes "add a band here" mean here.
                     if (auto* p = processor.getAPVTS().getParameter(pid::numBands))
                     {
                         const auto next = static_cast<float>(numBands + 1);
                         p->beginChangeGesture();
                         p->setValueNotifyingHost(p->convertTo0to1(next));
                         p->endChangeGesture();
                     }

                     setCrossover(juce::jlimit(0, kMaxCrossovers - 1, numBands - 1),
                                  juce::jlimit(minFrequency, maxFrequency, hz));
                 });

    menu.addItem("Distribute bands evenly", active > 1, false,
                 [this]
                 {
                     // Even in log space, not in hertz: evenly spaced in hertz
                     // puts five of six edges above 10 kHz and is useless.
                     const int edges = juce::jmax(1, numBands - 1);

                     for (int i = 0; i < edges; ++i)
                     {
                         const auto t = static_cast<float>(i + 1) / static_cast<float>(edges + 1);
                         setCrossover(i, minFrequency * std::pow(maxFrequency / minFrequency, t));
                     }
                 });

    menu.addSeparator();

    menu.addItem("Reset crossovers", true, false,
                 [this]
                 {
                     for (int i = 0; i < kMaxCrossovers; ++i)
                         if (auto* p = crossoverParams[static_cast<size_t>(i)])
                         {
                             p->beginChangeGesture();
                             p->setValueNotifyingHost(p->getDefaultValue());
                             p->endChangeGesture();
                         }
                 });

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMinimumWidth(180));
}

void SpectrumDisplay::mouseUp(const juce::MouseEvent& e)
{
    if (draggedEqNode.isValid())
    {
        if (auto* p = eqNodeGain(draggedEqNode.band, draggedEqNode.node))
            p->endChangeGesture();

        if (auto* p = eqNodeFreq(draggedEqNode.band, draggedEqNode.node))
            p->endChangeGesture();

        draggedEqNode = {};
        repaintPlot();
    }

    if (draggedDivider >= 0)
    {
        if (auto* parameter = crossoverParams[static_cast<size_t>(draggedDivider)])
            parameter->endChangeGesture();

        draggedDivider = -1;
    }

    updateHover(e.position);
    repaintPlot();
}

void SpectrumDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int divider = draggedDivider >= 0 ? draggedDivider : dividerAt(e.position);

    if (divider < 0)
        return;

    auto* parameter = crossoverParams[static_cast<size_t>(divider)];

    if (parameter == nullptr)
        return;

    // A double-click arrives between this divider's mouseDown and mouseUp, so a
    // gesture is already open; opening a second one would nest them.
    const bool needsGesture = draggedDivider != divider;

    if (needsGesture)
        parameter->beginChangeGesture();

    setCrossover(divider, parameter->convertFrom0to1(parameter->getDefaultValue()));

    if (needsGesture)
        parameter->endChangeGesture();

    // A drag continuing out of the double-click starts from the reset value.
    dragStartX = e.position.x;
    dragStartHz = crossoverHz[static_cast<size_t>(divider)];
    repaintPlot();
}

//==============================================================================
juce::String SpectrumDisplay::getTooltip()
{
    const int divider = draggedDivider >= 0 ? draggedDivider : hoveredDivider;

    if (divider >= 0 && divider < kMaxCrossovers)
        return "Crossover " + juce::String(divider + 1) + " \xe2\x80\x94 " +
               formatFrequency(crossoverHz[static_cast<size_t>(divider)]) +
               "\nDrag to move, shift-drag for fine control, double-click to reset.";

    if (hoveredBand >= 0 && hoveredBand < numBands)
    {
        const float low = hoveredBand == 0 ? minFrequency : crossoverHz[static_cast<size_t>(hoveredBand - 1)];
        const float high = hoveredBand == numBands - 1 ? maxFrequency : crossoverHz[static_cast<size_t>(hoveredBand)];

        return "Band " + juce::String(hoveredBand + 1) + " \xe2\x80\x94 " + formatFrequency(low) + " to " +
               formatFrequency(high) + "\nClick to edit this band.";
    }

    return "Input and output spectrum. Click a band to edit it, drag a divider to move a crossover.";
}
} // namespace ember::gui
