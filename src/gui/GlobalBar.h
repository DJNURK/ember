#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "dsp/EmberTypes.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/Widgets.h"
#include "plugin/PluginProcessor.h"

/**
    Ember's global control strip — the row that sits above everything else and
    owns the settings that are not band-specific.

    It carries, left to right:

        input / output / mix knobs and auto-gain,
        the band count,
        oversampling (realtime and offline are separate parameters and are
            labelled so nobody confuses the two),
        the crossover and stereo modes,
        the latency the plugin currently reports to the host,
        A/B compare with a copy across, undo and redo,
        and the buttons that open the preset browser and show the modulation
            panel.

    THE STRIP COLLAPSES. There is more here than fits at the 800 px minimum
    width, so `resized()` measures what it can afford and moves the rest into an
    overflow menu on the "..." button, least-important first: offline
    oversampling, then stereo mode, then crossover mode, then realtime
    oversampling, then latency. Nothing is ever simply lost — everything that is
    hidden is reachable, with its current value ticked, from that one menu.

    THREADING. Everything here is message-thread only. The processor is read
    through its atomic accessors from one 30 Hz timer (latency, undo/redo
    availability, the active A/B slot and the MIDI-learn state); no other
    component in the editor is reached across to — the editor wires the two
    `std::function` hooks instead.
*/
namespace ember::gui
{
//==============================================================================
/**
    A small square button whose face is a vector icon rather than text.

    The look-and-feel still draws the key, so an icon button and a text button
    in the same row have exactly the same body, edge and hover behaviour.
*/
class IconButton : public juce::Button
{
public:
    enum class Icon
    {
        undo = 0, ///< anticlockwise circular arrow
        redo,     ///< the same, mirrored
        copy,     ///< two offset cards
        more      ///< three dots: the overflow menu
    };

    IconButton(Icon iconToDraw, const juce::String& componentName);
    ~IconButton() override;

    void setIcon(Icon newIcon);
    Icon getIcon() const noexcept { return icon; }

    void paintButton(juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    /** Draws `iconToDraw` centred in `area` — square, vector, no assets. */
    static void drawIcon(juce::Graphics&, Icon iconToDraw, juce::Rectangle<float> area, juce::Colour colour);

    Icon icon;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconButton)
};

//==============================================================================
/**
    The band count as a row of numbered chips, bound to `pid::numBands`.

    A combo box would read as one word of text that has to be opened before the
    current value means anything; this shows the answer at a glance — the lit
    chips ARE the bands, each in its own band colour, so the strip and the band
    panels below agree without a legend.

    Interaction matches every other Ember control: click or drag to set, wheel
    to step, double-click to return to the default, and a tooltip that names
    the parameter.
*/
class BandCountSelector : public juce::Component, public juce::SettableTooltipClient
{
public:
    BandCountSelector(EmberAudioProcessor& processorToUse, const juce::String& parameterIDToUse);
    ~BandCountSelector() override;

    /** The count currently shown, 1..kMaxBands. */
    int getCount() const noexcept { return currentCount; }

    /** Called after the user (or the parameter) changes the count. */
    std::function<void(int newCount)> onCountChanged;

    juce::String getTooltip() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    int segmentAt(juce::Point<int> position) const;
    juce::Rectangle<float> segmentBounds(int index) const;
    void applyCount(int newCount, bool asGesture);
    void valueFromParameter(float denormalisedValue);

    /** Closes the gesture a drag opened, if one is open. Anything that writes a
        complete gesture of its own has to call this first — a nested
        begin/endChangeGesture pair is an assertion in debug and a malformed
        automation write in release. */
    void finishDragGesture();

    EmberAudioProcessor& processor;
    juce::String parameterID;
    juce::RangedAudioParameter* parameter{nullptr};
    std::unique_ptr<juce::ParameterAttachment> attachment;

    int currentCount{3};
    int minimumCount{kMinBands};
    int maximumCount{kMaxBands};
    int hoveredSegment{-1};
    bool dragging{false};
    float wheelAccumulator{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandCountSelector)
};

//==============================================================================
/**
    The latency the plugin reports to the host, in milliseconds and in samples.

    Oversampling and the linear-phase crossover both change it, sometimes by a
    lot, so it is shown rather than buried: the value is pushed in from the
    strip's timer with `setLatency`, and only repaints when it actually moves.
*/
class LatencyReadout : public juce::Component, public juce::SettableTooltipClient
{
public:
    LatencyReadout();
    ~LatencyReadout() override;

    /** @param samples     the processor's current `getLatencySamples()`
        @param sampleRate  the current rate; <= 0 means "not prepared yet" and
                           shows a placeholder instead of a nonsense number */
    void setLatency(int samples, double sampleRate);

    void paint(juce::Graphics&) override;

private:
    int latencySamples{0};
    double currentSampleRate{0.0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LatencyReadout)
};

//==============================================================================
/**
    The "move a controller now" banner, shown across the strip for as long as
    `EmberAudioProcessor::isMidiLearning()` is true.

    It names the parameter being learned (and the CC it is already mapped to, if
    any), pulses so it cannot be mistaken for a static label, and swallows mouse
    clicks — while a learn is armed the strip's own controls are deliberately
    out of reach, and the way out is the Cancel button.
*/
class MidiLearnBanner : public juce::Component
{
public:
    MidiLearnBanner();
    ~MidiLearnBanner() override;

    /** Cancel was pressed. */
    std::function<void()> onCancel;

    /** @param parameterName  display name of the parameter being learned
        @param existingCC     its current CC, or -1 when unmapped */
    void setTarget(const juce::String& parameterName, int existingCC);

    /** Advances the pulse; call once per timer tick while visible. */
    void advancePulse();

    /** The strip's UI scale, so the text matches the rest of the row. */
    void setUiScale(float newScale);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    EmberButton cancelButton{"Cancel", EmberButton::Style::ghost};
    juce::String targetName;
    int mappedCC{-1};
    float pulsePhase{0.0f};
    float uiScale{1.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiLearnBanner)
};

//==============================================================================
/**
    The strip itself.

    The editor creates one, sets whichever hooks it wants, and gives it a row
    across the top — `preferredHeight (uiScale)` is the height it is designed
    for. It never talks to its sibling panels: everything the editor has to
    coordinate comes back through the hooks below.
*/
class GlobalBar : public juce::Component, private juce::Timer
{
public:
    explicit GlobalBar(EmberAudioProcessor& processorToUse);
    ~GlobalBar() override;

    //==========================================================================
    /** The height the strip is drawn for at a given UI scale (see
        `EmberFonts::scaleFor`). Give it less and it scales itself down; give it
        a lot more and the row simply centres in the space. */
    static int preferredHeight(float uiScale);

    //==========================================================================
    // Hooks the editor supplies. All optional — an unset hook hides the button
    // that would have called it rather than leaving a dead control on screen,
    // so set them before the first `resized()`.

    /** The preset button was pressed. */
    std::function<void()> onPresetBrowserRequested;

    /** The modulation button was toggled; the argument is its new state. */
    std::function<void(bool shouldBeVisible)> onModulationPanelToggled;

    /** The whole plugin state was replaced under the editor's feet — an A/B
        recall, an undo or a redo. Parameter attachments update themselves, but
        anything reading non-parameter state (the modulation graph, the selected
        band) should refresh here. */
    std::function<void()> onStateReplaced;

    /** The band count changed, so the band panels need re-laying out. */
    std::function<void(int newBandCount)> onBandCountChanged;

    //==========================================================================
    /** Keeps the modulation button in step when the panel is shown or hidden
        from somewhere else. Does not call `onModulationPanelToggled`. */
    void setModulationPanelVisible(bool shouldBeVisible);
    bool isModulationPanelVisible() const noexcept;

    /** Polls the processor immediately instead of waiting for the next tick —
        useful right after the editor restores something itself. */
    void refreshFromProcessor();

    //==========================================================================
    void paint(juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    //==========================================================================
    /** One optional element of the row, in priority order: the first to be
        dropped into the overflow menu is last. */
    enum class Optional
    {
        latency = 0,
        osRealtime,
        crossoverMode,
        stereoMode,
        osOffline,
        count
    };

    static constexpr int kNumOptional = static_cast<int>(Optional::count);

    void timerCallback() override;

    void updateLatency();
    void updateHistoryButtons();
    void updateSlotButtons();
    void updateMidiLearn();

    void handleSlotClicked(int slot);
    void handleCopyClicked();
    void handleUndoClicked();
    void handleRedoClicked();
    void stateWasReplaced();

    void showOverflowMenu();
    void addChoiceSubMenu(juce::PopupMenu& menu, const juce::String& title, const juce::String& parameterIDToUse);
    void setChoiceParameter(const juce::String& parameterIDToUse, int choiceIndex);

    /** "11.6 ms · 512 samples", or a placeholder before `prepareToPlay`. */
    juce::String latencyDescription() const;

    /** `base` px at the strip's own scale, clamped to a sane range. */
    int scaled(float base, int minimum, int maximum) const;

    bool shows(Optional item) const noexcept { return optionalVisible[static_cast<size_t>(item)]; }

    //==========================================================================
    EmberAudioProcessor& processor;

    LabelledKnob inputKnob, outputKnob, mixKnob;
    EmberToggle autoGainToggle{{}, ToggleLook::pill};

    BandCountSelector bandCount;

    EmberComboBox osRealtimeBox, osOfflineBox;
    EmberComboBox crossoverModeBox, stereoModeBox;
    LatencyReadout latencyReadout;

    EmberButton slotAButton{"A"}, slotBButton{"B"};
    IconButton copyButton{IconButton::Icon::copy, "copySlot"};
    IconButton undoButton{IconButton::Icon::undo, "undo"};
    IconButton redoButton{IconButton::Icon::redo, "redo"};
    IconButton overflowButton{IconButton::Icon::more, "more"};

    EmberButton presetButton{"Presets"}, modulationButton{"Mod"};

    MidiLearnBanner midiBanner;

    //==========================================================================
    // Painted furniture, all rebuilt by resized().
    struct Caption
    {
        juce::Rectangle<int> bounds;
        juce::String text;   ///< stored ready to draw (already upper-cased)
        bool centred{false}; ///< knob captions centre over the knob; the rest run left
    };

    std::vector<Caption> captions;
    std::vector<int> dividers;
    std::array<bool, static_cast<size_t>(kNumOptional)> optionalVisible{};

    float uiScale{1.0f};

    // Polled state, kept so the strip only touches its children when something
    // has actually changed.
    int lastLatencySamples{-1};
    double lastSampleRate{-1.0};
    int lastActiveSlot{-1};
    int copyFlashTicks{0};
    bool lastCanUndo{true};
    bool lastCanRedo{true};
    juce::String lastUndoDescription, lastRedoDescription;
    bool lastMidiLearning{false};
    juce::String lastMidiLearnParameter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GlobalBar)
};
} // namespace ember::gui
