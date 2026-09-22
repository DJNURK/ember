#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ember
{
class EmberAudioProcessor;
}

namespace ember::gui
{
/**
    The EMBER wordmark, with a filament that is lit by the plugin itself.

    Drawn as vector paths rather than shipped as artwork, so it is crisp at
    every zoom level and costs nothing to ship. The letterforms are built from
    the condensed display face and cached as a `juce::Path` at each size, so
    paint() does no text layout.

    THE FILAMENT
    ------------
    The glyph to the left of the word is a tube filament: a hairpin of wire
    inside a bulb outline. Its brightness follows the plugin's global heat, so
    the logo is dark when nothing is happening and glows when the plugin is
    working. That is the whole art direction in one element - the interface is
    a piece of hardware that gets hot - and it is the only part of the UI that
    is decorative rather than functional, which is why it is the one place a
    little theatre is allowed.
*/
class Wordmark : public juce::Component, private juce::Timer
{
public:
    explicit Wordmark(EmberAudioProcessor&);
    ~Wordmark() override;

    /** Width this wants at a given height, so a header can lay it out without
        guessing at the letterform metrics. */
    int preferredWidth(int forHeight) const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Rebuilds the cached letterform path. Called on resize, not on paint. */
    void rebuildPath();

    EmberAudioProcessor& processor;

    juce::Path wordPath;
    juce::Rectangle<float> filamentArea;

    /** Smoothed for the eye: the same 30 ms / 400 ms the rest of the heat
        system uses, so the logo and the bands warm together. */
    float heat{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Wordmark)
};
} // namespace ember::gui
