#include "gui/tutorial/Reference.h"
#include <BinaryData.h>

namespace ember::gui::tutorial
{
juce::String Reference::kindOf(const juce::String& parameterID)
{
    // band3Drive -> bandNDrive, lfo2Rate -> lfoNRate, xover4 -> xoverN.
    static const char* prefixes[] = {"band", "lfo", "eg", "ef", "midi", "macro", "xover"};

    for (const auto* prefix : prefixes)
    {
        const juce::String p{prefix};

        if (! parameterID.startsWith(p))
            continue;

        auto rest = parameterID.substring(p.length());

        if (rest.isEmpty() || ! juce::CharacterFunctions::isDigit(rest[0]))
            continue;

        int digits = 0;
        while (digits < rest.length() && juce::CharacterFunctions::isDigit(rest[digits]))
            ++digits;

        return p + "N" + rest.substring(digits);
    }

    return parameterID;
}

std::vector<Article> Reference::parse(const juce::String& markdown)
{
    std::vector<Article> result;
    Article current;

    const auto flush = [&result, &current]
    {
        if (current.isValid())
            result.push_back(current);

        current = {};
    };

    for (const auto& raw : juce::StringArray::fromLines(markdown))
    {
        const auto line = raw.trim();

        if (line.startsWith("## "))
        {
            flush();
            current.kind = line.substring(3).trim();
            continue;
        }

        if (current.kind.isEmpty())
            continue; // preamble before the first article

        // "**Drive** — 0 to 40 dB"
        if (line.startsWith("**") && current.displayName.isEmpty())
        {
            const auto closing = line.indexOf(2, "**");

            if (closing > 0)
            {
                current.displayName = line.substring(2, closing).trim();
                auto rest = line.substring(closing + 2).trim();

                // Either an em dash or a hyphen, because both get typed.
                if (rest.startsWith("—") || rest.startsWith("-"))
                    rest = rest.substring(1).trim();

                current.range = rest;
            }

            continue;
        }

        if (line.startsWith("*When to use it:*"))
        {
            current.whenToUse = line.fromFirstOccurrenceOf("*When to use it:*", false, false).trim();
            continue;
        }

        if (line.startsWith("*Show me:*"))
        {
            current.showMe = line.fromFirstOccurrenceOf("*Show me:*", false, false).trim();
            continue;
        }

        if (line.isNotEmpty())
            current.body += (current.body.isEmpty() ? "" : " ") + line;
    }

    flush();
    return result;
}

const std::vector<Article>& Reference::articles()
{
    static const std::vector<Article> loaded = []
    {
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            if (juce::String(BinaryData::namedResourceList[i]) != "reference_md")
                continue;

            int size = 0;

            if (const auto* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size))
                return parse(juce::String::fromUTF8(data, size));
        }

        return std::vector<Article>{};
    }();

    return loaded;
}

const Article* Reference::articleFor(const juce::String& parameterID)
{
    const auto kind = kindOf(parameterID);

    for (const auto& article : articles())
        if (article.kind == kind)
            return &article;

    return nullptr;
}

std::vector<const Article*> Reference::search(const juce::String& term)
{
    std::vector<const Article*> result;
    const auto needle = term.trim().toLowerCase();

    for (const auto& article : articles())
    {
        if (needle.isEmpty() || article.displayName.toLowerCase().contains(needle)
            || article.kind.toLowerCase().contains(needle) || article.body.toLowerCase().contains(needle)
            || article.whenToUse.toLowerCase().contains(needle))
            result.push_back(&article);
    }

    return result;
}
} // namespace ember::gui::tutorial
