#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ember::gui::tutorial
{
/**
    An invisible, click-through marker that gives a painted region a name.

    Several of Ember's controls are painted rather than built from components -
    the display's mode selector, the analyser gear, the crossover handles, the
    EQ panel's chips. That is deliberate: at 17 px, three child components each
    with a border and a label cost more room than they are worth, and a handle
    drawn into the plot can sit over the curve in a way a component cannot.

    But a tour needs something with a `ComponentID` to point at. An anchor is
    that something: zero-cost to draw, transparent to the mouse, and positioned
    by whoever paints the region it stands for.

    The alternative was teaching the tour engine to ask a component for a named
    sub-rectangle, which spreads tour knowledge into every panel that has one.
    This keeps it in one place, at the price of one empty component per painted
    target - and an empty component is cheaper than a protocol.
*/
class TourAnchor final : public juce::Component
{
public:
    explicit TourAnchor(const juce::String& tourTargetName)
    {
        setComponentID(tourTargetName);
        setInterceptsMouseClicks(false, false);

        // Present in the tree and locatable, but never painted and never
        // focusable: it exists only to be found by name.
        setPaintingIsUnclipped(true);
        setAccessible(false);
    }

    void paint(juce::Graphics&) override {}

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TourAnchor)
};
} // namespace ember::gui::tutorial
