# Ember — Status

Last updated: 2026-09-14. Everything below is measured on the host machine
(Apple M2, macOS 26.5, Apple clang 21, JUCE 8.0.15) unless marked otherwise.

## Milestones

| Milestone | State | Evidence |
|-----------|-------|----------|
| M1 Skeleton | **done** | VST3 + AU + Standalone build; `pluginval --strictness-level 10` SUCCESS on VST3 and AU; `auval` clean |
| M2 Core DSP | **done** | crossovers, oversampling, per-band drive/mix/level; null and flatness tests green |
| M3 Full DSP | **done** | 19 styles, feedback, dynamics, tone, auto-gain, linear phase, M/S; all gates green |
| M4 GUI | in progress | processor contract fixed; panels being built |
| M5 Modulation | **done** (DSP side) | 23 sources, 64 routings, cycle rejection, state round-trip tested |
| M6 Presets & docs | in progress | 35-preset library designed and being generated; README/MANUAL/TESTING/CHANGELOG written |
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
| Manual checklist in `docs/TESTING.md` | renders produced in `test-renders/`; checklist not yet walked in a DAW |

## CPU

Measured with `ember_benchmark` on an Apple M2, worst-case settings (every band a
different style, feedback and dynamics active, auto-gain on):

| Configuration | % of one core |
|---|---|
| 6 bands, 4×, 48 kHz | 7.6 % |
| 6 bands, 2×, 48 kHz | 4.9 % |
| 6 bands, no oversampling | 2.9 % |
| 3 bands, 4×, 48 kHz | 3.2 % |
| 6 bands, 16×, 48 kHz | 24 % |

The specification's 3 % target is met at 3 bands / 4× and missed by roughly 2.5×
at 6 bands / 4×. What was tried: the obvious hot-loop work (trigonometry out of
the pan law, pointers hoisted out of per-sample loops, an integer delay instead
of Lagrange interpolation for a latency that is always whole, the spectrum FFT
skipped when no editor is open) moved it only from 11.4 % to 10.7 %, because the
resampling filters dominate. Switching the realtime path to polyphase IIR — and
keeping the linear-phase FIR for offline rendering, where latency and CPU do not
matter — was the real win, 10.7 % → 7.6 %.

With oversampling off the floor is 2.9 %, so 3 % at six oversampled bands is not
reachable by tuning; it would need the band chain itself to be roughly halved,
most plausibly by hand-vectorising the style shapers and the half-band filters.
That is a substantial piece of work and has not been attempted. The number is
reported rather than hidden, and the benchmark prints it in CI.

## Known gaps

- CPU target missed at the maximum configuration, as above.
- CI and the release pipeline are written but have never run — there is no
  remote repository yet, so no tag has been pushed and no installers have been
  produced by the pipeline. Local packaging scripts are syntax-checked only.
- Windows and Linux builds are untested: this host is macOS.
- The AUv3 target is wired behind `EMBER_BUILD_AUV3` but not built or validated.
