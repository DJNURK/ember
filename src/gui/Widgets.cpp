#include "gui/Widgets.h"

#include <cmath>

namespace ember::gui
{
namespace
{
/** Key under which a modulation drag carries its source index. */
const juce::Identifier& modulationDragKey()
{
    static const juce::Identifier key("emberModSource");
    return key;
}

/** Right-click menu item IDs, shared by the menu builder and its handler. */
enum KnobMenuItem
{
    menuReset = 1,
    menuTypeValue,
    menuMidiLearn,
    menuMidiClear,
    menuRemoveModulation
};

/** How fast an observed modulation span relaxes back towards the live offset,
    per timer tick. At 30 Hz this is roughly a half-second settle, which is slow
    enough to show an LFO's full sweep and quick enough that reducing a depth is
    visible immediately. */
constexpr float kModulationSpanRelax = 0.06f;
} // namespace

//==============================================================================
juce::var ModulationDrag::makeDescription(int sourceFlatIndex)
{
    auto* payload = new juce::DynamicObject();
    payload->setProperty(modulationDragKey(), sourceFlatIndex);
    return juce::var(payload);
}

int ModulationDrag::sourceIndexFromDescription(const juce::var& description)
{
    if (auto* payload = description.getDynamicObject())
    {
        if (payload->hasProperty(modulationDragKey()))
        {
            const int index = static_cast<int>(payload->getProperty(modulationDragKey()));
            return juce::isPositiveAndBelow(index, kNumModSources) ? index : -1;
        }
    }

    return -1;
}

//==============================================================================
ModulatableKnob::ModulatableKnob(EmberAudioProcessor& processorToUse, const juce::String& parameterIDToUse)
    : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), processor(processorToUse),
      parameterID(parameterIDToUse)
{
    parameter = processor.getAPVTS().getParameter(parameterID);
    jassert(parameter != nullptr); // an ID that does not resolve: use the ember::pid:: helpers

    modulationTargetIndex = processor.getModulationTargetIndex(parameterID);

    setRotaryParameters(EmberLookAndFeel::kRotaryStartAngle, EmberLookAndFeel::kRotaryEndAngle, true);
    setScrollWheelEnabled(true);
    setMouseDragSensitivity(220);
    setWantsKeyboardFocus(false);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.getAPVTS(),
                                                                                        parameterID, *this);

    if (parameter != nullptr)
    {
        // Double-click resets; the alt+single-click shortcut JUCE enables by
        // default is turned off because hosts claim alt for their own drags.
        setDoubleClickReturnValue(true, static_cast<double>(parameter->convertFrom0to1(parameter->getDefaultValue())),
                                  juce::ModifierKeys());
    }

    updateTooltip();
    refreshModulationDisplay();
    startTimerHz(refreshRateHz);
}

ModulatableKnob::~ModulatableKnob()
{
    stopTimer();
}

//==============================================================================
void ModulatableKnob::setModulationPollingEnabled(bool shouldPoll)
{
    pollingEnabled = shouldPoll;

    if (pollingEnabled)
        startTimerHz(refreshRateHz);
    else
        stopTimer();
}

void ModulatableKnob::setRefreshRateHz(int hz)
{
    refreshRateHz = juce::jlimit(10, 60, hz);

    if (pollingEnabled)
        startTimerHz(refreshRateHz);
}

void ModulatableKnob::setFineDragFactor(float factor)
{
    fineDragFactor = juce::jlimit(0.02f, 1.0f, factor);
}

void ModulatableKnob::setExtraTooltipText(const juce::String& text)
{
    extraTooltip = text;
    updateTooltip();
}

//==============================================================================
juce::Rectangle<float> ModulatableKnob::getRotaryArea()
{
    // The same rectangle juce::Slider hands the look-and-feel, so the ring this
    // class draws and the arcs the look-and-feel draws share a centre.
    return getLookAndFeel().getSliderLayout(*this).sliderBounds.toFloat();
}

void ModulatableKnob::paint(juce::Graphics& g)
{
    juce::Slider::paint(g); // track, value arc and pointer

    const auto geo = EmberLookAndFeel::rotaryGeometry(getRotaryArea());

    if (geo.outerRadius <= 1.0f)
        return;

    const auto rotary = getRotaryParameters();
    const float startAngle = rotary.startAngleRadians;
    const float endAngle = rotary.endAngleRadians;
    const auto accent = EmberStyleProps::accentColourFor(*this);
    const juce::PathStrokeType ringStroke(geo.modRingThickness, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded);

    if (modulationActive)
    {
        // The full ring says "this destination is assigned" even when every
        // source happens to be sitting still.
        g.setColour(accent.withAlpha(0.16f));
        g.strokePath(EmberLookAndFeel::arcPath(geo.centre, geo.modRingRadius, startAngle, endAngle), ringStroke);

        const float valueProportion = static_cast<float>(valueToProportionOfLength(getValue()));
        const float low = juce::jlimit(0.0f, 1.0f, valueProportion + modulationSpanLow);
        const float high = juce::jlimit(0.0f, 1.0f, valueProportion + modulationSpanHigh);

        if (high - low > 1.0e-3f)
        {
            g.setColour(accent.withAlpha(0.6f));
            g.strokePath(EmberLookAndFeel::arcPath(geo.centre, geo.modRingRadius,
                                                   EmberLookAndFeel::rotaryAngle(low, startAngle, endAngle),
                                                   EmberLookAndFeel::rotaryAngle(high, startAngle, endAngle)),
                         ringStroke);
        }

        const float live = juce::jlimit(0.0f, 1.0f, valueProportion + modulationOffset);
        const auto marker = geo.centre.getPointOnCircumference(
            geo.modRingRadius, EmberLookAndFeel::rotaryAngle(live, startAngle, endAngle));
        const float markerRadius = juce::jmax(1.6f, geo.modRingThickness * 0.85f);

        g.setColour(accent);
        g.fillEllipse(juce::Rectangle<float>(markerRadius * 2.0f, markerRadius * 2.0f).withCentre(marker));
    }

    const auto outerCircle =
        juce::Rectangle<float>(geo.outerRadius * 2.0f, geo.outerRadius * 2.0f).withCentre(geo.centre);

    if (midiLearning)
    {
        const float pulse = 0.35f + 0.35f * std::sin(learnPhase);
        g.setColour(EmberColours::accent.withAlpha(pulse));
        g.drawEllipse(outerCircle.reduced(1.0f), 2.0f);
    }

    if (dragHighlight)
    {
        g.setColour(accent.withAlpha(0.18f));
        g.fillEllipse(outerCircle);
        g.setColour(accent);
        g.drawEllipse(outerCircle.reduced(1.0f), 2.0f);
    }
}

//==============================================================================
void ModulatableKnob::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        forwardingToSlider = false;
        showParameterMenu();
        return;
    }

    virtualDragPosition = e.position;
    lastRawDragPosition = e.position;
    forwardingToSlider = true;

    juce::Slider::mouseDown(e);
}

void ModulatableKnob::mouseDrag(const juce::MouseEvent& e)
{
    if (!forwardingToSlider)
        return;

    // JUCE derives a rotary drag from the distance since mouse-down, so fine
    // control accumulates a scaled "virtual" pointer position rather than
    // changing the drag sensitivity — which would make the value jump the
    // moment shift is pressed or released mid-gesture.
    const float factor = e.mods.isShiftDown() ? fineDragFactor : 1.0f;

    virtualDragPosition += (e.position - lastRawDragPosition) * factor;
    lastRawDragPosition = e.position;

    juce::Slider::mouseDrag(e.withNewPosition(virtualDragPosition));
}

void ModulatableKnob::mouseUp(const juce::MouseEvent& e)
{
    if (!forwardingToSlider)
        return;

    forwardingToSlider = false;
    juce::Slider::mouseUp(e);
}

void ModulatableKnob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isShiftDown())
    {
        auto fine = wheel;
        fine.deltaX *= fineDragFactor;
        fine.deltaY *= fineDragFactor;
        juce::Slider::mouseWheelMove(e, fine);
        return;
    }

    juce::Slider::mouseWheelMove(e, wheel);
}

void ModulatableKnob::valueChanged()
{
    juce::Slider::valueChanged();
    updateTooltip();
}

void ModulatableKnob::parentHierarchyChanged()
{
    juce::Slider::parentHierarchyChanged();

    // Show the value bubble inside the editor rather than in a desktop window
    // of its own, which is only possible once there is a parent to put it in.
    if (auto* top = getTopLevelComponent())
        if (top != this)
            setPopupDisplayEnabled(true, false, top);
}

//==============================================================================
bool ModulatableKnob::isInterestedInDragSource(const SourceDetails& details)
{
    return modulationTargetIndex >= 0 && ModulationDrag::sourceIndexFromDescription(details.description) >= 0;
}

void ModulatableKnob::itemDragEnter(const SourceDetails&)
{
    dragHighlight = true;
    repaint();
}

void ModulatableKnob::itemDragExit(const SourceDetails&)
{
    dragHighlight = false;
    repaint();
}

void ModulatableKnob::itemDropped(const SourceDetails& details)
{
    dragHighlight = false;

    const int sourceIndex = ModulationDrag::sourceIndexFromDescription(details.description);

    if (sourceIndex >= 0 && onModulationDropped != nullptr)
        onModulationDropped(parameterID, sourceIndex);

    refreshModulationDisplay();
    repaint();
}

//==============================================================================
void ModulatableKnob::showParameterMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());

    if (parameter != nullptr)
        menu.addSectionHeader(parameter->getName(40));

    menu.addItem(menuReset, "Reset to default", parameter != nullptr, false);

    if (onEditValueRequested != nullptr)
        menu.addItem(menuTypeValue, "Type value...", true, false);

    menu.addSeparator();

    const bool learningThis = processor.isMidiLearning() && processor.getMidiLearnParameterID() == parameterID;
    const int mappedCC = processor.getMidiCCForParameter(parameterID);

    menu.addItem(menuMidiLearn, learningThis ? "Cancel MIDI Learn" : "MIDI Learn", true, false);
    menu.addItem(menuMidiClear,
                 mappedCC >= 0 ? "Clear MIDI mapping (CC " + juce::String(mappedCC) + ")" : "Clear MIDI mapping",
                 mappedCC >= 0, false);

    menu.addSeparator();
    menu.addItem(menuRemoveModulation, "Remove modulation", modulationActive && onRemoveModulation != nullptr, false);

    juce::Component::SafePointer<ModulatableKnob> safeThis(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMinimumWidth(180),
                       [safeThis](int result) mutable
                       {
                           if (auto* knob = safeThis.getComponent())
                               if (result != 0)
                                   knob->handleMenuResult(result);
                       });
}

void ModulatableKnob::handleMenuResult(int menuItemId)
{
    switch (menuItemId)
    {
    case menuReset:
        if (parameter != nullptr)
            setValue(getDoubleClickReturnValue(), juce::sendNotificationSync);
        break;

    case menuTypeValue:
        if (onEditValueRequested != nullptr)
            onEditValueRequested();
        break;

    case menuMidiLearn:
        if (processor.isMidiLearning() && processor.getMidiLearnParameterID() == parameterID)
            processor.cancelMidiLearn();
        else if (onMidiLearn != nullptr)
            onMidiLearn(parameterID);
        else
            processor.beginMidiLearn(parameterID);
        break;

    case menuMidiClear:
        if (onClearMidiMapping != nullptr)
            onClearMidiMapping(parameterID);
        else
            processor.clearMidiMapping(parameterID);
        break;

    case menuRemoveModulation:
        if (onRemoveModulation != nullptr)
            onRemoveModulation(parameterID);
        break;

    default:
        break;
    }

    refreshModulationDisplay();
    updateTooltip();
    repaint();
}

//==============================================================================
void ModulatableKnob::timerCallback()
{
    refreshModulationDisplay();
}

void ModulatableKnob::refreshModulationDisplay()
{
    bool needsRepaint = false;

    const bool nowActive = modulationTargetIndex >= 0 &&
                           processor.getModulationEngine().getNumConnectionsForTarget(modulationTargetIndex) > 0;

    const float offset = nowActive ? processor.getModulationDepth(parameterID) : 0.0f;

    if (nowActive != modulationActive)
    {
        modulationActive = nowActive;
        modulationSpanLow = offset;
        modulationSpanHigh = offset;
        needsRepaint = true;
        updateTooltip();
    }

    if (modulationActive)
    {
        const float previousLow = modulationSpanLow;
        const float previousHigh = modulationSpanHigh;

        // Relax towards the live offset first, then let the live offset widen
        // the span: the ring shows what the modulation has recently swept, and
        // shrinks within about a second when the depth is reduced.
        modulationSpanLow += (offset - modulationSpanLow) * kModulationSpanRelax;
        modulationSpanHigh += (offset - modulationSpanHigh) * kModulationSpanRelax;
        modulationSpanLow = juce::jmin(modulationSpanLow, offset);
        modulationSpanHigh = juce::jmax(modulationSpanHigh, offset);

        if (std::abs(modulationSpanLow - previousLow) > 1.0e-4f ||
            std::abs(modulationSpanHigh - previousHigh) > 1.0e-4f || std::abs(offset - modulationOffset) > 1.0e-4f)
            needsRepaint = true;
    }

    modulationOffset = offset;

    const bool learning = processor.isMidiLearning() && processor.getMidiLearnParameterID() == parameterID;

    if (learning != midiLearning)
    {
        midiLearning = learning;
        learnPhase = 0.0f;
        needsRepaint = true;
        updateTooltip();
    }

    if (midiLearning)
    {
        learnPhase += juce::MathConstants<float>::twoPi * 1.4f / static_cast<float>(juce::jmax(1, refreshRateHz));

        if (learnPhase > juce::MathConstants<float>::twoPi)
            learnPhase -= juce::MathConstants<float>::twoPi;

        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

void ModulatableKnob::updateTooltip()
{
    juce::String text;

    if (parameter != nullptr)
    {
        text << parameter->getName(48) << ": " << parameter->getCurrentValueAsText();

        const auto unit = parameter->getLabel();

        if (unit.isNotEmpty())
            text << " " << unit;
    }
    else
    {
        text << parameterID;
    }

    if (extraTooltip.isNotEmpty())
        text << "\n" << extraTooltip;

    if (modulationActive)
        text << "\nModulated";

    const int mappedCC = processor.getMidiCCForParameter(parameterID);

    if (mappedCC >= 0)
        text << "\nMIDI CC " << mappedCC;

    if (midiLearning)
        text << "\nMove a MIDI controller to map it";

    text << "\nDouble-click to reset, shift-drag for fine control";

    setTooltip(text);
}

//==============================================================================
ParameterValueLabel::ParameterValueLabel(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                                         bool showUnit)
    : includeUnit(showUnit)
{
    parameter = state.getParameter(parameterID);
    jassert(parameter != nullptr);

    setJustificationType(juce::Justification::centred);
    setEditable(false, true, false); // double-click to type a value
    setFont(EmberFonts::get(EmberFonts::Role::value));
    setMinimumHorizontalScale(0.7f);
    setColour(juce::Label::textColourId, EmberColours::textPrimary);
    setBorderSize({0, 2, 0, 2});

    if (parameter != nullptr)
    {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *parameter, [this](float newValue) { showValue(newValue); }, state.undoManager);
        attachment->sendInitialUpdate();
    }
}

ParameterValueLabel::~ParameterValueLabel() = default;

void ParameterValueLabel::beginEditing()
{
    showEditor();
}

void ParameterValueLabel::textWasEdited()
{
    if (parameter == nullptr || attachment == nullptr)
        return;

    const float normalised = juce::jlimit(0.0f, 1.0f, parameter->getValueForText(getText()));
    attachment->setValueAsCompleteGesture(parameter->convertFrom0to1(normalised));

    // Re-format from the parameter so a typed "3" becomes "3.00 dB".
    showValue(parameter->convertFrom0to1(parameter->getValue()));
}

void ParameterValueLabel::showValue(float denormalisedValue)
{
    if (parameter == nullptr)
        return;

    auto text = parameter->getText(parameter->convertTo0to1(denormalisedValue), 0);

    if (includeUnit)
    {
        const auto unit = parameter->getLabel();

        if (unit.isNotEmpty())
            text << " " << unit;
    }

    setText(text, juce::dontSendNotification);
}

//==============================================================================
EmberButton::EmberButton(const juce::String& buttonText, Style buttonStyle)
    : juce::TextButton(buttonText), style(buttonStyle)
{
    applyStyleColours();
}

EmberButton::~EmberButton() = default;

void EmberButton::setEmberStyle(Style newStyle)
{
    style = newStyle;
    applyStyleColours();
    repaint();
}

void EmberButton::attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID)
{
    setClickingTogglesState(true);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, parameterID, *this);
}

void EmberButton::applyStyleColours()
{
    switch (style)
    {
    case Style::neutral:
        setColour(juce::TextButton::buttonColourId, EmberColours::panelRaised);
        setColour(juce::TextButton::buttonOnColourId, EmberColours::accentDim);
        setColour(juce::TextButton::textColourOffId, EmberColours::textSecondary);
        setColour(juce::TextButton::textColourOnId, EmberColours::textPrimary);
        break;

    case Style::accent:
        setColour(juce::TextButton::buttonColourId, EmberColours::accentDim);
        setColour(juce::TextButton::buttonOnColourId, EmberColours::accent);
        setColour(juce::TextButton::textColourOffId, EmberColours::textPrimary);
        setColour(juce::TextButton::textColourOnId, EmberColours::backgroundDeep);
        break;

    case Style::ghost:
        setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour(juce::TextButton::buttonOnColourId, EmberColours::panelRaised);
        setColour(juce::TextButton::textColourOffId, EmberColours::textSecondary);
        setColour(juce::TextButton::textColourOnId, EmberColours::textPrimary);
        break;

    case Style::danger:
        setColour(juce::TextButton::buttonColourId, EmberColours::panelRaised);
        setColour(juce::TextButton::buttonOnColourId, EmberColours::warning.withMultipliedBrightness(0.6f));
        setColour(juce::TextButton::textColourOffId, EmberColours::warning);
        setColour(juce::TextButton::textColourOnId, EmberColours::textPrimary);
        break;
    }
}

//==============================================================================
EmberToggle::EmberToggle(const juce::String& buttonText, ToggleLook look) : juce::ToggleButton(buttonText)
{
    EmberStyleProps::setToggleLook(*this, look);
    setColour(juce::ToggleButton::textColourId, EmberColours::textSecondary);
}

EmberToggle::~EmberToggle() = default;

void EmberToggle::setLook(ToggleLook newLook)
{
    EmberStyleProps::setToggleLook(*this, newLook);
    repaint();
}

void EmberToggle::attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID)
{
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, parameterID, *this);
}

int EmberToggle::preferredWidth() const
{
    const float height = static_cast<float>(juce::jmax(12, getHeight()));
    const auto font = EmberFonts::forHeight(height, 0.46f, false);
    const auto text = getButtonText();

    float control = 0.0f;

    switch (EmberStyleProps::toggleLookFor(*this))
    {
    case ToggleLook::pill:
        control = juce::jlimit(11.0f, 22.0f, height * 0.58f) * 1.85f;
        break;
    case ToggleLook::led:
        control = juce::jlimit(7.0f, 14.0f, height * 0.44f);
        break;
    case ToggleLook::check:
        control = juce::jlimit(11.0f, 20.0f, height * 0.62f);
        break;
    }

    const float textWidth = text.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth(font, text) + height * 0.4f;

    return juce::roundToInt(control + textWidth + 6.0f);
}

//==============================================================================
EmberComboBox::EmberComboBox()
{
    setJustificationType(juce::Justification::centredLeft);
    setColour(juce::ComboBox::textColourId, EmberColours::textPrimary);
}

EmberComboBox::~EmberComboBox() = default;

void EmberComboBox::attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID)
{
    auto* parameter = state.getParameter(parameterID);
    jassert(parameter != nullptr);

    if (parameter == nullptr)
        return;

    attachTo(state, parameterID, parameter->getAllValueStrings());
}

void EmberComboBox::attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                             const juce::StringArray& itemNames)
{
    attachment.reset();
    clear(juce::dontSendNotification);

    for (int i = 0; i < itemNames.size(); ++i)
        addItem(itemNames[i].isEmpty() ? juce::String(i + 1) : itemNames[i], i + 1);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(state, parameterID, *this);
}

//==============================================================================
SectionHeader::SectionHeader(const juce::String& titleText) : title(titleText)
{
    setInterceptsMouseClicks(false, false);
}

SectionHeader::~SectionHeader() = default;

void SectionHeader::setTitle(const juce::String& newTitle)
{
    title = newTitle;
    repaint();
}

void SectionHeader::setTrailingText(const juce::String& newTrailingText)
{
    trailing = newTrailingText;
    repaint();
}

void SectionHeader::setAccentColour(juce::Colour colour)
{
    EmberStyleProps::setAccentColour(*this, colour);
    repaint();
}

int SectionHeader::preferredHeight(float uiScale)
{
    return juce::roundToInt(juce::jmax(16.0f, 20.0f * uiScale));
}

void SectionHeader::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 8.0f || area.getHeight() < 6.0f)
        return;

    const auto accent = EmberStyleProps::accentColourFor(*this);
    const float gap = juce::jmax(4.0f, area.getHeight() * 0.28f);

    // A short accent stroke, then the title, then a rule out to the right.
    const float tickWidth = juce::jmax(2.0f, area.getHeight() * 0.13f);
    const float tickHeight = juce::jmax(6.0f, area.getHeight() * 0.55f);

    g.setColour(accent);
    g.fillRoundedRectangle(
        juce::Rectangle<float>(tickWidth, tickHeight).withCentre({area.getX() + tickWidth * 0.5f, area.getCentreY()}),
        tickWidth * 0.5f);

    auto remaining = area.withTrimmedLeft(tickWidth + gap);

    if (trailing.isNotEmpty())
    {
        const auto trailingFont = EmberFonts::forHeight(area.getHeight(), 0.48f, false);
        const float trailingWidth = juce::GlyphArrangement::getStringWidth(trailingFont, trailing) + 4.0f;

        g.setFont(trailingFont);
        g.setColour(EmberColours::textDisabled);
        g.drawText(trailing, remaining.removeFromRight(juce::jmin(trailingWidth, remaining.getWidth())),
                   juce::Justification::centredRight, false);
    }

    if (title.isNotEmpty())
    {
        const auto titleFont = EmberFonts::forHeight(area.getHeight(), 0.54f, true);
        const auto upper = title.toUpperCase();
        const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, upper) + 3.0f;

        g.setFont(titleFont);
        g.setColour(EmberColours::textSecondary);
        g.drawText(upper, remaining.removeFromLeft(juce::jmin(titleWidth, remaining.getWidth())),
                   juce::Justification::centredLeft, false);
    }

    const auto rule = remaining.withTrimmedLeft(gap).withTrimmedRight(gap);

    if (rule.getWidth() > 4.0f)
        EmberLookAndFeel::drawHairline(g, rule, EmberColours::outlineStrong);
}

//==============================================================================
LevelMeter::LevelMeter(Mode meterMode, Orientation meterOrientation) : mode(meterMode), orientation(meterOrientation)
{
    setInterceptsMouseClicks(false, false);
    startTimerHz(30);
}

LevelMeter::~LevelMeter()
{
    stopTimer();
}

void LevelMeter::setSource(std::function<float()> supplier)
{
    source = std::move(supplier);
}

void LevelMeter::setRefreshRateHz(int hz)
{
    startTimerHz(juce::jlimit(10, 60, hz));
}

void LevelMeter::setDecibelRange(float minDb, float maxDb)
{
    minDecibels = juce::jmin(minDb, maxDb - 1.0f);
    maxDecibels = juce::jmax(maxDb, minDecibels + 1.0f);
}

void LevelMeter::setMaxGainReductionDb(float db)
{
    maxReductionDb = juce::jmax(1.0f, db);
}

void LevelMeter::setShowBackground(bool shouldShow)
{
    showBackground = shouldShow;
    repaint();
}

float LevelMeter::normalisedFor(float rawValue) const
{
    if (mode == Mode::gainReduction)
        return juce::jlimit(0.0f, 1.0f, rawValue / maxReductionDb);

    const float decibels = juce::Decibels::gainToDecibels(juce::jmax(0.0f, rawValue), minDecibels);
    return juce::jlimit(0.0f, 1.0f, (decibels - minDecibels) / (maxDecibels - minDecibels));
}

void LevelMeter::timerCallback()
{
    const float target = normalisedFor(source != nullptr ? source() : 0.0f);
    const float previousLevel = currentNormalised;
    const float previousPeak = peakNormalised;

    // Instant attack, eased release: at 30 Hz a linear fall reads as a stutter.
    currentNormalised = target > currentNormalised ? target : currentNormalised + (target - currentNormalised) * 0.35f;

    if (currentNormalised >= peakNormalised)
    {
        peakNormalised = currentNormalised;
        peakHoldFrames = 24;
    }
    else if (peakHoldFrames > 0)
    {
        --peakHoldFrames;
    }
    else
    {
        peakNormalised = juce::jmax(currentNormalised, peakNormalised - 0.02f);
    }

    if (std::abs(currentNormalised - previousLevel) > 0.002f || std::abs(peakNormalised - previousPeak) > 0.002f)
        repaint();
}

void LevelMeter::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 2.0f || area.getHeight() < 2.0f)
        return;

    const float corner = juce::jmin(3.0f, juce::jmin(area.getWidth(), area.getHeight()) * 0.3f);

    if (showBackground)
        EmberLookAndFeel::drawWell(g, area, corner);

    const auto inner = area.reduced(1.5f);

    if (inner.getWidth() < 1.0f || inner.getHeight() < 1.0f)
        return;

    const bool vertical = orientation == Orientation::vertical;
    const bool reduction = mode == Mode::gainReduction;
    const auto accent = EmberStyleProps::accentColourFor(*this);

    // Level grows from the bottom (or the left); gain reduction eats down from
    // the top (or in from the right), which is how a reduction reads.
    auto barFor = [inner, vertical, reduction](float proportion)
    {
        const float p = juce::jlimit(0.0f, 1.0f, proportion);

        if (vertical)
            return reduction ? inner.withHeight(inner.getHeight() * p)
                             : inner.withTop(inner.getBottom() - inner.getHeight() * p);

        return reduction ? inner.withLeft(inner.getRight() - inner.getWidth() * p)
                         : inner.withWidth(inner.getWidth() * p);
    };

    const auto bar = barFor(currentNormalised);

    if (bar.getWidth() > 0.3f && bar.getHeight() > 0.3f)
    {
        const bool hot = !reduction && currentNormalised > 0.93f;
        g.setColour(hot ? EmberColours::warning : accent);
        g.fillRect(bar);
    }

    if (peakNormalised > 0.004f)
    {
        const auto peakBar = barFor(peakNormalised);
        const float thickness = 1.5f;

        const auto tick =
            vertical
                ? juce::Rectangle<float>(inner.getX(), reduction ? peakBar.getBottom() - thickness : peakBar.getY(),
                                         inner.getWidth(), thickness)
                : juce::Rectangle<float>(reduction ? peakBar.getX() : peakBar.getRight() - thickness, inner.getY(),
                                         thickness, inner.getHeight());

        g.setColour(EmberColours::textPrimary.withAlpha(0.75f));
        g.fillRect(tick.getIntersection(inner));
    }
}

//==============================================================================
LabelledKnob::LabelledKnob(EmberAudioProcessor& processorToUse, const juce::String& parameterID,
                           const juce::String& caption)
    : knob(processorToUse, parameterID), valueLabel(processorToUse.getAPVTS(), parameterID)
{
    captionLabel.setJustificationType(juce::Justification::centred);
    captionLabel.setInterceptsMouseClicks(false, false);
    captionLabel.setColour(juce::Label::textColourId, EmberColours::textSecondary);
    captionLabel.setMinimumHorizontalScale(0.7f);

    setCaption(caption.isNotEmpty()
                   ? caption
                   : (knob.getParameter() != nullptr ? knob.getParameter()->getName(18) : parameterID));

    // The knob's "Type value..." item drives this readout's inline editor.
    knob.onEditValueRequested = [this] { valueLabel.beginEditing(); };

    addAndMakeVisible(captionLabel);
    addAndMakeVisible(knob);
    addAndMakeVisible(valueLabel);
}

LabelledKnob::~LabelledKnob() = default;

void LabelledKnob::setCaption(const juce::String& newCaption)
{
    captionLabel.setText(newCaption, juce::dontSendNotification);
}

void LabelledKnob::setAccentColour(juce::Colour colour)
{
    EmberStyleProps::setAccentColour(*this, colour);
    EmberStyleProps::setAccentColour(knob, colour);
    EmberStyleProps::setAccentColour(valueLabel, colour);
    repaint();
}

void LabelledKnob::resized()
{
    auto area = getLocalBounds();
    const float height = static_cast<float>(area.getHeight());

    const bool showCaption = height >= 52.0f;
    const bool showValue = height >= 38.0f;

    captionLabel.setVisible(showCaption);
    valueLabel.setVisible(showValue);

    if (showCaption)
    {
        const int captionHeight = juce::jlimit(11, 20, juce::roundToInt(height * 0.19f));
        captionLabel.setFont(EmberFonts::forHeight(static_cast<float>(captionHeight), 0.68f, false));
        captionLabel.setBounds(area.removeFromTop(captionHeight));
    }

    if (showValue)
    {
        const int valueHeight = juce::jlimit(12, 22, juce::roundToInt(height * 0.2f));
        valueLabel.setFont(EmberFonts::forHeight(static_cast<float>(valueHeight), 0.82f, false));
        valueLabel.setBounds(area.removeFromBottom(valueHeight));
    }

    const int side = juce::jmax(0, juce::jmin(area.getWidth(), area.getHeight()));
    knob.setBounds(juce::Rectangle<int>(side, side).withCentre(area.getCentre()));
}
} // namespace ember::gui
