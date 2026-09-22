#include "gui/GlobalBar.h"
#include "gui/EmberTheme.h"
#include "gui/tutorial/TourTargets.h"
#include "plugin/ParameterIDs.h"

#include <algorithm>
#include <cmath>

namespace ember::gui
{
namespace
{
/** Height the strip is authored against, at UI scale 1.0. */
constexpr float kReferenceBarHeight = 76.0f;

/** `base` px scaled by `scale`, clamped so an extreme window never produces an
    unreadable or absurd control. */
int scaledSize(float base, float scale, int minimum, int maximum)
{
    return juce::jlimit(minimum, maximum, juce::roundToInt(base * scale));
}

/** The index a choice parameter currently sits on. */
int currentChoiceIndex(const juce::RangedAudioParameter& parameter)
{
    return juce::roundToInt(parameter.getNormalisableRange().convertFrom0to1(parameter.getValue()));
}

/** A circular arrow: an arc with a head on one end. `clockwise` puts the head
    on the right-hand end (redo); otherwise it goes on the left (undo). */
void drawCircularArrow(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool clockwise)
{
    const float size = juce::jmin(area.getWidth(), area.getHeight());

    if (size < 4.0f)
        return;

    const auto centre = area.getCentre();
    const float radius = size * 0.33f;
    const float thickness = juce::jmax(1.1f, radius * 0.34f);

    const float from = juce::degreesToRadians(-140.0f);
    const float to = juce::degreesToRadians(140.0f);

    g.setColour(colour);
    g.strokePath(EmberLookAndFeel::arcPath(centre, radius, from, to),
                 juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The head sits on one end of the arc and points along it, into the gap at
    // the bottom — which is what makes the direction readable at 20 px.
    const float headAngle = clockwise ? to : from;
    const juce::Point<float> tip(centre.x + radius * std::sin(headAngle), centre.y - radius * std::cos(headAngle));

    juce::Point<float> direction(std::cos(headAngle), std::sin(headAngle));

    if (!clockwise)
        direction = {-direction.x, -direction.y};

    const juce::Point<float> perpendicular(-direction.y, direction.x);
    const float headLength = radius * 1.05f;
    const float headWidth = radius * 0.9f;

    const auto apex = tip + direction * (headLength * 0.5f);
    const auto back = tip - direction * (headLength * 0.5f);

    juce::Path head;
    head.startNewSubPath(apex);
    head.lineTo(back + perpendicular * (headWidth * 0.5f));
    head.lineTo(back - perpendicular * (headWidth * 0.5f));
    head.closeSubPath();

    g.fillPath(head);
}

/** Two offset cards: "copy this one onto the other one". */
void drawCopyIcon(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const float size = juce::jmin(area.getWidth(), area.getHeight());

    if (size < 5.0f)
        return;

    const float card = size * 0.58f;
    const float offset = size * 0.16f;
    const float corner = card * 0.22f;
    const float thickness = juce::jmax(1.0f, size * 0.075f);

    const auto centre = area.getCentre();
    const juce::Rectangle<float> shape(card, card);

    g.setColour(colour.withMultipliedAlpha(0.55f));
    g.drawRoundedRectangle(shape.withCentre({centre.x - offset, centre.y - offset}), corner, thickness);

    const auto front = shape.withCentre({centre.x + offset, centre.y + offset});

    g.setColour(EmberColours::panelRaised());
    g.fillRoundedRectangle(front, corner);
    g.setColour(colour);
    g.drawRoundedRectangle(front, corner, thickness);
}

/** Three dots: the overflow menu. */
void drawMoreIcon(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const float size = juce::jmin(area.getWidth(), area.getHeight());

    if (size < 4.0f)
        return;

    const float dot = juce::jmax(1.4f, size * 0.11f);
    const float spacing = size * 0.3f;
    const auto centre = area.getCentre();

    g.setColour(colour);

    for (int i = -1; i <= 1; ++i)
    {
        const juce::Rectangle<float> shape(dot * 2.0f, dot * 2.0f);
        g.fillEllipse(shape.withCentre({centre.x + static_cast<float>(i) * spacing, centre.y}));
    }
}
} // namespace

//==============================================================================
IconButton::IconButton(Icon iconToDraw, const juce::String& componentName)
    : juce::Button(componentName), icon(iconToDraw)
{
    setColour(juce::TextButton::buttonColourId, EmberColours::panelRaised());
    setColour(juce::TextButton::buttonOnColourId, EmberColours::accentDim());
}

IconButton::~IconButton() = default;

void IconButton::setIcon(Icon newIcon)
{
    if (icon == newIcon)
        return;

    icon = newIcon;
    repaint();
}

void IconButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto background =
        findColour(getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);

    getLookAndFeel().drawButtonBackground(g, *this, background, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    auto colour = EmberColours::textSecondary();

    if (!isEnabled())
        colour = EmberColours::textDisabled();
    else if (getToggleState())
        colour = EmberColours::textPrimary();
    else if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
        colour = EmberColours::textPrimary();

    const auto area = getLocalBounds().toFloat().reduced(juce::jmax(2.0f, static_cast<float>(getHeight()) * 0.22f));

    drawIcon(g, icon, area, colour);
}

void IconButton::drawIcon(juce::Graphics& g, Icon iconToDraw, juce::Rectangle<float> area, juce::Colour colour)
{
    switch (iconToDraw)
    {
    case Icon::undo:
        drawCircularArrow(g, area, colour, false);
        break;
    case Icon::redo:
        drawCircularArrow(g, area, colour, true);
        break;
    case Icon::copy:
        drawCopyIcon(g, area, colour);
        break;
    case Icon::more:
        drawMoreIcon(g, area, colour);
        break;
    }
}

//==============================================================================
BandCountSelector::BandCountSelector(EmberAudioProcessor& processorToUse, const juce::String& parameterIDToUse)
    : processor(processorToUse), parameterID(parameterIDToUse)
{
    parameter = processor.getAPVTS().getParameter(parameterID);
    jassert(parameter != nullptr);

    if (parameter != nullptr)
    {
        const auto& range = parameter->getNormalisableRange();
        minimumCount = juce::jlimit(1, kMaxBands, juce::roundToInt(range.start));
        maximumCount = juce::jlimit(minimumCount, kMaxBands, juce::roundToInt(range.end));

        attachment = std::make_unique<juce::ParameterAttachment>(
            *parameter, [this](float newValue) { valueFromParameter(newValue); }, &processor.getUndoManager());

        attachment->sendInitialUpdate();
    }
}

BandCountSelector::~BandCountSelector()
{
    // Closing the editor mid-drag never delivers a mouseUp, and an unbalanced
    // beginChangeGesture leaves the host writing automation until the processor
    // itself is destroyed (where JUCE asserts on it).
    finishDragGesture();
}

juce::String BandCountSelector::getTooltip()
{
    juce::String tip = (parameter != nullptr ? parameter->getName(24) : juce::String("Band Count"));
    tip << ": " << currentCount << (currentCount == 1 ? " band" : " bands");
    tip << "\nHow many bands the signal is split into.";
    tip << "\nClick or drag to set, wheel to step, double-click to reset.";

    return tip;
}

juce::Rectangle<float> BandCountSelector::segmentBounds(int index) const
{
    const auto area = getLocalBounds().toFloat().reduced(juce::jmax(1.5f, static_cast<float>(getHeight()) * 0.11f),
                                                         juce::jmax(1.5f, static_cast<float>(getHeight()) * 0.13f));

    if (area.getWidth() <= 0.0f || maximumCount <= 0)
        return {};

    const float chipWidth = area.getWidth() / static_cast<float>(maximumCount);
    const float gap = juce::jmax(1.0f, chipWidth * 0.1f);

    const juce::Rectangle<float> chip(area.getX() + chipWidth * static_cast<float>(index), area.getY(), chipWidth,
                                      area.getHeight());

    return chip.reduced(gap * 0.5f, 0.0f);
}

int BandCountSelector::segmentAt(juce::Point<int> position) const
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() <= 0.0f || maximumCount <= 0)
        return -1;

    const float proportion = (static_cast<float>(position.x) - area.getX()) / area.getWidth();
    const int index = static_cast<int>(std::floor(proportion * static_cast<float>(maximumCount)));

    return juce::jlimit(0, maximumCount - 1, index);
}

void BandCountSelector::applyCount(int newCount, bool asCompleteGesture)
{
    const int clamped = juce::jlimit(minimumCount, maximumCount, newCount);

    if (clamped == currentCount)
        return;

    if (attachment != nullptr)
    {
        // A parameter notifies its listeners synchronously when the write comes
        // from the message thread, so this call lands straight back in
        // valueFromParameter(), which stores the count, repaints and announces
        // it. Falling through to do that again here would relay every single
        // band-count change to the editor twice — once per drag step.
        if (asCompleteGesture)
            attachment->setValueAsCompleteGesture(static_cast<float>(clamped));
        else
            attachment->setValueAsPartOfGesture(static_cast<float>(clamped));

        if (currentCount == clamped)
            return;
    }

    currentCount = clamped;
    repaint();

    if (onCountChanged != nullptr)
        onCountChanged(currentCount);
}

void BandCountSelector::finishDragGesture()
{
    if (!dragging)
        return;

    dragging = false;

    if (attachment != nullptr)
        attachment->endGesture();
}

void BandCountSelector::valueFromParameter(float denormalisedValue)
{
    const int newCount = juce::jlimit(minimumCount, maximumCount, juce::roundToInt(denormalisedValue));

    if (newCount == currentCount)
        return;

    currentCount = newCount;
    repaint();

    if (onCountChanged != nullptr)
        onCountChanged(currentCount);
}

void BandCountSelector::mouseDown(const juce::MouseEvent& e)
{
    if (attachment == nullptr)
        return;

    dragging = true;
    attachment->beginGesture();
    applyCount(segmentAt(e.getPosition()) + 1, false);
}

void BandCountSelector::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging)
        return;

    hoveredSegment = segmentAt(e.getPosition());
    applyCount(hoveredSegment + 1, false);
}

void BandCountSelector::mouseUp(const juce::MouseEvent&)
{
    finishDragGesture();
}

void BandCountSelector::mouseMove(const juce::MouseEvent& e)
{
    const int segment = segmentAt(e.getPosition());

    if (segment == hoveredSegment)
        return;

    hoveredSegment = segment;
    repaint();
}

void BandCountSelector::mouseExit(const juce::MouseEvent&)
{
    if (hoveredSegment < 0)
        return;

    hoveredSegment = -1;
    repaint();
}

void BandCountSelector::mouseDoubleClick(const juce::MouseEvent&)
{
    // The double-click follows the second mouse-down, whose gesture is still
    // open; the reset below writes a complete gesture of its own.
    finishDragGesture();

    if (parameter == nullptr)
        return;

    const auto& range = parameter->getNormalisableRange();
    applyCount(juce::roundToInt(range.convertFrom0to1(parameter->getDefaultValue())), true);
}

void BandCountSelector::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    finishDragGesture();

    wheelAccumulator += wheel.isReversed ? -wheel.deltaY : wheel.deltaY;

    if (std::abs(wheelAccumulator) < 0.2f)
        return;

    const int step = wheelAccumulator > 0.0f ? 1 : -1;
    wheelAccumulator = 0.0f;

    applyCount(currentCount + step, true);
}

void BandCountSelector::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (bounds.getWidth() < 8.0f || bounds.getHeight() < 6.0f)
        return;

    EmberLookAndFeel::drawWell(g, bounds, juce::jlimit(2.0f, 7.0f, bounds.getHeight() * 0.22f));

    // Every chip is the same height, so the two faces are built once rather
    // than once per chip.
    const float chipHeight = segmentBounds(0).getHeight();
    const auto litFont = EmberFonts::forHeight(chipHeight, 0.58f, true);
    const auto unlitFont = EmberFonts::forHeight(chipHeight, 0.58f, false);

    for (int i = 0; i < maximumCount; ++i)
    {
        const auto chip = segmentBounds(i);

        if (chip.getWidth() < 2.0f)
            continue;

        const bool lit = i < currentCount;
        const bool hovered = (hoveredSegment == i) && isEnabled();
        const float corner = juce::jmax(1.5f, chip.getHeight() * 0.22f);

        auto fill = lit ? EmberColours::bandDim(i) : EmberColours::panel();

        if (hovered)
            fill = fill.brighter(0.18f);

        g.setColour(fill);
        g.fillRoundedRectangle(chip, corner);

        if (lit)
        {
            g.setColour(EmberColours::band(i).withAlpha(hovered ? 1.0f : 0.85f));
            g.drawRoundedRectangle(chip.reduced(0.5f), corner, 1.0f);
        }
        else if (hovered)
        {
            g.setColour(EmberColours::outlineStrong());
            g.drawRoundedRectangle(chip.reduced(0.5f), corner, 1.0f);
        }

        g.setColour(lit ? EmberColours::textPrimary() : EmberColours::textDisabled());
        g.setFont(lit ? litFont : unlitFont);
        g.drawText(juce::String(i + 1), chip, juce::Justification::centred, false);
    }
}

//==============================================================================
LatencyReadout::LatencyReadout()
{
    // Mouse events are wanted only so the tooltip window finds this component.
    setInterceptsMouseClicks(true, false);
}

LatencyReadout::~LatencyReadout() = default;

void LatencyReadout::setLatency(int samples, double sampleRate)
{
    if (samples == latencySamples && std::abs(sampleRate - currentSampleRate) < 1.0e-6)
        return;

    latencySamples = samples;
    currentSampleRate = sampleRate;
    repaint();
}

void LatencyReadout::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (bounds.getWidth() < 8.0f || bounds.getHeight() < 6.0f)
        return;

    EmberLookAndFeel::drawWell(g, bounds, juce::jlimit(2.0f, 7.0f, bounds.getHeight() * 0.22f));

    auto inner = bounds.reduced(juce::jmax(3.0f, bounds.getWidth() * 0.06f), 1.0f);

    juce::String milliseconds("-- ms");

    if (currentSampleRate > 0.0)
    {
        const double ms = 1000.0 * static_cast<double>(latencySamples) / currentSampleRate;
        milliseconds = juce::String(ms, ms < 10.0 ? 2 : 1) + " ms";
    }

    const auto samplesText = juce::String(latencySamples) + " smp";
    const auto valueFont = EmberFonts::forHeight(inner.getHeight(), 0.58f, false);
    const auto unitFont = EmberFonts::forHeight(inner.getHeight(), 0.44f, false);

    const float samplesWidth = juce::GlyphArrangement::getStringWidth(unitFont, samplesText) + 3.0f;
    const float millisecondsWidth = juce::GlyphArrangement::getStringWidth(valueFont, milliseconds) + 3.0f;
    const bool roomForBoth = inner.getWidth() > samplesWidth + millisecondsWidth + 4.0f;

    if (roomForBoth)
    {
        auto right = inner.removeFromRight(samplesWidth);
        g.setFont(unitFont);
        g.setColour(EmberColours::textSecondary());
        g.drawText(samplesText, right, juce::Justification::centredRight, false);
    }

    g.setFont(valueFont);
    g.setColour(latencySamples > 0 ? EmberColours::textPrimary() : EmberColours::textSecondary());
    g.drawText(milliseconds, inner, roomForBoth ? juce::Justification::centredLeft : juce::Justification::centred,
               false);
}

//==============================================================================
MidiLearnBanner::MidiLearnBanner()
{
    cancelButton.onClick = [this]
    {
        if (onCancel != nullptr)
            onCancel();
    };

    cancelButton.setTooltip("Stop waiting for a MIDI message and leave the mapping as it is.");

    addAndMakeVisible(cancelButton);
    setInterceptsMouseClicks(true, true);
}

MidiLearnBanner::~MidiLearnBanner() = default;

void MidiLearnBanner::setTarget(const juce::String& parameterName, int existingCC)
{
    if (parameterName == targetName && existingCC == mappedCC)
        return;

    targetName = parameterName;
    mappedCC = existingCC;
    repaint();
}

void MidiLearnBanner::advancePulse()
{
    pulsePhase += 0.22f;

    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaint();
}

void MidiLearnBanner::setUiScale(float newScale)
{
    if (std::abs(newScale - uiScale) < 1.0e-3f)
        return;

    uiScale = newScale;
    resized();
    repaint();
}

void MidiLearnBanner::resized()
{
    auto area = getLocalBounds().reduced(juce::roundToInt(10.0f * uiScale), juce::roundToInt(6.0f * uiScale));

    const int buttonWidth = juce::jlimit(52, 120, juce::roundToInt(68.0f * uiScale));
    const int buttonHeight = juce::jlimit(16, 32, juce::jmin(area.getHeight(), juce::roundToInt(24.0f * uiScale)));

    auto right = area.removeFromRight(juce::jmin(buttonWidth, area.getWidth()));
    cancelButton.setBounds(right.withSizeKeepingCentre(right.getWidth(), buttonHeight));
}

void MidiLearnBanner::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);

    if (bounds.getWidth() < 24.0f || bounds.getHeight() < 12.0f)
        return;

    const float corner = juce::jlimit(3.0f, 12.0f, bounds.getHeight() * 0.12f);
    const float pulse = 0.5f + 0.5f * std::sin(pulsePhase);

    g.setColour(EmberColours::backgroundDeep().withAlpha(0.94f));
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(EmberColours::accent().withAlpha(0.3f + 0.55f * pulse));
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, juce::jmax(1.0f, 1.4f * uiScale));

    auto area = bounds.reduced(10.0f * uiScale, 6.0f * uiScale);

    // The lamp: a filled dot that breathes with the same pulse.
    const float lamp = juce::jlimit(6.0f, 16.0f, area.getHeight() * 0.3f);
    auto lampArea = area.removeFromLeft(lamp * 2.0f);

    g.setColour(EmberColours::accentGlow().withAlpha(0.15f + 0.35f * pulse));
    g.fillEllipse(juce::Rectangle<float>(lamp * 2.0f, lamp * 2.0f).withCentre(lampArea.getCentre()));
    g.setColour(EmberColours::accent().withAlpha(0.55f + 0.45f * pulse));
    g.fillEllipse(juce::Rectangle<float>(lamp, lamp).withCentre(lampArea.getCentre()));

    // Keep clear of the cancel button.
    area = area.withTrimmedRight(static_cast<float>(cancelButton.getWidth()) + 12.0f * uiScale);

    if (area.getWidth() < 24.0f)
        return;

    juce::String headline("MIDI Learn");

    if (targetName.isNotEmpty())
        headline << "  —  " << targetName;

    juce::String detail("Move a MIDI controller to map it.");

    if (mappedCC >= 0)
        detail << "  Currently CC " << mappedCC << ".";

    const bool twoLines = area.getHeight() >= 28.0f * uiScale;

    if (twoLines)
    {
        auto top = area.removeFromTop(area.getHeight() * 0.54f);

        g.setFont(EmberFonts::get(EmberFonts::Role::body, uiScale));
        g.setColour(EmberColours::textPrimary());
        g.drawFittedText(headline, top.toNearestInt(), juce::Justification::bottomLeft, 1, 0.75f);

        g.setFont(EmberFonts::get(EmberFonts::Role::micro, uiScale));
        g.setColour(EmberColours::textSecondary());
        g.drawFittedText(detail, area.toNearestInt(), juce::Justification::topLeft, 1, 0.75f);
    }
    else
    {
        g.setFont(EmberFonts::get(EmberFonts::Role::label, uiScale));
        g.setColour(EmberColours::textPrimary());
        g.drawFittedText(headline + "   " + detail, area.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
}

//==============================================================================
GlobalBar::GlobalBar(EmberAudioProcessor& processorToUse)
    : processor(processorToUse), inputKnob(processorToUse, pid::inputGain, "Input"),
      outputKnob(processorToUse, pid::outputGain, "Output"), mixKnob(processorToUse, pid::globalMix, "Mix"),
      bandCount(processorToUse, pid::numBands)
{
    optionalVisible.fill(true);

    auto& state = processor.getAPVTS();

    inputKnob.getKnob().setExtraTooltipText("Level into the crossover, before any saturation.");
    outputKnob.getKnob().setExtraTooltipText("Level after the bands are summed.");
    mixKnob.getKnob().setExtraTooltipText("Dry/wet blend across the whole plugin.");

    addAndMakeVisible(inputKnob);
    addAndMakeVisible(outputKnob);
    addAndMakeVisible(mixKnob);

    // The switch is captioned by the strip itself, like every other control in
    // the row, so it carries no text of its own.
    autoGainToggle.attachTo(state, pid::autoGain);
    autoGainToggle.setTooltip("Auto Gain\nCompensates the level the drive adds, so styles and drive "
                              "amounts can be compared without the loudest one simply winning.");
    addAndMakeVisible(autoGainToggle);

    bandCount.onCountChanged = [this](int newCount)
    {
        if (onBandCountChanged != nullptr)
            onBandCountChanged(newCount);
    };
    bandCount.setComponentID(tutorial::TourTargets::bandCount);
    addAndMakeVisible(bandCount);

    osRealtimeBox.attachTo(state, pid::osFactor);
    osRealtimeBox.setScrollWheelEnabled(true);
    osRealtimeBox.setTooltip("Oversampling — realtime\nUsed while you play. Higher factors reject more "
                             "aliasing and cost more CPU and latency.");
    osRealtimeBox.setComponentID(tutorial::TourTargets::oversampling);
    addAndMakeVisible(osRealtimeBox);

    osOfflineBox.attachTo(state, pid::osOffline);
    osOfflineBox.setScrollWheelEnabled(true);
    osOfflineBox.setTooltip("Oversampling — offline render\nUsed when the host bounces or freezes, where "
                            "CPU no longer matters. It can safely be higher than the realtime factor.");
    addAndMakeVisible(osOfflineBox);

    crossoverModeBox.attachTo(state, pid::xoverMode);
    crossoverModeBox.setScrollWheelEnabled(true);
    crossoverModeBox.setTooltip("Crossover mode\nMinimum phase is zero latency with a phase shift at each "
                                "edge; linear phase keeps the phase flat and adds latency.");
    crossoverModeBox.setComponentID(tutorial::TourTargets::crossoverMode);
    addAndMakeVisible(crossoverModeBox);

    stereoModeBox.attachTo(state, pid::stereoMode);
    stereoModeBox.setScrollWheelEnabled(true);
    stereoModeBox.setTooltip("Stereo mode\nStereo processes left and right; mid-side processes the centre "
                             "and the sides separately.");
    stereoModeBox.setComponentID(tutorial::TourTargets::stereoMode);
    addAndMakeVisible(stereoModeBox);

    latencyReadout.setTooltip("Latency reported to the host.");
    addAndMakeVisible(latencyReadout);

    slotAButton.setClickingTogglesState(false);
    slotBButton.setClickingTogglesState(false);
    slotAButton.setTooltip("Compare slot A.\nSwitching stores the current settings in the slot you leave.");
    slotBButton.setTooltip("Compare slot B.\nSwitching stores the current settings in the slot you leave.");
    slotAButton.onClick = [this] { handleSlotClicked(0); };
    slotBButton.onClick = [this] { handleSlotClicked(1); };
    slotAButton.setComponentID(tutorial::TourTargets::compareA);
    addAndMakeVisible(slotAButton);
    slotBButton.setComponentID(tutorial::TourTargets::compareB);
    addAndMakeVisible(slotBButton);

    copyButton.onClick = [this] { handleCopyClicked(); };
    addAndMakeVisible(copyButton);

    undoButton.onClick = [this] { handleUndoClicked(); };
    redoButton.onClick = [this] { handleRedoClicked(); };
    undoButton.setComponentID(tutorial::TourTargets::undo);
    addAndMakeVisible(undoButton);
    redoButton.setComponentID(tutorial::TourTargets::redo);
    addAndMakeVisible(redoButton);

    overflowButton.setTooltip("More global settings.");
    overflowButton.onClick = [this] { showOverflowMenu(); };
    presetButton.setComponentID(tutorial::TourTargets::presetsButton);
    modulationButton.setComponentID(tutorial::TourTargets::modButton);
    helpButton.setComponentID(tutorial::TourTargets::helpButton);
    helpButton.setTooltip("Tutorials & help");
    helpButton.onClick = [this]
    {
        if (onHelpRequested != nullptr)
            onHelpRequested();
    };
    addAndMakeVisible(helpButton);

    overflowButton.setComponentID(tutorial::TourTargets::overflow);
    addChildComponent(overflowButton);

    presetButton.setTooltip("Browse, save and manage presets.");
    presetButton.onClick = [this]
    {
        if (onPresetBrowserRequested != nullptr)
            onPresetBrowserRequested();
    };
    addChildComponent(presetButton);

    // Latched on, because a modulation panel starts open; an editor that hides
    // it to begin with corrects this with `setModulationPanelVisible`.
    modulationButton.setClickingTogglesState(true);
    modulationButton.setToggleState(true, juce::dontSendNotification);
    modulationButton.setTooltip("Show or hide the modulation sources and routings.");
    modulationButton.onClick = [this]
    {
        if (onModulationPanelToggled != nullptr)
            onModulationPanelToggled(modulationButton.getToggleState());
    };
    addChildComponent(modulationButton);

    midiBanner.onCancel = [this]
    {
        processor.cancelMidiLearn();
        refreshFromProcessor();
    };
    addChildComponent(midiBanner);

    refreshFromProcessor();
    startTimerHz(30);
}

GlobalBar::~GlobalBar()
{
    stopTimer();
}

int GlobalBar::preferredHeight(float uiScaleToUse)
{
    return juce::roundToInt(juce::jmax(48.0f, kReferenceBarHeight * uiScaleToUse));
}

int GlobalBar::scaled(float base, int minimum, int maximum) const
{
    return scaledSize(base, uiScale, minimum, maximum);
}

void GlobalBar::setModulationPanelVisible(bool shouldBeVisible)
{
    modulationButton.setToggleState(shouldBeVisible, juce::dontSendNotification);
}

bool GlobalBar::isModulationPanelVisible() const noexcept
{
    return modulationButton.getToggleState();
}

void GlobalBar::refreshFromProcessor()
{
    updateLatency();
    updateHistoryButtons();
    updateSlotButtons();
    updateMidiLearn();
}

void GlobalBar::lookAndFeelChanged()
{
    repaint();
}

//==============================================================================
void GlobalBar::setIoSectionVisible(bool shouldBeVisible)
{
    if (showIoSection == shouldBeVisible)
        return;

    showIoSection = shouldBeVisible;

    const std::initializer_list<juce::Component*> io{&inputKnob, &outputKnob, &mixKnob, &autoGainToggle};

    for (auto* component : io)
        component->setVisible(shouldBeVisible);

    resized();
}

void GlobalBar::timerCallback()
{
    refreshFromProcessor();

    if (copyFlashTicks > 0 && --copyFlashTicks == 0)
        copyButton.setToggleState(false, juce::dontSendNotification);
}

void GlobalBar::updateLatency()
{
    const int samples = processor.getLatencySamples();
    const double rate = processor.getSampleRate();

    if (samples == lastLatencySamples && std::abs(rate - lastSampleRate) < 1.0e-6)
        return;

    lastLatencySamples = samples;
    lastSampleRate = rate;

    latencyReadout.setLatency(samples, rate);
    latencyReadout.setTooltip("Latency reported to the host: " + latencyDescription() +
                              ".\nOversampling and the linear-phase crossover both add delay; the host "
                              "compensates for it.");
}

void GlobalBar::updateHistoryButtons()
{
    auto& undoManager = processor.getUndoManager();

    const bool canUndo = undoManager.canUndo();
    const bool canRedo = undoManager.canRedo();

    // Fetching the names is cheap — a reference-counted string copy — but
    // building a tooltip out of one allocates, so that only happens when the
    // history has actually moved. This runs 30 times a second.
    const auto undoName = canUndo ? undoManager.getUndoDescription() : juce::String();
    const auto redoName = canRedo ? undoManager.getRedoDescription() : juce::String();

    if (canUndo != lastCanUndo || undoName != lastUndoDescription)
    {
        lastCanUndo = canUndo;
        lastUndoDescription = undoName;

        undoButton.setEnabled(canUndo);

        // JUCE only has a description when a transaction was given a name, so
        // fall back to something honest rather than an empty tooltip.
        if (!canUndo)
            undoButton.setTooltip("Nothing to undo");
        else if (undoName.isNotEmpty())
            undoButton.setTooltip("Undo " + undoName);
        else
            undoButton.setTooltip("Undo the last change");
    }

    if (canRedo != lastCanRedo || redoName != lastRedoDescription)
    {
        lastCanRedo = canRedo;
        lastRedoDescription = redoName;

        redoButton.setEnabled(canRedo);

        if (!canRedo)
            redoButton.setTooltip("Nothing to redo");
        else if (redoName.isNotEmpty())
            redoButton.setTooltip("Redo " + redoName);
        else
            redoButton.setTooltip("Redo the last undone change");
    }
}

void GlobalBar::updateSlotButtons()
{
    const int slot = processor.getActiveSlot();

    if (slot == lastActiveSlot)
        return;

    lastActiveSlot = slot;

    slotAButton.setToggleState(slot == 0, juce::dontSendNotification);
    slotBButton.setToggleState(slot != 0, juce::dontSendNotification);

    copyButton.setTooltip(slot == 0 ? "Copy the current settings into slot B."
                                    : "Copy the current settings into slot A.");
}

void GlobalBar::updateMidiLearn()
{
    const bool learning = processor.isMidiLearning();

    if (learning != lastMidiLearning)
    {
        lastMidiLearning = learning;
        midiBanner.setVisible(learning);

        if (learning)
            midiBanner.toFront(false);
        else
            lastMidiLearnParameter.clear();
    }

    if (!learning)
        return;

    const auto learnID = processor.getMidiLearnParameterID();

    if (learnID != lastMidiLearnParameter)
    {
        lastMidiLearnParameter = learnID;

        juce::String displayName = learnID;

        if (auto* learnParameter = processor.getAPVTS().getParameter(learnID))
            displayName = learnParameter->getName(40);

        midiBanner.setTarget(displayName, processor.getMidiCCForParameter(learnID));
    }

    midiBanner.advancePulse();
}

//==============================================================================
void GlobalBar::handleSlotClicked(int slot)
{
    if (processor.getActiveSlot() == slot)
        return;

    processor.setActiveSlot(slot);
    updateSlotButtons();
    stateWasReplaced();
}

void GlobalBar::handleCopyClicked()
{
    processor.copyCurrentSlotToOther();

    // A short confirmation flash: there is nothing else on screen that would
    // otherwise tell the user the copy happened.
    copyFlashTicks = 12;
    copyButton.setToggleState(true, juce::dontSendNotification);
}

void GlobalBar::handleUndoClicked()
{
    auto& undoManager = processor.getUndoManager();

    if (!undoManager.canUndo())
        return;

    undoManager.undo();
    updateHistoryButtons();
    stateWasReplaced();
}

void GlobalBar::handleRedoClicked()
{
    auto& undoManager = processor.getUndoManager();

    if (!undoManager.canRedo())
        return;

    undoManager.redo();
    updateHistoryButtons();
    stateWasReplaced();
}

void GlobalBar::stateWasReplaced()
{
    if (onStateReplaced != nullptr)
        onStateReplaced();
}

//==============================================================================
juce::String GlobalBar::latencyDescription() const
{
    const int samples = processor.getLatencySamples();
    const double rate = processor.getSampleRate();

    juce::String text;

    if (rate > 0.0)
    {
        const double ms = 1000.0 * static_cast<double>(samples) / rate;
        text << juce::String(ms, ms < 10.0 ? 2 : 1) << " ms";
    }
    else
    {
        text << "-- ms";
    }

    text << " (" << samples << (samples == 1 ? " sample)" : " samples)");

    return text;
}

void GlobalBar::addChoiceSubMenu(juce::PopupMenu& menu, const juce::String& title, const juce::String& parameterIDToUse)
{
    auto* choiceParameter = processor.getAPVTS().getParameter(parameterIDToUse);

    if (choiceParameter == nullptr)
        return;

    const auto choices = choiceParameter->getAllValueStrings();

    if (choices.isEmpty())
        return;

    const int current = juce::jlimit(0, choices.size() - 1, currentChoiceIndex(*choiceParameter));

    juce::PopupMenu sub;

    for (int i = 0; i < choices.size(); ++i)
    {
        sub.addItem(choices[i], true, i == current,
                    [safeThis = juce::Component::SafePointer<GlobalBar>(this), parameterIDToUse, i]
                    {
                        if (auto* bar = safeThis.getComponent())
                            bar->setChoiceParameter(parameterIDToUse, i);
                    });
    }

    menu.addSubMenu(title + ":  " + choices[current], sub, true);
}

void GlobalBar::setChoiceParameter(const juce::String& parameterIDToUse, int choiceIndex)
{
    auto* choiceParameter = processor.getAPVTS().getParameter(parameterIDToUse);

    if (choiceParameter == nullptr)
        return;

    const auto& range = choiceParameter->getNormalisableRange();
    const float denormalised = juce::jlimit(range.start, range.end, static_cast<float>(choiceIndex));

    processor.getUndoManager().beginNewTransaction();
    choiceParameter->beginChangeGesture();
    choiceParameter->setValueNotifyingHost(choiceParameter->convertTo0to1(denormalised));
    choiceParameter->endChangeGesture();
}

void GlobalBar::showOverflowMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());

    if (!shows(Optional::osRealtime))
        addChoiceSubMenu(menu, "Oversampling (realtime)", pid::osFactor);

    if (!shows(Optional::osOffline))
        addChoiceSubMenu(menu, "Oversampling (offline render)", pid::osOffline);

    if (!shows(Optional::crossoverMode))
        addChoiceSubMenu(menu, "Crossover mode", pid::xoverMode);

    if (!shows(Optional::stereoMode))
        addChoiceSubMenu(menu, "Stereo mode", pid::stereoMode);

    if (!shows(Optional::latency))
    {
        menu.addSeparator();

        juce::PopupMenu::Item item("Latency:  " + latencyDescription());
        item.setEnabled(false);
        menu.addItem(item);
    }

    // ---- view: zoom and appearance ---------------------------------------
    // The design wants zoom and settings as their own header items. At 860 px
    // there is no room for two more controls without pushing a real one out,
    // so they live here - reachable at every size, which matters more than
    // being one click away at the widest one.
    menu.addSeparator();

    juce::PopupMenu zoom;

    for (const int percent : {75, 100, 125, 150, 200})
        zoom.addItem(juce::String(percent) + " %", true, false,
                     [this, percent]
                     {
                         if (onZoomRequested)
                             onZoomRequested(static_cast<float>(percent) * 0.01f);
                     });

    menu.addSubMenu("Zoom", zoom);

    juce::PopupMenu appearance;

    appearance.addItem("Ember", true, EmberTheme::variant() == ThemeVariant::emberDefault,
                       [this]
                       {
                           EmberTheme::setVariant(ThemeVariant::emberDefault);
                           if (onAppearanceChanged)
                               onAppearanceChanged();
                       });

    appearance.addItem("Cool Ember", true, EmberTheme::variant() == ThemeVariant::coolEmber,
                       [this]
                       {
                           EmberTheme::setVariant(ThemeVariant::coolEmber);
                           if (onAppearanceChanged)
                               onAppearanceChanged();
                       });

    appearance.addSeparator();

    appearance.addItem("Reduce motion", true, EmberTheme::reduceMotion(),
                       [this]
                       {
                           EmberTheme::setReduceMotion(!EmberTheme::reduceMotion());
                           if (onAppearanceChanged)
                               onAppearanceChanged();
                       });

    menu.addSubMenu("Appearance", appearance);

    if (menu.getNumItems() == 0)
        return;

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(&overflowButton)
                           .withMinimumWidth(juce::roundToInt(200.0f * uiScale)));
}

//==============================================================================
void GlobalBar::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (bounds.getWidth() < 4.0f || bounds.getHeight() < 4.0f)
        return;

    const float corner = juce::jlimit(3.0f, 12.0f, bounds.getHeight() * 0.12f);
    EmberLookAndFeel::drawPanel(g, bounds, corner, false);

    for (const auto x : dividers)
    {
        const juce::Rectangle<float> line(static_cast<float>(x) - 0.5f, bounds.getY() + bounds.getHeight() * 0.2f, 1.0f,
                                          bounds.getHeight() * 0.6f);
        EmberLookAndFeel::drawHairline(g, line, EmberColours::outline(), true);
    }

    if (captions.empty())
        return;

    g.setFont(EmberFonts::get(EmberFonts::Role::micro, uiScale));
    g.setColour(EmberColours::textDisabled());

    for (const auto& caption : captions)
        g.drawFittedText(caption.text, caption.bounds,
                         caption.centred ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.65f);
}

//==============================================================================
void GlobalBar::resized()
{
    captions.clear();
    dividers.clear();

    const auto bounds = getLocalBounds();

    if (bounds.isEmpty())
        return;

    uiScale = juce::jlimit(0.72f, 2.0f, static_cast<float>(bounds.getHeight()) / kReferenceBarHeight);

    midiBanner.setUiScale(uiScale);
    midiBanner.setBounds(bounds);

    // ---------------------------------------------------------------- metrics
    const int padH = scaled(12.0f, 5, 26);
    const int padV = scaled(7.0f, 2, 18);

    auto content = bounds.reduced(padH, padV);

    if (content.getWidth() < 16 || content.getHeight() < 10)
        return;

    const int captionH = scaled(11.0f, 8, 18);
    const int captionGap = scaled(3.0f, 2, 6);
    const int controlH = juce::jmin(content.getHeight(), scaled(23.0f, 14, 34));
    const int tightGap = scaled(2.0f, 1, 5);
    const int itemGap = scaled(6.0f, 3, 12);
    const int groupGap = scaled(13.0f, 7, 24);

    const int knobW = scaled(50.0f, 34, 74);
    const int chipW = scaled(18.0f, 11, 28);
    const int bandsW = chipW * kMaxBands + scaled(5.0f, 3, 9);
    const int osW = scaled(54.0f, 40, 82);
    const int xoverW = scaled(92.0f, 62, 130);
    const int stereoW = scaled(76.0f, 52, 106);
    const int latencyW = scaled(94.0f, 62, 132);
    const int slotW = scaled(24.0f, 17, 36);
    const int iconW = scaled(26.0f, 18, 38);
    const int presetsW = scaled(60.0f, 42, 90);
    const int modW = scaled(44.0f, 30, 66);

    // Wide enough for the switch itself (measured, since only the toggle knows
    // how big its track is at this height) and for the caption above it.
    autoGainToggle.setSize(iconW, controlH);
    const int autoW = juce::jmax(autoGainToggle.preferredWidth(), scaled(50.0f, 34, 74));

    const int levelsW = 3 * knobW + 2 * tightGap + itemGap + autoW;

    // ------------------------------------------------------- what fits where
    const bool showPresets = (onPresetBrowserRequested != nullptr);
    const bool showModulation = (onModulationPanelToggled != nullptr);

    presetButton.setVisible(showPresets);
    modulationButton.setVisible(showModulation);

    const int compareW = 2 * slotW + tightGap + itemGap + iconW;
    const int historyW = 2 * iconW + tightGap;
    const int sessionW = compareW + groupGap + historyW;

    int toolsW = showPresets ? presetsW : 0;

    if (showModulation)
        toolsW += (toolsW > 0 ? tightGap : 0) + modW;

    const int rightW = sessionW + (toolsW > 0 ? groupGap + toolsW : 0);
    const int leftMandatoryW = levelsW + groupGap + bandsW;

    struct Candidate
    {
        Optional item;
        int width;
        int group; // 0 = oversampling, 1 = routing, 2 = its own
    };

    const Candidate candidates[] = {
        {Optional::latency, latencyW, 2},   {Optional::osRealtime, osW, 0}, {Optional::crossoverMode, xoverW, 1},
        {Optional::stereoMode, stereoW, 1}, {Optional::osOffline, osW, 0},
    };

    int leftOver = 0;

    const auto fitOptional = [&](int budget)
    {
        std::array<bool, static_cast<size_t>(kNumOptional)> visible{};
        visible.fill(false);

        bool groupStarted[3] = {false, false, false};
        int remaining = budget;

        for (const auto& candidate : candidates)
        {
            const auto groupIndex = static_cast<size_t>(candidate.group);
            const int cost = candidate.width + (groupStarted[groupIndex] ? itemGap : groupGap);

            if (cost > remaining)
                continue;

            remaining -= cost;
            groupStarted[groupIndex] = true;
            visible[static_cast<size_t>(candidate.item)] = true;
        }

        leftOver = juce::jmax(0, remaining);

        return visible;
    };

    const int fullBudget = content.getWidth() - leftMandatoryW - rightW - groupGap;

    optionalVisible = fitOptional(fullBudget);

    const auto anythingHidden = [this]
    { return std::find(optionalVisible.begin(), optionalVisible.end(), false) != optionalVisible.end(); };

    if (anythingHidden())
        optionalVisible = fitOptional(fullBudget - iconW - groupGap);

    const bool needOverflow = anythingHidden();

    // Whatever is left over after packing is shared out between the groups on
    // the left, up to one extra gap each: a wide window then spreads rather
    // than bunching everything against the edge with a void in the middle.
    const int leftGroups = 2 + (shows(Optional::osRealtime) || shows(Optional::osOffline) ? 1 : 0) +
                           (shows(Optional::crossoverMode) || shows(Optional::stereoMode) ? 1 : 0) +
                           (shows(Optional::latency) ? 1 : 0) + (needOverflow ? 1 : 0);

    const int spreadGap = groupGap + juce::jlimit(0, groupGap, leftOver / juce::jmax(1, leftGroups));

    overflowButton.setVisible(needOverflow);
    osRealtimeBox.setVisible(shows(Optional::osRealtime));
    osOfflineBox.setVisible(shows(Optional::osOffline));
    crossoverModeBox.setVisible(shows(Optional::crossoverMode));
    stereoModeBox.setVisible(shows(Optional::stereoMode));
    latencyReadout.setVisible(shows(Optional::latency));

    // ----------------------------------------------------------- placement
    // Every control row sits at the same height whether or not its group has a
    // caption, so the strip reads as one line rather than a ransom note.
    const auto controlRow = [&](juce::Rectangle<int> cell, const juce::String& caption)
    {
        const bool tall = cell.getHeight() >= captionH + captionGap + controlH;
        const int blockH = tall ? captionH + captionGap + controlH : juce::jmin(cell.getHeight(), controlH);

        auto block = cell.withSizeKeepingCentre(cell.getWidth(), blockH);

        if (tall)
        {
            auto captionArea = block.removeFromTop(captionH);

            // Upper-cased here rather than in paint(): the banner is not opaque,
            // so a MIDI-learn pulse repaints the strip underneath it at 30 Hz.
            if (caption.isNotEmpty())
                captions.push_back({captionArea, caption.toUpperCase(), false});

            block.removeFromTop(captionGap);
        }

        return block;
    };

    auto rightArea = content.removeFromRight(juce::jmin(rightW, content.getWidth()));

    bool leftStarted = false;

    const auto nextLeft = [&](int width, bool startsGroup)
    {
        if (leftStarted)
        {
            auto gap = content.removeFromLeft(juce::jmin(startsGroup ? spreadGap : itemGap, content.getWidth()));

            if (startsGroup)
                dividers.push_back(gap.getCentreX());
        }

        leftStarted = true;

        return content.removeFromLeft(juce::jmin(width, content.getWidth()));
    };

    // Levels: the knobs normally take the full height and caption themselves.
    // In a short strip they drop their own caption, so the row supplies one —
    // three unlabelled knobs would be a puzzle.
    //
    // The redesign moves Input / Output / Mix / Auto-Gain to the footer, where
    // they sit beside the CPU and latency readouts. When the footer owns them
    // this block is skipped entirely rather than drawing a second copy.
    if (showIoSection)
    {
        auto cell = nextLeft(levelsW, true);
        const bool knobsNeedCaption = cell.getHeight() < 52;

        const auto placeKnob = [&](LabelledKnob& knob, const juce::String& caption)
        {
            auto column = cell.removeFromLeft(juce::jmin(knobW, cell.getWidth()));

            if (knobsNeedCaption && column.getHeight() > captionH + 16)
                captions.push_back({column.removeFromTop(captionH), caption.toUpperCase(), true});

            knob.setBounds(column);
        };

        placeKnob(inputKnob, "In");
        cell.removeFromLeft(tightGap);
        placeKnob(outputKnob, "Out");
        cell.removeFromLeft(tightGap);
        placeKnob(mixKnob, "Mix");
        cell.removeFromLeft(itemGap);

        autoGainToggle.setBounds(controlRow(cell, "Auto Gain"));
    }

    bandCount.setBounds(controlRow(nextLeft(bandsW, true), "Bands"));

    if (shows(Optional::osRealtime) || shows(Optional::osOffline))
    {
        const bool both = shows(Optional::osRealtime) && shows(Optional::osOffline);
        auto cell = nextLeft(osW * (both ? 2 : 1) + (both ? itemGap : 0), true);

        if (shows(Optional::osRealtime))
        {
            osRealtimeBox.setBounds(controlRow(cell.removeFromLeft(juce::jmin(osW, cell.getWidth())), "Realtime OS"));

            if (both)
                cell.removeFromLeft(itemGap);
        }

        if (shows(Optional::osOffline))
            osOfflineBox.setBounds(controlRow(cell.removeFromLeft(juce::jmin(osW, cell.getWidth())), "Offline OS"));
    }

    if (shows(Optional::crossoverMode))
        crossoverModeBox.setBounds(controlRow(nextLeft(xoverW, true), "Crossover"));

    if (shows(Optional::stereoMode))
        stereoModeBox.setBounds(controlRow(nextLeft(stereoW, !shows(Optional::crossoverMode)), "Stereo"));

    if (shows(Optional::latency))
        latencyReadout.setBounds(controlRow(nextLeft(latencyW, true), "Latency"));

    if (needOverflow)
        overflowButton.setBounds(controlRow(nextLeft(iconW, true), {}));

    // The right-hand cluster: compare, history, and the panel buttons.
    if (leftStarted && rightArea.getX() > bounds.getX() + groupGap)
        dividers.push_back(rightArea.getX() - groupGap / 2);

    {
        auto row = controlRow(rightArea.removeFromLeft(juce::jmin(compareW, rightArea.getWidth())), "Compare");

        slotAButton.setBounds(row.removeFromLeft(juce::jmin(slotW, row.getWidth())));
        row.removeFromLeft(tightGap);
        slotBButton.setBounds(row.removeFromLeft(juce::jmin(slotW, row.getWidth())));
        row.removeFromLeft(itemGap);
        copyButton.setBounds(row.removeFromLeft(juce::jmin(iconW, row.getWidth())));
    }

    {
        auto gap = rightArea.removeFromLeft(juce::jmin(groupGap, rightArea.getWidth()));
        dividers.push_back(gap.getCentreX());

        auto row = controlRow(rightArea.removeFromLeft(juce::jmin(historyW, rightArea.getWidth())), "History");

        undoButton.setBounds(row.removeFromLeft(juce::jmin(iconW, row.getWidth())));
        row.removeFromLeft(tightGap);
        redoButton.setBounds(row.removeFromLeft(juce::jmin(iconW, row.getWidth())));
    }

    if (toolsW > 0)
    {
        auto gap = rightArea.removeFromLeft(juce::jmin(groupGap, rightArea.getWidth()));
        dividers.push_back(gap.getCentreX());

        auto row = controlRow(rightArea.removeFromLeft(juce::jmin(toolsW, rightArea.getWidth())), {});

        if (showPresets)
        {
            presetButton.setBounds(row.removeFromLeft(juce::jmin(presetsW, row.getWidth())));

            if (showModulation)
                row.removeFromLeft(tightGap);
        }

        if (showModulation)
            modulationButton.setBounds(row.removeFromLeft(juce::jmin(modW, row.getWidth())));

        // The "?" is last and smallest: it is the control you need once and
        // then stop needing, so it yields space to everything else.
        if (row.getWidth() > 4)
        {
            row.removeFromLeft(juce::jmin(tightGap, row.getWidth()));
            helpButton.setBounds(row.removeFromLeft(juce::jmin(row.getHeight(), row.getWidth())));
            helpButton.setVisible(helpButton.getWidth() > 12);
        }
        else
        {
            helpButton.setVisible(false);
        }
    }
}
} // namespace ember::gui
