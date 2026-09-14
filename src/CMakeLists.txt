# ------------------------------------------------------------------ DSP core
# Pure-DSP static library: no GUI, no plugin wrapper. Linked by both the plugin
# and the test/benchmark targets so DSP can be exercised headlessly.
add_library(ember_dsp STATIC)

target_sources(ember_dsp PRIVATE
    dsp/EmberEngine.cpp
    dsp/BandChain.cpp
    dsp/BandFx.cpp
    dsp/Crossover.cpp
    dsp/StyleCalibrator.cpp
    dsp/styles/StyleFactory.cpp
    dsp/styles/TubeStyles.cpp
    dsp/styles/TapeStyles.cpp
    dsp/styles/AmpStyles.cpp
    dsp/styles/RectifyStyles.cpp
    dsp/styles/DestroyStyles.cpp
    dsp/styles/FxStyles.cpp
    dsp/modulation/ModTypes.cpp
    dsp/modulation/ModSources.cpp
    dsp/modulation/ModulationEngine.cpp
    plugin/Parameters.cpp
    plugin/PresetManager.cpp
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp
    gui/EmberLookAndFeel.cpp
    gui/Widgets.cpp
    gui/SpectrumDisplay.cpp
    gui/BandPanel.cpp
    gui/GlobalBar.cpp
    gui/ModPanel.cpp
    gui/PresetBrowser.cpp)

target_include_directories(ember_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(ember_dsp PUBLIC
    EMBER_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(ember_dsp
    PUBLIC
        ember_resources
        juce::juce_dsp
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

set_target_properties(ember_dsp PROPERTIES FOLDER "Ember")

# ------------------------------------------------------------------ Plugin
# The plugin target's own sources live in ember_dsp so that the tests can build
# a real EmberAudioProcessor; this target only needs the JUCE wrapper, which
# pulls createPluginFilter out of the library.
target_sources(Ember PRIVATE plugin/PluginEntry.cpp)

target_compile_definitions(Ember PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(Ember
    PRIVATE
        ember_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
