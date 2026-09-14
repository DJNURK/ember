#!/usr/bin/env bash
# Syntax-check a single Ember translation unit with the exact flags the real
# build uses. Usage: scripts/syntax-check.sh src/dsp/Crossover.cpp [more.cpp ...]
# Compiles only (-fsyntax-only): fast, no linking, catches every API mistake.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
if [ ! -f build/compile_commands.json ]; then
  echo "configure first: cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 2
fi
FLAGS=(
  '-DEMBER_VERSION_STRING="1.0.0"'
  -DJUCE_DISPLAY_SPLASH_SCREEN=0
  -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
  -DJUCE_MODAL_LOOPS_PERMITTED=0
  -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1
  -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1
  -DJUCE_MODULE_AVAILABLE_juce_audio_processors=1
  -DJUCE_MODULE_AVAILABLE_juce_audio_processors_headless=1
  -DJUCE_MODULE_AVAILABLE_juce_core=1
  -DJUCE_MODULE_AVAILABLE_juce_data_structures=1
  -DJUCE_MODULE_AVAILABLE_juce_dsp=1
  -DJUCE_MODULE_AVAILABLE_juce_events=1
  -DJUCE_MODULE_AVAILABLE_juce_graphics=1
  -DJUCE_MODULE_AVAILABLE_juce_gui_basics=1
  -DJUCE_MODULE_AVAILABLE_juce_gui_extra=1
  -DJUCE_STRICT_REFCOUNTEDPOINTER=1
  -DJUCE_TARGET_HAS_BINARY_DATA=1
  -DJUCE_USE_CURL=0
  -DJUCE_VST3_CAN_REPLACE_VST2=0
  -DJUCE_WEB_BROWSER=0
  -DNDEBUG=1
  -D_NDEBUG=1
  -I/Users/nurk/Desktop/SATURATE/src
  -I/Users/nurk/Desktop/SATURATE/build/juce_binarydata_ember_resources/JuceLibraryCode
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/VST3_SDK
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/serd
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/sord
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/sord/src
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/sratom
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/lilv
  -I/Users/nurk/Desktop/SATURATE/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/lilv/src
  -O3
  -DNDEBUG
  -std=c++20
  -arch
  arm64
  -fPIC
  -Wall
  -Wextra
  -Wshadow
  -Wno-unused-parameter
  -O3
  -Wshadow-all
  -Wshorten-64-to-32
  -Wstrict-aliasing
  -Wuninitialized
  -Wunused-parameter
  -Wconversion
  -Wsign-compare
  -Wint-conversion
  -Wconditional-uninitialized
  -Wconstant-conversion
  -Wsign-conversion
  -Wbool-conversion
  -Wextra-semi
  -Wunreachable-code
  -Wcast-align
  -Wshift-sign-overflow
  -Wmissing-prototypes
  -Wnullable-to-nonnull-conversion
  -Wno-ignored-qualifiers
  -Wswitch-enum
  -Wpedantic
  -Wdeprecated
  -Wfloat-equal
  -Wmissing-field-initializers
  -Wzero-as-null-pointer-constant
  -Wunused-private-field
  -Woverloaded-virtual
  -Wreorder
  -Winconsistent-missing-destructor-override
)
RC=0
for f in "$@"; do
  echo "==> $f"
  if /usr/bin/c++ -fsyntax-only "${FLAGS[@]}" "$f"; then
    echo "    OK"
  else
    echo "    FAILED: $f" >&2
    RC=1
  fi
done
exit $RC
