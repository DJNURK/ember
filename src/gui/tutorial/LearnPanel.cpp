#include "gui/tutorial/LearnPanel.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include "gui/tutorial/TourLibrary.h"
#include "gui/tutorial/UserSettings.h"
#include "plugin/PluginProcessor.h"

namespace ember::gui::tutorial
{
//==============================================================================
/** One tour, with how far through it you are drawn as a ring. */
class LearnPanel::TourCard : public juce::Component
{
public:
    TourCard(const Tour& tourToShow, TourEngine& engineToUse) : tour(tourToShow), engine(engineToUse)
    {
        refresh();

        startButton.onClick = [this]
        {
            // Resume rather than restart when there is progress to resume from:
            // being sent back to step one after finishing seven is the fastest
            // way to make someone close a help panel.
            const auto from = UserSettings::tourCompleted(tour.id) ? 0 : UserSettings::tourProgress(tour.id);
            engine.start(tour, from);
        };

        addAndMakeVisible(startButton);
    }

    void refresh()
    {
        progress = UserSettings::tourProgress(tour.id);
        completed = UserSettings::tourCompleted(tour.id);

        startButton.setButtonText(completed ? "Again" : (progress > 0 ? "Resume" : "Start"));
        repaint();
    }

    void resized() override
    {
        startButton.setBounds(getLocalBounds().removeFromRight(72).reduced(0, 12).withTrimmedRight(12));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& tk = EmberTheme::tokens();
        const auto area = getLocalBounds().toFloat().reduced(2.0f);

        EmberLookAndFeel::drawPanel(g, area, Metrics::panelRadius, false);

        auto inner = area.reduced(12.0f);
        auto ringArea = inner.removeFromLeft(inner.getHeight()).reduced(4.0f);
        inner.removeFromLeft(10.0f);
        inner.removeFromRight(72.0f);

        // The ring: a full circle when finished, an arc while in progress.
        const auto total = juce::jmax(1, static_cast<int>(tour.steps.size()));
        const auto fraction = completed ? 1.0f : juce::jlimit(0.0f, 1.0f, static_cast<float>(progress) / static_cast<float>(total));

        g.setColour(tk.panelEdge);
        g.drawEllipse(ringArea, 2.0f);

        if (fraction > 0.0f)
        {
            juce::Path arc;
            arc.addCentredArc(ringArea.getCentreX(), ringArea.getCentreY(), ringArea.getWidth() * 0.5f,
                              ringArea.getHeight() * 0.5f, 0.0f, 0.0f,
                              juce::MathConstants<float>::twoPi * fraction, true);
            g.setColour(completed ? tk.ember : tk.ember.withAlpha(0.7f));
            g.strokePath(arc, juce::PathStrokeType(2.5f));
        }

        g.setFont(EmberFonts::get(EmberFonts::Role::body));
        g.setColour(tk.text);
        g.drawText(tour.title, inner.removeFromTop(18.0f), juce::Justification::centredLeft, true);

        g.setFont(EmberFonts::get(EmberFonts::Role::micro));
        g.setColour(tk.textDim);
        g.drawText(juce::String(tour.estimatedMinutes) + " min  ·  " + juce::String(tour.steps.size()) + " steps",
                   inner.removeFromTop(14.0f), juce::Justification::centredLeft, true);
    }

private:
    const Tour& tour;
    TourEngine& engine;
    juce::TextButton startButton;
    int progress{0};
    bool completed{false};
};

//==============================================================================
/** The reference, filtered by the search box. */
class LearnPanel::ArticleList : public juce::Component
{
public:
    void setFilter(const juce::String& term)
    {
        shown = Reference::search(term);
        updateHeight();
        repaint();
    }

    void scrollTo(const juce::String& parameterID)
    {
        const auto* wanted = Reference::articleFor(parameterID);

        if (wanted == nullptr)
            return;

        for (size_t i = 0; i < shown.size(); ++i)
            if (shown[i] == wanted)
            {
                highlighted = static_cast<int>(i);
                repaint();
                return;
            }
    }

    void updateHeight()
    {
        // Measured rather than assumed: articles vary from two lines to six,
        // and a fixed row height either clips the long ones or leaves the short
        // ones swimming.
        int total = 8;

        for (const auto* article : shown)
            total += heightOf(*article);

        setSize(juce::jmax(getWidth(), LearnPanel::preferredWidth - 24), juce::jmax(total, 40));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& tk = EmberTheme::tokens();
        int y = 4;

        for (size_t i = 0; i < shown.size(); ++i)
        {
            const auto& article = *shown[i];
            const auto height = heightOf(article);
            const juce::Rectangle<int> row{0, y, getWidth(), height};

            if (static_cast<int>(i) == highlighted)
            {
                g.setColour(tk.ember.withAlpha(0.10f));
                g.fillRoundedRectangle(row.toFloat().reduced(2.0f), Metrics::controlRadius);
            }

            auto inner = row.reduced(8, 6);

            g.setFont(EmberFonts::get(EmberFonts::Role::value));
            g.setColour(tk.text);
            g.drawText(article.displayName, inner.removeFromTop(16), juce::Justification::centredLeft, true);

            if (article.range.isNotEmpty())
            {
                g.setFont(EmberFonts::get(EmberFonts::Role::micro));
                g.setColour(tk.textDim);
                g.drawText(article.range, inner.removeFromTop(13), juce::Justification::centredLeft, true);
            }

            g.setFont(EmberFonts::get(EmberFonts::Role::micro));
            g.setColour(tk.textDim);
            g.drawFittedText(article.body, inner.removeFromTop(bodyHeight(article)), juce::Justification::topLeft, 5,
                             1.0f);

            if (article.whenToUse.isNotEmpty())
            {
                g.setColour(tk.textMuted.brighter(0.4f));
                g.drawFittedText(article.whenToUse, inner, juce::Justification::topLeft, 3, 1.0f);
            }

            y += height;
        }

        if (shown.empty())
        {
            g.setFont(EmberFonts::get(EmberFonts::Role::body));
            g.setColour(tk.textDim);
            g.drawText("Nothing matches.", getLocalBounds(), juce::Justification::centred, false);
        }
    }

private:
    static int bodyHeight(const Article& a) { return juce::jlimit(26, 70, a.body.length() / 4); }

    static int heightOf(const Article& a)
    {
        return 16 + (a.range.isNotEmpty() ? 13 : 0) + bodyHeight(a)
               + (a.whenToUse.isNotEmpty() ? juce::jlimit(20, 48, a.whenToUse.length() / 4) : 0) + 16;
    }

    std::vector<const Article*> shown{Reference::search({})};
    int highlighted{-1};
};

//==============================================================================
namespace
{
/** A simple scrollable body of text, for the tabs that are only prose. */
class TextPane : public juce::Component
{
public:
    explicit TextPane(juce::StringPairArray rowsToShow, juce::String headingToShow = {})
        : rows(std::move(rowsToShow)), heading(std::move(headingToShow))
    {
        setSize(LearnPanel::preferredWidth - 24, 40 + rows.size() * 26 + (heading.isNotEmpty() ? 28 : 0));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& tk = EmberTheme::tokens();
        auto area = getLocalBounds().reduced(8, 6);

        if (heading.isNotEmpty())
        {
            g.setFont(EmberFonts::get(EmberFonts::Role::section));
            g.setColour(tk.textDim);
            g.drawText(heading.toUpperCase(), area.removeFromTop(24), juce::Justification::centredLeft, false);
        }

        for (const auto& key : rows.getAllKeys())
        {
            auto row = area.removeFromTop(26);

            g.setFont(EmberFonts::get(EmberFonts::Role::value));
            g.setColour(tk.text);
            g.drawText(key, row.removeFromLeft(row.getWidth() * 4 / 10), juce::Justification::centredLeft, true);

            g.setFont(EmberFonts::get(EmberFonts::Role::micro));
            g.setColour(tk.textDim);
            g.drawText(rows[key], row, juce::Justification::centredLeft, true);
        }
    }

private:
    juce::StringPairArray rows;
    juce::String heading;
};

juce::StringPairArray shortcutRows()
{
    juce::StringPairArray rows;
    rows.set("Double-click", "Reset a control to its default");
    rows.set("Shift-drag", "Fine control");
    rows.set("Scroll", "Step a control's value");
    rows.set("Right-click", "MIDI learn, remove modulation, What is this?");
    rows.set("Alt-drag a node", "Q, on the EQ's mid node");
    rows.set("Alt-drag a knob", "Modulation amount, when modulated");
    rows.set("Alt-drag a crossover", "Snap to musical frequencies");
    rows.set("Cmd-drag a crossover", "Move every crossover together");
    rows.set("Right-click display", "Add a band, distribute, reset");
    rows.set("Shift+F1", "Help for the control under the pointer");
    rows.set("Arrow keys", "Next and previous, during a tour");
    rows.set("Escape", "Exit a tour");
    return rows;
}

juce::StringPairArray aboutRows()
{
    juce::StringPairArray rows;
    rows.set("Version", EMBER_VERSION_STRING);
    rows.set("Formats", "VST3, Audio Unit, Standalone");
    rows.set("Type", "Inter and Barlow Condensed, SIL OFL");
    rows.set("Settings", UserSettings::file().getFullPathName());
    return rows;
}
} // namespace

//==============================================================================
LearnPanel::LearnPanel(TourEngine& engineToUse) : engine(engineToUse)
{
    for (auto* tab : {&toursTab, &referenceTab, &shortcutsTab, &aboutTab})
    {
        tab->setClickingTogglesState(true);
        tab->setRadioGroupId(1);
        addAndMakeVisible(tab);
    }

    toursTab.onClick = [this] { setTab(Tab::tours); };
    referenceTab.onClick = [this] { setTab(Tab::reference); };
    shortcutsTab.onClick = [this] { setTab(Tab::shortcuts); };
    aboutTab.onClick = [this] { setTab(Tab::about); };

    closeButton.onClick = [this] { hide(); };
    addAndMakeVisible(closeButton);

    searchBox.setTextToShowWhenEmpty("Search", EmberTheme::tokens().textMuted);
    searchBox.onTextChange = [this]
    {
        if (referenceView != nullptr)
            referenceView->setFilter(searchBox.getText());
    };
    addChildComponent(searchBox);

    referenceView = std::make_unique<ArticleList>();
    shortcutsView = std::make_unique<TextPane>(shortcutRows(), "Modifiers");
    aboutView = std::make_unique<TextPane>(aboutRows(), "Ember");
    toursView = std::make_unique<juce::Component>();

    addAndMakeVisible(viewport);
    viewport.setScrollBarsShown(true, false);

    rebuildTourCards();
    setTab(Tab::tours);
    setVisible(false);
    setWantsKeyboardFocus(true);
}

LearnPanel::~LearnPanel() = default;

void LearnPanel::rebuildTourCards()
{
    tourCards.clear();

    int y = 4;

    for (const auto& tour : TourLibrary::tours())
    {
        auto card = std::make_unique<TourCard>(tour, engine);
        card->setBounds(0, y, preferredWidth - 24, 64);
        y += 68;
        toursView->addAndMakeVisible(card.get());
        tourCards.push_back(std::move(card));
    }

    toursView->setSize(preferredWidth - 24, juce::jmax(y, 40));
}

void LearnPanel::setTab(Tab tab)
{
    currentTab = tab;

    toursTab.setToggleState(tab == Tab::tours, juce::dontSendNotification);
    referenceTab.setToggleState(tab == Tab::reference, juce::dontSendNotification);
    shortcutsTab.setToggleState(tab == Tab::shortcuts, juce::dontSendNotification);
    aboutTab.setToggleState(tab == Tab::about, juce::dontSendNotification);

    searchBox.setVisible(tab == Tab::reference);

    switch (tab)
    {
    case Tab::tours:
        viewport.setViewedComponent(toursView.get(), false);
        break;
    case Tab::reference:
        referenceView->updateHeight();
        viewport.setViewedComponent(referenceView.get(), false);
        break;
    case Tab::shortcuts:
        viewport.setViewedComponent(shortcutsView.get(), false);
        break;
    case Tab::about:
        viewport.setViewedComponent(aboutView.get(), false);
        break;
    }

    resized();
    repaint();
}

void LearnPanel::show(Tab tab)
{
    visible = true;
    setVisible(true);
    toFront(true);
    setTab(tab);

    for (auto& card : tourCards)
        card->refresh();

    startTimerHz(4); // refresh the rings while a tour runs behind the panel
}

void LearnPanel::hide()
{
    stopTimer();
    visible = false;
    setVisible(false);

    if (onDismiss != nullptr)
        onDismiss();
}

void LearnPanel::showArticleFor(const juce::String& parameterID)
{
    show(Tab::reference);
    searchBox.setText({}, juce::dontSendNotification);
    referenceView->setFilter({});
    referenceView->scrollTo(parameterID);
}

void LearnPanel::timerCallback()
{
    for (auto& card : tourCards)
        card->refresh();
}

bool LearnPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hide();
        return true;
    }

    return false;
}

void LearnPanel::resized()
{
    auto area = getLocalBounds().reduced(8);

    auto header = area.removeFromTop(26);
    closeButton.setBounds(header.removeFromRight(56));
    header.removeFromRight(6);

    const int tabWidth = header.getWidth() / 4;
    toursTab.setBounds(header.removeFromLeft(tabWidth));
    referenceTab.setBounds(header.removeFromLeft(tabWidth));
    shortcutsTab.setBounds(header.removeFromLeft(tabWidth));
    aboutTab.setBounds(header);

    area.removeFromTop(6);

    if (searchBox.isVisible())
    {
        searchBox.setBounds(area.removeFromTop(24));
        area.removeFromTop(6);
    }

    viewport.setBounds(area);

    if (referenceView != nullptr)
        referenceView->updateHeight();
}

void LearnPanel::paint(juce::Graphics& g)
{
    const auto& tk = EmberTheme::tokens();

    g.setColour(tk.panel);
    g.fillRect(getLocalBounds());

    TextureCache::fillBrushed(g, getLocalBounds(), 0.04f);

    // A single edge on the left, where it meets the rest of the interface.
    g.setColour(tk.panelEdge);
    g.fillRect(juce::Rectangle<int>(0, 0, 1, getHeight()));
}
} // namespace ember::gui::tutorial
