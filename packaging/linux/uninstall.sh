#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Remove the Ember VST3 plug-in installed by install.sh.
#
#   ./uninstall.sh                 # remove ~/.vst3/Ember.vst3
#   sudo ./uninstall.sh --system   # remove /usr/local/lib/vst3/Ember.vst3
#   ./uninstall.sh --prefix DIR    # remove DIR/Ember.vst3
#
# Exactly the inverse of install.sh: it deletes the Ember.vst3 bundle it created
# and nothing else — a ~/.vst3 shared with other plug-ins is left in place, and
# the directory itself is removed only when Ember was the last thing in it.
# ---------------------------------------------------------------------------
set -euo pipefail

USER_VST3_DIR="$HOME/.vst3"
SYSTEM_VST3_DIR="/usr/local/lib/vst3"

if [ -n "${VST3_PATH:-}" ]; then
    USER_VST3_DIR="${VST3_PATH%%:*}"
fi

DEST=""
ASSUME_YES=0

info() { printf '==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<USAGE
usage: uninstall.sh [options]

Removes Ember.vst3 installed by install.sh.

options:
  --system         Remove from $SYSTEM_VST3_DIR
                   (needs write access there — usually run with sudo)
  --prefix DIR     Remove DIR/Ember.vst3
  -y, --yes        Do not ask for confirmation
  -h, --help       Show this help and exit

default location:
  $USER_VST3_DIR

environment:
  VST3_PATH        If set, its first entry replaces ~/.vst3 as the default

examples:
  ./uninstall.sh
  sudo ./uninstall.sh --system
  ./uninstall.sh --prefix /opt/vst3
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

TARGET="$DEST/Ember.vst3"

if [ ! -e "$TARGET" ]; then
    printf 'Nothing to do: %s does not exist.\n' "$TARGET"
    if [ "$DEST" = "$USER_VST3_DIR" ] && [ -e "$SYSTEM_VST3_DIR/Ember.vst3" ]; then
        printf 'A system-wide copy is installed at %s — remove it with:\n' "$SYSTEM_VST3_DIR/Ember.vst3"
        printf '  sudo ./uninstall.sh --system\n'
    fi
    exit 0
fi

info "about to remove: $TARGET"
printf '      last modified: %s\n' "$(date -r "$TARGET" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo unknown)"
printf '      size:          %s\n' "$(du -sh "$TARGET" 2>/dev/null | cut -f1 || echo unknown)"

if [ "$ASSUME_YES" -eq 0 ]; then
    if [ -t 0 ]; then
        printf 'Remove it? [y/N] ' >&2
        read -r reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) die "aborted — nothing was changed" ;;
        esac
    else
        die "refusing to delete without confirmation; re-run with --yes"
    fi
fi

if [ ! -w "$DEST" ]; then
    die "$DEST is not writable by $(id -un) — for --system, run with sudo"
fi

rm -rf "$TARGET"

[ ! -e "$TARGET" ] || die "failed to remove $TARGET"

info "removed $TARGET"

# Tidy up the containing directory only if it is now empty; never touch a shared
# VST3 folder that still holds other plug-ins.
if [ -d "$DEST" ] && [ -z "$(ls -A "$DEST" 2>/dev/null)" ]; then
    rmdir "$DEST" 2>/dev/null && info "removed empty directory $DEST" || true
fi

printf '\nRescan plug-ins in your DAW (or restart it) so Ember disappears from its list.\n'
