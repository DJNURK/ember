#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace ember
{
class EmberAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit EmberAudioProcessorEditor(EmberAudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    EmberAudioProcessor& processorRef;
    juce::GenericAudioProcessorEditor generic;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberAudioProcessorEditor)
};
} // namespace ember
