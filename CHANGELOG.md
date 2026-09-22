# Changelog

All notable changes to Ember are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [2.1.1] — 2026-09-22

Three of the four things 2.1.0 listed as known limitations, plus everything an
adversarial review of those fixes turned up. One of the four was withdrawn: the
fix was worse than the gap, which is the sort of thing you only find out by
checking the fix against real content.

### Fixed

- **Crossovers can no longer be driven closer than a third of an octave.** The
  engine enforced 1/35th while the GUI enforced a third and the manual promised
  a third; automation could put two edges almost on top of each other and leave
  a band a few hertz wide.
- **Dither works.** The parameter saved and restored but was read by no code —
  Bitcrush always used its own triangular default. Every factory preset now
  stores Triangular, which is what the DSP was doing when they were auditioned,
  so wiring the control up does not change how any of them sound.
- **The XY pad's Y axis works.** A pad has two dimensions but a modulation
  source emits one value, and the axis selector defaulted to X with nothing
  able to change it. There is an **XY Axis** parameter now, and a control for
  it beside the pad — it was reachable only from host automation at first.
  Each coordinate is also its own modulation destination: routing to **XY Y**
  reached nothing at all before, and routing to **XY X** drove whichever axis
  was selected.
- **The band strip cannot come up empty.** Every module's width came from an
  animated weight starting at zero, so the first layout had nothing to share
  out and gave up. A host ticks its display clock a frame later and fills the
  strip, which is why this was never visible in use — but the steady state must
  not depend on an animation having run, and anywhere the clock does not tick
  the plugin's whole control surface stayed blank. The documentation
  screenshots were rendered that way and have been retaken.
- **The tone editor no longer paints across the band beside it.** With the
  Learn panel open and six bands up, a module is too narrow for the tone
  editor, so the layout dropped it — and the visibility pass that ran
  afterwards put it back, at the width it had before. It was drawn 89 px
  outside its own module, over its neighbour. Visibility is now settled before
  the modules are laid out, so the layout has the last word.
- **The band layout the editor reads is no longer a data race.** It came
  straight out of the struct the audio thread rewrites every control block.
  The engine now publishes the layout its filters are actually running —
  modulation applied, spacing enforced — and the editor reads that.
- **The crossover's modulation ghost points at the real edge.** It was drawn
  from the modulated parameter alone, without the spacing the engine enforces
  on top, so the marker whose only job is to say where the edge really is
  could sit a third of an octave away from it — and two ghosts could be drawn
  in the opposite order to the edges they described.
- **The Reference tab reads as sentences.** Each article's advice is written to
  follow the words "When to use it", and the panel was dropping them, so every
  entry's second paragraph began mid-clause on a lowercase word.
- **The parameter reference reads as sentences.** Every one of the 54 rows in
  the manual's table ended with an orphan fragment, because the parser took
  only the first physical line of each entry's advice and appended the rest to
  the prose above it. The same parser runs inside the plugin, so the Learn
  panel had it too.
- **A release cannot ship with the wrong version on it.** The tag-versus-build
  check was a warning, and v1.0.1 duly published binaries reporting 1.0.0. It
  fails the build now.
- **Tone nodes can be dragged in every band.** The drag was clamped to the
  band, but each node has its own range — the high shelf stops at 1 kHz — and a
  band can sit entirely outside it. Band 1 runs to 120 Hz by default, so a high
  shelf dragged anywhere in it saturated back to 1 kHz: the node could not be
  moved at all. Where band and range do not overlap, the range wins.
- **"Distribute bands evenly" distributes them.** Each edge was clamped against
  the one above it as it stood before the command ran, so a layout squeezed to
  the bottom of the spectrum stayed squeezed.
- **The display cannot hold a frequency it cannot draw.** With two edges pinned
  at 20 kHz the fallback midpoint was 22.4 kHz, outside both the axis and the
  parameter.
- **The help says what the plugin does.** The Dither, Crossover Frequency, XY X
  and XY Y articles all still described the behaviour this release changed —
  including one that told the user Dither was not connected to anything.

### Withdrawn

- **Tone nodes are no longer clamped into their band.** 2.1.0 listed this as a
  gap and 2.1.1 first "fixed" it. The fix was wrong: no preset stores a node
  frequency, so all 36 factory presets load at the defaults — 150 Hz, 1 kHz,
  4 kHz — and under the default crossovers two of those three fall outside each
  band. Outside the band a node does nothing, which is why it never mattered; a
  150 Hz low shelf on a band starting at 600 Hz is flat across everything the
  band carries. Clamped to 600 Hz it lands at full strength on the band's own
  content, measured here at 1.16 dB at 900 Hz for a −6 dB shelf. 34 of the 36
  factory presets set a tone gain, so 34 of them quietly changed voicing.

  A node outside its band is something the interface should discourage, not
  something the engine should rewrite underneath a saved file. The mouse is
  still clamped; automation is not.

### Known

- The eight animated GIFs the tutorial specification asks for are not built.
  The screenshot harness renders stills and would need a frame-sequence mode.
- CPU at six bands and 4× is 4.25 % against a 3 % target; met at three bands.
- Automation and modulation can still place a tone node outside its band, where
  it has no audible effect. See Withdrawn above for why this is deliberate.

## [2.1.0] — 2026-09-22

In-plugin help. Nothing about the audio changes.

### Added

- **Eight guided tours**, 44 steps: quick start, multiband basics, saturation
  styles, feedback and dynamics, the tone EQ, modulation, mixing recipes, and
  HQ/CPU. A step dims the interface around the control it is describing and
  waits for you to use it; every active step carries a "Do it for me" link so
  nobody can be stuck.
- **A Learn panel** with four tabs — Tours with a completion ring each, a
  searchable Reference, every modifier in one Shortcuts table, and About. It
  takes width from the band strip rather than covering the controls it
  describes.
- **53 reference articles** covering all 209 parameters, keyed by kind so
  `band3Drive` and `band5Drive` share one article with a different number in
  it. Tests assert every parameter resolves to an article, that no article is
  too long to read in a 360 px panel, and that every "Show me" link names a
  tour that exists.
- **A first-run welcome** offering the two-minute tour. Skip leaves it for next
  time; only "Don't show again" is permanent.
- **`scripts/gen-docs.py`**, which writes the manual's parameter reference from
  the same file the plugin reads, with a CI check that fails if they drift.

### Fixed

- `gainReductionDb` was a plain float written on the audio thread and read by
  the GUI meter — a data race the project's own rules forbid, which
  ThreadSanitizer had not caught because no test drove the meter and the audio
  thread at once.
- A band's controls stayed on screen after the band count was reduced past it:
  the visibility test read the band *count*, which is never zero, instead of
  whether that band was active.
- Band control sets were created on first selection, so an unselected band's
  controls did not exist to be pointed at. All active bands are built during
  layout now.

### Known

- `dither` and the XY pad's Y axis are saved and restored but currently do
  nothing: `ditherMode` is read by no code, and the XY source only ever emits
  its X axis. Both are documented as inert rather than described as working.
- Crossovers can be placed closer by automation than by dragging: the GUI
  enforces a third of an octave, the engine 1/35th.
- Tone node frequencies are clamped to their band by the mouse only; automation
  and modulation can push them outside it.

## [2.0.0] — 2026-09-22

A complete rebuild of the interface, and the tone stage becomes an EQ you can
grab. The saturation, crossovers, modulation engine and presets are unchanged:
every factory preset written for 1.x loads into 2.0 with an identical response.

### Added

#### The interface

- **Heat.** Each band measures how much of its output is no longer a scaled
  copy of its input - a normalised distortion residual - and that drives the
  spectrum tint, the module chrome, the knob glow and the logo filament. A
  level ratio cannot show this: Ember gain-matches every style to within
  0.1 dB, so a screaming band and a clean one measure the same loudness.
- **Band strip.** Every active band is on screen as its own module, the
  selected one 1.6x wide with the full control set, the rest showing Drive, the
  style name, a heat bar and a sparkline of their tone curve.
- **Embedded type.** Inter and Barlow Condensed ship with the plug-in, so it
  renders identically on every platform. Values use tabular figures and stop
  changing width as they count.
- **Footer** carrying input, output, mix, auto-gain, CPU, latency and a hint
  line. Tooltip popups are gone: on a surface this dense they spend their life
  covering the control beside the one being described.
- **Zoom** at 75-200 %, a **Cool Ember** accent variant, and **Reduce motion**.
- **One motion clock.** A single VBlank attachment drives every animation and
  meter rather than six timers at unrelated rates beating against each other.

#### The EQ

- **Per-band node editor** replacing the three anonymous tone knobs. Drag for
  gain and frequency, Alt-drag or scroll the mid node for Q, double-click to
  reset. The curve fades outside the band's own range, because that is the only
  range it affects.
- **Pre/post switch** per band: tone before the saturator changes which
  harmonics exist, after it shapes what came out.
- **Overlay on the main display** with every band's curve and the combined
  response weighted by band level, and the same nodes draggable there.
- **Analyser settings** - resolution, averaging, tilt, freeze, peak hold -
  stored in the session.
- **Crossover handling**: Alt snaps to musical points, Cmd links all edges
  proportionally, right-click adds a band at the pointer, distributes evenly or
  resets, and a note name follows the mouse.

### Changed

- Band regions lose their per-band hues. A band's colour is its heat now, and
  the old ramp's blue-white top band would read as modulation under the rule
  that warm is audio and cool is control.

- Realtime CPU, full plugin with six modulation routings, on the host machine:
  **6.57 % → 4.25 %** of one core at 6 bands / 4× / 48 kHz, and
  **3.24 % → 2.15 %** at 3 bands, so the specification's 3 % target is now met
  at three bands. 16× went 23.6 % → 15.8 % and 96 kHz 15.4 % → 9.9 %. Both
  figures are medians of five alternating runs of the two binaries.

  No audio changed. Every change is the same arithmetic in a different loop:
  per-sample recursions that ran one channel to completion before starting the
  other now run both in the same iteration, which covers two dependency chains
  in the time of one; the tone stack, the crossover filters, the dry delays and
  the oversampler's latency compensator are written out in place rather than
  called out of line into JUCE's separately compiled filter classes; and the
  oversampler's two polyphase cascades and two channels are now four lanes of
  one SIMD register. A new test runs that oversampler against
  `juce::dsp::Oversampling` and requires the two to agree exactly.

  The aliasing, flatness, gain-match, stability and smoothing gates all report
  the same numbers afterwards as before, to the digit. See `docs/STATUS.md` for
  the full breakdown, including what was tried and did not pay.

### Fixed

- `docs/STATUS.md` quoted stale aliasing figures (−91.2 dB for hard clip at 16×
  rather than the −88.7 dB the code actually measures, and −84 rather than −78
  for Foldback). Re-measuring the unmodified code produced the corrected
  figures exactly, so this is a documentation drift rather than a regression.

## [1.0.1] — 2026-09-16

### Fixed

- The standalone application now ships on all three platforms. The Linux
  package never contained one — the release job built only the VST3 target —
  and both zip archives, which are the no-installer route, carried plug-in
  bundles only. Only the Windows and macOS installers had it. The Linux
  tarball now includes the `Ember` binary and `install.sh` places it on PATH
  (`~/.local/bin`, or `/usr/local/bin` with `--system`), with `uninstall.sh`
  removing it again; the Windows zip includes `Ember.exe` and the macOS zip
  includes `Ember.app`. The staging steps now fail the build if a standalone
  is missing, so this cannot silently regress.

### Changed

- `release.yml` rejects a non-numeric version up front. Inno Setup accepts
  only a numeric `VersionInfoVersion` and does not report otherwise until the
  installer compiles, three platform builds into the run.

No audio-processing code changed in this release; the DSP, GUI and presets are
identical to 1.0.0.

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
- Per-style gain matching measured at `prepare()` by running a deterministic flat-noise
  burst at −18 dBFS through each shaper at 1 dB drive steps, so switching styles or raising
  drive does not jump the level. The reference is flat rather than pink because several
  styles roll off above a few kHz, where pink noise carries almost no energy. The table is
  computed once per sample rate and shared across bands; the residual after compensation is
  0.09 dB worst case across all 19 styles.
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

### Measured at release

All figures from the host machine (Apple M2, macOS, Apple clang 21, JUCE 8.0.15):

- Crossover band sum is magnitude-flat to better than **0.01 dB** for every band count from
  1 to 6; the linear-phase sum nulls against the delayed input at **−150.9 dB**.
- Per-style gain match at 0 dB drive: **0.09 dB** worst case across 19 styles.
- Aliasing, 10 kHz at 44.1 kHz through Hard Clip at 16× oversampling: **−91.2 dB** relative
  to the fundamental. Foldback −84 dB, Rectify −92 dB, Smudge −95 dB.
- Feedback loop bounded and finite through 60 s of full-scale noise at every combination of
  amount and frequency, and through 20 s of the worst full-chain settings.
- CPU, full plugin with six modulation routings: **6.6 %** of one core at 6 bands / 4× /
  48 kHz, **3.2 %** at 3 bands. The specification's 3 % target is met at three bands and
  missed at six; see `docs/STATUS.md` for what was tried and what closing it would take.
- 35 unit tests and 41,281 assertions pass, and pass again under
  `-fsanitize=address,undefined`.
- `pluginval --strictness-level 10` passes on VST3 and Audio Unit, including the editor,
  editor automation and parameter thread-safety groups. `auval` is clean.
