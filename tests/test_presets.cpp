#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include <BinaryData.h>
#include "dsp/EmberTypes.h"
#include "dsp/modulation/ModTypes.h"
#include "dsp/modulation/ModulationEngine.h"
#include "plugin/ParameterIDs.h"

using namespace ember;

namespace
{
/** Minimal host for an APVTS so the tests can inspect the real parameter
    layout — ranges, defaults, choice counts — without pulling in the plugin
    wrapper. */
class LayoutProbe : public juce::AudioProcessor
{
public:
    LayoutProbe() : apvts(*this, nullptr, "EMBER", pid::createParameterLayout()) {}

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "LayoutProbe"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    juce::AudioProcessorValueTreeState apvts;
};

struct FactoryPreset
{
    juce::String resourceName;
    juce::String fileName;
    juce::String xml;
};

juce::Array<FactoryPreset> loadFactoryPresets()
{
    juce::Array<FactoryPreset> out;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const auto* res = BinaryData::namedResourceList[i];
        const juce::String original(BinaryData::getNamedResourceOriginalFilename(res));
        if (! original.endsWithIgnoreCase(".xml"))
            continue;
        int size = 0;
        const char* data = BinaryData::getNamedResource(res, size);
        if (data == nullptr || size <= 0)
            continue;
        out.add({ juce::String(res), original,
                  juce::String(juce::CharPointer_UTF8(data), static_cast<size_t>(size)) });
    }
    return out;
}
} // namespace

TEST_CASE("the factory library ships the presets the specification asks for", "[presets]")
{
    auto presets = loadFactoryPresets();
    // The spec asks for at least 30 factory presets across six categories.
    INFO("found " << presets.size() << " factory presets");
    REQUIRE(presets.size() >= 30);

    juce::StringArray categories, names;
    for (const auto& p : presets)
    {
        auto xml = juce::parseXML(p.xml);
        INFO("unparseable preset: " << p.fileName);
        REQUIRE(xml != nullptr);
        REQUIRE(xml->getTagName() == "EMBER_PRESET");

        const auto name = xml->getStringAttribute("name");
        const auto category = xml->getStringAttribute("category");
        INFO("preset without a name or category: " << p.fileName);
        REQUIRE(name.isNotEmpty());
        REQUIRE(category.isNotEmpty());

        INFO("duplicate preset name: " << name);
        REQUIRE(! names.contains(name));
        names.add(name);
        categories.addIfNotAlreadyThere(category);
    }

    INFO("categories: " << categories.joinIntoString(", "));
    REQUIRE(categories.size() >= 6);
}

TEST_CASE("every factory preset references real parameters and legal values", "[presets]")
{
    // A preset naming a parameter that does not exist loads silently and does
    // nothing — the user hears the previous preset. A value outside the range
    // is clamped, which is worse: it loads looking correct and sounds wrong.
    LayoutProbe probe;
    auto presets = loadFactoryPresets();
    REQUIRE(! presets.isEmpty());

    for (const auto& p : presets)
    {
        auto xml = juce::parseXML(p.xml);
        REQUIRE(xml != nullptr);
        const auto tree = juce::ValueTree::fromXml(*xml);
        REQUIRE(tree.isValid());

        const auto params = tree.getChildWithName(probe.apvts.state.getType());
        INFO(p.fileName << " has no <" << probe.apvts.state.getType().toString() << "> parameter block");
        REQUIRE(params.isValid());
        REQUIRE(params.getNumChildren() > 0);

        for (int i = 0; i < params.getNumChildren(); ++i)
        {
            const auto entry = params.getChild(i);
            if (entry.getType().toString() != "PARAM")
                continue;

            const juce::String id = entry.getProperty("id").toString();
            INFO(p.fileName << " references unknown parameter '" << id << "'");
            auto* param = probe.apvts.getParameter(id);
            REQUIRE(param != nullptr);

            REQUIRE(entry.hasProperty("value"));
            const float value = static_cast<float>(static_cast<double>(entry.getProperty("value")));
            INFO(p.fileName << ": " << id << " = " << value << " is not finite");
            REQUIRE(std::isfinite(value));

            const auto& range = param->getNormalisableRange();
            INFO(p.fileName << ": " << id << " = " << value
                 << " outside [" << range.start << ", " << range.end << "]");
            REQUIRE(value >= range.start - 1.0e-4f);
            REQUIRE(value <= range.end + 1.0e-4f);
        }
    }
}

TEST_CASE("factory preset modulation graphs are well formed", "[presets][modulation]")
{
    LayoutProbe probe;

    // How many parameters modulation may target — the same table the processor
    // builds at construction.
    int numTargets = 0;
    for (auto* p : probe.getParameters())
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            if (pid::isModulatable(withID->paramID))
                ++numTargets;
    REQUIRE(numTargets > 0);

    int presetsWithModulation = 0;

    for (const auto& p : loadFactoryPresets())
    {
        auto xml = juce::parseXML(p.xml);
        REQUIRE(xml != nullptr);
        const auto tree = juce::ValueTree::fromXml(*xml);
        const auto mod = tree.getChildWithName(ModStateIds::modulation);
        if (! mod.isValid())
            continue;

        ++presetsWithModulation;
        INFO(p.fileName << " declares more connections than the engine has slots");
        REQUIRE(mod.getNumChildren() <= kMaxModConnections);

        for (int i = 0; i < mod.getNumChildren(); ++i)
        {
            const auto c = mod.getChild(i);
            if (c.getType() != ModStateIds::connection)
                continue;

            const int source = static_cast<int>(c.getProperty(ModStateIds::source, -1));
            const int target = static_cast<int>(c.getProperty(ModStateIds::target, -1));
            const float amount = static_cast<float>(static_cast<double>(c.getProperty(ModStateIds::amount, 0.0)));
            const int curve = static_cast<int>(c.getProperty(ModStateIds::curve, 0));

            INFO(p.fileName << " connection " << i << ": source " << source << " out of range");
            REQUIRE(source >= 0);
            REQUIRE(source < kNumModSources);

            INFO(p.fileName << " connection " << i << ": target " << target << " out of range");
            REQUIRE(target >= 0);
            REQUIRE(target < numTargets);

            INFO(p.fileName << " connection " << i << ": amount " << amount << " outside [-1, 1]");
            REQUIRE(amount >= -1.0f);
            REQUIRE(amount <= 1.0f);

            INFO(p.fileName << " connection " << i << ": curve " << curve << " is not a ModCurve");
            REQUIRE(curve >= 0);
            REQUIRE(curve < static_cast<int>(ModCurve::Count));
        }
    }

    INFO("presets shipping modulation: " << presetsWithModulation);
    REQUIRE(presetsWithModulation > 0);
}
