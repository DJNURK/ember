#include <catch2/catch_test_macros.hpp>
#include "dsp/EmberEngine.h"

TEST_CASE("engine prepares and processes without crashing", "[smoke]")
{
    ember::EmberEngine engine;
    engine.prepare({48000.0, 512, 2});
    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    engine.process(buffer);
    REQUIRE(buffer.getNumSamples() == 512);
}
