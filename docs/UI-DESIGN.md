# Ember — UI Design

The presentation layer, specified before it is built. Everything here is
binding on `src/gui/`; where this document and the code disagree, the code is
wrong.

Art direction in one line: **a hot tube unit photographed at night**. Heat is
the metaphor — the harder a band is driven, the more that part of the interface
glows. At rest the plugin is nearly monochrome.

Anti-goals, stated so they can be checked against a screenshot: flat corporate
grey, hairline web-dashboard chrome, stock JUCE rotaries, skeuomorphic clutter,
rainbow accents.

---

## 1. Colour tokens

Defined once in `src/gui/EmberTheme.h` as `EmberTheme::Tokens`. **No colour
literal may appear anywhere else in `src/gui/`** — there is a unit test that
greps for `Colour (0x` and `fromRGB` outside the theme file and fails the build
if it finds one.

### Chassis and panels

| Token | Hex | Use |
|---|---|---|
| `bgDeep` | `#0B0B0D` | window background; near-black, very slightly warm |
| `panel` | `#15151A` | recessed panel surface |
| `panelRaised` | `#1D1D24` | raised module / control surface |
| `panelEdge` | `#2A2A33` | 1 px top bevel highlight |
| `panelShadow` | `#050507` | bottom shadow edge |

> The source prompt writes the shadow as `#05050 7`; read as `#050507`.

### Text

| Token | Hex | Use | Contrast on `panel` |
|---|---|---|---|
| `text` | `#E8E4DC` | values, primary labels | 13.9:1 |
| `textDim` | `#8A8780` | captions, units, axis labels | 5.4:1 |
| `textMuted` | `#55534E` | grid lines, disabled, decorative | 2.3:1 — **decorative only, never text** |

`textMuted` fails 4.5:1 by design; the contrast test asserts it is never passed
to `drawText`, only to grid and separator strokes.

### Warm — audio

| Token | Hex | Use |
|---|---|---|
| `ember` | `#FF7A1A` | primary accent |
| `emberHot` | `#FFB347` | maximum drive / glow peaks |
| `emberDeep` | `#B3420B` | low-drive, cooled state |
| `tubeGlow` | `#FF5A00` | bloom layers, 0–60 % alpha |
| `signal` | `#F2C14E` | meters, spectrum trace at moderate level |

### Cool — modulation

| Token | Hex | Use |
|---|---|---|
| `cold` | `#5D82AA` | modulation sources, connections, mod arcs |

> The brief specifies `#5A7FA8`. That measures **4.36:1** on `panel` and so
> fails the brief's own §6 requirement that every colour clear 4.5:1 for text.
> Lifted 1 % in lightness — same hue, same saturation — to 4.54:1. The
> contrast test would otherwise have to be weakened to accept it, and a
> failing accessibility gate is worse than a 1 % colour shift.

### Alarm

| Token | Hex | Use |
|---|---|---|
| `danger` | `#FF3B30` | clip indicators only |

**The rule: warm = audio, cool = modulation, red = clipping. No other hues.**

### What this replaces

The current palette gives each band its own hue — a flame ramp from deep red
(band 1) to blue-white (band 6). That is deliberately discarded. Under the new
system **band identity comes from number and position, not hue**; the only thing
that tints a band is its heat. A blue-white band 6 would also break the
warm/cool rule, since cool now means modulation.

### Accent variant

`Cool Ember` shifts `ember` → `#FFB347` and `emberDeep` → `#C4761A`, leaving
every other token alone. It exists to prove the token indirection is real: if
any component hard-codes a colour, switching variants will expose it
immediately.

It did exactly that. The first migration made `EmberColours` a set of
`static const juce::Colour` members initialised from the tokens, which meant
they captured the default palette once at load: rendering the variant showed
knob arcs and heat bars shifting to amber-gold while the band chips, COMPARE
and Mod buttons stayed orange. They are accessor functions now, reading the
live tokens on every call, and the whole interface follows the variant.

---

## 2. Spacing, radius, stroke

Spacing scale — **4 / 8 / 12 / 16 / 24 / 32**. No other gaps.

| Purpose | Value |
|---|---|
| Control-to-label | 4 |
| Within a control group | 8 |
| Between control groups | 12 |
| Panel inner padding | 16 |
| Between major regions | 8 |
| Module inner padding | 12 |
| Corner radius, panels | 6 |
| Corner radius, controls | 4 |
| Corner radius, pills | height / 2 |
| Bevel highlight | 1 px |
| Panel drop shadow | 8–12 px, offset y +2 |
| Hairline / separator | 1 px |
| Knob arc track | 3 px |
| Knob arc fill | 3 px + 2 px glow |

---

## 3. Typography

Two bundled open-licence families, embedded as `BinaryData`:

| Role | Family | Size @ 1.0 | Tracking | Case |
|---|---|---|---|---|
| Logo | Barlow Condensed SemiBold | 22 | +2 % | upper |
| Section header | Barlow Condensed Medium | 11 | +8 % | upper |
| Label | Inter Medium | 10.5 | +4 % | upper |
| Value | Inter Medium, tabular | 12 | 0 | as-is |
| Large readout | Inter SemiBold, tabular | 16 | 0 | as-is |
| Footer / CPU | Inter Medium, tabular | 11 | 0 | as-is |

Never below 10 px at any zoom. All numerals tabular so values do not jitter
while dragging.

Labels sit **below** knobs. The live value appears in a floating pill **above**
the knob while dragging, and only while dragging.

Both families ship in `resources/fonts/` and are embedded through
`juce_add_binary_data`, licences included as the OFL requires: Inter 4.1
Medium/SemiBold and Barlow Condensed Medium/SemiBold, 1.0 MB in total.
`EmberFonts::usingEmbeddedFaces()` reports whether they loaded, and a test
fails if they did not — a silent fallback to system faces would quietly undo
the reason for embedding them.

Tabular figures are real, not aspirational: JUCE 8 exposes OpenType features,
so value and readout roles enable `tnum`, and `test_theme.cpp` proves it by
measuring that "111111" and "888888" render to the same width.

---

## 4. Layout

Default **1180×700**, minimum **860×510**, aspect locked at **1180 : 700**
(1.6857), zoom menu at 75 / 100 / 125 / 150 / 200 %.

> The prompt asks for a 860×520 minimum *and* a locked aspect; those are
> incompatible (860 at the default aspect is 510.2 tall). Width is taken as
> binding, so the minimum is 860×510 — 10 px shorter than requested, 2 % off.
> The other two test sizes already sit on the locked aspect: 1920×1140 is
> 1.6842, within half a percent.

Region heights: header and footer are fixed; the three middle regions split
whatever remains in the ratio **38 : 34 : 18**.

```
                                         1180×700   1920×1140   860×510
  HEADER                     fixed            52          52        52
  DISPLAY                    38/90           255         441       178
  BAND STRIP                 34/90           228         394       159
  MOD RAIL    expanded       18/90           121         209        85
              collapsed      fixed            36          36        36
  FOOTER                     fixed            44          44        44
```

With the mod rail collapsed, display and band strip split the freed space in
the same 38:34 ratio.

### 1180×700 — default, mod rail collapsed

```
┌────────────────────────────────────────────────────────────────────────────┐
│ ◆EMBER   ‹ Broken Radio          › ⬤  A B ⧉  ↺ ↻   HQ ⬤ LIN ○ M/S ○  ⚙ 100%│ 52
├────────────────────────────────────────────────────────────────────────────┤
│▮  0 ┌──────────────┬──────────────────┬────────────────────────────┐  0  ▮ │
│▮ -20│      ◇       │        ◇         │                            │ -20 ▮ │
│▮ -40│   band 1     │     band 2       │        band 3              │ -40 ▮ │
│▮ -60│   ░░░░░░     │   ▒▒▒▒▒▒▒▒       │      ▓▓▓▓▓▓▓▓▓▓            │ -60 ▮ │
│▮ -80└──────────────┴──────────────────┴────────────────────────────┘ -80 ▮ │ 299
│ IN    30      100      300     1k      3k      10k                    OUT  │
├────────────────────────────────────────────────────────────────────────────┤
│ ┌─ 1 ─ Broken Tube ──── ⬤ ⬤ ─────────────┐ ┌─ 2 ─ Warm Tape ─┐ ┌─ 3 ─ ... ┐│
│ │  ( DRIVE )   ( MIX )   ( LEVEL )       │ │    ( DRIVE )    │ │ ( DRIVE )││
│ │   20.0 dB     40 %     -6.0 dB         │ │     ▁▃▅ heat    │ │  ▁ heat  ││ 269
│ │  (fb)(fq)(dy)(lo)(md)(hi)(pan)  ╭────╮ │ │                 │ │          ││
│ └────────────────────────────────╰curve╯─┘ └─────────────────┘ └──────────┘│
├────────────────────────────────────────────────────────────────────────────┤
│ › MODULATION   2 routings                               [ Sources ][Matrix ]│ 36
├────────────────────────────────────────────────────────────────────────────┤
│ IN ▬▬▬▬▬▬○▬▬ -0.0   OUT ▬▬▬▬○▬▬▬▬ +2.0   MIX ▬▬▬▬▬▬▬▬○ 100%  AG ⬤ │ 2.1% 4smp│ 44
└────────────────────────────────────────────────────────────────────────────┘
```

### 860×510 — minimum

Header collapses: the logo loses its wordmark and keeps the filament glyph;
HQ / LIN / M/S become a single overflow button. The band strip drops to Drive +
heat bar only for unselected modules; the selected module keeps its knob grid
but loses the transfer-curve viewer below 900 px wide.

```
┌──────────────────────────────────────────────────────────┐
│ ◆  ‹ Broken Radio      › A B  ↺ ↻          ⋯    ⚙   75% │ 52
├──────────────────────────────────────────────────────────┤
│▮ ┌──────────┬────────────┬──────────────────────┐      ▮ │
│▮ │    ◇     │     ◇      │                      │      ▮ │ 178
│▮ │  ░░░░    │  ▒▒▒▒▒▒    │    ▓▓▓▓▓▓▓▓          │      ▮ │
│  30    100    300    1k     3k    10k                    │
├──────────────────────────────────────────────────────────┤
│ ┌─ 1 ─ Broken Tube ── ⬤⬤ ─┐ ┌─ 2 ──┐ ┌─ 3 ──┐            │
│ │ (DRIVE) (MIX) (LEVEL)   │ │(DRV) │ │(DRV) │            │ 159
│ │ (fb)(fq)(dy)(lo)(md)(hi)│ │ ▁▃▅  │ │  ▁   │            │
│ └─────────────────────────┘ └──────┘ └──────┘            │
├──────────────────────────────────────────────────────────┤
│ › MODULATION  2                          [Src][Mtx]      │ 36
├──────────────────────────────────────────────────────────┤
│ IN ▬▬○▬ OUT ▬▬○▬ MIX ▬▬▬○ AG⬤              │ 2.1%  4smp  │ 44
└──────────────────────────────────────────────────────────┘
```

### 1920×1140 — mod rail expanded

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ ◆EMBER      ‹ Broken Radio ›  ⬤   save  ▤     A B ⧉   ↺ ↻    HQ⬤ LIN○ M/S○   ⚙ 150% │ 52
├──────────────────────────────────────────────────────────────────────────────────────┤
│▮   0 ┌────────────┬──────────────┬─────────────────┬──────────────────────────┐ 0  ▮ │
│▮ -20 │     ◇      │      ◇       │       ◇         │                          │-20 ▮ │
│▮ -40 │  band 1    │   band 2     │    band 3       │      band 4              │-40 ▮ │ 441
│▮ -60 │  ░░░░░     │  ▒▒▒▒▒▒▒     │   ▓▓▓▓▓▓▓▓      │     ▒▒▒▒▒                │-60 ▮ │
│▮ -80 └────────────┴──────────────┴─────────────────┴──────────────────────────┘-80 ▮ │
│ IN      30       100       300      1k       3k       10k                       OUT  │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ ┌─ 1 ─ Broken Tube ─ ⬤⬤ ────────────────┐ ┌─ 2 ─┐ ┌─ 3 ─┐ ┌─ 4 ─┐ ┌ + ┐             │
│ │ (  DRIVE  ) ( MIX ) ( LEVEL )         │ │(DRV)│ │(DRV)│ │(DRV)│ │ghst│             │ 394
│ │ (fb)(fq)(dy)(lo)(md)(hi)(pan) ╭─────╮ │ │ ▁▃▅ │ │  ▁  │ │ ▃▅▇ │ │    │             │
│ └───────────────────────────────╰curve╯─┘ └─────┘ └─────┘ └─────┘ └────┘             │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ ⌄ MODULATION   ┌ XLFO 1 ╌╌╌╌ ┐ ┌ EG 1 ◺ ┐ ┌ FOLLOW ▂▄▆ ┐ ┌ XY ┐ ┌ MACRO ┐  [Src][Mtx]│ 209
│                └ ∿∿∿∿ drag ─┘ └────────┘ └───────────┘ └────┘ └───────┘             │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ IN ▬▬▬▬▬▬○▬▬▬ -0.0 dB   OUT ▬▬▬▬▬○▬▬▬▬ +2.0 dB   MIX ▬▬▬▬▬▬▬▬▬○ 100 %   AG ⬤ │ 2.1 % · 4 smp │ 44
└──────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 5. Component inventory

### Foundation — `EmberTheme.h`, `EmberLookAndFeel`

| Component | Replaces | Notes |
|---|---|---|
| `EmberTheme::Tokens` | `EmberColours` | the only place a colour literal may appear |
| `EmberTheme::variant` | — | `Default` \| `CoolEmber` |
| `EmberFonts` | existing | two families, tabular figures, tracking per role |
| `TextureCache` | — | brushed-metal noise, generated once, ~6 % opacity |
| `GlowCache` | — | Gaussian-blurred sprites at init; **never blur per frame** |
| `EmberLookAndFeel` | existing | rebuilt against tokens |

### Controls — `Widgets.h`

| Component | State |
|---|---|
| `EmberKnob` | rebuilt: machined cap, 270° recessed arc, bipolar fill from centre, 2 px glow, drag pill, double-click reset, shift-fine, alt-drag mod amount, scroll |
| `ModulatableKnob` | kept; gains the cold mod arc + moving dot |
| `EmberLED` | new: recessed window, amber filament, 120 ms fade |
| `EmberSwitch` | rebuilt as backlit switch |
| `EmberComboBox` | rebuilt: grouped headers, style waveform icons, filament mark on current |
| `EmberFader` | new: recessed track, ember fill, machined cap |
| `EmberButton` | rebuilt: flat uppercase, hover glow, 1 px press inset |
| `HeatBar` | new: per-band heat strip for collapsed modules |
| `TransferCurveView` | new: live input/output waveshape of the current style at the current drive |
| `SegmentedMeter` | rebuilt `LevelMeter`: segmented, warm gradient, latching clip cap |

Tooltips are **disabled globally**; the footer line carries the description of
whatever is under the mouse. Scrollbars are 4 px, warm on hover.

### Regions

| Region | New class | From |
|---|---|---|
| Header | `HeaderBar` | merges `PresetBar` + `GlobalBar` |
| Display | `DisplayPanel` | `SpectrumDisplay` + edge meters |
| Band strip | `BandStrip` + `BandModule` | `BandPanel` becomes one module, instantiated per band |
| Mod rail | `ModRail` | `ModPanel` |
| Footer | `FooterBar` | new |

`BandPanel` already builds a full control set for all six bands and
shows/hides them, so becoming a per-band module is a re-parenting job rather
than a rewrite.

---

## 6. Heat

Heat is a per-band 0–1 value: how much harmonic energy the band is adding,
smoothed for the eye.

```
heatRaw   = normalise (outputRmsInBand_dB − inputRmsInBand_dB, 0 … 12 dB)
          × driveWeight (drive parameter, 0 … 1)
heatSmoothed = attack 30 ms, release 400 ms
```

**This needs a measurement the DSP does not currently take.** `EmberEngine`
publishes `bandLevels` (post-band peak) but nothing pre-band, so the difference
cannot be formed in the GUI. `BandChain` therefore gains a pre/post RMS pair
published through `std::atomic<float>`, written once per block.

That is a measurement-only addition: no sample is altered, no branch is added
to the processing path, and the atomics are `relaxed` stores of values the
chain already has in registers. The DSP, parameters, state, presets and
modulation engine are otherwise untouched, per the brief. The smoothing is done
on the **GUI** side, in the 60 Hz tick, so the audio thread stores a raw ratio
and nothing more.

What heat drives:

| Element | At heat 0 | At heat 1 |
|---|---|---|
| Spectrum band region | `emberDeep` @ 8 % | `emberHot` @ 34 %, glow radius ×3 |
| Drive knob inner ring | unlit | bright, soft bloom behind cap |
| Module title bar | flat `panelRaised` | faint warm gradient |
| Style icon | `textDim` | `ember` |
| Heat bar (collapsed module) | 1 px `emberDeep` | full-height `emberHot` |
| Logo filament (global) | dim | lit |
| Window vignette (global) | none | very subtle warm wash |

Global heat is the level-weighted mean of the active bands.

---

## 7. Motion

One `juce::VBlankAttachment` drives every animation, meter and heat update.
There is no second timer anywhere in the GUI.

| Transition | Duration | Curve |
|---|---|---|
| Module expand / collapse | 180 ms | ease-out cubic |
| Hover in / out | 120 ms | ease-out cubic |
| LED on / off | 120 ms | linear |
| Value pill fade | 120 ms | ease-out |
| Heat | 30 ms attack / 400 ms release | one-pole |

Nothing exceeds 250 ms. The only idle animation is meters, heat and the logo
filament. **Reduce motion** in settings disables bloom and all transitions,
leaving meters and values live.

---

## 8. Performance rules

- Zero allocation in `paint()` on the spectrum and knob paths. Paths, gradients
  and glow sprites are built once and reused; `Path` objects live as members.
- Glow is composited from pre-blurred sprites. No per-frame blur, ever.
- Brushed texture generated once at startup into an `Image`, tiled.
- Repaints coalesce into dirty regions; a knob repaints its own bounds, not the
  module.
- Budget: idle GUI ≤ 2 % of one core, spectrum + meters ≤ 5 %.
- The spectrum FFT is already skipped when no editor is open; that stays.

## 9. Accessibility

- Every control keyboard reachable; focus ring drawn in `ember`.
- A unit test walks the token table and asserts ≥ 4.5:1 for every
  text-on-surface pair that the UI actually uses, and asserts `textMuted` is
  never used for text.
- Minimum 10 px type at every zoom level.
- Dark only. There is no light theme — it is a tube unit.
