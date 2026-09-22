#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include "dsp/EmberTypes.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/Widgets.h"
#include "gui/MotionClock.h"
#include "gui/tutorial/TourAnchor.h"
#include "plugin/PluginProcessor.h"

/**
    The editing surface for ONE band.

    Ember has up to six bands but only ever edits one at a time, so this panel
    is the whole per-band parameter set — style, drive, mix, level, pan, width,
    feedback, dynamics, tone, bypass and solo — retargeted at whichever band the
    editor selects.

    WHY IT DOES NOT REBUILD ON `setBand`
    ------------------------------------
    Every control here is bound to an APVTS parameter through an attachment
    created in its constructor, and the shared widgets deliberately expose no
    way to rebind one. Tearing the controls down and building them again on each
    band change would mean destroying live attachments, re-reading the whole
    parameter set and re-laying out mid-paint — visible as a flicker, and a
    gesture in flight would be lost.

    So one control set per band is built LAZILY, the first time that band is
    selected, and kept. `setBand` then only hides one set, shows another and
    re-runs the layout: the attachments it needs are already bound and already
    in sync. Hidden sets have their modulation polling switched off, so the
    five bands you are not looking at cost nothing per frame.

    THREADING
    ---------
    Message thread only. DSP state is read exclusively through the processor's
    atomic accessors — `getBandLevel` and `getBandGainReductionDb` through the
    shared `LevelMeter`s, the band's frequency span through APVTS raw values
    from this panel's own 30 Hz timer.
*/
namespace ember::gui
{
class BandPanel : public juce::Component, private juce::Timer
{
public:
    explicit BandPanel(EmberAudioProcessor& processorToUse);
    ~BandPanel() override;

    //==========================================================================
    /** Edits `newBandIndex` (zero-based, clamped to 0 .. kMaxBands-1).

        This does NOT write the selection back to the processor: the editor owns
        `EmberAudioProcessor::setSelectedBand` and drives this panel from it, so
        the data flows one way and a selection can never bounce between the two. */
    void setBand(int newBandIndex);

    /** Lights a module without selecting it - used while the mouse is over that
        band's node on the main display, so the two halves of the EQ editor are
        visibly the same thing. -1 clears it. */
    void setHighlightedBand(int bandIndex);

    /** Hands the strip the editor's clock so module widths can animate rather
        than snap. Without one the widths jump, which is correct but reads as a
        relayout rather than as a module opening. */
    void setMotionClock(MotionClock&);

    /** The band currently being edited, zero-based. */
    int getBand() const noexcept { return currentBand; }

    //==========================================================================
    // Modulation and MIDI gestures are REPORTED, never acted on: this panel
    // does not know the modulation engine exists. The editor forwards these to
    // the modulation panel, which owns the graph.

    /** A module was clicked. The editor owns selection, so the strip only
        reports the click rather than selecting anything itself. */
    std::function<void(int bandIndex)> onBandClicked;

    /** A modulation source was dropped on one of this band's knobs. */
    std::function<void(const juce::String& targetParameterID, int sourceFlatIndex)> onModulationDropped;

    /** "Remove modulation" was chosen on one of this band's knobs. */
    std::function<void(const juce::String& targetParameterID)> onRemoveModulation;

    /** "MIDI Learn" was chosen. When unset, `EmberAudioProcessor::beginMidiLearn`
        is called directly, which is the shared widgets' documented default. */
    std::function<void(const juce::String& targetParameterID)> onMidiLearn;

    /** "Clear MIDI mapping" was chosen. When unset,
        `EmberAudioProcessor::clearMidiMapping` is called directly. */
    std::function<void(const juce::String& targetParameterID)> onClearMidiMapping;

    //==========================================================================
    /** Re-reads the modulation rings of the visible controls. The knobs poll on
        their own as well; call this to update them the instant the graph
        changes rather than on the next poll. */
    void refreshModulationDisplay();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    //==========================================================================
    class DetentedKnob;    ///< a knob that snaps to its centre value while dragging
    class DynamicsControl; ///< the bipolar dynamics knob plus its reduction meter
    struct BandControls;   ///< one band's complete, already-attached control set

    /** The knob groups, left to right in the widest layout. */
    static constexpr int kSaturationGroup = 0;
    static constexpr int kStereoGroup = 1;
    static constexpr int kFeedbackGroup = 2;
    static constexpr int kDynamicsGroup = 3;
    static constexpr int kToneGroup = 4;
    static constexpr int kNumGroups = 5;

    /** A titled group: a small-caps header above a sunken well of knobs. The
        weight is the group's share of a row's width. */
    struct GroupBox
    {
        SectionHeader header;
        juce::Rectangle<int> well;
        float weight{1.0f};
    };

    //==========================================================================
    void timerCallback() override;

    /** Slot each band's module occupies. Computed in resized(), used by paint()
        and by hit-testing, so chrome and clicks can never disagree. */
    std::array<juce::Rectangle<int>, static_cast<size_t>(kMaxBands)> moduleBounds{};

    /** An empty, unpainted component over each module's slot, so a module is a
        thing that can be found by name - by a tour that wants to point at a
        whole band, and by the test that asserts no band's controls are drawn
        outside the module they belong to. A module is otherwise only a
        rectangle in `paint`, which nothing outside this class can see. */
    std::array<std::unique_ptr<tutorial::TourAnchor>, static_cast<size_t>(kMaxBands)> moduleAnchor;
    int highlightedBand{-1};

    /** Each module's width weight, 1.0 collapsed and 1.6 selected. */
    std::array<Animated, static_cast<size_t>(kMaxBands)> moduleWeight;
    MotionClock::Registration motionRegistration;

    /** Lays one unselected module out: title strip, Drive, heat bar. */
    void layoutCompactModule(juce::Rectangle<int> slot, BandControls& controls);

    /** Draws a module's chrome: raised panel, title bar warmed by heat. */
    void paintModuleChrome(juce::Graphics&, int band, juce::Rectangle<int> slot, bool selected);

    void mouseDown(const juce::MouseEvent&) override;

    BandControls& controlsFor(int bandIndex);
    void showControlsFor(int bandIndex);

    /** Installs this panel's forwarding callbacks and the tooltip line that
        says what the control does. */
    void wireKnob(ModulatableKnob& knob, const juce::String& hint);

    void updateHeaderStrings();
    int activeBandCount() const;
    juce::String frequencyRangeText() const;
    float uiScale() const;

    /** Rebuilds the fonts `paint` uses. Called from the layout, because a
        `juce::Font` allocates and the meters push this panel through `paint`
        up to 30 times a second. */
    void cacheHeaderFonts();

    void layoutHeader(juce::Rectangle<int> area, BandControls& controls);
    void layoutKnobGrid(juce::Rectangle<int> area, BandControls& controls);
    void layoutGroups(juce::Rectangle<int> area, BandControls& controls);
    void layoutRow(juce::Rectangle<int> row, const int* groupIndices, int numGroups, BandControls& controls);
    void layoutGroup(int groupIndex, juce::Rectangle<int> bounds, BandControls& controls);

    //==========================================================================
    EmberAudioProcessor& processor;

    std::array<std::unique_ptr<BandControls>, static_cast<size_t>(kMaxBands)> bandControls;
    int currentBand{0};

    SectionHeader styleHeader{"Style"};
    std::array<GroupBox, static_cast<size_t>(kNumGroups)> groups;

    /** The band's output level, in the header. Owned here rather than per band
        because it reads whichever band is selected. */
    LevelMeter outputMeter{LevelMeter::Mode::level, LevelMeter::Orientation::horizontal};

    // Areas the panel paints itself, filled in by the layout.
    juce::Rectangle<int> headerArea, chipArea, titleArea, meterLabelArea;
    juce::Rectangle<int> feedbackLinkA, feedbackLinkB;

    juce::String rangeText;
    bool bandActive{true};
    int lastStyleIndex{-1};

    // Built by cacheHeaderFonts() from the areas above, never inside paint().
    juce::Font chipFont{juce::FontOptions{}};
    juce::Font bandNameFont{juce::FontOptions{}};
    juce::Font rangeFont{juce::FontOptions{}};
    juce::Font meterLabelFont{juce::FontOptions{}};

    // Cached raw parameter pointers for the header readout: atomics, read on
    // the message thread from timerCallback.
    std::atomic<float>* numBandsValue{nullptr};
    std::array<std::atomic<float>*, static_cast<size_t>(kMaxCrossovers)> crossoverValues{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandPanel)
};
} // namespace ember::gui
