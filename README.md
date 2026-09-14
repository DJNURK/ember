# Ember

**Multiband analog saturation and distortion.** VST3, Audio Unit and Standalone, for
macOS, Windows and Linux.

Ember splits your signal into up to **six bands**, runs each one through its own analog-voiced
saturator, and glues the result back together with phase-compensated crossovers so that a
bypassed setting sums back to the input. Every band gets its own character, its own drive, and
its own place in the mix — warm the low end with tape, bite the mids with a cranked amp, and
leave the air untouched.

---

## What it does

- **Up to 6 bands.** Minimum-phase Linkwitz-Riley 4th-order crossovers by default, with
  all-pass compensation so the bands sum flat; an optional **linear-phase** mode for
  mastering work where pre-ringing is a fair trade for zero phase smear.
- **19 saturation styles** across eight families — tube (clean, warm, subtle, broken),
  tape (clean, warm, bright), transformer, amp (clean, crunch, lead), smudge, rectify,
  foldback, hard clip, decimate, bitcrush, and the FX pair shimmer and breathe.
  Styles are gain-matched against each other, so switching style auditions the *character*
  without a jump in level.
- **Per-band controls:** drive (0–40 dB), wet/dry mix, output level, pan, stereo width,
  a resonant **feedback** loop (amount + frequency), bipolar **dynamics** (expander below
  centre, compressor above), a three-band **tone** stack, plus per-band bypass and solo.
- **Drag-and-drop modulation.** 23 sources — 4 multi-point X-LFOs (free or host-synced),
  2 envelope generators, 4 envelope followers, an XY controller, 4 MIDI sources
  (velocity / CC / mod wheel / note) and 8 macros — dragged straight onto any continuous
  control. 64 simultaneous connections, each with its own depth and response curve, and a
  matrix view for editing them all at once.
- **Oversampling up to 16×** (off / 2× / 4× / 8× / 16×), with a separate, higher factor
  available for offline bounces. Hard-edged shapers additionally use antiderivative
  anti-aliasing so they stay clean without brute-forcing the rate.
- **Auto-gain**, stereo or **mid/side** processing, and output dither.
- **Resizable GUI** with a live spectrum display: drag the crossover points directly on the
  analyser, see each band's response, and scale the whole window to taste.

Latency — from oversampling and from linear-phase mode — is always reported to the host, so
your DAW's delay compensation keeps everything in time.

---

## Install

Grab the installer for your platform from the project's Releases page, then follow the
section below.

### Windows

1. Run **`Ember-1.0.0-Windows.exe`** and click through the installer.
2. The VST3 is installed to `C:\Program Files\Common Files\VST3\Ember.vst3`.
3. Start your DAW and rescan your plugins if it does not pick Ember up automatically.

### macOS

1. Open **`Ember-1.0.0-macOS.pkg`**. This installs both formats:
   - VST3 → `/Library/Audio/Plug-Ins/VST3/Ember.vst3`
   - Audio Unit → `/Library/Audio/Plug-Ins/Components/Ember.component`
2. **If the build you downloaded is unsigned**, macOS Gatekeeper will refuse to open it with
   a message like *"Ember-1.0.0-macOS.pkg cannot be opened because it is from an
   unidentified developer."* Two ways around it:

   **The easy way — right-click to open.** In Finder, **right-click** (or Control-click) the
   `.pkg` file, choose **Open** from the menu, and then click **Open** again in the dialog
   that appears. Double-clicking will *not* work; you have to use the right-click menu, which
   is what tells macOS you meant to run it.

   **If the plugins are installed but your DAW still refuses to load them**, strip the
   quarantine flag from the installed bundles in Terminal:

   ```sh
   sudo xattr -cr "/Library/Audio/Plug-Ins/VST3/Ember.vst3"
   sudo xattr -cr "/Library/Audio/Plug-Ins/Components/Ember.component"
   ```

   Enter your macOS password when prompted (nothing appears on screen as you type — that's
   normal). If you installed to your user folder instead of system-wide, use
   `~/Library/Audio/Plug-Ins/VST3/Ember.vst3` and
   `~/Library/Audio/Plug-Ins/Components/Ember.component` and drop the `sudo`.

3. Audio Units are cached by macOS. After a fresh install, quit and reopen Logic / GarageBand
   so the unit is re-validated.

### Linux

1. Extract the archive and run the installer script:

   ```sh
   tar -xzf Ember-1.0.0-Linux.tar.gz
   cd Ember-1.0.0-Linux
   ./install.sh
   ```

2. `install.sh` copies `Ember.vst3` into **`~/.vst3/`**, the standard per-user VST3 folder.
   To install for every user on the machine instead, copy it to `/usr/local/lib/vst3/`
   yourself:

   ```sh
   sudo cp -r Ember.vst3 /usr/local/lib/vst3/
   ```

3. Rescan plugins in your DAW. If your host does not search `~/.vst3`, add that folder to its
   plugin search paths.

---

## Build from source

### Prerequisites

- **CMake 3.22 or newer**
- A **C++20 compiler** — Xcode 14+/AppleClang on macOS, Visual Studio 2022 on Windows,
  GCC 11+ or Clang 14+ on Linux
- **Git** (CMake uses it to fetch dependencies)
- **Ninja** for the macOS and Linux presets (the Windows presets use the Visual Studio
  generator)
- On **Linux**, the JUCE development packages:

  ```sh
  sudo apt install build-essential cmake ninja-build pkg-config \
       libasound2-dev libjack-jackd2-dev \
       libfreetype-dev libfontconfig1-dev \
       libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
       libxcursor-dev libxcomposite-dev libxrender-dev \
       libglu1-mesa-dev mesa-common-dev
  ```

  (Ember builds with `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`, so the WebKit and libcurl
  development packages are *not* required.)

**JUCE is fetched automatically.** CMake pulls JUCE 8.0.15 and Catch2 v3.16.0 with
`FetchContent` during the configure step — you do not need to install or clone them
yourself. Budget for it: the first configure downloads a few hundred megabytes and can take
several minutes on a slow connection. Later configures reuse the cached checkout under
`build/<preset>/_deps/`.

### One command

```sh
./scripts/build.sh            # macOS and Linux
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1    # Windows
```

Each script picks the right preset for the machine it is running on, configures, and builds a
Release binary. That is all most people need.

### Driving CMake directly

```sh
cmake --preset macos-release          # configure
cmake --build --preset macos-release  # build
ctest --preset macos-release          # run the unit tests
```

Available presets (each configure preset has a matching build preset of the same name):

| Preset | What it is |
|---|---|
| `macos-release` | macOS Release, host architecture |
| `macos-universal` | macOS Release, universal arm64 + x86_64 |
| `macos-debug` | macOS Debug, host architecture |
| `linux-release` | Linux Release |
| `linux-asan` | Linux RelWithDebInfo with Address and UB sanitizers |
| `linux-debug` | Linux Debug |
| `windows-release` | Windows, Visual Studio 2022, x64, Release |
| `windows-debug` | Windows, Visual Studio 2022, x64, Debug |

Test presets exist for `macos-release`, `linux-release`, `linux-asan` and `windows-release`.

CMake options, all settable with `-D` at configure time:

| Option | Default | Effect |
|---|---|---|
| `EMBER_BUILD_TESTS` | `ON` | Build the Catch2 unit tests (`ember_tests`) |
| `EMBER_BUILD_BENCHMARK` | `ON` | Build the offline CPU benchmark (`ember_benchmark`) |
| `EMBER_BUILD_AUV3` | `OFF` | Also build the AUv3 app extension |
| `EMBER_UNIVERSAL` | `OFF` | macOS: build a universal arm64 + x86_64 binary |
| `EMBER_ENABLE_ASAN` | `OFF` | Build with Address/UB sanitizers (not MSVC) |

### Where the built plugins land

```
build/<preset>/Ember_artefacts/<config>/VST3/Ember.vst3
build/<preset>/Ember_artefacts/<config>/AU/Ember.component        (macOS only)
build/<preset>/Ember_artefacts/<config>/Standalone/Ember.app      (.exe / binary elsewhere)
```

Copy or symlink the VST3 into your system plugin folder to try a local build in a DAW.

---

## Tests, benchmark and validation

**Unit tests** — Catch2, run through CTest. Every `TEST_CASE` is registered individually, so
you can run a subset:

```sh
ctest --preset macos-release                      # everything
ctest --preset macos-release -R crossover         # just the crossover tests
./build/macos-release/tests/ember_tests           # or run the binary directly
```

**CPU benchmark** — an offline realtime-factor report across band counts, oversampling
factors, sample rates and buffer sizes. It is not a unit test; it prints numbers and fails
only if the engine cannot keep up with realtime:

```sh
./build/macos-release/tests/ember_benchmark
```

**Offline renders** — `ember_render` pushes fixed test signals through the engine and writes
WAVs plus a peak/RMS table, so the manual checklist can be done against real audio:

```sh
./build/macos-release/tests/ember_render test-renders
```

**pluginval** — Tracktion's plugin validator, at maximum strictness. The script fetches
pluginval v1.0.4 into `build/tools/` on first run, stages the Audio Unit where macOS can find
it, and validates every format it finds:

```sh
./scripts/run-pluginval.sh
```

Override the defaults with environment variables: `STRICTNESS=10` (the default),
`REPEAT=5` to catch intermittent failures, `PLUGINVAL_VERSION=v1.0.4`.

**Syntax check** — `scripts/syntax-check.sh src/dsp/Crossover.cpp` compiles a single
translation unit with the real build flags and no linking. Handy for a fast iteration loop
while the tree is mid-change.

**Manual checklist** — host and hardware behaviour that no automated test covers lives in
[`docs/TESTING.md`](docs/TESTING.md). Work through it before tagging a release.

---

## Repository layout

```
.
├── CMakeLists.txt          top-level build: options, juce_add_plugin(Ember)
├── CMakePresets.json       the configure / build / test presets listed above
├── CHANGELOG.md            release history, Keep a Changelog format
├── LICENSE                 GNU GPL v3
├── cmake/
│   └── Dependencies.cmake  pinned FetchContent declarations (JUCE, Catch2)
├── docs/
│   ├── PLAN.md             milestones and architecture
│   ├── STATUS.md           current milestone state
│   └── TESTING.md          the manual release checklist
├── packaging/
│   ├── windows/            Windows installer sources
│   ├── macos/              .pkg sources
│   └── linux/              tarball + install.sh sources
├── resources/
│   ├── fonts/  icons/      GUI assets compiled in as binary data
│   └── presets/            factory presets
├── scripts/
│   ├── build.sh            one-command build (macOS / Linux)
│   ├── build.ps1           one-command build (Windows)
│   ├── run-pluginval.sh    fetch + run pluginval at strictness 10
│   └── syntax-check.sh     single-file syntax check with real build flags
├── src/
│   ├── CMakeLists.txt      ember_dsp static library + the Ember plugin target
│   ├── dsp/                the audio engine — crossovers, oversampling, band chain
│   │   ├── styles/         the 19 saturation shapers, grouped by family
│   │   └── modulation/     control-rate modulation sources and routing graph
│   ├── gui/                editor components, spectrum display, mod matrix view
│   └── plugin/             AudioProcessor, editor shell, parameter definitions
├── tests/                  Catch2 unit tests, benchmark.cpp, render_tool.cpp
├── test-renders/           output of ember_render (git-ignored)
└── .github/workflows/      CI: build matrix, tests, pluginval, release
```

The CMake targets are `Ember` (the plugin, producing `Ember_VST3`, `Ember_AU` and
`Ember_Standalone`), `ember_dsp` (the host-free DSP library), `ember_tests`,
`ember_benchmark` and `ember_render`. The DSP lives in its own library with no GUI and no
plugin wrapper precisely so it can be exercised headlessly.

---

## Licensing

**Ember is distributed under the GNU General Public License, version 3.** The full text is in
[`LICENSE`](LICENSE).

The reason is straightforward: Ember is built on the **JUCE 8** framework. JUCE is dual
licensed — it is available under the **GPLv3** for open-source distribution, and a
**commercial JUCE licence** is required to distribute a closed-source product built with it.
Ember links JUCE under the GPLv3 branch of that choice, and so Ember itself is GPLv3. If you
fork Ember and distribute your fork, your fork is GPLv3 too; you must make your source
available under the same terms.

If you want to build a closed-source product from this code, the GPL does not permit it — you
would need a commercial licence from the JUCE authors *and* separate permission for Ember's
own code.

### Originality

**Ember is an independent, original work.** It is not affiliated with, sponsored by, or
endorsed by any other plugin vendor or audio software company. It contains no third-party
trademarks, no third-party artwork or GUI assets, no third-party presets, and no third-party
code beyond the open-source dependencies named above (JUCE and, for the test build only,
Catch2). The saturation styles are original designs described in
[`DECISIONS.md`](DECISIONS.md); where a style name describes a familiar *kind* of circuit —
tube, tape, transformer — it is a description of the sound, not a claim about any particular
product.

### Trademarks

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other
countries. Audio Units is a technology of Apple Inc. Ember is not affiliated with either
company; the names are used only to identify the plugin formats Ember supports.
