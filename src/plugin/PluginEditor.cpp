#include "PluginEditor.h"

namespace ember
{
namespace
{
// Layout proportions, expressed as fractions of the window so the interface
// holds together from 800x480 up to 3000x2000.
constexpr int kPresetBarHeight = 34;
constexpr int kGlobalBarHeight = 74;
// The band panel's content — style picker plus the saturation, stereo, feedback,
// dynamics and tone groups — needs this much before it starts clipping. Measured
// by rendering the editor offscreen, not guessed: at the previous 196 the style
// combo was cut in half and none of the knobs were reachable at the default size.
constexpr int kBandPanelHeight = 272;
// Below this the spectrum stops being a usable editing surface, so the band
// panel gives way first.
constexpr int kMinSpectrumHeight = 150;
constexpr int kEdge = 8;
} // namespace

EmberAudioProcessorEditor::EmberAudioProcessorEditor(EmberAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processorRef(p), presetBar(p), globalBar(p), spectrum(p), bandPanel(p),
      modPanel(p)
{
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(presetBar);
    addAndMakeVisible(globalBar);
    addAndMakeVisible(spectrum);
    addAndMakeVisible(bandPanel);
    addAndMakeVisible(modPanel);

    wirePanels();

    bandPanel.setBand(processorRef.getSelectedBand());

    // The modulation panel starts collapsed: at the default 1100x640 there is
    // not room for it open as well as a usable spectrum and a complete band
    // panel, and most sessions begin without any modulation at all.
    modPanel.setCollapsed(true);

    // The spectrum FFT only runs while a window is open.
    processorRef.setSpectrumAnalysisEnabled(true);

    const auto stored = processorRef.getEditorBounds();
    setResizable(true, true);
    setResizeLimits(800, 480, 3000, 2000);
    setSize(stored.getWidth(), stored.getHeight());
}

EmberAudioProcessorEditor::~EmberAudioProcessorEditor()
{
    processorRef.setSpectrumAnalysisEnabled(false);
    setLookAndFeel(nullptr);
}

void EmberAudioProcessorEditor::wirePanels()
{
    // Clicking a band region in the spectrum retargets the band panel.
    spectrum.onBandSelected = [this](int band) { selectBand(band); };

    // A knob that accepts a modulation drop does not know how to build a
    // routing; it hands the (target, source) pair up and the modulation panel
    // creates it, which is also where a rejected routing gets reported.
    bandPanel.onModulationDropped = [this](const juce::String& targetID, int sourceFlatIndex)
    { modPanel.createConnection(targetID, sourceFlatIndex); };

    bandPanel.onRemoveModulation = [this](const juce::String& targetID)
    {
        const int target = processorRef.getModulationTargetIndex(targetID);
        if (target < 0)
            return;
        auto& mod = processorRef.getModulationEngine();
        for (int i = mod.getNumConnections() - 1; i >= 0; --i)
            if (mod.getConnection(i).targetIndex == target)
                mod.removeConnection(i);
    };

    bandPanel.onMidiLearn = [this](const juce::String& targetID) { processorRef.beginMidiLearn(targetID); };
    bandPanel.onClearMidiMapping = [this](const juce::String& targetID) { processorRef.clearMidiMapping(targetID); };

    // PresetBar owns the browser overlay, so the global bar's button just asks
    // it to open rather than the editor keeping a second instance.
    globalBar.onPresetBrowserRequested = [this] { presetBar.showBrowser(); };
    globalBar.onModulationPanelToggled = [this](bool shouldBeVisible)
    {
        modPanel.setCollapsed(!shouldBeVisible);
        resized();
    };

    modPanel.onCollapsedChanged = [this] { resized(); };
}

void EmberAudioProcessorEditor::selectBand(int band)
{
    processorRef.setSelectedBand(band);
    bandPanel.setBand(band);
}

void EmberAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(gui::EmberColours::backgroundDeep);
}

void EmberAudioProcessorEditor::resized()
{
    // Remember the size so the window comes back the way the user left it.
    processorRef.setEditorBounds(getWidth(), getHeight());

    // Scale the fixed-height strips with the window so the layout still works
    // at 2x, rather than leaving them stranded at their 640-pixel proportions.
    const float scale = juce::jlimit(0.8f, 2.0f, static_cast<float>(getHeight()) / 640.0f);
    auto area = getLocalBounds().reduced(kEdge);

    presetBar.setBounds(area.removeFromTop(juce::roundToInt(kPresetBarHeight * scale)));
    area.removeFromTop(kEdge / 2);
    globalBar.setBounds(area.removeFromTop(juce::roundToInt(kGlobalBarHeight * scale)));
    area.removeFromTop(kEdge / 2);

    const int modHeight = juce::jmin(modPanel.getPreferredHeight(), area.getHeight() / 2);
    modPanel.setBounds(area.removeFromBottom(modHeight));
    area.removeFromBottom(kEdge / 2);

    // Give the band panel what its content actually needs, and only take it
    // away when the spectrum would otherwise stop being a usable surface.
    const int wantedBand = juce::roundToInt(kBandPanelHeight * scale);
    const int affordable = juce::jmax(0, area.getHeight() - juce::roundToInt(kMinSpectrumHeight * scale));
    bandPanel.setBounds(area.removeFromBottom(juce::jmin(wantedBand, affordable)));
    area.removeFromBottom(kEdge / 2);

    spectrum.setBounds(area);
}
} // namespace ember
