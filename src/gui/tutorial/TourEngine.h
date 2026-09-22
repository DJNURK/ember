#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace ember
{
class EmberAudioProcessor;
}

namespace ember::gui::tutorial
{
//==============================================================================
/** What a step waits for, when it waits for anything. */
enum class ActionType
{
    none = 0,         ///< passive: the step advances on Next
    parameterAtLeast, ///< a parameter reaches a value
    parameterChanged, ///< a parameter moves at all
    styleSelected,    ///< any band's style changes
    bandAdded,        ///< the band count rises
    modulationConnected,
    presetLoaded,
    clicked ///< the target component is clicked
};

struct StepAction
{
    ActionType type{ActionType::none};
    juce::String parameterID;
    float value{0.0f};

    [[nodiscard]] bool isActive() const noexcept { return type != ActionType::none; }
};

struct TourStep
{
    juce::String target; ///< a TourTargets name; empty means "no spotlight"
    juce::String title;
    juce::String body;
    juce::String hint; ///< shown under an active step's LED
    StepAction action;
    bool spotlight{true};
};

struct Tour
{
    juce::String id;
    juce::String title;
    int estimatedMinutes{2};
    std::vector<TourStep> steps;

    [[nodiscard]] bool isValid() const noexcept { return id.isNotEmpty() && !steps.empty(); }
};

//==============================================================================
/**
    Runs one tour: dims the editor, points at a control, and waits.

    WHAT IT DOES NOT DO
    -------------------
    It does not touch the audio. A tour observes - it watches a parameter until
    the user moves it - and the single exception is the "Do it for me" link,
    which exists so that nobody can be stuck on a step they cannot perform.
    Everything else is the user's own hand.

    It also does not reset anything. Tours run on whatever session is loaded,
    because a tour that clears your work to teach you something has taken
    something from you. A tour that genuinely cannot make its point from an
    arbitrary state says so once and offers a demo preset, snapshotting the
    state first and putting it back on exit.

    HOW IT FINDS THINGS
    -------------------
    By `setComponentID`, walking the editor's tree each time a step begins
    rather than caching. Panels are rebuilt as bands are selected and the window
    is resized, so a pointer captured when the tour started would be stale by
    step three.

    A target that resolves to nothing does not stall the tour: the step runs
    without a spotlight and the card says so, which is visible in a screenshot
    and reported by `test_tutorials`. Failing loudly beats a tour that looks
    fine and points at empty space.
*/
class TourEngine : public juce::Component, private juce::Timer
{
public:
    TourEngine(juce::Component& editorRoot, EmberAudioProcessor&);
    ~TourEngine() override;

    /** Starts `tour` at `fromStep`. Resuming is what the Learn panel's
        completion rings are for. */
    void start(Tour tour, int fromStep = 0);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running; }
    [[nodiscard]] juce::String currentTourId() const { return active.id; }
    [[nodiscard]] int currentStepIndex() const noexcept { return stepIndex; }

    void next();
    void back();

    /** Performs the current step's action on the user's behalf. */
    void doItForMe();

    /** Reports progress so it can be stored per user: tour id, step reached,
        and whether the tour ran to the end. */
    std::function<void(const juce::String& tourId, int stepReached, bool completed)> onProgress;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    class CoachCard;

    void timerCallback() override;
    void beginStep(int index);
    [[nodiscard]] juce::Component* findTarget(const juce::String& name) const;
    [[nodiscard]] juce::Rectangle<int> targetBoundsInThis() const;
    [[nodiscard]] bool actionSatisfied() const;
    void captureActionBaseline();

    juce::Component& root;
    EmberAudioProcessor& processor;

    Tour active;
    int stepIndex{0};
    bool running{false};

    juce::Component::SafePointer<juce::Component> target;

    /** The value the watched parameter had when the step began, so
        `parameterChanged` can tell "moved" from "already there". */
    float actionBaseline{0.0f};
    bool actionDone{false};
    int completionTicks{0};

    std::unique_ptr<CoachCard> card;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TourEngine)
};
} // namespace ember::gui::tutorial
