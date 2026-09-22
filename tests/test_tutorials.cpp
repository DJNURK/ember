// A tour that points at a name nothing claims fails silently: the spotlight
// lands nowhere, the card still says "turn this up", and the step looks broken
// rather than missing. That failure mode is invisible in code review and
// invisible in a build log, so it has to be a test.
//
// These construct a real editor and resolve every target of every shipped tour
// against it.
#include <catch2/catch_test_macros.hpp>
#include "gui/tutorial/Reference.h"
#include "gui/tutorial/TourLibrary.h"
#include "gui/tutorial/TourTargets.h"
#include "plugin/PluginEditor.h"
#include "plugin/ParameterIDs.h"
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

/** The component claiming `id`, or nullptr. `findID` answers whether a tour's
    target exists; this answers what it is, which is what a question about size
    or visibility needs. */
juce::Component* componentWithID(juce::Component& c, const juce::String& id)
{
    if (c.getComponentID() == id)
        return &c;

    for (auto* child : c.getChildren())
        if (auto* found = componentWithID(*child, id))
            return found;

    return nullptr;
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
                REQUIRE((step.hint.isNotEmpty() || step.action.type == ActionType::clicked ||
                         step.action.type == ActionType::presetLoaded ||
                         step.action.type == ActionType::modulationConnected));
        }
    }
}

TEST_CASE("the eight tours are the eight the manual promises", "[tutorials]")
{
    const juce::StringArray expected{"quickstart", "multiband",  "styles",  "feedback-dynamics",
                                     "tone-eq",    "modulation", "recipes", "hq-cpu"};

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
                if (!findID(*editor, target))
                    missing.add(tour.id + " step " + juce::String(i + 1) + " -> '" + target + "'");

    INFO("unresolved targets:\n"
         << missing.joinIntoString("\n") << "\n\navailable ids:\n"
         << available.joinIntoString("\n"));
    REQUIRE(missing.isEmpty());
}

TEST_CASE("the band strip lays out without the motion clock", "[tutorials][gui]")
{
    // Existing well before this test: every band module's width came from an
    // `Animated` weight that starts at zero and only reaches its target once a
    // `VBlankAttachment` frame has fired. The first layout therefore found no
    // width to share out and returned early, leaving the strip empty.
    //
    // In a host that is one invisible frame. Anywhere the clock never ticks -
    // an offscreen render, a peer that never arrives - it is permanent, and the
    // plugin's entire control surface is a blank rectangle. Two releases of
    // documentation screenshots shipped with that hole in them.
    //
    // No VBlank fires in this process either, which is exactly why the test can
    // see it: the steady state must not depend on an animation having run.
    EmberAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    REQUIRE(editor != nullptr);

    editor->setSize(1200, 720);
    editor->setSize(1180, 700);

    auto* bandCount = processor.getAPVTS().getRawParameterValue(ember::pid::numBands);
    REQUIRE(bandCount != nullptr);

    const int active = static_cast<int>(*bandCount);
    REQUIRE(active >= 1);

    for (int band = 0; band < active; ++band)
    {
        const auto id = TourTargets::bandDrive(band);
        auto* drive = componentWithID(*editor, id);

        INFO("band " << (band + 1) << " drive control '" << id << "'");
        REQUIRE(drive != nullptr);

        // Bounds, not just existence. The bug left every one of these present,
        // parented and correct - and zero pixels wide.
        REQUIRE(drive->getWidth() > 0);
        REQUIRE(drive->getHeight() > 0);
        REQUIRE(drive->isVisible());
    }
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

TEST_CASE("every parameter has a reference article", "[tutorials]")
{
    // The specification's own requirement, and the reason articles are keyed by
    // kind: 209 parameters collapse to 53 kinds, so adding a parameter to an
    // existing family costs nothing, while adding a new family fails here
    // rather than shipping undocumented.
    EmberAudioProcessor processor;

    juce::StringArray uncovered;
    int total = 0;

    for (auto* parameter : processor.getParameters())
    {
        auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter);

        if (withID == nullptr)
            continue;

        ++total;

        if (Reference::articleFor(withID->paramID) == nullptr)
            uncovered.add(withID->paramID + "  (kind: " + Reference::kindOf(withID->paramID) + ")");
    }

    INFO("parameters with no article:\n" << uncovered.joinIntoString("\n"));
    REQUIRE(total > 200);
    REQUIRE(uncovered.isEmpty());
}

TEST_CASE("articles are short enough to read in the panel", "[tutorials]")
{
    // The Learn panel is 360 px wide. An article that runs to a screenful stops
    // being a reference and becomes a chapter nobody reads.
    for (const auto& article : Reference::articles())
    {
        INFO("article: " << article.kind);
        REQUIRE(article.displayName.isNotEmpty());
        REQUIRE(article.body.isNotEmpty());
        REQUIRE(article.body.length() < 700);
        REQUIRE(article.whenToUse.length() < 500);
    }
}

TEST_CASE("a Show me link names a tour that exists", "[tutorials]")
{
    for (const auto& article : Reference::articles())
    {
        if (article.showMe.isEmpty())
            continue;

        const auto tourId = article.showMe.upToFirstOccurrenceOf("#", false, false).trim();

        INFO("article " << article.kind << " points at tour '" << tourId << "'");
        REQUIRE(TourLibrary::find(tourId) != nullptr);
    }
}
