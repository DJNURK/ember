#include "PresetManager.h"
#include <BinaryData.h>

namespace ember
{
namespace
{
constexpr const char* kStateType = "EMBER_PRESET";
constexpr const char* kParamsChild = "PARAMETERS";
constexpr const char* kModChild = "MODULATION";
constexpr const char* kPresetExtension = ".emberpreset";
} // namespace

PresetManager::PresetManager(juce::AudioProcessorValueTreeState& state,
                             std::function<juce::ValueTree()> getModulationTree,
                             std::function<void(const juce::ValueTree&)> setModulationTree)
    : apvts(state), getModTree(std::move(getModulationTree)), setModTree(std::move(setModulationTree))
{
    refresh();
}

juce::File PresetManager::getUserPresetDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("EmberAudio")
        .getChildFile("Ember")
        .getChildFile("Presets");
}

void PresetManager::loadFactoryPresets()
{
    // Factory presets are compiled in as binary data so the plugin has a full
    // library with nothing installed alongside it.
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const auto* resourceName = BinaryData::namedResourceList[i];
        const juce::String originalName(BinaryData::getNamedResourceOriginalFilename(resourceName));
        if (! originalName.endsWithIgnoreCase(".xml"))
            continue;

        int size = 0;
        const char* data = BinaryData::getNamedResource(resourceName, size);
        if (data == nullptr || size <= 0)
            continue;

        const juce::String xmlText(juce::CharPointer_UTF8(data), static_cast<size_t>(size));
        auto xml = juce::parseXML(xmlText);
        if (xml == nullptr)
            continue;

        PresetInfo info;
        info.name = xml->getStringAttribute("name", originalName.dropLastCharacters(4));
        info.category = xml->getStringAttribute("category", "Factory");
        info.isFactory = true;
        presets.add(info);
        factoryXml.set(info.name, xmlText);
    }
}

void PresetManager::refresh()
{
    presets.clearQuick();
    factoryXml.clear();

    loadFactoryPresets();

    auto dir = getUserPresetDirectory();
    if (dir.isDirectory())
    {
        for (const auto& entry : juce::RangedDirectoryIterator(dir, true, juce::String("*") + kPresetExtension,
                                                               juce::File::findFiles))
        {
            PresetInfo info;
            info.file = entry.getFile();
            info.name = info.file.getFileNameWithoutExtension();
            auto parent = info.file.getParentDirectory();
            info.category = (parent == dir) ? "User" : parent.getFileName();
            info.isFactory = false;
            presets.add(info);
        }
    }

    std::sort(presets.begin(), presets.end(), [](const PresetInfo& a, const PresetInfo& b)
    {
        if (a.isFactory != b.isFactory) return a.isFactory;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });

    if (onPresetListChanged)
        onPresetListChanged();
}

juce::StringArray PresetManager::getCategories() const
{
    juce::StringArray out;
    for (const auto& p : presets)
        out.addIfNotAlreadyThere(p.category);
    return out;
}

juce::Array<PresetManager::PresetInfo> PresetManager::search(const juce::String& query,
                                                             const juce::String& category) const
{
    juce::Array<PresetInfo> out;
    for (const auto& p : presets)
    {
        if (category.isNotEmpty() && p.category != category)
            continue;
        if (query.isNotEmpty() && ! p.name.containsIgnoreCase(query) && ! p.category.containsIgnoreCase(query))
            continue;
        out.add(p);
    }
    return out;
}

juce::ValueTree PresetManager::captureState() const
{
    juce::ValueTree tree(kStateType);
    tree.setProperty("name", currentName, nullptr);
    tree.setProperty("category", currentCategory, nullptr);
    tree.setProperty("pluginVersion", EMBER_VERSION_STRING, nullptr);
    tree.addChild(apvts.copyState().createCopy(), -1, nullptr);

    if (getModTree)
    {
        auto mod = getModTree();
        if (mod.isValid())
            tree.addChild(mod.createCopy(), -1, nullptr);
    }
    return tree;
}

void PresetManager::applyState(const juce::ValueTree& tree)
{
    if (! tree.isValid())
        return;

    // Parameters: accept either the APVTS child by its real type or the first
    // child that looks like a parameter tree, so hand-written and older presets
    // both load rather than silently doing nothing.
    auto params = tree.getChildWithName(apvts.state.getType());
    if (! params.isValid())
        params = tree.getChildWithName(kParamsChild);
    if (! params.isValid() && tree.getNumChildren() > 0)
        params = tree.getChild(0);

    if (params.isValid())
        apvts.replaceState(params.createCopy());

    if (setModTree)
    {
        auto mod = tree.getChildWithName(kModChild);
        setModTree(mod);   // an invalid tree means "no modulation": clear the graph
    }

    currentName = tree.getProperty("name", currentName).toString();
    currentCategory = tree.getProperty("category", currentCategory).toString();
    modified = false;

    if (onPresetLoaded)
        onPresetLoaded();
}

bool PresetManager::loadPreset(const PresetInfo& preset)
{
    std::unique_ptr<juce::XmlElement> xml;

    if (preset.isFactory)
    {
        if (! factoryXml.contains(preset.name))
            return false;
        xml = juce::parseXML(factoryXml[preset.name]);
    }
    else
    {
        if (! preset.file.existsAsFile())
            return false;
        xml = juce::parseXML(preset.file);
    }

    if (xml == nullptr)
        return false;

    applyState(juce::ValueTree::fromXml(*xml));
    currentName = preset.name;
    currentCategory = preset.category;
    modified = false;
    if (onPresetLoaded)
        onPresetLoaded();
    return true;
}

bool PresetManager::loadPresetByName(const juce::String& name)
{
    for (const auto& p : presets)
        if (p.name == name)
            return loadPreset(p);
    return false;
}

bool PresetManager::saveUserPreset(const juce::String& name, const juce::String& category)
{
    if (name.isEmpty())
        return false;

    auto dir = getUserPresetDirectory();
    if (category.isNotEmpty() && category != "User")
        dir = dir.getChildFile(juce::File::createLegalFileName(category));

    if (! dir.exists() && ! dir.createDirectory().wasOk())
        return false;

    currentName = name;
    currentCategory = category.isEmpty() ? "User" : category;

    auto file = dir.getChildFile(juce::File::createLegalFileName(name) + kPresetExtension);
    auto xml = captureState().createXml();
    if (xml == nullptr || ! xml->writeTo(file))
        return false;

    modified = false;
    refresh();
    return true;
}

bool PresetManager::deleteUserPreset(const PresetInfo& preset)
{
    if (preset.isFactory || ! preset.file.existsAsFile())
        return false;
    if (! preset.file.deleteFile())
        return false;
    refresh();
    return true;
}

bool PresetManager::renameUserPreset(const PresetInfo& preset, const juce::String& newName)
{
    if (preset.isFactory || ! preset.file.existsAsFile() || newName.isEmpty())
        return false;

    auto target = preset.file.getParentDirectory()
                      .getChildFile(juce::File::createLegalFileName(newName) + kPresetExtension);
    if (target.existsAsFile())
        return false;
    if (! preset.file.moveFileTo(target))
        return false;

    if (currentName == preset.name)
        currentName = newName;
    refresh();
    return true;
}

int PresetManager::indexOfCurrent() const
{
    for (int i = 0; i < presets.size(); ++i)
        if (presets.getReference(i).name == currentName && presets.getReference(i).category == currentCategory)
            return i;
    return -1;
}

bool PresetManager::loadNext()
{
    if (presets.isEmpty())
        return false;
    const int next = (indexOfCurrent() + 1) % presets.size();
    return loadPreset(presets.getReference(next));
}

bool PresetManager::loadPrevious()
{
    if (presets.isEmpty())
        return false;
    const int idx = indexOfCurrent();
    const int prev = idx <= 0 ? presets.size() - 1 : idx - 1;
    return loadPreset(presets.getReference(prev));
}
} // namespace ember
