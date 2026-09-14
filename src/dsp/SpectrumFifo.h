#pragma once
#include <array>
#include <atomic>
#include "dsp/EmberTypes.h"

namespace ember
{
/**
    Single-producer / single-consumer transport for spectrum frames.

    The audio thread writes magnitude frames; the GUI timer reads the most
    recent one. Both sides are wait-free: the writer never blocks and the reader
    never blocks, so a slow or stalled GUI can only cost dropped frames, never
    an audio glitch. Frames the GUI misses are simply overwritten — for a
    60 fps display that is the correct trade.
*/
struct SpectrumFrame
{
    std::array<float, kSpectrumBins> inputDb {};
    std::array<float, kSpectrumBins> outputDb {};
};

class SpectrumFifo
{
public:
    /** Audio thread. Never blocks, never allocates. */
    void push(const SpectrumFrame& frame) noexcept
    {
        const int slot = (writeIndex.load(std::memory_order_relaxed) + 1) % kNumSlots;
        slots[static_cast<size_t>(slot)] = frame;
        writeIndex.store(slot, std::memory_order_release);
        hasData.store(true, std::memory_order_release);
    }

    /** GUI thread. Returns false if nothing has been published yet. */
    bool readLatest(SpectrumFrame& dest) const noexcept
    {
        if (! hasData.load(std::memory_order_acquire))
            return false;
        const int slot = writeIndex.load(std::memory_order_acquire);
        dest = slots[static_cast<size_t>(slot)];
        return true;
    }

private:
    static constexpr int kNumSlots = 4;
    std::array<SpectrumFrame, kNumSlots> slots {};
    std::atomic<int> writeIndex { 0 };
    std::atomic<bool> hasData { false };
};
} // namespace ember
