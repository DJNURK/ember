#include "plugin/PluginProcessor.h"
#include <cstdio>
int main()
{
    ember::EmberAudioProcessor p;
    int n = 0;
    for (auto* q : p.getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*>(q))
        {
            std::printf("%s\n", w->paramID.toRawUTF8());
            ++n;
        }
    std::fprintf(stderr, "%d parameters\n", n);
    return 0;
}
