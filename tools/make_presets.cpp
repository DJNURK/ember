// Generates the factory preset XML files from the design in
// resources/presets/MANIFEST.md.
//
// Presets are emitted from the REAL parameter layout rather than written by
// hand, so every id and every modulation target index is correct by
// construction instead of by proofreading.
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include "dsp/EmberTypes.h"
#include "dsp/modulation/ModTypes.h"
#include "dsp/modulation/ModulationEngine.h"
#include "dsp/styles/SaturationStyle.h"
#include "plugin/ParameterIDs.h"

int main(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    std::printf("make_presets: not yet implemented\n");
    return 1;
}
