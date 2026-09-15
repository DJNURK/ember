// TEMPORARY audit harness - not part of the suite. Delete after the audit.
#include <catch2/catch_test_macros.hpp>
#include "dsp/modulation/ModulationEngine.h"
#include <random>
#include <thread>
#include <atomic>

using namespace ember;

namespace
{
std::mt19937 rng{12345};
int ri(int lo, int hi)
{
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}
float rf(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}
} // namespace

TEST_CASE("audit: fuzz graph edits + control blocks", "[audit]")
{
    ModulationEngine eng;
    eng.prepare(48000.0, 512, 40);

    // Register a realistic ownership map (source fields as targets).
    int t = 0;
    for (int i = 0; i < kNumXLFOs; ++i)
    {
        eng.setTargetOwner(t++, flatSourceIndex(ModSourceType::XLFO, i), ModSourceField::Rate);
        eng.setTargetOwner(t++, flatSourceIndex(ModSourceType::XLFO, i), ModSourceField::Depth);
    }
    for (int i = 0; i < kNumMacros; ++i)
        eng.setTargetOwner(t++, flatSourceIndex(ModSourceType::Macro, i), ModSourceField::Value);
    for (int i = 0; i < kNumEnvGenerators; ++i)
        eng.setTargetOwner(t++, flatSourceIndex(ModSourceType::EnvelopeGenerator, i), ModSourceField::Attack);

    std::array<float, kMaxBands> rms{};

    for (int iter = 0; iter < 20000; ++iter)
    {
        const int op = ri(0, 6);

        if (op == 0)
        {
            ModConnection c;
            c.sourceIndex = ri(-3, kNumModSources + 3);
            c.targetIndex = ri(-3, 60);
            c.amount = rf(-2.0f, 2.0f);
            c.curve = static_cast<ModCurve>(ri(-2, 8));
            c.smoothingMs = rf(-10.0f, 900.0f);
            c.enabled = ri(0, 1) != 0;
            eng.addConnection(c);
        }
        else if (op == 1)
        {
            eng.removeConnection(ri(-2, eng.getNumConnections() + 2));
        }
        else if (op == 2)
        {
            ModConnection c;
            c.sourceIndex = ri(-1, kNumModSources);
            c.targetIndex = ri(-1, 60);
            c.amount = rf(-1.0f, 1.0f);
            c.curve = static_cast<ModCurve>(ri(0, 4));
            c.smoothingMs = rf(0.0f, 500.0f);
            eng.setConnection(ri(-2, eng.getNumConnections() + 2), c);
        }
        else if (op == 3)
        {
            std::array<LfoPoint, 48> pts{};
            const int n = ri(-4, 48);
            for (auto& p : pts)
                p = LfoPoint{rf(-0.5f, 1.5f), rf(-2.0f, 2.0f), static_cast<ModCurve>(ri(-2, 8))};
            eng.setXLfoShape(ri(-2, kNumXLFOs + 2), pts.data(), n);
        }
        else if (op == 4)
        {
            juce::ValueTree tree(ModStateIds::modulation);
            const int n = ri(0, 80);
            for (int i = 0; i < n; ++i)
            {
                juce::ValueTree ch(ModStateIds::connection);
                ch.setProperty(ModStateIds::source, ri(-10, 40), nullptr);
                ch.setProperty(ModStateIds::target, ri(-10, 5000), nullptr);
                ch.setProperty(ModStateIds::amount, rf(-5.0f, 5.0f), nullptr);
                ch.setProperty(ModStateIds::curve, ri(-5, 40), nullptr);
                ch.setProperty(ModStateIds::smoothing, rf(-100.0f, 5000.0f), nullptr);
                ch.setProperty(ModStateIds::enabled, ri(0, 1) != 0, nullptr);
                tree.appendChild(ch, nullptr);
            }
            eng.fromValueTree(tree);
        }
        else if (op == 5)
        {
            ModSourceParameters sp;
            for (auto& l : sp.lfos)
            {
                l.rateHz = rf(-1.0f, 1000.0f);
                l.tempoSync = ri(0, 1) != 0;
                l.syncBeats = rf(-1.0f, 100.0f);
                l.phaseOffset = rf(-3.0f, 3.0f);
                l.steps = ri(-4, 200);
                l.smoothingMs = rf(-10.0f, 900.0f);
                l.depth = rf(-2.0f, 2.0f);
            }
            for (auto& e : sp.envGenerators)
            {
                e.attackMs = rf(-5.0f, 20000.0f);
                e.decayMs = rf(-5.0f, 20000.0f);
                e.sustain = rf(-1.0f, 2.0f);
                e.releaseMs = rf(-5.0f, 40000.0f);
                e.threshold = rf(-1.0f, 2.0f);
                e.detectorBand = ri(-4, 12);
                e.trigger = static_cast<EgTriggerMode>(ri(-2, 5));
            }
            for (auto& f : sp.envFollowers)
            {
                f.attackMs = rf(-5.0f, 3000.0f);
                f.releaseMs = rf(-5.0f, 9000.0f);
                f.band = ri(-4, 12);
                f.gainDb = rf(-100.0f, 100.0f);
                f.floorDb = rf(-300.0f, 50.0f);
            }
            sp.xy.x = rf(-1.0f, 2.0f);
            sp.xy.y = rf(-1.0f, 2.0f);
            sp.xy.smoothingMs = rf(-10.0f, 900.0f);
            sp.xy.axis = static_cast<XyAxis>(ri(-2, 5));
            for (auto& m : sp.midi)
            {
                m.kind = static_cast<MidiSourceKind>(ri(-2, 6));
                m.ccNumber = ri(-10, 400);
                m.smoothingMs = rf(-10.0f, 900.0f);
            }
            for (auto& m : sp.macros)
            {
                m.value = rf(-2.0f, 2.0f);
                m.smoothingMs = rf(-10.0f, 900.0f);
            }
            eng.setSourceParameters(sp);
        }
        else
        {
            juce::MidiBuffer mb;
            const int msgs = ri(0, 6);
            for (int i = 0; i < msgs; ++i)
            {
                const juce::uint8 bytes[3] = {static_cast<juce::uint8>(ri(0x80, 0xFF)),
                                              static_cast<juce::uint8>(ri(0, 255)),
                                              static_cast<juce::uint8>(ri(0, 255))};
                mb.addEvent(bytes, ri(1, 3), 0);
            }
            eng.processMidi(mb);
        }

        for (auto& v : rms)
            v = rf(0.0f, 4.0f);

        eng.setTransport(rf(-100.0f, 1.0e9f), rf(-1.0e9f, 1.0e9f), ri(0, 1) != 0);
        eng.updateControlBlock(rms.data(), ri(0, kMaxBands), ri(-2, 64));

        for (int k = -2; k < 45; ++k)
        {
            const float o = eng.getModulationOffset(k);
            REQUIRE(std::isfinite(o));
        }
        for (int k = -2; k < kNumModSources + 2; ++k)
            REQUIRE(std::isfinite(eng.getSourceValue(k)));
    }
}

TEST_CASE("audit: concurrent plan publish/consume", "[audit]")
{
    ModulationEngine eng;
    eng.prepare(48000.0, 512, 32);
    for (int i = 0; i < 16; ++i)
        eng.setTargetOwner(i, flatSourceIndex(ModSourceType::Macro, i % kNumMacros), ModSourceField::Value);

    std::atomic<bool> stop{false};
    std::array<float, kMaxBands> rms{};
    rms.fill(0.25f);

    std::thread audio(
        [&]
        {
            while (!stop.load())
            {
                eng.updateControlBlock(rms.data(), kMaxBands, kControlBlockSize);
                for (int k = 0; k < 32; ++k)
                    (void)eng.getModulationOffset(k);
            }
        });

    std::mt19937 r2{999};
    for (int i = 0; i < 200000; ++i)
    {
        ModConnection c;
        c.sourceIndex = std::uniform_int_distribution<int>(0, kNumModSources - 1)(r2);
        c.targetIndex = std::uniform_int_distribution<int>(0, 31)(r2);
        c.amount = 0.5f;
        c.smoothingMs = 5.0f;
        if (!eng.addConnection(c))
            eng.removeConnection(std::uniform_int_distribution<int>(0, 63)(r2));
    }

    stop.store(true);
    audio.join();
    SUCCEED();
}
