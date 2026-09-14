# Ember — Implementation Plan

## Milestones

| # | Milestone | Exit criteria |
|---|-----------|---------------|
| M1 | Skeleton | CMake + JUCE builds VST3/AU/Standalone on host; pass-through passes pluginval L10; CI matrix green on 3 OSes |
| M2 | Core DSP | Crossovers + oversampling + 4 styles + per-band drive/mix/level; null test < -100 dB; unit tests green |
| M3 | Full DSP | 16+ styles, feedback, dynamics, tone, auto-gain, linear phase, M/S; per-feature tests |
| M4 | GUI | Resizable UI, spectrum display, band drag-editing, preset browser, A/B, undo |
| M5 | Modulation | 6 source types, drag-and-drop routing, matrix view, 50+ connections, state persistence |
| M6 | Presets & docs | 30+ factory presets, README, MANUAL.md, CHANGELOG, TESTING.md |
| M7 | Release | Packaging for Win/macOS/Linux, release.yml, tag v1.0.0, artifacts on Release page |

## Architecture

```
EmberAudioProcessor  (src/plugin)
  └── EmberEngine    (src/dsp)              — owns the whole audio graph
        ├── ParameterSnapshot                — lock-free params + modulation applied
        ├── ModulationEngine                 — control-rate (32 smp) source eval + routing
        ├── Crossover (LR4 tree | LinearPhase FIR)
        ├── Band[0..5]
        │     ├── Oversampler (1/2/4/8/16x)
        │     ├── SaturationStyle  (18 styles, ADAA where applicable)
        │     ├── FeedbackLoop     (fractional delay + BPF + soft limiter)
        │     ├── Dynamics         (bipolar: expander <-> compressor)
        │     ├── ToneStack        (low shelf / peak / high shelf)
        │     └── Level / Pan / Width / Mix
        ├── Band summer + solo/bypass crossfades
        ├── AutoGain (RMS-matched, smoothed)
        └── Global mix / output gain / latency reporting
```

Threading: audio thread owns all DSP. GUI reads spectrum frames through a lock-free FIFO
and parameter values through `std::atomic`. No allocation, locks, logging, or exceptions
below `processBlock`.

## Build & verification loop

Each milestone ends with: Release build → `ctest` → `pluginval --strictness-level 10`
→ commit. Nothing moves forward with a red gate.
