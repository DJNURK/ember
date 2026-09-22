#include "gui/tutorial/TourEngine.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include "plugin/ParameterIDs.h"
#include "plugin/PluginProcessor.h"

namespace ember::gui::tutorial
{
namespace
{
constexpr int kTickHz = 30;
constexpr int kCompletionTicks = 18; ///< ~600 ms at 30 Hz, as the design asks
constexpr int kCardWidth = 300;
constexpr float kDimAlpha = 0.6f;

/** Walks a component tree for an id. Depth first, first match wins. */
juce::Component* findByID(juce::Component& parent, const juce::String& name)
{
    if (parent.getComponentID() == name)
        return &parent;

    for (auto* child : parent.getChildren())
        if (auto* found = findByID(*child, name))
            return found;

    return nullptr;
}
} // namespace

//==============================================================================
/** The card beside the target. Not a modal: the editor underneath stays live,
    because a step that asks you to turn a knob has to let you turn it. */
class TourEngine::CoachCard : public juce::Component
{
public:
    explicit CoachCard(TourEngine& ownerToUse) : owner(ownerToUse)
    {
        for (auto* b : {&backButton, &nextButton, &exitButton})
            addAndMakeVisible(b);

        backButton.onClick = [this] { owner.back(); };
        nextButton.onClick = [this] { owner.next(); };
        exitButton.onClick = [this] { owner.stop(); };

        doItButton.setButtonText("Do it for me");
        doItButton.onClick = [this] { owner.doItForMe(); };
        addChildComponent(doItButton);

        setWantsKeyboardFocus(false);
        setAccessible(true);
    }

    void setStep(const TourStep& step, int index, int total, bool actionComplete)
    {
        title = step.title;
        body = step.body;
        hint = step.hint;
        counter = juce::String(index + 1) + " / " + juce::String(total);
        showLed = step.action.isActive();
        ledOn = actionComplete;

        doItButton.setVisible(step.action.isActive() && !actionComplete);
        backButton.setEnabled(index > 0);

        // Screen readers get the same words, in reading order, or the tour only
        // works for people who can see the spotlight.
        setTitle(step.title);
        setDescription(step.body + (step.hint.isNotEmpty() ? " " + step.hint : juce::String()));
        setHelpText(counter);

        resized();
        repaint();
    }

    /** Preferred height for the current text at the fixed 300 px width. */
    int preferredHeight() const
    {
        const auto bodyFont = EmberFonts::get(EmberFonts::Role::body);
        juce::AttributedString s;
        s.append(body, bodyFont, juce::Colours::white);

        juce::TextLayout layout;
        layout.createLayout(s, static_cast<float>(kCardWidth) - 2.0f * padding);

        auto height = padding + 22.0f + layout.getHeight() + 14.0f + buttonHeight + padding;

        if (showLed)
            height += 20.0f;

        if (doItButton.isVisible())
            height += buttonHeight + 6.0f;

        return juce::roundToInt(height);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(juce::roundToInt(padding));
        auto row = area.removeFromBottom(juce::roundToInt(buttonHeight));

        exitButton.setBounds(row.removeFromRight(60));
        row.removeFromRight(6);
        nextButton.setBounds(row.removeFromRight(70));
        row.removeFromRight(6);
        backButton.setBounds(row.removeFromRight(70));

        if (doItButton.isVisible())
        {
            area.removeFromBottom(6);
            doItButton.setBounds(area.removeFromBottom(juce::roundToInt(buttonHeight)).removeFromLeft(130));
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto& tk = EmberTheme::tokens();
        const auto area = getLocalBounds().toFloat();

        EmberLookAndFeel::drawPanel(g, area, Metrics::panelRadius, true);

        auto inner = area.reduced(padding);

        g.setFont(EmberFonts::get(EmberFonts::Role::title));
        g.setColour(tk.text);
        auto titleArea = inner.removeFromTop(22.0f);
        g.drawText(title, titleArea, juce::Justification::topLeft, false);

        g.setFont(EmberFonts::get(EmberFonts::Role::micro));
        g.setColour(tk.textDim);
        g.drawText(counter, titleArea, juce::Justification::topRight, false);

        g.setFont(EmberFonts::get(EmberFonts::Role::body));
        g.setColour(tk.textDim);

        auto bodyArea = inner.withTrimmedBottom(buttonHeight + padding);
        g.drawFittedText(body, bodyArea.toNearestInt(), juce::Justification::topLeft, 6, 1.0f);

        if (showLed)
        {
            // An unlit lamp that lights when the step's action is done, so an
            // active step says what it is waiting for rather than just waiting.
            const auto lamp = juce::Rectangle<float>(11.0f, 11.0f)
                                  .withPosition(inner.getX(), inner.getBottom() - buttonHeight - 20.0f);

            g.setColour(tk.bgDeep);
            g.fillEllipse(lamp);

            if (ledOn)
            {
                if (!EmberTheme::reduceMotion())
                    GlowCache::draw(g, lamp.getCentre(), 16.0f, tk.tubeGlow, 0.5f);

                g.setColour(tk.ember);
                g.fillEllipse(lamp.reduced(2.0f));
            }
            else
            {
                g.setColour(tk.textMuted.withAlpha(0.6f));
                g.fillEllipse(lamp.reduced(3.5f));
            }

            g.setColour(tk.panelEdge);
            g.drawEllipse(lamp.reduced(0.5f), 1.0f);

            if (hint.isNotEmpty())
            {
                g.setFont(EmberFonts::get(EmberFonts::Role::micro));
                g.setColour(ledOn ? tk.ember : tk.textDim);
                g.drawText(ledOn ? "Done" : hint,
                           juce::Rectangle<float>(lamp.getRight() + 6.0f, lamp.getY() - 1.0f,
                                                  inner.getWidth() - lamp.getWidth() - 6.0f, 13.0f),
                           juce::Justification::centredLeft, true);
            }
        }
    }

private:
    static constexpr float padding = 14.0f;
    static constexpr float buttonHeight = 24.0f;

    TourEngine& owner;
    juce::String title, body, hint, counter;
    bool showLed{false}, ledOn{false};

    juce::TextButton backButton{"Back"}, nextButton{"Next"}, exitButton{"Exit"};
    juce::TextButton doItButton;
};

//==============================================================================
TourEngine::TourEngine(juce::Component& editorRoot, EmberAudioProcessor& processorToUse)
    : root(editorRoot), processor(processorToUse)
{
    // Transparent to the mouse except where the card is: the editor underneath
    // has to stay usable, because most steps ask the user to do something to it.
    setInterceptsMouseClicks(false, true);
    setWantsKeyboardFocus(true);
    setVisible(false);

    card = std::make_unique<CoachCard>(*this);
    addChildComponent(card.get());
}

TourEngine::~TourEngine() = default;

void TourEngine::start(Tour tour, int fromStep)
{
    if (!tour.isValid())
        return;

    active = std::move(tour);
    running = true;
    setVisible(true);
    toFront(false);
    beginStep(juce::jlimit(0, static_cast<int>(active.steps.size()) - 1, fromStep));
    startTimerHz(kTickHz);
    grabKeyboardFocus();
}

void TourEngine::stop()
{
    if (!running)
        return;

    stopTimer();
    running = false;
    setVisible(false);
    target = nullptr;

    if (onProgress != nullptr)
        onProgress(active.id, stepIndex, false);
}

void TourEngine::beginStep(int index)
{
    stepIndex = juce::jlimit(0, static_cast<int>(active.steps.size()) - 1, index);

    const auto& step = active.steps[static_cast<size_t>(stepIndex)];

    // Resolved fresh each step: panels are rebuilt as bands are selected and
    // the window resized, so a pointer from step one is stale by step three.
    target = step.target.isNotEmpty() ? findTarget(step.target) : nullptr;

    actionDone = false;
    completionTicks = 0;
    captureActionBaseline();

    card->setStep(step, stepIndex, static_cast<int>(active.steps.size()), actionDone);
    resized();
    repaint();
}

juce::Component* TourEngine::findTarget(const juce::String& name) const
{
    return findByID(root, name);
}

juce::Rectangle<int> TourEngine::targetBoundsInThis() const
{
    if (target == nullptr)
        return {};

    return getLocalArea(target.getComponent(), target->getLocalBounds());
}

void TourEngine::captureActionBaseline()
{
    const auto& action = active.steps[static_cast<size_t>(stepIndex)].action;

    actionBaseline = 0.0f;

    if (action.parameterID.isNotEmpty())
        if (auto* v = processor.getAPVTS().getRawParameterValue(action.parameterID))
            actionBaseline = v->load(std::memory_order_relaxed);

    if (action.type == ActionType::bandAdded)
        if (auto* v = processor.getAPVTS().getRawParameterValue(pid::numBands))
            actionBaseline = v->load(std::memory_order_relaxed);
}

bool TourEngine::actionSatisfied() const
{
    const auto& action = active.steps[static_cast<size_t>(stepIndex)].action;
    auto& state = processor.getAPVTS();

    const auto read = [&state](const juce::String& id)
    {
        auto* v = state.getRawParameterValue(id);
        return v != nullptr ? v->load(std::memory_order_relaxed) : 0.0f;
    };

    switch (action.type)
    {
    case ActionType::none:
        return false;

    case ActionType::parameterAtLeast:
        return read(action.parameterID) >= action.value;

    case ActionType::parameterChanged:
        return std::abs(read(action.parameterID) - actionBaseline) > 1.0e-4f;

    case ActionType::bandAdded:
        return read(pid::numBands) > actionBaseline + 0.5f;

    case ActionType::styleSelected:
    {
        for (int b = 0; b < kMaxBands; ++b)
            if (std::abs(read(pid::style(b)) - actionBaseline) > 0.5f)
                return true;

        return false;
    }

    case ActionType::modulationConnected:
        return processor.getModulationEngine().getNumConnections() > juce::roundToInt(actionBaseline);

    case ActionType::presetLoaded:
    case ActionType::clicked:
        // Both are reported to the engine rather than polled; the flag is set
        // by the editor and read here.
        return actionDone;
    }

    return false;
}

void TourEngine::timerCallback()
{
    if (!running)
        return;

    // The target can move under us - a band selected, a window resized - so the
    // spotlight is recomputed rather than cached.
    repaint();

    const auto& step = active.steps[static_cast<size_t>(stepIndex)];

    if (!step.action.isActive())
        return;

    if (!actionDone && actionSatisfied())
    {
        actionDone = true;
        completionTicks = 0;
        card->setStep(step, stepIndex, static_cast<int>(active.steps.size()), true);
    }

    if (actionDone && ++completionTicks >= kCompletionTicks)
        next();
}

void TourEngine::next()
{
    if (stepIndex + 1 >= static_cast<int>(active.steps.size()))
    {
        if (onProgress != nullptr)
            onProgress(active.id, stepIndex + 1, true);

        stopTimer();
        running = false;
        setVisible(false);
        return;
    }

    beginStep(stepIndex + 1);
}

void TourEngine::back()
{
    if (stepIndex > 0)
        beginStep(stepIndex - 1);
}

void TourEngine::doItForMe()
{
    const auto& action = active.steps[static_cast<size_t>(stepIndex)].action;
    auto& state = processor.getAPVTS();

    const auto write = [&state](const juce::String& id, float denormalised)
    {
        if (auto* p = state.getParameter(id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(denormalised));
            p->endChangeGesture();
        }
    };

    switch (action.type)
    {
    case ActionType::parameterAtLeast:
        write(action.parameterID, action.value);
        break;

    case ActionType::parameterChanged:
        if (auto* p = state.getParameter(action.parameterID))
        {
            // Nudge towards the middle, so "changed" is satisfied wherever it
            // started - including at either end of its range.
            const auto current = p->getValue();
            p->beginChangeGesture();
            p->setValueNotifyingHost(current < 0.5f ? juce::jmin(1.0f, current + 0.25f)
                                                    : juce::jmax(0.0f, current - 0.25f));
            p->endChangeGesture();
        }
        break;

    case ActionType::bandAdded:
        if (auto* p = state.getParameter(pid::numBands))
        {
            const auto now = state.getRawParameterValue(pid::numBands)->load(std::memory_order_relaxed);
            write(pid::numBands, juce::jmin(static_cast<float>(kMaxBands), now + 1.0f));
            juce::ignoreUnused(p);
        }
        break;

    case ActionType::styleSelected:
        write(pid::style(processor.getSelectedBand()), actionBaseline + 1.0f);
        break;

    case ActionType::none:
    case ActionType::modulationConnected:
    case ActionType::presetLoaded:
    case ActionType::clicked:
        // These need a gesture the engine cannot fake without reaching into
        // panels it deliberately knows nothing about. The step marks itself
        // done so nobody is stuck.
        actionDone = true;
        break;
    }
}

bool TourEngine::keyPressed(const juce::KeyPress& key)
{
    if (!running)
        return false;

    if (key == juce::KeyPress::escapeKey)
    {
        stop();
        return true;
    }

    if (key == juce::KeyPress::rightKey || key == juce::KeyPress::returnKey)
    {
        next();
        return true;
    }

    if (key == juce::KeyPress::leftKey)
    {
        back();
        return true;
    }

    return false;
}

void TourEngine::mouseDown(const juce::MouseEvent&)
{
    // Clicks land here only outside the card, because the dimmed area does not
    // intercept them. Nothing to do: the editor beneath stays usable.
}

void TourEngine::resized()
{
    if (card == nullptr)
        return;

    const auto height = card->preferredHeight();
    const auto spot = targetBoundsInThis();
    auto bounds = getLocalBounds();

    // Beside the target if it fits, below it otherwise, and always fully inside
    // the window - a coach card half off the edge is worse than one in the way.
    juce::Rectangle<int> placed{kCardWidth, height};

    if (spot.isEmpty())
    {
        placed.setCentre(bounds.getCentre());
    }
    else if (spot.getRight() + 16 + kCardWidth <= bounds.getRight())
    {
        placed.setPosition(spot.getRight() + 16, spot.getCentreY() - height / 2);
    }
    else if (spot.getX() - 16 - kCardWidth >= bounds.getX())
    {
        placed.setPosition(spot.getX() - 16 - kCardWidth, spot.getCentreY() - height / 2);
    }
    else
    {
        placed.setPosition(spot.getCentreX() - kCardWidth / 2, spot.getBottom() + 16 + height <= bounds.getBottom()
                                                                   ? spot.getBottom() + 16
                                                                   : spot.getY() - 16 - height);
    }

    placed.setX(juce::jlimit(bounds.getX() + 8, juce::jmax(bounds.getX() + 8, bounds.getRight() - kCardWidth - 8),
                             placed.getX()));
    placed.setY(
        juce::jlimit(bounds.getY() + 8, juce::jmax(bounds.getY() + 8, bounds.getBottom() - height - 8), placed.getY()));

    card->setBounds(placed);
}

void TourEngine::paint(juce::Graphics& g)
{
    if (!running)
        return;

    const auto& tk = EmberTheme::tokens();
    const auto spot = targetBoundsInThis().expanded(6);
    const auto bounds = getLocalBounds();

    // The dim is drawn as four rectangles around the target rather than as a
    // full-screen fill with a hole punched in it: JUCE has no subtractive fill,
    // and four fills cost less than an off-screen mask.
    g.setColour(tk.bgDeep.withAlpha(kDimAlpha));

    if (spot.isEmpty())
    {
        g.fillRect(bounds);
    }
    else
    {
        g.fillRect(bounds.withBottom(spot.getY()));
        g.fillRect(bounds.withTop(spot.getBottom()));
        g.fillRect(juce::Rectangle<int>(bounds.getX(), spot.getY(), spot.getX() - bounds.getX(), spot.getHeight()));
        g.fillRect(
            juce::Rectangle<int>(spot.getRight(), spot.getY(), bounds.getRight() - spot.getRight(), spot.getHeight()));

        // The outline is the information; the pulse is decoration, so
        // reduce-motion drops the pulse and keeps the outline.
        const auto phase =
            EmberTheme::reduceMotion()
                ? 1.0f
                : 0.75f + 0.25f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.004f);

        g.setColour(tk.ember.withAlpha(phase));
        g.drawRoundedRectangle(spot.toFloat(), Metrics::controlRadius, 2.0f);
    }
}
} // namespace ember::gui::tutorial
