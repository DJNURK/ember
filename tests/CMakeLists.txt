add_executable(ember_tests test_smoke.cpp)
target_link_libraries(ember_tests PRIVATE ember_dsp Catch2::Catch2WithMain juce::juce_recommended_config_flags)
set_target_properties(ember_tests PROPERTIES FOLDER "Ember")
catch_discover_tests(ember_tests)
