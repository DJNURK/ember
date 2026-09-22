#include "gui/MotionClock.h"
#include "gui/EmberTheme.h"

namespace ember::gui
{
namespace
{
/** A frame longer than this is not a slow frame, it is a stall - a debugger
    pause, a dragged window, a display sleep. Handing an animation the real
    elapsed time would teleport it to its end state, which looks like a glitch
    rather than like motion. Three frames at 60 Hz. */
constexpr float kMaximumFrameSeconds = 0.05f;
} // namespace

MotionClock::MotionClock(juce::Component& host)
    : lastTicks(juce::Time::getHighResolutionTicks()), attachment(&host, [this] { tick(); })
{
}

MotionClock::~MotionClock() = default;

MotionClock::Registration MotionClock::add(Listener listener)
{
    const auto id = nextId++;
    listeners.push_back({id, std::move(listener)});
    return {*this, id};
}

void MotionClock::remove(int listenerId)
{
    // During a tick the vector is being iterated, so removals are deferred.
    // A listener destroying its own registration is the common case - a panel
    // going away - and must not invalidate the iteration underneath it.
    if (inTick)
    {
        pendingRemovals.push_back(listenerId);
        return;
    }

    listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                   [listenerId](const Entry& e) { return e.id == listenerId; }),
                    listeners.end());
}

void MotionClock::tick()
{
    const auto now = juce::Time::getHighResolutionTicks();
    const auto elapsed = static_cast<float>(juce::Time::highResolutionTicksToSeconds(now - lastTicks));
    lastTicks = now;

    if (elapsed > 0.0f)
    {
        // Smoothed, because a single frame's timing is noisy and this is only
        // ever read as a diagnostic.
        const auto instant = 1.0f / elapsed;
        frameRate += (instant - frameRate) * 0.05f;
    }

    const auto delta = juce::jlimit(0.0f, kMaximumFrameSeconds, elapsed);

    inTick = true;

    for (auto& entry : listeners)
        if (entry.callback != nullptr)
            entry.callback(delta);

    inTick = false;

    for (const auto id : pendingRemovals)
        remove(id);

    pendingRemovals.clear();
}

//==============================================================================
MotionClock::Registration::Registration(MotionClock& owner, int listenerId) : clock(&owner), id(listenerId) {}

MotionClock::Registration::Registration(Registration&& other) noexcept : clock(other.clock), id(other.id)
{
    other.clock = nullptr;
    other.id = -1;
}

MotionClock::Registration& MotionClock::Registration::operator=(Registration&& other) noexcept
{
    if (this != &other)
    {
        if (clock != nullptr && id >= 0)
            clock->remove(id);

        clock = other.clock;
        id = other.id;
        other.clock = nullptr;
        other.id = -1;
    }

    return *this;
}

MotionClock::Registration::~Registration()
{
    if (clock != nullptr && id >= 0)
        clock->remove(id);
}

//==============================================================================
bool Animated::advance(float secondsElapsed) noexcept
{
    if (! isMoving())
        return false;

    // Reduce motion is not "faster", it is "immediate": someone who turns it on
    // is asking not to see movement at all, and a 40 ms version of the same
    // slide is still movement.
    if (EmberTheme::reduceMotion() || duration <= 0.0f || secondsElapsed <= 0.0f)
    {
        current = target;
        return true;
    }

    const auto previous = current;
    const auto remaining = target - current;

    // Ease-out cubic, expressed as a per-frame fraction of what is left. Framing
    // it that way makes it frame-rate independent for free: at 120 Hz each step
    // is half the size and there are twice as many.
    const auto step = juce::jlimit(0.0f, 1.0f, secondsElapsed / duration);
    const auto eased = 1.0f - std::pow(1.0f - step, 3.0f);

    current += remaining * eased;

    if (std::abs(target - current) < 1.0e-4f)
        current = target;

    return std::abs(current - previous) > 1.0e-5f;
}
} // namespace ember::gui
