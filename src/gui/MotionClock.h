#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace ember::gui
{
/**
    One clock for every moving thing in the editor.

    WHY ONE
    -------
    Before this, six components each ran their own `juce::Timer` at whatever
    rate suited them: the spectrum at 30 Hz, the footer at 20, the wordmark at
    30, the band strip, the preset bar and the modulation panel at others again.
    That has three costs. Timers at unrelated rates beat against each other, so
    two things that should move together visibly do not. Each one repaints on
    its own schedule, so a frame can be composited three times for three
    independent reasons. And nothing can be paused as a group.

    A single `juce::VBlankAttachment` ticks at the display's own refresh rate,
    which is also the only rate at which a repaint can actually become visible.

    WHAT IT GUARANTEES
    ------------------
    - Listeners are called in registration order, once per frame, on the message
      thread.
    - `secondsSinceLastFrame` is real elapsed time, clamped to a sane maximum:
      a debugger pause or a dragged window would otherwise hand an animation a
      two-second step and teleport it to its end state.
    - Removing a listener during its own callback is safe; the list is only
      compacted between frames.

    Animations ask for time and decide for themselves what to do with it. The
    clock has no opinion about easing.
*/
class MotionClock
{
public:
    /** @param host  a component that is on screen; the attachment follows its
                     peer, so the clock stops when the editor is closed. */
    explicit MotionClock(juce::Component& host);
    ~MotionClock();

    using Listener = std::function<void(float secondsSinceLastFrame)>;

    /** Registers `listener` and returns a token. The listener runs until the
        token is destroyed, which is what makes removal safe from a component
        destructor without every caller remembering to deregister. */
    class Registration
    {
    public:
        Registration() = default;
        Registration(MotionClock& owner, int listenerId);
        Registration(Registration&&) noexcept;
        Registration& operator=(Registration&&) noexcept;
        ~Registration();

        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

    private:
        MotionClock* clock{nullptr};
        int id{-1};
    };

    [[nodiscard]] Registration add(Listener);

    /** Frames per second the clock is actually running at, measured. Shown in
        no UI; it exists so a performance question can be answered with a
        number. */
    [[nodiscard]] float measuredFrameRate() const noexcept { return frameRate; }

private:
    friend class Registration;

    void tick();
    void remove(int listenerId);

    struct Entry
    {
        int id{};
        Listener callback;
    };

    std::vector<Entry> listeners;
    std::vector<int> pendingRemovals;
    bool inTick{false};
    int nextId{1};

    juce::int64 lastTicks{0};
    float frameRate{60.0f};

    juce::VBlankAttachment attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MotionClock)
};

//==============================================================================
/**
    A value that moves towards a target over a fixed duration.

    The design allows 120-180 ms for a state change and forbids anything over
    250. Rather than trust every call site to remember that, this takes the
    duration once and clamps it.

    With "reduce motion" on, it snaps: the end states are the truth and the
    animation is decoration.
*/
class Animated
{
public:
    Animated() = default;
    explicit Animated(float initial) : current(initial), target(initial) {}

    /** @param durationSeconds  clamped to the design's 250 ms ceiling. */
    void setDuration(float durationSeconds) noexcept
    {
        duration = juce::jlimit(0.0f, 0.25f, durationSeconds);
    }

    void setTarget(float newTarget) noexcept { target = newTarget; }

    /** Advances and returns true when the value moved enough to be worth a
        repaint. */
    bool advance(float secondsElapsed) noexcept;

    [[nodiscard]] float getValue() const noexcept { return current; }
    [[nodiscard]] bool isMoving() const noexcept { return std::abs(target - current) > 1.0e-4f; }

    void snapToTarget() noexcept { current = target; }

private:
    float current{0.0f};
    float target{0.0f};
    float duration{0.15f};
};
} // namespace ember::gui
