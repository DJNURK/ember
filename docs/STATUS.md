# Ember — Status

Last updated: 2026-09-15. Everything below is measured on the host machine
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
| Realtime CPU ≤ 3 % of one core (stereo 48 kHz, 6 bands, 4×) | **NOT met — 6.6 %.** See below |
| ASan/UBSan clean | **pass** — the full suite clean under `-fsanitize=address,undefined`; the Linux sanitiser job runs the same suite in CI |
| ThreadSanitizer clean | **pass** — 43 tests / 1,523,562 assertions, **0 data races**, via the `EMBER_ENABLE_TSAN` build. Added because every other test in the suite calls `processBlock` from the thread that sets the parameters, so no amount of ASan over them could ever have found a race — and the one place a host is guaranteed to create one is exactly where a validator crashed |
| Manual checklist in `docs/TESTING.md` | 34 renders produced in `test-renders/`; checklist not yet walked in a DAW |
| GUI | **pass** — pluginval L10 constructs and destroys the editor, opens it while audio is processing, drives editor automation and runs parameter thread-safety checks. Visually inspected by rendering the real editor offscreen with `ember_rendereditor` at 800×480, 1100×640 and 1100×1000; this found a clipped band panel at the default size that no test caught |

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

These numbers are hardware-specific and should be read that way. The same
benchmark on the Linux CI runner reports 11.9 % for the full plugin at 6 bands /
4× and 6.1 % at 3 bands — roughly 1.8× the M2 figures, on a shared cloud vCPU.
A CPU gate in CI would therefore be measuring the runner, not the plugin, which
is why the benchmark reports its numbers into the job log and only fails if the
plugin cannot sustain realtime at all.

### Where the time goes

Sampled with `sample(1)` over the 6-band / 4× / 48 kHz configuration, 4,937
samples inside `EmberEngine::process`:

| Component | Share of engine time |
|---|---|
| Oversampling (up 16 %, down 19 %) | **35 %** |
| Saturation styles | 25 % |
| Feedback loop | 19 % |
| Tone stack | 6 % |
| Dynamics | 5 % |
| Crossover, band sum, misc | ~10 % |

This is what makes the target unreachable by tuning: closing 6.4 % → 3 % needs a
**2.1× whole-engine speedup**, which is more than deleting the oversampler
entirely would buy. With oversampling off the engine floor is already 2.9 %.

Two cheaper levers were measured and rejected:

- **Lower-order half-band filters** (`maximum quality = false` on
  `juce::dsp::Oversampling`) moved the full plugin 6.38 % → 6.29 %, about 1 %,
  and would spend part of the 11 dB of margin the −80 dBFS aliasing gate is
  currently passing with. Not worth it.
- **Oversampling once globally** instead of once per band would cut the 35 %
  by roughly 6×, but it moves the crossovers to the oversampled rate, which
  multiplies their cost by four and changes the sound. Net gain is close to
  zero and the risk is not.

What would actually work is hand-vectorising the two dominant blocks — the
half-band filters and the style shapers — across the 12 independent streams
(6 bands × 2 channels). The polyphase IIR is serial per sample but the streams
are independent, so 4-wide NEON is available on the 35 %. The shapers are
harder: ADAA carries state across samples, and the bands run different styles,
so only the 2 channels vectorise cleanly. This has not been attempted; it is a
substantial rewrite of code that currently passes 1,523,562 assertions, and it
was not worth risking against a release. The benchmark prints these numbers in
CI so the figure cannot quietly drift.

## Known gaps

- CPU target missed at the maximum configuration, as above.
- No tag has been pushed, so no public release exists yet. A
  `workflow_dispatch` dry run of `release.yml` has exercised the pipeline:
  the **macOS** `.pkg` was produced and expanded to confirm `Ember.vst3` →
  `/Library/Audio/Plug-Ins/VST3`, `Ember.component` →
  `/Library/Audio/Plug-Ins/Components` and `Ember.app` → `/Applications`,
  with `lipo` confirming `x86_64 arm64`; the **Linux** `.tar.gz` was produced
  and confirmed to carry a correct VST3 bundle plus an `install.sh` that
  targets `~/.vst3`. The **Windows** installer step failed on that run (Git
  Bash mangling ISCC's switches) and the fix has not yet been re-run, so the
  `.exe` is the one asset never yet built successfully.
- Windows and Linux plugin builds are exercised by CI, not on this host.
- The AUv3 target is wired behind `EMBER_BUILD_AUV3` but cannot be built on
  this machine, and the reason is worth recording. JUCE only emits an AUv3
  app-extension target under the **Xcode generator**; with Ninja the format is
  silently dropped, and `-DEMBER_BUILD_AUV3=ON` produces `Ember_AU`,
  `Ember_VST3` and `Ember_Standalone` with no `Ember_AUv3` and no warning.
  Switching to `-G Xcode` then fails at configure — `Xcode 1.5 not supported`
  — because only the Command Line Tools are installed, not Xcode.app.

  So AUv3 needs two things this host does not have: a full Xcode install, and
  a CMake preset using the Xcode generator rather than Ninja. Until both
  exist, the flag does nothing. The specification listed AUv3 as an
  off-by-default stretch goal, so this does not block the release, but the
  flag should not be mistaken for a working build path.
