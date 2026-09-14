#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

namespace ember
{
/**
    Factory and user preset handling, plus the "modified" bookkeeping the GUI
    shows next to the preset name.

    A preset is the full plugin state: every APVTS parameter plus the modulation
    graph. The graph is fetched and restored through callbacks so this class does
    not need to know anything about the modulation engine.

    Message thread only.
*/
class PresetManager
{
public:
    struct PresetInfo
    {
        juce::String name;
        juce::String category; ///< "Drums", "Bass", ... ; folder name for user presets
        juce::File file;       ///< invalid for factory presets
        bool isFactory{false};
    };

    PresetManager(juce::AudioProcessorValueTreeState& state, std::function<juce::ValueTree()> getModulationTree,
                  std::function<void(const juce::ValueTree&)> setModulationTree);

    /** ~/Documents/EmberAudio/Ember/Presets (or the platform equivalent). */
    static juce::File getUserPresetDirectory();

    void refresh();

    const juce::Array<PresetInfo>& getAllPresets() const noexcept { return presets; }
    juce::StringArray getCategories() const;
    juce::Array<PresetInfo> search(const juce::String& query, const juce::String& category = {}) const;

    bool loadPreset(const PresetInfo& preset);
    bool loadPresetByName(const juce::String& name);
    bool saveUserPreset(const juce::String& name, const juce::String& category);
    bool deleteUserPreset(const PresetInfo& preset);
    bool renameUserPreset(const PresetInfo& preset, const juce::String& newName);

    bool loadNext();
    bool loadPrevious();

    juce::String getCurrentPresetName() const { return currentName; }
    juce::String getCurrentPresetCategory() const { return currentCategory; }

    /** True once any parameter has changed since the preset was loaded. */
    bool isModified() const noexcept { return modified; }
    void markModified() noexcept { modified = true; }
    void clearModified() noexcept { modified = false; }

    /** Complete plugin state: parameters + modulation graph. */
    juce::ValueTree captureState() const;
    void applyState(const juce::ValueTree& tree);

    std::function<void()> onPresetListChanged;
    std::function<void()> onPresetLoaded;

private:
    void loadFactoryPresets();
    int indexOfCurrent() const;

    juce::AudioProcessorValueTreeState& apvts;
    std::function<juce::ValueTree()> getModTree;
    std::function<void(const juce::ValueTree&)> setModTree;

    juce::Array<PresetInfo> presets;
    juce::HashMap<juce::String, juce::String> factoryXml; // name -> xml text

    juce::String currentName{"Init"};
    juce::String currentCategory{"Factory"};
    bool modified{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};
} // namespace ember
