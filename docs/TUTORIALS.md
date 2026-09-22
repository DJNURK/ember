# Ember — In-plugin help

The plan for the tutorial system, written before it is built.

Users should be able to learn every feature without leaving the plugin. That
means three things, in descending order of how often they are needed: a line of
text explaining whatever is under the pointer, an article you can look up, and a
guided tour that walks you through a workflow.

The first already exists — the footer hint line. This document covers the other
two.

---

## 1. Shape of the thing

```
TourEngine ── runs one tour at a time
  ├── Spotlight     dims everything except the target
  ├── CoachCard     title, body, step counter, Back / Next / Exit
  └── StepAction    watches for the user doing the thing

TourLibrary ── the tours, loaded from JSON in BinaryData
LearnPanel  ── Tours · Reference · Shortcuts · About
Reference   ── articles, one per parameter kind, from a single Markdown file
```

The engine knows nothing about Ember's controls. It is given a target name, a
card to show and optionally an action to wait for; everything specific to this
plugin lives in the JSON and in `TourTargets.h`.

---

## 2. Target names

A tour is data, so it needs a vocabulary of names that survives a layout change.
`src/gui/tutorial/TourTargets.h` is that vocabulary; components claim a name
with `setComponentID` and the engine finds them by walking the editor's tree.

Names are dotted, lowercase, most general part first — `band.3.drive`,
`header.preset`, `display.crossover.1`. Indices are **one-based**, because they
are one-based on screen and a tour is written by someone reading the interface
rather than the source.

**A tour pointing at a name nothing claims fails silently** — the spotlight
lands nowhere and the step looks broken rather than missing. `test_tutorials`
therefore constructs a real editor and asserts every target in every shipped
tour resolves to a component that exists.

---

## 3. Steps

A step is passive or active.

**Passive** shows a card and waits for Next. Use it to explain something that is
already on screen.

**Active** additionally watches for the user doing a thing, lights a small LED
when they do, and advances after 600 ms. Use it when doing beats reading.

| Action | Completes when |
|---|---|
| `parameterAtLeast` | the named parameter reaches a value |
| `parameterChanged` | the named parameter moves at all |
| `styleSelected` | any band's style changes |
| `bandAdded` | the band count rises |
| `modulationConnected` | a routing is created |
| `presetLoaded` | a preset is loaded |
| `clicked` | the target component is clicked |

Every active step carries a **"Do it for me"** link that performs the action, so
nobody is ever stuck. That link is the only thing in the system that writes a
parameter — a tour otherwise observes and never touches the audio.

### The state a tour starts from

Tours run on whatever the user already has loaded. That is deliberate: a tour
that resets your session to teach you something has taken something from you.

A tour that cannot demonstrate its point from an arbitrary state warns once and
offers to load a demo preset, snapshotting the current state and restoring it on
exit.

---

## 4. Presentation

**Spotlight** dims everything except the target to 60 %, with a feathered edge,
a thin ember outline and a subtle pulse. Reduce-motion drops the pulse, not the
outline — the outline is the information.

**Coach card** is 300 px wide, a raised panel with title, body, step counter
("3 / 8"), and Back · Next · Exit. It positions itself beside the target without
covering it and without leaving the window, and repositions on resize.

Keyboard: `→` / `Enter` next, `←` back, `Esc` exit. The card's text is exposed
through `AccessibilityHandler`, because a tour that only works for people who
can see the spotlight is not a help system.

---

## 5. Reference articles

210 parameters, but only **54 distinct kinds** — `band3Drive` and `band5Drive`
want the same article with a different number in it. Articles are keyed by kind,
and the test asserts that every one of the 210 resolves to one.

An article is short by design: one to three sentences of *what it does*, one or
two of *when to use it*, and a "Show me" link into the relevant tour step.

`resources/help/reference.md` is the single source. `scripts/gen-docs.py`
generates both the in-plugin text and the manual's parameter tables from it, and
CI fails if they are out of date — the in-plugin help and the manual cannot be
allowed to drift, because the one people read is whichever they happen to open.

---

## 6. Settings that persist

Per-user, not per-session: whether the welcome card has been dismissed, and how
far through each tour the user got.

| Platform | Path |
|---|---|
| macOS | `~/Library/Application Support/EmberAudio/Ember/settings.json` |
| Windows | `%APPDATA%/EmberAudio/Ember/settings.json` |
| Linux | `~/.config/EmberAudio/Ember/settings.json` |

"Don't show again" is permanent. A welcome card that returns after being
dismissed is not a welcome, it is a nag.

---

## 7. The eight tours

| Tour | Time | Covers |
|---|---|---|
| Quick start | 2 min | load a preset, select a band, Drive, style, Mix, Auto-Gain, A/B |
| Multiband basics | 3 min | add a band, drag a crossover, solo, bypass, linear phase |
| Saturation styles | 3 min | the categories, using the transfer curve to hear the difference |
| Feedback & Dynamics | 2 min | resonant howl, the internal limiter, one-knob compress/expand |
| Tone EQ | 2 min | nodes, pre/post, the overlay, combined response |
| Modulation | 4 min | drag an XLFO onto Drive, amount and curve, sync, the matrix |
| Mixing recipes | 3 min | parallel drum crunch, warm bass, vocal presence |
| HQ & CPU | 1 min | oversampling, the offline setting, latency |

---

## 8. Order of work

1. Target registry and this plan. ← *done*
2. `TourEngine`, `Spotlight`, `CoachCard`, keyboard and accessibility.
3. JSON schema, loader, the seven action types.
4. `LearnPanel` with its four tabs and search.
5. The eight tours, authored against real target ids.
6. `reference.md`, the article-coverage test, `gen-docs.py` and the CI check.
7. Help entry points: the `?` button, first-run welcome, Shift+F1.
8. Screenshots and the eight GIFs.
