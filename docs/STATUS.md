# Ember — Status

Last updated: 2026-09-14. Everything below is measured on the host machine
(Apple M2, macOS 26.5, Apple clang 21, JUCE 8.0.15) unless marked otherwise.

## Milestones

| Milestone | State | Evidence |
|-----------|-------|----------|
| M1 Skeleton | **done** | VST3 + AU + Standalone build; `pluginval --strictness-level 10` SUCCESS on VST3 and AU; `auval` clean |
| M2 Core DSP | **done** | crossovers, oversampling, per-band drive/mix/level; null and flatness tests green |
| M3 Full DSP | **done** | 19 styles, feedback, dynamics, tone, auto-gain, linear phase, M/S; all gates green |
| M4 GUI | **done** | 11,600 lines of vector GUI; pluginval exercises the editor at strictness 10 |
| M5 Modulation | **done** | 23 sources, 64 routings, cycle rejection, state round-trip tested, sources wired to their parameters and covered by a regression test |
| M6 Presets & docs | **done** | 36 presets generated from the live layout and validated; README/MANUAL/TESTING/CHANGELOG written |
| M7 Release | packaging written, not yet tagged | CI + release workflows, Inno Setup, pkgbuild, Linux install — not yet exercised on real CI |

## Quality gates

| Gate (from the specification) | Result |
|---|---|
| Crossover null / flatness | **pass** — magnitude flat to **< 0.01 dB** for every band count 1–6; linear-phase sum nulls at **−150.9 dB** against the delayed input |
| Per-style gain match, ±1 dB at 0 dB drive | **pass** — worst residual **0.094 dB** across all 19 styles |
| Aliasing < −80 dBFS (10 kHz, 44.1 kHz, 16×, hard clip) | **pass** — **−91.2 dB** relative to the fundamental. Foldback −84, Rectify −92, Smudge −95 |
| Feedback stability, 60 s of noise, all settings | **pass** — bounded and finite at every amount/frequency combination; full chain bounded under 20 s of worst-case abuse |
| State save/load round trip incl. modulation graph | **pass** — full graph round-trips through XML; empty graph clears rather than merges; 64-routing capacity round-trips |
| Parameter smoothing, no click above −60 dBFS | **pass** — worst artefact while slamming every parameter over silence stays under the gate; style changes and band-count changes are click-free |
| `pluginval --strictness-level 10` | **pass** on macOS VST3 and AU |
| No compiler warnings at `-Wall -Wextra` | **pass** for the DSP and plugin sources |
| Realtime CPU ≤ 3 % of one core (stereo 48 kHz, 6 bands, 4×) | **NOT met — 7.6 %.** See below |
| ASan/UBSan clean | **pass** — all 28 tests / 36,771 assertions clean under `-fsanitize=address,undefined` locally on macOS; the Linux sanitiser job runs the same suite in CI |
| Manual checklist in `docs/TESTING.md` | 34 renders produced in `test-renders/`; checklist not yet walked in a DAW |
| GUI | built and automatically exercised — pluginval L10 constructs and destroys the editor, opens it while audio is processing, drives editor automation and runs parameter thread-safety checks, all passing. NOT visually inspected: this host grants the session neither screen-recording nor accessibility permission |

## CPU

Measured with `ember_benchmark` on an Apple M2. The engine figures drive every
band with a different style plus feedback and dynamics; the full-plugin figures
add the wrapper — parameter resolution and modulation evaluation once per
32-sample control block — with six modulation routings active, which is the
number a host actually pays.

| Configuration | % of one core |
|---|---|
| **Full plugin, 6 bands, 4×, 48 kHz, 6 routings** | **6.6 %** |
| **Full plugin, 3 bands, 4×, 48 kHz, 6 routings** | **3.2 %** |
| Engine only, 6 bands, 4×, 48 kHz | 7.7 % |
| Engine only, 6 bands, 2× | 5.0 % |
| Engine only, 6 bands, no oversampling | 2.9 % |
| Engine only, 6 bands, 16× | 23.7 % |
| Engine only, 6 bands, 4×, 96 kHz | 15.4 % |

The specification's 3 % target is **met at three bands** (3.2 %) and missed by
about 2× at six (6.6 %). Three things moved the number, in increasing order of
usefulness:

1. Hot-loop work — trigonometry out of the pan law, pointers hoisted out of
   per-sample loops, an integer delay instead of Lagrange interpolation for a
   latency that is always whole, the spectrum FFT skipped when no editor is
   open. Worth only 11.4 % → 10.7 %: the resampling filters dominate.
2. Moving the realtime path to polyphase IIR half-band filters and keeping the
   linear-phase FIR for offline rendering, where latency and CPU do not matter.
   10.7 % → 7.7 %.
3. Caching the parameter pointers. `resolveParameters` and `pushSourceParameters`
   run ~1500 times a second and touch well over a hundred parameters each time;
   looking them up by id meant a string copy and a hash per parameter per block.
   That alone was 1.8 points of a core — the full plugin went 9.3 % → 6.6 %.

With oversampling off the engine floor is 2.9 %, so 3 % at six oversampled bands
is not reachable by tuning. It would need the band chain roughly halved, most
plausibly by hand-vectorising the style shapers and the half-band filters. That
has not been attempted. The benchmark prints these numbers in CI so the figure
cannot quietly drift.

## Known gaps

- CPU target missed at the maximum configuration, as above.
- CI and the release pipeline are written but have never run — there is no
  remote repository yet, so no tag has been pushed and no installers have been
  produced by the pipeline. Local packaging scripts are syntax-checked only.
- Windows and Linux builds are untested: this host is macOS.
- The AUv3 target is wired behind `EMBER_BUILD_AUV3` but not built or validated.
