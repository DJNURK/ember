# Ember — Factory Preset Library (Manifest)

This file is the **design** of Ember's 35 factory presets. It is the source of truth from
which `resources/presets/<Category>/<NN>_<Name>.xml` is generated once the parameter layout in
`src/plugin/ParameterIDs.h` is frozen. No per-preset XML exists yet by design — the IDs are still
moving, so `resources/presets/` holds only the `Init.xml` placeholder.

The spec asks for at least 30 presets across six categories. This library has **35**, so there
is margin for one or two to be dropped in voicing without falling under the bar.

## Design intent

A preset library is worthless if every entry is "drive 20, Warm Tube". Ember's factory bank is
built around the idea that each source material needs a *different kind* of processing, not a
different amount of the same processing:

- **Drums** are about transient behaviour and band-split control. Wherever the point is to keep
  the hit intact (01, 02, 06, 07) the low band gets weight and a compressive dynamics setting so
  it stays steady, while the top band gets a negative (expansive) setting so the stick and beater
  attack survives instead of being flattened. The three presets whose point is the opposite — the
  room-mic glue of 03, the overhead density of 04 and the parallel smash of 05 — are compressive
  in every band on purpose. Crossovers are placed on drum anatomy — beater click, snare body,
  cymbal sheen — not on round numbers.
- **Bass** is about low-band weight *without* mud. Four of the six bass presets (08, 09, 12, 13)
  leave the lowest band almost dry (mix 15–45 %) and do the audible harmonic work in the mids and
  highs, which is what actually makes bass translate on small speakers; the two that do not (10,
  11) are the ones whose whole job is low-band weight.
- **Vocals** are about restraint and upper-mid presence. In the four full-signal presets drive
  stays low and band mix at or below 60 %, and the high band carries a small positive tone lift
  rather than brute drive. The exception is 17, which drives hard on purpose and is then blended
  in parallel at 45 % global mix.
- **Guitar** spans clean sparkle to feedback-on-the-edge sustain, and uses the amp styles where
  they belong.
- **Mix bus** is very gentle wideband work: drive 2–6 dB, mix 15–35 %, **auto-gain on in every
  preset**, and no destructive styles anywhere in the category.
- **Creative** is where the destructive styles (Foldback, Hard Clip, Decimate, Bitcrush, Broken
  Tube, Rectify) and essentially all of the modulation live. Several of these presets deliberately
  run low or no oversampling because the aliasing *is* the effect.

Coverage was designed, not accidental: all 19 styles are used, every band count from 1 to 6 is
represented, every oversampling factor from Off to 16× appears, and all six modulation source
types are exercised. The checks at the end of this file verify that.

### Naming policy

Names describe the **sound**, not somebody's product. No preset references a commercial plugin or
a trademarked hardware name — no manufacturer names, no model numbers, no drum-machine numbers.
Generic craft vocabulary ("console", "tape", "transformer", "amp", "reamp") is used as ordinary
audio-engineering language and is not a brand reference.

## How to read a preset entry

Each preset has a per-band table, a global line, and — where it applies — a modulation section.

| Column | Parameter | Range |
|---|---|---|
| `#` | band index (1-based; zero-based in code) | 1 … `numBands` |
| `Range (Hz)` | derived from the crossover list, shown for readability only | — |
| `Style` | `pid::style(band)` | one of the 19 `StyleID` values |
| `Drive (dB)` | `pid::drive(band)` | 0 … 40 |
| `Mix (%)` | `pid::bandMix(band)` | 0 … 100 |
| `Level (dB)` | `pid::level(band)` | −24 … +24 |
| `Pan` | `pid::pan(band)` | −100 … +100, 0 = centre |
| `Width (%)` | `pid::width(band)` | 0 … 200, 100 = unchanged |
| `FB (%)` | `pid::feedback(band)` | 0 … 100 |
| `FB (Hz)` | `pid::feedbackFreq(band)` | 20 … 2000 |
| `Dyn (%)` | `pid::dynamics(band)` | −100 … +100 |
| `Low / Mid / High (dB)` | `pid::toneLow/toneMid/toneHigh(band)` | −12 … +12 each |

### Conventions

1. **Dynamics sign.** `+` is **compressive** (drive is levelled, sustain is brought up, the band
   sits still); `−` is **expansive** (transients are accentuated, the stage opens up on peaks).
   Every value in this manifest is written with that convention. If the DSP ends up with the
   opposite sign, negate every `Dyn` value at XML-generation time rather than re-voicing presets.
2. **`—` in the `FB (Hz)` column** means feedback amount is 0 %, so the frequency is inert; write
   the parameter's own default (200 Hz) into the XML.
3. **Feedback frequency always lies inside its band's passband.** A resonance tuned outside the
   band it lives in is inaudible. Check this if crossovers are re-voiced.
4. **Unstated parameters take their plugin defaults:** pan 0, width 100 %, feedback 0 %,
   band bypass off, band solo off, input gain 0 dB, output gain 0 dB, crossover mode
   Minimum-phase LR4. Any preset that departs from these says so on its Global line.
   Two parameters are the exception and are written into **every** preset rather than left
   alone, because their plugin defaults in `Parameters.cpp` are not what a preset wants:
   `dither` defaults to **Triangular** and `osOffline` defaults to **8×** independently of the
   realtime factor. So every preset stores dither **Off** except 28, and every preset stores
   offline oversampling **equal to its own realtime oversampling** except 28.
5. **Modulation routings** are written as `<source> -> <destination>, <amount>, <curve>`.
   The amount is a signed percentage of the destination parameter's full range and maps to
   `ModConnection::amount` as `amount / 100` (so +50 % → `0.5f`). Curves are the `ModCurve` names
   exactly: `Linear`, `ExpoIn`, `ExpoOut`, `SCurve`, `Stepped`. Connection smoothing is the
   default 5 ms unless a routing says otherwise.
6. **Modulation sources need their own settings**, so every preset with routings also carries a
   *Source setup* line giving the XLFO / EG / follower / MIDI / macro parameters it depends on.
   A preset that ships a routing without configuring its source is a broken preset.
   *Open gap:* where a Source setup below says `sync 1/8`, `sync 1/4`, `sync 1/16`, `sync 1/2`
   or `sync 4 bars` (presets 29, 30, 31, 33, 35) there is **no parameter to store the division
   in yet** — the layout in `Parameters.cpp` gives each XLFO a boolean `lfoSync`
   (Free / Tempo Sync) and a free-running `lfoRate` in Hz, and nothing else. Those divisions are
   the intended voicing and need either a division parameter or a stated convention for encoding
   one in `lfoRate` before the XML can be generated. Presets 21 and 34 run their XLFO free and
   are unaffected.
7. **Macros ship with a sensible default position** so the preset sounds right the moment it
   loads and the macro is a "turn me" control, not a silent one.
8. **Oversampling is chosen per preset, not globally.** Smooth tanh-class styles at low drive are
   fine at 2×; hard-edged styles (Hard Clip, Foldback, Rectify, Crunch, Lead at high drive) get 8×
   or 16×; the deliberately lo-fi Creative presets run 2× or Off because their aliasing is musical.

### Category counts and file layout

| # | Category | Presets | Directory |
|---|---|---|---|
| 1 | Drums | 7 | `resources/presets/Drums/` |
| 2 | Bass | 6 | `resources/presets/Bass/` |
| 3 | Vocals | 5 | `resources/presets/Vocals/` |
| 4 | Guitar | 5 | `resources/presets/Guitar/` |
| 5 | Mix Bus | 5 | `resources/presets/MixBus/` |
| 6 | Creative | 7 | `resources/presets/Creative/` |
| | **Total** | **35** | |

Preset numbering is global and stable (`01`…`35`) so the files have a deterministic order on disk
and a renamed preset never silently swaps places with another. It does **not** set the browser's
order: `PresetManager::refresh()` sorts factory-first, then by category, then by the preset's
display `name` attribute, so within a category the browser lists presets alphabetically. If the
numbered order is wanted in the GUI too, the number has to go into the sort key, not just the
file name.

---

## 1. Drums

### 01 — Kick Weight and Click
*Separates the weight of a kick from its beater attack so both can be pushed independently.*
`Drums/01_Kick_Weight_and_Click.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 95 | Transformer | 12 | 80 | +1.0 | 0 | 100 | 0 | — | +25 | +1.5 | 0 | −1.0 |
| 2 | 95 – 2200 | Warm Tube | 6 | 45 | −1.5 | 0 | 100 | 0 | — | −15 | −2.0 | −1.0 | 0 |
| 3 | > 2200 | Bright Tape | 9 | 60 | +0.5 | 0 | 100 | 0 | — | −30 | 0 | +1.0 | +2.0 |

**Global:** 3 bands · crossovers 95 / 2200 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
stereo mode Stereo · minimum-phase LR4.

**Modulation**
- Envelope Follower 1 -> Band 3 drive, +18 %, ExpoOut — the click only saturates when the kick hits.

**Source setup:** Envelope Follower 1 — band 1 (sub), attack 1 ms, release 90 ms, gain +3 dB.

---

### 02 — Snare Crack
*Thickens snare body while pushing the crack forward without turning the whole hit into fizz.*
`Drums/02_Snare_Crack.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 180 | Warm Tube | 8 | 55 | 0 | 0 | 100 | 0 | — | +15 | +1.0 | 0 | 0 |
| 2 | 180 – 3500 | Crunch | 7 | 35 | −0.5 | 0 | 100 | 0 | — | −20 | −1.5 | +1.0 | 0 |
| 3 | > 3500 | Bright Tape | 11 | 65 | +1.0 | 0 | 100 | 0 | — | −35 | 0 | +1.0 | +2.5 |

**Global:** 3 bands · crossovers 180 / 3500 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

**Modulation**
- Envelope Generator 1 -> Band 3 drive, +22 %, ExpoOut — transient-triggered, so the top band only
  bites on the hit and the tail stays clean.

**Source setup:** Envelope Generator 1 — trigger Input Transient, threshold −22 dB, attack 0.5 ms,
decay 60 ms, sustain 0 %, release 120 ms.

---

### 03 — Room Mic Glue
*Squashes and colours room or ambience mics so they sit behind the close mics as one block.*
`Drums/03_Room_Mic_Glue.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 350 | Warm Tape | 10 | 70 | −1.0 | 0 | 100 | 0 | — | +40 | +1.0 | −1.0 | 0 |
| 2 | > 350 | Warm Tape | 14 | 85 | 0 | 0 | 130 | 0 | — | +55 | −1.0 | +1.5 | +1.0 |

**Global:** 2 bands · crossover 350 Hz · OS 2× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 04 — Overhead Sheen
*Gentle density on overheads that keeps cymbals from turning brittle when the mix gets loud.*
`Drums/04_Overhead_Sheen.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 1800 | Subtle Tube | 5 | 40 | 0 | 0 | 100 | 0 | — | 0 | 0 | 0 | 0 |
| 2 | > 1800 | Clean Tape | 7 | 55 | −0.5 | 0 | 115 | 0 | — | +20 | 0 | 0 | +1.0 |

**Global:** 2 bands · crossover 1800 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 05 — Parallel Smash Bus
*A deliberately destroyed drum layer meant to be blended under the dry kit, not used on its own.*
`Drums/05_Parallel_Smash_Bus.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 220 | Crunch | 26 | 100 | −3.0 | 0 | 100 | 0 | — | +70 | +2.0 | 0 | −2.0 |
| 2 | > 220 | Lead | 30 | 100 | −4.0 | 0 | 110 | 0 | — | +80 | −3.0 | +2.0 | −1.0 |

**Global:** 2 bands · crossover 220 Hz · OS 8× · **global mix 35 %** · auto-gain **OFF** (the
blend is the point; auto-gain would fight it) · Stereo · minimum-phase LR4.

**Modulation**
- Macro 1 "Blend" -> Global mix, +50 %, Linear — one knob to dial the smashed layer in and out.

**Source setup:** Macro 1 default 0 % (so the preset loads at its stored 35 % global mix and the
macro adds up to +50 % on top).

---

### 06 — Hat and Shaker Polish
*High-frequency-only sweetening for hats, shakers and tambourines that would otherwise sound flat.*
`Drums/06_Hat_and_Shaker_Polish.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 4000 | Clean Tube | 3 | 25 | 0 | 0 | 100 | 0 | — | 0 | 0 | 0 | 0 |
| 2 | > 4000 | Shimmer | 6 | 45 | −0.5 | 0 | 110 | 0 | — | −25 | 0 | 0 | +1.5 |

**Global:** 2 bands · crossover 4000 Hz · **OS 8×** (all the content is near Nyquist, so aliasing
is the whole risk) · global mix 100 % · auto-gain **ON** · Stereo · minimum-phase LR4.

---

### 07 — Kit Sculpt Six
*Full six-band treatment of a drum bus: weight, body, honk control, crack and sheen each handled
separately.*
`Drums/07_Kit_Sculpt_Six.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 60 | Clean Tube | 5 | 30 | 0 | 0 | 90 | 0 | — | +25 | +1.0 | 0 | 0 |
| 2 | 60 – 130 | Transformer | 12 | 70 | +0.5 | 0 | 95 | 0 | — | +20 | +1.0 | 0 | 0 |
| 3 | 130 – 400 | Warm Tube | 7 | 40 | −1.0 | 0 | 100 | 0 | — | −10 | −1.0 | 0 | 0 |
| 4 | 400 – 1500 | Subtle Tube | 4 | 25 | −1.5 | 0 | 100 | 0 | — | −15 | −1.5 | 0 | 0 |
| 5 | 1500 – 5000 | Crunch | 10 | 45 | +0.5 | 0 | 105 | 0 | — | −25 | 0 | +1.5 | 0 |
| 6 | > 5000 | Bright Tape | 8 | 50 | 0 | 0 | 115 | 0 | — | −20 | 0 | 0 | +1.5 |

**Global:** 6 bands · crossovers 60 / 130 / 400 / 1500 / 5000 Hz · OS 8× · global mix 100 % ·
auto-gain **ON** · Stereo · minimum-phase LR4.

**Modulation**
- Envelope Follower 1 -> Band 5 drive, +20 %, ExpoOut — the crack band leans in when the kick and
  snare land and relaxes between hits.

**Source setup:** Envelope Follower 1 — band 2 (kick weight), attack 2 ms, release 120 ms, gain 0 dB.

---

## 2. Bass

### 08 — Sub Anchor
*Keeps the sub almost untouched while the harmonics that make bass audible are built above it.*
`Bass/08_Sub_Anchor.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 70 | Clean Tube | 4 | 20 | +0.5 | 0 | 70 | 0 | — | +30 | +1.0 | 0 | 0 |
| 2 | 70 – 700 | Transformer | 13 | 75 | 0 | 0 | 100 | 0 | — | +10 | −1.0 | +1.0 | 0 |
| 3 | > 700 | Warm Tube | 9 | 55 | +0.5 | 0 | 100 | 0 | — | 0 | 0 | +1.0 | +0.5 |

**Global:** 3 bands · crossovers 70 / 700 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 09 — Bass Grind Upper
*Makes a flat DI bass cut on phones and laptops by grinding only the top band.*
`Bass/09_Bass_Grind_Upper.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 90 | Subtle Tube | 3 | 15 | +1.0 | 0 | 70 | 0 | — | +20 | +0.5 | 0 | 0 |
| 2 | 90 – 900 | Warm Tape | 11 | 60 | −0.5 | 0 | 100 | 0 | — | +15 | −2.0 | +1.0 | 0 |
| 3 | > 900 | Crunch | 22 | 70 | +2.0 | 0 | 100 | 0 | — | −10 | −2.0 | +2.0 | +1.0 |

**Global:** 3 bands · crossovers 90 / 900 Hz · **OS 8×** (Crunch at 22 dB aliases badly below that) ·
global mix 100 % · auto-gain **ON** · Stereo · minimum-phase LR4.

---

### 10 — Round and Warm
*Smooth, unhurried thickening for fingerstyle electric bass; nothing here sounds like distortion.*
`Bass/10_Round_and_Warm.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 500 | Warm Tube | 10 | 65 | 0 | 0 | 85 | 0 | — | +25 | +1.5 | 0 | −1.0 |
| 2 | > 500 | Warm Tape | 8 | 50 | −1.0 | 0 | 100 | 0 | — | +10 | 0 | +0.5 | −1.5 |

**Global:** 2 bands · crossover 500 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 11 — Synth Bass Iron
*Transformer weight and a tuned low resonance that give a sterile synth bass some physical thump.*
`Bass/11_Synth_Bass_Iron.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 120 | Transformer | 16 | 90 | 0 | 0 | 80 | 12 | 55 | +35 | +2.0 | 0 | −1.0 |
| 2 | > 120 | Transformer | 10 | 60 | −0.5 | 0 | 100 | 0 | — | 0 | 0 | +1.0 | 0 |

**Global:** 2 bands · crossover 120 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 12 — Long Sub Bloom
*For long sustained sub notes: holds the tail up and lets upper grit grow as the note decays, so
the note stays audible on speakers that cannot reproduce its fundamental.*
`Bass/12_Long_Sub_Bloom.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 110 | Clean Tube | 8 | 45 | 0 | 0 | 60 | 18 | 45 | +60 | +1.0 | 0 | 0 |
| 2 | > 110 | Smudge | 14 | 55 | −1.0 | 0 | 100 | 0 | — | +20 | −1.0 | +1.0 | −1.0 |

**Global:** 2 bands · crossover 110 Hz · OS 8× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

**Modulation**
- Envelope Follower 1 -> Band 2 mix, −35 %, SCurve (smoothing 60 ms) — grit backs off on the
  attack and blooms in as the note decays.

**Source setup:** Envelope Follower 1 — band 1 (sub), attack 5 ms, release 400 ms, gain 0 dB.

---

### 13 — Bass Reamp Bite
*A DI bass pushed through amp-style stages in the mids and highs while the fundamental stays clean.*
`Bass/13_Bass_Reamp_Bite.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 100 | Clean Tube | 5 | 25 | +1.0 | 0 | 70 | 0 | — | +25 | +1.5 | 0 | 0 |
| 2 | 100 – 1200 | Clean Amp | 18 | 80 | −1.0 | 0 | 100 | 0 | — | 0 | −1.0 | +1.5 | 0 |
| 3 | > 1200 | Lead | 24 | 65 | 0 | 0 | 100 | 0 | — | −15 | −3.0 | +2.0 | −1.0 |

**Global:** 3 bands · crossovers 100 / 1200 Hz · OS 8× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

## 3. Vocals

### 14 — Vocal Presence Lift
*The everyday vocal preset: a little density low, a little presence high, nothing that announces
itself.*
`Vocals/14_Vocal_Presence_Lift.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 200 | Subtle Tube | 3 | 20 | −0.5 | 0 | 100 | 0 | — | +15 | −1.0 | 0 | 0 |
| 2 | 200 – 2500 | Warm Tube | 6 | 40 | 0 | 0 | 100 | 0 | — | +20 | 0 | +0.5 | 0 |
| 3 | > 2500 | Clean Tube | 8 | 50 | +0.5 | 0 | 100 | 0 | — | −10 | 0 | +1.0 | +1.0 |

**Global:** 3 bands · crossovers 200 / 2500 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 15 — Intimate Whisper
*Brings a quiet, close vocal forward by adding density rather than gain, and pulls back when the
singer pushes.*
`Vocals/15_Intimate_Whisper.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 400 | Subtle Tube | 4 | 25 | 0 | 0 | 100 | 0 | — | +35 | −1.5 | 0 | 0 |
| 2 | > 400 | Breathe | 7 | 35 | +0.5 | 0 | 105 | 0 | — | +40 | 0 | +1.0 | +1.5 |

**Global:** 2 bands · crossover 400 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

**Modulation**
- Envelope Follower 2 -> Band 2 drive, −25 %, ExpoOut (smoothing 30 ms) — loud phrases get less
  drive, so the preset never turns spitty on a belted line.

**Source setup:** Envelope Follower 2 — band 2, attack 8 ms, release 250 ms, gain 0 dB.

---

### 16 — Rap Vocal Edge
*Forward, dense and mid-heavy; built to stay on top of a loud beat without riding the fader.*
`Vocals/16_Rap_Vocal_Edge.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 180 | Warm Tube | 5 | 30 | −1.0 | 0 | 100 | 0 | — | +25 | −2.0 | 0 | 0 |
| 2 | 180 – 3000 | Transformer | 12 | 60 | +0.5 | 0 | 100 | 0 | — | +30 | −1.0 | +2.0 | 0 |
| 3 | > 3000 | Clean Tape | 9 | 45 | 0 | 0 | 100 | 0 | — | −15 | 0 | +1.0 | +1.0 |

**Global:** 3 bands · crossovers 180 / 3000 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 17 — Gritty Lead Vocal
*Audible rock-vocal dirt kept usable by blending it in parallel instead of replacing the clean
signal.*
`Vocals/17_Gritty_Lead_Vocal.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 300 | Warm Tube | 7 | 35 | 0 | 0 | 100 | 0 | — | +20 | −1.0 | 0 | 0 |
| 2 | > 300 | Crunch | 20 | 100 | −2.0 | 0 | 100 | 0 | — | +10 | −2.0 | +2.0 | −1.0 |

**Global:** 2 bands · crossover 300 Hz · OS 8× · **global mix 45 %** · auto-gain **OFF** ·
Stereo · minimum-phase LR4.

**Modulation**
- Macro 2 "Grit" -> Global mix, +45 %, Linear — one control from "a bit ragged" to "fully torn".

**Source setup:** Macro 2 default 0 %.

---

### 18 — Backing Vocal Bed
*Softens and widens stacked backing vocals so they form a bed behind the lead instead of competing
with it.*
`Vocals/18_Backing_Vocal_Bed.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 600 | Warm Tape | 8 | 45 | −1.0 | 0 | 90 | 0 | — | +30 | −1.0 | 0 | 0 |
| 2 | > 600 | Clean Tape | 10 | 60 | 0 | 0 | 150 | 0 | — | +25 | 0 | +0.5 | +1.0 |

**Global:** 2 bands · crossover 600 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
**stereo mode Mid-Side** (the width settings act on the side signal, which is the point) ·
minimum-phase LR4.

---

## 4. Guitar

### 19 — Clean Electric Sparkle
*Adds body and a top-end shine to clean electric parts without ever crossing into breakup.*
`Guitar/19_Clean_Electric_Sparkle.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 900 | Clean Tube | 6 | 40 | 0 | 0 | 100 | 0 | — | +20 | 0 | 0 | 0 |
| 2 | > 900 | Bright Tape | 9 | 55 | +0.5 | 0 | 110 | 0 | — | 0 | 0 | +1.0 | +1.5 |

**Global:** 2 bands · crossover 900 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 20 — Crunch Rhythm Stack
*Rhythm guitar with a tight low end, aggressive mids, and the fizz pulled out of the top.*
`Guitar/20_Crunch_Rhythm_Stack.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 140 | Clean Tube | 4 | 25 | −1.5 | 0 | 85 | 0 | — | +30 | −2.0 | 0 | 0 |
| 2 | 140 – 1800 | Crunch | 24 | 90 | 0 | 0 | 100 | 0 | — | 0 | −1.0 | +2.0 | 0 |
| 3 | > 1800 | Warm Tape | 12 | 60 | −1.0 | 0 | 100 | 0 | — | −10 | 0 | 0 | −2.0 |

**Global:** 3 bands · crossovers 140 / 1800 Hz · OS 8× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 21 — Singing Lead Sustain
*A lead tone on the edge of feedback: notes bloom and hold instead of dying, and the mod wheel
pushes it over.*
`Guitar/21_Singing_Lead_Sustain.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 500 | Clean Amp | 14 | 70 | −1.0 | 0 | 100 | 0 | — | +20 | −1.0 | +1.0 | 0 |
| 2 | > 500 | Lead | 30 | 100 | 0 | 0 | 100 | 35 | 780 | +45 | −1.0 | +2.0 | −1.0 |

**Global:** 2 bands · crossover 500 Hz · OS 8× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

**Modulation**
- XLFO 1 -> Band 2 feedback frequency, +12 %, SCurve (smoothing 120 ms) — a slow drift, like an
  amp that never quite settles.
- MIDI Source 1 -> Band 2 feedback amount, +40 %, ExpoIn — the mod wheel takes it into howl.

**Source setup:** XLFO 1 — free, 0.18 Hz, 3-point triangle, depth 100 %, phase 0°, smooth 40 %.
MIDI Source 1 — type CC, CC 1 (mod wheel), smoothing 80 ms.

---

### 22 — Acoustic Body and Air
*Opens up a close-miked acoustic: a touch of body, a touch of air, and the mid-range left alone.*
`Guitar/22_Acoustic_Body_and_Air.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 250 | Subtle Tube | 3 | 20 | −0.5 | 0 | 100 | 0 | — | +15 | −1.5 | 0 | 0 |
| 2 | 250 – 4000 | Warm Tube | 6 | 35 | 0 | 0 | 100 | 0 | — | +10 | 0 | +0.5 | 0 |
| 3 | > 4000 | Shimmer | 5 | 30 | 0 | 0 | 110 | 0 | — | −20 | 0 | 0 | +1.5 |

**Global:** 3 bands · crossovers 250 / 4000 Hz · **OS 8×** · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 23 — Amp Room Dirt
*Grubby, mono-leaning amp tone for garage and lo-fi rock parts.*
`Guitar/23_Amp_Room_Dirt.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 700 | Smudge | 18 | 75 | −1.0 | 0 | 90 | 0 | — | +35 | +1.0 | 0 | −1.0 |
| 2 | > 700 | Rectify | 16 | 55 | −1.0 | 0 | 70 | 0 | — | +25 | −2.0 | +2.0 | −3.0 |

**Global:** 2 bands · crossover 700 Hz · **OS 8×** (Rectify is hard-edged; ADAA plus 8× keeps it
under the alias gate) · global mix 100 % · auto-gain **ON** · Stereo · minimum-phase LR4.

---

## 5. Mix Bus

Every preset in this category uses **auto-gain ON** and no destructive styles. Drive stays in the
2–6 dB range and band mix under 35 %: on a mix bus the job is cohesion, not colour.

### 24 — Bus Glue Whisper
*The most conservative preset in the bank — single-band, barely there, safe to leave on.*
`MixBus/24_Bus_Glue_Whisper.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | full range | Subtle Tube | 3 | 22 | 0 | 0 | 100 | 0 | — | +12 | 0 | 0 | 0 |

**Global:** **1 band** (no crossovers) · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 25 — Tape Cohesion
*Pulls a mix together the way a tape pass does: slightly softer peaks, slightly rounder top.*
`MixBus/25_Tape_Cohesion.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 120 | Clean Tape | 4 | 30 | 0 | 0 | 95 | 0 | — | +15 | +0.5 | 0 | 0 |
| 2 | 120 – 5000 | Warm Tape | 5 | 35 | 0 | 0 | 100 | 0 | — | +20 | 0 | 0 | 0 |
| 3 | > 5000 | Clean Tape | 6 | 30 | −0.5 | 0 | 105 | 0 | — | +10 | 0 | 0 | −0.5 |

**Global:** 3 bands · crossovers 120 / 5000 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
Stereo · minimum-phase LR4.

---

### 26 — Console Weight
*Iron-style density across the mix: a small, solid increase in perceived size with no softening.*
`MixBus/26_Console_Weight.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 180 | Transformer | 6 | 35 | 0 | 0 | 95 | 0 | — | +15 | +0.5 | 0 | 0 |
| 2 | > 180 | Transformer | 4 | 25 | 0 | 0 | 100 | 0 | — | +10 | 0 | +0.5 | 0 |

**Global:** 2 bands · crossover 180 Hz · OS 4× · global mix 100 % · auto-gain **ON** · Stereo ·
minimum-phase LR4.

---

### 27 — Wide and Quiet
*Mid/Side bus treatment: the lows are tightened toward the centre and the sides are opened,
with almost no audible saturation.*
`MixBus/27_Wide_and_Quiet.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 300 | Subtle Tube | 2 | 15 | 0 | 0 | 85 | 0 | — | +10 | 0 | 0 | 0 |
| 2 | > 300 | Clean Tube | 5 | 30 | 0 | 0 | 125 | 0 | — | +10 | 0 | 0 | +0.5 |

**Global:** 2 bands · crossover 300 Hz · OS 4× · global mix 100 % · auto-gain **ON** ·
**stereo mode Mid-Side** · minimum-phase LR4.

---

### 28 — Master Polish
*A final-stage preset: four bands, linear-phase crossovers, and the smallest amount of everything.*
`MixBus/28_Master_Polish.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 90 | Clean Tube | 2 | 15 | 0 | 0 | 100 | 0 | — | +5 | +0.5 | 0 | 0 |
| 2 | 90 – 700 | Subtle Tube | 3 | 20 | 0 | 0 | 100 | 0 | — | +8 | 0 | 0 | 0 |
| 3 | 700 – 6000 | Subtle Tube | 2 | 18 | 0 | 0 | 100 | 0 | — | +8 | 0 | +0.5 | 0 |
| 4 | > 6000 | Clean Tape | 3 | 15 | −0.5 | 0 | 100 | 0 | — | +5 | 0 | 0 | −0.5 |

**Global:** 4 bands · crossovers 90 / 700 / 6000 Hz · **crossover mode Linear Phase** ·
OS 8× realtime, **16× offline** · global mix 100 % · auto-gain **ON** · Stereo ·
**dither Triangular**.

*The only preset that enables dither and linear phase.* Both are appropriate here and nowhere
else: linear phase costs latency and pre-ringing, and dither belongs only on a final render.
Ember reports the linear-phase latency through `setLatencySamples`, so the host compensates it.

---

## 6. Creative

This is where the destructive styles and essentially all of the modulation live. Several presets
here deliberately run low or no oversampling — their aliasing is part of the sound, and that is a
decision, not an oversight.

### 29 — Broken Radio
*Squeezes the signal into a narrow, degraded band as if it came through a failing transmitter.*
`Creative/29_Broken_Radio.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 400 | Broken Tube | 20 | 40 | −6.0 | 0 | 60 | 0 | — | −20 | −6.0 | 0 | 0 |
| 2 | 400 – 3000 | Decimate | 18 | 100 | 0 | 0 | 70 | 0 | — | 0 | −3.0 | +4.0 | −2.0 |
| 3 | > 3000 | Bitcrush | 12 | 70 | −5.0 | 0 | 60 | 0 | — | 0 | 0 | 0 | −6.0 |

**Global:** 3 bands · crossovers 400 / 3000 Hz · **OS 2×** (deliberate — Decimate and Bitcrush are
sample-rate and bit-depth destroyers, so their artefacts are the effect) · global mix 100 % ·
auto-gain **OFF** · Stereo · minimum-phase LR4 · output trim +2 dB.

**Modulation**
- XLFO 2 -> Band 2 drive, +35 %, Stepped — a rhythmic stutter in the degradation.
- Macro 3 "Signal Loss" -> Global mix, +100 %, Linear.

**Source setup:** XLFO 2 — sync 1/8, 8-point stepped shape, depth 100 %, phase 0°, smooth 0 %.
Macro 3 default 0 %.

---

### 30 — Ring of Foldback
*A wavefolder on the upper band turns sustained material into a metallic, bell-like ring.*
`Creative/30_Ring_of_Foldback.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 600 | Warm Tube | 8 | 40 | 0 | 0 | 100 | 0 | — | 0 | 0 | 0 | 0 |
| 2 | > 600 | Foldback | 26 | 85 | −4.0 | 0 | 120 | 0 | — | −30 | −2.0 | +2.0 | −3.0 |

**Global:** 2 bands · crossover 600 Hz · **OS 16×** (folding is the worst aliaser in the plugin;
ADAA plus 16× is what keeps it musical) · **global mix 70 %** · auto-gain **OFF** · Stereo ·
minimum-phase LR4.

**Modulation**
- XLFO 1 -> Band 2 drive, +45 %, SCurve — the fold depth breathes in time.
- Envelope Follower 1 -> Band 2 mix, −25 %, Linear — loud passages get less fold, so it stays
  intelligible.

**Source setup:** XLFO 1 — sync 1/4, 4-point triangle, depth 100 %, phase 0°, smooth 25 %.
Envelope Follower 1 — band 2, attack 10 ms, release 200 ms, gain 0 dB.

---

### 31 — Pulse Gate Grind
*Tempo-locked gating built from modulation rather than a gate: the bands are chopped in sixteenths.*
`Creative/31_Pulse_Gate_Grind.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 250 | Hard Clip | 16 | 60 | −3.0 | 0 | 100 | 0 | — | 0 | 0 | 0 | −2.0 |
| 2 | > 250 | Crunch | 28 | 100 | −2.0 | 0 | 110 | 0 | — | 0 | −2.0 | +3.0 | 0 |

**Global:** 2 bands · crossover 250 Hz · OS 8× · global mix 100 % · auto-gain **OFF** · Stereo ·
minimum-phase LR4.

**Modulation**
- XLFO 3 -> Band 2 level, −60 %, Stepped — the chop.
- XLFO 3 -> Band 1 level, −40 %, Stepped — the lows duck less, so the pulse keeps its weight.
- Macro 4 "Chop Depth" -> XLFO 3 depth, +100 %, Linear — a source modulating a source, which the
  dependency-ordered mod graph (D11) evaluates correctly.

**Source setup:** XLFO 3 — sync 1/16, 16-point square/gate shape, depth 60 %, phase 0°,
smooth 5 %. Macro 4 default 40 %.

---

### 32 — Resonant Howl
*Pushes the feedback path close to its limit on both bands for a screaming, resonant texture.*
`Creative/32_Resonant_Howl.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 500 | Warm Tube | 12 | 50 | −2.0 | 0 | 100 | 25 | 90 | +20 | 0 | 0 | 0 |
| 2 | > 500 | Lead | 30 | 90 | −3.0 | 0 | 110 | 65 | 620 | +30 | −3.0 | +3.0 | −2.0 |

**Global:** 2 bands · crossover 500 Hz · OS 8× · **global mix 65 %** · auto-gain **OFF** ·
Stereo · minimum-phase LR4.

**Modulation**
- XY Controller -> Band 2 feedback frequency, +80 %, ExpoIn — sweeping the pad moves the howl.
- Macro 5 "Howl" -> Band 2 feedback amount, +50 %, Linear.

**Source setup:** XY pad at X 35 %, Y 50 %. Macro 5 default 0 %.
The feedback loop is bounded by its internal soft limiter and hard ceiling (D9), so even Macro 5
fully open cannot run away — it gets loud, not unstable.

---

### 33 — Vowel Sweep
*Five bands with two "formant" bands cross-faded by one LFO, giving a talking, vowel-like sweep.*
`Creative/33_Vowel_Sweep.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 250 | Warm Tube | 10 | 50 | −1.0 | 0 | 100 | 0 | — | +10 | 0 | 0 | 0 |
| 2 | 250 – 700 | Rectify | 18 | 75 | −1.0 | 0 | 100 | 0 | — | +15 | −1.0 | +3.0 | −1.0 |
| 3 | 700 – 1800 | Rectify | 20 | 80 | −1.0 | 0 | 100 | 0 | — | +20 | −2.0 | +4.0 | −2.0 |
| 4 | 1800 – 4000 | Smudge | 14 | 60 | −2.0 | 0 | 105 | 0 | — | 0 | −1.0 | +2.0 | −3.0 |
| 5 | > 4000 | Clean Tape | 8 | 35 | −3.0 | 0 | 100 | 0 | — | −10 | 0 | 0 | −2.0 |

**Global:** 5 bands · crossovers 250 / 700 / 1800 / 4000 Hz · OS 8× · **global mix 85 %** ·
auto-gain **OFF** · Stereo · minimum-phase LR4.

**Modulation**
- XLFO 1 -> Band 2 level, +55 %, SCurve
- XLFO 1 -> Band 3 level, −55 %, SCurve — inverse amount on the same source, so the two formant
  bands trade places rather than both rising.
- Envelope Generator 1 -> Band 3 drive, +25 %, ExpoOut — each new note re-bites.
- Macro 6 "Sweep Rate" -> XLFO 1 rate, +50 %, Linear.

**Source setup:** XLFO 1 — sync 1/2, 5-point sine-ish shape, depth 100 %, phase 0°, smooth 50 %.
Envelope Generator 1 — trigger Input Transient, threshold −26 dB, attack 2 ms, decay 120 ms,
sustain 20 %, release 200 ms. Macro 6 default 50 %.

---

### 34 — Bit Rot Texture
*Digital decay: deliberate bit and sample-rate destruction with a random modulator eating at the
top band.*
`Creative/34_Bit_Rot_Texture.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 300 | Broken Tube | 9 | 35 | −1.0 | 0 | 90 | 0 | — | +10 | −2.0 | 0 | 0 |
| 2 | 300 – 2500 | Bitcrush | 14 | 65 | −2.0 | 0 | 100 | 0 | — | 0 | −2.0 | +1.0 | −2.0 |
| 3 | > 2500 | Decimate | 16 | 55 | −4.0 | 0 | 80 | 0 | — | 0 | 0 | 0 | −5.0 |

**Global:** 3 bands · crossovers 300 / 2500 Hz · **OS Off** (deliberate — oversampling a
bitcrusher defeats it) · global mix 100 % · auto-gain **OFF** · Stereo · minimum-phase LR4 ·
output trim +3 dB.

**Modulation**
- XLFO 4 -> Band 3 mix, +50 %, Stepped — random dropouts in the top band.
- Macro 7 "Rot" -> Band 2 drive, +100 %, Linear.

**Source setup:** XLFO 4 — free, 0.7 Hz, 8-point random / sample-and-hold shape, depth 100 %,
phase 0°, smooth 0 %. Macro 7 default 25 %.

---

### 35 — Shimmer Haze
*Wide, airy and slowly moving; turns pads, keys and guitars into a hazy background texture.*
`Creative/35_Shimmer_Haze.xml`

| # | Range (Hz) | Style | Drive (dB) | Mix (%) | Level (dB) | Pan | Width (%) | FB (%) | FB (Hz) | Dyn (%) | Low (dB) | Mid (dB) | High (dB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | < 200 | Subtle Tube | 4 | 25 | −1.0 | 0 | 80 | 0 | — | +25 | −2.0 | 0 | 0 |
| 2 | 200 – 2000 | Breathe | 12 | 55 | 0 | 0 | 130 | 0 | — | +40 | 0 | +1.0 | +1.0 |
| 3 | > 2000 | Shimmer | 16 | 70 | −1.0 | 0 | 160 | 0 | — | −15 | 0 | 0 | +2.0 |

**Global:** 3 bands · crossovers 200 / 2000 Hz · OS 8× · **global mix 75 %** · auto-gain **OFF** ·
Stereo · minimum-phase LR4.

**Modulation**
- XLFO 2 -> Band 3 width, +35 %, SCurve (smoothing 200 ms) — the haze widens and narrows over bars.
- Envelope Follower 3 -> Band 3 mix, −30 %, ExpoOut — busy passages pull the shimmer back so it
  never swamps the source.

**Source setup:** XLFO 2 — sync 4 bars, 4-point triangle, depth 80 %, phase 0°, smooth 80 %.
Envelope Follower 3 — band 2, attack 20 ms, release 500 ms, gain 0 dB.

---

## Coverage checks

These are the reasons to trust that the library is varied rather than 35 restatements of one idea.
They are also worth re-running by eye if presets are re-voiced.

### Every style is used

| Style | Presets |
|---|---|
| Clean Tube | 06, 07, 08, 12, 13, 14, 19, 20, 27, 28 |
| Warm Tube | 01, 02, 07, 08, 10, 14, 16, 17, 22, 30, 32, 33 |
| Subtle Tube | 04, 07, 09, 14, 15, 22, 24, 27, 28, 35 |
| Broken Tube | 29, 34 |
| Clean Tape | 04, 16, 18, 25, 28, 33 |
| Warm Tape | 03, 09, 10, 18, 20, 25 |
| Bright Tape | 01, 02, 07, 19 |
| Transformer | 01, 07, 08, 11, 16, 26 |
| Clean Amp | 13, 21 |
| Crunch | 02, 05, 07, 09, 17, 20, 31 |
| Lead | 05, 13, 21, 32 |
| Smudge | 12, 23, 33 |
| Rectify | 23, 33 |
| Foldback | 30 |
| Hard Clip | 31 |
| Decimate | 29, 34 |
| Bitcrush | 29, 34 |
| Shimmer | 06, 22, 35 |
| Breathe | 15, 35 |

All 19 `StyleID` values appear. Foldback and Hard Clip appear once each by intent — they are the
two most specialised shapers in the plugin and a library that leaned on them would sound broken.

### Every band count is used

| Bands | Presets |
|---|---|
| 1 | 24 |
| 2 | 03, 04, 05, 06, 10, 11, 12, 15, 17, 18, 19, 21, 23, 26, 27, 30, 31, 32 |
| 3 | 01, 02, 08, 09, 13, 14, 16, 20, 22, 25, 29, 34, 35 |
| 4 | 28 |
| 5 | 33 |
| 6 | 07 |

`kMinBands` (1) and `kMaxBands` (6) are both exercised, so the preset bank doubles as a smoke test
for band-count changes and the crossfade in `EmberEngine`.

### Oversampling spread

| Factor | Presets |
|---|---|
| Off | 34 |
| 2× | 03, 29 |
| 4× | 01, 02, 04, 08, 10, 11, 14, 15, 16, 18, 19, 24, 25, 26, 27 |
| 8× | 05, 06, 07, 09, 12, 13, 17, 20, 21, 22, 23, 28, 31, 32, 33, 35 |
| 16× | 30 |

Preset 28 is the only one that sets a different **offline** factor (16×).

### Modulation source coverage

| Source type | Used by |
|---|---|
| XLFO 1–4 | 21 (1), 29 (2), 30 (1), 31 (3), 33 (1), 34 (4), 35 (2) |
| Envelope Generator 1–2 | 02 (1), 33 (1) |
| Envelope Follower 1–4 | 01 (1), 07 (1), 12 (1), 15 (2), 30 (1), 35 (3) |
| XY Controller | 32 |
| MIDI Source 1–4 | 21 (1) |
| Macro 1–8 | 05 (1), 17 (2), 29 (3), 31 (4), 32 (5), 33 (6), 34 (7) |

All six `ModSourceType` values are exercised. 15 of 35 presets ship routings; the heaviest presets
(31 and 33) carry 3 and 4 connections, far below the 64 preallocated slots. Macro 8 is deliberately
left free in every preset so there is always an empty macro to assign.

### Category rules honoured

- Mix Bus: all five presets have auto-gain **ON**, drive ≤ 6 dB, band mix ≤ 35 %, and use no
  destructive style.
- Creative: all seven have auto-gain **OFF** (output level is a creative choice there, and
  auto-gain would fight the deliberate level drops), and every destructive style in the plugin
  appears in this category. The only two non-Creative presets with auto-gain off are 05 and 17,
  both of which are parallel-blend presets where the same reasoning applies.
- No preset exceeds any parameter range; every feedback frequency lies inside its own band.

---

## Notes for whoever generates the XML

1. **Wait for the parameter layout to freeze.** These presets are written against the IDs in
   `src/plugin/ParameterIDs.h` as they stand, but the values above are stated in *engineering
   units* (dB, %, Hz), not normalised 0–1. Convert through each parameter's own
   `NormalisableRange` rather than assuming a linear mapping — `drive`, `feedbackFreq` and the
   crossover frequencies are near-certainly skewed ranges.
2. **Check the dynamics sign convention** (see Conventions §1) against the final `Dynamics`
   implementation before generating. If it is inverted, negate every `Dyn` value; do not re-voice.
3. **`StyleID` order is append-only** per `EmberTypes.h`. Presets store the style *value*, so a
   renumbering would silently change what every preset sounds like.
4. **The XY Controller is a single flat modulation source** (`kNumXYControllers == 1`), so preset
   32 routes it once. If the engine ends up exposing X and Y as two separate flat sources, that
   preset can be extended with a Y-axis routing to Band 2 feedback amount and Macro 5 freed up.
5. **Match the existing XML shape.** `resources/presets/Init.xml` already establishes the format,
   and this manifest defers to it rather than proposing a competing one:

   ```xml
   <EMBER_PRESET name="Kick Weight and Click" category="Drums" pluginVersion="1.0.0">
     <!-- APVTS state tree, then the modulation connection list -->
   </EMBER_PRESET>
   ```

   Use the exact category strings from the table above — `Drums`, `Bass`, `Vocals`, `Guitar`,
   `Mix Bus`, `Creative` — so the browser can group them. (`Init.xml` uses `category="Factory"`
   because it is not part of the six-category library.)
6. **The generator does not exist yet.** `resources/presets/Init.xml` names it
   `ember_makepresets`, but there is no such CMake target in this repository — the only
   executables the build defines are `ember_tests`, `ember_benchmark` and `ember_render`. Keep
   that name when it is written, so the two files agree. If it is taught to parse the per-band
   tables directly, the column order here is fixed and machine-readable: band index, range, style,
   drive, mix, level, pan, width, feedback, feedback frequency, dynamics, low, mid, high. The
   `Range (Hz)` column is derived from the crossover list and should be ignored on read;
   `—` appears only in `FB (Hz)`.
7. **The category subdirectories need a build change to be picked up.** This manifest writes each
   preset to `resources/presets/<Category>/<NN>_<Name>.xml`, but the top-level `CMakeLists.txt`
   collects presets with a non-recursive `file(GLOB … "resources/presets/*.xml")` into
   `ember_resources`. As it stands, nothing inside `Drums/`, `Bass/`, `Vocals/`, `Guitar/`,
   `MixBus/` or `Creative/` would be compiled in, so `PresetManager::loadFactoryPresets()` would
   find only `Init.xml`. Whoever generates the XML must either switch that glob to `GLOB_RECURSE`
   (plus `resources/presets/*/*.xml`) or write all 35 files flat into `resources/presets/`. The
   flat layout still works, because the browser groups on the `category` XML attribute, not on the
   directory.
