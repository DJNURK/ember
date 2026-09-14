#include "PluginEditor.h"

namespace ember
{
EmberAudioProcessorEditor::EmberAudioProcessorEditor(EmberAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processorRef(p)
{
    setLookAndFeel(&lookAndFeel);

    // The spectrum FFT only runs while a window is open.
    processorRef.getEngineSpectrumEnabled(true);

    const auto stored = processorRef.getEditorBounds();
    setResizable(true, true);
    setResizeLimits(800, 480, 3000, 2000);
    getConstrainer()->setFixedAspectRatio(0.0);
    setSize(stored.getWidth(), stored.getHeight());
}

EmberAudioProcessorEditor::~EmberAudioProcessorEditor()
{
    processorRef.getEngineSpectrumEnabled(false);
    setLookAndFeel(nullptr);
}

void EmberAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(gui::EmberColours::backgroundDeep);
}

void EmberAudioProcessorEditor::resized()
{
    // Remember the size so the window comes back the way the user left it.
    processorRef.setEditorBounds(getWidth(), getHeight());
}
} // namespace ember
