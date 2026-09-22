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

---

## Parameter reference

<!-- BEGIN GENERATED PARAMETER REFERENCE -->

<!-- Generated by scripts/gen-docs.py from resources/help/reference.md.
     Edit that file, not this section. -->

### Global

| Control | Range | What it does |
|---|---|---|
| **Input Gain** | −24 to +24 dB | Level into the whole chain, before mid/side encoding and the crossover. It therefore changes how hard every band hits its saturator, on top of each band's own Drive. without touching six Drive knobs. Leave it at 0 and use Drive if you want one band pushed and the others clean. |
| **Output Gain** | −24 to +24 dB | The last gain in the chain, after global Mix and auto-gain. It is a clean gain stage: nothing downstream of it distorts. expects. Use it rather than Input Gain when you only want the level changed, not the amount of saturation. |
| **Mix** | 0 to 100 % | Global dry/wet against a delay-compensated dry tap, so blending never combs however much oversampling or linear-phase latency is in the chain. is the finer tool — reach for this to pull the whole effect back at once. |
| **Auto Gain** | Off or On | Continuously RMS-matches the wet path back to the dry over a ~0.3 s window, clamped to ±12 dB. Ember already gain-matches each style across the whole drive range; this catches what Level, Tone and Dynamics change on top of that. does not read as better. Off once you are setting levels deliberately. |
| **Band Count** | 1 to 6 | How many bands the crossover splits the signal into. Changing it while audio is playing crossfades the old and new splits over 20 ms, and because the crossover sums back to the original signal the total level never jumps. costs CPU, so add one only when you can name what it is for. |
| **Crossover Frequency** | 20 Hz to 20 kHz | The edge between band N and band N+1; defaults 120, 600, 2500, 6000, 12000 Hz. Dragging on the display keeps edges a third of an octave apart; the engine's own backstop is looser, so automation can push them closer. body of a kick, above the fundamental of a bass. Judge the result unsoloed. |
| **Crossover Mode** | Minimum Phase or Linear Phase | Minimum Phase (the default) is a 4th-order Linkwitz-Riley with zero latency; the bands sum flat in magnitude. Linear Phase has constant group delay and no phase smear, at the cost of latency (reported to the host) and a little pre-ringing. smears a transient. Minimum Phase while tracking or monitoring. |
| **Stereo Mode** | Stereo or Mid-Side | In Mid-Side the signal is encoded to mid and side before the crossover and decoded after the band sum, so the whole multiband chain — drive, tone, dynamics, pan, width — operates on mid and side rather than left and right. reverse. Band Width behaves differently here, so set it after you switch, not before. |
| **Oversampling** | Off, 2x, 4x, 8x or 16x | The realtime factor, using minimum-phase polyphase filters. Higher factors push aliasing from the hard-edged styles further out of band, at a cost in CPU and latency. Decimate or Bitcrush grits in a way that does not track the pitch. |
| **Oversampling Offline** | Off, 2x, 4x, 8x or 16x | A separate factor used only when the host reports an offline render, default 8x. Offline also switches from the minimum-phase polyphase filters to linear-phase FIR ones: nothing is waiting on latency or CPU, so it takes the better path. its own, so you monitor cheaply and bounce at quality. |
| **Dither** | Off, Rectangular or Triangular | Selects the noise the Bitcrush style's quantiser adds to decorrelate its quantisation error. Triangular (TPDF) is the default and the usual choice. Note that this control is not currently connected to the DSP: the Bitcrush style always uses its own triangular default whatever this is set to. your session, so it is safe to leave alone. |

### Per band

| Control | Range | What it does |
|---|---|---|
| **Drive** | 0 to 40 dB | How hard the band is pushed into its saturation style. Each style applies drive itself, so it means "further into this circuit", not "louder". Every style is level-matched across the whole range, so a sweep changes character, not level. off — parallel blend keeps the transients that heavy drive flattens. |
| **Style** | 19 styles, Clean Tube through Breathe | The nonlinearity this band runs, in eight families: tube, tape, transformer, amp, rectify, destroy, bitcrush and FX. Changes crossfade over 20 ms and every style is level-matched, so switching compares character, not loudness. for softened highs, destroy for obviously broken — then audition inside it. |
| **Mix** | 0 to 100 % | Parallel blend for this band only, against a dry tap delayed to match the band's own latency. At 0 % the band is untouched; at 100 % you hear only the chain. source. Drive hard and blend back rather than driving gently — you keep the transients that heavy drive flattens. |
| **Level** | −24 to +24 dB | The band's gain, applied after tone and before the band's dry/wet blend. It does not change how hard the saturator is hit; it changes how much of this band appears in the sum. that a style has quietly lifted. For loudness into the saturator, use Drive. |
| **Pan** | −100 to +100 | Constant-power pan for this band, unity gain at centre. It is applied after Width, so the two combine predictably. In Mid-Side stereo mode this pans the mid/side pair, which is rarely what you want. a grungy upper-mid band pushed slightly off centre, lows left alone. |
| **Width** | 0 to 200 % | Scales the band's side component before the pan law: 0 % is mono, 100 % is unchanged, 200 % doubles the difference between the channels. Narrowing a band does not change its level at the centre. to a small speaker, and widen an air band for lift that costs no level. |
| **Feedback** | 0 to 100 % | Sends the band's saturated output back through a one-period delay and a bandpass tuned to Feedback Frequency. Loop gain is capped at 0.95 with a soft limiter inside it, so it can howl but cannot run away. Q rises with the knob, 2 to 8. bypasses it and flushes the delay line. |
| **Feedback Frequency** | 20 Hz to 2 kHz | The pitch the feedback loop resonates at. The delay is exactly one period long, so the round trip arrives back in phase and sings rather than combs. source already has. It does nothing at all while Feedback is at 0. |
| **Dynamics** | −100 to +100 % | One bipolar knob, two behaviours. Positive compresses — threshold −6 dB down to −30 dB, ratio up to 8:1, program-dependent attack; negative expands and gates. Exactly 0 is a true bypass: the buffer is not touched at all. negative to gate the bleed out of a band you drove hard. |
| **Tone Position** | Post or Pre | Whether the band's three-node EQ runs after the saturator or before it. Post (the default) shapes what came out; Pre changes what goes in, and therefore which harmonics the saturator generates at all. before a tube stage is a different instrument from boosting it afterwards. |
| **Tone Low** | −12 to +12 dB | Gain of the low shelf, at Tone Low Freq, with a Butterworth slope (Q 0.707). Like the whole tone stage, it only affects this band's own frequency range, however far outside it the shelf is set. low end before a Pre-positioned saturator so it stops eating headroom. |
| **Tone Low Frequency** | 20 Hz to 1 kHz | Corner of the low shelf; default 150 Hz. Dragging the node sideways on the display clamps it to the band's own span, but the parameter range is wider, so automation and modulation can take it outside. the low mids when the band is carrying body rather than bass. |
| **Tone Mid** | −12 to +12 dB | Gain of the peaking filter, at Tone Mid Freq with Tone Mid Q. This is the node that does the surgical work; the two shelves are for broad tilts. with Tone Position set to Pre — to aim the saturator at one part of the band. |
| **Tone Mid Frequency** | 100 Hz to 8 kHz | Centre of the peaking filter; default 1 kHz. Dragging the node sideways clamps it to the band's own span; the parameter range itself is wider. there. Pair it with a high Q for a notch and a low one for a tilt. |
| **Tone Mid Q** | 0.2 to 6 | Width of the peaking filter; default 0.7. Low values are a broad tilt over most of the band, high values a narrow notch or peak. (under 1) when you want the band to change character rather than lose one note. |
| **Tone High** | −12 to +12 dB | Gain of the high shelf, at Tone High Freq, with a Butterworth slope (Q 0.707). or to add air after a tape style has softened the top. |
| **Tone High Frequency** | 1 kHz to 18 kHz | Corner of the high shelf; default 4 kHz. Dragging the node sideways clamps it to the band's own span; the parameter range is wider. 10 kHz and above when you only want air. |
| **Tone Bypass** | Off or On | Switches the whole tone stage off for this band. The three gains are flattened rather than the filters skipped, so the node positions survive, the band's latency does not change, and there is no click on the way back. the saturation. Faster and more honest than zeroing three nodes by hand. |
| **Bypass** | Off or On | Takes the band's processing out: the dry signal passes through, still delayed to match the other bands so the sum does not comb. Level, Pan and Width still apply, and the band reads cold on the heat display. crossover. To hear the band on its own instead, use Solo. |
| **Solo** | Off or On | Mutes every band that is not soloed. Several bands can be soloed at once; with none soloed, all bands pass. sounds harsh soloed is not necessarily wrong in context — always decide unsoloed. |

### Modulation sources

| Control | Range | What it does |
|---|---|---|
| **LFO Rate** | 0.01 to 40 Hz | Cycle rate of the shape LFO. With Sync on, this same control carries the division against a 120 BPM reference where a quarter note is 2 Hz — so 2 Hz is one beat, 1 Hz is two, 0.5 Hz is a bar of four. above 10 Hz for tremolo and buzz. Turn Sync on first if you want it in time. |
| **LFO Sync** | Free or Tempo Sync | Free runs at the Rate in Hz. Tempo Sync re-derives the phase from the host's timeline every time it reports a new position, so the shape stays locked through loops, locates and tempo changes. deliberately does not line up with the grid. |
| **LFO Phase** | 0 to 360° | Offset applied when the shape is read, not to the running phase, so changing it never makes the LFO jump or restart. step, or to set where in the cycle a synced LFO sits against the bar. |
| **LFO Depth** | 0 to 100 % | Scales the LFO's bipolar −1…+1 output. This is the source's own level, before each connection's amount — turning it down thins every destination at once. much. For one destination alone, use that connection's amount instead. |
| **LFO Smooth** | 0 to 100 % | A one-pole smoother on the output, 0 to 500 ms across the range. It rounds the corners of a stepped or sharp-cornered shape. Drive or a crossover. Too much and a fast LFO flattens into almost nothing. |
| **LFO Steps** | Off, or up to 32 | Quantises the cycle to a grid of this many steps. Off (and 1) leave the shape smooth; 2 and above snap it, turning any shape into a sequence. feel. A random-looking shape stepped to 4 makes a usable pattern generator. |
| **Env Gen Attack** | 0.1 to 2000 ms | Time to rise to full after the envelope triggers. Segments are exponential with an overshooting target, so a retrigger part-way through the release restarts from wherever the level is and still arrives at 1. something after it has started. |
| **Env Gen Decay** | 1 to 5000 ms | Time to fall from full to the Sustain level once the attack has finished. envelope that keeps moving for the length of a note. |
| **Env Gen Sustain** | 0 to 100 % | The level the envelope holds at while the gate is open, after the decay. with slopes. At 0 % you get a blip whose length is the decay time. |
| **Env Gen Release** | 1 to 5000 ms | Time to fall back to zero after the gate closes — the detector dropping back below threshold in Input Transient mode, or the last MIDI note lifting. short ones when the envelope should stop as soon as the note does. |
| **Env Gen Threshold** | −60 to 0 dB | Detector level at which the envelope fires, in Input Transient mode only. The trigger is hysteretic — it re-arms only after the detector has fallen to 70 % of the threshold — so a signal sitting right on it cannot machine-gun the envelope. mode it has no effect at all. |
| **Env Gen Trigger** | Input Transient or MIDI Note | What fires the envelope. Input Transient watches the full-range detector against Threshold. MIDI Note fires on note-on and releases when the last held note lifts, so a legato chord holds the gate open. are feeding Ember a MIDI part alongside the audio and want it in time with that. |
| **Env Follower Attack** | 0.1 to 500 ms | How fast the follower rises towards a louder signal. shape of a phrase instead of every hit. |
| **Env Follower Release** | 1 to 2000 ms | How fast the follower falls back once the signal quietens. 100 ms upward on drums. Short releases plus a fast attack give a twitchy source. |
| **Env Follower Band** | Full Range, or Band 1 to 6 | Which signal the follower listens to: the whole input, or one band's RMS. The engine measures per-band RMS once per control block and every follower shares it, so choosing a band costs nothing extra. at a high band to drive it from hats and sibilance. |
| **Env Follower Gain** | −24 to +24 dB | Make-up applied to the detector before the level-to-value mapping, which is logarithmic: −60 dBFS maps to 0 and 0 dBFS to 1. follower that sits near 0 all the time needs gain, not a faster attack. |
| **XY X** | 0 to 100 % | Horizontal position of the XY pad, smoothed before it is used. This is the axis the XY modulation source emits. hand and automate later — two or three connections at different amounts off one gesture. |
| **XY Y** | 0 to 100 % | Vertical position of the XY pad, smoothed alongside X. Note that the XY modulation source currently emits the X axis only, so this value is stored and recalled but does not drive modulation on its own. parameter. To modulate from a second hand-controlled value today, use a Macro. |
| **MIDI Type** | Velocity, CC, Mod Wheel or Note Number | What this MIDI source reads: the velocity of the most recent note-on, an arbitrary CC, CC 1 broken out for convenience, or the note number scaled by 127. All four produce 0 to 1. to make a filter or crossover track pitch. |
| **MIDI CC** | 0 to 127 | Which continuous controller the source follows when Type is CC. Ignored for the other three types. Wheel already covers CC 1 without setting this. |
| **MIDI Smooth** | 0 to 500 ms | One-pole smoothing on the source's output. MIDI arrives in 7-bit steps, which step audibly on a destination like Drive without this. for a mod wheel driving something slow, lower it when the response feels late. |
| **Macro** | 0 to 100 % | A smoothed knob that exists only to be a modulation source, so one gesture can move as many destinations as you like at whatever amounts and curves you set. together. Build the sound at both extremes, then drive the macro between them. |

<!-- END GENERATED PARAMETER REFERENCE -->
