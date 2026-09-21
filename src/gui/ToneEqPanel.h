#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "dsp/BandFx.h"

namespace ember
{
class EmberAudioProcessor;
}

namespace ember::gui
{
/**
    One band's tone stage, as a curve you can grab.

    Three anonymous knobs labelled Low / Mid / High told you three numbers and
    nothing about their shape - which frequencies they touched, how wide the
    mid bell was, whether the low shelf reached far enough to matter. This
    draws the response and lets you drag it.

    WHY IT OWNS A ToneStack
    -----------------------
    The curve is not modelled: this panel keeps its own `ToneStack`, configures
    it from the same parameters the audio path uses and asks it for its
    magnitude. Same class, same coefficient design, so the drawn line cannot
    drift from the filters - and `test_eq` has already checked that class
    against a measured sweep of real audio. Reading the *processing* instance
    instead would be a data race: its coefficients are written on the audio
    thread.

    The panel never processes a sample through its copy.

    OUTSIDE THE BAND, THE CURVE FADES
    ---------------------------------
    A band's tone stage only shapes that band. Drawing its curve across the
    full spectrum at full strength would claim otherwise, so it fades to 20 %
    outside the band's own frequency span - visible enough to place, faint
    enough not to lie.
*/
class ToneEqPanel : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ToneEqPanel(EmberAudioProcessor&, int bandIndex);
    ~ToneEqPanel() override;

    /** Collapsed form: a 40x14 sparkline of the curve, for an unselected
        module where the full editor would not be legible. */
    void setCompact(bool shouldBeCompact);

    /** The smallest size at which the full editor is worth drawing. */
    static juce::Point<int> minimumUsefulSize() { return {220, 90}; }

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    juce::String getTooltip() override;

private:
    enum class Node
    {
        low = 0,
        mid,
        high,
        count
    };

    static constexpr int kNumNodes = static_cast<int>(Node::count);

    void timerCallback() override;

    /** Re-reads the parameters into the drawing filter and rebuilds the path.
        Returns true when anything actually moved. */
    bool refreshCurve();

    float xForFrequency(float hz) const;
    float frequencyForX(float x) const;
    float yForDecibels(float db) const;
    float decibelsForY(float y) const;

    juce::Point<float> positionOf(Node) const;
    Node nodeAt(juce::Point<float>) const;

    juce::RangedAudioParameter* gainParam(Node) const;
    juce::RangedAudioParameter* freqParam(Node) const;

    /** Writes a parameter as a complete host gesture. Dragging opens one on
        mouseDown and closes it on mouseUp instead, so a drag is one automation
        move rather than sixty. */
    void setParam(juce::RangedAudioParameter*, float denormalised, bool asGesture);

    EmberAudioProcessor& processor;
    int band;

    /** For drawing only - never processes audio. */
    ToneStack drawingFilter;

    juce::Path curve;
    juce::Rectangle<float> plot;

    /** The band's own span, so the curve can fade outside it. */
    float bandLowHz{20.0f}, bandHighHz{20000.0f};

    Node hovered{Node::count};
    Node dragging{Node::count};
    bool compact{false};
    bool draggingQ{false};

    /** Last values seen, so the 30 Hz poll only rebuilds when something moved. */
    std::array<float, 3> shownGain{};
    std::array<float, 3> shownFreq{};
    float shownQ{0.0f};
    float shownHeat{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneEqPanel)
};
} // namespace ember::gui
