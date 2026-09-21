#include "gui/BandPanel.h"
#include "gui/EmberTheme.h"

#include "dsp/styles/SaturationStyle.h"
#include "plugin/ParameterIDs.h"

#include <algorithm>
#include <initializer_list>
#include <cmath>

namespace ember::gui
{
namespace
{
/** std::array wants a size_t; every index here is a checked int. */
constexpr size_t idx(int index) noexcept
{
    return static_cast<size_t>(index);
}

/** Each group's share of the width of the row it sits in. Drive's group is the
    widest because the drive knob is the panel's dominant control. */
constexpr float kGroupWeights[5] = {3.8f, 2.0f, 2.0f, 1.7f, 3.0f};

/** Below this `layoutGroups` cannot place a group at all, so `resized` reserves
    it for the knobs before the header and the style row take their share. */
constexpr int kMinimumGroupsHeight = 40;

/** The shortest header that still draws its two lines of text: `layoutHeader`
    insets 5 px top and bottom and `paint` wants more than 12 px left over. */
constexpr int kMinimumHeaderHeight = 26;

/** The smallest dynamics knob worth drawing. Below this the scale and the typed
    readout give up their rows rather than squeezing the knob out of existence. */
constexpr int kMinimumKnobSide = 20;

/** "120 Hz", "1.20 kHz", "12.0 kHz" — three significant figures, always. */
juce::String formatFrequency(float hz)
{
    if (hz >= 10000.0f)
        return juce::String(hz / 1000.0f, 1) + " kHz";

    if (hz >= 1000.0f)
        return juce::String(hz / 1000.0f, 2) + " kHz";

    return juce::String(juce::roundToInt(hz)) + " Hz";
}

/** The en dash used between the two edges of a band's frequency span. */
juce::String enDash()
{
    return juce::String::fromUTF8("\xe2\x80\x93");
}

/** Fills a combo box with all 19 styles, grouped under their category name.

    Section headings carry item ID 0, which `ComboBox::getNumItems` and
    `getItemForIndex` both skip, so the item indices stay exactly the choice
    parameter's own indices and a plain `ComboBoxAttachment` maps them
    one-to-one. That is why the box is populated here rather than through
    `EmberComboBox::attachTo`, which builds a flat list. */
void populateStyleBox(juce::ComboBox& box)
{
    box.clear(juce::dontSendNotification);

    int lastCategory = -1;

    for (int i = 0; i < kNumStyles; ++i)
    {
        const auto styleId = static_cast<StyleID>(i);
        const auto category = getStyleCategory(styleId);

        if (static_cast<int>(category) != lastCategory)
        {
            box.addSectionHeading(juce::String(getCategoryName(category)).toUpperCase());
            lastCategory = static_cast<int>(category);
        }

        box.addItem(juce::String(getStyleName(styleId)), i + 1);
    }
}
} // namespace

//==============================================================================
/**
    A knob that snaps to a centre value while being dragged.

    Dynamics is bipolar and its "off" position is exactly zero, which is
    otherwise the one value a continuous rotary makes hard to hit. The detent is
    drag-only: typing a value or a modulation offset passes through untouched.
*/
class BandPanel::DetentedKnob final : public ModulatableKnob
{
public:
    DetentedKnob(EmberAudioProcessor& processorToUse, const juce::String& parameterIDToUse, double centre)
        : ModulatableKnob(processorToUse, parameterIDToUse), centreValue(centre)
    {
    }

    double snapValue(double attemptedValue, DragMode dragMode) override
    {
        if (dragMode == juce::Slider::notDragging)
            return attemptedValue;

        const double tolerance = (getMaximum() - getMinimum()) * 0.018;

        return std::abs(attemptedValue - centreValue) <= tolerance ? centreValue : attemptedValue;
    }

private:
    double centreValue{0.0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DetentedKnob)
};

//==============================================================================
/**
    The band's dynamics stage: one bipolar, centre-detented knob, the reduction
    the band is actually producing, and a scale that says which way is which.

    The scale highlights the side the control is on, so "negative expands,
    positive compresses" is readable without a manual.
*/
class BandPanel::DynamicsControl final : public juce::Component
{
public:
    DynamicsControl(BandPanel& owner, EmberAudioProcessor& processorToUse, int bandIndex)
        : knob(processorToUse, pid::dynamics(bandIndex), 0.0),
          valueLabel(processorToUse.getAPVTS(), pid::dynamics(bandIndex)),
          reduction(LevelMeter::Mode::gainReduction, LevelMeter::Orientation::vertical)
    {
        // The readout gives up its row when the control gets short (see
        // resized), and there is nothing to type into once it has.
        knob.onEditValueRequested = [this]
        {
            if (valueLabel.isVisible())
                valueLabel.beginEditing();
        };

        // Only the expand/compress scale reads the value, so repaint that strip
        // rather than dragging the knob, the readout and the meter through
        // paint on every step of a drag.
        knob.onValueChange = [this] { repaint(scaleArea.isEmpty() ? getLocalBounds() : scaleArea); };

        owner.wireKnob(knob, "Below centre expands and gates, above centre compresses. Centre is off.");

        // The engine reports reduction as a negative gain in dB; the meter wants
        // positive decibels of reduction.
        //
        // `processorToUse` is a reference PARAMETER: capturing it by reference
        // would leave this lambda holding a reference whose lifetime ends when
        // the constructor returns, and the meter's timer calls it long after
        // that. Capture the processor's address instead.
        reduction.setSource([processorPtr = &processorToUse, bandIndex]
                            { return juce::jmax(0.0f, -processorPtr->getBandGainReductionDb(bandIndex)); });
        reduction.setMaxGainReductionDb(18.0f);

        addAndMakeVisible(knob);
        addAndMakeVisible(valueLabel);
        addAndMakeVisible(reduction);

        setAccentColour(EmberColours::band(bandIndex));
    }

    ModulatableKnob& getKnob() noexcept { return knob; }

    void setAccentColour(juce::Colour colour)
    {
        EmberStyleProps::setAccentColour(*this, colour);
        EmberStyleProps::setAccentColour(knob, colour);
        EmberStyleProps::setAccentColour(valueLabel, colour);
        EmberStyleProps::setAccentColour(reduction, colour);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();

        if (area.getWidth() < 12 || area.getHeight() < 12)
            return;

        const float height = static_cast<float>(area.getHeight());

        // The reduction meter takes a slim column on the right, with room for
        // its caption underneath.
        const int meterWidth = juce::jlimit(10, 26, juce::roundToInt(static_cast<float>(area.getWidth()) * 0.20f));
        auto meterColumn = area.removeFromRight(meterWidth);
        const int captionHeight = juce::jlimit(8, 15, juce::roundToInt(height * 0.15f));
        meterCaptionArea = meterColumn.removeFromBottom(captionHeight);
        reduction.setBounds(meterColumn.reduced(juce::jmax(1, meterWidth / 6), 1));

        area.removeFromRight(juce::jmax(2, meterWidth / 3));

        // "expand / compress" scale under the knob, then the typed readout.
        //
        // Both have floor heights that do NOT shrink with the control, so below
        // about 45 px they took the whole column and the knob — the actual
        // control — was left on a zero-size rectangle. Give up the readout and
        // then the scale instead, the way LabelledKnob gives up its caption and
        // its readout to keep its knob.
        int scaleHeight = juce::jlimit(9, 17, juce::roundToInt(height * 0.15f));
        int valueHeight = juce::jlimit(12, 22, juce::roundToInt(height * 0.19f));

        if (area.getHeight() - scaleHeight - valueHeight < kMinimumKnobSide)
            valueHeight = 0;

        if (area.getHeight() - scaleHeight - valueHeight < kMinimumKnobSide)
            scaleHeight = 0;

        scaleArea = scaleHeight > 0 ? area.removeFromBottom(scaleHeight) : juce::Rectangle<int>();

        valueLabel.setVisible(valueHeight > 0);

        if (valueHeight > 0)
        {
            auto valueBounds = area.removeFromBottom(valueHeight);
            valueLabel.setFont(EmberFonts::forHeight(static_cast<float>(valueHeight), 0.82f, false));
            valueLabel.setBounds(valueBounds);
        }

        const int side = juce::jmax(0, juce::jmin(area.getWidth(), area.getHeight()));
        knob.setBounds(juce::Rectangle<int>(side, side).withCentre(area.getCentre()));

        // Fonts for paint(), built here rather than once per frame.
        scaleFont =
            EmberFonts::forHeight(juce::jmax(1.0f, static_cast<float>(scaleArea.getHeight()) - 3.0f), 0.92f, false);
        captionFont =
            EmberFonts::forHeight(juce::jmax(1.0f, static_cast<float>(meterCaptionArea.getHeight())), 0.86f, false);
    }

    void paint(juce::Graphics& g) override
    {
        const auto accent = EmberStyleProps::accentColourFor(*this);
        const double value = knob.getValue();
        const bool expanding = value < -0.5;
        const bool compressing = value > 0.5;

        if (scaleArea.getWidth() > 24 && scaleArea.getHeight() > 6)
        {
            const auto strip = scaleArea.toFloat();
            const float lineY = strip.getY() + 1.5f;

            g.setColour(EmberColours::outline);
            g.drawLine(strip.getX(), lineY, strip.getRight(), lineY, 1.0f);

            g.setColour(EmberColours::outlineStrong);
            g.drawLine(strip.getCentreX(), lineY - 2.5f, strip.getCentreX(), lineY + 2.5f, 1.2f);

            const auto textArea = strip.withTrimmedTop(3.0f);
            g.setFont(scaleFont);

            // At the minimum window size the two words meet in the middle and
            // read as one. Fall back to the arrows, which carry the same meaning
            // in the space available, and drop them entirely if even that will
            // not fit.
            const float needed = juce::GlyphArrangement::getStringWidth(scaleFont, "EXPAND") +
                                 juce::GlyphArrangement::getStringWidth(scaleFont, "COMPRESS") + 10.0f;

            if (textArea.getWidth() >= needed)
            {
                g.setColour(expanding ? accent : EmberColours::textDisabled);
                g.drawText("EXPAND", textArea, juce::Justification::centredLeft, false);

                g.setColour(compressing ? accent : EmberColours::textDisabled);
                g.drawText("COMPRESS", textArea, juce::Justification::centredRight, false);
            }
            else if (textArea.getWidth() >= 28.0f)
            {
                g.setColour(expanding ? accent : EmberColours::textDisabled);
                g.drawText(juce::String::fromUTF8("\xe2\x86\x90"), textArea, juce::Justification::centredLeft, false);

                g.setColour(compressing ? accent : EmberColours::textDisabled);
                g.drawText(juce::String::fromUTF8("\xe2\x86\x92"), textArea, juce::Justification::centredRight, false);
            }
        }

        if (meterCaptionArea.getHeight() > 6)
        {
            g.setColour(EmberColours::textDisabled);
            g.setFont(captionFont);
            g.drawText("GR", meterCaptionArea, juce::Justification::centredTop, false);
        }
    }

private:
    DetentedKnob knob;
    ParameterValueLabel valueLabel;
    LevelMeter reduction;

    juce::Rectangle<int> scaleArea, meterCaptionArea;

    // Built by resized() from the areas above, never inside paint().
    juce::Font scaleFont{juce::FontOptions{}};
    juce::Font captionFont{juce::FontOptions{}};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsControl)
};

//==============================================================================
/**
    One band's complete control set, already attached to that band's parameters.

    Constructed lazily, once per band, and never rebound: switching bands hides
    one of these and shows another.
*/
struct BandPanel::BandControls
{
    BandControls(BandPanel& owner, EmberAudioProcessor& processorToUse, int bandIndex)
        : drive(processorToUse, pid::drive(bandIndex), "Drive"), mix(processorToUse, pid::bandMix(bandIndex), "Mix"),
          level(processorToUse, pid::level(bandIndex), "Level"), pan(processorToUse, pid::pan(bandIndex), "Pan"),
          width(processorToUse, pid::width(bandIndex), "Width"),
          feedbackAmount(processorToUse, pid::feedback(bandIndex), "Amount"),
          feedbackFrequency(processorToUse, pid::feedbackFreq(bandIndex), "Freq"),
          toneLow(processorToUse, pid::toneLow(bandIndex), "Low"),
          toneMid(processorToUse, pid::toneMid(bandIndex), "Mid"),
          toneHigh(processorToUse, pid::toneHigh(bandIndex), "High"), dynamics(owner, processorToUse, bandIndex),
          bypass("BYPASS", ToggleLook::led), solo("SOLO", ToggleLook::led)
    {
        const auto colour = EmberColours::band(bandIndex);

        // ---- style picker, grouped by family ------------------------------
        styleValue = processorToUse.getAPVTS().getRawParameterValue(pid::style(bandIndex));

        populateStyleBox(styleBox);
        styleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processorToUse.getAPVTS(), pid::style(bandIndex), styleBox);

        styleBox.setTooltip("Saturation style for this band, grouped by family.");
        EmberStyleProps::setAccentColour(styleBox, colour);

        // ---- knobs ---------------------------------------------------------
        owner.wireKnob(drive.getKnob(), "How hard this band is pushed into its style.");
        owner.wireKnob(mix.getKnob(), "Dry / saturated blend inside this band.");
        owner.wireKnob(level.getKnob(), "Output trim for the band, after saturation.");
        owner.wireKnob(pan.getKnob(), "Position of the band in the stereo field.");
        owner.wireKnob(width.getKnob(), "Stereo width of the band: 0% mono, 200% wide.");
        owner.wireKnob(feedbackAmount.getKnob(), "How much of the band is fed back around the saturator.");
        owner.wireKnob(feedbackFrequency.getKnob(), "Centre frequency the feedback resonates at.");
        owner.wireKnob(toneLow.getKnob(), "Low shelf of the band's tone stack.");
        owner.wireKnob(toneMid.getKnob(), "Mid bell of the band's tone stack.");
        owner.wireKnob(toneHigh.getKnob(), "High shelf of the band's tone stack.");

        for (auto* knob :
             {&drive, &mix, &level, &pan, &width, &feedbackAmount, &feedbackFrequency, &toneLow, &toneMid, &toneHigh})
        {
            knob->setAccentColour(colour);
            allKnobs.add(&knob->getKnob());
        }

        allKnobs.add(&dynamics.getKnob());

        // ---- band state ----------------------------------------------------
        bypass.attachTo(processorToUse.getAPVTS(), pid::bypass(bandIndex));
        bypass.setTooltip("Bypass this band: it passes through unprocessed.");
        EmberStyleProps::setAccentColour(bypass, EmberColours::warning);

        solo.attachTo(processorToUse.getAPVTS(), pid::solo(bandIndex));
        solo.setTooltip("Solo this band: every other band is muted.");
        EmberStyleProps::setAccentColour(solo, colour);

        const std::initializer_list<juce::Component*> everything{
            &styleBox,          &drive,    &mix,     &level,   &pan,      &width,  &feedbackAmount,
            &feedbackFrequency, &dynamics, &toneLow, &toneMid, &toneHigh, &bypass, &solo};

        for (auto* component : everything)
        {
            allComponents.add(component);
            owner.addChildComponent(component);
        }
    }

    void setControlsVisible(bool shouldBeVisible)
    {
        for (auto* component : allComponents)
            component->setVisible(shouldBeVisible);
    }

    /** Compact modules keep only Drive.

        An unselected module is a few dozen pixels wide once six bands are up;
        everything else would be illegible rather than merely small, and the
        point of showing it at all is "which bands are doing something". Drive
        plus the heat bar answers that. */
    void setCompact()
    {
        for (auto* component : allComponents)
            component->setVisible(component == &drive);
    }

    void setPollingEnabled(bool shouldPoll)
    {
        for (auto* knob : allKnobs)
            knob->setModulationPollingEnabled(shouldPoll);
    }

    void refreshModulation()
    {
        for (auto* knob : allKnobs)
            knob->refreshModulationDisplay();
    }

    /** The selected style, as an index into `StyleID`. */
    int currentStyleIndex() const
    {
        if (styleValue == nullptr)
            return 0;

        return juce::jlimit(0, kNumStyles - 1, juce::roundToInt(styleValue->load(std::memory_order_relaxed)));
    }

    EmberComboBox styleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> styleAttachment;

    LabelledKnob drive, mix, level, pan, width;
    LabelledKnob feedbackAmount, feedbackFrequency;
    LabelledKnob toneLow, toneMid, toneHigh;
    DynamicsControl dynamics;

    EmberToggle bypass, solo;

    std::atomic<float>* styleValue{nullptr};

    juce::Array<juce::Component*> allComponents;
    juce::Array<ModulatableKnob*> allKnobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandControls)
};

//==============================================================================
BandPanel::BandPanel(EmberAudioProcessor& processorToUse) : processor(processorToUse)
{
    groups[idx(kSaturationGroup)].header.setTitle("Saturation");
    groups[idx(kStereoGroup)].header.setTitle("Stereo");
    groups[idx(kFeedbackGroup)].header.setTitle("Feedback");
    groups[idx(kDynamicsGroup)].header.setTitle("Dynamics");
    groups[idx(kToneGroup)].header.setTitle("Tone");

    for (int i = 0; i < kNumGroups; ++i)
    {
        groups[idx(i)].weight = kGroupWeights[idx(i)];
        addAndMakeVisible(groups[idx(i)].header);
    }

    addAndMakeVisible(styleHeader);

    outputMeter.setSource([this] { return processor.getBandLevel(currentBand); });
    outputMeter.setDecibelRange(-48.0f, 6.0f);
    addAndMakeVisible(outputMeter);

    auto& state = processor.getAPVTS();
    numBandsValue = state.getRawParameterValue(pid::numBands);

    for (int i = 0; i < kMaxCrossovers; ++i)
        crossoverValues[idx(i)] = state.getRawParameterValue(pid::crossover(i));

    // The control set for the processor's selected band is built here; the
    // other five are built the first time they are asked for.
    setBand(processor.getSelectedBand());

    startTimerHz(30);
}

BandPanel::~BandPanel()
{
    stopTimer();
}

//==============================================================================
void BandPanel::setBand(int newBandIndex)
{
    const int clamped = juce::jlimit(0, kMaxBands - 1, newBandIndex);

    if (clamped == currentBand && bandControls[idx(clamped)] != nullptr)
        return;

    currentBand = clamped;
    showControlsFor(currentBand);

    const auto colour = EmberColours::band(currentBand);
    styleHeader.setAccentColour(colour);

    for (auto& group : groups)
        group.header.setAccentColour(colour);

    EmberStyleProps::setAccentColour(outputMeter, colour);

    lastStyleIndex = -1;
    updateHeaderStrings();

    resized();
    repaint();
}

void BandPanel::refreshModulationDisplay()
{
    if (auto* controls = bandControls[idx(currentBand)].get())
        controls->refreshModulation();
}

//==============================================================================
BandPanel::BandControls& BandPanel::controlsFor(int bandIndex)
{
    const int clamped = juce::jlimit(0, kMaxBands - 1, bandIndex);
    auto& slot = bandControls[idx(clamped)];

    if (slot == nullptr)
        slot = std::make_unique<BandControls>(*this, processor, clamped);

    return *slot;
}

void BandPanel::showControlsFor(int bandIndex)
{
    // Every active band is on screen now - the panel is a strip of modules, not
    // a single editor with five hidden twins. The selected one shows its full
    // control set; the rest keep Drive and their heat bar.
    for (int band = 0; band < kMaxBands; ++band)
    {
        if (auto* controls = bandControls[idx(band)].get())
        {
            const bool active = band < activeBandCount();

            if (! active)
            {
                controls->setControlsVisible(false);
                controls->setPollingEnabled(false);
            }
            else if (band == bandIndex)
            {
                controls->setControlsVisible(true);
                controls->setPollingEnabled(true);
            }
            else
            {
                controls->setCompact();
                controls->setPollingEnabled(true);
            }
        }
    }

    auto& selectedControls = controlsFor(bandIndex);
    selectedControls.setControlsVisible(true);
    selectedControls.setPollingEnabled(true);
}

void BandPanel::wireKnob(ModulatableKnob& knob, const juce::String& hint)
{
    knob.setExtraTooltipText(hint);

    knob.onModulationDropped = [this](const juce::String& targetParameterID, int sourceFlatIndex)
    {
        if (onModulationDropped)
            onModulationDropped(targetParameterID, sourceFlatIndex);
    };

    knob.onRemoveModulation = [this](const juce::String& targetParameterID)
    {
        if (onRemoveModulation)
            onRemoveModulation(targetParameterID);
    };

    // MIDI learn has a sensible default in the processor, so fall back to it
    // rather than swallowing the gesture when the editor supplies nothing.
    knob.onMidiLearn = [this](const juce::String& targetParameterID)
    {
        if (onMidiLearn)
            onMidiLearn(targetParameterID);
        else
            processor.beginMidiLearn(targetParameterID);
    };

    knob.onClearMidiMapping = [this](const juce::String& targetParameterID)
    {
        if (onClearMidiMapping)
            onClearMidiMapping(targetParameterID);
        else
            processor.clearMidiMapping(targetParameterID);
    };
}

//==============================================================================
void BandPanel::timerCallback()
{
    updateHeaderStrings();
}

int BandPanel::activeBandCount() const
{
    if (numBandsValue == nullptr)
        return kMaxBands;

    return juce::jlimit(kMinBands, kMaxBands, juce::roundToInt(numBandsValue->load(std::memory_order_relaxed)));
}

juce::String BandPanel::frequencyRangeText() const
{
    const int active = activeBandCount();

    if (currentBand >= active)
        return "not in use at " + juce::String(active) + (active == 1 ? " band" : " bands");

    const int numEdges = active - 1;

    if (numEdges <= 0)
        return "full range";

    std::array<float, idx(kMaxCrossovers)> edges{};

    for (int i = 0; i < numEdges; ++i)
        edges[idx(i)] =
            crossoverValues[idx(i)] != nullptr ? crossoverValues[idx(i)]->load(std::memory_order_relaxed) : 0.0f;

    // The crossover parameters are independent, so a user (or an automation
    // lane) can cross them over each other: sort before reading edges off.
    std::sort(edges.begin(), edges.begin() + numEdges);

    if (currentBand == 0)
        return "below " + formatFrequency(edges[0]);

    if (currentBand >= numEdges)
        return "above " + formatFrequency(edges[idx(numEdges - 1)]);

    return formatFrequency(edges[idx(currentBand - 1)]) + " " + enDash() + " " +
           formatFrequency(edges[idx(currentBand)]);
}

void BandPanel::updateHeaderStrings()
{
    const bool nowActive = currentBand < activeBandCount();
    const auto text = frequencyRangeText();

    if (text != rangeText || nowActive != bandActive)
    {
        rangeText = text;
        bandActive = nowActive;
        repaint(headerArea.isEmpty() ? getLocalBounds() : headerArea);
    }

    if (auto* controls = bandControls[idx(currentBand)].get())
    {
        const int styleIndex = controls->currentStyleIndex();

        if (styleIndex != lastStyleIndex)
        {
            lastStyleIndex = styleIndex;
            styleHeader.setTrailingText(getCategoryName(getStyleCategory(static_cast<StyleID>(styleIndex))));
        }
    }
}

float BandPanel::uiScale() const
{
    if (const auto* lookAndFeel = dynamic_cast<const EmberLookAndFeel*>(&getLookAndFeel()))
        return lookAndFeel->getUiScale();

    return EmberFonts::scaleFor(static_cast<float>(juce::jmax(1, getHeight())));
}

//==============================================================================
void BandPanel::resized()
{
    const float scale = uiScale();
    const int active = activeBandCount();

    for (auto& b : moduleBounds)
        b = {};

    auto strip = getLocalBounds().reduced(juce::jmax(4, juce::roundToInt(6.0f * scale)));

    if (strip.getWidth() < 40 || strip.getHeight() < 40 || active <= 0)
        return;

    const int moduleGap = juce::jmax(4, juce::roundToInt(static_cast<float>(Spacing::sm) * scale));

    // The selected module is 1.6x the width of an unselected one. Weights
    // rather than fixed widths, so six bands at the minimum size degrade
    // evenly instead of the last one falling off the end.
    constexpr float kSelectedWeight = 1.6f;
    const float totalWeight = static_cast<float>(active - 1) + kSelectedWeight;
    const int available = strip.getWidth() - moduleGap * juce::jmax(0, active - 1);

    if (available < active * 24)
        return;

    int x = strip.getX();

    for (int band = 0; band < active; ++band)
    {
        const bool selected = band == currentBand;
        const float weight = selected ? kSelectedWeight : 1.0f;
        const int width = (band == active - 1) ? (strip.getRight() - x)
                                               : juce::roundToInt(static_cast<float>(available) * weight / totalWeight);

        moduleBounds[idx(band)] = {x, strip.getY(), width, strip.getHeight()};
        x += width + moduleGap;
    }

    // ---- the selected module gets the full editor -------------------------
    auto& controls = controlsFor(currentBand);
    auto area = moduleBounds[idx(currentBand)].reduced(juce::jmax(4, juce::roundToInt(
                                                           static_cast<float>(Spacing::md) * scale)));

    const int gap = juce::jmax(6, juce::roundToInt(10.0f * scale));
    juce::ignoreUnused(SectionHeader::preferredHeight(scale));
    const int comboHeight = juce::jlimit(22, 44, juce::roundToInt(29.0f * scale));

    // The knob groups are budgeted FIRST, because they are what the module is
    // for and `layoutGroups` places nothing at all below kMinimumGroupsHeight.
    // Capping the header and the style row independently compounded instead of
    // cooperating: at the minimum window the two strips took 96 of 135 px and
    // every knob was dropped on the floor.
    // A module is roughly 226 px tall. A 40 px header plus a style row with its
    // own section title spent 105 of that on chrome and left 93 for the knobs,
    // below the two-row minimum, so every module fell back to one squeezed row
    // of five groups with truncated labels. The design calls this "a thin
    // engraved title bar", not two stacked strips: the STYLE caption goes, and
    // the dropdown carries its own meaning.
    styleHeader.setVisible(false);

    int headerHeight = juce::jlimit(26, 52, juce::roundToInt(32.0f * scale));
    int styleRowHeight = comboHeight;

    int deficit = kMinimumGroupsHeight + 2 * gap + headerHeight + styleRowHeight - area.getHeight();

    if (deficit > 0)
    {
        const int fromHeader = juce::jlimit(0, deficit, headerHeight - kMinimumHeaderHeight);
        headerHeight -= fromHeader;
        deficit -= fromHeader;

        const int fromStyleRow = juce::jlimit(0, deficit, styleRowHeight - comboHeight);
        styleRowHeight -= fromStyleRow;
    }

    headerArea = area.removeFromTop(juce::jmin(headerHeight, area.getHeight()));
    layoutHeader(headerArea, controls);
    area.removeFromTop(juce::jmin(gap, area.getHeight()));

    auto styleRow = area.removeFromTop(juce::jmin(styleRowHeight, area.getHeight()));
    controls.styleBox.setBounds(styleRow);

    area.removeFromTop(juce::jmin(gap, area.getHeight()));
    layoutKnobGrid(area, controls);

    // ---- everything else is compact ---------------------------------------
    // Visibility is settled here rather than only in setBand: setBand can run
    // before the band-count parameter has been read, and anything it hid then
    // would stay hidden through every later layout.
    showControlsFor(currentBand);

    for (int band = 0; band < active; ++band)
        if (band != currentBand)
            layoutCompactModule(moduleBounds[idx(band)], controlsFor(band));
}

void BandPanel::layoutCompactModule(juce::Rectangle<int> slot, BandControls& controls)
{
    const float scale = uiScale();
    auto area = slot.reduced(juce::jmax(3, juce::roundToInt(static_cast<float>(Spacing::sm) * scale)));

    if (area.getWidth() < 20 || area.getHeight() < 40)
        return;

    // Title strip at the top and the heat bar at the bottom are painted, not
    // laid out; the knob takes what is left, square and centred.
    const int titleHeight = juce::jmax(14, juce::roundToInt(22.0f * scale));
    const int heatHeight = juce::jmax(4, juce::roundToInt(6.0f * scale));

    area.removeFromTop(titleHeight);
    area.removeFromBottom(heatHeight + juce::jmax(2, juce::roundToInt(4.0f * scale)));

    const int size = juce::jmin(area.getWidth(), area.getHeight());

    if (size < 16)
        return;

    controls.drive.setBounds(juce::Rectangle<int>(size, size).withCentre(area.getCentre()));
}

void BandPanel::mouseDown(const juce::MouseEvent& event)
{
    const int active = activeBandCount();

    for (int band = 0; band < active; ++band)
    {
        if (band != currentBand && moduleBounds[idx(band)].contains(event.getPosition()))
        {
            if (onBandClicked != nullptr)
                onBandClicked(band);

            return;
        }
    }
}

void BandPanel::layoutHeader(juce::Rectangle<int> area, BandControls& controls)
{
    const float scale = uiScale();
    const int gap = juce::jmax(5, juce::roundToInt(8.0f * scale));

    auto row =
        area.reduced(juce::jmax(4, juce::roundToInt(8.0f * scale)), juce::jmax(3, juce::roundToInt(5.0f * scale)));

    if (row.getWidth() < 40 || row.getHeight() < 12)
    {
        chipArea = {};
        titleArea = {};
        meterLabelArea = {};
        cacheHeaderFonts();
        return;
    }

    // ---- the band badge, in the band's own colour
    chipArea = row.removeFromLeft(juce::jmin(row.getHeight(), row.getWidth() / 4));
    row.removeFromLeft(gap);

    // ---- bypass / solo, from the right
    const int toggleHeight = juce::jlimit(14, 30, juce::roundToInt(static_cast<float>(row.getHeight()) * 0.8f));
    const int widthCap = juce::jmax(24, row.getWidth() / 3);

    controls.solo.setSize(widthCap, toggleHeight);
    controls.bypass.setSize(widthCap, toggleHeight);

    const int soloWidth = juce::jmin(widthCap, controls.solo.preferredWidth());
    const int bypassWidth = juce::jmin(widthCap, controls.bypass.preferredWidth());

    auto soloBounds = row.removeFromRight(soloWidth);
    controls.solo.setBounds(soloBounds.withSizeKeepingCentre(soloBounds.getWidth(), toggleHeight));

    row.removeFromRight(juce::jmin(gap, row.getWidth()));

    auto bypassBounds = row.removeFromRight(juce::jmin(bypassWidth, row.getWidth()));
    controls.bypass.setBounds(bypassBounds.withSizeKeepingCentre(bypassBounds.getWidth(), toggleHeight));

    // ---- band output meter, when there is room for it to mean anything
    const bool showMeter = row.getWidth() > juce::roundToInt(170.0f * scale);
    outputMeter.setVisible(showMeter);
    meterLabelArea = {};

    if (showMeter)
    {
        row.removeFromRight(gap);

        auto meterBounds = row.removeFromRight(juce::jlimit(44, juce::roundToInt(160.0f * scale), row.getWidth() / 3));
        const int barHeight = juce::jlimit(5, 13, juce::roundToInt(8.0f * scale));
        outputMeter.setBounds(meterBounds.withSizeKeepingCentre(meterBounds.getWidth(), barHeight));

        meterLabelArea = row.removeFromRight(juce::jlimit(20, 44, juce::roundToInt(30.0f * scale)));
    }

    titleArea = row;
    cacheHeaderFonts();
}

void BandPanel::cacheHeaderFonts()
{
    // Each juce::Font allocates, and the output and gain-reduction meters push
    // this panel through paint() up to 30 times a second, so the fonts are
    // built from the areas once per layout instead.
    const float chipHeight = static_cast<float>(chipArea.getHeight()) - 2.0f; // paint() insets the chip by 1 px
    chipFont = EmberFonts::forHeight(juce::jmax(1.0f, chipHeight), 0.56f, true);

    // paint() splits the title area 0.56 / 0.44 between the name and the range.
    const float titleHeight = static_cast<float>(titleArea.getHeight());
    const float nameHeight = titleHeight * 0.56f;

    bandNameFont = EmberFonts::forHeight(juce::jmax(1.0f, nameHeight), 0.86f, true);
    rangeFont = EmberFonts::forHeight(juce::jmax(1.0f, titleHeight - nameHeight), 0.84f, false);

    meterLabelFont = EmberFonts::get(EmberFonts::Role::micro, uiScale());
}

//==============================================================================
void BandPanel::layoutKnobGrid(juce::Rectangle<int> area, BandControls& controls)
{
    // The design's grid, not the old five-group row:
    //   row 1, large : DRIVE, MIX, LEVEL
    //   row 2, small : FEEDBACK, FREQ, DYNAMICS, LOW, MID, HIGH, PAN, WIDTH
    //
    // The group system spent about 24 px per row on section headers, which a
    // module-sized slot cannot afford - with roughly 115 px of knob height it
    // could never satisfy a two-row minimum and always fell back to one
    // squeezed row. Dropping the headers is what makes two rows fit at all;
    // every knob carries its own label anyway, so nothing is lost but the
    // captions that were truncating to "A..." and "Fr...".
    for (auto& group : groups)
    {
        group.well = {};
        group.header.setVisible(false);
    }

    feedbackLinkA = {};
    feedbackLinkB = {};

    if (area.getWidth() < 40 || area.getHeight() < 40)
        return;

    const float scale = uiScale();
    const int gap = juce::jmax(3, juce::roundToInt(6.0f * scale));

    // The large row takes the height it can up to a comfortable knob, and the
    // small row gets what is left.
    int largeHeight = juce::jlimit(36, 92, juce::roundToInt(static_cast<float>(area.getHeight()) * 0.56f));

    // The small row must survive. Taking 56 % for the large row left it under
    // the 16 px `place` refuses to draw into at the 860x510 minimum, and eight
    // controls simply vanished - unreachable, not merely small, which is a
    // layout break rather than graceful degradation. The large row gives way
    // instead.
    constexpr int kMinimumSmallRow = 26;

    if (area.getHeight() - largeHeight - gap < kMinimumSmallRow)
        largeHeight = juce::jmax(30, area.getHeight() - gap - kMinimumSmallRow);

    auto largeRow = area.removeFromTop(juce::jmin(largeHeight, area.getHeight()));
    area.removeFromTop(juce::jmin(gap, area.getHeight()));
    auto smallRow = area;

    const auto place = [gap](juce::Rectangle<int> row, std::initializer_list<juce::Component*> items)
    {
        const auto count = static_cast<int>(items.size());

        if (count <= 0 || row.getWidth() < count * 12 || row.getHeight() < 16)
            return;

        const int each = (row.getWidth() - gap * (count - 1)) / count;
        int x = row.getX();

        for (auto* item : items)
        {
            item->setBounds(x, row.getY(), each, row.getHeight());
            x += each + gap;
        }
    };

    place(largeRow, {&controls.drive, &controls.mix, &controls.level});
    place(smallRow, {&controls.feedbackAmount, &controls.feedbackFrequency, &controls.dynamics, &controls.toneLow,
                     &controls.toneMid, &controls.toneHigh, &controls.pan, &controls.width});
}

void BandPanel::layoutGroups(juce::Rectangle<int> area, BandControls& controls)
{
    for (auto& group : groups)
        group.well = {};

    feedbackLinkA = {};
    feedbackLinkB = {};

    if (area.getWidth() < 40 || area.getHeight() < kMinimumGroupsHeight)
        return;

    const float scale = uiScale();
    const int gap = juce::jmax(6, juce::roundToInt(10.0f * scale));

    // Rows, widest arrangement first. A group never gets narrower than one
    // comfortable knob slot, so the panel folds instead of squashing.
    static constexpr int rowOfFive[] = {kSaturationGroup, kStereoGroup, kFeedbackGroup, kDynamicsGroup, kToneGroup};
    static constexpr int twoRowTop[] = {kSaturationGroup, kStereoGroup};
    static constexpr int twoRowBottom[] = {kFeedbackGroup, kDynamicsGroup, kToneGroup};
    static constexpr int threeRowTop[] = {kSaturationGroup};
    static constexpr int threeRowMiddle[] = {kStereoGroup, kFeedbackGroup};
    static constexpr int threeRowBottom[] = {kDynamicsGroup, kToneGroup};

    const auto rowWeight = [this](const int* indices, int count)
    {
        float total = 0.0f;

        for (int i = 0; i < count; ++i)
            total += groups[idx(indices[i])].weight;

        return total;
    };

    // 62 px per weight unit was tuned when the selected band had the whole
    // panel width. In the strip it never does - the expanded module is about
    // 45 % of the width with three bands up - and 62 let five groups squeeze
    // into a slot where every knob label truncated to "P...". Folding to two
    // rows is what the design asks for at this width anyway.
    const float minimumSlot = 80.0f * scale;
    const float availableWidth = static_cast<float>(area.getWidth());

    // Folding costs height: two rows need twice the height of one and three
    // need three times. Choosing on width alone therefore had it backwards — a
    // panel too NARROW for one row fell through to an arrangement that needed
    // more height than it had, every row failed `layoutRow`'s own minimum and
    // nothing was laid out at all (the 800x480 case). Height vetoes the taller
    // arrangements, so a short panel keeps the single row and squeezes its
    // slots rather than losing the controls.
    // These were set when the band panel was full width and had the height to
    // match. In the strip a module is about 226 px tall, of which its header
    // and style row take 105, so a 180 px two-row minimum could never be met
    // and every module was forced back to one squeezed row of five groups.
    // Two rows of compact knobs genuinely fit in 130.
    const int twoRowMinimumHeight = juce::roundToInt(130.0f * scale);
    const int threeRowMinimumHeight = juce::roundToInt(200.0f * scale);

    // An absolute floor as well as the scaled one. The scaled test alone is
    // only as trustworthy as uiScale, and when that comes back small the
    // threshold collapses and five groups squeeze into a module-width slot
    // with every label truncated to "W...". Five groups need real estate in
    // pixels, whatever the scale factor believes.
    const float fiveGroupMinimumWidth = juce::jmax(620.0f, minimumSlot * rowWeight(rowOfFive, 5));

    if (availableWidth >= fiveGroupMinimumWidth || area.getHeight() < twoRowMinimumHeight)
    {
        layoutRow(area, rowOfFive, 5, controls);
        return;
    }

    const float threeGroupMinimumWidth = juce::jmax(330.0f, minimumSlot * rowWeight(twoRowBottom, 3));

    if (availableWidth >= threeGroupMinimumWidth || area.getHeight() < threeRowMinimumHeight)
    {
        const int rowHeight = juce::jmax(0, (area.getHeight() - gap) / 2);
        auto top = area.removeFromTop(rowHeight);
        area.removeFromTop(gap);

        layoutRow(top, twoRowTop, 2, controls);
        layoutRow(area, twoRowBottom, 3, controls);
        return;
    }

    const int rowHeight = juce::jmax(0, (area.getHeight() - 2 * gap) / 3);
    auto top = area.removeFromTop(rowHeight);
    area.removeFromTop(gap);
    auto middle = area.removeFromTop(rowHeight);
    area.removeFromTop(gap);

    layoutRow(top, threeRowTop, 1, controls);
    layoutRow(middle, threeRowMiddle, 2, controls);
    layoutRow(area, threeRowBottom, 2, controls);
}

void BandPanel::layoutRow(juce::Rectangle<int> row, const int* groupIndices, int numGroups, BandControls& controls)
{
    if (numGroups <= 0 || row.getWidth() < 8 || row.getHeight() < 8)
        return;

    const int gap = juce::jmax(6, juce::roundToInt(10.0f * uiScale()));

    float totalWeight = 0.0f;

    for (int i = 0; i < numGroups; ++i)
        totalWeight += groups[idx(groupIndices[i])].weight;

    totalWeight = juce::jmax(0.001f, totalWeight);

    const int available = juce::jmax(0, row.getWidth() - gap * (numGroups - 1));

    for (int i = 0; i < numGroups; ++i)
    {
        const bool last = (i == numGroups - 1);
        const float weight = groups[idx(groupIndices[i])].weight;
        const int cellWidth =
            last ? row.getWidth() : juce::roundToInt(static_cast<float>(available) * weight / totalWeight);

        auto cell = row.removeFromLeft(cellWidth);

        if (!last)
            row.removeFromLeft(juce::jmin(gap, row.getWidth()));

        layoutGroup(groupIndices[i], cell, controls);
    }
}

void BandPanel::layoutGroup(int groupIndex, juce::Rectangle<int> bounds, BandControls& controls)
{
    auto& group = groups[idx(groupIndex)];

    const float scale = uiScale();
    const int headerHeight = SectionHeader::preferredHeight(scale);
    const int spacing = juce::jmax(2, juce::roundToInt(4.0f * scale));
    const int maximumWell = juce::roundToInt(154.0f * scale);

    if (bounds.getWidth() < 4 || bounds.getHeight() < 4)
        return;

    // A tall row would otherwise stretch the wells into empty slabs: keep the
    // group its natural height and centre it instead.
    const int naturalHeight = headerHeight + spacing + maximumWell;

    if (bounds.getHeight() > naturalHeight)
        bounds = bounds.withSizeKeepingCentre(bounds.getWidth(), naturalHeight);

    // A group squeezed below its header height keeps a proportional share for
    // the title rather than swallowing the knobs whole.
    group.header.setBounds(bounds.removeFromTop(juce::jmin(headerHeight, bounds.getHeight() / 3)));
    bounds.removeFromTop(juce::jmin(spacing, bounds.getHeight()));
    group.well = bounds;

    auto inner =
        bounds.reduced(juce::jmax(4, juce::roundToInt(8.0f * scale)), juce::jmax(3, juce::roundToInt(7.0f * scale)));

    // Too tight to inset: use the well itself rather than an inverted rectangle.
    if (inner.getWidth() < 8 || inner.getHeight() < 8)
        inner = bounds;

    const float cellMargin = juce::jmax(1.0f, 2.5f * scale);
    const auto margin = juce::FlexItem::Margin(0.0f, cellMargin, 0.0f, cellMargin);

    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::row;
    flex.alignItems = juce::FlexBox::AlignItems::stretch;

    if (groupIndex == kSaturationGroup)
    {
        // Drive is the band's headline control: a full-height knob, with mix
        // and level stacked half-height beside it.
        juce::FlexBox stack;
        stack.flexDirection = juce::FlexBox::Direction::column;
        stack.items.add(juce::FlexItem(controls.mix).withFlex(1.0f));
        stack.items.add(juce::FlexItem(controls.level).withFlex(1.0f));

        flex.items.add(juce::FlexItem(controls.drive).withFlex(1.45f).withMargin(margin));
        flex.items.add(juce::FlexItem(stack).withFlex(1.0f).withMargin(margin));
        flex.performLayout(inner.toFloat());
    }
    else if (groupIndex == kStereoGroup)
    {
        flex.items.add(juce::FlexItem(controls.pan).withFlex(1.0f).withMargin(margin));
        flex.items.add(juce::FlexItem(controls.width).withFlex(1.0f).withMargin(margin));
        flex.performLayout(inner.toFloat());
    }
    else if (groupIndex == kFeedbackGroup)
    {
        flex.items.add(juce::FlexItem(controls.feedbackAmount).withFlex(1.0f).withMargin(margin));
        flex.items.add(juce::FlexItem(controls.feedbackFrequency).withFlex(1.0f).withMargin(margin));
        flex.performLayout(inner.toFloat());

        // The tie drawn between these two in paint(): amount and frequency are
        // one effect, and the link says so.
        feedbackLinkA = controls.feedbackAmount.getKnob().getBounds() + controls.feedbackAmount.getPosition();
        feedbackLinkB = controls.feedbackFrequency.getKnob().getBounds() + controls.feedbackFrequency.getPosition();
    }
    else if (groupIndex == kDynamicsGroup)
    {
        controls.dynamics.setBounds(inner);
    }
    else
    {
        flex.items.add(juce::FlexItem(controls.toneLow).withFlex(1.0f).withMargin(margin));
        flex.items.add(juce::FlexItem(controls.toneMid).withFlex(1.0f).withMargin(margin));
        flex.items.add(juce::FlexItem(controls.toneHigh).withFlex(1.0f).withMargin(margin));
        flex.performLayout(inner.toFloat());
    }
}

//==============================================================================
void BandPanel::paintModuleChrome(juce::Graphics& g, int band, juce::Rectangle<int> slot, bool selected)
{
    if (slot.isEmpty())
        return;

    const auto& tk = EmberTheme::tokens();
    const float scale = uiScale();
    const float corner = juce::jlimit(4.0f, 16.0f, 11.0f * scale);
    const auto area = slot.toFloat();
    const float heat = juce::jlimit(0.0f, 1.0f, processor.getBandHeat(band));

    EmberLookAndFeel::drawPanel(g, area, corner, selected);

    // A module warms with its band. Selected modules read brighter at the same
    // heat, so selection and activity stay separable - one is where you are
    // working, the other is what the audio is doing.
    const auto tint = tk.heatTint(heat);
    const int titleHeight = juce::jmax(14, juce::roundToInt(22.0f * scale));
    const auto titleBar = area.withHeight(static_cast<float>(titleHeight));

    if (heat > 0.01f)
    {
        g.setGradientFill(juce::ColourGradient(tint.withAlpha(heat * (selected ? 0.30f : 0.20f)), titleBar.getCentreX(),
                                               titleBar.getY(), tint.withAlpha(0.0f), titleBar.getCentreX(),
                                               titleBar.getBottom(), false));
        g.fillRoundedRectangle(titleBar, corner);
    }

    if (selected)
    {
        // A thin ember underline is the whole selection marker. An outline
        // around the module would fight the heat tint for the same edge.
        const float underline = juce::jmax(1.5f, 2.0f * scale);
        g.setColour(tk.ember);
        g.fillRect(area.withTop(titleBar.getBottom() - underline).withHeight(underline).reduced(corner * 0.5f, 0.0f));
    }

    // Compact modules carry their own number, style name and heat bar, because
    // the full header only exists on the selected one.
    if (! selected)
    {
        auto inner = area.reduced(static_cast<float>(Spacing::sm) * scale);
        auto title = inner.withHeight(static_cast<float>(titleHeight));

        g.setFont(EmberFonts::get(EmberFonts::Role::section, scale));
        g.setColour(heat > 0.02f ? tint : tk.textDim);
        g.drawText(juce::String(band + 1), title, juce::Justification::centredLeft, false);

        if (auto* controls = bandControls[idx(band)].get())
        {
            const auto name = controls->styleBox.getText();

            if (name.isNotEmpty() && title.getWidth() > 60.0f * scale)
            {
                g.setFont(EmberFonts::get(EmberFonts::Role::micro, scale));
                g.setColour(tk.textDim);
                g.drawText(name, title.withTrimmedLeft(title.getHeight()), juce::Justification::centredLeft, true);
            }
        }

        // The heat bar: the one thing that has to be readable at 40 px wide.
        const float barHeight = juce::jmax(4.0f, 6.0f * scale);
        const auto barArea = inner.withTop(inner.getBottom() - barHeight);

        g.setColour(tk.bgDeep);
        g.fillRoundedRectangle(barArea, barHeight * 0.5f);

        if (heat > 0.01f)
        {
            const auto filled = barArea.withWidth(barArea.getWidth() * heat);
            g.setColour(tint);
            g.fillRoundedRectangle(filled, barHeight * 0.5f);

            if (! EmberTheme::reduceMotion())
                GlowCache::drawForRect(g, filled, barHeight * 0.5f, tk.tubeGlow, heat * 0.5f);
        }
    }
}

void BandPanel::paint(juce::Graphics& g)
{
    const float scale = uiScale();
    const auto colour = EmberColours::band(currentBand);
    const float corner = juce::jlimit(4.0f, 16.0f, 11.0f * scale);

    // The strip itself is the chassis; each module is a plate bolted to it.
    const int active = activeBandCount();

    for (int band = 0; band < active; ++band)
        paintModuleChrome(g, band, moduleBounds[idx(band)], band == currentBand);

    if (!headerArea.isEmpty())
    {
        const auto header = headerArea.toFloat();
        EmberLookAndFeel::drawWell(g, header, corner * 0.72f);

        // A rule in the band's colour, fading out along the header, so the band
        // identity is readable from the far side of a large window.
        const float ruleHeight = juce::jmax(1.5f, 2.0f * scale);
        const auto rule = header.withTop(header.getBottom() - ruleHeight).reduced(corner * 0.6f, 0.0f);

        if (rule.getWidth() > 4.0f)
        {
            g.setGradientFill(juce::ColourGradient(colour.withAlpha(bandActive ? 0.85f : 0.30f), rule.getX(),
                                                   rule.getY(), colour.withAlpha(0.0f), rule.getRight(), rule.getY(),
                                                   false));
            g.fillRect(rule);
        }
    }

    if (chipArea.getWidth() > 10 && chipArea.getHeight() > 10)
    {
        const auto chip = chipArea.toFloat().reduced(1.0f);
        const float chipCorner = chip.getHeight() * 0.28f;

        g.setColour(colour.withAlpha(bandActive ? 0.18f : 0.08f));
        g.fillRoundedRectangle(chip, chipCorner);

        g.setColour(colour.withAlpha(bandActive ? 0.75f : 0.32f));
        g.drawRoundedRectangle(chip.reduced(0.5f), chipCorner, 1.0f);

        g.setColour(bandActive ? colour : colour.withAlpha(0.45f));
        g.setFont(chipFont);
        g.drawText(juce::String(currentBand + 1), chip, juce::Justification::centred, false);
    }

    if (titleArea.getWidth() > 30 && titleArea.getHeight() > 12)
    {
        auto title = titleArea.toFloat();
        auto nameArea = title.removeFromTop(title.getHeight() * 0.56f);

        g.setColour(EmberColours::textPrimary);
        g.setFont(bandNameFont);
        g.drawText("BAND " + juce::String(currentBand + 1), nameArea, juce::Justification::centredLeft, true);

        g.setColour(bandActive ? EmberColours::textSecondary : EmberColours::warning.withAlpha(0.8f));
        g.setFont(rangeFont);
        g.drawText(rangeText, title, juce::Justification::centredLeft, true);
    }

    if (outputMeter.isVisible() && meterLabelArea.getWidth() > 10)
    {
        g.setColour(EmberColours::textDisabled);
        g.setFont(meterLabelFont);
        g.drawText("OUT", meterLabelArea, juce::Justification::centredRight, false);
    }

    for (const auto& group : groups)
        if (!group.well.isEmpty())
            EmberLookAndFeel::drawWell(g, group.well.toFloat(), corner * 0.6f);

    // The feedback tie: drawn before the children, between the two knobs, so it
    // reads as one control with two halves.
    if (!feedbackLinkA.isEmpty() && !feedbackLinkB.isEmpty())
    {
        const float left = static_cast<float>(feedbackLinkA.getRight());
        const float right = static_cast<float>(feedbackLinkB.getX());
        const float centreY = static_cast<float>(feedbackLinkA.getCentreY());

        if (right - left > 6.0f)
        {
            g.setColour(colour.withAlpha(0.32f));
            g.drawLine(left, centreY, right, centreY, juce::jmax(1.0f, 1.2f * scale));

            const float dot = juce::jmax(2.0f, 2.6f * scale);
            g.setColour(colour.withAlpha(0.8f));
            g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({(left + right) * 0.5f, centreY}));
        }
    }
}
} // namespace ember::gui
