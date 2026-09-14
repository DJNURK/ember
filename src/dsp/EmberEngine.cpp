#include "EmberEngine.h"

namespace ember
{
void EmberEngine::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    reset();
}

void EmberEngine::reset() {}

void EmberEngine::process(juce::AudioBuffer<float>&) {}
} // namespace ember
