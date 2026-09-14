#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Ember — clang-format entry point for local use.
#
#   scripts/format.sh                        reformat every tracked C++ source in place
#   scripts/format.sh --check                verify formatting, change nothing
#   scripts/format.sh src/dsp/Crossover.cpp  restrict to specific files
#
# NOTE: the "clang-format" job in .github/workflows/ci.yml does not invoke this
# script — it runs the equivalent git ls-files + clang-format --dry-run --Werror
# inline. Both are kept to the same file set; if you change the selection here,
# change it there too (or switch that job to `scripts/format.sh --check`).
#
# Style comes from the repo's .clang-format (LLVM base, Allman braces, 4 spaces,
# 120 columns). Only tracked sources under src/ and tests/ are considered, so the
# JUCE / Catch2 checkouts under build/_deps are never touched.
#
# Written for bash 3.2 (the /bin/bash that ships with macOS).
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ -t 1 ]; then
    BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'; RESET=$'\033[0m'
else
    BOLD=''; DIM=''; RED=''; GREEN=''; RESET=''
fi

say() { printf '%s==>%s %s\n' "$BOLD" "$RESET" "$*"; }
die() { printf '%s==> error:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Ember source formatter

usage: scripts/format.sh [--check] [--help] [file ...]

  --check      Do not rewrite anything: run clang-format --dry-run --Werror and
               exit non-zero if any file would change. Same check CI performs.
  --help, -h   Show this help and exit.
  file ...     Optional explicit paths (relative to the repo root or absolute).
               Without any, every tracked C/C++ source under src/ and tests/ is
               used.

Override the binary with CLANG_FORMAT=/path/to/clang-format.
Sources fetched into build/_deps (JUCE, Catch2) are never formatted.
EOF
}

# ------------------------------------------------------------------ options
CHECK_ONLY=0
EXPLICIT=()

while [ $# -gt 0 ]; do
    case "$1" in
        --check)    CHECK_ONLY=1; shift ;;
        --help|-h)  usage; exit 0 ;;
        --)         shift
                    while [ $# -gt 0 ]; do EXPLICIT[${#EXPLICIT[@]}]="$1"; shift; done ;;
        -*)         usage >&2; die "unknown option: $1" ;;
        *)          EXPLICIT[${#EXPLICIT[@]}]="$1"; shift ;;
    esac
done

# ------------------------------------------------------------------ find clang-format
find_clang_format() {
    if [ -n "${CLANG_FORMAT:-}" ]; then
        command -v "$CLANG_FORMAT" >/dev/null 2>&1 \
            || die "CLANG_FORMAT is set to '$CLANG_FORMAT' but that is not executable"
        printf '%s\n' "$CLANG_FORMAT"
        return 0
    fi
    for candidate in clang-format \
                     clang-format-20 clang-format-19 clang-format-18 clang-format-17 \
                     clang-format-16 clang-format-15 clang-format-14 \
                     /opt/homebrew/opt/llvm/bin/clang-format \
                     /usr/local/opt/llvm/bin/clang-format; do
        if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if ! CF="$(find_clang_format)"; then
    case "$(uname -s)" in
        Darwin) die "clang-format not found on PATH.
    Install it with:  brew install clang-format
    (or 'brew install llvm' and use CLANG_FORMAT=/opt/homebrew/opt/llvm/bin/clang-format)" ;;
        *)      die "clang-format not found on PATH.
    Debian/Ubuntu:  sudo apt install clang-format
    Fedora:         sudo dnf install clang-tools-extra
    Arch:           sudo pacman -S clang
    Or point at an existing binary:  CLANG_FORMAT=/path/to/clang-format scripts/format.sh" ;;
    esac
fi

CF_VERSION_LINE="$("$CF" --version 2>&1 | head -n1)"
CF_VERSION="$(printf '%s' "$CF_VERSION_LINE" | sed -E 's/^[^0-9]*([0-9]+\.[0-9]+(\.[0-9]+)?).*$/\1/')"
CF_MAJOR="${CF_VERSION%%.*}"
case "$CF_MAJOR" in
    ''|*[!0-9]*) CF_MAJOR=0 ;;
esac

# --dry-run / --Werror landed in clang-format 10; without them --check cannot work.
if [ "$CHECK_ONLY" -eq 1 ] && [ "$CF_MAJOR" -lt 10 ]; then
    die "--check needs clang-format 10 or newer for --dry-run/--Werror, found: $CF_VERSION_LINE
    Upgrade clang-format, or point at a newer one with CLANG_FORMAT=..."
fi

[ -f "$ROOT/.clang-format" ] || die "no .clang-format at $ROOT/.clang-format — refusing to format with an unknown style"

cd "$ROOT"

# ------------------------------------------------------------------ collect files
is_cxx_source() {
    case "$1" in
        *.cpp|*.cc|*.cxx|*.c|*.h|*.hpp|*.hh|*.hxx|*.inl|*.mm|*.m) return 0 ;;
        *) return 1 ;;
    esac
}

# Never format anything inside a build tree — that is where FetchContent puts
# the JUCE and Catch2 checkouts (build/_deps/...).
is_excluded() {
    case "$1" in
        build/*|build-*/*|cmake-build-*/*|*/_deps/*|_deps/*) return 0 ;;
        *) return 1 ;;
    esac
}

FILES=()
add_file() {
    is_cxx_source "$1" || return 0
    is_excluded "$1" && return 0
    [ -f "$1" ] || return 0
    FILES[${#FILES[@]}]="$1"
}

if [ ${#EXPLICIT[@]} -gt 0 ]; then
    for arg in "${EXPLICIT[@]}"; do
        # Normalise absolute paths that live inside the repo to repo-relative.
        case "$arg" in
            # $ROOT is quoted inside the expansion too: an unquoted pattern would
            # treat glob metacharacters in the repo path (e.g. a directory named
            # "Ember [v2]") as a bracket expression, silently skip the strip, and
            # leave an absolute path that the build-tree check below cannot match.
            "$ROOT"/*) arg="${arg#"$ROOT"/}" ;;
            ./*)       arg="${arg#./}" ;;
        esac
        is_cxx_source "$arg" || die "not a C/C++ source file: $arg"
        is_excluded "$arg" && die "refusing to format a build-tree file: $arg"
        [ -f "$arg" ] || die "no such file: $arg"
        FILES[${#FILES[@]}]="$arg"
    done
    SOURCE_DESC="${#FILES[@]} file(s) given on the command line"
elif git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    while IFS= read -r -d '' f; do
        add_file "$f"
    done < <(git -C "$ROOT" ls-files -z -- src tests)
    SOURCE_DESC="${#FILES[@]} tracked source(s) under src/ and tests/"
else
    # Not a git checkout (tarball export): fall back to walking the two trees.
    while IFS= read -r -d '' f; do
        f="${f#./}"
        add_file "$f"
    done < <(find ./src ./tests \
                  \( -name build -o -name '_deps' -o -name 'build-*' -o -name 'cmake-build-*' \) -prune -o \
                  -type f -print0 2>/dev/null)
    SOURCE_DESC="${#FILES[@]} source(s) under src/ and tests/ (no git checkout — used find)"
fi

if [ ${#FILES[@]} -eq 0 ]; then
    die "no C/C++ sources found to format under src/ and tests/"
fi

# ------------------------------------------------------------------ run
printf '    %s%s, style from .clang-format%s\n' "$DIM" "$CF_VERSION_LINE" "$RESET"

if [ "$CHECK_ONLY" -eq 1 ]; then
    say "checking $SOURCE_DESC"
    if "$CF" --style=file --dry-run --Werror "${FILES[@]}"; then
        printf '%s==> formatting OK (%d file(s))%s\n' "$GREEN" "${#FILES[@]}" "$RESET"
        exit 0
    fi
    printf '\n%s==> formatting check failed.%s Run %sscripts/format.sh%s to fix the files listed above.\n' \
        "$RED" "$RESET" "$BOLD" "$RESET" >&2
    exit 1
fi

say "formatting $SOURCE_DESC"
"$CF" --style=file -i "${FILES[@]}"
printf '%s==> formatted %d file(s)%s\n' "$GREEN" "${#FILES[@]}" "$RESET"
