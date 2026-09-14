#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Notarise a signed Ember installer .pkg with Apple, then staple the ticket.
#
#   notarize.sh <pkg-path>
#
# The package must already be signed with a "Developer ID Installer" identity
# (build-pkg.sh does this when INSTALLER_SIGN_ID is set) and the bundles inside
# it must be signed with a "Developer ID Application" identity and the hardened
# runtime, or Apple will reject the submission.
#
# Environment (all required):
#   APPLE_ID            Apple ID e-mail of the developer account
#   APPLE_TEAM_ID       10-character Team ID, e.g. ABCDE12345
#   APPLE_APP_PASSWORD  App-specific password generated at appleid.apple.com
#                       (NOT the account password)
#
# Exits non-zero if notarisation is rejected, after printing Apple's log.
# ---------------------------------------------------------------------------
set -euo pipefail

info() { printf '==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
usage: notarize.sh <pkg-path>

  <pkg-path>  Signed installer package, e.g. build/packages/Ember-1.0.0-macOS.pkg

environment (all required):
  APPLE_ID            Apple ID e-mail of the developer account
  APPLE_TEAM_ID       10-character Team ID (e.g. ABCDE12345)
  APPLE_APP_PASSWORD  app-specific password from appleid.apple.com

example:
  APPLE_ID=dev@example.com APPLE_TEAM_ID=ABCDE12345 APPLE_APP_PASSWORD=abcd-efgh-ijkl-mnop \
      ./packaging/macos/notarize.sh build/packages/Ember-1.0.0-macOS.pkg
USAGE
}

if [ $# -eq 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
    usage
    exit 0
fi

if [ $# -ne 1 ]; then
    usage >&2
    exit 2
fi

PKG="$1"

[ "$(uname -s)" = "Darwin" ] || die "notarize.sh only runs on macOS (needs xcrun notarytool/stapler)."
[ -f "$PKG" ] || die "package not found: $PKG"

case "$PKG" in
    *.pkg) ;;
    *) die "expected a .pkg, got: $PKG" ;;
esac

command -v xcrun >/dev/null 2>&1 \
    || die "'xcrun' not found. Install the Xcode command line tools: xcode-select --install"

# --- credentials -----------------------------------------------------------
MISSING=""
[ -n "${APPLE_ID:-}" ]           || MISSING="$MISSING APPLE_ID"
[ -n "${APPLE_TEAM_ID:-}" ]      || MISSING="$MISSING APPLE_TEAM_ID"
[ -n "${APPLE_APP_PASSWORD:-}" ] || MISSING="$MISSING APPLE_APP_PASSWORD"

if [ -n "$MISSING" ]; then
    printf 'error: missing required environment variable(s):%s\n\n' "$MISSING" >&2
    printf 'Set them in the shell (or as CI secrets) and re-run. See notarize.sh --help.\n' >&2
    exit 1
fi

# The package must be signed, otherwise Apple rejects it with a confusing error.
if ! pkgutil --check-signature "$PKG" >/dev/null 2>&1; then
    die "$PKG is not signed. Re-run build-pkg.sh with INSTALLER_SIGN_ID set to a \"Developer ID Installer\" identity."
fi

SUBMIT_LOG="$(mktemp "${TMPDIR:-/tmp}/ember-notarize.XXXXXXXX")"
cleanup() { rm -f "$SUBMIT_LOG"; }
trap cleanup EXIT INT TERM

notarytool() {
    xcrun notarytool "$@" \
        --apple-id "$APPLE_ID" \
        --team-id "$APPLE_TEAM_ID" \
        --password "$APPLE_APP_PASSWORD"
}

info "submitting $PKG to Apple for notarisation (this can take several minutes)"

set +e
notarytool submit "$PKG" --wait --timeout 30m 2>&1 | tee "$SUBMIT_LOG"
SUBMIT_STATUS=${PIPESTATUS[0]}
set -e

# notarytool prints "  id: <uuid>" for the submission; the first one is ours.
SUBMISSION_ID="$(awk '/^[[:space:]]*id:/ { print $2; exit }' "$SUBMIT_LOG")"

if [ "$SUBMIT_STATUS" -ne 0 ] || ! grep -Eq '^[[:space:]]*status:[[:space:]]*Accepted' "$SUBMIT_LOG"; then
    printf '\n' >&2
    if [ -n "$SUBMISSION_ID" ]; then
        printf 'error: notarisation was not accepted (submission %s). Apple'\''s log follows:\n\n' "$SUBMISSION_ID" >&2
        notarytool log "$SUBMISSION_ID" >&2 || printf 'error: could not fetch the notarisation log\n' >&2
    else
        printf 'error: notarisation failed and no submission id was returned — see the output above.\n' >&2
    fi
    exit 1
fi

info "notarisation accepted (submission ${SUBMISSION_ID:-unknown})"

info "stapling the ticket to $PKG"
xcrun stapler staple "$PKG" || die "stapler staple failed for $PKG"

info "validating the stapled ticket"
xcrun stapler validate "$PKG" || die "stapler validate failed for $PKG"

info "$PKG is notarised and stapled"
