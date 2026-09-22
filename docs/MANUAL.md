# Ember — User Manual

Ember is a multiband saturation and distortion plugin. It splits your signal into
up to six frequency bands, runs each band through its own saturation stage with
its own character and amount, and puts the result back together. Everything that
can be modulated, can be modulated by anything.

---

## 1. The quick version

1. Pick a preset near what you want.
2. Drag the band edges on the spectrum display to decide *where* the distortion
   happens.
3. Click a band to select it, then set its **Style** and **Drive**.
4. Use **Mix** per band, or the global **Mix**, to blend back toward clean.
5. Turn on **Auto-Gain** so louder does not fool you into thinking better.

Ember gain-matches every style at 0 dB drive, so switching styles compares
character rather than level.

---

## 2. Signal flow

```
Input Gain
  └─ [Mid/Side encode, optional]
      └─ Crossover (1–6 bands)
          └─ for each band:
               Drive → Saturation Style (oversampled) → Feedback
                     → Dynamics → Tone → Level / Pan / Width → Mix
          └─ Band sum
      └─ [Mid/Side decode]
  └─ Auto-Gain → Global Mix → Output Gain
```

---

## 3. Bands and crossovers

- **Band count** — 1 to 6. Changing it while audio is playing is click-free:
  Ember crossfades the old and new band splits, and because a crossover sums
  back to the original signal, the total never jumps.
- **Crossover frequencies** — drag the vertical dividers on the spectrum
  display, 20 Hz to 20 kHz, with a minimum spacing of about a third of an
  octave so bands cannot collapse into each other.
- **Crossover type** —
  - *Minimum Phase* (default): Linkwitz-Riley 4th order. Zero latency. The bands
    sum flat in magnitude.
  - *Linear Phase*: constant group delay, no phase smear between bands, at the
    cost of latency and a little pre-ringing. Ember reports the latency to your
    host, so recorded tracks stay aligned. Prefer it on a mix bus; avoid it when
    tracking.

Per band you also get **Bypass** and **Solo**. Solo is for finding the right
crossover points — a band that sounds harsh soloed is not necessarily wrong in
context.

---

## 4. Saturation styles

Drive is the only character control. Each style interprets it differently, so
drive does not simply mean "more" — it means "further into this circuit".

| Group | Styles | Character |
|---|---|---|
| Tube | Clean Tube, Warm Tube, Subtle Tube, Broken Tube | Asymmetric, even-harmonic warmth. *Broken Tube* drifts its bias and adds crossover distortion — deliberately unwell. |
| Tape | Clean Tape, Warm Tape, Bright Tape | Symmetric, compressed, top end softening as you push. *Bright Tape* pre-emphasises so highs saturate first. |
| Transformer | Transformer | Low-frequency saturation with odd harmonics and a bass bloom. |
| Amp | Clean Amp, Crunch, Lead | Cascaded gain stages with tone shaping between them. More stages, more compression and sustain. |
| Rectify | Smudge, Rectify | Half- and full-wave rectification blended with the dry band. Rectify reads as an octave-up snarl. |
| Destroy | Foldback, Hard Clip, Decimate | Wavefolding, hard clipping, and sample-rate reduction. |
| Crush | Bitcrush | Bit-depth reduction with dither. |
| FX | Shimmer, Breathe | Ring-modulation sparkle, and a filter that moves with the performance. |

**Drive** runs 0–40 dB. Ember measures each style's output level across the whole
drive range and compensates, so a drive sweep changes character far more than it
changes loudness.

---

## 5. Per-band controls

- **Drive** (0–40 dB) — how hard the band hits its saturation stage.
- **Mix** (0–100 %) — parallel blend for this band only. The single most useful
  control for keeping distortion from eating the source.
- **Level** (−24…+24 dB), **Pan** (−100…+100), **Width** (0–200 %).
- **Feedback** (0–100 %) and **Feedback Frequency** (20 Hz–2 kHz) — a short
  resonant path around the saturation stage. This is the howl and the ring. It
  is internally limited, so it cannot run away no matter how you set it.
- **Dynamics** (−100…+100 %) — one knob, two behaviours. Positive compresses with
  a program-dependent attack and release. Negative expands and gates. Zero is a
  true bypass.
### Tone — the band's EQ

The three tone controls are a curve you drag, not three knobs. The panel inside
the selected module draws the band's response over a ±12 dB grid with three
nodes on it:

| Gesture | Effect |
|---|---|
| Drag a node up / down | Gain, ±12 dB |
| Drag a node left / right | Frequency, within the band's own range |
| Alt-drag or scroll the mid node | Q, 0.2 to 6 |
| Double-click a node | Reset that node |
| **PRE / POST** | Whether tone runs before or after the saturator |
| **FLAT** | Zero the three gains, leaving the node positions alone |
| Lamp, top right | Tone bypass — lit means the stage is off |

The curve fades outside the band's own frequency range, because that is the
only range it affects, and the nodes are clamped to it for the same reason.

**Pre versus post matters more than it sounds.** Post-EQ shapes what came out
of the saturator. Pre-EQ changes what goes in, and therefore which harmonics
the saturator generates at all — boosting 3 kHz before a tube stage is a
different instrument from boosting it afterwards.

Unselected modules show a small sparkline of the same curve, so the strip tells
you at a glance which bands have shaping on them.

---

## 6. Global controls

- **Input** / **Output Gain** (±24 dB).
- **Mix** — global dry/wet. The dry path is delay-compensated, so blending never
  causes comb filtering.
- **Auto-Gain** — continuously RMS-matches the wet path to the dry path so that
  A/B comparisons are about tone, not level.
- **Oversampling** — Off / 2× / 4× / 8× / 16×, with a separate setting for
  offline rendering. Higher factors reduce aliasing from the hard-edged styles.
  Hard Clip, Foldback and the rectifiers also use antiderivative anti-aliasing,
  so they stay clean at lower factors than you might expect.
- **Stereo mode** — Stereo or Mid/Side. In Mid/Side, the whole multiband chain
  operates on mid and side rather than left and right.
- **A/B** — two full snapshots of every setting. **Copy** pushes the current one
  into the other slot so you can diverge from a common starting point.
- **Undo / Redo** — covers parameter changes and modulation edits.

---

## 7. Modulation

Any source can modulate any continuous parameter, including another source's
parameters.

**Sources**
- **XLFO 1–4** — multi-point shape editor. Free-running in Hz, or synced to host
  tempo, with phase offset and optional step quantisation.
- **Envelope Generator 1–2** — ADSR, triggered by an input transient or a MIDI note.
- **Envelope Follower 1–4** — attack/release detector, pointed at the full range
  or at one specific band.
- **XY Controller** — two hands on two axes.
- **MIDI 1–4** — velocity, CC, mod wheel, or note number.
- **Macro 1–8** — one knob driving as many destinations as you like.

**Making a connection**: drag a source's handle onto any knob. A ring appears
around the knob showing the range the modulation will sweep. Per connection you
get a bipolar **amount**, a response **curve**, and a **smoothing** time. Up to
64 connections are available at once.

Modulation is computed every 32 samples and interpolated between, so fast
modulation stays smooth rather than stepping. The full graph is saved with the
plugin state and travels with presets.

---

## 8. Presets

The preset browser has folders, search, and save / rename / delete. A dot next
to the name means you have changed something since loading it. User presets live
in `Documents/EmberAudio/Ember/Presets` and are plain XML — easy to back up,
share, or version-control.

---

## 9. The display

The main display shows the analyser, the EQ, or both — the selector sits in its
top-right corner.

| Gesture | Effect |
|---|---|
| Drag a crossover handle | Move that band edge |
| **Alt**-drag a handle | Snap to musical points: 60, 120, 250, 500 Hz, 1, 2, 4, 8 kHz |
| **Cmd/Ctrl**-drag a handle | Move every crossover by the same interval |
| **Shift**-drag a handle | Fine control |
| Drag an EQ node | The same node as in the module, mirrored live |
| Hover an EQ node | Lights the band module it belongs to |
| Click a band region | Select that band |
| Right-click | Add a band here, distribute evenly, reset crossovers |

A note name and frequency follow the pointer along the bottom — `A2 · 110 Hz` —
because deciding where a crossover goes is usually a musical question.

The **combined response** is the thicker line: every band's tone curve weighted
by how much signal that band is actually carrying. A band with nothing in it
does not bend the line, however its curve is set.

The gear opens the analyser's own settings: resolution, averaging, tilt, freeze
and peak hold. They are stored with the session but are not automatable — they
change what you see, not what you hear.

### Heat

The interface warms up with the plugin. Each band measures how much of its
output is no longer a scaled copy of its input, and that drives the band's tint
on the display, its module's title bar, the glow behind its Drive knob and the
filament in the logo.

It measures **distortion, not loudness**. Ember gain-matches its styles, so a
band being hammered and a band sitting clean read the same on a meter; heat is
what tells them apart.

---

## 10. Keyboard and mouse

| Gesture | Effect |
|---|---|
| Double-click any control | Reset to default |
| **Shift**-drag | Fine control |
| Scroll over a control | Step its value |
| Right-click a control | MIDI learn, remove modulation |
| Drag a modulation source onto a control | Create a routing |
| **Alt**-drag a modulated control | Edit the modulation amount |

Modulation is drawn in cool blue everywhere, and audio in warm orange. On a
control that is moving, the colour tells you whether you moved it or something
else did.

---

## 11. Practical notes

**Latency.** Oversampling and linear-phase mode both add latency, and Ember
reports it so your host compensates. If you are monitoring through Ember while
tracking, use Off or 2× oversampling and minimum-phase crossovers.

**CPU.** Cost scales with band count and oversampling factor. 16× on six bands is
for offline rendering, not for thirty instances in a live session. Set the
offline oversampling factor high and the realtime one low — Ember switches
automatically when your host renders.

**Where to start.** On a mix bus, one or two bands, Subtle Tube or Clean Tape,
drive under 10 dB, mix around 30 %, auto-gain on. On drums, three bands and a
much more aggressive low band. On bass, keep the lowest band nearly clean and
distort the band above it — that is how you get grit that survives a small
speaker without losing weight.

**Verifying transparency.** With all drives at zero and mix at 100 %, Ember is
magnitude-flat. If you want to confirm it in your own session, null it against a
delay-compensated dry copy.
