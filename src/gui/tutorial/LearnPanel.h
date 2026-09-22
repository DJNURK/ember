#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/tutorial/Reference.h"
#include "gui/tutorial/TourEngine.h"

namespace ember
{
class EmberAudioProcessor;
}

namespace ember::gui::tutorial
{
/**
    The manual, inside the plugin.

    Slides in from the right over the band strip. Four tabs:

      Tours      — the eight guided tours, with how far through each you are
      Reference  — an article per control, searchable
      Shortcuts  — every modifier, in one table
      About      — version, licence, and where the settings live

    WHY NOT A WEB VIEW
    ------------------
    A browser embedded in a plugin is a second rendering engine, a second theme
    to maintain and a second set of platform bugs, in exchange for text layout
    that JUCE already does. It also cannot be styled to match the rest of the
    interface, so help would look like a foreign object bolted to a piece of
    hardware. The articles are short by design; they do not need a browser.

    IT DOES NOT COVER THE CONTROLS IT DESCRIBES
    -------------------------------------------
    The panel takes width from the band strip rather than floating over it, and
    the display stays fully visible. Reading about Drive while unable to see
    Drive would defeat the point.
*/
class LearnPanel : public juce::Component, private juce::Timer
{
public:
    explicit LearnPanel(TourEngine&);
    ~LearnPanel() override;

    enum class Tab
    {
        tours = 0,
        reference,
        shortcuts,
        about
    };

    void show(Tab tab = Tab::tours);
    void hide();
    [[nodiscard]] bool isShowing() const noexcept { return visible; }

    /** Opens the Reference tab scrolled to the article covering `parameterID`.
        This is what "What is this?" on a control calls. */
    void showArticleFor(const juce::String& parameterID);

    /** The panel wants to close. */
    std::function<void()> onDismiss;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int preferredWidth = 360;

private:
    class TourCard;
    class ArticleList;

    void timerCallback() override;
    void setTab(Tab);
    void rebuildTourCards();

    TourEngine& engine;

    Tab currentTab{Tab::tours};
    bool visible{false};

    juce::TextButton toursTab{"Tours"}, referenceTab{"Reference"}, shortcutsTab{"Shortcuts"}, aboutTab{"About"};
    juce::TextButton closeButton{"Close"};

    juce::TextEditor searchBox;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> toursView;
    std::unique_ptr<ArticleList> referenceView;
    std::unique_ptr<juce::Component> shortcutsView;
    std::unique_ptr<juce::Component> aboutView;

    std::vector<std::unique_ptr<TourCard>> tourCards;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LearnPanel)
};
} // namespace ember::gui::tutorial
