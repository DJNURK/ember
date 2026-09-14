cmake_minimum_required(VERSION 3.22)

project(Ember
    VERSION 1.0.0
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
juce_add_binary_data(ember_resources SOURCES ${EMBER_PRESET_FILES})
set_target_properties(ember_resources PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "Ember")

add_subdirectory(src)

if(EMBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
