#include <catch2/catch_test_macros.hpp>
#include "TestHelpers.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ParameterIDs.h"
#include "dsp/modulation/ModTypes.h"

using namespace ember;
using namespace embertest;

namespace
{
void prepareAndRun(EmberAudioProcessor& p, int blocks = 64, int blockSize = 128)
{
    p.prepareToPlay(48000.0, blockSize);
    juce::AudioBuffer<float> buf(2, blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i)
    {
        fillWhiteNoise(buf, 0x55u + static_cast<uint32_t>(i), 0.25f);
        p.processBlock(buf, midi);
    }
}

void setParam(EmberAudioProcessor& p, const juce::String& id, float realValue)
{
    auto* param = p.getAPVTS().getParameter(id);
    REQUIRE(param != nullptr);
    param->setValueNotifyingHost(param->getNormalisableRange().convertTo0to1(realValue));
}
} // namespace

TEST_CASE("modulation sources are actually driven by their parameters", "[processor][modulation]")
{
    // Regression test for a whole subsystem being inert: the modulation engine
    // exposes setXLfoParameters/setMacroParameters/... and if the processor
    // never calls them, every source sits at its default forever. Nothing else
    // in the suite would notice — the plugin still passes audio, still saves
    // state, still validates — it just silently does not modulate.
    EmberAudioProcessor proc;

    setParam(proc, pid::macro(0), 100.0f);
    setParam(proc, pid::macro(1), 0.0f);
    // Long enough for the sources' own smoothing to settle (macros smooth over
    // 20 ms by default, which is several blocks at this size).
    prepareAndRun(proc);

    const int macro0 = flatSourceIndex(ModSourceType::Macro, 0);
    const int macro1 = flatSourceIndex(ModSourceType::Macro, 1);

    const float hi = proc.getModulationEngine().getSourceValue(macro0);
    const float lo = proc.getModulationEngine().getSourceValue(macro1);

    INFO("macro 1 at 100% reads " << hi << ", macro 2 at 0% reads " << lo);
    REQUIRE(hi > 0.9f);
    REQUIRE(lo < 0.1f);
}

TEST_CASE("an LFO advances once the transport is running", "[processor][modulation]")
{
    EmberAudioProcessor proc;
    setParam(proc, pid::lfoRate(0), 8.0f); // fast enough to move within a few blocks
    setParam(proc, pid::lfoDepth(0), 100.0f);

    proc.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;

    const int lfo0 = flatSourceIndex(ModSourceType::XLFO, 0);

    float minV = 1.0e9f, maxV = -1.0e9f;
    for (int i = 0; i < 64; ++i)
    {
        fillWhiteNoise(buf, 0x99u + static_cast<uint32_t>(i), 0.1f);
        proc.processBlock(buf, midi);
        const float v = proc.getModulationEngine().getSourceValue(lfo0);
        minV = juce::jmin(minV, v);
        maxV = juce::jmax(maxV, v);
    }

    INFO("LFO swept " << minV << " .. " << maxV);
    REQUIRE(maxV - minV > 0.5f); // it must genuinely move, not sit still
}

TEST_CASE("a macro routed to a drive actually changes the offset", "[processor][modulation]")
{
    EmberAudioProcessor proc;

    const int target = proc.getModulationTargetIndex(pid::drive(0));
    REQUIRE(target >= 0);

    ModConnection c;
    c.sourceIndex = flatSourceIndex(ModSourceType::Macro, 0);
    c.targetIndex = target;
    c.amount = 0.5f;
    c.curve = ModCurve::Linear;
    c.smoothingMs = 0.0f;
    REQUIRE(proc.getModulationEngine().addConnection(c));

    setParam(proc, pid::macro(0), 100.0f);
    prepareAndRun(proc, 32);

    const float offset = proc.getModulationEngine().getModulationOffset(target);
    INFO("modulation offset on band 1 drive: " << offset);
    REQUIRE(offset > 0.25f);
}

TEST_CASE("every parameter survives a state save and restore", "[processor][state]")
{
    // This mirrors what pluginval's state test does, and it is worth owning
    // locally: pluginval randomises the values it uses, so a parameter that
    // restores incorrectly only shows up on some seeds.
    EmberAudioProcessor proc;

    auto& params = proc.getParameters();
    REQUIRE(params.size() > 100);

    uint32_t seed = 0x13579BDFu;
    auto nextUnit = [&seed]() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };

    // Randomise, capture, randomise again, restore, compare.
    juce::Array<float> wanted;
    for (auto* p : params)
    {
        const float v = nextUnit();
        p->setValueNotifyingHost(v);
        wanted.add(p->getValue());
    }

    juce::MemoryBlock state;
    proc.getStateInformation(state);

    for (auto* p : params)
        p->setValueNotifyingHost(nextUnit());

    proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    for (int i = 0; i < params.size(); ++i)
    {
        auto* p = params[i];
        const auto name = dynamic_cast<juce::AudioProcessorParameterWithID*>(p) != nullptr
                              ? dynamic_cast<juce::AudioProcessorParameterWithID*>(p)->paramID
                              : juce::String(i);
        INFO("parameter '" << name << "' was " << wanted[i] << " and came back " << p->getValue());
        REQUIRE(std::abs(p->getValue() - wanted[i]) < 0.01f);
    }
}

TEST_CASE("state restore is stable across many randomisations", "[processor][state]")
{
    // The same check over many seeds, because the failure this guards against
    // was seed-dependent and showed up roughly one run in three.
    for (uint32_t trial = 0; trial < 25; ++trial)
    {
        EmberAudioProcessor proc;
        auto& params = proc.getParameters();

        uint32_t seed = 0xA5A5u + trial * 7919u;
        auto nextUnit = [&seed]() noexcept
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        };

        juce::Array<float> wanted;
        for (auto* p : params)
        {
            p->setValueNotifyingHost(nextUnit());
            wanted.add(p->getValue());
        }

        juce::MemoryBlock state;
        proc.getStateInformation(state);
        for (auto* p : params)
            p->setValueNotifyingHost(nextUnit());
        proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

        for (int i = 0; i < params.size(); ++i)
        {
            auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(params[i]);
            INFO("trial " << trial << ", parameter '" << (withID != nullptr ? withID->paramID : juce::String(i))
                          << "'");
            REQUIRE(std::abs(params[i]->getValue() - wanted[i]) < 0.01f);
        }
    }
}

TEST_CASE("every factory preset loads and actually changes the plugin", "[processor][presets]")
{
    // test_presets.cpp checks the XML is structurally valid against the
    // parameter layout. That is not the same as the preset working: a preset
    // whose parameter block never reaches the APVTS would still pass it and
    // silently do nothing when a user clicks it. This loads each one through
    // the real PresetManager and checks the plugin's state actually moved.
    EmberAudioProcessor proc;
    auto& presets = proc.getPresetManager();

    const auto all = presets.getAllPresets();
    INFO("factory presets visible to the manager: " << all.size());
    REQUIRE(all.size() >= 30);

    auto snapshot = [&proc]
    {
        juce::Array<float> values;
        for (auto* p : proc.getParameters())
            values.add(p->getValue());
        return values;
    };

    // "Init" is every parameter at its default, so loading it from the default
    // state legitimately changes nothing; it is the reference, not a failure.
    const auto initState = snapshot();

    int changed = 0;
    for (const auto& info : all)
    {
        INFO("loading preset '" << info.name << "' (" << info.category << ")");
        REQUIRE(presets.loadPreset(info));
        REQUIRE(presets.getCurrentPresetName() == info.name);

        const auto loaded = snapshot();
        REQUIRE(loaded.size() == initState.size());

        bool differs = false;
        for (int i = 0; i < loaded.size() && !differs; ++i)
            differs = std::abs(loaded[i] - initState[i]) > 1.0e-4f;
        if (differs)
            ++changed;

        // Whatever it loaded must be finite and in range, and must not have
        // left the processor unable to run.
        proc.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        fillWhiteNoise(buf, 0x31337u, 0.3f);
        proc.processBlock(buf, midi);
        REQUIRE(allFinite(buf));
        REQUIRE(peak(buf) < 32.0f);
    }

    // All but Init must move something.
    INFO(changed << " of " << all.size() << " presets changed the state");
    REQUIRE(changed >= all.size() - 1);
}
