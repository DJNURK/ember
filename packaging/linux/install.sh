#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Install the Ember VST3 plug-in on Linux.
#
# Run it from inside the extracted release tarball:
#
#   tar xf Ember-1.0.0-Linux.tar.gz
#   cd Ember-1.0.0-Linux
#   ./install.sh
#
# By default Ember.vst3 is copied to ~/.vst3 (or to the first entry of VST3_PATH
# if that variable is set), which is the per-user VST3 location every Linux host
# scans. --system installs to /usr/local/lib/vst3 for all users instead.
#
# Undo with ./uninstall.sh (same options).
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

USER_VST3_DIR="$HOME/.vst3"
SYSTEM_VST3_DIR="/usr/local/lib/vst3"

# Honour VST3_PATH (colon-separated, like PATH) when the user has pointed their
# hosts somewhere other than ~/.vst3.
if [ -n "${VST3_PATH:-}" ]; then
    USER_VST3_DIR="${VST3_PATH%%:*}"
fi

DEST=""
ASSUME_YES=0

info() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<USAGE
usage: install.sh [options]

Installs Ember.vst3 for use by VST3 hosts.

options:
  --system         Install for all users into $SYSTEM_VST3_DIR
                   (needs write access there — usually run with sudo)
  --prefix DIR     Install into DIR, i.e. DIR/Ember.vst3. DIR is the VST3
                   directory itself, not a /usr-style prefix.
  -y, --yes        Do not ask before replacing an existing installation
  -h, --help       Show this help and exit

default destination:
  $USER_VST3_DIR

environment:
  VST3_PATH        If set, its first entry replaces ~/.vst3 as the default
  EMBER_VST3       Path to the Ember.vst3 bundle to install, if it is not
                   next to this script

examples:
  ./install.sh                      # install for the current user
  sudo ./install.sh --system        # install for every user
  ./install.sh --prefix /opt/vst3   # install into /opt/vst3/Ember.vst3
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --system)
            DEST="$SYSTEM_VST3_DIR"
            ;;
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a directory argument"
            DEST="$2"
            shift
            ;;
        --prefix=*)
            DEST="${1#--prefix=}"
            [ -n "$DEST" ] || die "--prefix needs a directory argument"
            ;;
        -y|--yes|--force)
            ASSUME_YES=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'error: unknown option: %s\n\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

[ -n "$DEST" ] || DEST="$USER_VST3_DIR"

# --- locate the bundle -----------------------------------------------------
SRC=""
if [ -n "${EMBER_VST3:-}" ]; then
    [ -d "$EMBER_VST3" ] || die "EMBER_VST3 is set to '$EMBER_VST3', which is not a directory"
    SRC="$EMBER_VST3"
else
    for candidate in \
        "$SCRIPT_DIR/Ember.vst3" \
        "$SCRIPT_DIR/VST3/Ember.vst3" \
        "$SCRIPT_DIR/../Ember.vst3" \
        "$SCRIPT_DIR/../../build/linux-release/Ember_artefacts/Release/VST3/Ember.vst3" \
        "$SCRIPT_DIR/../../build/Ember_artefacts/Release/VST3/Ember.vst3"
    do
        if [ -d "$candidate" ]; then
            SRC="$(cd -- "$candidate" && pwd)"
            break
        fi
    done
fi

if [ -z "$SRC" ]; then
    cat >&2 <<EOF
error: Ember.vst3 was not found.

Looked next to this script ($SCRIPT_DIR) and in the usual build output
directories. Run install.sh from inside the extracted release tarball, or set
EMBER_VST3 to the bundle you want to install:

  EMBER_VST3=/path/to/Ember.vst3 ./install.sh
EOF
    exit 1
fi

TARGET="$DEST/Ember.vst3"

info "source:      $SRC"
info "destination: $TARGET"

# --- do not clobber silently ----------------------------------------------
if [ -e "$TARGET" ]; then
    warn "an Ember installation already exists and will be replaced:"
    printf '      %s\n' "$TARGET" >&2
    if [ -d "$TARGET" ]; then
        printf '      last modified: %s\n' "$(date -r "$TARGET" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo unknown)" >&2
        printf '      size:          %s\n' "$(du -sh "$TARGET" 2>/dev/null | cut -f1 || echo unknown)" >&2
    fi
    if [ "$ASSUME_YES" -eq 0 ]; then
        if [ -t 0 ]; then
            printf 'Replace it? [y/N] ' >&2
            read -r reply
            case "$reply" in
                [yY]|[yY][eE][sS]) ;;
                *) die "aborted — nothing was changed" ;;
            esac
        else
            die "refusing to replace it without confirmation; re-run with --yes to overwrite"
        fi
    fi
fi

# --- install ---------------------------------------------------------------
if ! mkdir -p "$DEST" 2>/dev/null; then
    die "cannot create $DEST — check permissions (for --system, run with sudo)"
fi

if [ ! -w "$DEST" ]; then
    die "$DEST is not writable by $(id -un) — for --system, run with sudo"
fi

rm -rf "$TARGET"
cp -a "$SRC" "$TARGET"

info "installed $TARGET"

if [ "$DEST" != "$USER_VST3_DIR" ] && [ "$DEST" != "$SYSTEM_VST3_DIR" ]; then
    printf '\nNote: %s is not a standard VST3 directory. Add it to your host'\''s\n' "$DEST"
    printf 'plug-in search paths, or export VST3_PATH=%s\n' "$DEST"
fi

printf '\nRescan plug-ins in your DAW (or restart it) to pick Ember up.\n'
printf 'To remove it again: ./uninstall.sh%s\n' "$([ "$DEST" = "$SYSTEM_VST3_DIR" ] && printf ' --system' || printf '')"
