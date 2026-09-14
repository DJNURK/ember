#!/usr/bin/env bash
# Fetch pluginval (pinned) and validate every built Ember format at strictness 10.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PV_VERSION="${PLUGINVAL_VERSION:-v1.0.4}"
TOOLS="$ROOT/build/tools"
STRICTNESS="${STRICTNESS:-10}"
REPEAT="${REPEAT:-1}"
mkdir -p "$TOOLS"

case "$(uname -s)" in
  Darwin) PV_ZIP=pluginval_macOS.zip;   PV_BIN="$TOOLS/pluginval.app/Contents/MacOS/pluginval" ;;
  Linux)  PV_ZIP=pluginval_Linux.zip;   PV_BIN="$TOOLS/pluginval" ;;
  *)      PV_ZIP=pluginval_Windows.zip; PV_BIN="$TOOLS/pluginval.exe" ;;
esac

if [ ! -e "$PV_BIN" ]; then
  echo "==> fetching pluginval $PV_VERSION"
  curl -sSL -o "$TOOLS/$PV_ZIP" \
    "https://github.com/Tracktion/pluginval/releases/download/$PV_VERSION/$PV_ZIP"
  ( cd "$TOOLS" && unzip -oq "$PV_ZIP" )
  chmod +x "$PV_BIN" 2>/dev/null || true
fi

# macOS only discovers AudioUnits that are installed, so stage the .component
# into the user plug-in folder before validating it.
if [ "$(uname -s)" = "Darwin" ]; then
  for c in "$ROOT"/build/*/Ember_artefacts/*/AU/Ember.component "$ROOT"/build/Ember_artefacts/*/AU/Ember.component; do
    [ -d "$c" ] || continue
    mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"
    rm -rf "$HOME/Library/Audio/Plug-Ins/Components/Ember.component"
    cp -R "$c" "$HOME/Library/Audio/Plug-Ins/Components/"
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    sleep 2
  done
fi

shopt -s nullglob
TARGETS=()
for d in "$ROOT"/build/*/Ember_artefacts/*/VST3/Ember.vst3 "$ROOT"/build/Ember_artefacts/*/VST3/Ember.vst3 \
         "$HOME"/Library/Audio/Plug-Ins/Components/Ember.component; do
  TARGETS+=("$d")
done

if [ ${#TARGETS[@]} -eq 0 ]; then
  echo "no built plug-ins found under $ROOT/build — build first" >&2
  exit 1
fi

FAILED=0
for t in "${TARGETS[@]}"; do
  echo "==> pluginval (strictness $STRICTNESS): $t"
  if ! "$PV_BIN" --strictness-level "$STRICTNESS" --validate-in-process \
        --repeat "$REPEAT" --timeout-ms 600000 --validate "$t"; then
    echo "!! FAILED: $t" >&2
    FAILED=1
  fi
done
exit $FAILED
