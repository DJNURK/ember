#pragma once
#include <juce_dsp/juce_dsp.h>

namespace ember
{
/** Placeholder engine — replaced by the real multiband chain in M2. */
class EmberEngine
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void process(juce::AudioBuffer<float>& buffer);

private:
    double sampleRate { 44100.0 };
};
} // namespace ember
