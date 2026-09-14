#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build the Ember macOS installer .pkg.
#
#   build-pkg.sh <version> <artefacts-dir> <output-pkg-path>
#
# <artefacts-dir> is the Ember_artefacts/<config> directory produced by CMake,
# e.g. build/macos-universal/Ember_artefacts/Release. It must contain
# VST3/Ember.vst3, AU/Ember.component and Standalone/Ember.app.
#
# Three component packages are built with pkgbuild and combined by productbuild
# using distribution.xml, so the user can pick formats individually:
#
#   Ember.vst3       -> /Library/Audio/Plug-Ins/VST3
#   Ember.component  -> /Library/Audio/Plug-Ins/Components
#   Ember.app        -> /Applications
#
# Environment:
#   INSTALLER_SIGN_ID  Optional. "Developer ID Installer: Name (TEAMID)".
#                      When set, the final .pkg is signed with it, which is a
#                      prerequisite for notarisation (see notarize.sh).
#
# Code-sign the .vst3/.component/.app bundles *before* running this script;
# pkgbuild copies them verbatim and does not sign their contents.
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
DIST_TEMPLATE="$SCRIPT_DIR/distribution.xml"

PKG_ID_VST3="audio.ember.plugin.vst3"
PKG_ID_AU="audio.ember.plugin.au"
PKG_ID_APP="audio.ember.plugin.standalone"

info() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
usage: build-pkg.sh <version> <artefacts-dir> <output-pkg-path>

  <version>          Version string for the package, e.g. 1.0.0
  <artefacts-dir>    Ember_artefacts/<config> directory holding
                     VST3/Ember.vst3, AU/Ember.component, Standalone/Ember.app
  <output-pkg-path>  Where to write the installer, e.g.
                     build/packages/Ember-1.0.0-macOS.pkg

environment:
  INSTALLER_SIGN_ID  "Developer ID Installer: ..." identity; when set the
                     resulting .pkg is signed with productbuild --sign

example:
  ./packaging/macos/build-pkg.sh 1.0.0 \
      build/macos-universal/Ember_artefacts/Release \
      build/packages/Ember-1.0.0-macOS.pkg
USAGE
}

if [ $# -eq 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
    usage
    exit 0
fi

if [ $# -ne 3 ]; then
    usage >&2
    exit 2
fi

VERSION="$1"
ARTEFACTS_IN="$2"
OUTPUT_IN="$3"

[ "$(uname -s)" = "Darwin" ] || die "build-pkg.sh only runs on macOS (pkgbuild/productbuild are Xcode tools)."

case "$VERSION" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "version '$VERSION' does not look like x.y.z" ;;
esac

for tool in pkgbuild productbuild ditto; do
    command -v "$tool" >/dev/null 2>&1 \
        || die "'$tool' not found. Install the Xcode command line tools: xcode-select --install"
done

[ -d "$ARTEFACTS_IN" ] || die "artefacts directory not found: $ARTEFACTS_IN"
ARTEFACTS="$(cd -- "$ARTEFACTS_IN" && pwd)"

[ -f "$DIST_TEMPLATE" ] || die "missing distribution file: $DIST_TEMPLATE"

VST3_SRC="$ARTEFACTS/VST3/Ember.vst3"
AU_SRC="$ARTEFACTS/AU/Ember.component"
APP_SRC="$ARTEFACTS/Standalone/Ember.app"

MISSING=""
[ -d "$VST3_SRC" ] || MISSING="$MISSING
  $VST3_SRC"
[ -d "$AU_SRC" ]   || MISSING="$MISSING
  $AU_SRC"
[ -d "$APP_SRC" ]  || MISSING="$MISSING
  $APP_SRC"

if [ -n "$MISSING" ]; then
    printf 'error: expected build artefacts are missing:%s\n\n' "$MISSING" >&2
    cat <<'HINT' >&2
Build them first, for example:

  cmake --preset macos-universal
  cmake --build --preset macos-universal

then point this script at build/macos-universal/Ember_artefacts/Release.
HINT
    exit 1
fi

# Output path may not exist yet; create its parent and make it absolute.
OUTPUT_DIR="$(dirname -- "$OUTPUT_IN")"
mkdir -p "$OUTPUT_DIR"
OUTPUT="$(cd -- "$OUTPUT_DIR" && pwd)/$(basename -- "$OUTPUT_IN")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ember-pkg.XXXXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

PKGS="$WORK/pkgs"
RESOURCES="$WORK/resources"
mkdir -p "$PKGS" "$RESOURCES"

# --- component packages ----------------------------------------------------
# $1 bundle to package, $2 install location, $3 package identifier, $4 output name
build_component() {
    local src="$1" install_location="$2" identifier="$3" out_name="$4" root
    root="$WORK/root/$out_name"
    mkdir -p "$root"
    # --noqtn: never carry a com.apple.quarantine flag from the build machine
    # into the payload. Code signatures live inside the bundles, not in xattrs,
    # so dropping quarantine is safe and keeps installed plug-ins unflagged.
    ditto --noqtn "$src" "$root/$(basename -- "$src")"
    info "pkgbuild $out_name  ->  $install_location"
    pkgbuild --quiet \
        --root "$root" \
        --identifier "$identifier" \
        --version "$VERSION" \
        --install-location "$install_location" \
        "$PKGS/$out_name" \
        || die "pkgbuild failed for $src"
}

build_component "$VST3_SRC" "/Library/Audio/Plug-Ins/VST3"       "$PKG_ID_VST3" "Ember-VST3.pkg"
build_component "$AU_SRC"   "/Library/Audio/Plug-Ins/Components" "$PKG_ID_AU"   "Ember-AU.pkg"
build_component "$APP_SRC"  "/Applications"                      "$PKG_ID_APP"  "Ember-Standalone.pkg"

# --- distribution file -----------------------------------------------------
DIST="$WORK/distribution.xml"
sed -e "s/__EMBER_VERSION__/$VERSION/g" "$DIST_TEMPLATE" > "$DIST"

if [ -f "$REPO_ROOT/LICENSE" ]; then
    cp "$REPO_ROOT/LICENSE" "$RESOURCES/LICENSE.txt"
else
    warn "no LICENSE at $REPO_ROOT — the installer will be built without a licence page"
    grep -v '<license ' "$DIST" > "$DIST.tmp"
    mv "$DIST.tmp" "$DIST"
fi

# --- final product ---------------------------------------------------------
info "productbuild $OUTPUT"

if [ -n "${INSTALLER_SIGN_ID:-}" ]; then
    info "signing with: $INSTALLER_SIGN_ID"
    productbuild \
        --distribution "$DIST" \
        --package-path "$PKGS" \
        --resources "$RESOURCES" \
        --sign "$INSTALLER_SIGN_ID" \
        "$OUTPUT" \
        || die "productbuild failed (signing identity '$INSTALLER_SIGN_ID' available in the keychain?)"
else
    warn "INSTALLER_SIGN_ID is not set — building an unsigned package (cannot be notarised)"
    productbuild \
        --distribution "$DIST" \
        --package-path "$PKGS" \
        --resources "$RESOURCES" \
        "$OUTPUT" \
        || die "productbuild failed"
fi

[ -f "$OUTPUT" ] || die "productbuild reported success but $OUTPUT does not exist"

info "built $OUTPUT"
ls -lh "$OUTPUT"

if [ -n "${INSTALLER_SIGN_ID:-}" ]; then
    pkgutil --check-signature "$OUTPUT" || warn "pkgutil could not verify the signature"
    printf '\nNext: ./packaging/macos/notarize.sh "%s"\n' "$OUTPUT"
fi
