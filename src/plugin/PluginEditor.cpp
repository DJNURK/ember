#include "PluginEditor.h"
#include "gui/EmberTheme.h"
#include "gui/tutorial/TourLibrary.h"
#include "gui/tutorial/UserSettings.h"

namespace ember
{
namespace
{
// Layout proportions, expressed as fractions of the window so the interface
// holds together from 800x480 up to 3000x2000.
// The band panel's content — style picker plus the saturation, stereo, feedback,
// dynamics and tone groups — needs this much before it starts clipping. Measured
// by rendering the editor offscreen, not guessed: at the previous 196 the style
// combo was cut in half and none of the knobs were reachable at the default size.
// Below this the spectrum stops being a usable editing surface, so the band
// panel gives way first.
constexpr int kMinSpectrumHeight = 150;
constexpr int kEdge = 8;
} // namespace

EmberAudioProcessorEditor::EmberAudioProcessorEditor(EmberAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processorRef(p), presetBar(p), globalBar(p), spectrum(p), bandPanel(p),
      modPanel(p), footer(p)
{
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(presetBar);
    addAndMakeVisible(globalBar);
    addAndMakeVisible(spectrum);
    addAndMakeVisible(bandPanel);
    addAndMakeVisible(modPanel);
    addAndMakeVisible(footer);

    // The tour overlay sits above everything; the Learn panel beside the strip.
    addChildComponent(learnPanel);
    addChildComponent(tourEngine);

    tourEngine.onProgress = [](const juce::String& tourId, int step, bool completed)
    { gui::tutorial::UserSettings::setTourProgress(tourId, step, completed); };

    globalBar.onHelpRequested = [this] { learnPanel.isShowing() ? learnPanel.hide() : learnPanel.show(); };

    bandPanel.setMotionClock(motion);

    // The footer owns Input / Output / Mix / Auto-Gain now.
    globalBar.setIoSectionVisible(false);

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
    bandPanel.onBandClicked = [this](int band) { selectBand(band); };
    spectrum.onEqNodeHovered = [this](int band) { bandPanel.setHighlightedBand(band); };

    // First run only, and never again after "Don't show again".
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<EmberAudioProcessorEditor>(this)]
        {
            if (safe != nullptr)
                safe->showWelcomeIfFirstRun();
        });

    globalBar.onZoomRequested = [this](float factor)
    {
        // Zoom sets the window to the design's default size scaled, rather than
        // multiplying whatever size it happens to be - otherwise repeated zooms
        // compound and 125 % twice lands at 156 %.
        setSize(juce::roundToInt(static_cast<float>(gui::Metrics::defaultWidth) * factor),
                juce::roundToInt(static_cast<float>(gui::Metrics::defaultHeight) * factor));
    };

    globalBar.onAppearanceChanged = [this]
    {
        // Colours come from the theme at paint time, so nothing needs rebuilding
        // - but every component has to be told to look again.
        for (auto* child : getChildren())
            child->repaint();

        repaint();
    };

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

void EmberAudioProcessorEditor::showWelcomeIfFirstRun()
{
    if (gui::tutorial::UserSettings::welcomeDismissed())
        return;

    // A card over the display rather than a modal dialog: a modal stops the
    // plugin being a plugin, and someone who opened Ember to work should be
    // able to ignore this entirely.
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::NoIcon)
            .withTitle("New to Ember?")
            .withMessage("Ember splits the signal into bands and saturates each one separately.\n\n"
                         "The two-minute tour covers the whole idea.")
            .withButton("Take the tour")
            .withButton("Skip")
            .withButton("Don't show again"),
        [safe = juce::Component::SafePointer<EmberAudioProcessorEditor>(this)](int result)
        {
            if (safe == nullptr)
                return;

            // Skip leaves the flag alone, so the card returns next time. Only
            // "Don't show again" is permanent - a welcome that comes back after
            // being dismissed is a nag, and one that vanishes after being
            // skipped once was never offered properly.
            if (result == 2)
                gui::tutorial::UserSettings::setWelcomeDismissed(true);
            else if (result == 0)
                if (const auto* tour = gui::tutorial::TourLibrary::find("quickstart"))
                    safe->tourEngine.start(*tour, 0);
        });
}

void EmberAudioProcessorEditor::selectBand(int band)
{
    processorRef.setSelectedBand(band);
    bandPanel.setBand(band);
}

void EmberAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(gui::EmberColours::backgroundDeep());
}

void EmberAudioProcessorEditor::resized()
{
    // Remember the size so the window comes back the way the user left it.
    processorRef.setEditorBounds(getWidth(), getHeight());

    // Scale the fixed-height strips with the window so the layout still works
    // at 2x, rather than leaving them stranded at their 640-pixel proportions.
    const float scale = juce::jlimit(0.8f, 2.0f, static_cast<float>(getHeight()) / 640.0f);
    auto area = getLocalBounds().reduced(kEdge);

    // One header row, as the design specifies, rather than two stacked strips.
    // The logo and preset name take the left; the global controls take the
    // right and lean on GlobalBar's own overflow menu, which drops the least
    // important control into "..." rather than off the edge - so a narrower
    // slot costs reachability nothing.
    auto header = area.removeFromTop(juce::roundToInt(static_cast<float>(gui::Metrics::headerHeight) * scale));
    const int presetWidth = juce::jlimit(180, 460, juce::roundToInt(static_cast<float>(header.getWidth()) * 0.38f));

    presetBar.setBounds(header.removeFromLeft(presetWidth));
    header.removeFromLeft(kEdge / 2);
    globalBar.setBounds(header);
    area.removeFromTop(kEdge / 2);

    // The tour overlay covers the editor; the Learn panel takes width from the
    // right rather than floating over the controls it is describing.
    tourEngine.setBounds(getLocalBounds());

    if (learnPanel.isShowing())
    {
        const int panelWidth = juce::jmin(gui::tutorial::LearnPanel::preferredWidth, area.getWidth() / 2);
        learnPanel.setBounds(area.removeFromRight(panelWidth));
        area.removeFromRight(kEdge / 2);
    }

    footer.setBounds(area.removeFromBottom(gui::FooterBar::preferredHeight(scale)));
    area.removeFromBottom(kEdge / 2);

    // The three middle regions split what is left in the design's 38 : 34 : 18
    // ratio rather than each taking a fixed height. The band strip is now a row
    // of modules, not a single editor, so a fixed height starved it: the group
    // layout saw less than its two-row minimum and was forced to squeeze five
    // groups into a module-width slot, truncating every knob label.
    const int modCollapsed = juce::roundToInt(static_cast<float>(gui::Metrics::modRailCollapsed) * scale);
    const int modWanted = juce::jmin(modPanel.getPreferredHeight(), area.getHeight() / 2);
    const bool modExpanded = modWanted > modCollapsed;

    const int spacing = kEdge / 2;
    int remaining = area.getHeight() - 2 * spacing;

    int modHeight = modCollapsed;

    if (modExpanded)
    {
        modHeight = juce::jmin(
            modWanted, remaining * gui::Metrics::modRailWeight /
                           (gui::Metrics::displayWeight + gui::Metrics::bandStripWeight + gui::Metrics::modRailWeight));
        modHeight = juce::jmax(modHeight, modCollapsed);
    }

    remaining -= modHeight;

    const int bandHeight = juce::jmax(0, remaining * gui::Metrics::bandStripWeight /
                                             (gui::Metrics::displayWeight + gui::Metrics::bandStripWeight));

    modPanel.setBounds(area.removeFromBottom(modHeight));
    area.removeFromBottom(spacing);

    // The spectrum still has a floor: below it the display stops being a usable
    // surface however generous the ratio would be.
    const int affordable = juce::jmax(0, area.getHeight() - juce::roundToInt(kMinSpectrumHeight * scale));
    bandPanel.setBounds(area.removeFromBottom(juce::jmin(bandHeight, affordable)));
    area.removeFromBottom(spacing);

    spectrum.setBounds(area);
}
} // namespace ember
