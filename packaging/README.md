# Packaging Ember

Everything needed to turn a Release build of Ember into the three installers that go on
the GitHub Release page:

| Platform | Artefact | Produced by |
|----------|----------|-------------|
| Windows  | `Ember-1.0.0-Windows.exe` | `packaging/windows/ember.iss` (Inno Setup 6) |
| macOS    | `Ember-1.0.0-macOS.pkg`   | `packaging/macos/build-pkg.sh` + `distribution.xml` |
| Linux    | `Ember-1.0.0-Linux.tar.gz` | `tar` + `packaging/linux/install.sh` / `uninstall.sh` |

All of them consume the CMake output directory
`<build>/Ember_artefacts/<config>/` which holds `VST3/Ember.vst3`,
`AU/Ember.component` (macOS only) and `Standalone/Ember.{app,exe,<binary>}`.

Nothing here builds the plug-in. Build first:

```sh
cmake --preset macos-universal        # or linux-release / windows-release
cmake --build --preset macos-universal
```

Testing is not part of packaging, and `macos-universal` has no test preset: the gates
(`ctest --preset macos-release` / `linux-release` / `windows-release`, and
`scripts/run-pluginval.sh`) run from the matching test preset's own build directory.
`.github/workflows/ci.yml` does that on every push; the release workflow only builds
and packages.

---

## Windows — `windows/ember.iss`

An Inno Setup 6 script. It installs:

* `Ember.vst3` into `{commoncf64}\VST3\` — `C:\Program Files\Common Files\VST3`.
  A `.vst3` on Windows is a *folder* bundle, so it is copied recursively.
* Optionally `Ember.exe` into `{autopf}\EmberAudio\Ember\` with a Start Menu shortcut
  (and an unchecked desktop-shortcut task). This is a separate component the user can
  deselect; the VST3 component is `fixed`.

Requires admin rights (`PrivilegesRequired=admin`), installs in 64-bit mode only, and
carries a fixed `AppId` GUID so upgrades replace the previous version instead of piling
up in *Apps & features*. The uninstaller removes the VST3 bundle and the program folder.

### Defines

| Define | Default | Meaning |
|--------|---------|---------|
| `EmberVersion` | `1.0.0` | Version shown in the wizard, and in the output file name |
| `EmberSourceDir` | `..\..\build\windows-release\Ember_artefacts\Release` | Where the artefacts are |
| `EmberOutputDir` | `..\..\build\packages` | Where the `.exe` is written |
| `EmberLicenseFile` | `..\..\LICENSE` | Licence shown in the wizard |

Relative paths are resolved against `packaging\windows\`; CI should pass absolute ones.
If `Ember.vst3` is missing the compile fails with an explanatory `#error`. If
`Ember.exe` is missing the script emits a message and builds a VST3-only installer.

### By hand

```bat
iscc packaging\windows\ember.iss ^
     /DEmberVersion=1.0.0 ^
     /DEmberSourceDir=%CD%\build\windows-release\Ember_artefacts\Release ^
     /DEmberOutputDir=%CD%\build\packages
```

Output: `build\packages\Ember-1.0.0-Windows.exe`.

Inno Setup 6.0+ works; 6.3+ is preferred (it uses the newer `x64compatible`
architecture identifier, which also covers ARM64 machines running x64 code).
Install it with `winget install JRSoftware.InnoSetup` or from
<https://jrsoftware.org/isdl.php>.

Signing the installer is optional and not done by these files. If you have a code-signing
certificate, sign `Ember.exe`, the VST3 binary and the finished installer with `signtool`
before publishing.

---

## macOS — `macos/build-pkg.sh`, `macos/distribution.xml`, `macos/notarize.sh`

```sh
./packaging/macos/build-pkg.sh <version> <artefacts-dir> <output-pkg-path>
```

`build-pkg.sh` runs `pkgbuild` three times, once per format:

| Component package | Install location |
|-------------------|------------------|
| `Ember-VST3.pkg` (`audio.ember.plugin.vst3`) | `/Library/Audio/Plug-Ins/VST3` |
| `Ember-AU.pkg` (`audio.ember.plugin.au`) | `/Library/Audio/Plug-Ins/Components` |
| `Ember-Standalone.pkg` (`audio.ember.plugin.standalone`) | `/Applications` |

and combines them with `productbuild --distribution packaging/macos/distribution.xml`.
The distribution file declares the three choices so the user can install any subset, and
sets `hostArchitectures="arm64,x86_64"` so the package runs on both Apple silicon and
Intel. `__EMBER_VERSION__` in that file is substituted at build time; if the repository
has no `LICENSE` file the `<license>` line is dropped and a warning is printed.

All work happens in a `mktemp -d` directory removed by a `trap`. Missing artefacts abort
with a message naming the exact paths that were expected.

### Signing and notarising

The bundles must be signed **before** packaging — `pkgbuild` copies them verbatim:

```sh
codesign --force --deep --options runtime --timestamp \
         --sign "Developer ID Application: Your Name (TEAMID)" \
         build/macos-universal/Ember_artefacts/Release/VST3/Ember.vst3 \
         build/macos-universal/Ember_artefacts/Release/AU/Ember.component \
         build/macos-universal/Ember_artefacts/Release/Standalone/Ember.app
```

Then build a signed package and notarise it:

```sh
INSTALLER_SIGN_ID="Developer ID Installer: Your Name (TEAMID)" \
  ./packaging/macos/build-pkg.sh 1.0.0 \
      build/macos-universal/Ember_artefacts/Release \
      build/packages/Ember-1.0.0-macOS.pkg

APPLE_ID=dev@example.com \
APPLE_TEAM_ID=ABCDE12345 \
APPLE_APP_PASSWORD=abcd-efgh-ijkl-mnop \
  ./packaging/macos/notarize.sh build/packages/Ember-1.0.0-macOS.pkg
```

`notarize.sh` refuses to run unless all three variables are set (it names the missing
ones) and unless the package is signed. It runs `xcrun notarytool submit --wait`, prints
`xcrun notarytool log <id>` when Apple rejects the submission, then staples and validates
the ticket.

Without `INSTALLER_SIGN_ID` the script still produces a working unsigned `.pkg` — useful
for local testing, but Gatekeeper will warn end users, and it cannot be notarised.

### Environment variables

| Variable | Used by | Purpose |
|----------|---------|---------|
| `INSTALLER_SIGN_ID` | `build-pkg.sh` | `Developer ID Installer: …` identity for `productbuild --sign` |
| `APPLE_ID` | `notarize.sh` | Developer-account Apple ID |
| `APPLE_TEAM_ID` | `notarize.sh` | 10-character Team ID |
| `APPLE_APP_PASSWORD` | `notarize.sh` | App-specific password from appleid.apple.com — **never** the account password |

Testing the finished package locally:

```sh
sudo installer -pkg build/packages/Ember-1.0.0-macOS.pkg -target /
auval -v aumf Emb1 Embr        # AU registers as kAudioUnitType_MusicEffect (see DECISIONS.md D14)
```

---

## Linux — `linux/install.sh`, `linux/uninstall.sh`

Linux gets a tarball rather than a distro package, because VST3 hosts on Linux all scan
`~/.vst3` and `/usr/local/lib/vst3` regardless of packaging. Expected layout:

```
Ember-1.0.0-Linux/
├── Ember.vst3/        (the bundle, containing Contents/x86_64-linux/Ember.so)
├── install.sh
├── uninstall.sh
└── LICENSE
```

The tarball carries that single top-level directory, so `install.sh` can find
`Ember.vst3` next to itself and hand off to `./uninstall.sh` afterwards. Build it by
hand:

```sh
cmake --preset linux-release && cmake --build --preset linux-release --target Ember_VST3
STAGE=build/packages/Ember-1.0.0-Linux
mkdir -p "$STAGE"
cp -a build/linux-release/Ember_artefacts/Release/VST3/Ember.vst3 "$STAGE/"
cp packaging/linux/install.sh packaging/linux/uninstall.sh LICENSE "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh"
tar -C build/packages -czf build/packages/Ember-1.0.0-Linux.tar.gz Ember-1.0.0-Linux
```

`install.sh` finds `Ember.vst3` next to itself (or via `EMBER_VST3=`) and copies it to:

* `~/.vst3` by default — or the first entry of `VST3_PATH` when that variable is set;
* `/usr/local/lib/vst3` with `--system` (run under `sudo`);
* any directory with `--prefix DIR`, where `DIR` is the VST3 directory itself.

If something is already installed at the destination it prints the path, modification
time and size, and asks before replacing it. `--yes` skips the prompt; without a TTY and
without `--yes` it refuses rather than clobbering silently. `uninstall.sh` takes the same
options and removes exactly what was installed, leaving a shared `~/.vst3` in place unless
it is empty.

---

## What CI expects

`.github/workflows/release.yml` is the only consumer of these files; it runs on a `v*`
tag or a manual dispatch. Its contract:

1. Build Release on each runner with the matching preset — `macos-universal`,
   `linux-release`, `windows-release` — with `EMBER_BUILD_TESTS=OFF`. The `ctest` and
   pluginval gates live in `ci.yml` and run on every push and pull request; the release
   workflow builds and packages only.
2. Call each entry point with the tag version (`v1.0.0` → `1.0.0`):
   * Windows — `ISCC.exe /DEmberVersion=<v> /DEmberSourceDir=<Ember_artefacts\Release>`,
     plus `/O` and `/F` to pin the output directory and file name. The other two defines
     keep their defaults, so the repository `LICENSE` is the wizard's licence page.
   * macOS — stage the three signed bundles into a `VST3/`, `AU/`, `Standalone/` tree and
     run `build-pkg.sh <version> <that directory> <out.pkg>`: three positional arguments,
     no options, no environment inputs. `INSTALLER_SIGN_ID` is deliberately left unset —
     the workflow runs `productsign` and `notarytool` itself, so `notarize.sh` is the
     hand equivalent of those steps rather than a CI step.
   * Linux — `tar` `Ember.vst3` together with both scripts and `LICENSE`, under a single
     `Ember-<version>-Linux/` directory.
3. Collect everything in `dist/` and upload five assets:
   `Ember-<version>-Windows.exe`, `Ember-<version>-Windows.zip`,
   `Ember-<version>-macOS.pkg`, `Ember-<version>-macOS.zip`,
   `Ember-<version>-Linux.tar.gz`. The two `.zip`s are raw bundles zipped by the
   workflow — nothing in this directory produces them.

Repository secrets, all optional — without them the release is still produced, ad-hoc
signed, so forks and pull requests still build installers:

| Secret | Consumed as |
|--------|-------------|
| `APPLE_CERT_P12_BASE64`, `APPLE_CERT_PASSWORD` | base64 `.p12` imported into a temporary keychain for `codesign` |
| `APPLE_INSTALLER_CERT_P12_BASE64` | "Developer ID Installer" `.p12`, used by `productsign` on the finished `.pkg` |
| `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | `xcrun notarytool` credentials — the same three `notarize.sh` reads |

Runner prerequisites: Xcode command line tools on macOS (`pkgbuild`, `productbuild`,
`xcrun`), Inno Setup 6 on Windows (the workflow locates `ISCC.exe` or installs it with
Chocolatey), nothing beyond `tar` on Linux.

Version numbers live in exactly one place, `project(Ember VERSION 1.0.0)` in
`CMakeLists.txt`. When bumping it, pass the same value to the packaging entry points; the
defaults in `ember.iss` are only a convenience for hand builds.
