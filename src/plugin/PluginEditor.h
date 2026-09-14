#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/SpectrumDisplay.h"
#include "gui/BandPanel.h"
#include "gui/GlobalBar.h"
#include "gui/ModPanel.h"
#include "gui/PresetBrowser.h"

namespace ember
{
/**
    The plugin window: preset bar across the top, global controls under it, the
    spectrum display in the middle, the selected band's controls below that, and
    the collapsible modulation panel at the bottom.

    It owns the look and feel, the tooltip window and the drag-and-drop
    container the modulation sources drag from and the knobs drop onto, and it
    wires the panels to each other — a knob accepting a modulation drop asks the
    modulation panel to create the routing, clicking a band in the spectrum
    retargets the band panel, and so on. Panels never reach across to each
    other directly; everything goes through the callbacks they expose, so each
    one stays independently testable.

    It holds no DSP state: everything shown comes from the processor's
    accessors. Opening and closing the window switches the engine's spectrum
    analysis on and off, so instances with no editor pay nothing for FFTs.
*/
class EmberAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer
{
public:
    explicit EmberAudioProcessorEditor(EmberAudioProcessor&);
    ~EmberAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void wirePanels();
    void selectBand(int band);

    EmberAudioProcessor& processorRef;
    gui::EmberLookAndFeel lookAndFeel;

    gui::PresetBar presetBar;
    gui::GlobalBar globalBar;
    gui::SpectrumDisplay spectrum;
    gui::BandPanel bandPanel;
    gui::ModPanel modPanel;

    // One tooltip window for the whole editor; every control's getTooltip()
    // feeds it.
    juce::TooltipWindow tooltips{this, 600};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberAudioProcessorEditor)
};
} // namespace ember
