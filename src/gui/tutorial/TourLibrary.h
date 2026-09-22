#pragma once
#include "gui/tutorial/TourEngine.h"

namespace ember::gui::tutorial
{
/**
    The shipped tours, parsed from JSON compiled into the binary.

    WHY JSON RATHER THAN CODE
    -------------------------
    A tour is prose with a few hooks in it. Writing one in C++ means a rebuild
    to fix a typo and a diff full of string escapes, and it puts the person best
    placed to write the words - whoever knows the plugin - behind a compiler.
    As data, a tour can be reviewed as the thing it is: eight steps of text.

    The cost is that a mistake becomes a runtime problem rather than a compile
    error, which is why `parse` reports what it rejected and why
    `test_tutorials` loads every shipped tour and checks every target resolves.

    THE SCHEMA
    ----------
    ```json
    {
      "id": "quickstart",
      "title": "Quick start",
      "estimatedMinutes": 2,
      "steps": [
        {
          "target": "band.1.drive",
          "title": "Drive is the heart of Ember",
          "body": "Turn Drive up to push the band into saturation.",
          "hint": "Try about 12 dB.",
          "action": { "type": "parameterAtLeast",
                      "parameter": "band1Drive", "value": 12 }
        }
      ]
    }
    ```

    `target`, `hint` and `action` are optional. A step with no action is passive
    and advances on Next; a step with no target shows its card centred and dims
    the whole editor, which is how a tour opens and closes.
*/
class TourLibrary
{
public:
    /** Every tour compiled into the binary, in the order they should be listed.
        Parsed once on first call. */
    [[nodiscard]] static const std::vector<Tour>& tours();

    /** The tour with this id, or nullptr. */
    [[nodiscard]] static const Tour* find(const juce::String& id);

    /** Parses one tour from JSON text.

        Returns an invalid Tour and fills `errorOut` when the text is not a
        tour: a missing id, no steps, or a step with neither title nor body.
        Unknown action types are an error rather than a silently passive step,
        because a typo in "parameterAtLeast" would otherwise produce a tour that
        looks right and never completes. */
    [[nodiscard]] static Tour parse(const juce::String& json, juce::String& errorOut);

    /** Action name as it appears in JSON, for the parser and for tests. */
    [[nodiscard]] static juce::String nameOf(ActionType);
    [[nodiscard]] static bool actionFromName(const juce::String&, ActionType& out);
};
} // namespace ember::gui::tutorial
