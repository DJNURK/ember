#include "gui/SpectrumDisplay.h"

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
        plotArea = area;
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
}

void SpectrumDisplay::rebuildColumns()
{
    // One screen column per pixel: the curve then has exactly as much detail as
    // the display can show, at any window size.
    const int columns = juce::jlimit(2, 8192, juce::roundToInt(plotArea.getWidth()));
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

    attackCoefficient = 1.0f - std::exp(-1.0f / (rate * 0.020f));
    releaseCoefficient = 1.0f - std::exp(-1.0f / (rate * 0.320f));
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

    if (parametersMoved || heatMoved || curveMoved)
        repaint();
}

bool SpectrumDisplay::refreshParameters()
{
    bool changed = false;

    if (numBandsParam != nullptr)
    {
        const int bands = juce::jlimit(
            kMinBands, kMaxBands, juce::roundToInt(numBandsParam->convertFrom0to1(numBandsParam->getValue())));

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

        const float normalised = parameter->getValue();
        float hz = juce::jlimit(minFrequency, maxFrequency, parameter->convertFrom0to1(normalised));

        // Keep the edges ascending and a third of an octave apart exactly as the
        // engine does, so a region boundary is always a real band boundary.
        if (i > 0)
            hz = juce::jlimit(minFrequency, maxFrequency, juce::jmax(hz, crossoverHz[index - 1] * kMinCrossoverRatio));

        // Crossovers are modulation destinations; when something is sweeping one
        // the divider stays on the value the user edits and the live position is
        // drawn as a ghost beside it.
        const float offset = processor.getModulationDepth(crossoverIds[index]);
        const float modulated =
            std::abs(offset) > 1.0e-4f
                ? juce::jlimit(minFrequency, maxFrequency,
                               parameter->convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalised + offset)))
                : hz;

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

    for (int band = 0; band < kMaxBands; ++band)
    {
        const auto index = static_cast<size_t>(band);
        float target = 0.0f;

        if (band < numBands && driveParams[index] != nullptr)
        {
            // Drive is what the band is ASKED to do; its level is what it is
            // actually doing. A hard-driven band with nothing going through it
            // glows faintly, one with signal in it glows properly.
            const float drive = juce::jlimit(0.0f, 1.0f, driveParams[index]->getValue()
                                                             + processor.getModulationDepth(driveIds[index]));
            const float levelDb =
                juce::Decibels::gainToDecibels(juce::jmax(0.0f, processor.getBandLevel(band)), -60.0f);
            const float level = juce::jlimit(0.0f, 1.0f, (levelDb + 60.0f) / 60.0f);

            target = juce::jlimit(0.0f, 1.0f, drive * (0.45f + 0.55f * level));
        }

        float& heat = bandHeat[index];
        const float previous = heat;

        heat += (target - heat) * (target > heat ? 0.45f : 0.12f);

        if (std::abs(heat - previous) > 0.002f)
            changed = true;
    }

    return changed;
}

bool SpectrumDisplay::refreshSpectrum()
{
    const int columns = static_cast<int>(inputCurve.size());

    if (columns < 2 || outputCurve.size() != inputCurve.size() || plotArea.getWidth() < 2.0f)
        return false;

    // Wait-free: false simply means the engine has not published a frame yet, so
    // there is nothing to fade in or out — keep what is on screen.
    if (!processor.getSpectrumFifo().readLatest(frame))
        return false;

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

    for (int column = 0; column < columns; ++column)
    {
        const float left = plotArea.getX() + static_cast<float>(column) * columnWidth;
        const float firstBin = frequencyForX(left) * binsPerHz;
        const float lastBin = frequencyForX(left + columnWidth) * binsPerHz;

        const auto index = static_cast<size_t>(column);
        const float inputTarget = juce::jmax(floorDb, aggregateBins(frame.inputDb, firstBin, lastBin));
        const float outputTarget = juce::jmax(floorDb, aggregateBins(frame.outputDb, firstBin, lastBin));

        largestMove = juce::jmax(largestMove, advance(inputCurve[index], inputTarget));
        largestMove = juce::jmax(largestMove, advance(outputCurve[index], outputTarget));
    }

    return largestMove > 0.02f;
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

    EmberLookAndFeel::drawWell(g, bounds, corner);

    if (plotArea.getWidth() < 8.0f || plotArea.getHeight() < 8.0f)
        return;

    juce::Graphics::ScopedSaveState saved(g);

    juce::Path clip;
    clip.addRoundedRectangle(bounds.reduced(1.0f), corner);
    g.reduceClipRegion(clip);

    paintBandRegions(g, scale);
    paintGrid(g, scale);
    paintSpectra(g, scale);
    paintCrossovers(g, scale);
    paintDragReadout(g, scale);
}

void SpectrumDisplay::paintBandRegions(juce::Graphics& g, float scale) const
{
    const float tabHeight = juce::jmax(2.0f, 2.5f * scale);
    const auto font = EmberFonts::get(EmberFonts::Role::micro, scale);
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

        // Intensity carries the band's heat; the selected band is lifted clear of
        // the others whatever it is doing.
        const float alpha = (selected ? 0.20f : 0.06f) + heat * (selected ? 0.28f : 0.18f)
                            + (hovered && !selected ? 0.04f : 0.0f);

        g.setGradientFill(juce::ColourGradient(colour.withAlpha(alpha), region.getCentreX(), region.getY(),
                                               colour.withAlpha(alpha * 0.16f), region.getCentreX(),
                                               region.getBottom(), false));
        g.fillRect(region);

        g.setColour(colour.withAlpha(selected ? 1.0f : juce::jlimit(0.0f, 1.0f, 0.32f + heat * 0.5f)));
        g.fillRect(region.withHeight(tabHeight));

        if (selected)
        {
            g.setColour(colour.withAlpha(0.28f));
            g.drawRect(region.reduced(0.5f), 1.0f);
        }

        if (region.getWidth() > labelHeight * 1.8f && region.getHeight() > labelHeight * 4.0f)
        {
            const auto labelArea =
                juce::Rectangle<float>(region.getX() + 4.0f * scale, region.getY() + tabHeight + 2.0f * scale,
                                       region.getWidth() - 8.0f * scale, labelHeight);

            g.setFont(font);
            g.setColour(selected ? colour.brighter(0.3f) : colour.withAlpha(0.55f));
            g.drawText(juce::String(band + 1), labelArea, juce::Justification::topLeft, false);
        }
    }
}

void SpectrumDisplay::paintGrid(juce::Graphics& g, float scale) const
{
    const auto font = EmberFonts::get(EmberFonts::Role::micro, scale);
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
                                           EmberColours::outline.withAlpha(decibels < -0.5f ? 0.42f : 0.7f));

            if (plotArea.getWidth() > labelWidth * 3.0f)
            {
                const auto textArea = juce::Rectangle<float>(plotArea.getRight() - labelWidth - 3.0f * scale,
                                                             y - labelHeight - 1.0f, labelWidth, labelHeight);

                g.setColour(EmberColours::textDisabled.withAlpha(0.6f));
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
                                       EmberColours::outline.withAlpha(0.48f), true);

        if (!roomForLabels)
            continue;

        const juce::String text(line.label);
        const float width = juce::GlyphArrangement::getStringWidth(font, text) + 10.0f * scale;
        const auto textArea = juce::Rectangle<float>(x - width * 0.5f, axisArea.getY(), width, axisArea.getHeight());

        // Skip a label rather than let two collide on a narrow window.
        if (textArea.getX() <= lastLabelRight || textArea.getX() < 0.0f
            || textArea.getRight() > static_cast<float>(getWidth()))
            continue;

        g.setColour(EmberColours::textDisabled);
        g.drawText(text, textArea, juce::Justification::centred, false);
        lastLabelRight = textArea.getRight();
    }
}

void SpectrumDisplay::paintSpectra(juce::Graphics& g, float scale) const
{
    if (!hasFrame || inputCurve.size() < 2 || outputCurve.size() < 2)
        return;

    juce::Path inputLine;
    buildCurvePath(inputLine, inputCurve);

    if (!inputLine.isEmpty())
    {
        // The input is a body, not a line: a filled shape the output curve can
        // be read against without the two ever being confused.
        juce::Path inputFill(inputLine);
        inputFill.lineTo(plotArea.getRight(), plotArea.getBottom() + 2.0f);
        inputFill.lineTo(plotArea.getX(), plotArea.getBottom() + 2.0f);
        inputFill.closeSubPath();

        g.setGradientFill(juce::ColourGradient(EmberColours::textSecondary.withAlpha(0.20f), plotArea.getCentreX(),
                                               plotArea.getY(), EmberColours::textSecondary.withAlpha(0.05f),
                                               plotArea.getCentreX(), plotArea.getBottom(), false));
        g.fillPath(inputFill);

        g.setColour(EmberColours::textSecondary.withAlpha(0.34f));
        g.strokePath(inputLine, juce::PathStrokeType(juce::jmax(1.0f, 0.9f * scale), juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    juce::Path outputLine;
    buildCurvePath(outputLine, outputCurve);

    if (!outputLine.isEmpty())
    {
        g.setColour(EmberColours::accentGlow.withAlpha(0.16f));
        g.strokePath(outputLine, juce::PathStrokeType(juce::jmax(2.5f, 3.4f * scale), juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

        g.setColour(EmberColours::accent.withAlpha(0.94f));
        g.strokePath(outputLine, juce::PathStrokeType(juce::jmax(1.0f, 1.5f * scale), juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    }
}

void SpectrumDisplay::buildCurvePath(juce::Path& path, const std::vector<float>& curve) const
{
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
    const float handleWidth = juce::jmax(5.0f, 7.0f * scale);
    const float handleHeight = juce::jmin(juce::jmax(10.0f, 15.0f * scale), plotArea.getHeight() * 0.5f);

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

            g.setColour(EmberColours::accent.withAlpha(0.32f));
            g.fillRect(juce::Rectangle<float>(modulatedX - 0.5f, plotArea.getY(), 1.0f, plotArea.getHeight()));
        }

        const float thickness = highlighted ? juce::jmax(1.5f, 1.6f * scale) : juce::jmax(1.0f, 1.1f * scale);

        g.setColour(highlighted ? EmberColours::accent : EmberColours::outlineStrong.withAlpha(0.8f));
        g.fillRect(juce::Rectangle<float>(x - thickness * 0.5f, plotArea.getY(), thickness, plotArea.getHeight()));

        if (handleHeight < 6.0f)
            continue;

        const auto handle =
            juce::Rectangle<float>(handleWidth, handleHeight)
                .withCentre(juce::Point<float>(x, plotArea.getBottom() - handleHeight * 0.5f - 2.0f * scale));

        g.setColour(highlighted ? EmberColours::accent : EmberColours::panelRaised);
        g.fillRoundedRectangle(handle, handleWidth * 0.45f);
        g.setColour(highlighted ? EmberColours::accent.brighter(0.25f) : EmberColours::outlineStrong);
        g.drawRoundedRectangle(handle.reduced(0.5f), handleWidth * 0.45f, 1.0f);

        const float gripOffset = handleWidth * 0.22f;
        const float gripHeight = handleHeight * 0.44f;
        const float gripTop = handle.getCentreY() - gripHeight * 0.5f;

        g.setColour(highlighted ? EmberColours::backgroundDeep.withAlpha(0.75f)
                                : EmberColours::textDisabled.withAlpha(0.9f));
        g.fillRect(juce::Rectangle<float>(x - gripOffset - 0.5f, gripTop, 1.0f, gripHeight));
        g.fillRect(juce::Rectangle<float>(x + gripOffset - 0.5f, gripTop, 1.0f, gripHeight));
    }
}

void SpectrumDisplay::paintDragReadout(juce::Graphics& g, float scale) const
{
    if (draggedDivider < 0 || draggedDivider >= kMaxCrossovers)
        return;

    const auto font = EmberFonts::get(EmberFonts::Role::value, scale);
    const juce::String text = formatFrequency(crossoverHz[static_cast<size_t>(draggedDivider)]);
    const float boxWidth = juce::GlyphArrangement::getStringWidth(font, text) + 16.0f * scale;
    const float boxHeight = font.getHeight() + 8.0f * scale;

    if (boxWidth > plotArea.getWidth() || boxHeight > plotArea.getHeight())
        return;

    const float x = xForFrequency(crossoverHz[static_cast<size_t>(draggedDivider)]);

    auto box = juce::Rectangle<float>(boxWidth, boxHeight)
                   .withCentre(juce::Point<float>(x, plotArea.getY() + boxHeight * 0.5f + 5.0f * scale));

    box.setX(juce::jlimit(plotArea.getX() + 2.0f, plotArea.getRight() - boxWidth - 2.0f, box.getX()));

    const float corner = juce::jmax(2.0f, 3.0f * scale);

    g.setColour(EmberColours::backgroundDeep.withAlpha(0.94f));
    g.fillRoundedRectangle(box, corner);
    g.setColour(EmberColours::accent.withAlpha(0.85f));
    g.drawRoundedRectangle(box.reduced(0.5f), corner, 1.0f);

    g.setFont(font);
    g.setColour(EmberColours::textPrimary);
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

void SpectrumDisplay::setCrossover(int index, float hz)
{
    if (index < 0 || index >= kMaxCrossovers)
        return;

    auto* parameter = crossoverParams[static_cast<size_t>(index)];

    if (parameter == nullptr)
        return;

    const float target = clampCrossover(index, hz);

    // Through the parameter, not the value tree: this is what puts the move in
    // front of the host's automation and the undo manager.
    parameter->setValueNotifyingHost(parameter->convertTo0to1(target));
    crossoverHz[static_cast<size_t>(index)] = target;
}

void SpectrumDisplay::updateHover(juce::Point<float> position)
{
    const int divider = dividerAt(position);
    const int band = divider >= 0 ? -1 : bandAt(position);

    if (divider == hoveredDivider && band == hoveredBand)
        return;

    hoveredDivider = divider;
    hoveredBand = band;

    setMouseCursor(divider >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                : (band >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
    repaint();
}

void SpectrumDisplay::selectBandAt(juce::Point<float> position)
{
    const int band = bandAt(position);

    if (band < 0)
        return;

    setSelectedBandAndNotify(band);
    repaint();
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
    repaint();
}

void SpectrumDisplay::mouseDown(const juce::MouseEvent& e)
{
    const int divider = dividerAt(e.position);

    if (divider >= 0)
    {
        draggedDivider = divider;
        dragFine = e.mods.isShiftDown();
        dragStartX = e.position.x;
        dragStartHz = crossoverHz[static_cast<size_t>(divider)];

        if (auto* parameter = crossoverParams[static_cast<size_t>(divider)])
            parameter->beginChangeGesture();

        repaint();
        return;
    }

    selectBandAt(e.position);
}

void SpectrumDisplay::mouseDrag(const juce::MouseEvent& e)
{
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

    setCrossover(draggedDivider, juce::jlimit(minFrequency, maxFrequency, hz));
    repaint();
}

void SpectrumDisplay::mouseUp(const juce::MouseEvent& e)
{
    if (draggedDivider >= 0)
    {
        if (auto* parameter = crossoverParams[static_cast<size_t>(draggedDivider)])
            parameter->endChangeGesture();

        draggedDivider = -1;
    }

    updateHover(e.position);
    repaint();
}

void SpectrumDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int divider = draggedDivider >= 0 ? draggedDivider : dividerAt(e.position);

    if (divider < 0)
        return;

    auto* parameter = crossoverParams[static_cast<size_t>(divider)];

    if (parameter == nullptr)
        return;

    const float target = clampCrossover(divider, parameter->convertFrom0to1(parameter->getDefaultValue()));

    // A double-click arrives between this divider's mouseDown and mouseUp, so a
    // gesture is already open; opening a second one would nest them.
    if (draggedDivider != divider)
        parameter->beginChangeGesture();

    parameter->setValueNotifyingHost(parameter->convertTo0to1(target));

    if (draggedDivider != divider)
        parameter->endChangeGesture();

    crossoverHz[static_cast<size_t>(divider)] = target;
    dragStartX = e.position.x;
    dragStartHz = target;
    repaint();
}

//==============================================================================
juce::String SpectrumDisplay::getTooltip()
{
    const int divider = draggedDivider >= 0 ? draggedDivider : hoveredDivider;

    if (divider >= 0 && divider < kMaxCrossovers)
        return "Crossover " + juce::String(divider + 1) + " \xe2\x80\x94 "
             + formatFrequency(crossoverHz[static_cast<size_t>(divider)])
             + "\nDrag to move, shift-drag for fine control, double-click to reset.";

    if (hoveredBand >= 0 && hoveredBand < numBands)
    {
        const float low =
            hoveredBand == 0 ? minFrequency : crossoverHz[static_cast<size_t>(hoveredBand - 1)];
        const float high = hoveredBand == numBands - 1 ? maxFrequency
                                                       : crossoverHz[static_cast<size_t>(hoveredBand)];

        return "Band " + juce::String(hoveredBand + 1) + " \xe2\x80\x94 " + formatFrequency(low) + " to "
             + formatFrequency(high) + "\nClick to edit this band.";
    }

    return "Input and output spectrum. Click a band to edit it, drag a divider to move a crossover.";
}
} // namespace ember::gui
