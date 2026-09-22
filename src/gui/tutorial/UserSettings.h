#pragma once
#include <juce_core/juce_core.h>

namespace ember::gui::tutorial
{
/**
    What Ember remembers about *you*, as opposed to about a session.

    Two things live here: whether the welcome card has been dismissed, and how
    far through each tour you got. Both are properties of the person, not of the
    project — opening a different song should not offer you the welcome tour
    again, and finishing a tour in one session should be finished in the next.

    | Platform | Path |
    |---|---|
    | macOS   | `~/Library/Application Support/EmberAudio/Ember/settings.json` |
    | Windows | `%APPDATA%/EmberAudio/Ember/settings.json` |
    | Linux   | `~/.config/EmberAudio/Ember/settings.json` |

    WRITING IS BEST-EFFORT
    ----------------------
    A plugin cannot assume it may write to disk: sandboxes, read-only home
    directories and locked-down studio machines are all normal. Every write
    therefore fails silently and every read falls back to a default. The worst
    case is that the welcome card appears again, which is a small annoyance;
    refusing to load or throwing from a GUI callback would be worse.

    "Don't show again" is permanent within whatever persistence is available. A
    welcome card that returns after being dismissed is not a welcome, it is a
    nag.
*/
class UserSettings
{
public:
    /** The settings file, whether or not it exists yet. */
    [[nodiscard]] static juce::File file();

    [[nodiscard]] static bool welcomeDismissed();
    static void setWelcomeDismissed(bool);

    /** How far through `tourId` the user got, and whether they finished it. */
    [[nodiscard]] static int tourProgress(const juce::String& tourId);
    [[nodiscard]] static bool tourCompleted(const juce::String& tourId);
    static void setTourProgress(const juce::String& tourId, int stepReached, bool completed);

    /** Forgets everything. Exposed for tests, which must not depend on the
        state of the machine they run on. No control calls it: "forget my
        progress" is not worth a button a mis-click can reach, and deleting the
        file by hand does the same thing. */
    static void reset();

private:
    [[nodiscard]] static juce::var load();
    static void save(const juce::var&);
};
} // namespace ember::gui::tutorial
