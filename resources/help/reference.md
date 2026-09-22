# Ember — parameter reference

One article per parameter kind. `N` stands for the index: `bandNDrive` is the
article for `band1Drive` through `band6Drive`.

---

## inGain
**Input Gain** — −24 to +24 dB

Level into the whole chain, before mid/side encoding and the crossover. It
therefore changes how hard every band hits its saturator, on top of each band's
own Drive.

*When to use it:* to bring a quiet source up to where the styles start working
without touching six Drive knobs. Leave it at 0 and use Drive if you want one
band pushed and the others clean.

## outGain
**Output Gain** — −24 to +24 dB

The last gain in the chain, after global Mix and auto-gain. It is a clean gain
stage: nothing downstream of it distorts.

*When to use it:* to match Ember's output back to the level your session
expects. Use it rather than Input Gain when you only want the level changed,
not the amount of saturation.

## mix
**Mix** — 0 to 100 %

Global dry/wet against a delay-compensated dry tap, so blending never combs
however much oversampling or linear-phase latency is in the chain.

*When to use it:* parallel processing across every band at once. Per-band Mix
is the finer tool — reach for this to pull the whole effect back at once.

*Show me:* quickstart#5

## autoGain
**Auto Gain** — Off or On

Continuously RMS-matches the wet path back to the dry over a ~0.3 s window,
clamped to ±12 dB. Ember already gain-matches each style across the whole drive
range; this catches what Level, Tone and Dynamics change on top of that.

*When to use it:* on while you audition styles and drive amounts, so louder
does not read as better. Off once you are setting levels deliberately.

*Show me:* quickstart#6

## numBands
**Band Count** — 1 to 6

How many bands the crossover splits the signal into. Changing it while audio is
playing crossfades the old and new splits over 20 ms, and because the crossover
sums back to the original signal the total level never jumps.

*When to use it:* one or two on a mix bus, three on drums. Each extra band
costs CPU, so add one only when you can name what it is for.

*Show me:* multiband#1

## xoverN
**Crossover Frequency** — 20 Hz to 20 kHz

The edge between band N and band N+1; defaults 120, 600, 2500, 6000, 12000 Hz.
Edges are kept a third of an octave apart however they are moved — by hand, by
automation or by modulation — so a band can never be squeezed down to nothing.

*When to use it:* place edges where the source changes character — under the
body of a kick, above the fundamental of a bass. Judge the result unsoloed.

*Show me:* multiband#2

## xoverMode
**Crossover Mode** — Minimum Phase or Linear Phase

Minimum Phase (the default) is a 4th-order Linkwitz-Riley with zero latency; the
bands sum flat in magnitude. Linear Phase has constant group delay and no phase
smear, at the cost of latency (reported to the host) and a little pre-ringing.

*When to use it:* Linear Phase on a mix bus, or wherever a band's phase rotation
smears a transient. Minimum Phase while tracking or monitoring.

*Show me:* multiband#5

## stereoMode
**Stereo Mode** — Stereo or Mid-Side

In Mid-Side the signal is encoded to mid and side before the crossover and
decoded after the band sum, so the whole multiband chain — drive, tone,
dynamics, pan, width — operates on mid and side rather than left and right.

*When to use it:* to saturate the centre without touching the sides, or the
reverse. Band Width behaves differently here, so set it after you switch, not
before.

## osFactor
**Oversampling** — Off, 2x, 4x, 8x or 16x

The realtime factor, using minimum-phase polyphase filters. Higher factors push
aliasing from the hard-edged styles further out of band, at a cost in CPU and
latency.

*When to use it:* 2x is a sane default; go higher when a bright source through
Decimate or Bitcrush grits in a way that does not track the pitch.

*Show me:* hq-cpu#1

## osOffline
**Oversampling Offline** — Off, 2x, 4x, 8x or 16x

A separate factor used only when the host reports an offline render, default 8x.
Offline also switches from the minimum-phase polyphase filters to linear-phase
FIR ones: nothing is waiting on latency or CPU, so it takes the better path.

*When to use it:* set this high and the realtime factor low. Ember switches on
its own, so you monitor cheaply and bounce at quality.

*Show me:* hq-cpu#2

## dither
**Dither** — Off, Rectangular or Triangular

Selects the noise the Bitcrush style's quantiser adds to decorrelate its
quantisation error. Triangular (TPDF) is the default and the usual choice: it
turns the harsh, signal-locked distortion of a bare quantiser into an even hiss
that sits under the music. Rectangular is quieter but leaves the noise floor
moving with the signal. Off is the raw quantiser, which on low bit depths is a
sound in its own right rather than a fault.

*When to use it:* leave it on Triangular unless you want the artefacts. Turn it
Off when you are after the gritty, pumping edge of an early sampler, and reach
for Rectangular when Triangular's hiss is audible in the quiet parts but you
still want the error broken up.

## bandNDrive
**Drive** — 0 to 40 dB

How hard the band is pushed into its saturation style. Each style applies drive
itself, so it means "further into this circuit", not "louder". Every style is
level-matched across the whole range, so a sweep changes character, not level.

*When to use it:* this is the main control. Reach for Mix before backing Drive
off — parallel blend keeps the transients that heavy drive flattens.

*Show me:* quickstart#3

## bandNStyle
**Style** — 19 styles, Clean Tube through Breathe

The nonlinearity this band runs, in eight families: tube, tape, transformer,
amp, rectify, destroy, bitcrush and FX. Changes crossfade over 20 ms and every
style is level-matched, so switching compares character, not loudness.

*When to use it:* pick the family first — tube for even-harmonic warmth, tape
for softened highs, destroy for obviously broken — then audition inside it.

*Show me:* styles#1

## bandNMix
**Mix** — 0 to 100 %

Parallel blend for this band only, against a dry tap delayed to match the band's
own latency. At 0 % the band is untouched; at 100 % you hear only the chain.

*When to use it:* the most useful control for keeping distortion from eating the
source. Drive hard and blend back rather than driving gently — you keep the
transients that heavy drive flattens.

*Show me:* recipes#1

## bandNLevel
**Level** — −24 to +24 dB

The band's gain, applied after tone and before the band's dry/wet blend. It
does not change how hard the saturator is hit; it changes how much of this band
appears in the sum.

*When to use it:* to rebalance bands after the crossover, or to trim a band
that a style has quietly lifted. For loudness into the saturator, use Drive.

## bandNPan
**Pan** — −100 to +100

Constant-power pan for this band, unity gain at centre. It is applied after
Width, so the two combine predictably. In Mid-Side stereo mode this pans the
mid/side pair, which is rarely what you want.

*When to use it:* to move one frequency range without moving the whole source —
a grungy upper-mid band pushed slightly off centre, lows left alone.

## bandNWidth
**Width** — 0 to 200 %

Scales the band's side component before the pan law: 0 % is mono, 100 % is
unchanged, 200 % doubles the difference between the channels. Narrowing a band
does not change its level at the centre.

*When to use it:* mono the lowest band so the bass stays centred and translates
to a small speaker, and widen an air band for lift that costs no level.

## bandNFeedback
**Feedback** — 0 to 100 %

Sends the band's saturated output back through a one-period delay and a bandpass
tuned to Feedback Frequency. Loop gain is capped at 0.95 with a soft limiter
inside it, so it can howl but cannot run away. Q rises with the knob, 2 to 8.

*When to use it:* for the howl and the ring, not as a subtle tool. Exactly 0
bypasses it and flushes the delay line.

*Show me:* feedback-dynamics#1

## bandNFeedbackFreq
**Feedback Frequency** — 20 Hz to 2 kHz

The pitch the feedback loop resonates at. The delay is exactly one period long,
so the round trip arrives back in phase and sings rather than combs.

*When to use it:* tune it to the track — a note in the key, or a resonance the
source already has. It does nothing at all while Feedback is at 0.

*Show me:* feedback-dynamics#2

## bandNDynamics
**Dynamics** — −100 to +100 %

One bipolar knob, two behaviours. Positive compresses — threshold −6 dB down to
−30 dB, ratio up to 8:1, program-dependent attack; negative expands and gates.
Exactly 0 is a true bypass: the buffer is not touched at all.

*When to use it:* positive to re-glue a band that saturation has made spiky,
negative to gate the bleed out of a band you drove hard.

*Show me:* feedback-dynamics#3

## bandNTonePre
**Tone Position** — Post or Pre

Whether the band's three-node EQ runs after the saturator or before it. Post
(the default) shapes what came out; Pre changes what goes in, and therefore
which harmonics the saturator generates at all.

*When to use it:* Pre when you want to steer the distortion — boosting 3 kHz
before a tube stage is a different instrument from boosting it afterwards.

*Show me:* tone-eq#2

## bandNToneLow
**Tone Low** — −12 to +12 dB

Gain of the low shelf, at Tone Low Freq, with a Butterworth slope (Q 0.707).
Like the whole tone stage, it only affects this band's own frequency range,
however far outside it the shelf is set.

*When to use it:* to put weight back after a style thinned the band, or to trim
low end before a Pre-positioned saturator so it stops eating headroom.

*Show me:* tone-eq#1

## bandNToneLowHz
**Tone Low Frequency** — 20 Hz to 1 kHz

Corner of the low shelf; default 150 Hz. Dragging the node sideways on the
display clamps it to the band's own span, but the parameter range is wider, so
automation and modulation can take it outside.

*When to use it:* move it down to lift only the deepest weight, or up towards
the low mids when the band is carrying body rather than bass.

## bandNToneMid
**Tone Mid** — −12 to +12 dB

Gain of the peaking filter, at Tone Mid Freq with Tone Mid Q. This is the node
that does the surgical work; the two shelves are for broad tilts.

*When to use it:* to notch a resonance that saturation has made obvious, or —
with Tone Position set to Pre — to aim the saturator at one part of the band.

## bandNToneMidHz
**Tone Mid Frequency** — 100 Hz to 8 kHz

Centre of the peaking filter; default 1 kHz. Dragging the node sideways clamps
it to the band's own span; the parameter range itself is wider.

*When to use it:* sweep it with a boost to find what is bothering you, then cut
there. Pair it with a high Q for a notch and a low one for a tilt.

## bandNToneMidQ
**Tone Mid Q** — 0.2 to 6

Width of the peaking filter; default 0.7. Low values are a broad tilt over most
of the band, high values a narrow notch or peak.

*When to use it:* high (3 and up) to remove a single ringing resonance, low
(under 1) when you want the band to change character rather than lose one note.

## bandNToneHigh
**Tone High** — −12 to +12 dB

Gain of the high shelf, at Tone High Freq, with a Butterworth slope (Q 0.707).

*When to use it:* to take the edge off a band that a bright style has sharpened,
or to add air after a tape style has softened the top.

## bandNToneHighHz
**Tone High Frequency** — 1 kHz to 18 kHz

Corner of the high shelf; default 4 kHz. Dragging the node sideways clamps it
to the band's own span; the parameter range is wider.

*When to use it:* down around 2–4 kHz to work on presence and bite, up towards
10 kHz and above when you only want air.

## bandNToneBypass
**Tone Bypass** — Off or On

Switches the whole tone stage off for this band. The three gains are flattened
rather than the filters skipped, so the node positions survive, the band's
latency does not change, and there is no click on the way back.

*When to use it:* to hear how much of what you like is the EQ and how much is
the saturation. Faster and more honest than zeroing three nodes by hand.

## bandNBypass
**Bypass** — Off or On

Takes the band's processing out: the dry signal passes through, still delayed to
match the other bands so the sum does not comb. Level, Pan and Width still
apply, and the band reads cold on the heat display.

*When to use it:* to check what one band is contributing without changing the
crossover. To hear the band on its own instead, use Solo.

*Show me:* multiband#4

## bandNSolo
**Solo** — Off or On

Mutes every band that is not soloed. Several bands can be soloed at once; with
none soloed, all bands pass.

*When to use it:* for finding crossover points, and only for that. A band that
sounds harsh soloed is not necessarily wrong in context — always decide
unsoloed.

*Show me:* multiband#3

## lfoNRate
**LFO Rate** — 0.01 to 40 Hz

Cycle rate of the shape LFO. With Sync on, this same control carries the
division against a 120 BPM reference where a quarter note is 2 Hz — so 2 Hz is
one beat, 1 Hz is two, 0.5 Hz is a bar of four.

*When to use it:* under 1 Hz for slow movement you feel rather than hear, and
above 10 Hz for tremolo and buzz. Turn Sync on first if you want it in time.

*Show me:* modulation#3

## lfoNSync
**LFO Sync** — Free or Tempo Sync

Free runs at the Rate in Hz. Tempo Sync re-derives the phase from the host's
timeline every time it reports a new position, so the shape stays locked through
loops, locates and tempo changes.

*When to use it:* Sync for anything rhythmic. Free when you want movement that
deliberately does not line up with the grid.

*Show me:* modulation#3

## lfoNPhase
**LFO Phase** — 0 to 360°

Offset applied when the shape is read, not to the running phase, so changing it
never makes the LFO jump or restart.

*When to use it:* to offset one LFO against another so two bands move out of
step, or to set where in the cycle a synced LFO sits against the bar.

## lfoNDepth
**LFO Depth** — 0 to 100 %

Scales the LFO's bipolar −1…+1 output. This is the source's own level, before
each connection's amount — turning it down thins every destination at once.

*When to use it:* when one LFO drives several destinations and they are all too
much. For one destination alone, use that connection's amount instead.

## lfoNSmooth
**LFO Smooth** — 0 to 100 %

A one-pole smoother on the output, 0 to 500 ms across the range. It rounds the
corners of a stepped or sharp-cornered shape.

*When to use it:* to take the click out of a square or stepped shape driving
Drive or a crossover. Too much and a fast LFO flattens into almost nothing.

## lfoNSteps
**LFO Steps** — Off, or up to 32

Quantises the cycle to a grid of this many steps. Off (and 1) leave the shape
smooth; 2 and above snap it, turning any shape into a sequence.

*When to use it:* with 8 or 16 steps and Sync on for a stepped, sequenced
feel. A random-looking shape stepped to 4 makes a usable pattern generator.

## egNAttack
**Env Gen Attack** — 0.1 to 2000 ms

Time to rise to full after the envelope triggers. Segments are exponential with
an overshooting target, so a retrigger part-way through the release restarts
from wherever the level is and still arrives at 1.

*When to use it:* short (under 5 ms) to catch a transient, long to swell into
something after it has started.

## egNDecay
**Env Gen Decay** — 1 to 5000 ms

Time to fall from full to the Sustain level once the attack has finished.

*When to use it:* short with a low Sustain for a percussive blip, long for an
envelope that keeps moving for the length of a note.

## egNSustain
**Env Gen Sustain** — 0 to 100 %

The level the envelope holds at while the gate is open, after the decay.

*When to use it:* at 100 % the decay does nothing and the envelope is a gate
with slopes. At 0 % you get a blip whose length is the decay time.

## egNRelease
**Env Gen Release** — 1 to 5000 ms

Time to fall back to zero after the gate closes — the detector dropping back
below threshold in Input Transient mode, or the last MIDI note lifting.

*When to use it:* long releases on a slow destination like a crossover edge,
short ones when the envelope should stop as soon as the note does.

## egNThreshold
**Env Gen Threshold** — −60 to 0 dB

Detector level at which the envelope fires, in Input Transient mode only. The
trigger is hysteretic — it re-arms only after the detector has fallen to 70 % of
the threshold — so a signal sitting right on it cannot machine-gun the envelope.

*When to use it:* set it just under the peaks you want to catch. In MIDI Note
mode it has no effect at all.

## egNTrigger
**Env Gen Trigger** — Input Transient or MIDI Note

What fires the envelope. Input Transient watches the full-range detector against
Threshold. MIDI Note fires on note-on and releases when the last held note
lifts, so a legato chord holds the gate open.

*When to use it:* Input Transient on audio with clear hits. MIDI Note when you
are feeding Ember a MIDI part alongside the audio and want it in time with that.

## efNAttack
**Env Follower Attack** — 0.1 to 500 ms

How fast the follower rises towards a louder signal.

*When to use it:* under 1 ms to follow transients, 20 ms and up to follow the
shape of a phrase instead of every hit.

## efNRelease
**Env Follower Release** — 1 to 2000 ms

How fast the follower falls back once the signal quietens.

*When to use it:* long enough that the follower does not chatter between hits —
100 ms upward on drums. Short releases plus a fast attack give a twitchy source.

## efNBand
**Env Follower Band** — Full Range, or Band 1 to 6

Which signal the follower listens to: the whole input, or one band's RMS. The
engine measures per-band RMS once per control block and every follower shares
it, so choosing a band costs nothing extra.

*When to use it:* point it at the low band to drive something from the kick, or
at a high band to drive it from hats and sibilance.

## efNGain
**Env Follower Gain** — −24 to +24 dB

Make-up applied to the detector before the level-to-value mapping, which is
logarithmic: −60 dBFS maps to 0 and 0 dBFS to 1.

*When to use it:* to bring a quiet source into the useful part of the range. A
follower that sits near 0 all the time needs gain, not a faster attack.

## xyX
**XY X** — 0 to 100 %

Horizontal position of the XY pad, smoothed before it is used. Whether the
modulation source emits this axis or the vertical one is set by XY Axis;
either way both coordinates stay live and both can be modulated.

*When to use it:* as a performance control for anything you want to sweep by
hand and automate later — two or three connections at different amounts off one
gesture.

## xyY
**XY Y** — 0 to 100 %

Vertical position of the XY pad, smoothed alongside X. Set XY Axis to Y and the
modulation source emits this coordinate instead of the horizontal one. It is a
modulation destination in its own right, so a second modulator can drive the
pad's vertical position while you move the horizontal one by hand.

*When to use it:* whenever a gesture wants two dimensions — one axis sent to the
routing, the other holding a position you set. Pair it with XY Axis; on its own
this parameter only moves the pad.

## midiNType
**MIDI Type** — Velocity, CC, Mod Wheel or Note Number

What this MIDI source reads: the velocity of the most recent note-on, an
arbitrary CC, CC 1 broken out for convenience, or the note number scaled by 127.
All four produce 0 to 1.

*When to use it:* Velocity to make playing harder mean more drive, Note Number
to make a filter or crossover track pitch.

## midiNCC
**MIDI CC** — 0 to 127

Which continuous controller the source follows when Type is CC. Ignored for the
other three types.

*When to use it:* when your controller sends something other than CC 1. Mod
Wheel already covers CC 1 without setting this.

## midiNSmooth
**MIDI Smooth** — 0 to 500 ms

One-pole smoothing on the source's output. MIDI arrives in 7-bit steps, which
step audibly on a destination like Drive without this.

*When to use it:* 20 ms (the default) is enough for most controllers. Raise it
for a mod wheel driving something slow, lower it when the response feels late.

## macroN
**Macro** — 0 to 100 %

A smoothed knob that exists only to be a modulation source, so one gesture can
move as many destinations as you like at whatever amounts and curves you set.

*When to use it:* whenever you find yourself wanting to move three controls
together. Build the sound at both extremes, then drive the macro between them.

*Show me:* modulation#4

## xyAxis
**XY Axis** — X or Y

Which of the XY pad's two dimensions the modulation source emits. A pad has two
axes but a modulation source carries one value, so this chooses between them.

*When to use it:* set it to Y when you want the vertical axis driving a
routing; leave it on X otherwise. Both axes are always live in the pad, so
switching this changes which one is sent without losing the other's position.

*Show me:* modulation#2
