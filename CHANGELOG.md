# Changelog

All notable changes to Ember are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.0.0] — 2026-09-14

First public release of Ember, a multiband analog saturation and distortion plugin.

### Added

#### Multiband engine

- Variable band count, 1 to 6 bands, changeable while audio is playing; band count and
  crossover changes are crossfaded over 20 ms so they do not click.
- Linkwitz-Riley 4th-order crossovers implemented as a cascaded tree with second-order
  all-pass compensation on every band, so the bands sum back to unity magnitude regardless
  of how many are active.
- Optional linear-phase crossover mode using FFT partitioned convolution, for mastering work
  where phase smear matters more than latency. Off by default; latency is reported to the
  host either way.
- Per-band solo and bypass, both crossfaded rather than switched.

#### Saturation

- 19 saturation styles: Clean Tube, Warm Tube, Subtle Tube, Broken Tube, Clean Tape,
  Warm Tape, Bright Tape, Transformer, Clean Amp, Crunch, Lead, Smudge, Rectify, Foldback,
  Hard Clip, Decimate, Bitcrush, Shimmer and Breathe.
- Per-style gain matching measured at `prepare()` by running a deterministic pink-noise
  burst through each shaper at 1 dB drive steps, so switching styles or raising drive does
  not jump the level. The table is computed once per sample rate and shared across bands.
- First-order antiderivative anti-aliasing on the hard-edged shapers (hard clip, wavefolder,
  rectifiers), with an ill-conditioned-difference guard that falls back to direct evaluation
  when consecutive samples are too close.

#### Per-band controls

- Drive, 0 to 40 dB, applied as a smoothed pre-shaper gain with the matched make-up applied
  after.
- Wet/dry mix, output level, pan and stereo width per band.
- Resonant feedback loop per band: a fractional-delay line tuned to a settable feedback
  frequency plus a state-variable bandpass, bounded by an in-loop `tanh` limiter and a hard
  ceiling so it cannot run away at any Q or gain setting.
- Bipolar dynamics per band — expansion below centre, compression above — on a single
  control.
- Three-band tone stack per band: low shelf, mid peak, high shelf.

#### Modulation

- Drag-and-drop modulation: drag a source onto any continuous control to create a
  connection, with per-connection depth and a response curve (linear, exponential in,
  exponential out, S-curve, stepped).
- 23 modulation sources: 4 multi-point X-LFOs with host sync or free running, 2 envelope
  generators (transient- or MIDI-triggered), 4 band-selectable envelope followers, an XY
  controller, 4 MIDI sources and 8 macros.
- 64 preallocated connection slots, so editing the graph never allocates on the audio
  thread. Cycle detection runs at edit time, which lets sources modulate other sources
  without recursion during processing.
- Matrix view for reviewing and editing every connection at once.
- MIDI Learn for CC sources.
- The full modulation graph is saved and restored with the plugin state and with presets.

#### Global

- Oversampling at 1×, 2×, 4×, 8× and 16× using equiripple FIR half-band polyphase filters,
  with a separate factor selectable for offline rendering.
- Auto-gain with RMS matching and smoothing.
- Stereo and mid/side processing modes.
- Input gain, output gain, global wet/dry mix, and output dither (off, rectangular,
  triangular).
- Exact latency reporting through `setLatencySamples` for both oversampling and
  linear-phase mode.
- Denormal and non-finite protection: `ScopedNoDenormals` across `processBlock`, per-control-
  block sanitising of every recursive stage, and a final output guard, so one bad host buffer
  cannot poison the engine state.

#### Interface

- Resizable GUI that scales cleanly across display densities.
- Live spectrum analyser with the band split points draggable directly on the display and
  each band's response drawn in place.
- Preset browser with factory presets, plus A/B comparison and undo/redo.

#### Formats and platforms

- VST3, Audio Unit and Standalone builds. The Audio Unit registers as
  `kAudioUnitType_MusicEffect` so that MIDI-driven modulation sources receive MIDI in Logic
  Pro.
- macOS (arm64, x86_64 and universal), Windows x64, and Linux x86_64.
- AUv3 app extension available behind the off-by-default `EMBER_BUILD_AUV3` CMake option.

#### Build and verification

- CMake 3.22+ build with presets for every supported platform, fetching JUCE 8.0.15 and
  Catch2 v3.16.0 at configure time.
- `ember_dsp`, a host-free static library holding the whole audio engine, so the DSP can be
  tested headlessly.
- Catch2 unit tests covering crossover summing, per-style behaviour, aliasing floors,
  feedback stability and parameter smoothing.
- `ember_benchmark`, an offline realtime-factor report across band counts, oversampling
  factors, sample rates and buffer sizes.
- `ember_render`, an offline render tool that writes reference WAVs to `test-renders/` for
  the manual checklist.
- `scripts/run-pluginval.sh`, which fetches pluginval v1.0.4 and validates every built format
  at strictness level 10.
- Manual release checklist in `docs/TESTING.md`.

### Licensing

- Released under the GNU General Public License v3, because Ember links JUCE 8 under its
  GPLv3 terms. See `LICENSE`.
