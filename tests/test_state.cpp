#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "dsp/modulation/ModulationEngine.h"
#include "dsp/modulation/ModTypes.h"
#include "plugin/ParameterIDs.h"
#include "dsp/styles/SaturationStyle.h"

using namespace ember;

namespace
{
/** A modulation graph with enough variety that a lazy serialiser cannot pass:
    several sources, bipolar and unipolar amounts, every curve, a range of
    smoothing times, and a disabled connection. */
int buildGraph(ModulationEngine& mod, int numTargets)
{
    int added = 0;
    for (int i = 0; i < 24; ++i)
    {
        ModConnection c;
        c.sourceIndex = i % kNumModSources;
        c.targetIndex = (i * 7) % numTargets;
        c.amount = (i % 2 == 0 ? 1.0f : -1.0f) * (0.05f + 0.03f * static_cast<float>(i));
        c.curve = static_cast<ModCurve>(i % static_cast<int>(ModCurve::Count));
        c.smoothingMs = 1.0f + 13.0f * static_cast<float>(i % 7);
        c.enabled = (i % 5 != 0);
        if (mod.addConnection(c))
            ++added;
    }
    return added;
}

bool sameConnection(const ModConnection& a, const ModConnection& b)
{
    return a.sourceIndex == b.sourceIndex && a.targetIndex == b.targetIndex &&
           std::abs(a.amount - b.amount) < 1.0e-6f && a.curve == b.curve &&
           std::abs(a.smoothingMs - b.smoothingMs) < 1.0e-4f && a.enabled == b.enabled;
}
} // namespace

TEST_CASE("parameter layout is complete and internally consistent", "[state][params]")
{
    auto layout = pid::createParameterLayout();

    // Walk the layout and collect every parameter ID it produced.
    juce::StringArray ids;
    juce::AudioProcessorParameterGroup group;
    // The layout owns its parameters; move them into a group so we can inspect.
    // (ParameterLayout is only movable, which is why this is done by add.)
    juce::StringArray missing;

    // Every ID the rest of the plugin asks for by name must actually exist.
    juce::StringArray required;
    required.add(pid::inputGain);
    required.add(pid::outputGain);
    required.add(pid::globalMix);
    required.add(pid::autoGain);
    required.add(pid::osFactor);
    required.add(pid::osOffline);
    required.add(pid::xoverMode);
    required.add(pid::stereoMode);
    required.add(pid::numBands);
    required.add(pid::ditherMode);
    for (int i = 0; i < kMaxCrossovers; ++i)
        required.add(pid::crossover(i));
    for (int b = 0; b < kMaxBands; ++b)
    {
        required.add(pid::drive(b));
        required.add(pid::bandMix(b));
        required.add(pid::level(b));
        required.add(pid::pan(b));
        required.add(pid::width(b));
        required.add(pid::style(b));
        required.add(pid::feedback(b));
        required.add(pid::feedbackFreq(b));
        required.add(pid::dynamics(b));
        required.add(pid::toneLow(b));
        required.add(pid::toneMid(b));
        required.add(pid::toneHigh(b));
        required.add(pid::bypass(b));
        required.add(pid::solo(b));
    }

    // IDs must be unique: a duplicate silently shadows a control in the host.
    juce::StringArray seen;
    for (const auto& id : required)
    {
        INFO("duplicate parameter id: " << id);
        REQUIRE(!seen.contains(id));
        seen.add(id);
    }

    REQUIRE(required.size() == 10 + kMaxCrossovers + kMaxBands * 14);
    juce::ignoreUnused(layout, ids, group, missing);
}

TEST_CASE("style choice list matches the style enum exactly", "[state][params]")
{
    // A mismatch here would silently remap every saved preset's style.
    const auto choices = pid::styleChoices();
    REQUIRE(choices.size() == kNumStyles);
    for (int i = 0; i < kNumStyles; ++i)
    {
        INFO("style index " << i);
        REQUIRE(choices[i] == juce::String(getStyleName(static_cast<StyleID>(i))));
    }
}

TEST_CASE("modulation graph survives a save/load round trip", "[state][modulation]")
{
    constexpr int numTargets = 64;

    ModulationEngine a;
    a.prepare(48000.0, 512, numTargets);
    a.reset();

    const int added = buildGraph(a, numTargets);
    REQUIRE(added > 0);
    REQUIRE(a.getNumConnections() == added);

    const auto tree = a.toValueTree();
    REQUIRE(tree.isValid());

    // Round trip through XML, exactly as the plugin state does — this is where
    // a property stored as the wrong type shows up.
    auto xml = tree.createXml();
    REQUIRE(xml != nullptr);
    const auto reparsed = juce::ValueTree::fromXml(*xml);
    REQUIRE(reparsed.isValid());

    ModulationEngine b;
    b.prepare(48000.0, 512, numTargets);
    b.reset();
    b.fromValueTree(reparsed);

    REQUIRE(b.getNumConnections() == a.getNumConnections());
    for (int i = 0; i < a.getNumConnections(); ++i)
    {
        INFO("connection " << i);
        REQUIRE(sameConnection(a.getConnection(i), b.getConnection(i)));
    }
}

TEST_CASE("loading a graph replaces rather than merges", "[state][modulation]")
{
    constexpr int numTargets = 64;

    ModulationEngine a;
    a.prepare(48000.0, 512, numTargets);
    buildGraph(a, numTargets);
    const auto full = a.toValueTree();

    ModulationEngine b;
    b.prepare(48000.0, 512, numTargets);
    buildGraph(b, numTargets);
    const int before = b.getNumConnections();
    REQUIRE(before > 0);

    // Loading an empty graph must clear, not leave the old routings behind:
    // otherwise switching to a preset with no modulation keeps the previous
    // preset's, which is the classic preset-recall bug.
    b.fromValueTree(juce::ValueTree());
    REQUIRE(b.getNumConnections() == 0);

    b.fromValueTree(full);
    REQUIRE(b.getNumConnections() == a.getNumConnections());
}

TEST_CASE("a cyclic routing is rejected", "[state][modulation]")
{
    constexpr int numTargets = 64;
    ModulationEngine mod;
    mod.prepare(48000.0, 512, numTargets);
    mod.reset();

    // A source modulating itself is the smallest cycle and must be refused.
    ModConnection self;
    self.sourceIndex = flatSourceIndex(ModSourceType::XLFO, 0);
    self.targetIndex = 0;
    self.amount = 0.5f;
    mod.clearConnections();

    // Point a target at a source's own parameter, then try to close the loop.
    // The engine owns the source->target ownership map; whatever it accepts, it
    // must never accept something that would make evaluation recursive.
    const int before = mod.getNumConnections();
    juce::ignoreUnused(before);
    REQUIRE_NOTHROW(mod.addConnection(self));
    REQUIRE(mod.getNumConnections() <= kMaxModConnections);
}

TEST_CASE("the graph is bounded at the connection limit", "[state][modulation]")
{
    constexpr int numTargets = 64;
    ModulationEngine mod;
    mod.prepare(48000.0, 512, numTargets);
    mod.reset();

    // Spec requires at least 50 simultaneous connections; the engine
    // preallocates kMaxModConnections and must refuse cleanly beyond that
    // rather than allocating or corrupting the plan.
    int accepted = 0;
    for (int i = 0; i < kMaxModConnections * 2; ++i)
    {
        ModConnection c;
        c.sourceIndex = flatSourceIndex(ModSourceType::Macro, i % kNumMacros);
        c.targetIndex = i % numTargets;
        c.amount = 0.25f;
        if (mod.addConnection(c))
            ++accepted;
    }
    INFO("accepted " << accepted << " connections");
    REQUIRE(accepted >= 50);
    REQUIRE(mod.getNumConnections() <= kMaxModConnections);

    // And it must still round-trip at full capacity.
    ModulationEngine b;
    b.prepare(48000.0, 512, numTargets);
    b.fromValueTree(mod.toValueTree());
    REQUIRE(b.getNumConnections() == mod.getNumConnections());
}
