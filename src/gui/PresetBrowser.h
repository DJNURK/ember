#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "gui/EmberLookAndFeel.h"
#include "gui/Widgets.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PresetManager.h"

/**
    Ember's preset UI, in two pieces that are meant to be used together:

      - `PresetBar`     a compact strip for the plugin header: previous / next,
                        the current preset name, and the "modified" dot. It owns
                        and opens the browser.
      - `PresetBrowser` the full-size overlay: categories on the left, presets on
                        the right, live search, and save / rename / delete.

    HOW AN EDITOR USES THIS
    -----------------------
        PresetBar presetBar { processor };          // a member of the editor
        addAndMakeVisible (presetBar);
        presetBar.onPresetChanged = [this] { ... }; // optional

    That is all: the bar creates the browser on demand, parents it to the
    top-level component so it can dim the whole window, and destroys it again
    when it is dismissed.

    PRESETMANAGER CALLBACK OWNERSHIP
    --------------------------------
    `PresetManager` has exactly one `onPresetListChanged` and one
    `onPresetLoaded`. Both components here CHAIN: each remembers whatever was
    installed when it was constructed, calls it first, and puts it back in its
    destructor. Every lambda that reaches back into a component does so through
    a `juce::Component::SafePointer`, so a callback that fires after the
    component has gone is a no-op rather than a dangling call. The nesting is
    strictly LIFO in practice (the bar outlives the browser it creates), which
    is what makes restore-on-destruction correct.

    If a panel elsewhere needs to know about preset changes, listen to
    `PresetBar::onPresetChanged` rather than taking the manager's callbacks —
    otherwise the last component constructed wins.

    THREADING
    ---------
    Message thread only. Nothing here touches DSP state; the only timer is the
    bar's 8 Hz poll of `PresetManager::isModified()`, which is a plain bool with
    no callback of its own.
*/
namespace ember::gui
{
//==============================================================================
/**
    The full preset browser.

    It is an OVERLAY: add it to a component at that component's full size (the
    bar parents it to the top-level editor) and it dims everything behind it and
    centres its own panel. Clicking the backdrop, pressing Escape or loading a
    preset all fire `onDismiss`; the browser never deletes itself.

    Keyboard: up/down (and page up/down, home/end) move through the preset list
    wherever the focus is, Return loads the highlighted preset, Escape closes —
    or, while a save/rename/delete prompt is open, confirms and cancels it.
*/
class PresetBrowser final : public juce::Component, public juce::KeyListener
{
public:
    explicit PresetBrowser(EmberAudioProcessor& processorToUse);
    ~PresetBrowser() override;

    /** Puts the PresetManager's callbacks back the way they were found, early.
        The destructor does this anyway; call it first when the browser's
        deletion is being deferred to the message queue, so the manager is never
        left holding a callback into an object that is only still alive because
        a delete is pending. Idempotent. */
    void detachFromManager();

    //==========================================================================
    /** The browser wants to go away: Escape, the Close button, a click on the
        backdrop, or a preset having just been loaded. The owner hides or
        deletes it. */
    std::function<void()> onDismiss;

    /** The current preset changed — loaded, saved, renamed or deleted — so a
        header strip can refresh its name. */
    std::function<void()> onPresetChanged;

    /** Rebuilds both lists from the PresetManager, preserving the selection by
        name where it still exists. Wired to `PresetManager::onPresetListChanged`;
        call it directly only if something outside this component has written
        preset files. */
    void refreshLists();

    //==========================================================================
    void paint(juce::Graphics&) override;
    void resized() override;
    void parentSizeChanged() override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress&) override;

    /** juce::KeyListener, attached to the search field: a single-line
        juce::TextEditor swallows the arrow keys (they move the caret), so the
        list navigation has to be intercepted ahead of it. */
    bool keyPressed(const juce::KeyPress&, juce::Component* originatingComponent) override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    //==========================================================================
    /** What the footer is currently asking for. */
    enum class PromptMode
    {
        none = 0,   ///< the plain action row
        save,       ///< name + category, "Save" / "Replace"
        rename,     ///< name only, user presets only
        removeAsked ///< "Delete X?", user presets only
    };

    /** Rows of the category column. Row 0 is the "All" pseudo-category. */
    struct CategoryModel final : public juce::ListBoxModel
    {
        explicit CategoryModel(PresetBrowser& ownerToUse) : owner(ownerToUse) {}

        int getNumRows() override;
        void paintListBoxItem(int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void returnKeyPressed(int lastRowSelected) override;

        PresetBrowser& owner;
    };

    /** Rows of the preset column: whatever the current category and search
        query leave visible. */
    struct PresetModel final : public juce::ListBoxModel
    {
        explicit PresetModel(PresetBrowser& ownerToUse) : owner(ownerToUse) {}

        int getNumRows() override;
        void paintListBoxItem(int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
        void returnKeyPressed(int lastRowSelected) override;
        juce::String getTooltipForRow(int row) override;

        PresetBrowser& owner;
    };

    //==========================================================================
    void installManagerCallbacks();
    void restoreManagerCallbacks();

    void rebuildCategories();
    void applyFilter();
    void selectPreset(const juce::String& presetName, const juce::String& presetCategory);
    const PresetManager::PresetInfo* getSelectedPreset() const;
    bool isSelectionEditable() const;

    void loadSelectedPreset();
    void updateActionButtons();
    void setPrompt(PromptMode mode);
    void confirmPrompt();
    void cancelPrompt();
    void revealUserFolder();
    void dismiss();
    void notifyPresetChanged();

    juce::StringArray userCategoryNames() const;
    bool userPresetExists(const juce::String& presetName, const juce::String& presetCategory) const;

    float scale() const;
    void paintPresetRow(int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected);
    void paintCategoryRow(int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected);

    //==========================================================================
    // Only the preset manager is kept: everything the browser does goes through
    // it, so it never reaches into the processor or the DSP.
    PresetManager& manager;

    std::function<void()> previousListChanged, previousPresetLoaded;
    bool callbacksInstalled{false};

    // Model state.
    juce::StringArray categoryNames;                     // "All" first, then the manager's own
    juce::Array<PresetManager::PresetInfo> shownPresets; // what the preset list is showing
    int hoveredPresetRow{-1}, hoveredCategoryRow{-1};

    PromptMode promptMode{PromptMode::none};
    PresetManager::PresetInfo promptTarget; // captured when the prompt opens

    // Geometry, in float so the panel edges stay crisp at any DPI.
    juce::Rectangle<float> panelArea, categoryWell, presetWell, promptWell;

    // Widgets.
    CategoryModel categoryModel{*this};
    PresetModel presetModel{*this};

    SectionHeader title{"Presets"};
    juce::TextEditor searchField;
    juce::ListBox categoryList, presetList;

    EmberButton saveButton{"Save", EmberButton::Style::accent};
    EmberButton renameButton{"Rename", EmberButton::Style::neutral};
    EmberButton deleteButton{"Delete", EmberButton::Style::danger};
    EmberButton folderButton{"Folder", EmberButton::Style::ghost};
    EmberButton closeButton{"Close", EmberButton::Style::ghost};

    juce::Label promptLabel;
    juce::TextEditor nameField;
    EmberComboBox categoryField;
    EmberButton confirmButton{"Save", EmberButton::Style::accent};
    EmberButton cancelButton{"Cancel", EmberButton::Style::ghost};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowser)
};

//==============================================================================
/**
    The header strip: `‹ | Preset Name · Category ● | ›`.

    The arrows step through the whole library (`PresetManager::loadPrevious` /
    `loadNext`), the amber dot appears as soon as anything has been edited since
    the preset was loaded, and clicking the name opens the browser over the
    whole editor.

    Give it a row about 24–34 px tall in the plugin header; everything inside
    scales from that height.
*/
class PresetBar final : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar(EmberAudioProcessor& processorToUse);
    ~PresetBar() override;

    //==========================================================================
    /** A preset was loaded, saved, renamed or deleted — by the arrows, by the
        browser, or by the host recalling state. */
    std::function<void()> onPresetChanged;

    /** Opens the browser over the top-level component. Safe to call twice. */
    void showBrowser();

    /** Closes it. The deletion is posted to the message queue, so this is safe
        to call from inside one of the browser's own callbacks. */
    void hideBrowser();

    bool isBrowserOpen() const noexcept { return browser != nullptr; }

    /** Re-reads the name and the modified flag. Wired to the manager's
        callbacks; you should not need to call it. */
    void refreshDisplay();

    //==========================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

    //==========================================================================
    /** The `‹` / `›` keys either side of the name. */
    class ChevronButton final : public juce::Button
    {
    public:
        explicit ChevronButton(bool pointsRight);

        void paintButton(juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        bool right;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChevronButton)
    };

    /** The name plate: preset name, category, modified dot, and a hint chevron
        that says it opens something. */
    class NameDisplay final : public juce::Button
    {
    public:
        NameDisplay();

        void setPresetText(const juce::String& presetName, const juce::String& presetCategory, bool isModified);

        void paintButton(juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        juce::String name, category;
        bool modified{false};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NameDisplay)
    };

private:
    void timerCallback() override;
    void installManagerCallbacks();
    void restoreManagerCallbacks();

    EmberAudioProcessor& processor;
    PresetManager& manager;

    std::function<void()> previousListChanged, previousPresetLoaded;

    ChevronButton previousButton{false}, nextButton{true};
    NameDisplay nameDisplay;

    std::unique_ptr<PresetBrowser> browser;

    // Last displayed state, so the poll only repaints when something moved.
    juce::String shownName, shownCategory;
    bool shownModified{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBar)
};
} // namespace ember::gui
