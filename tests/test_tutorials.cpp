// A tour that points at a name nothing claims fails silently: the spotlight
// lands nowhere, the card still says "turn this up", and the step looks broken
// rather than missing. That failure mode is invisible in code review and
// invisible in a build log, so it has to be a test.
//
// These construct a real editor and resolve every target of every shipped tour
// against it.
#include <catch2/catch_test_macros.hpp>
#include "gui/tutorial/TourLibrary.h"
#include "gui/tutorial/TourTargets.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

using namespace ember;
using namespace ember::gui::tutorial;

namespace
{
/** Every component id present in a tree, so a failure can say what WAS there
    rather than only what was not. */
void collectIDs(juce::Component& c, juce::StringArray& into)
{
    if (c.getComponentID().isNotEmpty())
        into.add(c.getComponentID());

    for (auto* child : c.getChildren())
        collectIDs(*child, into);
}

bool findID(juce::Component& c, const juce::String& id)
{
    if (c.getComponentID() == id)
        return true;

    for (auto* child : c.getChildren())
        if (findID(*child, id))
            return true;

    return false;
}
} // namespace

TEST_CASE("every shipped tour parses", "[tutorials]")
{
    const auto& tours = TourLibrary::tours();

    // Eight, as the specification lists. A tour that fails to parse is dropped
    // at load rather than shown broken, so a count short of eight means one of
    // them is malformed.
    REQUIRE(tours.size() == 8);

    for (const auto& tour : tours)
    {
        INFO("tour: " << tour.id);
        REQUIRE(tour.isValid());
        REQUIRE(tour.title.isNotEmpty());
        REQUIRE(tour.estimatedMinutes > 0);
        REQUIRE(tour.steps.size() >= 3);

        for (size_t i = 0; i < tour.steps.size(); ++i)
        {
            const auto& step = tour.steps[i];
            INFO("  step " << (i + 1));

            REQUIRE(step.title.isNotEmpty());
            REQUIRE(step.body.isNotEmpty());

            // An active step with no hint gives the user nothing to act on.
            if (step.action.isActive())
                REQUIRE((step.hint.isNotEmpty() || step.action.type == ActionType::clicked
                         || step.action.type == ActionType::presetLoaded
                         || step.action.type == ActionType::modulationConnected));
        }
    }
}

TEST_CASE("the eight tours are the eight the manual promises", "[tutorials]")
{
    const juce::StringArray expected{"quickstart", "multiband",     "styles",  "feedback-dynamics",
                                     "tone-eq",    "modulation",    "recipes", "hq-cpu"};

    juce::StringArray actual;

    for (const auto& tour : TourLibrary::tours())
        actual.add(tour.id);

    // Order matters: the first tour a new user sees should be the one they
    // need, not whatever sorted first.
    REQUIRE(actual.joinIntoString(",") == expected.joinIntoString(","));
}

TEST_CASE("every tour target resolves to a real component", "[tutorials]")
{
    EmberAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    REQUIRE(editor != nullptr);

    // A different size first: the editor sizes itself from the processor's
    // stored bounds, so setSize to the same value is a no-op and resized()
    // never runs - and the band modules are created lazily during layout, so
    // only the selected band would exist.
    editor->setSize(1200, 720);
    editor->setSize(1180, 700);

    juce::StringArray available;
    collectIDs(*editor, available);

    juce::StringArray missing;

    for (const auto& tour : TourLibrary::tours())
        for (size_t i = 0; i < tour.steps.size(); ++i)
            if (const auto& target = tour.steps[i].target; target.isNotEmpty())
                if (! findID(*editor, target))
                    missing.add(tour.id + " step " + juce::String(i + 1) + " -> '" + target + "'");

    INFO("unresolved targets:\n" << missing.joinIntoString("\n") << "\n\navailable ids:\n"
                                 << available.joinIntoString("\n"));
    REQUIRE(missing.isEmpty());
}

TEST_CASE("every parameter a tour names exists", "[tutorials]")
{
    EmberAudioProcessor processor;
    auto& state = processor.getAPVTS();

    juce::StringArray missing;

    for (const auto& tour : TourLibrary::tours())
        for (size_t i = 0; i < tour.steps.size(); ++i)
        {
            const auto& action = tour.steps[i].action;

            if (action.parameterID.isNotEmpty() && state.getParameter(action.parameterID) == nullptr)
                missing.add(tour.id + " step " + juce::String(i + 1) + " -> '" + action.parameterID + "'");
        }

    INFO("tours naming parameters that do not exist:\n" << missing.joinIntoString("\n"));
    REQUIRE(missing.isEmpty());
}

TEST_CASE("a parameterAtLeast target is reachable", "[tutorials]")
{
    // A step asking for 12 dB on a parameter whose range stops at 10 can never
    // complete, and the user has no way to know that.
    EmberAudioProcessor processor;
    auto& state = processor.getAPVTS();

    for (const auto& tour : TourLibrary::tours())
        for (size_t i = 0; i < tour.steps.size(); ++i)
        {
            const auto& action = tour.steps[i].action;

            if (action.type != ActionType::parameterAtLeast)
                continue;

            auto* p = state.getParameter(action.parameterID);
            REQUIRE(p != nullptr);

            const auto maximum = p->convertFrom0to1(1.0f);
            INFO(tour.id << " step " << (i + 1) << " wants " << action.value << " from " << action.parameterID
                         << ", which maxes at " << maximum);
            REQUIRE(action.value <= maximum);
        }
}

TEST_CASE("the parser rejects what it should", "[tutorials]")
{
    juce::String error;

    REQUIRE_FALSE(TourLibrary::parse("not json", error).isValid());
    REQUIRE(error.isNotEmpty());

    REQUIRE_FALSE(TourLibrary::parse(R"({"title":"no id","steps":[]})", error).isValid());
    REQUIRE(error.contains("id"));

    REQUIRE_FALSE(TourLibrary::parse(R"({"id":"x","steps":[]})", error).isValid());

    // The one that matters: a typo in an action type must be an error, not a
    // silently passive step that never completes.
    const auto typo = R"({"id":"x","title":"x","steps":[
        {"title":"t","body":"b","action":{"type":"parameterAtLest","parameter":"mix","value":1}}]})";

    REQUIRE_FALSE(TourLibrary::parse(typo, error).isValid());
    REQUIRE(error.contains("unknown action type"));

    // And an action that needs a parameter but was not given one.
    const auto noParam = R"({"id":"x","title":"x","steps":[
        {"title":"t","body":"b","action":{"type":"parameterChanged"}}]})";

    REQUIRE_FALSE(TourLibrary::parse(noParam, error).isValid());
    REQUIRE(error.contains("needs a parameter"));
}
