cmake_minimum_required(VERSION 3.22)

project(Ember
    VERSION 2.1.1
    DESCRIPTION "Ember — multiband analog saturation and distortion"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(EMBER_BUILD_TESTS     "Build Catch2 unit tests"            ON)
option(EMBER_BUILD_BENCHMARK "Build the offline CPU benchmark"    ON)
option(EMBER_BUILD_AUV3      "Build the AUv3 app-extension"       OFF)
option(EMBER_UNIVERSAL       "macOS: build arm64 + x86_64"        OFF)
option(EMBER_ENABLE_ASAN     "Build with Address/UB sanitizers"   OFF)
option(EMBER_ENABLE_TSAN     "Build with ThreadSanitizer"         OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
    if(EMBER_UNIVERSAL)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /MP)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

if(EMBER_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer finds what ASan cannot: the suite's single-threaded tests can
# exercise every line of the audio path and still miss a race between the
# message thread writing parameters and the audio thread reading them, which is
# the one thing a host does constantly.
if(EMBER_ENABLE_TSAN AND NOT MSVC)
    if(EMBER_ENABLE_ASAN)
        message(FATAL_ERROR "EMBER_ENABLE_TSAN and EMBER_ENABLE_ASAN cannot be combined")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

include(Dependencies)

set(EMBER_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND EMBER_FORMATS AU)
    if(EMBER_BUILD_AUV3)
        list(APPEND EMBER_FORMATS AUv3)
    endif()
endif()

juce_add_plugin(Ember
    COMPANY_NAME                "EmberAudio"
    COMPANY_WEBSITE             "https://example.invalid/ember"
    BUNDLE_ID                   "audio.ember.plugin"
    PLUGIN_MANUFACTURER_CODE    Embr
    PLUGIN_CODE                 Emb1
    FORMATS                     ${EMBER_FORMATS}
    PRODUCT_NAME                "Ember"
    DESCRIPTION                 "Multiband analog saturation and distortion"
    VST3_CATEGORIES             Fx Distortion
    AU_MAIN_TYPE                kAudioUnitType_MusicEffect
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

# Factory presets are compiled in, so a fresh install has a full library with
# nothing to copy alongside the plug-in.
file(GLOB EMBER_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/presets/*.xml")

# The two UI families are embedded rather than resolved from the host system,
# so Ember renders identically on Windows, macOS and Linux. Both are SIL Open
# Font License 1.1; the licences ship beside them and are embedded too, which
# is what the OFL requires of a binary redistribution.
# Tours are data compiled into the binary: a tour is prose with a few hooks in
# it, and writing one in C++ would put whoever knows the plugin behind a
# compiler to fix a typo.
file(GLOB EMBER_TUTORIAL_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/tutorials/*.json")

# The reference is one Markdown file read by both the plugin and the docs
# generator, so the in-plugin help and the manual cannot drift apart.
file(GLOB EMBER_HELP_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/help/*.md")

file(GLOB EMBER_FONT_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*OFL.txt")

juce_add_binary_data(ember_resources SOURCES ${EMBER_PRESET_FILES} ${EMBER_FONT_FILES}
                                             ${EMBER_TUTORIAL_FILES}
                                             ${EMBER_HELP_FILES})
set_target_properties(ember_resources PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "Ember")

add_subdirectory(src)

if(EMBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
