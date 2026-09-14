#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "gui/EmberLookAndFeel.h"
#include "plugin/PluginProcessor.h"

/**
    Ember's shared controls.

    Every value control in the plugin should come from this file. They all
    attach through `juce::AudioProcessorValueTreeState`, so automation, host
    sync, undo and A/B all work without a panel doing anything; and they all
    carry the interaction contract the plugin promises:

        double-click  -> reset to the parameter's default
        shift-drag    -> fine control (also shift + mouse wheel)
        mouse wheel   -> adjust
        right-click   -> MIDI learn / clear mapping / remove modulation
        hover         -> tooltip with the parameter name and current value

    WHAT A PANEL HAS TO PROVIDE
    ---------------------------
      - The editor must own one `EmberLookAndFeel` and one `juce::TooltipWindow`
        (tooltips do not appear without one), and must derive from
        `juce::DragAndDropContainer` for modulation drag-and-drop to work.
      - A modulation source list starts a drag with
        `startDragging (ModulationDrag::makeDescription (flatSourceIndex), sourceComponent)`.
        Any `ModulatableKnob` whose parameter is a legal destination then accepts
        the drop and calls its `onModulationDropped` callback. The modulation
        panel owns what happens next (creating the routing); the knob only
        reports the gesture.
      - Set `EmberStyleProps::setAccentColour (control, EmberColours::band (b))`
        on anything inside a band region — knobs, toggles and meters all tint.

    THREADING
    ---------
    Everything here is message-thread only. DSP state is read exclusively
    through the processor's atomic accessors, from `juce::Timer` callbacks.
*/
namespace ember::gui
{
//==============================================================================
/**
    The drag payload that carries a modulation source between panels.

    The source list and the knob must agree on a format; this is it. Never
    hand-build the `juce::var` — go through these functions.
*/
namespace ModulationDrag
{
/** Payload for dragging modulation source `sourceFlatIndex` (the flat index
    from `ember::flatSourceIndex`, 0 .. kNumModSources-1). */
juce::var makeDescription(int sourceFlatIndex);

/** The flat source index carried by a drag description, or -1 if the
    description is not an Ember modulation drag or names no valid source. */
int sourceIndexFromDescription(const juce::var& description);
} // namespace ModulationDrag

//==============================================================================
/**
    A rotary knob bound to one APVTS parameter, and a modulation destination.

    Drawing is split in two: `EmberLookAndFeel::drawRotarySlider` paints the
    track, the value arc and the pointer; this class paints the outer ring the
    look-and-feel deliberately leaves free —

      - a faint full ring whenever the parameter has at least one modulation
        routing, so an assigned destination is visible even while the source
        sits still;
      - a bright arc covering the span the modulation has been observed to
        sweep, grown from the live offset reported by
        `EmberAudioProcessor::getModulationDepth` and relaxed back towards the
        current offset so the ring follows a depth that was just changed;
      - a dot at the instantaneous modulated position.

    The knob polls the processor on its own timer (30 Hz by default). Turn that
    off with `setModulationPollingEnabled (false)` if the owning panel prefers
    to drive every child from one timer, and call `refreshModulationDisplay()`
    from that timer instead.
*/
class ModulatableKnob : public juce::Slider, public juce::DragAndDropTarget, private juce::Timer
{
public:
    /** @param processorToUse  the plugin processor; kept by reference and must
                               outlive the knob (it always does — the editor
                               owns both)
        @param parameterIDToUse  an APVTS parameter ID, built with the
                               `ember::pid::` helpers, never a literal */
    ModulatableKnob(EmberAudioProcessor& processorToUse, const juce::String& parameterIDToUse);
    ~ModulatableKnob() override;

    //==========================================================================
    /** The parameter this knob drives. */
    const juce::String& getParameterID() const noexcept { return parameterID; }

    /** The parameter object, or nullptr if the ID did not resolve (a
        programming error — an assertion fires in debug builds). */
    juce::RangedAudioParameter* getParameter() const noexcept { return parameter; }

    /** True when the modulation graph currently routes anything here. */
    bool isModulated() const noexcept { return modulationActive; }

    /** True when this parameter is a legal modulation destination at all;
        false for bypass, solo and choice parameters. Non-destinations still
        work as knobs, they just refuse modulation drops. */
    bool isModulationDestination() const noexcept { return modulationTargetIndex >= 0; }

    //==========================================================================
    // Callbacks the owning panel supplies. All optional; an item whose callback
    // is missing is greyed out in the menu rather than silently doing nothing.

    /** A modulation source was dropped on this knob. The panel decides what to
        do (usually `ModulationEngine::addConnection`). */
    std::function<void(const juce::String& targetParameterID, int sourceFlatIndex)> onModulationDropped;

    /** "Remove modulation" was chosen. The panel removes every routing that
        targets `targetParameterID`. */
    std::function<void(const juce::String& targetParameterID)> onRemoveModulation;

    /** "MIDI Learn" was chosen. Defaults to
        `EmberAudioProcessor::beginMidiLearn` when left unset. */
    std::function<void(const juce::String& targetParameterID)> onMidiLearn;

    /** "Clear MIDI mapping" was chosen. Defaults to
        `EmberAudioProcessor::clearMidiMapping` when left unset. */
    std::function<void(const juce::String& targetParameterID)> onClearMidiMapping;

    /** "Type value..." was chosen. Set this to pop up your value readout's
        editor (`LabelledKnob` wires it for you); the menu item is hidden when
        it is unset. */
    std::function<void()> onEditValueRequested;

    //==========================================================================
    /** Opens the right-click menu programmatically (from a panel's own
        context menu, for instance). Always asynchronous. */
    void showParameterMenu();

    /** Re-reads modulation state and repaints if anything moved. Call this from
        your own timer when polling is disabled. */
    void refreshModulationDisplay();

    /** Whether the knob runs its own polling timer. Default: true. */
    void setModulationPollingEnabled(bool shouldPoll);

    /** Polling / repaint rate, clamped to 10..60 Hz. Default: 30. */
    void setRefreshRateHz(int hz);

    /** How much a shift-drag (or shift + wheel) slows things down.
        Default 0.2, clamped to 0.02..1.0. */
    void setFineDragFactor(float factor);

    /** An extra line appended to the tooltip — what the control does, in a few
        words. The parameter name, value, MIDI mapping and interaction hints are
        added automatically. */
    void setExtraTooltipText(const juce::String& text);

    //==========================================================================
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void valueChanged() override;
    void parentHierarchyChanged() override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    void timerCallback() override;
    void handleMenuResult(int menuItemId);
    void updateTooltip();
    juce::Rectangle<float> getRotaryArea();

    EmberAudioProcessor& processor;
    juce::String parameterID;
    juce::RangedAudioParameter* parameter{nullptr};
    int modulationTargetIndex{-1};

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    juce::String extraTooltip;

    // Modulation display state, all message thread.
    float modulationOffset{0.0f};
    float modulationSpanLow{0.0f};
    float modulationSpanHigh{0.0f};
    bool modulationActive{false};

    bool dragHighlight{false};
    bool midiLearning{false};
    float learnPhase{0.0f};

    // Fine-drag bookkeeping: JUCE derives the value from the distance since
    // mouse-down, so a scaled "virtual" position is accumulated rather than the
    // sensitivity being changed mid-drag (which would make the value jump).
    juce::Point<float> virtualDragPosition;
    juce::Point<float> lastRawDragPosition;
    float fineDragFactor{0.2f};
    bool forwardingToSlider{false};

    int refreshRateHz{30};
    bool pollingEnabled{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulatableKnob)
};

//==============================================================================
/**
    A parameter's value, formatted the way the parameter formats it, which the
    user can double-click and retype.

    Typing goes through the parameter's own `getValueForText`, so "-6", "-6 dB"
    and "-6dB" all work, and the edit is applied as one gesture so the host
    records a single automation move.
*/
class ParameterValueLabel : public juce::Label
{
public:
    /** @param showUnit  append the parameter's unit ("dB", "%", "Hz") */
    ParameterValueLabel(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                        bool showUnit = true);
    ~ParameterValueLabel() override;

    /** The parameter, or nullptr if the ID did not resolve. */
    juce::RangedAudioParameter* getParameter() const noexcept { return parameter; }

    /** Opens the inline editor — this is what `ModulatableKnob`'s
        "Type value..." menu item should be wired to. */
    void beginEditing();

protected:
    void textWasEdited() override;

private:
    void showValue(float denormalisedValue);

    juce::RangedAudioParameter* parameter{nullptr};
    bool includeUnit{true};
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterValueLabel)
};

//==============================================================================
/**
    A text button in the Ember style.

    The style only picks colours — the look-and-feel does the drawing, so a
    plain `juce::TextButton` looks right too; this just saves every panel
    repeating the same four `setColour` calls.
*/
class EmberButton : public juce::TextButton
{
public:
    enum class Style
    {
        neutral = 0, ///< the default: a raised grey key
        accent,      ///< the one primary action in a group
        ghost,       ///< outline only, for secondary actions in a dense row
        danger       ///< destructive: delete preset, clear all
    };

    explicit EmberButton(const juce::String& buttonText = {}, Style buttonStyle = Style::neutral);
    ~EmberButton() override;

    void setEmberStyle(Style newStyle);
    Style getEmberStyle() const noexcept { return style; }

    /** Turns the button into a latching toggle bound to a bool parameter. */
    void attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID);

private:
    void applyStyleColours();

    Style style{Style::neutral};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberButton)
};

//==============================================================================
/**
    A toggle: a sliding pill switch, a round lamp, or a tick box.

    Use `pill` for a mode ("Linear Phase"), `led` for a state that must be
    readable at a glance in a dense row ("Bypass", "Solo"), `check` for a list
    of options.
*/
class EmberToggle : public juce::ToggleButton
{
public:
    explicit EmberToggle(const juce::String& buttonText = {}, ToggleLook look = ToggleLook::pill);
    ~EmberToggle() override;

    void setLook(ToggleLook newLook);

    /** Binds to a bool parameter. */
    void attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID);

    /** Width that fits the switch plus its text at the toggle's current
        height — use it when laying out a row of toggles. */
    int preferredWidth() const;

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberToggle)
};

//==============================================================================
/**
    A combo box that fills itself from the parameter it is bound to.

    `attachTo` reads the parameter's own value strings, so a choice list never
    has to be duplicated in the GUI: add a style to `ember::StyleID` and it
    appears here.
*/
class EmberComboBox : public juce::ComboBox
{
public:
    EmberComboBox();
    ~EmberComboBox() override;

    /** Populates from the parameter's value strings (item IDs 1..n, in
        parameter order) and keeps the two in sync. */
    void attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID);

    /** As above, but with an explicit list — for a choice parameter whose
        display names should be grouped or abbreviated. `itemNames` must be in
        the same order, and the same length, as the parameter's own choices. */
    void attachTo(juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                  const juce::StringArray& itemNames);

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberComboBox)
};

//==============================================================================
/**
    A small-caps group title with a rule running out to the right, and an
    optional right-aligned annotation.

    Give it `preferredHeight (uiScale)` in your layout; it draws its own
    vertical centring, so an over-tall strip still looks correct.
*/
class SectionHeader : public juce::Component
{
public:
    explicit SectionHeader(const juce::String& titleText = {});
    ~SectionHeader() override;

    void setTitle(const juce::String& newTitle);
    const juce::String& getTitle() const noexcept { return title; }

    /** Right-aligned annotation: a count, a unit, a mode name. */
    void setTrailingText(const juce::String& newTrailingText);

    /** Colour of the short accent rule. Defaults to the global accent; band
        panels pass `EmberColours::band (b)`. */
    void setAccentColour(juce::Colour colour);

    /** Suggested height at a given UI scale (see `EmberFonts::scaleFor`). */
    static int preferredHeight(float uiScale);

    void paint(juce::Graphics&) override;

private:
    juce::String title;
    juce::String trailing;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionHeader)
};

//==============================================================================
/**
    A slim meter driven by a supplier function, so it works for anything the
    processor exposes without knowing what it is.

        LevelMeter m { LevelMeter::Mode::level };
        m.setSource ([&p, band] { return p.getBandLevel (band); });

        LevelMeter gr { LevelMeter::Mode::gainReduction };
        gr.setSource ([&p, band] { return p.getBandGainReductionDb (band); });

    The supplier is called on the message thread from the meter's own timer, so
    it must only touch the processor's atomic accessors. `Mode::level` expects a
    LINEAR peak in 0..1 and draws it on a dB scale; `Mode::gainReduction`
    expects POSITIVE decibels of reduction and draws downwards from the top.
*/
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    enum class Mode
    {
        level = 0,    ///< linear 0..1 peak, drawn bottom-up on a dB scale
        gainReduction ///< positive dB of reduction, drawn top-down
    };

    enum class Orientation
    {
        vertical = 0,
        horizontal
    };

    explicit LevelMeter(Mode meterMode = Mode::level, Orientation meterOrientation = Orientation::vertical);
    ~LevelMeter() override;

    /** The value supplier. Until one is set the meter simply reads empty. */
    void setSource(std::function<float()> supplier);

    /** Refresh rate, clamped to 10..60 Hz. Default: 30. */
    void setRefreshRateHz(int hz);

    /** Displayed range in `Mode::level`. Default -60 .. +6 dB. */
    void setDecibelRange(float minDb, float maxDb);

    /** Full-scale reduction in `Mode::gainReduction`. Default 18 dB. */
    void setMaxGainReductionDb(float db);

    /** Draws the recessed background behind the bar. Default: true; turn it off
        when the meter sits on top of artwork that already provides one. */
    void setShowBackground(bool shouldShow);

    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    float normalisedFor(float rawValue) const;

    std::function<float()> source;
    Mode mode;
    Orientation orientation;

    float currentNormalised{0.0f};
    float peakNormalised{0.0f};
    int peakHoldFrames{0};

    float minDecibels{-60.0f};
    float maxDecibels{6.0f};
    float maxReductionDb{18.0f};
    bool showBackground{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};

//==============================================================================
/**
    Caption, knob and typed value readout in one column — the unit almost every
    panel actually wants.

    Layout is proportional: the caption and the readout take a fixed share of
    the height and the knob fills a centred square in what is left, so the same
    component looks right from a 44 px cell to a 120 px one. Below about 52 px
    the caption is dropped and below about 38 px the readout goes too, leaving
    the bare knob — the tooltip still names it.
*/
class LabelledKnob : public juce::Component
{
public:
    /** @param caption  the text above the knob; when empty the parameter's own
                        display name is used. */
    LabelledKnob(EmberAudioProcessor& processor, const juce::String& parameterID, const juce::String& caption = {});
    ~LabelledKnob() override;

    ModulatableKnob& getKnob() noexcept { return knob; }
    ParameterValueLabel& getValueLabel() noexcept { return valueLabel; }

    void setCaption(const juce::String& newCaption);

    /** Tints knob, readout and caption — band panels pass
        `EmberColours::band (b)`. */
    void setAccentColour(juce::Colour colour);

    void resized() override;

private:
    juce::Label captionLabel;
    ModulatableKnob knob;
    ParameterValueLabel valueLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LabelledKnob)
};
} // namespace ember::gui
