#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace ember::gui::tutorial
{
/**
    The reference articles, parsed from one Markdown file.

    209 parameters, 53 distinct kinds: `band3Drive` and `band5Drive` want the
    same article with a different number in it. Articles are therefore keyed by
    kind, and `articleFor` collapses an id to its kind before looking it up.

    ONE SOURCE FOR TWO READERS
    --------------------------
    `resources/help/reference.md` is read by the plugin and by
    `scripts/gen-docs.py`, which regenerates the manual's parameter tables from
    it. CI fails if the two drift. Whichever a user opens, they get the same
    words - and nobody has to remember to update the other one.
*/
struct Article
{
    juce::String kind;        ///< the parameter kind, indices collapsed to N
    juce::String displayName; ///< "Drive"
    juce::String range;       ///< "0 to 40 dB"
    juce::String body;        ///< what it does
    juce::String whenToUse;
    juce::String showMe; ///< "quickstart#3", or empty

    [[nodiscard]] bool isValid() const noexcept { return kind.isNotEmpty() && body.isNotEmpty(); }
};

class Reference
{
public:
    /** Every article, in the order the source file lists them. */
    [[nodiscard]] static const std::vector<Article>& articles();

    /** The article covering `parameterID`, or nullptr.

        Collapses the index first: `band3Drive` is looked up as `bandNDrive`. */
    [[nodiscard]] static const Article* articleFor(const juce::String& parameterID);

    /** `band3Drive` -> `bandNDrive`. Exposed because the coverage test needs to
        report which kind a missing parameter maps to. */
    [[nodiscard]] static juce::String kindOf(const juce::String& parameterID);

    /** Articles whose name, kind or body contain `term`, case-insensitively.
        An empty term returns everything. */
    [[nodiscard]] static std::vector<const Article*> search(const juce::String& term);

    /** Parses the reference format. Exposed for tests. */
    [[nodiscard]] static std::vector<Article> parse(const juce::String& markdown);
};
} // namespace ember::gui::tutorial
