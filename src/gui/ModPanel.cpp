#include "gui/ModPanel.h"

#include "dsp/modulation/ModSources.h"
#include "dsp/modulation/ModTypes.h"
#include "plugin/ParameterIDs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ember::gui
{
namespace
{
//==============================================================================
// Shared constants and small helpers.

/** Repaint / poll rate for every live element in the panel. */
constexpr int kRefreshHz = 30;

/** How long a header message stays up, in timer ticks (about 4 seconds). */
constexpr int kMessageTicks = kRefreshHz * 4;

/** Default amounts for a routing the user creates by dropping a source on a
    knob. A bipolar source sweeps both ways, so it gets half the span of a
    unipolar one and both end up moving the destination by the same total. */
constexpr float kDefaultBipolarAmount = 0.25f;
constexpr float kDefaultUnipolarAmount = 0.5f;
constexpr float kDefaultSmoothingMs = 5.0f;

/** Radio group for the two view keys in the header. */
constexpr int kViewRadioGroup = 0x4d4f44;

/** How far a shift-drag is slowed down, matching `ModulatableKnob`. */
constexpr float kFineDragFactor = 0.2f;

/** Points the value scope keeps: 192 frames at 30 Hz is about 6 seconds. */
constexpr int kScopeLength = 192;

juce::StringArray curveNames()
{
    return {"Linear", "Expo In", "Expo Out", "S-Curve", "Stepped"};
}

juce::String curveName(ModCurve curve)
{
    const auto names = curveNames();
    const int index = static_cast<int>(curve);

    return juce::isPositiveAndBelow(index, names.size()) ? names[index] : names[0];
}

ModCurve curveFromIndex(int index)
{
    return juce::isPositiveAndBelow(index, static_cast<int>(ModCurve::Count)) ? static_cast<ModCurve>(index)
                                                                              : ModCurve::Linear;
}

/** The order a vertical drag on a segment walks through, from "sags below the
    line" to "bulges above it". `Stepped` is deliberately not on this ramp — it
    is not a bend, and it lives in the segment's right-click menu instead. */
constexpr std::array<ModCurve, 4> kCurveRamp{{ModCurve::ExpoIn, ModCurve::SCurve, ModCurve::Linear, ModCurve::ExpoOut}};

int rampIndexOf(ModCurve curve)
{
    for (int i = 0; i < static_cast<int>(kCurveRamp.size()); ++i)
        if (kCurveRamp[static_cast<size_t>(i)] == curve)
            return i;

    return 2; // Linear
}

juce::String formatAmount(double amount)
{
    const int percent = juce::roundToInt(amount * 100.0);

    return (percent > 0 ? juce::String("+") : juce::String()) + juce::String(percent) + "%";
}

juce::String formatMilliseconds(double ms)
{
    if (ms < 1.0)
        return "0 ms";

    return (ms < 10.0 ? juce::String(ms, 1) : juce::String(juce::roundToInt(ms))) + " ms";
}

/** A source's live value as text: bipolar sources read -100..+100, unipolar
    ones 0..100. */
juce::String formatSourceValue(float value, bool bipolar)
{
    const int percent = juce::roundToInt(value * 100.0f);

    return (bipolar && percent > 0 ? juce::String("+") : juce::String()) + juce::String(percent);
}

/** The badge that follows the mouse while a source is being dragged. Built at
    twice the logical size and handed over as a ScaledImage, so it stays crisp
    on a retina display without any bitmap ever being stored. */
juce::Image createSourceDragImage(int flatIndex)
{
    constexpr float kImageScale = 2.0f;
    constexpr float kHeight = 26.0f;

    const auto text = modSourceDisplayName(flatIndex).toUpperCase();
    const auto font = EmberFonts::sized(12.0f, true);
    const float textWidth = juce::GlyphArrangement::getStringWidth(font, text);
    const float width = textWidth + 42.0f;

    juce::Image image(juce::Image::ARGB, juce::roundToInt(width * kImageScale), juce::roundToInt(kHeight * kImageScale),
                      true);

    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(kImageScale));

    const auto area = juce::Rectangle<float>(0.0f, 0.0f, width, kHeight).reduced(1.5f);

    g.setColour(EmberColours::backgroundDeep.withAlpha(0.95f));
    g.fillRoundedRectangle(area, 5.0f);
    g.setColour(EmberColours::accent);
    g.drawRoundedRectangle(area, 5.0f, 1.5f);

    // The same grip mark the tile shows, so the badge reads as "this is the
    // thing you picked up".
    const float dot = 1.7f;
    for (int column = 0; column < 2; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            const float x = area.getX() + 9.0f + static_cast<float>(column) * 5.0f;
            const float y = area.getCentreY() + (static_cast<float>(row) - 1.0f) * 5.0f;
            g.setColour(EmberColours::accent.withAlpha(0.85f));
            g.fillEllipse(x - dot, y - dot, dot * 2.0f, dot * 2.0f);
        }
    }

    g.setFont(font);
    g.setColour(EmberColours::textPrimary);
    g.drawText(text, area.withTrimmedLeft(24.0f), juce::Justification::centredLeft, false);

    return image;
}

/** Starts the modulation drag from any component. The payload is the shared
    `ModulationDrag` description, which is what every `ModulatableKnob`
    accepts. */
void startSourceDrag(juce::Component& sourceComponent, int flatIndex)
{
    if (!juce::isPositiveAndBelow(flatIndex, kNumModSources))
        return;

    auto* container = juce::DragAndDropContainer::findParentDragContainerFor(&sourceComponent);

    if (container == nullptr || container->isDragAndDropActive())
        return;

    const auto image = createSourceDragImage(flatIndex);
    const juce::Point<int> offset(-image.getWidth() / 4, -image.getHeight() / 2 - 10);

    container->startDragging(ModulationDrag::makeDescription(flatIndex), &sourceComponent,
                             juce::ScaledImage(image, 2.0), true, &offset, nullptr);
}

//==============================================================================
/**
    The grip you pick a modulation source up by.

    It is a component rather than a painted region so that the cursor, the
    tooltip and the hover state all come for free, and so a click on it can
    never be confused with a click on the tile behind it.
*/
class DragGrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DragGrip(int flatIndex = -1)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        setSourceIndex(flatIndex);
    }

    void setSourceIndex(int flatIndex)
    {
        sourceIndex = flatIndex;

        if (juce::isPositiveAndBelow(sourceIndex, kNumModSources))
            setTooltip("Drag " + modSourceDisplayName(sourceIndex) + " onto any knob to modulate it.");
    }

    /** Called on mouse-down, before the drag starts, so the owner can select
        the source the user is about to pick up. */
    std::function<void()> onPressed;

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        const float radius = juce::jlimit(0.9f, 1.6f, area.getWidth() * 0.11f);
        const float spacingX = radius * 3.2f;
        const float spacingY = radius * 3.2f;

        g.setColour(hovering ? EmberColours::accent : EmberColours::textDisabled);

        for (int column = 0; column < 2; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                const float x = area.getCentreX() + (static_cast<float>(column) - 0.5f) * spacingX;
                const float y = area.getCentreY() + (static_cast<float>(row) - 1.0f) * spacingY;
                g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
            }
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        hovering = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hovering = false;
        repaint();
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onPressed != nullptr)
            onPressed();
    }

    void mouseDrag(const juce::MouseEvent&) override { startSourceDrag(*this, sourceIndex); }

private:
    int sourceIndex{-1};
    bool hovering{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DragGrip)
};

//==============================================================================
/**
    One modulation source: its name, its live output and the grip you drag it
    by. Clicking anywhere else selects it, which is what reveals its own
    parameters in the pane next door.
*/
class SourceTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SourceTile(int flatIndex)
        : info(sourceInfoFromFlatIndex(flatIndex)), name(modSourceDisplayName(flatIndex)), grip(flatIndex)
    {
        grip.onPressed = [this]
        {
            if (onSelected != nullptr)
                onSelected();
        };

        addAndMakeVisible(grip);
        setTooltip(name + " - click to edit it, drag the grip onto a knob to route it.");
    }

    std::function<void()> onSelected;

    void setSelected(bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        repaint();
    }

    /** Stores a new live value, repainting only when it actually moved enough
        to be visible — 23 tiles repainting 30 times a second for nothing is
        exactly the kind of idle cost a plugin GUI must not have. */
    void setValue(float newValue)
    {
        if (std::abs(newValue - value) < 0.004f)
            return;

        value = newValue;
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        grip.setBounds(area.removeFromLeft(juce::jlimit(10, 18, area.getHeight() / 2)));
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onSelected != nullptr)
            onSelected();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(0.5f);
        const float corner = 3.5f;

        g.setColour(selected ? EmberColours::panelRaised : EmberColours::panel);
        g.fillRoundedRectangle(area, corner);

        g.setColour(selected ? EmberColours::accent : EmberColours::outline);
        g.drawRoundedRectangle(area.reduced(0.5f), corner, selected ? 1.4f : 1.0f);

        auto body = area.reduced(3.0f);
        body.removeFromLeft(static_cast<float>(grip.getWidth()));

        const float barHeight = juce::jlimit(3.0f, 6.0f, body.getHeight() * 0.22f);
        auto bar = body.removeFromBottom(barHeight);
        auto textRow = body;

        const auto font = EmberFonts::forHeight(textRow.getHeight(), 0.62f, selected);
        g.setFont(font);

        const auto readout = formatSourceValue(value, info.bipolar);
        const float readoutWidth = juce::GlyphArrangement::getStringWidth(font, "-100") + 4.0f;

        if (textRow.getWidth() > readoutWidth * 2.2f)
        {
            g.setColour(EmberColours::textDisabled);
            g.drawText(readout, textRow.removeFromRight(readoutWidth), juce::Justification::centredRight, false);
        }

        g.setColour(selected ? EmberColours::textPrimary : EmberColours::textSecondary);
        g.drawText(name, textRow, juce::Justification::centredLeft, false);

        // The value bar: bipolar sources grow out of the centre, unipolar ones
        // out of the left edge, so the polarity is readable without a legend.
        g.setColour(EmberColours::track);
        g.fillRoundedRectangle(bar, barHeight * 0.5f);

        const auto accent = selected ? EmberColours::accent : EmberColours::accent.withAlpha(0.62f);
        const float clamped = juce::jlimit(info.bipolar ? -1.0f : 0.0f, 1.0f, value);

        auto filled = bar;

        if (info.bipolar)
        {
            const float centre = bar.getCentreX();
            const float extent = clamped * bar.getWidth() * 0.5f;
            filled = juce::Rectangle<float>(juce::jmin(centre, centre + extent), bar.getY(), std::abs(extent),
                                            bar.getHeight());
        }
        else
        {
            filled = bar.withWidth(bar.getWidth() * clamped);
        }

        if (filled.getWidth() > 0.5f)
        {
            g.setColour(accent);
            g.fillRoundedRectangle(filled, juce::jmin(barHeight * 0.5f, filled.getWidth() * 0.5f));
        }

        if (info.bipolar)
        {
            g.setColour(EmberColours::outlineStrong);
            g.fillRect(juce::Rectangle<float>(bar.getCentreX() - 0.5f, bar.getY(), 1.0f, bar.getHeight()));
        }
    }

private:
    ModSourceInfo info;
    juce::String name;
    DragGrip grip;
    float value{0.0f};
    bool selected{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceTile)
};

//==============================================================================
/**
    A rolling history of a source's output.

    Envelopes, followers and MIDI sources have no shape to draw, but they are
    the ones you most want to see moving while you set an attack time. The
    scope is fed one frame per timer tick and keeps about six seconds.
*/
class ValueScope : public juce::Component
{
public:
    ValueScope() { history.fill(0.0f); }

    void setBipolar(bool shouldBeBipolar)
    {
        bipolar = shouldBeBipolar;
        repaint();
    }

    void setCaption(const juce::String& newCaption)
    {
        caption = newCaption;
        repaint();
    }

    void clearHistory()
    {
        history.fill(0.0f);
        head = 0;
        repaint();
    }

    void push(float value)
    {
        history[static_cast<size_t>(head)] = juce::jlimit(-1.0f, 1.0f, value);
        head = (head + 1) % kScopeLength;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        if (area.getWidth() < 12.0f || area.getHeight() < 12.0f)
            return;

        EmberLookAndFeel::drawWell(g, area, 4.0f);

        const auto plot = area.reduced(4.0f);
        const float zeroY = bipolar ? plot.getCentreY() : plot.getBottom();

        g.setColour(EmberColours::outline);
        g.drawHorizontalLine(juce::roundToInt(zeroY), plot.getX(), plot.getRight());

        juce::Path curve;
        juce::Path fill;

        for (int i = 0; i < kScopeLength; ++i)
        {
            const float sample = history[static_cast<size_t>((head + i) % kScopeLength)];
            const float x =
                plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(kScopeLength - 1);
            const float y = bipolar ? plot.getCentreY() - sample * plot.getHeight() * 0.5f
                                    : plot.getBottom() - juce::jmax(0.0f, sample) * plot.getHeight();

            if (i == 0)
            {
                curve.startNewSubPath(x, y);
                fill.startNewSubPath(x, zeroY);
                fill.lineTo(x, y);
            }
            else
            {
                curve.lineTo(x, y);
                fill.lineTo(x, y);
            }
        }

        fill.lineTo(plot.getRight(), zeroY);
        fill.closeSubPath();

        g.setColour(EmberColours::accent.withAlpha(0.14f));
        g.fillPath(fill);

        g.setColour(EmberColours::accent);
        g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto labelRow = area.reduced(6.0f, 3.0f).removeFromTop(juce::jmin(14.0f, area.getHeight() * 0.3f));
        const auto font = EmberFonts::forHeight(labelRow.getHeight(), 0.8f, false);
        g.setFont(font);

        if (caption.isNotEmpty())
        {
            g.setColour(EmberColours::textDisabled);
            g.drawText(caption, labelRow, juce::Justification::topLeft, false);
        }

        const float latest = history[static_cast<size_t>((head + kScopeLength - 1) % kScopeLength)];
        g.setColour(EmberColours::textSecondary);
        g.drawText(formatSourceValue(latest, bipolar), labelRow, juce::Justification::topRight, false);
    }

private:
    std::array<float, static_cast<size_t>(kScopeLength)> history{};
    int head{0};
    bool bipolar{false};
    juce::String caption;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueScope)
};

//==============================================================================
/**
    The XLFO shape editor.

        click empty space      add a breakpoint there and drag it
        drag a breakpoint      move it (shift for fine control)
        drag a segment         bend it: expo in / s-curve / linear / expo out
        double-click a point   remove it
        right-click a point    remove it
        right-click a segment  pick any curve, including Stepped
        right-click the field  shape presets

    The editor owns a local copy of the points; every edit is pushed straight
    to `ModulationEngine::setXLfoShape`, which sorts and clamps them, and the
    curve is then drawn by asking the engine's own `evaluateShapeAt`. Drawing
    what the DSP actually plays — rather than a second interpretation of the
    same points — is the only way the picture can never lie.
*/
class ShapeEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ShapeEditor(EmberAudioProcessor& processorToUse) : processor(processorToUse)
    {
        setTooltip("Click to add a point, drag to move it, drag between points to bend the segment. "
                   "Double-click or right-click a point to remove it.");
    }

    std::function<void(const juce::String&, bool)> onMessage;

    void setLfoIndex(int newIndex)
    {
        lfoIndex = juce::jlimit(0, kNumXLFOs - 1, newIndex);
        stepsParameter = processor.getAPVTS().getRawParameterValue(pid::lfoSteps(lfoIndex));
        depthParameter = processor.getAPVTS().getRawParameterValue(pid::lfoDepth(lfoIndex));
        refreshFromEngine();
    }

    void refreshFromEngine()
    {
        const auto& lfo = processor.getModulationEngine().getXLfo(lfoIndex);
        const int count = lfo.getNumShapePoints();

        points.clear();
        points.reserve(static_cast<size_t>(juce::jmax(1, count)));

        for (int i = 0; i < count; ++i)
            points.push_back(lfo.getShapePoint(i));

        hoverPoint = -1;
        hoverSegment = -1;
        dragPoint = -1;
        dragSegment = -1;
        repaint();
    }

    /** Called from the panel's timer: the live-value marker moves, and a shape
        that changed underneath the editor (a preset load, an undo) is picked
        up rather than being drawn from a stale copy. */
    void tick()
    {
        if (dragPoint < 0 && dragSegment < 0 && shapeChangedExternally())
        {
            refreshFromEngine();
            return;
        }

        const float value = processor.getModulationEngine().getSourceValue(lfoIndex);

        if (std::abs(value - liveValue) < 0.004f)
            return;

        liveValue = value;
        repaint();
    }

    /** Replaces the shape with one of the built-in starting points. */
    void applyPreset(int presetIndex)
    {
        points.clear();

        const auto add = [this](float position, float value, ModCurve curve)
        {
            LfoPoint point{};
            point.position = position;
            point.value = value;
            point.curve = curve;
            points.push_back(point);
        };

        switch (presetIndex)
        {
        case 0: // sine
            add(0.0f, 1.0f, ModCurve::SCurve);
            add(0.5f, -1.0f, ModCurve::SCurve);
            break;

        case 1: // triangle
            add(0.0f, -1.0f, ModCurve::Linear);
            add(0.5f, 1.0f, ModCurve::Linear);
            break;

        case 2: // ramp
            add(0.0f, -1.0f, ModCurve::Linear);
            add(0.97f, 1.0f, ModCurve::Linear);
            break;

        case 3: // square
            add(0.0f, 1.0f, ModCurve::Linear);
            add(0.48f, 1.0f, ModCurve::Linear);
            add(0.5f, -1.0f, ModCurve::Linear);
            add(0.98f, -1.0f, ModCurve::Linear);
            break;

        default: // random steps
        {
            juce::Random random;

            for (int i = 0; i < 8; ++i)
            {
                const float position = static_cast<float>(i) / 8.0f;
                add(position, random.nextFloat() * 2.0f - 1.0f, ModCurve::Linear);
                add(position + 0.12f, random.nextFloat() * 2.0f - 1.0f, ModCurve::Linear);
            }

            break;
        }
        }

        pushShape();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        if (area.getWidth() < 20.0f || area.getHeight() < 20.0f)
            return;

        EmberLookAndFeel::drawWell(g, area, 4.0f);

        const auto plot = plotArea();

        // ---- the range the depth parameter actually lets through
        const float depth = depthParameter != nullptr ? juce::jlimit(0.0f, 1.0f, depthParameter->load() * 0.01f) : 1.0f;

        if (depth < 0.995f)
        {
            const float half = plot.getHeight() * 0.5f * depth;
            g.setColour(EmberColours::panelRaised.withAlpha(0.45f));
            g.fillRect(juce::Rectangle<float>(plot.getX(), plot.getCentreY() - half, plot.getWidth(), half * 2.0f));
        }

        // ---- grid
        const int steps = stepsParameter != nullptr ? juce::roundToInt(stepsParameter->load()) : 0;
        const int divisions = steps >= 2 ? juce::jmin(steps, 32) : 4;

        g.setColour(EmberColours::outline.withAlpha(steps >= 2 ? 0.9f : 0.55f));

        for (int i = 1; i < divisions; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(divisions);
            g.fillRect(juce::Rectangle<float>(x, plot.getY(), 1.0f, plot.getHeight()));
        }

        for (int i = 1; i < 4; ++i)
        {
            const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4.0f;
            g.setColour(i == 2 ? EmberColours::outlineStrong : EmberColours::outline.withAlpha(0.5f));
            g.fillRect(juce::Rectangle<float>(plot.getX(), y, plot.getWidth(), 1.0f));
        }

        // ---- the curve, sampled from the engine's own evaluator
        const auto& lfo = processor.getModulationEngine().getXLfo(lfoIndex);
        const int samples = juce::jlimit(32, 512, juce::roundToInt(plot.getWidth()));

        juce::Path curve;
        juce::Path fill;

        for (int i = 0; i < samples; ++i)
        {
            const float phase = static_cast<float>(i) / static_cast<float>(samples - 1);
            const float value = lfo.evaluateShapeAt(phase);
            const auto point = toScreen(phase, value);

            if (i == 0)
            {
                curve.startNewSubPath(point);
                fill.startNewSubPath(point.x, plot.getCentreY());
                fill.lineTo(point);
            }
            else
            {
                curve.lineTo(point);
                fill.lineTo(point);
            }
        }

        fill.lineTo(plot.getRight(), plot.getCentreY());
        fill.closeSubPath();

        g.setColour(EmberColours::accent.withAlpha(0.13f));
        g.fillPath(fill);

        g.setColour(EmberColours::accent);
        g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ---- live output, as a line across the field
        const float liveY = plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, liveValue) * plot.getHeight() * 0.5f;
        g.setColour(EmberColours::accentGlow.withAlpha(0.5f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), liveY - 0.5f, plot.getWidth(), 1.0f));
        g.setColour(EmberColours::accent);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({plot.getRight(), liveY}));

        // ---- breakpoints
        for (int i = 0; i < static_cast<int>(points.size()); ++i)
        {
            const auto& point = points[static_cast<size_t>(i)];
            const auto centre = toScreen(point.position, point.value);
            const bool active = (i == dragPoint) || (i == hoverPoint);
            const float radius = active ? 5.4f : 4.0f;

            g.setColour(active ? EmberColours::accent : EmberColours::panelRaised);
            g.fillEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre));
            g.setColour(active ? EmberColours::textPrimary : EmberColours::accent);
            g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre), 1.3f);
        }

        // ---- the segment under the cursor names its own curve
        const int segment = dragSegment >= 0 ? dragSegment : hoverSegment;

        if (segment >= 0 && segment < static_cast<int>(points.size()))
        {
            const auto& point = points[static_cast<size_t>(segment)];
            const auto label = curveName(point.curve).toUpperCase();
            const auto font = EmberFonts::sized(juce::jlimit(9.0f, 12.0f, plot.getHeight() * 0.13f), true);
            const float width = juce::GlyphArrangement::getStringWidth(font, label) + 10.0f;
            const auto box = juce::Rectangle<float>(width, font.getHeight() + 5.0f)
                                 .withCentre({plot.getCentreX(), plot.getY() + font.getHeight() * 0.9f});

            g.setColour(EmberColours::backgroundDeep.withAlpha(0.88f));
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(EmberColours::accent);
            g.setFont(font);
            g.drawText(label, box, juce::Justification::centred, false);
        }

        if (points.size() >= static_cast<size_t>(kMaxLfoPoints))
        {
            const auto font = EmberFonts::sized(10.0f, false);
            g.setFont(font);
            g.setColour(EmberColours::textDisabled);
            g.drawText("point limit reached", plot.reduced(4.0f), juce::Justification::bottomRight, false);
        }
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        const int point = hitTestPoint(event.position);
        const int segment = point >= 0 ? -1 : hitTestSegment(event.position);

        if (point != hoverPoint || segment != hoverSegment)
        {
            hoverPoint = point;
            hoverSegment = segment;
            setMouseCursor(point >= 0 ? juce::MouseCursor::DraggingHandCursor
                                      : (segment >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                                      : juce::MouseCursor::CrosshairCursor));
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hoverPoint = -1;
        hoverSegment = -1;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const int point = hitTestPoint(event.position);

        if (event.mods.isPopupMenu())
        {
            showContextMenu(point, point >= 0 ? -1 : hitTestSegment(event.position));
            return;
        }

        dragStartPosition = event.position;

        if (point >= 0)
        {
            dragPoint = point;
            dragSegment = -1;
            dragStartValue = points[static_cast<size_t>(point)];
            repaint();
            return;
        }

        const int segment = hitTestSegment(event.position);

        if (segment >= 0)
        {
            dragSegment = segment;
            dragPoint = -1;
            dragStartRamp = rampIndexOf(points[static_cast<size_t>(segment)].curve);
            repaint();
            return;
        }

        addPointAt(event.position);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragPoint >= 0 && dragPoint < static_cast<int>(points.size()))
        {
            const auto plot = plotArea();
            const float scale = event.mods.isShiftDown() ? kFineDragFactor : 1.0f;
            const auto delta = (event.position - dragStartPosition) * scale;

            const float position =
                juce::jlimit(0.0f, 1.0f, dragStartValue.position + delta.x / juce::jmax(1.0f, plot.getWidth()));
            const float value =
                juce::jlimit(-1.0f, 1.0f, dragStartValue.value - delta.y / juce::jmax(1.0f, plot.getHeight() * 0.5f));

            auto& point = points[static_cast<size_t>(dragPoint)];
            point.position = position;
            point.value = value;

            reorderAroundDraggedPoint();
            pushShape();
            return;
        }

        if (dragSegment >= 0 && dragSegment < static_cast<int>(points.size()))
        {
            const float travel = (dragStartPosition.y - event.position.y) / 18.0f;
            const int index =
                juce::jlimit(0, static_cast<int>(kCurveRamp.size()) - 1, dragStartRamp + juce::roundToInt(travel));
            const auto curve = kCurveRamp[static_cast<size_t>(index)];

            if (points[static_cast<size_t>(dragSegment)].curve != curve)
            {
                points[static_cast<size_t>(dragSegment)].curve = curve;
                pushShape();
            }
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        dragPoint = -1;
        dragSegment = -1;
        mouseMove(event);
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override { removePoint(hitTestPoint(event.position)); }

private:
    /** True when the engine's shape is no longer the one this editor last
        pushed — which only happens when something else wrote it. */
    bool shapeChangedExternally() const
    {
        const auto& lfo = processor.getModulationEngine().getXLfo(lfoIndex);

        if (lfo.getNumShapePoints() != static_cast<int>(points.size()))
            return true;

        for (int i = 0; i < static_cast<int>(points.size()); ++i)
        {
            const auto enginePoint = lfo.getShapePoint(i);
            const auto& localPoint = points[static_cast<size_t>(i)];

            if (std::abs(enginePoint.position - localPoint.position) > 1.0e-5f ||
                std::abs(enginePoint.value - localPoint.value) > 1.0e-5f || enginePoint.curve != localPoint.curve)
                return true;
        }

        return false;
    }

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(8.0f, 9.0f); }

    juce::Point<float> toScreen(float position, float value) const
    {
        const auto plot = plotArea();

        return {plot.getX() + plot.getWidth() * juce::jlimit(0.0f, 1.0f, position),
                plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, value) * plot.getHeight() * 0.5f};
    }

    int hitTestPoint(juce::Point<float> position) const
    {
        int best = -1;
        float bestDistance = 9.0f;

        for (int i = 0; i < static_cast<int>(points.size()); ++i)
        {
            const auto& point = points[static_cast<size_t>(i)];
            const float distance = toScreen(point.position, point.value).getDistanceFrom(position);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return best;
    }

    /** Which segment the cursor is over, or -1 when it is nowhere near the
        curve. A segment is identified by the point it STARTS at, which is the
        same convention `LfoPoint::curve` uses. */
    int hitTestSegment(juce::Point<float> position) const
    {
        if (points.size() < 2)
            return -1;

        const auto plot = plotArea();

        if (!plot.expanded(6.0f).contains(position))
            return -1;

        const float phase = juce::jlimit(0.0f, 1.0f, (position.x - plot.getX()) / juce::jmax(1.0f, plot.getWidth()));
        const float value = processor.getModulationEngine().getXLfo(lfoIndex).evaluateShapeAt(phase);
        const float curveY = plot.getCentreY() - value * plot.getHeight() * 0.5f;

        if (std::abs(curveY - position.y) > 10.0f)
            return -1;

        // The points are sorted, so the owning segment is the last point at or
        // before the phase; before the first point we are in the wrap segment.
        if (phase < points.front().position)
            return static_cast<int>(points.size()) - 1;

        int segment = 0;

        for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i)
        {
            if (points[static_cast<size_t>(i)].position <= phase)
            {
                segment = i;
                break;
            }
        }

        return segment;
    }

    void addPointAt(juce::Point<float> position)
    {
        if (points.size() >= static_cast<size_t>(kMaxLfoPoints))
        {
            if (onMessage != nullptr)
                onMessage("Shape is full: " + juce::String(kMaxLfoPoints) + " points is the maximum.", true);

            return;
        }

        const auto plot = plotArea();

        LfoPoint point{};
        point.position = juce::jlimit(0.0f, 1.0f, (position.x - plot.getX()) / juce::jmax(1.0f, plot.getWidth()));
        point.value =
            juce::jlimit(-1.0f, 1.0f, (plot.getCentreY() - position.y) / juce::jmax(1.0f, plot.getHeight() * 0.5f));
        point.curve = ModCurve::Linear;

        points.push_back(point);
        std::stable_sort(points.begin(), points.end(),
                         [](const LfoPoint& a, const LfoPoint& b) { return a.position < b.position; });

        for (int i = 0; i < static_cast<int>(points.size()); ++i)
        {
            if (std::abs(points[static_cast<size_t>(i)].position - point.position) < 1.0e-6f &&
                std::abs(points[static_cast<size_t>(i)].value - point.value) < 1.0e-6f)
            {
                dragPoint = i;
                break;
            }
        }

        dragStartPosition = position;
        dragStartValue = point;
        pushShape();
    }

    void removePoint(int index)
    {
        if (!juce::isPositiveAndBelow(index, static_cast<int>(points.size())))
            return;

        if (points.size() <= 2)
        {
            if (onMessage != nullptr)
                onMessage("A shape needs at least two points.", true);

            return;
        }

        points.erase(points.begin() + index);
        hoverPoint = -1;
        dragPoint = -1;
        pushShape();
    }

    /** Keeps the points sorted while one of them is being dragged past its
        neighbours, and keeps `dragPoint` pointing at the SAME point rather
        than at whatever slid into its index. */
    void reorderAroundDraggedPoint()
    {
        if (!juce::isPositiveAndBelow(dragPoint, static_cast<int>(points.size())))
            return;

        const auto moving = points[static_cast<size_t>(dragPoint)];
        points.erase(points.begin() + dragPoint);

        const auto destination =
            std::upper_bound(points.begin(), points.end(), moving,
                             [](const LfoPoint& a, const LfoPoint& b) { return a.position < b.position; });

        dragPoint = static_cast<int>(std::distance(points.begin(), destination));
        points.insert(destination, moving);
    }

    void pushShape()
    {
        processor.getModulationEngine().setXLfoShape(lfoIndex, points.data(), static_cast<int>(points.size()));
        repaint();
    }

    void showContextMenu(int point, int segment)
    {
        juce::PopupMenu menu;

        if (point >= 0)
        {
            menu.addSectionHeader("Point " + juce::String(point + 1));
            menu.addItem(1, "Remove point", points.size() > 2, false);
        }
        else if (segment >= 0)
        {
            menu.addSectionHeader("Segment " + juce::String(segment + 1));

            const auto names = curveNames();

            for (int i = 0; i < names.size(); ++i)
                menu.addItem(100 + i, names[i], true, points[static_cast<size_t>(segment)].curve == curveFromIndex(i));
        }
        else
        {
            menu.addSectionHeader("Shape");
        }

        menu.addSeparator();
        menu.addItem(200, "Preset: Sine");
        menu.addItem(201, "Preset: Triangle");
        menu.addItem(202, "Preset: Ramp");
        menu.addItem(203, "Preset: Square");
        menu.addItem(204, "Preset: Random");

        juce::Component::SafePointer<ShapeEditor> safeThis(this);

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safeThis, point, segment](int result)
                           {
                               if (safeThis == nullptr || result == 0)
                                   return;

                               if (result == 1)
                                   safeThis->removePoint(point);
                               else if (result >= 100 && result < 200)
                                   safeThis->setSegmentCurve(segment, curveFromIndex(result - 100));
                               else if (result >= 200)
                                   safeThis->applyPreset(result - 200);
                           });
    }

    void setSegmentCurve(int segment, ModCurve curve)
    {
        if (!juce::isPositiveAndBelow(segment, static_cast<int>(points.size())))
            return;

        points[static_cast<size_t>(segment)].curve = curve;
        pushShape();
    }

    EmberAudioProcessor& processor;
    std::vector<LfoPoint> points;
    int lfoIndex{0};

    std::atomic<float>* stepsParameter{nullptr};
    std::atomic<float>* depthParameter{nullptr};

    float liveValue{0.0f};

    int hoverPoint{-1};
    int hoverSegment{-1};
    int dragPoint{-1};
    int dragSegment{-1};
    int dragStartRamp{0};
    LfoPoint dragStartValue{};
    juce::Point<float> dragStartPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShapeEditor)
};

//==============================================================================
/**
    The XY controller's pad. Both axes are ordinary APVTS parameters, so the
    pad is just a two-dimensional view of them: the host, automation, undo and
    modulation of the axes themselves all keep working.
*/
class XyPad : public juce::Component, public juce::SettableTooltipClient
{
public:
    XyPad(juce::AudioProcessorValueTreeState& state, juce::UndoManager* undoManager)
    {
        xParameter = state.getParameter(pid::xyX);
        yParameter = state.getParameter(pid::xyY);

        if (xParameter != nullptr)
        {
            xAttachment = std::make_unique<juce::ParameterAttachment>(
                *xParameter,
                [this](float value)
                {
                    xValue = xParameter->convertTo0to1(value);
                    repaint();
                },
                undoManager);
            xAttachment->sendInitialUpdate();
        }

        if (yParameter != nullptr)
        {
            yAttachment = std::make_unique<juce::ParameterAttachment>(
                *yParameter,
                [this](float value)
                {
                    yValue = yParameter->convertTo0to1(value);
                    repaint();
                },
                undoManager);
            yAttachment->sendInitialUpdate();
        }

        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        setTooltip("XY controller. Drag the puck; shift-drag for fine control, double-click to centre it.");
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        if (area.getWidth() < 16.0f || area.getHeight() < 16.0f)
            return;

        EmberLookAndFeel::drawWell(g, area, 4.0f);

        const auto plot = area.reduced(6.0f);

        g.setColour(EmberColours::outline.withAlpha(0.6f));

        for (int i = 1; i < 4; ++i)
        {
            const float proportion = static_cast<float>(i) / 4.0f;
            g.fillRect(juce::Rectangle<float>(plot.getX() + plot.getWidth() * proportion, plot.getY(), 1.0f,
                                              plot.getHeight()));
            g.fillRect(juce::Rectangle<float>(plot.getX(), plot.getY() + plot.getHeight() * proportion, plot.getWidth(),
                                              1.0f));
        }

        const auto puck = puckCentre(plot);

        g.setColour(EmberColours::accent.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(puck.x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));
        g.fillRect(juce::Rectangle<float>(plot.getX(), puck.y - 0.5f, plot.getWidth(), 1.0f));

        const float radius = juce::jlimit(4.0f, 9.0f, plot.getWidth() * 0.045f);

        g.setColour(EmberColours::accentGlow.withAlpha(0.35f));
        g.fillEllipse(juce::Rectangle<float>(radius * 3.4f, radius * 3.4f).withCentre(puck));
        g.setColour(EmberColours::accent);
        g.fillEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(puck));
        g.setColour(EmberColours::backgroundDeep);
        g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(puck), 1.2f);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragStartPosition = event.position;
        dragStartX = xValue;
        dragStartY = yValue;

        if (xAttachment != nullptr)
            xAttachment->beginGesture();

        if (yAttachment != nullptr)
            yAttachment->beginGesture();

        // A plain click jumps the puck; the drag then continues from there.
        applyNormalised(normalisedFor(event.position));
        dragStartX = xValue;
        dragStartY = yValue;
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const auto plot = getLocalBounds().toFloat().reduced(6.0f);
        const float scale = event.mods.isShiftDown() ? kFineDragFactor : 1.0f;
        const auto delta = (event.position - dragStartPosition) * scale;

        applyNormalised({juce::jlimit(0.0f, 1.0f, dragStartX + delta.x / juce::jmax(1.0f, plot.getWidth())),
                         juce::jlimit(0.0f, 1.0f, dragStartY - delta.y / juce::jmax(1.0f, plot.getHeight()))});
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (xAttachment != nullptr)
            xAttachment->endGesture();

        if (yAttachment != nullptr)
            yAttachment->endGesture();
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (xParameter != nullptr && xAttachment != nullptr)
            xAttachment->setValueAsCompleteGesture(xParameter->convertFrom0to1(xParameter->getDefaultValue()));

        if (yParameter != nullptr && yAttachment != nullptr)
            yAttachment->setValueAsCompleteGesture(yParameter->convertFrom0to1(yParameter->getDefaultValue()));
    }

private:
    juce::Point<float> puckCentre(juce::Rectangle<float> plot) const
    {
        return {plot.getX() + plot.getWidth() * xValue, plot.getBottom() - plot.getHeight() * yValue};
    }

    juce::Point<float> normalisedFor(juce::Point<float> position) const
    {
        const auto plot = getLocalBounds().toFloat().reduced(6.0f);

        return {juce::jlimit(0.0f, 1.0f, (position.x - plot.getX()) / juce::jmax(1.0f, plot.getWidth())),
                juce::jlimit(0.0f, 1.0f, (plot.getBottom() - position.y) / juce::jmax(1.0f, plot.getHeight()))};
    }

    void applyNormalised(juce::Point<float> normalised)
    {
        if (xParameter != nullptr && xAttachment != nullptr)
            xAttachment->setValueAsPartOfGesture(xParameter->convertFrom0to1(normalised.x));

        if (yParameter != nullptr && yAttachment != nullptr)
            yAttachment->setValueAsPartOfGesture(yParameter->convertFrom0to1(normalised.y));
    }

    juce::RangedAudioParameter* xParameter{nullptr};
    juce::RangedAudioParameter* yParameter{nullptr};
    std::unique_ptr<juce::ParameterAttachment> xAttachment, yAttachment;

    float xValue{0.5f}, yValue{0.5f};
    float dragStartX{0.5f}, dragStartY{0.5f};
    juce::Point<float> dragStartPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(XyPad)
};

//==============================================================================
/**
    A horizontal value bar for the matrix.

    Matrix amounts and smoothing times are engine state, not APVTS parameters,
    so they cannot use `ModulatableKnob` — but they still owe the user the same
    contract: double-click resets, shift-drag is fine, the wheel works and a
    tooltip explains it.
*/
class BarSlider : public juce::Slider
{
public:
    BarSlider(bool isBipolar, double defaultValue) : bipolar(isBipolar)
    {
        setSliderStyle(juce::Slider::LinearHorizontal);
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        setSliderSnapsToMousePosition(false);
        setDoubleClickReturnValue(true, defaultValue);
        setPopupDisplayEnabled(false, false, nullptr);
    }

    /** How the value is written across the bar. */
    std::function<juce::String(double)> formatValue;

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(0.5f);

        if (area.getWidth() < 8.0f || area.getHeight() < 6.0f)
            return;

        const float corner = juce::jmin(3.0f, area.getHeight() * 0.3f);

        EmberLookAndFeel::drawWell(g, area, corner);

        const auto range = getNormalisableRange();
        const float proportion = static_cast<float>(range.convertTo0to1(getValue()));
        const auto accent = EmberStyleProps::accentColourFor(*this);

        auto filled = area.reduced(1.5f);

        if (bipolar)
        {
            const float centre = filled.getCentreX();
            const float extent = (proportion - 0.5f) * filled.getWidth();
            filled = juce::Rectangle<float>(juce::jmin(centre, centre + extent), filled.getY(), std::abs(extent),
                                            filled.getHeight());
        }
        else
        {
            filled = filled.withWidth(filled.getWidth() * proportion);
        }

        if (filled.getWidth() > 0.5f)
        {
            g.setColour(accent.withAlpha(isMouseOverOrDragging() ? 0.85f : 0.62f));
            g.fillRoundedRectangle(filled, juce::jmin(corner, filled.getWidth() * 0.5f));
        }

        if (bipolar)
        {
            g.setColour(EmberColours::outlineStrong);
            g.fillRect(
                juce::Rectangle<float>(area.getCentreX() - 0.5f, area.getY() + 1.5f, 1.0f, area.getHeight() - 3.0f));
        }

        const auto text = formatValue != nullptr ? formatValue(getValue()) : juce::String(getValue(), 2);

        g.setFont(EmberFonts::forHeight(area.getHeight(), 0.58f, false));
        g.setColour(isMouseOverOrDragging() ? EmberColours::textPrimary : EmberColours::textSecondary);
        g.drawText(text, area, juce::Justification::centred, false);

        if (isMouseOverOrDragging())
        {
            g.setColour(accent.withAlpha(0.6f));
            g.drawRoundedRectangle(area, corner, 1.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        virtualPosition = event.position;
        lastRawPosition = event.position;
        juce::Slider::mouseDown(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const float scale = event.mods.isShiftDown() ? kFineDragFactor : 1.0f;
        virtualPosition += (event.position - lastRawPosition) * scale;
        lastRawPosition = event.position;
        juce::Slider::mouseDrag(event.withNewPosition(virtualPosition));
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (event.mods.isShiftDown())
        {
            auto fine = wheel;
            fine.deltaX *= kFineDragFactor;
            fine.deltaY *= kFineDragFactor;
            juce::Slider::mouseWheelMove(event, fine);
            return;
        }

        juce::Slider::mouseWheelMove(event, wheel);
    }

private:
    bool bipolar;
    juce::Point<float> virtualPosition, lastRawPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BarSlider)
};

//==============================================================================
/** The destination cell of a matrix row: a name you can click to re-target. */
class DestinationButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    DestinationButton() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

    std::function<void()> onClick;

    void setText(const juce::String& newText)
    {
        text = newText;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds();

        if (area.getHeight() < 6)
            return;

        const auto chevron = area.removeFromRight(juce::jmin(12, area.getWidth() / 5));

        g.setFont(EmberFonts::forHeight(static_cast<float>(area.getHeight()), 0.56f, false));
        g.setColour(hovering ? EmberColours::textPrimary : EmberColours::textSecondary);
        g.drawFittedText(text, area, juce::Justification::centredLeft, 1, 0.75f);

        if (hovering)
        {
            const auto underline = getLocalBounds().toFloat().removeFromBottom(1.0f).reduced(1.0f, 0.0f);
            g.setColour(EmberColours::accent.withAlpha(0.7f));
            g.fillRect(underline);

            juce::Path arrow;
            const auto box = chevron.toFloat().reduced(3.0f, 0.0f).withSizeKeepingCentre(6.0f, 4.0f);
            arrow.startNewSubPath(box.getX(), box.getY());
            arrow.lineTo(box.getCentreX(), box.getBottom());
            arrow.lineTo(box.getRight(), box.getY());

            g.setColour(EmberColours::accent);
            g.strokePath(arrow, juce::PathStrokeType(1.2f));
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        hovering = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hovering = false;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (onClick != nullptr && getLocalBounds().contains(event.getPosition()))
            onClick();
    }

private:
    juce::String text;
    bool hovering{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DestinationButton)
};

//==============================================================================
/** Column geometry for the matrix, shared by the header strip and every row so
    the two can never drift apart. */
struct MatrixColumns
{
    juce::Rectangle<int> source, arrow, destination, amount, curve, smoothing, enabled, remove;
};

int matrixMinimumWidth(float scale)
{
    const auto px = [scale](float value) { return juce::roundToInt(value * scale); };

    return px(96.0f) + px(16.0f) + px(120.0f) + px(104.0f) + px(78.0f) + px(76.0f) + px(26.0f) + px(24.0f) +
           px(6.0f) * 7;
}

MatrixColumns matrixColumns(juce::Rectangle<int> row, float scale)
{
    const auto px = [scale](float value) { return juce::roundToInt(value * scale); };
    const int gap = px(6.0f);

    MatrixColumns columns;
    columns.source = row.removeFromLeft(px(96.0f));
    row.removeFromLeft(gap);
    columns.arrow = row.removeFromLeft(px(16.0f));
    row.removeFromLeft(gap);
    columns.remove = row.removeFromRight(px(24.0f));
    row.removeFromRight(gap);
    columns.enabled = row.removeFromRight(px(26.0f));
    row.removeFromRight(gap);
    columns.smoothing = row.removeFromRight(px(76.0f));
    row.removeFromRight(gap);
    columns.curve = row.removeFromRight(px(78.0f));
    row.removeFromRight(gap);
    columns.amount = row.removeFromRight(px(104.0f));
    row.removeFromRight(gap);
    columns.destination = row;

    return columns;
}
} // namespace

//==============================================================================
//==============================================================================
/**
    The always-visible header: the panel's name, the two view keys, how many
    routings exist, and the one place a refused edit is reported.
*/
class ModPanel::HeaderStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit HeaderStrip(ModPanel& ownerToUse) : owner(ownerToUse)
    {
        const auto configure = [this](EmberButton& button, View view, const juce::String& text)
        {
            button.setButtonText(text);
            button.setEmberStyle(EmberButton::Style::ghost);
            button.setClickingTogglesState(true);
            button.setRadioGroupId(kViewRadioGroup);
            button.onClick = [this, view]
            {
                owner.setView(view);
                owner.setCollapsed(false);
            };
            addAndMakeVisible(button);
        };

        configure(sourcesButton, View::sources, "Sources");
        configure(matrixButton, View::matrix, "Matrix");

        sourcesButton.setToggleState(true, juce::dontSendNotification);

        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setTooltip("Modulation. Click to collapse or expand.");
    }

    void setViewState(View view)
    {
        sourcesButton.setToggleState(view == View::sources, juce::dontSendNotification);
        matrixButton.setToggleState(view == View::matrix, juce::dontSendNotification);
    }

    void setRoutingCount(int count)
    {
        if (routingCount == count)
            return;

        routingCount = count;
        repaint();
    }

    void showMessage(const juce::String& text, bool isWarning)
    {
        message = text;
        messageIsWarning = isWarning;
        messageTicks = kMessageTicks;
        repaint();
    }

    /** Fades the message out; returns true when something needs repainting. */
    bool tick()
    {
        if (messageTicks <= 0)
            return false;

        --messageTicks;

        if (messageTicks == 0)
            message.clear();

        repaint();
        return true;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(juce::roundToInt(6.0f * owner.uiScale()), 3);
        const int buttonWidth = juce::jlimit(46, 84, juce::roundToInt(62.0f * owner.uiScale()));

        if (area.getWidth() > buttonWidth * 2 + 60)
        {
            matrixButton.setVisible(true);
            sourcesButton.setVisible(true);
            matrixButton.setBounds(area.removeFromRight(buttonWidth).reduced(1));
            area.removeFromRight(3);
            sourcesButton.setBounds(area.removeFromRight(buttonWidth).reduced(1));
        }
        else
        {
            matrixButton.setVisible(false);
            sourcesButton.setVisible(false);
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour(EmberColours::panel);
        g.fillRect(area);

        EmberLookAndFeel::drawHairline(g, area.removeFromBottom(1.0f), EmberColours::outlineStrong, false);

        auto content = getLocalBounds().reduced(juce::roundToInt(6.0f * owner.uiScale()), 0);
        const float height = static_cast<float>(content.getHeight());

        // ---- the collapse chevron
        const auto chevronArea = content.removeFromLeft(juce::roundToInt(height * 0.7f)).toFloat();
        const float size = juce::jlimit(4.0f, 7.0f, height * 0.2f);
        const auto centre = chevronArea.getCentre();

        juce::Path chevron;

        if (owner.isCollapsed())
        {
            chevron.startNewSubPath(centre.x - size * 0.5f, centre.y - size);
            chevron.lineTo(centre.x + size * 0.7f, centre.y);
            chevron.lineTo(centre.x - size * 0.5f, centre.y + size);
        }
        else
        {
            chevron.startNewSubPath(centre.x - size, centre.y - size * 0.5f);
            chevron.lineTo(centre.x, centre.y + size * 0.7f);
            chevron.lineTo(centre.x + size, centre.y - size * 0.5f);
        }

        g.setColour(EmberColours::accent);
        g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        content.removeFromLeft(4);

        // ---- title
        const auto titleFont = EmberFonts::forHeight(height, 0.44f, true);
        const juce::String title("MODULATION");
        const int titleWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(titleFont, title)) + 8;

        g.setFont(titleFont);
        g.setColour(EmberColours::textPrimary);
        g.drawText(title, content.removeFromLeft(juce::jmin(titleWidth, content.getWidth())),
                   juce::Justification::centredLeft, false);

        // ---- routing count, kept next to the title
        const auto countFont = EmberFonts::forHeight(height, 0.4f, false);
        const auto countText =
            juce::String(routingCount) + (routingCount == 1 ? juce::String(" routing") : juce::String(" routings"));
        const int countWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(countFont, countText)) + 10;

        if (content.getWidth() > countWidth)
        {
            g.setFont(countFont);
            g.setColour(EmberColours::textDisabled);
            g.drawText(countText, content.removeFromLeft(countWidth), juce::Justification::centredLeft, false);
        }

        // ---- the buttons live on the right; anything left over shows messages
        if (sourcesButton.isVisible())
            content.setRight(juce::jmin(content.getRight(), sourcesButton.getX() - 6));

        if (message.isNotEmpty() && content.getWidth() > 40)
        {
            const float alpha = juce::jlimit(0.0f, 1.0f, static_cast<float>(messageTicks) / 12.0f);
            g.setFont(EmberFonts::forHeight(height, 0.42f, false));
            g.setColour((messageIsWarning ? EmberColours::warning : EmberColours::accent).withAlpha(alpha));
            g.drawFittedText(message, content, juce::Justification::centredRight, 1, 0.8f);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (getLocalBounds().contains(event.getPosition()))
            owner.setCollapsed(!owner.isCollapsed());
    }

private:
    ModPanel& owner;
    EmberButton sourcesButton, matrixButton;

    juce::String message;
    bool messageIsWarning{false};
    int messageTicks{0};
    int routingCount{-1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderStrip)
};

//==============================================================================
/**
    The sources view: every source as a tile on the left, the selected source's
    own parameters on the right. Below about 560 px of width the two stack
    instead, so the panel stays usable in a narrow window.
*/
class ModPanel::SourcesView : public juce::Component
{
public:
    SourcesView(ModPanel& ownerToUse, EmberAudioProcessor& processorToUse)
        : owner(ownerToUse), processor(processorToUse), detail(ownerToUse, processorToUse)
    {
        tileViewport.setViewedComponent(&tileHolder, false);
        tileViewport.setScrollBarsShown(true, false);
        tileViewport.setScrollBarThickness(8);
        addAndMakeVisible(tileViewport);

        for (int i = 0; i < kNumModSources; ++i)
        {
            auto tile = std::make_unique<SourceTile>(i);
            tile->onSelected = [this, i] { owner.selectSource(i); };
            tileHolder.addAndMakeVisible(*tile);
            tiles.push_back(std::move(tile));
        }

        addAndMakeVisible(detail);
    }

    void setSelected(int flatIndex)
    {
        for (int i = 0; i < static_cast<int>(tiles.size()); ++i)
            tiles[static_cast<size_t>(i)]->setSelected(i == flatIndex);

        detail.setSource(flatIndex);
    }

    void tick()
    {
        const auto& engine = processor.getModulationEngine();

        for (int i = 0; i < static_cast<int>(tiles.size()); ++i)
            tiles[static_cast<size_t>(i)]->setValue(engine.getSourceValue(i));

        detail.tick();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const bool sideBySide = area.getWidth() >= 560;

        if (sideBySide)
        {
            const int listWidth = juce::jlimit(190, 430, juce::roundToInt(static_cast<float>(area.getWidth()) * 0.42f));
            tileViewport.setBounds(area.removeFromLeft(listWidth));
            area.removeFromLeft(8);
        }
        else
        {
            const int listHeight =
                juce::jlimit(60, 130, juce::roundToInt(static_cast<float>(area.getHeight()) * 0.42f));
            tileViewport.setBounds(area.removeFromTop(listHeight));
            area.removeFromTop(6);
        }

        detail.setBounds(area);

        // Twice on purpose: the first pass decides whether the tile list needs
        // a scrollbar, which changes the width the second pass lays out into.
        layoutTiles();
        layoutTiles();
    }

private:
    /** Lays the tiles out block by block — each source type starts on a new
        row — so the grid groups itself without any extra chrome. */
    void layoutTiles()
    {
        const float scale = owner.uiScale();
        const int width = juce::jmax(40, tileViewport.getMaximumVisibleWidth());
        const int tileHeight = juce::jlimit(24, 40, juce::roundToInt(30.0f * scale));
        const int minTileWidth = juce::jlimit(96, 170, juce::roundToInt(122.0f * scale));
        const int gap = 4;

        const int columns = juce::jmax(1, (width + gap) / (minTileWidth + gap));
        const int tileWidth = (width - gap * (columns - 1)) / columns;

        constexpr std::array<int, 6> blockSizes{
            {kNumXLFOs, kNumEnvGenerators, kNumEnvFollowers, kNumXYControllers, kNumMidiSources, kNumMacros}};

        int index = 0;
        int row = 0;

        for (const int blockSize : blockSizes)
        {
            for (int withinBlock = 0; withinBlock < blockSize; ++withinBlock)
            {
                const int column = withinBlock % columns;

                if (withinBlock > 0 && column == 0)
                    ++row;

                const int x = column * (tileWidth + gap);
                const int y = row * (tileHeight + gap);

                if (index < static_cast<int>(tiles.size()))
                    tiles[static_cast<size_t>(index)]->setBounds(x, y, tileWidth, tileHeight);

                ++index;
            }

            ++row;
        }

        tileHolder.setSize(width, juce::jmax(tileHeight, row * (tileHeight + gap)));
    }

    //==========================================================================
    /** The selected source's own parameters, plus its shape editor or scope. */
    class DetailPane : public juce::Component
    {
    public:
        DetailPane(ModPanel& ownerToUse, EmberAudioProcessor& processorToUse)
            : owner(ownerToUse), processor(processorToUse)
        {
            addAndMakeVisible(sectionHeader);
            addAndMakeVisible(grip);
        }

        void setSource(int flatIndex)
        {
            if (sourceIndex == flatIndex)
                return;

            sourceIndex = flatIndex;
            rebuild();
        }

        void tick()
        {
            if (!juce::isPositiveAndBelow(sourceIndex, kNumModSources))
                return;

            const float value = processor.getModulationEngine().getSourceValue(sourceIndex);

            if (shapeEditor != nullptr)
                shapeEditor->tick();

            if (scope != nullptr)
                scope->push(value);
        }

        void resized() override
        {
            auto area = getLocalBounds();

            if (area.getHeight() < 24 || area.getWidth() < 40)
                return;

            const float scale = owner.uiScale();
            const int headerHeight = juce::jmin(SectionHeader::preferredHeight(scale), area.getHeight() / 3);

            auto headerRow = area.removeFromTop(headerHeight);
            grip.setBounds(headerRow.removeFromRight(juce::jlimit(12, 20, headerHeight)).reduced(0, 2));
            headerRow.removeFromRight(4);
            sectionHeader.setBounds(headerRow);

            area.removeFromTop(4);

            // ---- the control strip along the bottom: the source's switches
            // and choice lists on the left, the shape presets on the right.
            // One row, not two, because the display above it is the part that
            // actually needs the height.
            const bool hasControlRow = !comboBoxes.empty() || toggle != nullptr || !shapeButtons.empty();
            const int controlHeight = hasControlRow ? juce::jlimit(17, 26, juce::roundToInt(21.0f * scale)) : 0;

            int knobHeight = juce::jlimit(34, 86, juce::roundToInt(static_cast<float>(area.getHeight()) * 0.42f));
            knobHeight = juce::jmin(knobHeight, juce::jmax(0, area.getHeight() - controlHeight - 52));

            if (!knobs.empty() && knobHeight > 20)
            {
                auto knobRow = area.removeFromBottom(knobHeight);
                const int cellWidth = knobRow.getWidth() / static_cast<int>(knobs.size());

                for (auto& knob : knobs)
                {
                    knob->setVisible(true);
                    knob->setBounds(knobRow.removeFromLeft(cellWidth).reduced(2, 0));
                }
            }
            else
            {
                for (auto& knob : knobs)
                    knob->setVisible(false);
            }

            const bool showControlRow = controlHeight > 0 && area.getHeight() > controlHeight + 16;

            for (auto& button : shapeButtons)
                button->setVisible(showControlRow);

            if (toggle != nullptr)
                toggle->setVisible(showControlRow);

            for (auto& box : comboBoxes)
                box->setVisible(showControlRow);

            for (auto& caption : comboCaptions)
                caption->setVisible(showControlRow);

            if (showControlRow)
            {
                auto controlRow = area.removeFromBottom(controlHeight);
                area.removeFromBottom(4);

                if (!shapeButtons.empty())
                {
                    const int count = static_cast<int>(shapeButtons.size());
                    const int buttonWidth =
                        juce::jlimit(22, juce::roundToInt(42.0f * scale),
                                     juce::jmax(1, controlRow.getWidth() / juce::jmax(2, count * 2)));

                    // Placed from the right, so the first preset still reads
                    // leftmost in the group.
                    for (int i = count - 1; i >= 0; --i)
                        shapeButtons[static_cast<size_t>(i)]->setBounds(
                            controlRow.removeFromRight(buttonWidth).reduced(1, 0));

                    controlRow.removeFromRight(6);
                }

                const int cells = static_cast<int>(comboBoxes.size()) + (toggle != nullptr ? 1 : 0);
                const int cellWidth = controlRow.getWidth() / juce::jmax(1, cells);

                if (toggle != nullptr)
                    toggle->setBounds(controlRow.removeFromLeft(cellWidth).reduced(2, 0));

                for (size_t i = 0; i < comboBoxes.size(); ++i)
                {
                    auto cell = controlRow.removeFromLeft(cellWidth).reduced(2, 0);
                    const int captionWidth = juce::jlimit(24, 58, cell.getWidth() / 3);

                    comboCaptions[i]->setBounds(cell.removeFromLeft(captionWidth));
                    comboCaptions[i]->setFont(
                        EmberFonts::forHeight(static_cast<float>(cell.getHeight()), 0.56f, false));
                    comboBoxes[i]->setBounds(cell);
                }
            }

            if (area.getHeight() > 16)
            {
                if (shapeEditor != nullptr)
                    shapeEditor->setBounds(area);

                if (scope != nullptr)
                    scope->setBounds(area);

                if (xyPad != nullptr)
                {
                    const int side = juce::jmin(area.getWidth(), area.getHeight());
                    xyPad->setBounds(area.withSizeKeepingCentre(juce::jmax(40, side), juce::jmax(40, side)));
                }
            }
        }

    private:
        void rebuild()
        {
            knobs.clear();
            comboBoxes.clear();
            comboCaptions.clear();
            shapeButtons.clear();
            toggle.reset();
            shapeEditor.reset();
            scope.reset();
            xyPad.reset();

            if (!juce::isPositiveAndBelow(sourceIndex, kNumModSources))
                return;

            const auto info = sourceInfoFromFlatIndex(sourceIndex);
            const int ordinal = info.indexWithinType;

            sectionHeader.setTitle(modSourceDisplayName(sourceIndex));
            sectionHeader.setTrailingText(info.bipolar ? "BIPOLAR" : "UNIPOLAR");
            grip.setSourceIndex(sourceIndex);

            switch (info.type)
            {
            case ModSourceType::XLFO:
                addShapeEditor(ordinal);
                addToggle(pid::lfoSync(ordinal), "Sync");
                addKnob(pid::lfoRate(ordinal), "Rate");
                addKnob(pid::lfoPhase(ordinal), "Phase");
                addKnob(pid::lfoSteps(ordinal), "Steps");
                addKnob(pid::lfoSmooth(ordinal), "Smooth");
                addKnob(pid::lfoDepth(ordinal), "Depth");
                break;

            case ModSourceType::EnvelopeGenerator:
                addScope(info.bipolar, "ENVELOPE");
                addCombo(pid::egTrigger(ordinal), "Trig");
                addKnob(pid::egAttack(ordinal), "Attack");
                addKnob(pid::egDecay(ordinal), "Decay");
                addKnob(pid::egSustain(ordinal), "Sustain");
                addKnob(pid::egRelease(ordinal), "Release");
                addKnob(pid::egThreshold(ordinal), "Thresh");
                break;

            case ModSourceType::EnvelopeFollower:
                addScope(info.bipolar, "FOLLOWER");
                addCombo(pid::efBand(ordinal), "Band");
                addKnob(pid::efAttack(ordinal), "Attack");
                addKnob(pid::efRelease(ordinal), "Release");
                addKnob(pid::efGain(ordinal), "Gain");
                break;

            case ModSourceType::XYController:
                xyPad = std::make_unique<XyPad>(processor.getAPVTS(), &processor.getUndoManager());
                addAndMakeVisible(*xyPad);
                addKnob(pid::xyX, "X");
                addKnob(pid::xyY, "Y");
                break;

            case ModSourceType::MidiSource:
                addScope(info.bipolar, "MIDI");
                addCombo(pid::midiType(ordinal), "Type");
                addKnob(pid::midiCC(ordinal), "CC");
                addKnob(pid::midiSmooth(ordinal), "Smooth");
                break;

            case ModSourceType::Macro:
                addScope(info.bipolar, "MACRO");
                addKnob(pid::macro(ordinal), "Value");
                break;

            case ModSourceType::Count:
                break;
            }

            resized();
            repaint();
        }

        void addKnob(const juce::String& parameterID, const juce::String& caption)
        {
            auto knob = std::make_unique<LabelledKnob>(processor, parameterID, caption);
            knob->getKnob().onModulationDropped = [this](const juce::String& target, int source)
            { owner.createConnection(target, source); };
            knob->getKnob().onRemoveModulation = [this](const juce::String& target)
            { owner.removeConnectionsForTarget(target); };
            addAndMakeVisible(*knob);
            knobs.push_back(std::move(knob));
        }

        void addCombo(const juce::String& parameterID, const juce::String& caption)
        {
            auto label = std::make_unique<juce::Label>();
            label->setText(caption, juce::dontSendNotification);
            label->setJustificationType(juce::Justification::centredRight);
            label->setInterceptsMouseClicks(false, false);
            label->setColour(juce::Label::textColourId, EmberColours::textSecondary);
            addAndMakeVisible(*label);

            auto box = std::make_unique<EmberComboBox>();
            box->attachTo(processor.getAPVTS(), parameterID);
            addAndMakeVisible(*box);

            comboCaptions.push_back(std::move(label));
            comboBoxes.push_back(std::move(box));
        }

        void addToggle(const juce::String& parameterID, const juce::String& text)
        {
            toggle = std::make_unique<EmberToggle>(text, ToggleLook::pill);
            toggle->attachTo(processor.getAPVTS(), parameterID);
            addAndMakeVisible(*toggle);
        }

        void addScope(bool bipolar, const juce::String& caption)
        {
            scope = std::make_unique<ValueScope>();
            scope->setBipolar(bipolar);
            scope->setCaption(caption);
            addAndMakeVisible(*scope);
        }

        void addShapeEditor(int lfoOrdinal)
        {
            shapeEditor = std::make_unique<ShapeEditor>(processor);
            shapeEditor->onMessage = [this](const juce::String& text, bool isWarning)
            { owner.showMessage(text, isWarning); };
            shapeEditor->setLfoIndex(lfoOrdinal);
            addAndMakeVisible(*shapeEditor);

            static const std::array<const char*, 5> presetNames{{"SIN", "TRI", "RMP", "SQR", "RND"}};

            for (int i = 0; i < static_cast<int>(presetNames.size()); ++i)
            {
                auto button =
                    std::make_unique<EmberButton>(presetNames[static_cast<size_t>(i)], EmberButton::Style::ghost);
                button->onClick = [this, i]
                {
                    if (shapeEditor != nullptr)
                        shapeEditor->applyPreset(i);
                };
                addAndMakeVisible(*button);
                shapeButtons.push_back(std::move(button));
            }
        }

        ModPanel& owner;
        EmberAudioProcessor& processor;
        int sourceIndex{-1};

        SectionHeader sectionHeader;
        DragGrip grip;

        std::vector<std::unique_ptr<LabelledKnob>> knobs;
        std::vector<std::unique_ptr<juce::Label>> comboCaptions;
        std::vector<std::unique_ptr<EmberComboBox>> comboBoxes;
        std::vector<std::unique_ptr<EmberButton>> shapeButtons;
        std::unique_ptr<EmberToggle> toggle;
        std::unique_ptr<ShapeEditor> shapeEditor;
        std::unique_ptr<ValueScope> scope;
        std::unique_ptr<XyPad> xyPad;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DetailPane)
    };

    ModPanel& owner;
    EmberAudioProcessor& processor;

    juce::Viewport tileViewport;
    juce::Component tileHolder;
    std::vector<std::unique_ptr<SourceTile>> tiles;
    DetailPane detail;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesView)
};

//==============================================================================
/** One routing. Every edit goes back through the panel, which owns the graph. */
class ModPanel::MatrixView : public juce::Component
{
public:
    MatrixView(ModPanel& ownerToUse, EmberAudioProcessor& processorToUse) : owner(ownerToUse), processor(processorToUse)
    {
        rowViewport.setViewedComponent(&rowHolder, false);
        rowViewport.setScrollBarsShown(true, true);
        rowViewport.setScrollBarThickness(8);
        addAndMakeVisible(rowViewport);

        addButton.setEmberStyle(EmberButton::Style::accent);
        addButton.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(addButton);

        clearButton.setEmberStyle(EmberButton::Style::danger);
        clearButton.onClick = [this] { owner.clearAllConnections(); };
        addAndMakeVisible(clearButton);

        rebuildRows();
    }

    void rebuildRows()
    {
        const auto& engine = processor.getModulationEngine();
        const int count = engine.getNumConnections();

        while (static_cast<int>(rows.size()) > count)
            rows.pop_back();

        while (static_cast<int>(rows.size()) < count)
        {
            auto row = std::make_unique<Row>(owner, processor, static_cast<int>(rows.size()));
            rowHolder.addAndMakeVisible(*row);
            rows.push_back(std::move(row));
        }

        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            rows[static_cast<size_t>(i)]->setSlot(i);
            rows[static_cast<size_t>(i)]->refresh();
        }

        clearButton.setEnabled(count > 0);
        addButton.setEnabled(count < kMaxModConnections);
        resized();
        repaint();
    }

    void flashSlot(int slot)
    {
        if (juce::isPositiveAndBelow(slot, static_cast<int>(rows.size())))
        {
            rows[static_cast<size_t>(slot)]->flash();
            rowViewport.setViewPosition(rowViewport.getViewPositionX(),
                                        juce::jmax(0, rows[static_cast<size_t>(slot)]->getY() - 4));
        }
    }

    void tick()
    {
        for (auto& row : rows)
            row->tickFlash();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const float scale = owner.uiScale();

        const int footerHeight = juce::jlimit(19, 30, juce::roundToInt(23.0f * scale));
        auto footer = area.removeFromBottom(footerHeight);
        area.removeFromBottom(4);

        const int buttonWidth = juce::jlimit(62, 110, juce::roundToInt(86.0f * scale));
        addButton.setBounds(footer.removeFromLeft(buttonWidth));
        footer.removeFromLeft(6);
        clearButton.setBounds(footer.removeFromLeft(buttonWidth));

        headerArea = area.removeFromTop(juce::jlimit(14, 24, juce::roundToInt(17.0f * scale)));
        rowViewport.setBounds(area);

        const int rowHeight = juce::jlimit(22, 36, juce::roundToInt(27.0f * scale));
        const int contentHeight = static_cast<int>(rows.size()) * rowHeight;

        // Worked out rather than asked for: `getMaximumVisibleWidth` only knows
        // about the scrollbar the viewport is showing NOW, which is the one
        // that belongs to the previous layout.
        const bool needsVerticalBar = contentHeight > rowViewport.getHeight();
        const int available = rowViewport.getWidth() - (needsVerticalBar ? rowViewport.getScrollBarThickness() : 0);

        contentWidth = juce::jmax(matrixMinimumWidth(scale), available);

        rowHolder.setSize(contentWidth, juce::jmax(rowHeight, contentHeight));

        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            rows[static_cast<size_t>(i)]->setBounds(0, i * rowHeight, contentWidth, rowHeight);
    }

    void paint(juce::Graphics& g) override
    {
        // The column header lines up with the rows even when they are scrolled
        // sideways, so the titles never lie about what they label.
        auto strip = headerArea.withX(headerArea.getX() - rowViewport.getViewPositionX()).withWidth(contentWidth);

        const auto columns = matrixColumns(strip.reduced(4, 0), owner.uiScale());

        {
            const juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(headerArea);
            g.setFont(EmberFonts::forHeight(static_cast<float>(headerArea.getHeight()), 0.62f, true));
            g.setColour(EmberColours::textDisabled);

            g.drawText("SOURCE", columns.source, juce::Justification::centredLeft, false);
            g.drawText("DESTINATION", columns.destination, juce::Justification::centredLeft, false);
            g.drawText("AMOUNT", columns.amount, juce::Justification::centred, false);
            g.drawText("CURVE", columns.curve, juce::Justification::centred, false);
            g.drawText("SMOOTH", columns.smoothing, juce::Justification::centred, false);
            g.drawText("ON", columns.enabled, juce::Justification::centred, false);
        }

        if (rows.empty())
        {
            g.setFont(EmberFonts::get(EmberFonts::Role::body, owner.uiScale()));
            g.setColour(EmberColours::textDisabled);
            g.drawFittedText("No routings yet. Drag a source grip onto any knob, or press Add Routing.",
                             rowViewport.getBounds().reduced(16), juce::Justification::centred, 2, 0.8f);
        }
    }

private:
    //==========================================================================
    class Row : public juce::Component
    {
    public:
        Row(ModPanel& ownerToUse, EmberAudioProcessor& processorToUse, int slotToUse)
            : owner(ownerToUse), processor(processorToUse), slot(slotToUse), amount(true, 0.0),
              smoothing(false, static_cast<double>(kDefaultSmoothingMs))
        {
            for (int i = 0; i < kNumModSources; ++i)
                sourceBox.addItem(modSourceDisplayName(i), i + 1);

            sourceBox.setTooltip("The modulation source feeding this routing.");
            sourceBox.onChange = [this]
            {
                if (!updating)
                    owner.changeConnectionRouting(slot, sourceBox.getSelectedId() - 1, -1);
            };
            addAndMakeVisible(sourceBox);

            destination.onClick = [this]
            {
                owner.showDestinationMenu(destination, currentSource(), slot,
                                          [this](int target) { owner.changeConnectionRouting(slot, -1, target); });
            };
            addAndMakeVisible(destination);

            amount.formatValue = [](double value) { return formatAmount(value); };
            amount.setRange(-1.0, 1.0, 0.0);
            amount.setTooltip("How far this source moves the destination. Negative inverts it. "
                              "Double-click to zero, shift-drag for fine control.");
            amount.onValueChange = [this]
            {
                if (!updating)
                    applyEdit([this](ModConnection& connection)
                              { connection.amount = static_cast<float>(amount.getValue()); });
            };
            addAndMakeVisible(amount);

            const auto names = curveNames();

            for (int i = 0; i < names.size(); ++i)
                curveBox.addItem(names[i], i + 1);

            curveBox.setTooltip("Response curve applied to the source before the amount.");
            curveBox.onChange = [this]
            {
                if (!updating)
                    applyEdit([this](ModConnection& connection)
                              { connection.curve = curveFromIndex(curveBox.getSelectedId() - 1); });
            };
            addAndMakeVisible(curveBox);

            smoothing.formatValue = [](double value) { return formatMilliseconds(value); };
            smoothing.setRange(0.0, 500.0, 0.0);
            smoothing.setSkewFactor(0.45);
            smoothing.setTooltip("Smoothing applied to this routing's contribution. "
                                 "Double-click for the default, shift-drag for fine control.");
            smoothing.onValueChange = [this]
            {
                if (!updating)
                    applyEdit([this](ModConnection& connection)
                              { connection.smoothingMs = static_cast<float>(smoothing.getValue()); });
            };
            addAndMakeVisible(smoothing);

            enableToggle.setTooltip("Enable or mute this routing. A muted routing ramps out rather than stepping.");
            enableToggle.onClick = [this]
            {
                if (!updating)
                    applyEdit([this](ModConnection& connection)
                              { connection.enabled = enableToggle.getToggleState(); });
            };
            addAndMakeVisible(enableToggle);

            deleteButton.setTooltip("Delete this routing.");
            deleteButton.onClick = [this] { owner.deleteConnection(slot); };
            addAndMakeVisible(deleteButton);
        }

        void setSlot(int newSlot) { slot = newSlot; }

        void refresh()
        {
            const auto& connection = processor.getModulationEngine().getConnection(slot);

            const juce::ScopedValueSetter<bool> guard(updating, true);

            sourceBox.setSelectedId(connection.sourceIndex + 1, juce::dontSendNotification);
            curveBox.setSelectedId(static_cast<int>(connection.curve) + 1, juce::dontSendNotification);
            amount.setValue(static_cast<double>(connection.amount), juce::dontSendNotification);
            smoothing.setValue(static_cast<double>(connection.smoothingMs), juce::dontSendNotification);
            enableToggle.setToggleState(connection.enabled, juce::dontSendNotification);

            const auto name = owner.describeTarget(connection.targetIndex);
            destination.setText(name);
            destination.setTooltip(name + " - click to send this source somewhere else.");

            setAlpha(connection.enabled ? 1.0f : 0.62f);
            repaint();
        }

        void flash()
        {
            flashAmount = 1.0f;
            repaint();
        }

        void tickFlash()
        {
            if (flashAmount <= 0.0f)
                return;

            flashAmount = juce::jmax(0.0f, flashAmount - 0.035f);
            repaint();
        }

        void resized() override
        {
            const auto columns = matrixColumns(getLocalBounds().reduced(4, 2), owner.uiScale());

            sourceBox.setBounds(columns.source);
            destination.setBounds(columns.destination);
            amount.setBounds(columns.amount);
            curveBox.setBounds(columns.curve);
            smoothing.setBounds(columns.smoothing);
            enableToggle.setBounds(columns.enabled);
            deleteButton.setBounds(columns.remove);
        }

        void paint(juce::Graphics& g) override
        {
            auto area = getLocalBounds().toFloat();

            if ((slot % 2) == 1)
            {
                g.setColour(EmberColours::panel.withAlpha(0.5f));
                g.fillRect(area);
            }

            if (flashAmount > 0.0f)
            {
                g.setColour(EmberColours::accent.withAlpha(flashAmount * 0.22f));
                g.fillRect(area);
            }

            EmberLookAndFeel::drawHairline(g, area.removeFromBottom(1.0f), EmberColours::outline, false);

            const auto columns = matrixColumns(getLocalBounds().reduced(4, 2), owner.uiScale());
            const auto arrow = columns.arrow.toFloat().reduced(2.0f, 0.0f);

            juce::Path path;
            path.startNewSubPath(arrow.getX(), arrow.getCentreY());
            path.lineTo(arrow.getRight(), arrow.getCentreY());
            path.startNewSubPath(arrow.getRight() - 4.0f, arrow.getCentreY() - 3.0f);
            path.lineTo(arrow.getRight(), arrow.getCentreY());
            path.lineTo(arrow.getRight() - 4.0f, arrow.getCentreY() + 3.0f);

            g.setColour(EmberColours::outlineStrong);
            g.strokePath(path, juce::PathStrokeType(1.2f));
        }

    private:
        int currentSource() const { return processor.getModulationEngine().getConnection(slot).sourceIndex; }

        void applyEdit(const std::function<void(ModConnection&)>& mutate)
        {
            auto connection = processor.getModulationEngine().getConnection(slot);
            mutate(connection);
            processor.getModulationEngine().setConnection(slot, connection);
            setAlpha(connection.enabled ? 1.0f : 0.62f);
            owner.connectionsChanged();
        }

        ModPanel& owner;
        EmberAudioProcessor& processor;
        int slot;

        EmberComboBox sourceBox, curveBox;
        DestinationButton destination;
        BarSlider amount, smoothing;
        EmberToggle enableToggle{"", ToggleLook::led};
        EmberButton deleteButton{juce::String::fromUTF8("\xc3\x97"), EmberButton::Style::danger};

        bool updating{false};
        float flashAmount{0.0f};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Row)
    };

    void showAddMenu()
    {
        owner.showSourceMenu(addButton,
                             [this](int source)
                             {
                                 owner.showDestinationMenu(addButton, source, -1, [this, source](int target)
                                                           { owner.createConnectionByIndex(source, target); });
                             });
    }

    ModPanel& owner;
    EmberAudioProcessor& processor;

    juce::Viewport rowViewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<Row>> rows;

    EmberButton addButton{"Add Routing", EmberButton::Style::accent};
    EmberButton clearButton{"Clear All", EmberButton::Style::danger};

    juce::Rectangle<int> headerArea;
    int contentWidth{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MatrixView)
};

//==============================================================================
//==============================================================================
ModPanel::ModPanel(EmberAudioProcessor& processorToUse) : processor(processorToUse)
{
    buildTargetGroups();

    header = std::make_unique<HeaderStrip>(*this);
    sourcesView = std::make_unique<SourcesView>(*this, processor);
    matrixView = std::make_unique<MatrixView>(*this, processor);

    addAndMakeVisible(*header);
    addAndMakeVisible(*sourcesView);
    addChildComponent(*matrixView);

    sourcesView->setSelected(selectedSource);
    header->setRoutingCount(processor.getModulationEngine().getNumConnections());
    lastGraphSignature = graphSignature();

    startTimerHz(kRefreshHz);
}

ModPanel::~ModPanel()
{
    stopTimer();
}

//==============================================================================
bool ModPanel::createConnection(const juce::String& targetParameterID, int sourceFlatIndex)
{
    const int targetIndex = processor.getModulationTargetIndex(targetParameterID);

    if (targetIndex < 0)
    {
        showMessage("That control cannot be modulated.", true);
        return false;
    }

    return createConnectionByIndex(sourceFlatIndex, targetIndex);
}

bool ModPanel::createConnectionByIndex(int sourceFlatIndex, int targetIndex)
{
    auto& engine = processor.getModulationEngine();

    if (!juce::isPositiveAndBelow(sourceFlatIndex, kNumModSources) ||
        !juce::isPositiveAndBelow(targetIndex, processor.getNumModulationTargets()))
    {
        showMessage("That routing is not valid.", true);
        return false;
    }

    for (int slot = 0; slot < engine.getNumConnections(); ++slot)
    {
        const auto& existing = engine.getConnection(slot);

        if (existing.sourceIndex == sourceFlatIndex && existing.targetIndex == targetIndex)
        {
            showMessage(modSourceDisplayName(sourceFlatIndex) + " already modulates " + describeTarget(targetIndex) +
                            ".",
                        false);
            matrixView->flashSlot(slot);
            return false;
        }
    }

    if (engine.getNumConnections() >= kMaxModConnections)
    {
        showMessage("The matrix is full: " + juce::String(kMaxModConnections) + " routings is the maximum.", true);
        return false;
    }

    const auto info = sourceInfoFromFlatIndex(sourceFlatIndex);

    ModConnection connection;
    connection.sourceIndex = sourceFlatIndex;
    connection.targetIndex = targetIndex;
    connection.amount = info.bipolar ? kDefaultBipolarAmount : kDefaultUnipolarAmount;
    connection.curve = ModCurve::Linear;
    connection.smoothingMs = kDefaultSmoothingMs;
    connection.enabled = true;

    if (!engine.addConnection(connection))
    {
        showMessage(modSourceDisplayName(sourceFlatIndex) + " cannot modulate " + describeTarget(targetIndex) +
                        ": it would create a modulation loop.",
                    true);
        return false;
    }

    connectionsChanged();
    showMessage(modSourceDisplayName(sourceFlatIndex) + " -> " + describeTarget(targetIndex), false);
    matrixView->flashSlot(engine.getNumConnections() - 1);

    return true;
}

bool ModPanel::changeConnectionRouting(int slot, int newSourceIndex, int newTargetIndex)
{
    auto& engine = processor.getModulationEngine();

    if (!juce::isPositiveAndBelow(slot, engine.getNumConnections()))
        return false;

    // A copy, not a reference: the original has to survive the rebuild below.
    const auto original = engine.getConnection(slot);

    auto updated = original;

    if (newSourceIndex >= 0)
        updated.sourceIndex = newSourceIndex;

    if (newTargetIndex >= 0)
        updated.targetIndex = newTargetIndex;

    if (updated.sourceIndex == original.sourceIndex && updated.targetIndex == original.targetIndex)
        return true;

    for (int other = 0; other < engine.getNumConnections(); ++other)
    {
        if (other == slot)
            continue;

        const auto& existing = engine.getConnection(other);

        if (existing.sourceIndex == updated.sourceIndex && existing.targetIndex == updated.targetIndex)
        {
            showMessage("That routing already exists.", true);
            matrixView->rebuildRows();
            return false;
        }
    }

    // `setConnection` rejects a cycle silently, so the graph is rebuilt instead:
    // the whole table goes back in slot order with this one routing replaced,
    // which lets the engine's own topological check answer AND keeps the row
    // the user is editing where it was. A refused rebuild restores the original
    // table exactly, and it can never fail — every routing in it was legal a
    // moment ago.
    std::vector<ModConnection> table;
    table.reserve(static_cast<size_t>(engine.getNumConnections()));

    for (int other = 0; other < engine.getNumConnections(); ++other)
        table.push_back(engine.getConnection(other));

    const auto restore = [&engine](const std::vector<ModConnection>& connections)
    {
        engine.clearConnections();

        for (const auto& connection : connections)
            engine.addConnection(connection);
    };

    auto replacement = table;
    replacement[static_cast<size_t>(slot)] = updated;

    engine.clearConnections();
    bool accepted = true;

    for (const auto& connection : replacement)
        accepted = engine.addConnection(connection) && accepted;

    if (!accepted)
    {
        restore(table);
        showMessage("That routing would create a modulation loop.", true);
        refreshFromEngine();
        return false;
    }

    connectionsChanged();
    matrixView->flashSlot(slot);

    return true;
}

int ModPanel::removeConnectionsForTarget(const juce::String& targetParameterID)
{
    const int targetIndex = processor.getModulationTargetIndex(targetParameterID);

    if (targetIndex < 0)
        return 0;

    auto& engine = processor.getModulationEngine();
    int removed = 0;

    for (int slot = engine.getNumConnections() - 1; slot >= 0; --slot)
    {
        if (engine.getConnection(slot).targetIndex == targetIndex)
        {
            engine.removeConnection(slot);
            ++removed;
        }
    }

    if (removed > 0)
    {
        connectionsChanged();
        showMessage(juce::String(removed) + (removed == 1 ? " routing removed from " : " routings removed from ") +
                        describeTarget(targetIndex) + ".",
                    false);
    }

    return removed;
}

void ModPanel::deleteConnection(int slot)
{
    auto& engine = processor.getModulationEngine();

    if (!juce::isPositiveAndBelow(slot, engine.getNumConnections()))
        return;

    const auto connection = engine.getConnection(slot);
    engine.removeConnection(slot);
    connectionsChanged();
    showMessage(modSourceDisplayName(connection.sourceIndex) + " -> " + describeTarget(connection.targetIndex) +
                    " removed.",
                false);
}

void ModPanel::clearAllConnections()
{
    auto& engine = processor.getModulationEngine();
    const int count = engine.getNumConnections();

    if (count == 0)
        return;

    engine.clearConnections();
    connectionsChanged();
    showMessage(juce::String(count) + (count == 1 ? " routing cleared." : " routings cleared."), false);
}

//==============================================================================
void ModPanel::selectSource(int sourceFlatIndex)
{
    if (!juce::isPositiveAndBelow(sourceFlatIndex, kNumModSources))
        return;

    selectedSource = sourceFlatIndex;
    sourcesView->setSelected(selectedSource);

    setView(View::sources);
    setCollapsed(false);
}

void ModPanel::setView(View newView)
{
    if (currentView == newView)
        return;

    currentView = newView;
    header->setViewState(currentView);
    resized();
}

void ModPanel::setCollapsed(bool shouldBeCollapsed)
{
    if (collapsed == shouldBeCollapsed)
        return;

    collapsed = shouldBeCollapsed;
    header->repaint();
    resized();

    if (onCollapsedChanged != nullptr)
        onCollapsedChanged();
}

int ModPanel::getHeaderHeight() const
{
    return juce::roundToInt(juce::jmax(24.0f, 27.0f * uiScale()));
}

int ModPanel::getPreferredHeight() const
{
    if (collapsed)
        return getHeaderHeight();

    return getHeaderHeight() + juce::roundToInt(juce::jmax(170.0f, 240.0f * uiScale()));
}

void ModPanel::refreshFromEngine()
{
    lastGraphSignature = graphSignature();
    matrixView->rebuildRows();
    header->setRoutingCount(processor.getModulationEngine().getNumConnections());
}

void ModPanel::connectionsChanged()
{
    refreshFromEngine();

    if (onConnectionsChanged != nullptr)
        onConnectionsChanged();
}

//==============================================================================
void ModPanel::paint(juce::Graphics& g)
{
    EmberLookAndFeel::drawPanel(g, getLocalBounds().toFloat(), juce::jmax(3.0f, 5.0f * uiScale()), false);
}

void ModPanel::resized()
{
    auto area = getLocalBounds();

    header->setBounds(area.removeFromTop(juce::jmin(getHeaderHeight(), area.getHeight())));

    const bool showBody = !collapsed && area.getHeight() > 12;
    const auto body = area.reduced(juce::roundToInt(6.0f * uiScale()), 5);

    sourcesView->setVisible(showBody && currentView == View::sources);
    matrixView->setVisible(showBody && currentView == View::matrix);

    if (!showBody)
        return;

    if (sourcesView->isVisible())
        sourcesView->setBounds(body);

    if (matrixView->isVisible())
        matrixView->setBounds(body);
}

void ModPanel::timerCallback()
{
    header->tick();

    if (!collapsed)
    {
        if (currentView == View::sources && sourcesView->isVisible())
            sourcesView->tick();

        if (currentView == View::matrix && matrixView->isVisible())
            matrixView->tick();
    }

    // Presets, undo and A/B all rewrite the graph behind the panel's back.
    const auto signature = graphSignature();

    if (signature != lastGraphSignature)
        refreshFromEngine();
}

void ModPanel::showMessage(const juce::String& text, bool isWarning)
{
    header->showMessage(text, isWarning);
}

//==============================================================================
void ModPanel::showSourceMenu(juce::Component& target, std::function<void(int)> onPicked)
{
    juce::PopupMenu menu;

    constexpr std::array<ModSourceType, 6> types{{ModSourceType::XLFO, ModSourceType::EnvelopeGenerator,
                                                  ModSourceType::EnvelopeFollower, ModSourceType::XYController,
                                                  ModSourceType::MidiSource, ModSourceType::Macro}};
    constexpr std::array<int, 6> counts{
        {kNumXLFOs, kNumEnvGenerators, kNumEnvFollowers, kNumXYControllers, kNumMidiSources, kNumMacros}};
    constexpr std::array<const char*, 6> groupNames{
        {"LFOs", "Envelope Generators", "Envelope Followers", "XY", "MIDI", "Macros"}};

    for (size_t block = 0; block < types.size(); ++block)
    {
        juce::PopupMenu submenu;

        for (int ordinal = 0; ordinal < counts[block]; ++ordinal)
        {
            const int flat = flatSourceIndex(types[block], ordinal);

            if (flat >= 0)
                submenu.addItem(flat + 1, modSourceDisplayName(flat));
        }

        menu.addSubMenu(groupNames[block], submenu);
    }

    juce::Component::SafePointer<ModPanel> safeThis(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [safeThis, onPicked](int result)
                       {
                           if (safeThis != nullptr && result > 0 && onPicked != nullptr)
                               onPicked(result - 1);
                       });
}

void ModPanel::showDestinationMenu(juce::Component& target, int sourceFlatIndex, int slotToIgnore,
                                   std::function<void(int)> onPicked)
{
    const int numTargets = processor.getNumModulationTargets();
    const auto& engine = processor.getModulationEngine();

    // Destinations this source already drives cannot be picked again: the
    // engine would refuse the duplicate, so the menu says so up front.
    std::vector<bool> alreadyUsed(static_cast<size_t>(juce::jmax(0, numTargets)), false);

    for (int slot = 0; slot < engine.getNumConnections(); ++slot)
    {
        if (slot == slotToIgnore)
            continue;

        const auto& connection = engine.getConnection(slot);

        if (connection.sourceIndex == sourceFlatIndex && juce::isPositiveAndBelow(connection.targetIndex, numTargets))
            alreadyUsed[static_cast<size_t>(connection.targetIndex)] = true;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader("Modulate with " + modSourceDisplayName(sourceFlatIndex));

    for (const auto& group : targetGroupOrder)
    {
        juce::PopupMenu submenu;
        bool anyItems = false;

        for (int index = 0; index < numTargets; ++index)
        {
            if (groupForTarget(index) != group)
                continue;

            auto name = describeTarget(index);

            if (name.startsWith(group + " "))
                name = name.substring(group.length() + 1);

            submenu.addItem(index + 1, name, !alreadyUsed[static_cast<size_t>(index)], false);
            anyItems = true;
        }

        if (anyItems)
            menu.addSubMenu(group, submenu);
    }

    juce::Component::SafePointer<ModPanel> safeThis(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [safeThis, onPicked](int result)
                       {
                           if (safeThis != nullptr && result > 0 && onPicked != nullptr)
                               onPicked(result - 1);
                       });
}

//==============================================================================
juce::String ModPanel::describeTarget(int targetIndex) const
{
    const auto id = processor.getModulationTargetID(targetIndex);

    if (id.isEmpty())
        return "None";

    if (auto* parameter = processor.getAPVTS().getParameter(id))
        return parameter->getName(64);

    return id;
}

juce::String ModPanel::groupForTarget(int targetIndex) const
{
    const auto id = processor.getModulationTargetID(targetIndex);

    return targetGroupById.contains(id) ? targetGroupById[id] : juce::String("Other");
}

void ModPanel::buildTargetGroups()
{
    const auto assign = [this](const juce::String& group, const juce::String& id)
    {
        if (id.isEmpty())
            return;

        targetGroupById.set(id, group);

        if (!targetGroupOrder.contains(group))
            targetGroupOrder.add(group);
    };

    assign("Global", pid::inputGain);
    assign("Global", pid::outputGain);
    assign("Global", pid::globalMix);

    for (int i = 0; i < kMaxCrossovers; ++i)
        assign("Crossovers", pid::crossover(i));

    for (int band = 0; band < kMaxBands; ++band)
    {
        const auto group = "Band " + juce::String(band + 1);

        assign(group, pid::drive(band));
        assign(group, pid::bandMix(band));
        assign(group, pid::level(band));
        assign(group, pid::pan(band));
        assign(group, pid::width(band));
        assign(group, pid::feedback(band));
        assign(group, pid::feedbackFreq(band));
        assign(group, pid::dynamics(band));
        assign(group, pid::toneLow(band));
        assign(group, pid::toneMid(band));
        assign(group, pid::toneHigh(band));
    }

    // The source parameters are destinations too — that is what makes an LFO's
    // rate modulatable by a macro. Group names match the parameters' own names
    // so the menu can drop the repeated prefix.
    for (int i = 0; i < kNumXLFOs; ++i)
    {
        const auto group = "LFO " + juce::String(i + 1);

        assign(group, pid::lfoRate(i));
        assign(group, pid::lfoPhase(i));
        assign(group, pid::lfoSmooth(i));
        assign(group, pid::lfoDepth(i));
    }

    for (int i = 0; i < kNumEnvGenerators; ++i)
    {
        const auto group = "Env Gen " + juce::String(i + 1);

        assign(group, pid::egAttack(i));
        assign(group, pid::egDecay(i));
        assign(group, pid::egSustain(i));
        assign(group, pid::egRelease(i));
        assign(group, pid::egThreshold(i));
    }

    for (int i = 0; i < kNumEnvFollowers; ++i)
    {
        const auto group = "Env Follower " + juce::String(i + 1);

        assign(group, pid::efAttack(i));
        assign(group, pid::efRelease(i));
        assign(group, pid::efGain(i));
    }

    assign("XY", pid::xyX);
    assign("XY", pid::xyY);

    for (int i = 0; i < kNumMidiSources; ++i)
        assign("MIDI " + juce::String(i + 1), pid::midiSmooth(i));

    for (int i = 0; i < kNumMacros; ++i)
        assign("Macros", pid::macro(i));
}

//==============================================================================
juce::uint64 ModPanel::graphSignature() const
{
    const auto& engine = processor.getModulationEngine();

    juce::uint64 hash = 1469598103934665603ULL;

    const auto mix = [&hash](int value)
    {
        hash ^= static_cast<juce::uint64>(static_cast<juce::int64>(value));
        hash *= 1099511628211ULL;
    };

    mix(engine.getNumConnections());

    for (int slot = 0; slot < engine.getNumConnections(); ++slot)
    {
        const auto& connection = engine.getConnection(slot);

        mix(connection.sourceIndex);
        mix(connection.targetIndex);
        mix(juce::roundToInt(connection.amount * 4096.0f));
        mix(static_cast<int>(connection.curve));
        mix(juce::roundToInt(connection.smoothingMs * 16.0f));
        mix(connection.enabled ? 1 : 0);
    }

    return hash;
}

float ModPanel::uiScale() const
{
    if (auto* lookAndFeel = dynamic_cast<EmberLookAndFeel*>(&getLookAndFeel()))
        return lookAndFeel->getUiScale();

    return 1.0f;
}
} // namespace ember::gui
