// The plugin target needs at least one source of its own, and JUCE's format
// wrappers resolve createPluginFilter() from here rather than from the static
// library, where a linker is entitled to drop an object nothing references.
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ember::EmberAudioProcessor();
}
