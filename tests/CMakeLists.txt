add_executable(ember_tests
    test_smoke.cpp
    test_crossover.cpp
    test_styles.cpp
    test_aliasing.cpp
    test_feedback.cpp
    test_smoothing.cpp
    test_state.cpp)

target_include_directories(ember_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ember_tests PRIVATE
    ember_dsp
    Catch2::Catch2WithMain
    juce::juce_recommended_config_flags)
set_target_properties(ember_tests PROPERTIES FOLDER "Ember")
catch_discover_tests(ember_tests)

if(EMBER_BUILD_BENCHMARK)
    add_executable(ember_benchmark benchmark.cpp)
    target_include_directories(ember_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ember_benchmark PRIVATE
        ember_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
    set_target_properties(ember_benchmark PROPERTIES FOLDER "Ember")
endif()

# Offline render tool: pushes test signals through the processor and writes the
# results to test-renders/ so the manual checklist in docs/TESTING.md can be
# completed against real audio rather than by ear alone.
add_executable(ember_render render_tool.cpp)
target_include_directories(ember_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ember_render PRIVATE
    ember_dsp
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)
set_target_properties(ember_render PROPERTIES FOLDER "Ember")
