# Ember — Status

Last updated: 2026-09-21. Everything below is measured on the host machine
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
| Aliasing < −80 dBFS (10 kHz, 44.1 kHz, 16×, hard clip) | **pass** — **−88.7 dB** relative to the fundamental. Foldback −78.0, Rectify −87.0, Smudge −95.5. (This row read −91.2 / −84 / −92 / −95 until 2026-09-21. Those were stale, not a regression: the CPU pass re-measured the *unmodified* code and got exactly these figures, bit for bit, so the drift happened in some earlier change that did not refresh the table. −78 for Foldback still clears its own −60 dB gate, but the margin is thinner than the number here suggested.) |
| Feedback stability, 60 s of noise, all settings | **pass** — bounded and finite at every amount/frequency combination; full chain bounded under 20 s of worst-case abuse |
| State save/load round trip incl. modulation graph | **pass** — full graph round-trips through XML; empty graph clears rather than merges; 64-routing capacity round-trips |
| Parameter smoothing, no click above −60 dBFS | **pass** — worst artefact while slamming every parameter over silence stays under the gate; style changes and band-count changes are click-free |
| `pluginval --strictness-level 10` | **pass** on macOS VST3 and AU |
| No compiler warnings at `-Wall -Wextra` | **pass** for the DSP and plugin sources |
| Realtime CPU ≤ 3 % of one core (stereo 48 kHz, 6 bands, 4×) | **NOT met — 4.25 %**, down from 6.6 %. Met at three bands (2.15 %). See below |
| ASan/UBSan clean | **pass** — the full suite clean under `-fsanitize=address,undefined`; the Linux sanitiser job runs the same suite in CI |
| ThreadSanitizer clean | **pass** — 43 tests / 1,523,562 assertions, **0 data races**, via the `EMBER_ENABLE_TSAN` build. Last run before the CPU pass; that pass added no threading and one test case, so the suite it covered is now 44 / 1,523,642. Added because every other test in the suite calls `processBlock` from the thread that sets the parameters, so no amount of ASan over them could ever have found a race — and the one place a host is guaranteed to create one is exactly where a validator crashed |
| Manual checklist in `docs/TESTING.md` | 34 renders produced in `test-renders/`; checklist not yet walked in a DAW |
| GUI | **pass** — pluginval L10 constructs and destroys the editor, opens it while audio is processing, drives editor automation and runs parameter thread-safety checks. Visually inspected by rendering the real editor offscreen with `ember_rendereditor` at 800×480, 1100×640 and 1100×1000; this found a clipped band panel at the default size that no test caught |

## CPU

Measured with `ember_benchmark` on an Apple M2. The engine figures drive every
band with a different style plus feedback and dynamics; the full-plugin figures
add the wrapper — parameter resolution and modulation evaluation once per
32-sample control block — with six modulation routings active, which is the
number a host actually pays.

Both columns are the same benchmark binary built from the commit before the
optimisation pass and from the commit after it, run **alternately** five times
each on an otherwise idle machine; each figure is the median of those five.
Alternating matters: the absolute numbers on this machine drift by several per
cent over a session, so a before and an after taken an hour apart measure the
thermal state as much as the code.

| Configuration | before | after |
|---|---|---|
| **Full plugin, 6 bands, 4×, 48 kHz, 6 routings** | 6.57 % | **4.25 %** |
| **Full plugin, 3 bands, 4×, 48 kHz, 6 routings** | 3.24 % | **2.15 %** |
| Engine only, 6 bands, 4×, 48 kHz | 7.70 % | 4.94 % |
| Engine only, 6 bands, 2× | 4.95 % | 3.14 % |
| Engine only, 6 bands, no oversampling | 2.87 % | 2.04 % |
| Engine only, 6 bands, 16× | 23.55 % | 15.84 % |
| Engine only, 6 bands, 4×, 96 kHz | 15.37 % | 9.88 % |
| Engine only, 6 bands, 4×, 64-sample blocks | 7.52 % | 4.79 % |

The specification's 3 % target is **met at three bands** (2.15 %) and still
missed at six (4.25 %) — by about 1.4× now rather than the 2.2× it was missed by
before. Nothing in this pass changes what the plugin sounds like: every change
is the same arithmetic in a different loop, and the aliasing, flatness,
gain-match, stability and smoothing gates all report the same numbers to the
digit afterwards as before.

### What the second optimisation pass did

Step by step, each measured at the time it landed (full plugin, 6 bands, 4×):

| Change | % of a core |
|---|---|
| starting point | 6.63 |
| oversampler: the two channels into one loop | 5.76 |
| feedback loop into one loop; latency compensator written out | 5.41 |
| tone stack and crossover written out | 4.75 |
| dry delays written out | 4.62 |
| style shapers: the two channels into one loop | 4.44 |
| oversampler: four lanes in one SIMD register | 4.42 |
| those lanes' multiply-adds fused | 4.29 |

Those steps do not add up to the 6.57 → 4.25 in the table above, and should not
be expected to: they were each measured on a different afternoon. The table is
the honest number, because it is the only measurement where both binaries ran
under the same conditions.

Four findings sit underneath that list.

1. **Half of the per-sample DSP was calling JUCE out of line.**
   `IIR::Filter::processSample`, `LinkwitzRileyFilter::processSample` and
   `DelayLine::pushSample` / `popSample` are all explicitly instantiated in
   translation units of their own, so none of them can be inlined into a caller.
   The profile showed them plainly, as lazy-binding stubs in the middle of the
   audio path. A six-band stereo frame was paying six tone-stack calls, sixty
   crossover calls and twenty-eight delay-line calls, each wrapping three to
   fourteen multiply-adds of real work — and `IIR::Filter` re-read its
   coefficient order through a further call on every one of them. The tone
   stack, the Linkwitz-Riley sections and both dry delays are now written out in
   place with the same arithmetic in the same order. Between them, about 0.8
   points of a core: the largest single item in the pass.

2. **Every per-sample recursion ran one channel at a time.** A polyphase allpass
   cascade, a tone-stack biquad chain, a tape shaper's `tanh` feeding a
   one-pole, the feedback resonator — all of them are chains of dependent
   multiply-adds, twenty-odd cycles deep against a handful of cycles of issue.
   Finishing one channel before starting the other leaves most of the core idle
   for the whole block. Interleaving the channels into the same iteration covers
   two chains in the time of one, and costs nothing but a loop reordering: all
   the state is per channel, so no arithmetic moves. This is most of the rest of
   the pass.

3. **The oversampler is now four lanes wide.** The two polyphase cascades and
   the two channels sit in one `SIMDRegister<float>` as
   `[ch0 direct, ch0 delayed, ch1 direct, ch1 delayed]`, with the shorter
   cascade padded by an alpha = 1 section — an exact identity for a section
   whose state starts at zero, so the padding changes no bit. Honest accounting:
   the registers are worth about 1.5 % of the plugin over four *scalar* chains
   in one loop (4.50 % against 4.42 % at six bands, 17.3 % against 16.9 % at
   16×, alternating binaries). The chains in flight are the win; the vector is a
   small increment on top, because four interleaved chains already bring a
   latency-bound kernel back to being issue-bound and a wider register has
   nothing left to fill.

   One subtlety is worth recording. `SIMDRegister`'s `operator*` and `operator+`
   are separate inlined calls, so `a * x + st` rounds twice, where JUCE's scalar
   `alpha * input + v` is one expression and the compiler contracts it into a
   single fused multiply-add. The obvious vector spelling therefore drifted from
   JUCE by 3.6e-7 on a full-scale signal. Using `multiplyAdd`, with a
   precomputed `-a` so the state update fuses too, makes the output
   bit-identical — and slightly faster, since two vector ops became one.

4. **The integer-latency compensator was a delay line with no delay in it.**
   JUCE compensates the oversampler's fractional latency with a
   `DelayLine<float, Thiran>` whose delay is always in (0.618, 1.618], and over
   that range it always lands on `delayInt == 0`. The ring buffer, the modulo
   and the two `getSample` calls around it reduce to

       y[n] = x[n-1] + a (x[n] - y[n-1]),   a = (1 - f) / (1 + f)

   — the same three operations, without the call. It was 28 % of the whole
   downsampling cost.

`tests/test_aliasing.cpp` now runs the replacement oversampler against
`juce::dsp::Oversampling` side by side, over eight blocks of noise with a hard
clipper between the up and the down pass, for one and two channels and every
stage count, and requires the difference to be **exactly zero** and the
latencies to match exactly. Nothing else in the suite would have caught a drift
there: the alias gate passes with 8 dB of margin and would swallow a coefficient
wrong in the seventh place, and the comb that a half-sample latency error
produces in the dry/wet blend is not measured anywhere.

### Where the time goes now

Sampled with `sample(1)` over the 6-band / 4× / 48 kHz configuration, 3,969
samples inside `EmberEngine::process`:

| Component | Share of engine time | was |
|---|---|---|
| Oversampling (up 12 %, down 14 %) | **25 %** | 35 % |
| Saturation styles | 24 % | 25 % |
| Feedback loop | 22 % | 19 % |
| Crossover | 12 % | — |
| Gain ramp, finite scan, band mix | 6 % | — |
| Dynamics | 5 % | 5 % |
| Tone stack | 2 % | 6 % |
| DC block, pan/width, band sum | 2 % | — |

Shares of a total that is itself 36 % smaller, so every one of these is down in
absolute terms: oversampling by more than half, the tone stack by four fifths.

The feedback loop is now the densest per-sample code in the plugin. It runs at
the oversampled rate and its inner loop carries a `fastTanh` — which ends in a
float division — inside a resonator recurrence. It was left alone beyond the
channel interleave, because the obvious remaining saving is to hoist the
fractional-delay index arithmetic out of the loop, and that changes the
interpolation fraction by about a thousandth of a sample: small, but inside a
feedback loop, and larger than "floating-point noise" honestly allows.

### Things that were tried and did not pay

- **The SIMD itself.** Worth 1.5 %, as above, over four scalar chains in one
  loop. It is kept — it is also the simpler code, one padded loop rather than a
  common run plus two remainders, and it holds up better at 16× and 96 kHz —
  but anyone reading the SIMD and assuming that is where the speed came from
  would draw the wrong lesson. The speed came from the loop shape.
- **Lower-order half-band filters** (`maximum quality = false` on
  `juce::dsp::Oversampling`) moved the full plugin 6.38 % → 6.29 %, about 1 %,
  and would spend part of the margin the −80 dBFS aliasing gate passes with.
  Still not worth it.
- **Oversampling once globally** instead of once per band would cut the
  oversampling share by roughly 6×, but it moves the crossovers to the
  oversampled rate, which multiplies their cost by four and changes the sound.
  Net gain close to zero, and the risk is not.

### What would be left, if 3 % at six bands ever had to be met

Closing 4.25 % → 3 % needs another 1.4×, and the remaining time is spread thin:
no single component is more than a quarter of it any more. The only structural
lever still worth a quarter is batching the *bands* — running all twelve streams
(6 bands × 2 channels) of the oversampler through one set of registers instead
of six sets of four lanes. That needs the engine, not the band, to own the
oversampler, and it needs a transpose between the band-planar layout the shapers
want and the lane-planar layout the filters want, which eats back into the win.
It is a much larger change than anything in this pass and it gives up the
per-band bypass short-circuit.

These numbers are hardware-specific and should be read that way. The same
benchmark on the Linux CI runner reported 11.9 % for the full plugin at 6 bands
/ 4× before this pass — roughly 1.8× the M2 figures, on a shared cloud vCPU. A
CPU gate in CI would therefore be measuring the runner, not the plugin, which is
why the benchmark reports its numbers into the job log and only fails if the
plugin cannot sustain realtime at all.

## Known gaps

- CPU target missed at six bands (4.25 % against 3 %), met at three (2.15 %).
  See above for what closing the rest would cost.
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
