#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Ember — one-command local build for macOS and Linux.
#
#   scripts/build.sh                 release build for this host, then ctest
#   scripts/build.sh --debug         debug build
#   scripts/build.sh --universal     macOS arm64 + x86_64 fat build
#   scripts/build.sh --asan          Linux Address/UB sanitiser build
#   scripts/build.sh --clean --pluginval
#
# Everything goes through the presets in CMakePresets.json, so this script and
# CI build exactly the same way. Windows uses scripts/build.ps1 instead.
#
# Written for bash 3.2 (the /bin/bash that ships with macOS): no associative
# arrays, no mapfile, no ${var^^}.
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ------------------------------------------------------------------ pretty output
if [ -t 1 ]; then
    BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'
    YELLOW=$'\033[33m'; RESET=$'\033[0m'
else
    BOLD=''; DIM=''; RED=''; GREEN=''; YELLOW=''; RESET=''
fi

say()  { printf '%s==>%s %s\n' "$BOLD" "$RESET" "$*"; }
warn() { printf '%s==> warning:%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die()  { printf '%s==> error:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Ember build script (macOS / Linux)

usage: scripts/build.sh [options]

  --debug          Build the *-debug preset instead of *-release.
  --universal      macOS only: build the macos-universal preset (arm64 + x86_64).
  --asan           Linux only: build the linux-asan preset (Address/UB sanitisers).
  --no-tests       Skip running ctest after the build. The test targets are still
                   compiled; this only skips executing them.
  --clean          Delete the preset's build directory before configuring.
  --jobs N         Parallel compile jobs (default: number of CPUs).
  --pluginval      Run scripts/run-pluginval.sh after a successful build.
  --help, -h       Show this help and exit.

Presets are taken from CMakePresets.json:

  macOS    macos-release (default) | macos-debug | macos-universal
  Linux    linux-release (default) | linux-debug | linux-asan

Build directory: build/<preset>/
Artefacts:       build/<preset>/Ember_artefacts/<config>/{VST3,AU,Standalone}/
EOF
}

# ------------------------------------------------------------------ options
WANT_DEBUG=0
WANT_UNIVERSAL=0
WANT_ASAN=0
RUN_TESTS=1
DO_CLEAN=0
RUN_PLUGINVAL=0
JOBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)      WANT_DEBUG=1 ;;
        --universal)  WANT_UNIVERSAL=1 ;;
        --asan)       WANT_ASAN=1 ;;
        --no-tests)   RUN_TESTS=0 ;;
        --clean)      DO_CLEAN=1 ;;
        --pluginval)  RUN_PLUGINVAL=1 ;;
        --jobs)       shift; [ $# -gt 0 ] || die "--jobs needs a number"; JOBS="$1" ;;
        --jobs=*)     JOBS="${1#--jobs=}" ;;
        -j)           shift; [ $# -gt 0 ] || die "-j needs a number"; JOBS="$1" ;;
        -j*)          JOBS="${1#-j}" ;;
        --help|-h)    usage; exit 0 ;;
        *)            usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

if [ -n "$JOBS" ]; then
    case "$JOBS" in
        ''|*[!0-9]*) die "--jobs expects a positive integer, got '$JOBS'" ;;
    esac
    [ "$JOBS" -gt 0 ] || die "--jobs expects a positive integer, got '$JOBS'"
fi

# ------------------------------------------------------------------ host / preset
HOST="$(uname -s)"
case "$HOST" in
    Darwin) OS=macos ;;
    Linux)  OS=linux ;;
    MINGW*|MSYS*|CYGWIN*)
        die "this is a Windows shell — use PowerShell and scripts/build.ps1 instead" ;;
    *)  die "unsupported host system '$HOST' (expected Darwin or Linux)" ;;
esac

if [ "$OS" = macos ]; then
    [ "$WANT_ASAN" -eq 0 ] || die "--asan is a Linux-only preset (linux-asan); not available on macOS"
    if [ "$WANT_UNIVERSAL" -eq 1 ] && [ "$WANT_DEBUG" -eq 1 ]; then
        die "--universal and --debug cannot be combined: macos-universal is a Release preset"
    fi
    if   [ "$WANT_UNIVERSAL" -eq 1 ]; then PRESET=macos-universal; CONFIG=Release
    elif [ "$WANT_DEBUG"     -eq 1 ]; then PRESET=macos-debug;     CONFIG=Debug
    else                                   PRESET=macos-release;   CONFIG=Release
    fi
else
    [ "$WANT_UNIVERSAL" -eq 0 ] || die "--universal is a macOS-only preset (macos-universal)"
    if [ "$WANT_ASAN" -eq 1 ] && [ "$WANT_DEBUG" -eq 1 ]; then
        die "--asan and --debug cannot be combined: linux-asan builds RelWithDebInfo"
    fi
    if   [ "$WANT_ASAN"  -eq 1 ]; then PRESET=linux-asan;    CONFIG=RelWithDebInfo
    elif [ "$WANT_DEBUG" -eq 1 ]; then PRESET=linux-debug;   CONFIG=Debug
    else                               PRESET=linux-release; CONFIG=Release
    fi
fi

BUILD_DIR="$ROOT/build/$PRESET"
ARTEFACT_DIR="$BUILD_DIR/Ember_artefacts/$CONFIG"

# ------------------------------------------------------------------ toolchain checks
if ! command -v cmake >/dev/null 2>&1; then
    if [ "$OS" = macos ]; then
        die "cmake not found on PATH.
    Install it with Homebrew:  brew install cmake ninja
    (or download the official package from https://cmake.org/download/ and add
     /Applications/CMake.app/Contents/bin to your PATH)"
    else
        die "cmake not found on PATH.
    Debian/Ubuntu:  sudo apt install cmake ninja-build
    Fedora:         sudo dnf install cmake ninja-build
    Arch:           sudo pacman -S cmake ninja
    Ember needs CMake 3.22 or newer; if your distro ships an older one, use
    the Kitware APT repo or 'pip install --user cmake'."
    fi
fi

CMAKE_VERSION="$(cmake --version 2>/dev/null | head -n1 \
    | sed -E 's/^[^0-9]*([0-9]+\.[0-9]+(\.[0-9]+)?).*$/\1/')"
case "$CMAKE_VERSION" in
    [0-9]*.[0-9]*) ;;
    *) die "could not parse the version reported by '$(command -v cmake)':
    $(cmake --version 2>&1 | head -n1)
    Ember needs CMake 3.22 or newer." ;;
esac

CM_MAJOR="${CMAKE_VERSION%%.*}"
CM_REST="${CMAKE_VERSION#*.}"
CM_MINOR="${CM_REST%%.*}"

if [ "$CM_MAJOR" -lt 3 ] || { [ "$CM_MAJOR" -eq 3 ] && [ "$CM_MINOR" -lt 22 ]; }; then
    if [ "$OS" = macos ]; then
        die "CMake $CMAKE_VERSION is too old — Ember needs 3.22 or newer
    (CMakePresets.json v3 and 'cmake --preset' require it).
    Upgrade with:  brew install cmake ninja   (or 'brew upgrade cmake')
    Found: $(command -v cmake)"
    else
        die "CMake $CMAKE_VERSION is too old — Ember needs 3.22 or newer
    (CMakePresets.json v3 and 'cmake --preset' require it).
    Debian/Ubuntu:  sudo apt install cmake  (or use the Kitware APT repo)
    Portable:       pip install --user cmake
    Found: $(command -v cmake)"
    fi
fi

# Every macOS/Linux preset uses the Ninja generator, so ninja must be present.
if ! command -v ninja >/dev/null 2>&1; then
    if [ "$OS" = macos ]; then
        die "ninja not found on PATH but the '$PRESET' preset uses the Ninja generator.
    Install it with:  brew install cmake ninja"
    else
        die "ninja not found on PATH but the '$PRESET' preset uses the Ninja generator.
    Debian/Ubuntu:  sudo apt install ninja-build
    Fedora:         sudo dnf install ninja-build
    Arch:           sudo pacman -S ninja"
    fi
fi

if ! command -v git >/dev/null 2>&1; then
    die "git not found on PATH — JUCE 8.0.15 is fetched at configure time with FetchContent,
    which shells out to git. Install git and re-run."
fi

# ------------------------------------------------------------------ parallelism
if [ -z "$JOBS" ]; then
    if [ "$OS" = macos ]; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    fi
    case "$JOBS" in
        ''|*[!0-9]*) JOBS=4 ;;
    esac
    [ "$JOBS" -gt 0 ] || JOBS=4
fi

# ------------------------------------------------------------------ build
say "Ember 1.0.0 — preset ${BOLD}$PRESET${RESET} (${CONFIG}), ${JOBS} job(s)"
printf '    %scmake %s, source %s%s\n' "$DIM" "$CMAKE_VERSION" "$ROOT" "$RESET"

cd "$ROOT"

if [ "$DO_CLEAN" -eq 1 ]; then
    # Deliberately paranoid: only ever remove build/<preset> inside the repo.
    case "$BUILD_DIR" in
        "$ROOT"/build/?*) ;;
        *) die "refusing to clean suspicious path: $BUILD_DIR" ;;
    esac
    if [ -d "$BUILD_DIR" ]; then
        say "cleaning $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    else
        say "nothing to clean ($BUILD_DIR does not exist)"
    fi
fi

START=$SECONDS

say "configuring"
cmake --preset "$PRESET"

say "building"
cmake --build --preset "$PRESET" --parallel "$JOBS"

BUILD_SECONDS=$((SECONDS - START))

# ------------------------------------------------------------------ tests
TEST_RESULT="skipped (--no-tests)"
if [ "$RUN_TESTS" -eq 1 ]; then
    say "running tests"
    # Not every configure preset has a matching test preset (the debug and
    # universal presets do not), so fall back to running ctest directly in the
    # preset's build directory when there is no test preset of the same name.
    if ctest --list-presets 2>/dev/null | grep -q "\"$PRESET\""; then
        ctest --preset "$PRESET" --parallel "$JOBS"
    else
        warn "no ctest preset named '$PRESET' in CMakePresets.json — running ctest directly in $BUILD_DIR"
        ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel "$JOBS"
    fi
    TEST_RESULT="passed"
fi

# ------------------------------------------------------------------ artefacts
# If the configuration directory is not where we expect it (e.g. someone edits
# the preset's CMAKE_BUILD_TYPE), fall back to whatever config dir actually
# exists so the paths we print are never a lie.
if [ ! -d "$ARTEFACT_DIR" ]; then
    for d in "$BUILD_DIR"/Ember_artefacts/*; do
        [ -d "$d" ] || continue
        case "$(basename "$d")" in
            JuceLibraryCode) continue ;;
        esac
        ARTEFACT_DIR="$d"
        break
    done
fi

printf '\n%s==> build finished in %ds (tests: %s)%s\n' "$GREEN" "$BUILD_SECONDS" "$TEST_RESULT" "$RESET"
printf '%sArtefacts:%s\n' "$BOLD" "$RESET"

FOUND_ANY=0
report() {
    # $1 = label, $2 = absolute path
    if [ -e "$2" ]; then
        FOUND_ANY=1
        printf '  %s%-12s%s %s\n' "$GREEN" "$1" "$RESET" "$2"
    else
        printf '  %s%-12s%s %s %s(not built)%s\n' "$DIM" "$1" "$RESET" "$2" "$DIM" "$RESET"
    fi
}

report "VST3"       "$ARTEFACT_DIR/VST3/Ember.vst3"
if [ "$OS" = macos ]; then
    report "AU"         "$ARTEFACT_DIR/AU/Ember.component"
    report "Standalone" "$ARTEFACT_DIR/Standalone/Ember.app"
else
    report "Standalone" "$ARTEFACT_DIR/Standalone/Ember"
fi

if [ "$FOUND_ANY" -eq 0 ]; then
    warn "no artefacts found under $ARTEFACT_DIR — check the build output above"
fi

if [ "$OS" = macos ]; then
    printf '\n%sInstall locally (optional):%s\n' "$BOLD" "$RESET"
    printf '  cp -R "%s" ~/Library/Audio/Plug-Ins/VST3/\n'       "$ARTEFACT_DIR/VST3/Ember.vst3"
    printf '  cp -R "%s" ~/Library/Audio/Plug-Ins/Components/\n' "$ARTEFACT_DIR/AU/Ember.component"
else
    printf '\n%sInstall locally (optional):%s\n' "$BOLD" "$RESET"
    printf '  mkdir -p ~/.vst3 && cp -R "%s" ~/.vst3/\n' "$ARTEFACT_DIR/VST3/Ember.vst3"
fi

# ------------------------------------------------------------------ pluginval
if [ "$RUN_PLUGINVAL" -eq 1 ]; then
    printf '\n'
    say "running pluginval"
    PV="$ROOT/scripts/run-pluginval.sh"
    [ -f "$PV" ] || die "scripts/run-pluginval.sh not found at $PV"
    bash "$PV"
fi
