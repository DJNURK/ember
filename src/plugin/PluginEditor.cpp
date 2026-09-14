#include "PluginEditor.h"

namespace ember
{
EmberAudioProcessorEditor::EmberAudioProcessorEditor(EmberAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processorRef(p), generic(p)
{
    addAndMakeVisible(generic);
    setResizable(true, true);
    setResizeLimits(800, 480, 3000, 2000);
    setSize(1100, 640);
}

void EmberAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff121214));
}

void EmberAudioProcessorEditor::resized()
{
    generic.setBounds(getLocalBounds());
}
} // namespace ember
