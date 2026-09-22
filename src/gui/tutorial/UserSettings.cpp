#include "gui/tutorial/UserSettings.h"

namespace ember::gui::tutorial
{
namespace
{
constexpr const char* kWelcomeKey = "welcomeDismissed";
constexpr const char* kToursKey = "tours";
constexpr const char* kStepKey = "step";
constexpr const char* kCompletedKey = "completed";
} // namespace

juce::File UserSettings::file()
{
    // getSpecialLocation gives the right directory on each platform: Application
    // Support on macOS, APPDATA on Windows, and .config on Linux.
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("EmberAudio")
        .getChildFile("Ember")
        .getChildFile("settings.json");
}

juce::var UserSettings::load()
{
    const auto f = file();

    if (!f.existsAsFile())
        return juce::var(new juce::DynamicObject());

    const auto parsed = juce::JSON::parse(f.loadFileAsString());

    // A corrupt file is treated as an empty one rather than as an error. The
    // cost of being wrong is one unnecessary welcome card; the cost of
    // propagating a parse failure out of a GUI constructor is an editor that
    // will not open.
    return parsed.isObject() ? parsed : juce::var(new juce::DynamicObject());
}

void UserSettings::save(const juce::var& value)
{
    const auto f = file();

    // Best effort throughout: a plugin cannot assume it may write to disk.
    if (!f.getParentDirectory().createDirectory())
        return;

    f.replaceWithText(juce::JSON::toString(value, false));
}

bool UserSettings::welcomeDismissed()
{
    const auto settings = load();

    if (auto* object = settings.getDynamicObject())
        return static_cast<bool>(object->getProperty(kWelcomeKey));

    return false;
}

void UserSettings::setWelcomeDismissed(bool dismissed)
{
    auto settings = load();

    if (auto* object = settings.getDynamicObject())
    {
        object->setProperty(kWelcomeKey, dismissed);
        save(settings);
    }
}

namespace
{
juce::DynamicObject* tourEntry(juce::var& settings, const juce::String& tourId, bool createIfMissing)
{
    auto* root = settings.getDynamicObject();

    if (root == nullptr)
        return nullptr;

    auto tours = root->getProperty(kToursKey);

    if (!tours.isObject())
    {
        if (!createIfMissing)
            return nullptr;

        tours = juce::var(new juce::DynamicObject());
        root->setProperty(kToursKey, tours);
    }

    auto* toursObject = tours.getDynamicObject();
    auto entry = toursObject->getProperty(tourId);

    if (!entry.isObject())
    {
        if (!createIfMissing)
            return nullptr;

        entry = juce::var(new juce::DynamicObject());
        toursObject->setProperty(tourId, entry);
    }

    return entry.getDynamicObject();
}
} // namespace

int UserSettings::tourProgress(const juce::String& tourId)
{
    auto settings = load();

    if (auto* entry = tourEntry(settings, tourId, false))
        return static_cast<int>(entry->getProperty(kStepKey));

    return 0;
}

bool UserSettings::tourCompleted(const juce::String& tourId)
{
    auto settings = load();

    if (auto* entry = tourEntry(settings, tourId, false))
        return static_cast<bool>(entry->getProperty(kCompletedKey));

    return false;
}

void UserSettings::setTourProgress(const juce::String& tourId, int stepReached, bool completed)
{
    auto settings = load();

    if (auto* entry = tourEntry(settings, tourId, true))
    {
        // Progress only moves forward. Exiting a tour at step two after having
        // finished it once should not un-finish it, and reopening it should
        // offer to resume from the furthest point reached rather than the most
        // recent one.
        const auto previous = static_cast<int>(entry->getProperty(kStepKey));
        entry->setProperty(kStepKey, juce::jmax(previous, stepReached));

        if (completed || static_cast<bool>(entry->getProperty(kCompletedKey)))
            entry->setProperty(kCompletedKey, true);

        save(settings);
    }
}

void UserSettings::reset()
{
    file().deleteFile();
}
} // namespace ember::gui::tutorial
