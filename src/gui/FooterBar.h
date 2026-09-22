#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/Widgets.h"
#include "gui/tutorial/TourAnchor.h"
#include "gui/tutorial/TourTargets.h"

namespace ember
{
class EmberAudioProcessor;
}

namespace ember::gui
{
/**
    The strip along the bottom: the levels that apply to the whole plugin, what
    it is costing, and a line that explains whatever the mouse is over.

    WHY THE LEVELS LIVE HERE
    ------------------------
    Input, Output and Mix are not band controls and not global *settings* - they
    are the plugin's top and tail. Putting them in the header mixed them in with
    band count, oversampling and crossover mode, which are decisions you make
    once; these three are things you ride. Along the bottom they read as the
    output stage of a piece of hardware, which is what they are.

    WHY THERE ARE NO TOOLTIP POPUPS
    -------------------------------
    A popup that appears over the interface hides the thing it is describing,
    and in a plugin full of small controls it spends most of its life covering a
    neighbouring knob. The footer carries the same text in a fixed place, so
    reading it never costs you sight of the control. `juce::TooltipWindow` is
    switched off globally; this is where that text goes instead.
*/
class FooterBar : public juce::Component, private juce::Timer
{
public:
    explicit FooterBar(EmberAudioProcessor&);
    ~FooterBar() override;

    /** The design's fixed height, scaled. */
    static int preferredHeight(float uiScale);

    /** Shows `text` on the tooltip line. Empty clears it. */
    void setHint(const juce::String& text);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Walks up from the component under the mouse until something has a
        tooltip, so hovering a knob's label explains the knob. */
    juce::String hintForComponentUnderMouse() const;

    EmberAudioProcessor& processor;

    LabelledKnob inputKnob, outputKnob, mixKnob;
    EmberToggle autoGainToggle{{}, ToggleLook::pill};

    tutorial::TourAnchor cpuAnchor{tutorial::TourTargets::footerCpu};

    juce::String hint;
    juce::Rectangle<int> hintArea, readoutArea;

    /** Smoothed so the number is readable rather than a blur. The processor
        reports a per-block figure that jitters by several percent. */
    float smoothedCpuPercent{0.0f};
    int latencySamples{0};
    double latencyRate{0.0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FooterBar)
};
} // namespace ember::gui
