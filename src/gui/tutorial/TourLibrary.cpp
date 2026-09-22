#include "gui/tutorial/TourLibrary.h"
#include <BinaryData.h>

namespace ember::gui::tutorial
{
namespace
{
struct ActionName
{
    ActionType type;
    const char* name;
};

/** The vocabulary, in one place, so the parser and the tests cannot disagree
    about what a tour is allowed to say. */
constexpr ActionName kActionNames[] = {
    {ActionType::none, "none"},
    {ActionType::parameterAtLeast, "parameterAtLeast"},
    {ActionType::parameterChanged, "parameterChanged"},
    {ActionType::styleSelected, "styleSelected"},
    {ActionType::bandAdded, "bandAdded"},
    {ActionType::modulationConnected, "modulationConnected"},
    {ActionType::presetLoaded, "presetLoaded"},
    {ActionType::clicked, "clicked"},
};

/** The order tours are listed in. Explicit rather than alphabetical or
    whatever BinaryData happens to emit: a first tour should be the one a new
    user needs, and "Feedback & Dynamics" sorting above "Quick start" would be
    a poor welcome. */
constexpr const char* kTourOrder[] = {
    "quickstart", "multiband", "styles", "feedback-dynamics", "tone-eq", "modulation", "recipes", "hq-cpu",
};
} // namespace

juce::String TourLibrary::nameOf(ActionType type)
{
    for (const auto& entry : kActionNames)
        if (entry.type == type)
            return entry.name;

    return "none";
}

bool TourLibrary::actionFromName(const juce::String& name, ActionType& out)
{
    for (const auto& entry : kActionNames)
    {
        if (name == entry.name)
        {
            out = entry.type;
            return true;
        }
    }

    return false;
}

Tour TourLibrary::parse(const juce::String& json, juce::String& errorOut)
{
    Tour tour;
    errorOut.clear();

    const auto parsed = juce::JSON::parse(json);

    if (!parsed.isObject())
    {
        errorOut = "not a JSON object";
        return {};
    }

    auto* object = parsed.getDynamicObject();
    tour.id = object->getProperty("id").toString();
    tour.title = object->getProperty("title").toString();
    tour.estimatedMinutes = static_cast<int>(object->getProperty("estimatedMinutes"));

    if (tour.id.isEmpty())
    {
        errorOut = "missing id";
        return {};
    }

    const auto steps = object->getProperty("steps");

    if (!steps.isArray())
    {
        errorOut = "'steps' is not an array";
        return {};
    }

    for (const auto& value : *steps.getArray())
    {
        auto* stepObject = value.getDynamicObject();

        if (stepObject == nullptr)
        {
            errorOut = "step is not an object";
            return {};
        }

        TourStep step;
        step.target = stepObject->getProperty("target").toString();
        step.title = stepObject->getProperty("title").toString();
        step.body = stepObject->getProperty("body").toString();
        step.hint = stepObject->getProperty("hint").toString();

        if (step.title.isEmpty() && step.body.isEmpty())
        {
            errorOut = "step " + juce::String(tour.steps.size() + 1) + " has neither title nor body";
            return {};
        }

        if (const auto action = stepObject->getProperty("action"); action.isObject())
        {
            auto* actionObject = action.getDynamicObject();
            const auto typeName = actionObject->getProperty("type").toString();

            // An unknown type is an error, not a silently passive step: a typo
            // in "parameterAtLeast" would otherwise produce a tour that looks
            // correct and never completes.
            if (!actionFromName(typeName, step.action.type))
            {
                errorOut = "step " + juce::String(tour.steps.size() + 1) + ": unknown action type '" + typeName + "'";
                return {};
            }

            step.action.parameterID = actionObject->getProperty("parameter").toString();
            step.action.value = static_cast<float>(static_cast<double>(actionObject->getProperty("value")));

            const bool needsParameter =
                step.action.type == ActionType::parameterAtLeast || step.action.type == ActionType::parameterChanged;

            if (needsParameter && step.action.parameterID.isEmpty())
            {
                errorOut = "step " + juce::String(tour.steps.size() + 1) + ": " + typeName + " needs a parameter";
                return {};
            }
        }

        tour.steps.push_back(std::move(step));
    }

    if (tour.steps.empty())
    {
        errorOut = "tour has no steps";
        return {};
    }

    return tour;
}

const std::vector<Tour>& TourLibrary::tours()
{
    static const std::vector<Tour> loaded = []
    {
        std::vector<Tour> result;

        for (const auto* wanted : kTourOrder)
        {
            const juce::String resourceName = juce::String(wanted).replace("-", "") + "_json";

            for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            {
                if (juce::String(BinaryData::namedResourceList[i]) != resourceName)
                    continue;

                int size = 0;

                if (const auto* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size))
                {
                    juce::String error;
                    auto tour = parse(juce::String::fromUTF8(data, size), error);

                    // A malformed tour is dropped rather than shown broken. The
                    // test catches it before a release; at runtime a missing
                    // tour is better than one that strands someone mid-step.
                    if (tour.isValid())
                        result.push_back(std::move(tour));
                    else
                        DBG("tour '" << wanted << "' rejected: " << error);
                }

                break;
            }
        }

        return result;
    }();

    return loaded;
}

const Tour* TourLibrary::find(const juce::String& id)
{
    for (const auto& tour : tours())
        if (tour.id == id)
            return &tour;

    return nullptr;
}
} // namespace ember::gui::tutorial
