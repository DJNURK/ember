#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "gui/EmberLookAndFeel.h"
#include "gui/Widgets.h"
#include "plugin/PluginProcessor.h"

/**
    Ember's modulation panel: the whole drag-and-drop routing system in one
    collapsible strip.

    WHAT IT IS
    ----------
    Two views behind a slim header.

      SOURCES  every modulation source as a live tile, plus the selected
               source's own parameters. Each tile has a grip you drag onto any
               `ModulatableKnob` in the plugin to route it. The XLFOs get a
               multi-point shape editor; every other source gets a rolling
               scope of its output, so a follower or an envelope can be dialled
               in by eye.

      MATRIX   one row per routing: source, destination, a bipolar amount, a
               response curve, smoothing, an enable lamp and a delete key.
               Routings can also be built here from two menus.

    HOW THE EDITOR WIRES IT UP
    --------------------------
    The editor owns this panel and the `juce::DragAndDropContainer` (it derives
    from one). For every `ModulatableKnob` it creates:

        knob.onModulationDropped = [this] (const juce::String& id, int source)
                                   { modPanel.createConnection (id, source); };
        knob.onRemoveModulation  = [this] (const juce::String& id)
                                   { modPanel.removeConnectionsForTarget (id); };

    That is the entire contract: the knob reports the gesture, this panel owns
    the graph edit and tells the user when the engine refuses one.

    Use `onCollapsedChanged` to re-run the editor's layout when the panel opens
    or closes, and `getHeaderHeight()` / `getPreferredHeight()` to size it.

    THREADING
    ---------
    Message thread only. Source values are read through the engine's lock-free
    `getSourceValue`, graph edits go through its message-thread API, and
    everything repaints from one 30 Hz timer in this class.
*/
namespace ember::gui
{
//==============================================================================
class ModPanel : public juce::Component, private juce::Timer
{
public:
    /** Which of the two views is showing. */
    enum class View
    {
        sources = 0,
        matrix
    };

    explicit ModPanel(EmberAudioProcessor& processorToUse);
    ~ModPanel() override;

    //==========================================================================
    /** Routes modulation source `sourceFlatIndex` (an `ember::flatSourceIndex`)
        to the parameter `targetParameterID`, with a sensible default amount for
        the source's polarity, a linear curve and light smoothing.

        Returns false — and says why in the panel's header — when the ID is not
        a modulation destination, the routing already exists, the table is full,
        or the engine rejects it because it would close a feedback loop in the
        source graph. This is what a knob's `onModulationDropped` calls. */
    bool createConnection(const juce::String& targetParameterID, int sourceFlatIndex);

    /** Removes every routing pointing at a parameter; returns how many went.
        This is what a knob's `onRemoveModulation` calls. */
    int removeConnectionsForTarget(const juce::String& targetParameterID);

    /** Selects a source tile and shows its parameters, expanding the panel and
        switching to the sources view if needed. Out-of-range indices are
        ignored. */
    void selectSource(int sourceFlatIndex);

    /** The currently selected source's flat index. */
    int getSelectedSource() const noexcept { return selectedSource; }

    void setView(View newView);
    View getView() const noexcept { return currentView; }

    /** Collapses to the header alone, or opens it again. */
    void setCollapsed(bool shouldBeCollapsed);
    bool isCollapsed() const noexcept { return collapsed; }

    /** Height of the header strip — the panel's height when collapsed. */
    int getHeaderHeight() const;

    /** Header height when collapsed, header plus a comfortable body when not.
        The editor is free to give it more; the panel fills whatever it gets. */
    int getPreferredHeight() const;

    /** Re-reads the whole graph from the engine and rebuilds the matrix. Called
        automatically when the panel notices the graph changed underneath it (a
        preset load, an undo, an A/B recall), so panels rarely need it. */
    void refreshFromEngine();

    //==========================================================================
    /** Fired after the user collapses or expands the panel: re-run your layout. */
    std::function<void()> onCollapsedChanged;

    /** Fired after any graph edit this panel made, so knobs elsewhere can
        refresh their modulation rings immediately instead of on their own next
        tick. */
    std::function<void()> onConnectionsChanged;

    //==========================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    //==========================================================================
    // Defined in ModPanel.cpp: the header strip and the two views.
    class HeaderStrip;
    class SourcesView;
    class MatrixView;

    void timerCallback() override;

    /** Shows a transient line of text in the header — the only place this panel
        ever reports a refused edit. */
    void showMessage(const juce::String& text, bool isWarning);

    /** Menu helpers shared by the matrix rows and the "add routing" key. */
    void showSourceMenu(juce::Component& target, std::function<void(int)> onPicked);
    void showDestinationMenu(juce::Component& target, int sourceFlatIndex, int slotToIgnore,
                             std::function<void(int)> onPicked);

    // Graph edits. Every one of them reports a refusal through `showMessage`
    // and leaves the engine exactly as it found it.
    bool createConnectionByIndex(int sourceFlatIndex, int targetIndex);
    bool changeConnectionRouting(int slot, int newSourceIndex, int newTargetIndex);
    void deleteConnection(int slot);
    void clearAllConnections();

    /** Readable name of a modulation destination, e.g. "Band 2 Drive". */
    juce::String describeTarget(int targetIndex) const;

    /** Which menu group a destination belongs to, e.g. "Band 2". */
    juce::String groupForTarget(int targetIndex) const;

    void buildTargetGroups();
    void connectionsChanged();

    /** A routing's amount, curve, smoothing or enable changed, but the SHAPE of
        the table did not. Re-arms the external-change detector and tells
        listeners, without rebuilding every matrix row — which a slider drag
        would otherwise do once per mouse move. */
    void connectionValueChanged();
    juce::uint64 graphSignature() const;
    float uiScale() const;

    EmberAudioProcessor& processor;

    std::unique_ptr<HeaderStrip> header;
    std::unique_ptr<SourcesView> sourcesView;
    std::unique_ptr<MatrixView> matrixView;

    View currentView{View::sources};
    bool collapsed{false};
    int selectedSource{0};

    juce::uint64 lastGraphSignature{0};

    // Destination menu grouping, built once from the pid:: helpers so the menu
    // never has to guess a group from a parameter name.
    juce::StringArray targetGroupOrder;
    juce::HashMap<juce::String, juce::String> targetGroupById;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModPanel)
};
} // namespace ember::gui
