#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "gui/EmberLookAndFeel.h"

namespace ember
{
/**
    The plugin window.

    Owns the look and feel, the tooltip window and the drag-and-drop container
    that the modulation sources drag from and the knobs drop onto, and arranges
    the panels. It deliberately holds no DSP state of its own: everything it
    shows comes from the processor's accessors.

    Opening and closing the editor switches the engine's spectrum analysis on
    and off, so a session full of instances with their windows closed does not
    pay for FFTs nobody is looking at.
*/
class EmberAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer
{
public:
    explicit EmberAudioProcessorEditor(EmberAudioProcessor&);
    ~EmberAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    EmberAudioProcessor& processorRef;
    gui::EmberLookAndFeel lookAndFeel;

    // A single tooltip window for the whole editor; every control's
    // getTooltip() feeds it.
    juce::TooltipWindow tooltips{this, 600};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberAudioProcessorEditor)
};
} // namespace ember
