# Ember — Manual Test Checklist

Automated tests (`ctest`) and pluginval cover what can be checked headlessly. This checklist
covers what cannot: real hosts, real audio devices, real displays, and a human listening.

**Work through all of it before tagging a release.** Copy this file into the release issue,
tick the boxes as you go, and record the build you tested at the top. An unticked box is not a
pass; if something fails, file it and note the item number rather than silently moving on.

| | |
|---|---|
| Ember version | |
| Build / commit | |
| OS and version | |
| Format tested | VST3 / AU / Standalone / AUv3 |
| Tester and date | |

Run the whole checklist once per **format** and once per **operating system** you intend to
ship. The AU-only and macOS-only items are marked.

Before you start, build the offline renders — several sections refer to them:

```sh
cmake --build --preset macos-release        # or your platform's preset
./build/macos-release/tests/ember_render test-renders
```

---

## 1. Loading

### 1.1 In a host

- [ ] The plugin appears in the host's plugin list after a rescan, under the name **Ember**,
      vendor **EmberAudio**, category **Distortion / Fx**.
- [ ] It instantiates on a stereo audio track without an error dialog or a hang.
- [ ] Audio passes through immediately at default settings and sounds unprocessed.
- [ ] The editor opens, closes, and reopens without crashing or leaking window state.
- [ ] Removing the plugin from the track and re-adding it works; no zombie audio, no stuck
      GUI window.
- [ ] Loading two instances on two tracks at once works, and they hold independent state.
- [ ] Closing the project with the editor open does not crash.
- [ ] Repeat in at least two hosts. Recommended coverage: **macOS** Logic Pro (AU) +
      Reaper or Ableton Live (VST3); **Windows** Reaper + Cubase or Studio One;
      **Linux** Reaper or Ardour or Bitwig.
- [ ] *(macOS, AU)* `auval -v aumf Emb1 Embr` passes with no errors or warnings.

### 1.2 Standalone

- [ ] The Standalone app launches, and the audio device settings dialog opens.
- [ ] Selecting an input and output device produces audio through the plugin.
- [ ] Changing the audio device, sample rate and buffer size from inside the settings dialog
      does not crash, glitch permanently, or leave silence behind.
- [ ] A MIDI input device can be selected and MIDI modulation sources respond to it.
- [ ] Settings persist across a quit and relaunch.

---

## 2. Parameter automation

Test **one parameter from each class** thoroughly, then spot-check the rest. For each: write
an automation lane in the host, play it back, and confirm the GUI follows, the audio follows,
and the host's displayed value and unit are correct.

### 2.1 Continuous parameters

Drive, band mix, level, pan, width, feedback amount, feedback frequency, dynamics, tone low /
mid / high, crossover frequencies, input gain, output gain, global mix, macros 1–8, XY X and Y.

- [ ] A slow ramp across the full range is smooth: no zipper noise, no stepping, no clicks.
- [ ] A **fast** jump (an instant step in the automation lane, or a hard snap from minimum to
      maximum) does not click or pop.
- [ ] The value shown by the host matches the value shown in Ember's GUI.
- [ ] Units and ranges read correctly in the host (dB for drive and gains, Hz for crossover
      and feedback frequency, % for mix and width).
- [ ] Touching a control in the GUI writes automation in the host when the lane is armed
      (begin/end gesture is reported correctly — the host should show one contiguous
      write, not a stream of single points).

### 2.2 Choice parameters

Style (per band), oversampling factor, offline oversampling factor, crossover mode,
stereo mode, dither mode, LFO sync, envelope-generator trigger, MIDI source type,
envelope-follower band.

- [ ] Every choice is selectable from host automation, and the host's list of choice names
      matches the GUI's.
- [ ] Automating **style** across all 19 values while audio plays produces no clicks — each
      change crossfades.
- [ ] Automating **oversampling** while audio plays does not click, and the host's delay
      compensation follows (see section 9).
- [ ] Switching **crossover mode** between minimum phase and linear phase while audio plays
      does not click or drop out.

### 2.3 Stepped / integer parameters

- [ ] **Band count** automates across 1–6 and the host shows integer steps, not a continuous
      sweep (see section 6 for the audio behaviour).

### 2.4 Boolean parameters

Per-band bypass, per-band solo, auto-gain.

- [ ] Each toggles from host automation and from the GUI, and the two stay in sync.
- [ ] Toggling during playback does not click (see section 5).

### 2.5 Automation plus modulation on the same parameter

- [ ] Automate a parameter that also has a modulation connection attached. The automation sets
      the **base** value and the modulation offsets it; neither fights the other, and the GUI
      shows both the base position and the modulated position.
- [ ] Releasing the automation lane leaves the base where the host put it, not where the
      modulation last pushed it.

---

## 3. Preset and state save / recall

- [ ] Build a non-default patch: 5 bands, a different style per band, moved crossovers,
      non-zero feedback and dynamics, linear-phase mode on, 8× oversampling, mid/side on,
      and **at least 20 modulation connections** across several source types with mixed
      depths and curves.
- [ ] Save it as a user preset. Load a factory preset, then reload yours: everything comes
      back, including every modulation connection, its depth, and its curve.
- [ ] Save the host project, close the project, reopen it: the plugin state is identical.
      Check the modulation matrix connection-by-connection, not just by ear.
- [ ] Close the host entirely, reopen, load the project: still identical.
- [ ] Copy the plugin state from one instance and paste it into another (host's copy/paste, or
      save/load a preset file): the target instance matches the source.
- [ ] Load a project saved with an **older** Ember build, if one exists: it loads without
      error and parameters land where they did before. Styles in particular must not shift —
      the style enum is append-only.
- [ ] Undo and redo across parameter changes, style changes, band-count changes and
      modulation edits all behave, and the A/B compare slots hold two independent states.
- [ ] Drag a preset's worth of changes, then hit A/B: the two states swap cleanly with no
      audio dropout.

---

## 4. GUI: resizing and display density

- [ ] Drag the window corner through the full size range. Layout reflows; nothing overlaps,
      clips, or disappears; text stays legible at the smallest size.
- [ ] The size is remembered when the editor is closed and reopened, and when the project is
      saved and reloaded.
- [ ] The spectrum display keeps up while resizing — no stutter, no stale frames, no runaway
      CPU during the drag.
- [ ] Drag the plugin window between a **Retina/HiDPI display and a standard-DPI display**.
      The GUI re-renders sharply on both; it does not stay blurry or double-sized.
- [ ] *(Windows)* Test at 100%, 150% and 200% display scaling, and with two monitors at
      different scalings. Drag the window between them.
- [ ] *(macOS)* Test on a Retina display and an external 1× display.
- [ ] *(Linux)* Test with a non-1.0 scale factor if your desktop supports it.
- [ ] Host-driven resize (hosts that scale plugin windows themselves, e.g. Studio One,
      Cubase) produces a correctly-scaled GUI, not a cropped one.
- [ ] Mouse hit-testing is correct after every resize and rescale: clicking a control's
      visible position actually grabs that control.

---

## 5. Per-band bypass and solo

With a multiband patch and a full-range source playing:

- [ ] Bypassing a band removes that band's processing while its audio still passes through at
      unity — the band is bypassed, not muted.
- [ ] Toggling bypass during playback is click-free (it crossfades).
- [ ] Soloing a band leaves only that band audible.
- [ ] Multiple simultaneous solos sum the soloed bands only.
- [ ] Clearing all solos restores the full mix at exactly the level it had before.
- [ ] Toggling solo during playback is click-free.
- [ ] Bypass and solo interact sanely: soloing a bypassed band plays that band's dry content.
- [ ] The global/host bypass produces a true bypass — a null against the dry signal, once
      latency is accounted for.

---

## 6. Band count changes during playback

With audio playing continuously throughout:

- [ ] Step the band count 1 → 2 → 3 → 4 → 5 → 6 and back down. No clicks, pops, dropouts or
      bursts of noise at any step.
- [ ] Do the same rapidly, several changes per second, for 30 seconds. Still clean; no
      runaway CPU; no crash.
- [ ] Drag a crossover frequency past a neighbouring crossover: the crossovers reorder or
      clamp without producing a click or an unstable filter.
- [ ] Change band count while a modulation connection targets a parameter on a band that is
      being removed: no crash, no stuck modulation, and the connection reappears intact when
      the band returns.
- [ ] Change band count while linear-phase mode is on: no crash, and the reported latency
      updates (see section 9).

---

## 7. Sample rate and buffer size

### 7.1 Sample rate

- [ ] Instantiate and play at each of **44.1, 48, 88.2, 96, 176.4 and 192 kHz**. The plugin
      sounds the same at each — crossover points, feedback frequency and tone bands land on
      the same frequencies, and drive produces the same character.
- [ ] Change the sample rate **mid-session** in the host or the standalone audio settings
      while audio is playing. No crash, no stuck state, no permanent silence; audio recovers
      within a moment.
- [ ] Reported latency changes with the sample rate as expected (section 9).
- [ ] At 192 kHz with 16× oversampling, the plugin still runs without dropouts on the target
      machine, or degrades gracefully — check it is not silently producing garbage.

### 7.2 Buffer size

- [ ] Play at 32, 64, 128, 256, 512, 1024 and 2048 samples. Identical audio at every size.
- [ ] **1-sample buffers.** Set the host or the standalone device to a 1-sample buffer if it
      allows it; otherwise use pluginval, which feeds 1-sample blocks at strictness 10. No
      crash, no denormal stall, and the output matches a large-buffer render.
- [ ] **Variable / non-uniform buffers.** Hosts do not always hand you `maximumExpectedSamples`.
      Verify with pluginval (it deliberately varies the block size) and, if your host offers
      it, with a track that has automation dense enough to split blocks. Output must be
      sample-identical to a fixed-buffer render of the same material.
- [ ] Change the buffer size mid-session while audio plays: no crash, no lasting glitch.
- [ ] The control-rate modulation (32-sample blocks) produces identical results regardless of
      how the host splits the buffer — render the same modulated passage at 64 and at 1024
      samples and null them against each other.

---

## 8. Channel configurations

- [ ] **Stereo in / stereo out**: the default. Stereo image is preserved at unity settings.
- [ ] **Mono in / mono out**: the plugin loads on a mono track and processes correctly. Per-band
      pan and width controls do not produce a broken or silent output.
- [ ] **Mono in / stereo out**, if the host offers it: both output channels carry the signal.
- [ ] A mono source fed to the stereo plugin stays centred and mono at unity — no phase or
      level difference between the channels.
- [ ] **Mid/side mode**: with a mono (side-free) source, the side band carries nothing and the
      output stays mono. With a wide source, M/S processing widens or narrows as expected and
      an M/S round trip at unity is transparent.
- [ ] Unsupported layouts (e.g. 5.1) are cleanly refused by the host rather than loaded and
      broken.

---

## 9. Latency reporting — oversampling and linear phase

Ember adds latency from FIR oversampling filters and from linear-phase crossovers, and reports
it to the host. This section proves the reported number is *exactly* right; if it is wrong,
everything the user records through Ember drifts out of time.

### 9.1 The null test

This is the definitive check. Do it in a DAW with delay compensation enabled:

1. Put your source material on **track A**.
2. Duplicate it to **track B**, identical audio, same start position.
3. Insert Ember on track B. Set **every band to 0 dB drive, 100% wet mix, unity level**, no
   feedback, no dynamics, no tone — Ember at unity is transparent by design.
4. **Invert the polarity** of track B (a polarity/phase-flip plugin, or the host's own
   polarity button).
5. Solo both tracks and play.

**Pass criteria: the sum is silence.** Measure it, do not just listen — put a meter on the
master and confirm the residual is below **−100 dBFS**. Anything audible means either the
latency is reported wrongly or the unity path is not transparent.

Repeat the null for each of these, re-checking that the residual stays below −100 dBFS:

- [ ] Oversampling **off**
- [ ] Oversampling **2×**
- [ ] Oversampling **4×**
- [ ] Oversampling **8×**
- [ ] Oversampling **16×**
- [ ] **Linear-phase** crossover mode, at 1 band
- [ ] **Linear-phase** crossover mode, at 6 bands
- [ ] Linear phase **and** 16× oversampling together
- [ ] Each of the above at 44.1 kHz and again at 96 kHz

### 9.2 Measuring the latency directly

The null test proves the host compensated correctly. This proves the number itself:

- [ ] Render a single **impulse** (one sample at 1.0, silence around it) through Ember with
      the host's delay compensation **disabled**, and measure how far the output peak has
      moved in samples.
- [ ] That offset must equal the latency the host reports for the plugin (Reaper shows it in
      the FX window's title bar or the performance meter; most hosts expose it somewhere in
      the plugin or track info).
- [ ] The reported latency must be **0** when oversampling is off and the crossovers are in
      minimum-phase mode.

### 9.3 Latency changes mid-session

- [ ] Change the oversampling factor while the transport is **stopped**: the host picks up the
      new latency, and a fresh null test still nulls.
- [ ] Change the oversampling factor while the transport is **rolling**: no crash and no
      lasting glitch. Some hosts will not re-query latency until you restart the transport —
      note which of your test hosts do this; it is host behaviour, not a bug, but it should be
      documented.
- [ ] Switch crossover mode while rolling: same expectation.
- [ ] Change band count in linear-phase mode: the reported latency updates and a fresh null
      still nulls.

---

## 10. CPU and stability

- [ ] Compare a live host reading against the offline numbers from
      `./build/<preset>/tests/ember_benchmark`. The host should be in the same
      neighbourhood; a host figure several times worse points at something in the plugin
      wrapper or the GUI, not the DSP.
- [ ] **Worst case:** 6 bands, 16× oversampling, linear phase on, feedback and dynamics active
      on every band, all modulation sources running, 50+ connections, at 96 kHz with a
      64-sample buffer. Note the CPU figure. It should be survivable on the target machine and
      must not produce dropouts at a realistic buffer size.
- [ ] **Idle cost:** with the editor closed, CPU with the plugin at default settings is close
      to a pass-through. Opening the editor adds a visible but modest amount (the spectrum
      analyser); it must not double the total.
- [ ] **Soak test:** leave the worst-case patch running on a loop for **30 minutes**. CPU does
      not creep upward, memory does not grow, and the audio does not degrade, click, or go
      silent.
- [ ] **Feedback stability:** set feedback to maximum on every band with a high-Q feedback
      frequency and feed it loud material for 60 seconds. The output stays bounded; it never
      runs away, never produces NaN/Inf silence, and recovers when the input stops.
- [ ] **No dropouts on automation storms:** automate a dozen parameters simultaneously at high
      density and confirm no buffer underruns.
- [ ] Run the plugin under the sanitizer build (`linux-asan` preset) through the test suite and
      a headless render; no ASan or UBSan reports.

---

## 11. Offline renders (`test-renders/`)

`ember_render` pushes fixed, deterministic test signals through the engine and writes them as
24-bit WAVs, along with a peak and RMS table printed to the console. They exist so this
checklist can be completed against real audio — with a spectrum analyser and a waveform view,
not just by ear — and so that a change in character between releases is visible rather than
remembered.

Build and run it from the repository root:

```sh
cmake --build --preset macos-release
./build/macos-release/tests/ember_render test-renders
```

With no argument it writes to `test-renders/` relative to the current directory. The folder is
git-ignored; regenerate it whenever the DSP changes and compare against the previous run.

### What each render is, and what to listen for

**`00-source-*.wav` — the unprocessed inputs.** A 20 Hz → 20 kHz log sweep, a set of
drum-like impulses (65 Hz body plus a 1.8 kHz snap, every 250 ms), and white noise. Always
compare a processed render against its matching source, not against another render.

- [ ] The three source files exist and are clean: full-scale-safe, no clipping, no DC.

**`01-unity-*.wav` — transparency.** 4 bands, 2× oversampling, 0 dB drive, 100% mix. This is
the crossover network doing nothing.

- [ ] Null `01-unity-sweep.wav` against `00-source-sweep.wav`. Residual below **−100 dBFS**.
- [ ] Look at the sweep's spectrum: flat. No dips or bumps at the crossover frequencies —
      those would mean the all-pass compensation is wrong.
- [ ] `01-unity-noise.wav` sounds identical to the source noise. No coloration, no phasiness.

**`02-style-NN-*.wav` — one render per saturation style.** 19 files, single band, 4×
oversampling, 18 dB drive, on the drum source.

- [ ] All 19 files are present and none is silent, clipped to a square, or full of NaNs.
- [ ] **Level consistency:** the printed RMS figures sit within a few dB of each other across
      all 19 styles. A style that is 10 dB louder or quieter than its neighbours means the
      gain matching failed for it.
- [ ] **Character:** each style sounds like its name. Tube styles add even harmonics and
      soften transients; tape styles compress and roll off the top; the transformer adds low
      thickening; amp styles get progressively more aggressive from clean to lead; rectify and
      foldback are obviously harsher; decimate and bitcrush are obviously digital. Two styles
      should not be indistinguishable.
- [ ] **Aliasing:** run each through a spectrum analyser on the sweep, or listen to the sweep
      versions. As the fundamental rises there should be no descending tones — that is
      aliasing folding back. The hard-edged styles (hard clip, foldback, rectify) are the ones
      to scrutinise; they rely on antiderivative anti-aliasing.
- [ ] **Transients:** the drum hits keep their attack. A style that smears the 1.8 kHz snap
      into a click or a blur has a filter or an ADAA problem.

**`03-feedback-*.wav` — the resonant feedback loop** at three amounts (30%, 60%, 90%),
3 bands, warm tube at 20 dB drive, feedback tuned to 320 Hz.

- [ ] Resonance increases with the amount and is centred on 320 Hz.
- [ ] Even at the highest setting the output stays bounded — check the peak figures — and does
      not build into a runaway tone or a screech that never decays.
- [ ] No digital clipping artefacts or wrap-around at the peaks.

**`04-dynamics-*.wav` — bipolar dynamics** at −100%, −50%, +50% and +100%, 3 bands, clean tape
at 10 dB drive, on drums.

- [ ] Negative values **expand**: the drum hits get punchier, the tails get quieter, the
      dynamic range widens.
- [ ] Positive values **compress**: the tails come up, the peaks come down, the RMS rises
      relative to the peak.
- [ ] The effect scales smoothly between the settings — no discontinuity in character between
      −50% and +50% through the centre.
- [ ] No pumping artefacts or envelope chatter on the fast transients.

**`05-midside-autogain.wav` — mid/side mode with auto-gain on.** 3 bands, transformer at
24 dB drive.

- [ ] The stereo image is intact: the centre has not collapsed and the sides have not
      inverted. Check on a correlation meter — correlation should not go negative.
- [ ] Auto-gain has brought the output RMS back near the source RMS despite the 24 dB of
      drive. Compare the printed RMS against the drum source's.
- [ ] Mono-compatibility: sum the render to mono. Nothing important disappears.

### Comparing against the previous release

- [ ] Keep the previous release's `test-renders/` folder. Null the new renders against the old
      ones file by file. Anything that changed should be explainable by a deliberate change in
      this release — if a style moved and nobody meant to move it, that is a regression, and
      it will change how every existing user's saved project sounds.

---

## 12. Sign-off

- [ ] Every box above is ticked, or has a filed issue linked next to it.
- [ ] `ctest` is green on every platform in the release matrix.
- [ ] `./scripts/run-pluginval.sh` passes at strictness 10 on every format.
- [ ] *(macOS)* `auval` is clean.
- [ ] `CHANGELOG.md` describes this release accurately.
- [ ] The installers from `packaging/` have been installed on a **clean machine** — not a
      development machine — and the plugin loads in a host there.
