#include "gui/ToneEqPanel.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include "plugin/ParameterIDs.h"
#include "plugin/PluginProcessor.h"

namespace ember::gui
{
namespace
{
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kRangeDb = 12.0f;
constexpr int kCurvePoints = 160;
constexpr float kNodeRadius = 5.0f;
constexpr float kHoveredNodeRadius = 6.5f;

float logPosition(float hz)
{
    return std::log10(juce::jlimit(kMinHz, kMaxHz, hz) / kMinHz) / std::log10(kMaxHz / kMinHz);
}

float hzForPosition(float position)
{
    return kMinHz * std::pow(kMaxHz / kMinHz, juce::jlimit(0.0f, 1.0f, position));
}
} // namespace

ToneEqPanel::ToneEqPanel(EmberAudioProcessor& processorToUse, int bandIndex)
    : processor(processorToUse), band(juce::jlimit(0, kMaxBands - 1, bandIndex))
{
    // Prepared at a nominal rate purely so the coefficient design is valid; the
    // curve is a function of the design, and the host's real rate only matters
    // near Nyquist, where the plot has already ended.
    drawingFilter.prepare(48000.0, 64, 1);

    setTooltip("Tone for this band. Drag a node to move it, Alt-drag or scroll the mid node for Q, "
               "double-click to reset.");

    startTimerHz(30);
}

ToneEqPanel::~ToneEqPanel() = default;

void ToneEqPanel::setCompact(bool shouldBeCompact)
{
    if (compact == shouldBeCompact)
        return;

    compact = shouldBeCompact;
    resized();
    repaint();
}

//==============================================================================
juce::RangedAudioParameter* ToneEqPanel::gainParam(Node node) const
{
    auto& state = processor.getAPVTS();

    switch (node)
    {
    case Node::low:
        return state.getParameter(pid::toneLow(band));
    case Node::mid:
        return state.getParameter(pid::toneMid(band));
    case Node::high:
        return state.getParameter(pid::toneHigh(band));
    case Node::count:
        break;
    }

    return nullptr;
}

juce::RangedAudioParameter* ToneEqPanel::freqParam(Node node) const
{
    auto& state = processor.getAPVTS();

    switch (node)
    {
    case Node::low:
        return state.getParameter(pid::toneLowHz(band));
    case Node::mid:
        return state.getParameter(pid::toneMidHz(band));
    case Node::high:
        return state.getParameter(pid::toneHighHz(band));
    case Node::count:
        break;
    }

    return nullptr;
}

void ToneEqPanel::setParam(juce::RangedAudioParameter* parameter, float denormalised, bool asGesture)
{
    if (parameter == nullptr)
        return;

    const auto normalised = parameter->convertTo0to1(denormalised);

    if (asGesture)
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(normalised);
        parameter->endChangeGesture();
    }
    else
    {
        parameter->setValueNotifyingHost(normalised);
    }
}

//==============================================================================
float ToneEqPanel::xForFrequency(float hz) const
{
    return plot.getX() + logPosition(hz) * plot.getWidth();
}

float ToneEqPanel::frequencyForX(float x) const
{
    return plot.getWidth() > 1.0f ? hzForPosition((x - plot.getX()) / plot.getWidth()) : kMinHz;
}

float ToneEqPanel::yForDecibels(float db) const
{
    const auto t = (kRangeDb - juce::jlimit(-kRangeDb, kRangeDb, db)) / (2.0f * kRangeDb);
    return plot.getY() + t * plot.getHeight();
}

float ToneEqPanel::decibelsForY(float y) const
{
    if (plot.getHeight() < 1.0f)
        return 0.0f;

    const auto t = juce::jlimit(0.0f, 1.0f, (y - plot.getY()) / plot.getHeight());
    return kRangeDb - t * 2.0f * kRangeDb;
}

juce::Point<float> ToneEqPanel::positionOf(Node node) const
{
    const auto index = static_cast<size_t>(node);

    if (index >= shownGain.size())
        return {};

    return {xForFrequency(shownFreq[index]), yForDecibels(shownGain[index])};
}

ToneEqPanel::Node ToneEqPanel::nodeAt(juce::Point<float> position) const
{
    // Nearest within a generous radius: exact hit testing on a 5 px disc is
    // unusable with a trackpad, and the three nodes are far apart in practice.
    auto best = Node::count;
    float bestDistance = kHoveredNodeRadius * 3.0f;

    for (int i = 0; i < kNumNodes; ++i)
    {
        const auto node = static_cast<Node>(i);
        const auto distance = position.getDistanceFrom(positionOf(node));

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = node;
        }
    }

    return best;
}

//==============================================================================
bool ToneEqPanel::refreshCurve()
{
    auto& state = processor.getAPVTS();

    const auto read = [&state](const juce::String& id, float fallback)
    {
        if (auto* value = state.getRawParameterValue(id))
            return value->load(std::memory_order_relaxed);

        return fallback;
    };

    const std::array<float, 3> gains{read(pid::toneLow(band), 0.0f), read(pid::toneMid(band), 0.0f),
                                     read(pid::toneHigh(band), 0.0f)};
    const std::array<float, 3> freqs{read(pid::toneLowHz(band), 150.0f), read(pid::toneMidHz(band), 1000.0f),
                                     read(pid::toneHighHz(band), 4000.0f)};
    const auto q = read(pid::toneMidQ(band), 0.7f);
    const auto heat = processor.getBandHeat(band);

    const bool moved = gains != shownGain || freqs != shownFreq || ! juce::exactlyEqual(q, shownQ)
                       || std::abs(heat - shownHeat) > 0.01f;

    if (! moved && ! curve.isEmpty())
        return false;

    shownGain = gains;
    shownFreq = freqs;
    shownQ = q;
    shownHeat = heat;

    drawingFilter.setShape({freqs[0], freqs[1], q, freqs[2]});
    drawingFilter.setGainsDb(gains[0], gains[1], gains[2]);

    curve.clear();

    if (plot.getWidth() < 4.0f || plot.getHeight() < 4.0f)
        return true;

    for (int i = 0; i < kCurvePoints; ++i)
    {
        const auto t = static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
        const auto hz = hzForPosition(t);
        const auto x = plot.getX() + t * plot.getWidth();
        const auto y = yForDecibels(drawingFilter.magnitudeDbAt(hz));

        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }

    return true;
}

void ToneEqPanel::timerCallback()
{
    // The band's span moves when a crossover moves, and the curve's fade
    // depends on it.
    const auto previousLow = bandLowHz;
    const auto previousHigh = bandHighHz;

    processor.getBandSpanHz(band, bandLowHz, bandHighHz);

    if (refreshCurve() || ! juce::exactlyEqual(previousLow, bandLowHz)
        || ! juce::exactlyEqual(previousHigh, bandHighHz))
        repaint();
}

void ToneEqPanel::resized()
{
    const auto area = getLocalBounds().toFloat();

    plot = compact ? area : area.reduced(static_cast<float>(Spacing::sm));

    curve.clear(); // forces a rebuild at the new size
    refreshCurve();
}

//==============================================================================
void ToneEqPanel::paint(juce::Graphics& g)
{
    const auto& tk = EmberTheme::tokens();

    if (plot.getWidth() < 4.0f || plot.getHeight() < 3.0f)
        return;

    if (compact)
    {
        // The sparkline: no grid, no nodes, just the shape, so an unselected
        // module still says at a glance whether it has tone on it.
        g.setColour(tk.bgDeep);
        g.fillRoundedRectangle(plot, Metrics::controlRadius);

        g.setColour(shownHeat > 0.02f ? tk.heatTint(shownHeat) : tk.textDim);
        g.strokePath(curve, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));
        return;
    }

    EmberLookAndFeel::drawWell(g, plot, Metrics::controlRadius);

    // ---- grid: 3 dB steps, the centre line stronger ------------------------
    for (int db = -12; db <= 12; db += 3)
    {
        const auto y = yForDecibels(static_cast<float>(db));
        g.setColour(db == 0 ? tk.textMuted.withAlpha(0.55f) : tk.textMuted.withAlpha(0.28f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));
    }

    for (const float hz : {100.0f, 1000.0f, 10000.0f})
    {
        const auto x = xForFrequency(hz);
        g.setColour(tk.textMuted.withAlpha(0.28f));
        g.fillRect(juce::Rectangle<float>(x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));
    }

    // ---- the band's own span ----------------------------------------------
    // Everything outside it is faded: this stage only shapes this band, and a
    // curve drawn at full strength across the whole spectrum would say
    // otherwise.
    const auto spanLeft = xForFrequency(bandLowHz);
    const auto spanRight = xForFrequency(bandHighHz);

    {
        juce::Graphics::ScopedSaveState save{g};
        g.reduceClipRegion(plot.toNearestInt());

        g.setColour(tk.panelRaised.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), plot.getY(), spanLeft - plot.getX(), plot.getHeight()));
        g.fillRect(juce::Rectangle<float>(spanRight, plot.getY(), plot.getRight() - spanRight, plot.getHeight()));
    }

    // ---- the curve ---------------------------------------------------------
    const auto curveColour = shownHeat > 0.02f ? tk.heatTint(shownHeat) : tk.ember;

    {
        juce::Graphics::ScopedSaveState save{g};
        g.reduceClipRegion(plot.toNearestInt());

        // Outside the span first, faint, then the in-band section over it at
        // full strength.
        g.setColour(curveColour.withAlpha(0.20f));
        g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));

        juce::Graphics::ScopedSaveState inBand{g};
        g.reduceClipRegion(juce::Rectangle<float>(spanLeft, plot.getY(), juce::jmax(0.0f, spanRight - spanLeft),
                                                  plot.getHeight())
                               .toNearestInt());

        if (shownHeat > 0.02f && ! EmberTheme::reduceMotion())
        {
            g.setColour(tk.tubeGlow.withAlpha(shownHeat * 0.45f));
            g.strokePath(curve, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved));
        }

        g.setColour(curveColour);
        g.strokePath(curve, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
    }

    // ---- the nodes ---------------------------------------------------------
    for (int i = 0; i < kNumNodes; ++i)
    {
        const auto node = static_cast<Node>(i);
        const auto centre = positionOf(node);
        const bool active = node == hovered || node == dragging;
        const auto radius = active ? kHoveredNodeRadius : kNodeRadius;

        if (active && ! EmberTheme::reduceMotion())
            GlowCache::draw(g, centre, radius * 2.6f, tk.tubeGlow, 0.45f);

        const auto disc = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);

        g.setColour(tk.panelShadow.withAlpha(0.6f));
        g.fillEllipse(disc.translated(0.0f, 1.0f));

        juce::ColourGradient cap(tk.panelRaised.brighter(0.25f), disc.getX(), disc.getY(), tk.panelRaised.darker(0.3f),
                                 disc.getRight(), disc.getBottom(), true);
        g.setGradientFill(cap);
        g.fillEllipse(disc);

        g.setColour(tk.ember);
        g.fillEllipse(disc.reduced(radius * 0.55f));

        g.setColour(tk.panelEdge.brighter(active ? 0.3f : 0.1f));
        g.drawEllipse(disc.reduced(0.5f), 1.0f);
    }

    // ---- the corner chips --------------------------------------------------
    {
        const auto read = [this](const juce::String& id)
        {
            auto* v = processor.getAPVTS().getRawParameterValue(id);
            return v != nullptr && v->load(std::memory_order_relaxed) > 0.5f;
        };

        const bool pre = read(pid::tonePre(band));
        const bool bypassed = read(pid::toneBypass(band));

        const auto drawChip = [&](Chip chip, const juce::String& label, bool lit)
        {
            const auto bounds = chipBounds(chip);

            if (bounds.isEmpty())
                return;

            const bool over = chip == hoveredChip;

            g.setColour(lit ? tk.ember.withAlpha(0.22f) : tk.bgDeep.withAlpha(0.7f));
            g.fillRoundedRectangle(bounds, bounds.getHeight() * 0.35f);

            g.setColour(lit ? tk.ember : tk.panelEdge.brighter(over ? 0.3f : 0.0f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), bounds.getHeight() * 0.35f, 1.0f);

            g.setFont(EmberFonts::get(EmberFonts::Role::micro));
            g.setColour(lit ? tk.text : tk.textDim);
            g.drawText(label, bounds, juce::Justification::centred, false);
        };

        drawChip(Chip::prePost, pre ? "PRE" : "POST", pre);
        drawChip(Chip::flat, "FLAT", false);
        // The bypass chip is a lamp: lit means the tone stage is off, which is
        // the state worth noticing.
        {
            const auto bounds = chipBounds(Chip::bypass);

            if (! bounds.isEmpty())
            {
                g.setColour(tk.bgDeep);
                g.fillEllipse(bounds);

                if (bypassed)
                {
                    if (! EmberTheme::reduceMotion())
                        GlowCache::draw(g, bounds.getCentre(), bounds.getWidth() * 1.3f, tk.tubeGlow, 0.4f);

                    g.setColour(tk.ember);
                    g.fillEllipse(bounds.reduced(bounds.getWidth() * 0.28f));
                }
                else
                {
                    g.setColour(tk.textMuted.withAlpha(0.6f));
                    g.fillEllipse(bounds.reduced(bounds.getWidth() * 0.34f));
                }

                g.setColour(tk.panelEdge.brighter(hoveredChip == Chip::bypass ? 0.3f : 0.0f));
                g.drawEllipse(bounds.reduced(0.5f), 1.0f);
            }
        }
    }

    // ---- the value pill ----------------------------------------------------
    const auto shown = dragging != Node::count ? dragging : hovered;

    if (shown != Node::count)
    {
        const auto index = static_cast<size_t>(shown);

        juce::String text = juce::String(shownGain[index], 1) + " dB";
        text += "  " + (shownFreq[index] >= 1000.0f
                            ? juce::String(shownFreq[index] / 1000.0f, 2) + " kHz"
                            : juce::String(juce::roundToInt(shownFreq[index])) + " Hz");

        if (shown == Node::mid)
            text += "  Q " + juce::String(shownQ, 2);

        const auto font = EmberFonts::get(EmberFonts::Role::value);
        const auto width = juce::GlyphArrangement::getStringWidth(font, text) + 14.0f;
        const auto height = font.getHeight() + 8.0f;

        auto pill = juce::Rectangle<float>(width, height).withCentre(positionOf(shown).translated(0.0f, -height - 6.0f));

        // Keep it inside the plot: a pill that runs off the edge is the one
        // thing a value readout must never do.
        pill.setX(juce::jlimit(plot.getX(), juce::jmax(plot.getX(), plot.getRight() - width), pill.getX()));
        pill.setY(juce::jmax(plot.getY() + 1.0f, pill.getY()));

        g.setColour(tk.bgDeep.withAlpha(0.92f));
        g.fillRoundedRectangle(pill, height * 0.5f);
        g.setColour(tk.panelEdge);
        g.drawRoundedRectangle(pill.reduced(0.5f), height * 0.5f, 1.0f);

        g.setFont(font);
        g.setColour(tk.text);
        g.drawText(text, pill, juce::Justification::centred, false);
    }
}


//==============================================================================
juce::Rectangle<float> ToneEqPanel::chipBounds(Chip chip) const
{
    if (compact || plot.getWidth() < 150.0f || plot.getHeight() < 50.0f)
        return {};

    const float height = juce::jlimit(11.0f, 16.0f, plot.getHeight() * 0.17f);
    const float pad = 3.0f;

    // Measured, not guessed. Sizing these by eye truncated "FLAT" to "FLA" at
    // the default window size - a label that does not fit is worse than no
    // label, because it looks like a different word.
    const auto font = EmberFonts::get(EmberFonts::Role::micro);
    const auto widthFor = [&font, height](const juce::String& text)
    { return juce::jmax(height, juce::GlyphArrangement::getStringWidth(font, text) + 10.0f); };

    // PRE/POST is sized for the longer of its two states, so the chip does not
    // resize as it toggles.
    const auto prePostWidth = widthFor("POST");
    const auto flatWidth = widthFor("FLAT");

    switch (chip)
    {
    case Chip::prePost:
        return {plot.getX() + pad, plot.getY() + pad, prePostWidth, height};
    case Chip::bypass:
        return {plot.getRight() - pad - height, plot.getY() + pad, height, height};
    case Chip::flat:
        return {plot.getRight() - pad * 2.0f - height - flatWidth, plot.getY() + pad, flatWidth, height};
    case Chip::count:
        break;
    }

    return {};
}

ToneEqPanel::Chip ToneEqPanel::chipAt(juce::Point<float> position) const
{
    for (int i = 0; i < static_cast<int>(Chip::count); ++i)
    {
        const auto chip = static_cast<Chip>(i);

        if (chipBounds(chip).contains(position))
            return chip;
    }

    return Chip::count;
}

void ToneEqPanel::clickChip(Chip chip)
{
    auto& state = processor.getAPVTS();

    const auto toggle = [&state](const juce::String& id)
    {
        if (auto* p = state.getParameter(id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
            p->endChangeGesture();
        }
    };

    switch (chip)
    {
    case Chip::prePost:
        toggle(pid::tonePre(band));
        break;

    case Chip::bypass:
        toggle(pid::toneBypass(band));
        break;

    case Chip::flat:
        // Flat resets gains only. The node positions are where the user put
        // them, and throwing those away as well would make this destructive
        // rather than useful.
        for (int i = 0; i < kNumNodes; ++i)
            setParam(gainParam(static_cast<Node>(i)), 0.0f, true);
        break;

    case Chip::count:
        break;
    }
}

//==============================================================================
void ToneEqPanel::mouseMove(const juce::MouseEvent& event)
{
    if (compact)
        return;

    const auto chip = chipAt(event.position);

    if (chip != hoveredChip)
    {
        hoveredChip = chip;
        repaint();
    }

    const auto node = chip == Chip::count ? nodeAt(event.position) : Node::count;

    if (node != hovered)
    {
        hovered = node;
        setMouseCursor(node != Node::count ? juce::MouseCursor::UpDownLeftRightResizeCursor
                                           : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ToneEqPanel::mouseExit(const juce::MouseEvent&)
{
    if (hovered == Node::count)
        return;

    hovered = Node::count;
    repaint();
}

void ToneEqPanel::mouseDown(const juce::MouseEvent& event)
{
    if (compact)
        return;

    if (const auto chip = chipAt(event.position); chip != Chip::count)
    {
        clickChip(chip);
        repaint();
        return;
    }

    dragging = nodeAt(event.position);

    if (dragging == Node::count)
        return;

    // Alt on the mid node edits Q instead of position, matching the scroll
    // wheel - both are "change the shape, not where it is".
    draggingQ = dragging == Node::mid && event.mods.isAltDown();

    if (auto* p = gainParam(dragging))
        p->beginChangeGesture();

    if (auto* p = freqParam(dragging))
        p->beginChangeGesture();

    if (draggingQ)
        if (auto* p = processor.getAPVTS().getParameter(pid::toneMidQ(band)))
            p->beginChangeGesture();

    repaint();
}

void ToneEqPanel::mouseDrag(const juce::MouseEvent& event)
{
    if (dragging == Node::count)
        return;

    if (draggingQ)
    {
        if (auto* p = processor.getAPVTS().getParameter(pid::toneMidQ(band)))
        {
            // Vertical drag, because horizontal is frequency everywhere else in
            // this panel and reusing it for Q would be a trap.
            const auto delta = static_cast<float>(-event.getDistanceFromDragStartY()) * 0.01f;
            const auto current = p->convertTo0to1(shownQ);
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, current + delta));
        }

        return;
    }

    setParam(gainParam(dragging), decibelsForY(event.position.y), false);

    // The node stays inside its own band: moving a low shelf up into the next
    // band's territory would draw a curve this stage cannot produce there.
    const auto requested = frequencyForX(event.position.x);
    setParam(freqParam(dragging), juce::jlimit(bandLowHz, bandHighHz, requested), false);
}

void ToneEqPanel::mouseUp(const juce::MouseEvent&)
{
    if (dragging == Node::count)
        return;

    if (auto* p = gainParam(dragging))
        p->endChangeGesture();

    if (auto* p = freqParam(dragging))
        p->endChangeGesture();

    if (draggingQ)
        if (auto* p = processor.getAPVTS().getParameter(pid::toneMidQ(band)))
            p->endChangeGesture();

    dragging = Node::count;
    draggingQ = false;
    repaint();
}

void ToneEqPanel::mouseDoubleClick(const juce::MouseEvent& event)
{
    const auto node = nodeAt(event.position);

    if (node == Node::count)
        return;

    if (auto* p = gainParam(node))
        setParam(p, p->convertFrom0to1(p->getDefaultValue()), true);

    if (auto* p = freqParam(node))
        setParam(p, p->convertFrom0to1(p->getDefaultValue()), true);

    if (node == Node::mid)
        if (auto* p = processor.getAPVTS().getParameter(pid::toneMidQ(band)))
            setParam(p, p->convertFrom0to1(p->getDefaultValue()), true);
}

void ToneEqPanel::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (compact)
        return;

    const auto node = nodeAt(event.position);

    if (node != Node::mid)
        return;

    if (auto* p = processor.getAPVTS().getParameter(pid::toneMidQ(band)))
    {
        const auto current = p->convertTo0to1(shownQ);
        p->beginChangeGesture();
        p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, current + wheel.deltaY * 0.5f));
        p->endChangeGesture();
    }
}

juce::String ToneEqPanel::getTooltip()
{
    const auto node = dragging != Node::count ? dragging : hovered;

    if (node == Node::count)
        return "Tone for this band. The curve fades outside the band's own frequency range.";

    const char* names[] = {"Low shelf", "Mid bell", "High shelf"};
    juce::String text = names[static_cast<size_t>(node)];

    text += ": drag for gain and frequency";

    if (node == Node::mid)
        text += ", Alt-drag or scroll for Q";

    return text + ", double-click to reset.";
}
} // namespace ember::gui
