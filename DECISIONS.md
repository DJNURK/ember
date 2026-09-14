# Ember — Design Decisions

Decisions made autonomously while implementing the spec, with rationale. Where the spec
asked for something impossible or inadvisable, the closest sound alternative is described
here rather than silently dropped.

## Toolchain

**D1 — JUCE 8.0.15 pinned via `FetchContent`.** Latest 8.x at time of writing.
Pinned by tag (not branch) so builds are reproducible. `GIT_SHALLOW` keeps the
clone small in CI.
*Licensing:* JUCE 8 is GPLv3 for open-source distribution; closed-source distribution
requires a commercial JUCE licence. Ember is therefore released under GPLv3. This is
stated in `LICENSE` and the README.

**D2 — CMake 4.3 / Ninja, no Projucer.** Presets in `CMakePresets.json`.

**D3 — Catch2 v3.16.0** for unit tests, `catch_discover_tests` so every `TEST_CASE`
becomes a `ctest` entry.

**D4 — pluginval v1.0.4** fetched as a prebuilt release binary in CI rather than built
from source; it is a test harness, not a shipped dependency.

## DSP

**D5 — Crossovers: LR4 as a cascaded-tree with all-pass compensation.** A naive chain of
LR4 splits is *not* magnitude-flat for more than two bands: each band above the first
picks up the phase rotation of the crossovers below it. Ember therefore compensates every
band with the second-order all-pass sections of the crossovers it did not pass through,
which restores the LR property (sum of all bands = unity magnitude, 360°/crossover phase
rotation). Verified by the null test in `tests/test_crossover.cpp`.

**D6 — Oversampling: `juce::dsp::Oversampling`, equiripple FIR half-band polyphase.**
The spec asks for polyphase half-band. JUCE offers IIR (minimum-phase, low latency) and
FIR equiripple (linear-phase, higher latency but no phase smear and much better stopband).
Ember uses **FIR equiripple** for the realtime path and reports its latency exactly; the
separate offline/render setting can select a higher factor. Rationale: aliasing
suppression is the point of the feature, and the FIR's latency is reported so the host
compensates it.

**D7 — Antiderivative anti-aliasing (ADAA, 1st order) for hard-edged shapers**
(hard clip, wavefolder, rectifiers). ADAA cuts aliasing by ~20–30 dB at a given
oversampling factor, which lets those styles meet the < −80 dBFS alias gate without
running at 16×. Smooth `tanh`-class shapers do not need it and pay no cost.
ADAA needs an ill-conditioned-difference guard: when `|x[n] − x[n−1]|` is below a
threshold the shaper falls back to the direct evaluation.

**D8 — Drive law.** Drive is specified 0–40 dB and applied as a smoothed linear gain
before the shaper, with a per-style **gain-match** curve measured offline (pink-noise RMS
through each shaper at each drive point, fitted) and applied after. This is what makes
"switching style doesn't jump in level" and "consistent perceived loudness at 0 dB drive"
true rather than aspirational.

**D9 — Feedback loop stability.** The resonant feedback path is a fractional-delay line
tuned to the feedback frequency plus a state-variable bandpass, and is *unconditionally*
bounded by a `tanh` soft limiter inside the loop plus a hard ceiling. Feedback gain is
capped below unity at the loop's peak magnitude, so it cannot run away even with the
bandpass Q at maximum. Verified by a 60 s noise stability test.

**D10 — Linear-phase mode: FFT partitioned-convolution crossovers.** Implemented as
FIR band filters derived from the same crossover frequencies, convolved via
`juce::dsp::Convolution` in a uniform-partition mode, with latency reported through
`setLatencySamples`. Linear phase is *not* free: it costs latency and pre-ringing, so it
is off by default.

**D11 — Modulation at control rate 32 samples, linearly interpolated per sample.**
Sources evaluate once per control block; each connection's contribution is ramped across
the block so no zipper noise reaches the audio. 64 connection slots are preallocated
(spec asks for ≥ 50); the graph is a flat source→target list evaluated in dependency order
with cycle detection at edit time, so source-modulating-source works without recursion at
audio rate.

**D12 — NaN/Inf and denormal policy.** `ScopedNoDenormals` at the top of `processBlock`;
every feedback/recursive stage is sanitised per control block (non-finite state is reset
to zero rather than propagating). A final output guard replaces non-finite samples with
silence. This is cheap insurance against a single bad host buffer poisoning the state.

## Scope

**D13 — iOS AUv3 is an off-by-default CMake option** (`EMBER_BUILD_AUV3`). It is a stretch
goal per the spec; the target is wired but not part of the default release matrix.
VST3 does not exist on iOS/Android, as the spec notes.

**D14 — AU registers as `kAudioUnitType_MusicEffect` (`aumf`), not `aufx`.**
Ember declares MIDI input because the spec requires MIDI modulation sources
(velocity, CC, mod wheel, note) and MIDI Learn. `auval` warns that an AU
implementing `MusicDeviceMIDIEvent` while typed `aufx` is mis-declared, and more
importantly Logic Pro will not route MIDI to an `aufx` unit — the MIDI sources
would silently never fire. `aumf` is the correct type for a MIDI-receiving
effect and is what comparable plugins use.

**D15 — Per-style gain matching is measured at `prepare()`, not hand-tuned.**
`StyleCalibrator` runs a fixed, deterministic pink-noise burst through every
style at 1 dB drive steps and stores the RMS deviation in a lookup table, which
the band chain interpolates at runtime. This makes "consistent perceived
loudness at 0 dB drive" and "switching styles doesn't jump in level" true by
construction rather than by hand-fitted constants that rot as the shapers are
tuned. The table depends only on sample rate, so it is computed once per rate,
cached, and shared by all six bands.

**D16 — Gain matching is defined against a reference stimulus: FLAT noise at
−18 dBFS RMS.** There is no signal-independent "correct" output gain for a
nonlinearity — the compensation that loudness-matches one stimulus does not
match another, because the stages respond to spectrum and crest factor, not just
level. Ember therefore calibrates and verifies against a stated reference.

The reference is flat, not pink, and that choice is load-bearing. Several styles
roll off above a few kHz (the amp cascade's post-lowpass sits at 7.5–9 kHz, and
first-order ADAA is a two-point average that costs ~3 dB on a flat spectrum and
almost nothing on a pink one). Pink noise carries very little energy up there, so
a pink-calibrated table is measuring a different quantity than the full-band
loudness the specification's "±1 dB between styles at 0 dB drive" gate is about —
for the amp styles the two differ by up to 4.7 dB. Flat is also the
assumption-free choice: the calibrator is handed one number, the oversampled
rate, and cannot distinguish 96 kHz with no oversampling (where the band really
does carry signal to Nyquist) from 48 kHz at 2× (where it does not), so weighting
the band evenly measures the style over its whole operating range instead of over
a guess about the host.

`tests/test_styles.cpp` measures with an independently seeded instance of that
same reference, normalised over the exact window it measures, so it checks the
table rather than restating it. Measured residual after compensation, across all
19 styles at 0 dB drive: worst 0.094 dB. (An earlier version of the test used
pink noise and reported up to 4.5 dB of "error" against a calibration that was
correct — the mismatch was in the measurement, not the DSP.)

**D17 — Crossover flatness is measured from the impulse response, not from a
windowed noise burst.** Comparing the windowed spectra of noise before and after
the crossover looks equivalent and is not: where the system has appreciable group
delay — precisely what happens around a crossover — the analysis window no longer
lines up with the delayed output, and the resulting amplitude error is worst at
the low frequencies where the delay is longest. That method reported ~5 dB of
"error" for a crossover that is in fact flat to better than 0.01 dB. The impulse
response, captured until it has decayed (verified in the test), gives the exact
transfer function.

**D18 — Aliasing is measured with a bin-centred fundamental and a rectangular
window.** The obvious measurement — Hann-window the output and look for
non-harmonic energy — is wrong by roughly the amount being measured: the
window's own sidelobes sit about −50 dB below a strong peak a few bins away and
land directly in the bins being called "aliases", so the method reports about
−51 dB regardless of how good the anti-aliasing is. Choosing a fundamental that
completes an exact integer number of cycles in the FFT window (3716 cycles in
16384 samples at 44.1 kHz, i.e. 10001.2 Hz) puts the fundamental, every harmonic
and every folded image on exact bins, so the spectrum can be taken with no window
and no leakage. Measured alias floors at 16× with this method: Hard Clip
−91 dB, Foldback −84 dB, Rectify −92 dB, Smudge −95 dB, relative to the
fundamental — comfortably inside the specification's −80 dB gate.
