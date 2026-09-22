#include "gui/PresetBrowser.h"
#include "gui/EmberTheme.h"
#include "gui/tutorial/TourTargets.h"

namespace ember::gui
{
namespace
{
//==============================================================================
/** How dark the editor goes behind the browser panel. Enough to push the
    plugin back without hiding it, so the overlay reads as temporary. */
constexpr float kBackdropAlpha = 0.76f;

/** The UI scale the rest of the plugin is drawing at. Falls back to deriving it
    from the window height if the component is not (yet) using Ember's
    look-and-feel, so nothing depends on construction order. */
float uiScaleFor(const juce::Component& component)
{
    if (const auto* lnf = dynamic_cast<const EmberLookAndFeel*>(&component.getLookAndFeel()))
        return lnf->getUiScale();

    const auto* top = component.getTopLevelComponent();
    const int height = top != nullptr ? top->getHeight() : component.getHeight();

    return EmberFonts::scaleFor(static_cast<float>(juce::jmax(1, height)));
}

/** Row height for the two lists: readable at 1.0, generous when the window is
    large, never so small that the dot and the tag collide. */
int listRowHeight(float uiScale)
{
    return juce::roundToInt(juce::jlimit(17.0f, 32.0f, 21.0f * uiScale));
}

/** Both list columns and the prompt strip use the same inner padding. */
float textPaddingFor(float rowHeight)
{
    return juce::jmax(4.0f, rowHeight * 0.30f);
}

/** A down-pointing chevron, used as the "this opens something" hint. */
void drawDownChevron(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const float size = juce::jmax(2.0f, juce::jmin(area.getWidth(), area.getHeight()) * 0.30f);
    const auto centre = area.getCentre();

    juce::Path path;
    path.startNewSubPath(centre.x - size, centre.y - size * 0.45f);
    path.lineTo(centre.x, centre.y + size * 0.55f);
    path.lineTo(centre.x + size, centre.y - size * 0.45f);

    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(juce::jmax(1.0f, size * 0.36f), juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
}

/** Puts a failed operation's reason where the prompt's title was: the prompt
    stays open so the user can correct the name and try again. */
void showPromptError(juce::Label& label, const juce::String& message)
{
    label.setColour(juce::Label::textColourId, EmberColours::warning());
    label.setText(message, juce::dontSendNotification);
}

/** Ember's small-caps annotation style, for the category tag on a row. */
juce::String tagTextFor(const PresetManager::PresetInfo& info)
{
    return info.category.isEmpty() ? juce::String(info.isFactory ? "FACTORY" : "USER") : info.category.toUpperCase();
}
} // namespace

//==============================================================================
// PresetBrowser::CategoryModel / PresetModel
//
// Thin adapters: the browser owns all the state and does all the drawing, these
// only exist because one class cannot be two juce::ListBoxModels.
//==============================================================================
int PresetBrowser::CategoryModel::getNumRows()
{
    return owner.categoryNames.size();
}

void PresetBrowser::CategoryModel::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                                    bool rowIsSelected)
{
    owner.paintCategoryRow(rowNumber, g, width, height, rowIsSelected);
}

void PresetBrowser::CategoryModel::selectedRowsChanged(int lastRowSelected)
{
    juce::ignoreUnused(lastRowSelected);
    owner.applyFilter();
}

void PresetBrowser::CategoryModel::returnKeyPressed(int lastRowSelected)
{
    juce::ignoreUnused(lastRowSelected);
    owner.presetList.grabKeyboardFocus();
}

//==============================================================================
int PresetBrowser::PresetModel::getNumRows()
{
    return owner.shownPresets.size();
}

void PresetBrowser::PresetModel::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                                  bool rowIsSelected)
{
    owner.paintPresetRow(rowNumber, g, width, height, rowIsSelected);
}

void PresetBrowser::PresetModel::selectedRowsChanged(int lastRowSelected)
{
    juce::ignoreUnused(lastRowSelected);
    owner.updateActionButtons();
}

void PresetBrowser::PresetModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    juce::ignoreUnused(row);
    owner.loadSelectedPreset();
}

void PresetBrowser::PresetModel::returnKeyPressed(int lastRowSelected)
{
    juce::ignoreUnused(lastRowSelected);
    owner.loadSelectedPreset();
}

juce::String PresetBrowser::PresetModel::getTooltipForRow(int row)
{
    if (!juce::isPositiveAndBelow(row, owner.shownPresets.size()))
        return {};

    const auto& info = owner.shownPresets.getReference(row);

    juce::String tip;
    tip << info.name << "\n" << info.category << (info.isFactory ? " - factory preset" : " - user preset");
    tip << "\nDouble-click or press Return to load";

    return tip;
}

//==============================================================================
// PresetBrowser
//==============================================================================
PresetBrowser::PresetBrowser(EmberAudioProcessor& processorToUse) : manager(processorToUse.getPresetManager())
{
    setWantsKeyboardFocus(true);

    addAndMakeVisible(title);

    searchField.setMultiLine(false, false);
    searchField.setReturnKeyStartsNewLine(false);
    searchField.setJustification(juce::Justification::centredLeft);
    searchField.setTextToShowWhenEmpty("Search", EmberColours::textDisabled());
    searchField.setTooltip("Filter by preset or category name");
    searchField.onTextChange = [this] { applyFilter(); };
    searchField.onReturnKey = [this] { loadSelectedPreset(); };
    searchField.onEscapeKey = [this]
    {
        if (searchField.getText().isNotEmpty())
        {
            searchField.setText({}, false);
            applyFilter();
        }
        else
        {
            dismiss();
        }
    };
    searchField.addKeyListener(this);
    addAndMakeVisible(searchField);

    for (auto* list : {&categoryList, &presetList})
    {
        list->setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        list->setOutlineThickness(0);
        list->setMultipleSelectionEnabled(false);
        list->getViewport()->setScrollBarThickness(9);

        // Rows are drawn, not built from components, so hover has to come from
        // listening to the whole subtree. See mouseMove().
        list->addMouseListener(this, true);
        addAndMakeVisible(*list);
    }

    categoryList.setModel(&categoryModel);
    presetList.setModel(&presetModel);

    saveButton.setTooltip("Save the current settings as a user preset");
    saveButton.onClick = [this] { setPrompt(PromptMode::save); };
    addAndMakeVisible(saveButton);

    renameButton.onClick = [this] { setPrompt(PromptMode::rename); };
    addAndMakeVisible(renameButton);

    deleteButton.onClick = [this] { setPrompt(PromptMode::removeAsked); };
    addAndMakeVisible(deleteButton);

    folderButton.setTooltip("Show the user preset folder in the file browser");
    folderButton.onClick = [this] { revealUserFolder(); };
    addAndMakeVisible(folderButton);

    closeButton.setTooltip("Close the browser (Escape)");
    closeButton.onClick = [this] { dismiss(); };
    addAndMakeVisible(closeButton);

    promptLabel.setJustificationType(juce::Justification::centredLeft);
    promptLabel.setInterceptsMouseClicks(false, false);
    addChildComponent(promptLabel);

    nameField.setMultiLine(false, false);
    nameField.setReturnKeyStartsNewLine(false);
    nameField.setSelectAllWhenFocused(true);
    nameField.setJustification(juce::Justification::centredLeft);
    nameField.setTextToShowWhenEmpty("Preset name", EmberColours::textDisabled());
    nameField.onReturnKey = [this] { confirmPrompt(); };
    nameField.onEscapeKey = [this] { cancelPrompt(); };
    nameField.onTextChange = [this]
    {
        if (promptMode == PromptMode::save)
            confirmButton.setButtonText(
                userPresetExists(nameField.getText().trim(), categoryField.getText().trim()) ? "Replace" : "Save");
    };
    addChildComponent(nameField);

    categoryField.setEditableText(true);
    categoryField.setTooltip("Pick an existing folder or type a new one");
    categoryField.setTextWhenNothingSelected("User");
    categoryField.onChange = [this]
    {
        if (promptMode == PromptMode::save)
            confirmButton.setButtonText(
                userPresetExists(nameField.getText().trim(), categoryField.getText().trim()) ? "Replace" : "Save");
    };
    addChildComponent(categoryField);

    confirmButton.onClick = [this] { confirmPrompt(); };
    addChildComponent(confirmButton);

    cancelButton.onClick = [this] { cancelPrompt(); };
    addChildComponent(cancelButton);

    installManagerCallbacks();
    refreshLists();
}

PresetBrowser::~PresetBrowser()
{
    restoreManagerCallbacks();

    searchField.removeKeyListener(this);

    // Symmetry with the constructor: the lists hold a raw pointer to this as a
    // mouse listener, so it is taken out explicitly rather than relying on the
    // member destruction order to make it unreachable.
    categoryList.removeMouseListener(this);
    presetList.removeMouseListener(this);

    categoryList.setModel(nullptr);
    presetList.setModel(nullptr);
}

void PresetBrowser::detachFromManager()
{
    restoreManagerCallbacks();
}

//==============================================================================
void PresetBrowser::installManagerCallbacks()
{
    // Installing twice would capture our own chained lambda as "what was there
    // before" and recurse forever the next time the manager fires.
    if (callbacksInstalled)
        return;

    const juce::Component::SafePointer<PresetBrowser> safeThis(this);

    previousListChanged = manager.onPresetListChanged;
    previousPresetLoaded = manager.onPresetLoaded;
    callbacksInstalled = true;

    auto chainedListChanged = previousListChanged;
    manager.onPresetListChanged = [safeThis, chainedListChanged]
    {
        if (chainedListChanged)
            chainedListChanged();

        if (auto* self = safeThis.getComponent())
            self->refreshLists();
    };

    auto chainedLoaded = previousPresetLoaded;
    manager.onPresetLoaded = [safeThis, chainedLoaded]
    {
        if (chainedLoaded)
            chainedLoaded();

        if (auto* self = safeThis.getComponent())
        {
            // The "currently loaded" marker moved; nothing else changed.
            self->presetList.repaint();
            self->updateActionButtons();
        }
    };
}

void PresetBrowser::restoreManagerCallbacks()
{
    if (!callbacksInstalled)
        return;

    callbacksInstalled = false;

    manager.onPresetListChanged = previousListChanged;
    manager.onPresetLoaded = previousPresetLoaded;

    previousListChanged = nullptr;
    previousPresetLoaded = nullptr;
}

//==============================================================================
void PresetBrowser::refreshLists()
{
    // Keep whatever was highlighted, or fall back to the loaded preset.
    const auto* selected = getSelectedPreset();
    const auto keepName = selected != nullptr ? selected->name : manager.getCurrentPresetName();
    const auto keepCategory = selected != nullptr ? selected->category : manager.getCurrentPresetCategory();

    rebuildCategories();
    applyFilter();
    selectPreset(keepName, keepCategory);
    updateActionButtons();
}

void PresetBrowser::rebuildCategories()
{
    const int selectedRow = categoryList.getSelectedRow();
    const auto wasSelected =
        juce::isPositiveAndBelow(selectedRow, categoryNames.size()) ? categoryNames[selectedRow] : juce::String();

    categoryNames.clearQuick();
    categoryNames.add("All");
    categoryNames.addArray(manager.getCategories());

    // Per-category totals, counted once here rather than re-scanning the whole
    // library inside every row's paint. Row 0 ("All") holds the grand total.
    const auto& allPresets = manager.getAllPresets();

    categoryCounts.clearQuick();
    categoryCounts.ensureStorageAllocated(categoryNames.size());
    categoryCounts.add(allPresets.size());

    for (int i = 1; i < categoryNames.size(); ++i)
    {
        const auto& categoryName = categoryNames.getReference(i);
        int count = 0;

        for (const auto& info : allPresets)
            if (info.category == categoryName)
                ++count;

        categoryCounts.add(count);
    }

    hoveredCategoryRow = -1;
    categoryList.updateContent();

    int row = wasSelected.isEmpty() ? 0 : categoryNames.indexOf(wasSelected);

    if (row < 0)
        row = 0;

    categoryList.selectRow(row, true, true);
}

void PresetBrowser::applyFilter()
{
    const int categoryRow = categoryList.getSelectedRow();
    const auto category =
        (categoryRow > 0 && categoryRow < categoryNames.size()) ? categoryNames[categoryRow] : juce::String();

    const auto* selected = getSelectedPreset();
    const auto keepName = selected != nullptr ? selected->name : juce::String();
    const auto keepCategory = selected != nullptr ? selected->category : juce::String();

    shownPresets = manager.search(searchField.getText().trim(), category);
    hoveredPresetRow = -1;

    presetList.deselectAllRows();
    presetList.updateContent();

    if (keepName.isNotEmpty())
        selectPreset(keepName, keepCategory);

    const int count = shownPresets.size();
    const auto countText = juce::String(count) + (count == 1 ? " preset" : " presets");

    if (countText != shownCountText)
    {
        shownCountText = countText;
        title.setTrailingText(countText);
    }

    // No repaint() here: filtering changes nothing this component draws. The
    // panel and the wells only move in resized(), the title repaints itself
    // above, and updateContent() repaints the list. Repainting the whole
    // overlay per keystroke would redraw the backdrop and the panel shadow.
    updateActionButtons();
}

void PresetBrowser::selectPreset(const juce::String& presetName, const juce::String& presetCategory)
{
    if (presetName.isEmpty())
        return;

    for (int i = 0; i < shownPresets.size(); ++i)
    {
        const auto& info = shownPresets.getReference(i);

        if (info.name == presetName && (presetCategory.isEmpty() || info.category == presetCategory))
        {
            presetList.selectRow(i, false, true);
            return;
        }
    }
}

const PresetManager::PresetInfo* PresetBrowser::getSelectedPreset() const
{
    const int row = presetList.getSelectedRow();

    if (!juce::isPositiveAndBelow(row, shownPresets.size()))
        return nullptr;

    return &shownPresets.getReference(row);
}

bool PresetBrowser::isSelectionEditable() const
{
    const auto* info = getSelectedPreset();
    return info != nullptr && !info->isFactory;
}

//==============================================================================
void PresetBrowser::loadSelectedPreset()
{
    if (promptMode != PromptMode::none)
        return;

    const auto* info = getSelectedPreset();

    if (info == nullptr)
        return;

    // Copied first: loading fires callbacks that can rebuild the array this
    // reference points into.
    const auto chosen = *info;

    if (manager.loadPreset(chosen))
    {
        notifyPresetChanged();
        dismiss();
    }
}

void PresetBrowser::updateActionButtons()
{
    const bool editable = isSelectionEditable();

    renameButton.setEnabled(editable);
    deleteButton.setEnabled(editable);

    renameButton.setTooltip(editable ? "Rename the selected user preset"
                                     : "Only user presets can be renamed - factory presets are read-only");
    deleteButton.setTooltip(editable ? "Delete the selected user preset"
                                     : "Only user presets can be deleted - factory presets are read-only");
}

//==============================================================================
juce::StringArray PresetBrowser::userCategoryNames() const
{
    juce::StringArray out;
    out.add("User");

    for (const auto& info : manager.getAllPresets())
        if (!info.isFactory && info.category.isNotEmpty())
            out.addIfNotAlreadyThere(info.category);

    out.sortNatural();
    return out;
}

bool PresetBrowser::userPresetExists(const juce::String& presetName, const juce::String& presetCategory) const
{
    const auto category = presetCategory.isEmpty() ? juce::String("User") : presetCategory;

    for (const auto& info : manager.getAllPresets())
        if (!info.isFactory && info.name == presetName && info.category == category)
            return true;

    return false;
}

void PresetBrowser::setPrompt(PromptMode mode)
{
    if (mode == PromptMode::rename || mode == PromptMode::removeAsked)
    {
        if (!isSelectionEditable())
            return;

        promptTarget = *getSelectedPreset();
    }

    promptMode = mode;

    const bool prompting = mode != PromptMode::none;

    saveButton.setVisible(!prompting);
    renameButton.setVisible(!prompting);
    deleteButton.setVisible(!prompting);
    folderButton.setVisible(!prompting);
    closeButton.setVisible(!prompting);

    promptLabel.setVisible(prompting);
    confirmButton.setVisible(prompting);
    cancelButton.setVisible(prompting);
    nameField.setVisible(mode == PromptMode::save || mode == PromptMode::rename);
    categoryField.setVisible(mode == PromptMode::save);

    promptLabel.setColour(juce::Label::textColourId, EmberColours::textSecondary());
    confirmButton.setEmberStyle(mode == PromptMode::removeAsked ? EmberButton::Style::danger
                                                                : EmberButton::Style::accent);

    if (mode == PromptMode::save)
    {
        const auto suggested =
            manager.getCurrentPresetName().isEmpty() ? juce::String("Untitled") : manager.getCurrentPresetName();
        const auto suggestedCategory =
            manager.getCurrentPresetCategory().isEmpty() || manager.getCurrentPresetCategory() == "Factory"
                ? juce::String("User")
                : manager.getCurrentPresetCategory();

        promptLabel.setText("Save preset as", juce::dontSendNotification);

        nameField.setText(suggested, false);
        nameField.selectAll();

        categoryField.clear(juce::dontSendNotification);
        categoryField.addItemList(userCategoryNames(), 1);
        categoryField.setText(suggestedCategory, juce::dontSendNotification);

        confirmButton.setButtonText(userPresetExists(suggested, suggestedCategory) ? "Replace" : "Save");
        confirmButton.setTooltip("Write the preset into the user folder");
    }
    else if (mode == PromptMode::rename)
    {
        promptLabel.setText("Rename \"" + promptTarget.name + "\"", juce::dontSendNotification);
        nameField.setText(promptTarget.name, false);
        nameField.selectAll();
        confirmButton.setButtonText("Rename");
        confirmButton.setTooltip("Rename the preset file");
    }
    else if (mode == PromptMode::removeAsked)
    {
        promptLabel.setText("Delete \"" + promptTarget.name + "\" from " + promptTarget.category + "?",
                            juce::dontSendNotification);
        confirmButton.setButtonText("Delete");
        confirmButton.setTooltip("Delete the preset file - this cannot be undone");
    }

    resized();
    repaint();

    if (mode == PromptMode::save || mode == PromptMode::rename)
        nameField.grabKeyboardFocus();
    else if (mode == PromptMode::removeAsked)
        grabKeyboardFocus(); // Return confirms, Escape cancels, through keyPressed
    else
        presetList.grabKeyboardFocus();
}

void PresetBrowser::confirmPrompt()
{
    if (promptMode == PromptMode::save)
    {
        const auto newName = nameField.getText().trim();

        if (newName.isEmpty())
        {
            showPromptError(promptLabel, "Give the preset a name");
            nameField.grabKeyboardFocus();
            return;
        }

        const auto newCategory =
            categoryField.getText().trim().isEmpty() ? juce::String("User") : categoryField.getText().trim();

        if (!manager.saveUserPreset(newName, newCategory))
        {
            showPromptError(promptLabel, "Could not write that preset - check the user folder is writable");
            return;
        }

        setPrompt(PromptMode::none);

        // Show what was just written, whatever the filter was doing before.
        searchField.setText({}, false);

        const int categoryRow = categoryNames.indexOf(newCategory);
        categoryList.selectRow(categoryRow > 0 ? categoryRow : 0, true, true);

        applyFilter();
        selectPreset(newName, newCategory);
        notifyPresetChanged();
        return;
    }

    if (promptMode == PromptMode::rename)
    {
        const auto newName = nameField.getText().trim();

        if (newName.isEmpty())
        {
            showPromptError(promptLabel, "Give the preset a name");
            nameField.grabKeyboardFocus();
            return;
        }

        if (newName == promptTarget.name)
        {
            setPrompt(PromptMode::none);
            return;
        }

        if (!manager.renameUserPreset(promptTarget, newName))
        {
            showPromptError(promptLabel, "Could not rename - \"" + newName + "\" may already exist there");
            return;
        }

        setPrompt(PromptMode::none);
        selectPreset(newName, promptTarget.category);
        notifyPresetChanged();
        return;
    }

    if (promptMode == PromptMode::removeAsked)
    {
        if (!manager.deleteUserPreset(promptTarget))
        {
            showPromptError(promptLabel, "Could not delete that file");
            return;
        }

        setPrompt(PromptMode::none);
        notifyPresetChanged();
        return;
    }

    // PromptMode::none: nothing to confirm.
}

void PresetBrowser::cancelPrompt()
{
    setPrompt(PromptMode::none);
}

void PresetBrowser::revealUserFolder()
{
    auto directory = PresetManager::getUserPresetDirectory();

    if (!directory.isDirectory() && !directory.createDirectory().wasOk())
        return;

    directory.revealToUser();
}

void PresetBrowser::dismiss()
{
    // The handler is PresetBar::hideBrowser, which clears this very member (and
    // then hands the browser to the message queue to be deleted). Calling
    // `onDismiss()` directly would therefore destroy the closure while it is
    // still running, so it is copied onto the stack and invoked from there.
    if (auto callback = onDismiss)
        callback();
}

void PresetBrowser::notifyPresetChanged()
{
    // Same reasoning as dismiss(): the owner is allowed to clear or replace
    // this callback from inside it.
    if (auto callback = onPresetChanged)
        callback();
}

//==============================================================================
float PresetBrowser::scale() const
{
    return uiScaleFor(*this);
}

void PresetBrowser::paint(juce::Graphics& g)
{
    g.fillAll(EmberColours::backgroundDeep().withAlpha(kBackdropAlpha));

    if (panelArea.getWidth() < 2.0f || panelArea.getHeight() < 2.0f)
        return;

    const float uiScale = scale();
    const float corner = juce::jlimit(4.0f, 14.0f, 9.0f * uiScale);

    // drawForRectangle builds and blurs an image every call, so it is skipped
    // when the damaged region lies inside the opaque part of the panel, where
    // the shadow could not show through anyway. That is the common case: the
    // lists have a transparent background, so every hovered row repaint brings
    // this paint routine with it.
    const auto clipped = g.getClipBounds().toFloat();

    if (!panelArea.reduced(corner + 1.0f).contains(clipped))
        juce::DropShadow(EmberTheme::tokens().panelShadow.withAlpha(0.55f), juce::roundToInt(20.0f * uiScale),
                         {0, juce::roundToInt(6.0f * uiScale)})
            .drawForRectangle(g, panelArea.toNearestInt());

    EmberLookAndFeel::drawPanel(g, panelArea, corner, false);

    EmberLookAndFeel::drawWell(g, categoryWell, corner * 0.55f);
    EmberLookAndFeel::drawWell(g, presetWell, corner * 0.55f);

    if (promptMode != PromptMode::none)
        EmberLookAndFeel::drawWell(g, promptWell, corner * 0.55f);
}

void PresetBrowser::resized()
{
    auto full = getLocalBounds().toFloat();

    if (full.getWidth() < 40.0f || full.getHeight() < 40.0f)
    {
        panelArea = {};
        return;
    }

    const float uiScale = scale();

    // The panel is a proportion of the window, clamped so it stays usable at
    // 800x480 and never sprawls on a 3000 px display.
    const float margin = juce::jlimit(10.0f, 32.0f, full.getWidth() * 0.04f);
    const float wantedWidth = juce::jlimit(300.0f, 780.0f, full.getWidth() * 0.68f);
    const float wantedHeight = juce::jlimit(220.0f, 580.0f, full.getHeight() * 0.82f);

    const float panelWidth = juce::jmin(wantedWidth, juce::jmax(120.0f, full.getWidth() - margin * 2.0f));
    const float panelHeight = juce::jmin(wantedHeight, juce::jmax(100.0f, full.getHeight() - margin * 2.0f));

    panelArea = juce::Rectangle<float>(panelWidth, panelHeight).withCentre(full.getCentre());

    const float pad = juce::jlimit(8.0f, 22.0f, 14.0f * uiScale);
    const float gap = juce::jlimit(4.0f, 12.0f, 7.0f * uiScale);
    const float rowHeight = juce::jlimit(20.0f, 38.0f, 26.0f * uiScale);

    // The action row is five buttons and three gaps wide. Above the minimum
    // window the proportional width wins; below it, the buttons shrink instead
    // of overlapping each other.
    const float contentWidth = juce::jmax(1.0f, panelWidth - pad * 2.0f);
    const float widestThatFits = juce::jmax(24.0f, (contentWidth - gap * 2.8f) / 5.0f);
    const float buttonWidth = juce::jmin(juce::jlimit(52.0f, 104.0f, panelWidth * 0.155f), widestThatFits);

    auto inner = panelArea.reduced(pad);

    // ---- header: title + count on the left, search on the right ------------
    auto headerRow = inner.removeFromTop(rowHeight);
    auto searchArea = headerRow.removeFromRight(juce::jlimit(100.0f, 250.0f, headerRow.getWidth() * 0.42f));
    searchField.setBounds(searchArea.reduced(0.0f, juce::jmax(1.0f, rowHeight * 0.08f)).toNearestInt());
    title.setBounds(headerRow.withTrimmedRight(gap).toNearestInt());

    inner.removeFromTop(gap);

    // ---- footer: either the action row or the open prompt -------------------
    const bool prompting = promptMode != PromptMode::none;
    const float footerHeight = prompting ? (rowHeight * 1.9f + gap * 2.0f) : rowHeight;

    auto footer = inner.removeFromBottom(juce::jmin(footerHeight, juce::jmax(0.0f, inner.getHeight() - rowHeight)));
    inner.removeFromBottom(gap);

    if (prompting)
    {
        promptWell = footer;

        auto promptInner = footer.reduced(juce::jmax(5.0f, pad * 0.5f), juce::jmax(4.0f, gap * 0.5f));
        auto labelRow = promptInner.removeFromTop(juce::jmin(promptInner.getHeight() * 0.42f, rowHeight * 0.8f));
        promptLabel.setBounds(labelRow.toNearestInt());

        promptInner.removeFromTop(gap * 0.5f);

        auto fieldRow = promptInner.removeFromTop(juce::jmin(promptInner.getHeight(), rowHeight));

        confirmButton.setBounds(fieldRow.removeFromRight(buttonWidth).toNearestInt());
        fieldRow.removeFromRight(gap * 0.6f);
        cancelButton.setBounds(fieldRow.removeFromRight(buttonWidth).toNearestInt());

        if (categoryField.isVisible())
        {
            fieldRow.removeFromRight(gap);
            categoryField.setBounds(
                fieldRow.removeFromRight(juce::jlimit(70.0f, 150.0f, fieldRow.getWidth() * 0.34f)).toNearestInt());
        }

        if (nameField.isVisible())
        {
            fieldRow.removeFromRight(gap);
            nameField.setBounds(fieldRow.toNearestInt());
        }
    }
    else
    {
        promptWell = {};

        auto actions = footer;

        saveButton.setBounds(actions.removeFromLeft(buttonWidth).toNearestInt());
        actions.removeFromLeft(gap * 0.6f);
        renameButton.setBounds(actions.removeFromLeft(buttonWidth).toNearestInt());
        actions.removeFromLeft(gap * 0.6f);
        deleteButton.setBounds(actions.removeFromLeft(buttonWidth).toNearestInt());

        closeButton.setBounds(actions.removeFromRight(buttonWidth).toNearestInt());
        actions.removeFromRight(gap * 0.6f);
        folderButton.setBounds(actions.removeFromRight(buttonWidth).toNearestInt());
    }

    // ---- body: categories | presets -----------------------------------------
    auto body = inner;
    const float categoryWidth = juce::jlimit(74.0f, 190.0f, body.getWidth() * 0.27f);

    categoryWell = body.removeFromLeft(categoryWidth);
    body.removeFromLeft(gap);
    presetWell = body;

    // reduced() on a well narrower than twice the inset would hand setBounds a
    // negative size, so the inset never eats more than half of either axis.
    const float inset = juce::jmax(2.0f, 3.0f * uiScale);

    const auto insetWell = [inset](juce::Rectangle<float> area)
    { return area.reduced(juce::jmin(inset, area.getWidth() * 0.5f), juce::jmin(inset, area.getHeight() * 0.5f)); };

    categoryList.setBounds(insetWell(categoryWell).toNearestInt());
    presetList.setBounds(insetWell(presetWell).toNearestInt());

    const int rowPixels = listRowHeight(uiScale);
    categoryList.setRowHeight(rowPixels);
    presetList.setRowHeight(rowPixels);
}

void PresetBrowser::parentSizeChanged()
{
    if (auto* parent = getParentComponent())
        setBounds(parent->getLocalBounds());
}

void PresetBrowser::visibilityChanged()
{
    if (!isVisible())
        return;

    // `addAndMakeVisible` makes the component visible BEFORE parenting it, so
    // focus is taken once the browser is genuinely on screen.
    const juce::Component::SafePointer<PresetBrowser> safeThis(this);

    juce::MessageManager::callAsync(
        [safeThis]
        {
            if (auto* self = safeThis.getComponent())
                if (self->isShowing())
                    self->searchField.grabKeyboardFocus();
        });
}

//==============================================================================
bool PresetBrowser::keyPressed(const juce::KeyPress& key)
{
    if (key.isKeyCode(juce::KeyPress::escapeKey))
    {
        if (promptMode != PromptMode::none)
            cancelPrompt();
        else
            dismiss();

        return true;
    }

    if (key.isKeyCode(juce::KeyPress::returnKey))
    {
        if (promptMode != PromptMode::none)
            confirmPrompt();
        else
            loadSelectedPreset();

        return true;
    }

    // Arrows work wherever the focus is — typing in the search field and then
    // walking the results is the fast path through a big library.
    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey) ||
        key.isKeyCode(juce::KeyPress::pageUpKey) || key.isKeyCode(juce::KeyPress::pageDownKey) ||
        key.isKeyCode(juce::KeyPress::homeKey) || key.isKeyCode(juce::KeyPress::endKey))
    {
        return presetList.keyPressed(key);
    }

    return false;
}

bool PresetBrowser::keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent)
{
    juce::ignoreUnused(originatingComponent);

    // Only the list movement is taken from the search field; everything else,
    // Return and Escape included, is left to the editor and then bubbles up to
    // the component keyPressed above.
    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey) ||
        key.isKeyCode(juce::KeyPress::pageUpKey) || key.isKeyCode(juce::KeyPress::pageDownKey))
    {
        return presetList.keyPressed(key);
    }

    return false;
}

void PresetBrowser::mouseDown(const juce::MouseEvent& event)
{
    // Clicks inside the lists reach this as a mouse-listener callback; only a
    // click on the backdrop itself should close anything.
    if (event.eventComponent != this)
        return;

    if (panelArea.contains(event.position))
        return;

    if (promptMode != PromptMode::none)
        cancelPrompt();
    else
        dismiss();
}

void PresetBrowser::mouseMove(const juce::MouseEvent& event)
{
    const auto updateHover = [](juce::ListBox& list, const juce::MouseEvent& e, int& hovered)
    {
        const auto position = e.getEventRelativeTo(&list).getPosition();
        const int row = list.getRowContainingPosition(position.x, position.y);

        if (row == hovered)
            return;

        const int previous = hovered;
        hovered = row;

        if (previous >= 0)
            list.repaintRow(previous);

        if (row >= 0)
            list.repaintRow(row);
    };

    const auto clearHover = [](juce::ListBox& list, int& hovered)
    {
        if (hovered < 0)
            return;

        const int previous = hovered;
        hovered = -1;
        list.repaintRow(previous);
    };

    auto* source = event.eventComponent;

    if (source == &presetList || presetList.isParentOf(source))
    {
        updateHover(presetList, event, hoveredPresetRow);
        clearHover(categoryList, hoveredCategoryRow);
    }
    else if (source == &categoryList || categoryList.isParentOf(source))
    {
        updateHover(categoryList, event, hoveredCategoryRow);
        clearHover(presetList, hoveredPresetRow);
    }
    else
    {
        clearHover(presetList, hoveredPresetRow);
        clearHover(categoryList, hoveredCategoryRow);
    }
}

void PresetBrowser::mouseExit(const juce::MouseEvent& event)
{
    auto* source = event.eventComponent;

    if (source == &presetList || presetList.isParentOf(source))
    {
        if (hoveredPresetRow >= 0)
        {
            const int previous = hoveredPresetRow;
            hoveredPresetRow = -1;
            presetList.repaintRow(previous);
        }
    }
    else if (source == &categoryList || categoryList.isParentOf(source))
    {
        if (hoveredCategoryRow >= 0)
        {
            const int previous = hoveredCategoryRow;
            hoveredCategoryRow = -1;
            categoryList.repaintRow(previous);
        }
    }
}

//==============================================================================
void PresetBrowser::paintCategoryRow(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (!juce::isPositiveAndBelow(rowNumber, categoryNames.size()))
        return;

    const auto area = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));

    if (area.getHeight() < 4.0f || area.getWidth() < 12.0f)
        return;

    const auto name = categoryNames[rowNumber];
    const bool hovered = rowNumber == hoveredCategoryRow;

    auto row = area.reduced(2.0f, 1.0f);
    const float corner = juce::jmin(4.0f, row.getHeight() * 0.3f);

    if (rowIsSelected)
    {
        g.setColour(EmberColours::accentGlow());
        g.fillRoundedRectangle(row, corner);
    }
    else if (hovered)
    {
        g.setColour(EmberColours::panelRaised().withAlpha(0.55f));
        g.fillRoundedRectangle(row, corner);
    }

    if (rowIsSelected)
    {
        const float barWidth = juce::jmax(1.5f, row.getHeight() * 0.10f);
        g.setColour(EmberColours::accent());
        g.fillRoundedRectangle(row.withWidth(barWidth).reduced(0.0f, row.getHeight() * 0.18f), barWidth * 0.5f);
    }

    // How many presets this category holds, so the column carries information
    // rather than just being a filter. Cached by rebuildCategories(): counting
    // here would rescan the whole library once per row, per repaint.
    const int count =
        juce::isPositiveAndBelow(rowNumber, categoryCounts.size()) ? categoryCounts.getUnchecked(rowNumber) : 0;

    const float pad = textPaddingFor(row.getHeight());
    auto text = row.reduced(pad, 0.0f);

    const auto countFont = EmberFonts::forHeight(row.getHeight(), 0.40f, false);
    const auto countText = juce::String(count);
    const float countWidth = juce::GlyphArrangement::getStringWidth(countFont, countText) + 2.0f;

    if (text.getWidth() > countWidth * 2.0f)
    {
        g.setFont(countFont);
        g.setColour(rowIsSelected ? EmberColours::accentDim() : EmberColours::textDisabled());
        g.drawText(countText, text.removeFromRight(countWidth), juce::Justification::centredRight, false);
        text.removeFromRight(pad * 0.5f);
    }

    g.setFont(EmberFonts::forHeight(row.getHeight(), 0.46f, false));
    g.setColour(rowIsSelected ? EmberColours::accent()
                              : (hovered ? EmberColours::textPrimary() : EmberColours::textSecondary()));
    g.drawFittedText(name, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}

void PresetBrowser::paintPresetRow(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (!juce::isPositiveAndBelow(rowNumber, shownPresets.size()))
        return;

    const auto area = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));

    if (area.getHeight() < 4.0f || area.getWidth() < 16.0f)
        return;

    const auto& info = shownPresets.getReference(rowNumber);
    const bool hovered = rowNumber == hoveredPresetRow;
    const bool isCurrent =
        info.name == manager.getCurrentPresetName() && info.category == manager.getCurrentPresetCategory();

    // A hairline wherever the factory block meets the user block.
    if (rowNumber > 0 && shownPresets.getReference(rowNumber - 1).isFactory != info.isFactory)
        EmberLookAndFeel::drawHairline(g, area.withHeight(1.0f), EmberColours::outline());

    auto row = area.reduced(2.0f, 1.0f);
    const float corner = juce::jmin(4.0f, row.getHeight() * 0.3f);

    if (rowIsSelected)
    {
        g.setColour(EmberColours::accentGlow());
        g.fillRoundedRectangle(row, corner);
        g.setColour(EmberColours::accent().withAlpha(0.5f));
        g.drawRoundedRectangle(row.reduced(0.5f), corner, 1.0f);
    }
    else if (hovered)
    {
        g.setColour(EmberColours::panelRaised().withAlpha(0.55f));
        g.fillRoundedRectangle(row, corner);
    }

    const float pad = textPaddingFor(row.getHeight());
    auto text = row.reduced(pad, 0.0f);

    // Ownership marker: a hollow ring for read-only factory content, a filled
    // ember dot for the user's own, bright for whatever is loaded right now.
    const float dot = juce::jlimit(4.0f, 9.0f, row.getHeight() * 0.32f);
    auto markerArea = text.removeFromLeft(dot * 2.0f);
    const auto marker =
        juce::Rectangle<float>(dot, dot).withCentre({markerArea.getX() + dot * 0.55f, markerArea.getCentreY()});

    if (info.isFactory)
    {
        g.setColour(isCurrent ? EmberColours::accent() : EmberColours::textDisabled());
        g.drawEllipse(marker.reduced(0.5f), juce::jmax(1.0f, dot * 0.16f));
    }
    else
    {
        g.setColour(isCurrent ? EmberColours::accent() : EmberColours::accentDim());
        g.fillEllipse(marker);
    }

    // Category tag, right aligned: grey for factory, ember for user.
    const auto tagFont = EmberFonts::forHeight(row.getHeight(), 0.38f, false);
    const auto tag = tagTextFor(info);
    const float tagWidth = juce::GlyphArrangement::getStringWidth(tagFont, tag) + 2.0f;

    if (text.getWidth() > tagWidth * 1.8f)
    {
        g.setFont(tagFont);
        g.setColour(info.isFactory ? EmberColours::textDisabled() : EmberColours::accentDim());
        g.drawText(tag, text.removeFromRight(tagWidth), juce::Justification::centredRight, false);
        text.removeFromRight(pad * 0.6f);
    }

    g.setFont(EmberFonts::forHeight(row.getHeight(), 0.48f, false));

    if (isCurrent)
        g.setColour(EmberColours::accent());
    else if (rowIsSelected || hovered)
        g.setColour(EmberColours::textPrimary());
    else
        g.setColour(info.isFactory ? EmberColours::textSecondary() : EmberColours::textPrimary());

    g.drawFittedText(info.name, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}

//==============================================================================
// PresetBar::ChevronButton
//==============================================================================
PresetBar::ChevronButton::ChevronButton(bool pointsRight) : juce::Button({}), right(pointsRight)
{
    setTooltip(pointsRight ? "Next preset" : "Previous preset");
}

void PresetBar::ChevronButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                                           bool shouldDrawButtonAsDown)
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 3.0f || area.getHeight() < 3.0f)
        return;

    if (shouldDrawButtonAsHighlighted && isEnabled())
    {
        g.setColour(EmberColours::panelRaised().withAlpha(shouldDrawButtonAsDown ? 0.9f : 0.6f));
        g.fillRoundedRectangle(area.reduced(area.getWidth() * 0.14f, area.getHeight() * 0.16f),
                               juce::jmax(2.0f, area.getHeight() * 0.18f));
    }

    auto colour = EmberColours::textSecondary();

    if (!isEnabled())
        colour = EmberColours::textDisabled();
    else if (shouldDrawButtonAsDown)
        colour = EmberColours::accent();
    else if (shouldDrawButtonAsHighlighted)
        colour = EmberColours::textPrimary();

    const float size = juce::jmax(2.5f, juce::jmin(area.getWidth(), area.getHeight()) * 0.22f);
    const auto centre = area.getCentre();
    const float reach = right ? size * 0.62f : -size * 0.62f;

    juce::Path path;
    path.startNewSubPath(centre.x - reach, centre.y - size);
    path.lineTo(centre.x + reach, centre.y);
    path.lineTo(centre.x - reach, centre.y + size);

    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(juce::jmax(1.0f, size * 0.34f), juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
}

//==============================================================================
// PresetBar::NameDisplay
//==============================================================================
PresetBar::NameDisplay::NameDisplay() : juce::Button({})
{
    setTooltip("Click to browse presets");
}

void PresetBar::NameDisplay::setPresetText(const juce::String& presetName, const juce::String& presetCategory,
                                           bool isModified)
{
    name = presetName;
    category = presetCategory;
    modified = isModified;
    repaint();
}

void PresetBar::NameDisplay::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                                         bool shouldDrawButtonAsDown)
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 12.0f || area.getHeight() < 6.0f)
        return;

    if (shouldDrawButtonAsHighlighted)
    {
        g.setColour(EmberColours::panelRaised().withAlpha(shouldDrawButtonAsDown ? 0.85f : 0.55f));
        g.fillRoundedRectangle(area.reduced(1.0f), juce::jmax(2.0f, area.getHeight() * 0.22f));
    }

    const float pad = juce::jmax(4.0f, area.getHeight() * 0.26f);
    auto content = area.reduced(pad, 0.0f);

    // Trailing hint that this opens the browser.
    auto chevronArea = content.removeFromRight(juce::jlimit(7.0f, 16.0f, area.getHeight() * 0.40f));
    drawDownChevron(g, chevronArea,
                    shouldDrawButtonAsHighlighted ? EmberColours::textSecondary() : EmberColours::textDisabled());

    const auto categoryFont = EmberFonts::forHeight(area.getHeight(), 0.38f, false);
    const auto categoryText = category.toUpperCase();

    if (categoryText.isNotEmpty())
    {
        const float categoryWidth = juce::GlyphArrangement::getStringWidth(categoryFont, categoryText) + pad;

        if (content.getWidth() > categoryWidth * 2.2f)
        {
            content.removeFromRight(pad * 0.4f);
            g.setFont(categoryFont);
            g.setColour(EmberColours::textDisabled());
            g.drawText(categoryText, content.removeFromRight(categoryWidth), juce::Justification::centredRight, false);
        }
    }

    // The modified dot sits right after the name, where it reads as part of it.
    const float dot = juce::jlimit(3.0f, 7.0f, area.getHeight() * 0.18f);
    const float dotSlot = modified ? dot * 2.2f : 0.0f;

    const auto nameFont = EmberFonts::forHeight(area.getHeight(), 0.50f, false);
    const float nameWidth = juce::GlyphArrangement::getStringWidth(nameFont, name) + 2.0f;
    auto nameArea = content.removeFromLeft(juce::jmax(0.0f, juce::jmin(nameWidth, content.getWidth() - dotSlot)));

    g.setFont(nameFont);
    g.setColour(shouldDrawButtonAsHighlighted ? EmberColours::textPrimary()
                                              : EmberColours::textPrimary().withMultipliedBrightness(0.94f));
    g.drawFittedText(name.isEmpty() ? juce::String("Init") : name, nameArea.toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.8f);

    if (modified)
    {
        const auto dotArea =
            juce::Rectangle<float>(dot, dot).withCentre({content.getX() + dot * 1.1f, content.getCentreY()});
        g.setColour(EmberColours::accent());
        g.fillEllipse(dotArea);
    }
}

//==============================================================================
// PresetBar
//==============================================================================
PresetBar::PresetBar(EmberAudioProcessor& processorToUse)
    : processor(processorToUse), manager(processorToUse.getPresetManager()), wordmark(processorToUse)
{
    previousButton.onClick = [this] { manager.loadPrevious(); };
    nextButton.onClick = [this] { manager.loadNext(); };
    nameDisplay.onClick = [this] { showBrowser(); };

    wordmark.setComponentID(tutorial::TourTargets::logo);
    previousButton.setComponentID(tutorial::TourTargets::presetPrevious);
    nextButton.setComponentID(tutorial::TourTargets::presetNext);
    nameDisplay.setComponentID(tutorial::TourTargets::presetName);
    addAndMakeVisible(wordmark);
    addAndMakeVisible(previousButton);
    addAndMakeVisible(nameDisplay);
    addAndMakeVisible(nextButton);

    installManagerCallbacks();

    // The name and category arrive through the manager's callbacks; the poll is
    // only here for isModified(), which has no callback of its own. Reading a
    // bool eight times a second costs nothing and keeps the dot honest.
    refreshDisplay();
    startTimerHz(8);
}

PresetBar::~PresetBar()
{
    stopTimer();

    // The browser goes first so the manager's callbacks unwind in the order
    // they were chained: browser, then bar, then whatever was there before.
    browser.reset();
    restoreManagerCallbacks();
}

void PresetBar::installManagerCallbacks()
{
    // See PresetBrowser::installManagerCallbacks: chaining onto our own lambda
    // would recurse for ever.
    if (callbacksInstalled)
        return;

    const juce::Component::SafePointer<PresetBar> safeThis(this);

    previousListChanged = manager.onPresetListChanged;
    previousPresetLoaded = manager.onPresetLoaded;
    callbacksInstalled = true;

    auto chainedListChanged = previousListChanged;
    manager.onPresetListChanged = [safeThis, chainedListChanged]
    {
        if (chainedListChanged)
            chainedListChanged();

        if (auto* self = safeThis.getComponent())
            self->refreshDisplay();
    };

    auto chainedLoaded = previousPresetLoaded;
    manager.onPresetLoaded = [safeThis, chainedLoaded]
    {
        if (chainedLoaded)
            chainedLoaded();

        if (auto* self = safeThis.getComponent())
            self->refreshDisplay();
    };
}

void PresetBar::restoreManagerCallbacks()
{
    if (!callbacksInstalled)
        return;

    callbacksInstalled = false;

    manager.onPresetListChanged = previousListChanged;
    manager.onPresetLoaded = previousPresetLoaded;

    previousListChanged = nullptr;
    previousPresetLoaded = nullptr;
}

void PresetBar::timerCallback()
{
    refreshDisplay();
}

void PresetBar::refreshDisplay()
{
    const auto name = manager.getCurrentPresetName();
    const auto category = manager.getCurrentPresetCategory();
    const bool modified = manager.isModified();

    if (name == shownName && category == shownCategory && modified == shownModified)
        return;

    const bool identityChanged = name != shownName || category != shownCategory;

    shownName = name;
    shownCategory = category;
    shownModified = modified;

    nameDisplay.setPresetText(name, category, modified);

    juce::String tip;
    tip << (name.isEmpty() ? juce::String("Init") : name);

    if (category.isNotEmpty())
        tip << " - " << category;

    if (modified)
        tip << "\nEdited since it was loaded";

    tip << "\nClick to browse presets";
    nameDisplay.setTooltip(tip);

    if (identityChanged && onPresetChanged)
        onPresetChanged();
}

void PresetBar::showBrowser()
{
    if (browser != nullptr)
    {
        browser->toFront(true);
        return;
    }

    auto* host = getTopLevelComponent();

    if (host == nullptr)
        return;

    browser = std::make_unique<PresetBrowser>(processor);
    browser->onDismiss = [this] { hideBrowser(); };
    browser->onPresetChanged = [this] { refreshDisplay(); };
    browser->setBounds(host->getLocalBounds());

    host->addAndMakeVisible(*browser);
    browser->toFront(true);
}

void PresetBar::hideBrowser()
{
    if (browser == nullptr)
        return;

    browser->onDismiss = nullptr;
    browser->onPresetChanged = nullptr;
    browser->setVisible(false);

    // Unchain from the preset manager now, while the order is still known,
    // rather than whenever the deferred delete happens to run.
    browser->detachFromManager();

    // Take it out of the editor before the delete is deferred, so the object
    // waiting on the message queue is fully detached: it receives no further
    // events, and it no longer matters whether the editor (and the shared
    // look-and-feel it points at) outlives the queued deletion.
    if (auto* host = browser->getParentComponent())
        host->removeChildComponent(browser.get());

    // This is normally called from inside one of the browser's own mouse or key
    // handlers, so the object is handed to the message queue instead of being
    // deleted underneath the callback that is still running.
    std::shared_ptr<PresetBrowser> doomed(std::move(browser));
    juce::MessageManager::callAsync([doomed]() mutable { doomed.reset(); });

    refreshDisplay();
}

void PresetBar::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 4.0f || area.getHeight() < 4.0f)
        return;

    EmberLookAndFeel::drawWell(g, area.reduced(0.5f), juce::jlimit(3.0f, 10.0f, area.getHeight() * 0.26f));
}

void PresetBar::resized()
{
    auto area = getLocalBounds();

    const int chevronWidth = juce::jlimit(14, 44, juce::roundToInt(static_cast<float>(area.getHeight()) * 0.80f));

    // The logo takes the left end, but only where there is room for it to be
    // legible - below that the preset name matters more than the branding.
    const int logoWidth = wordmark.preferredWidth(area.getHeight());

    // The preset name needs room to be read; the logo yields to it. "Three
    // times the logo" was far too cautious and hid the logo at every real
    // header width - it only has to leave the name a usable share.
    const bool showLogo = area.getWidth() > logoWidth + 170;

    wordmark.setVisible(showLogo);

    if (showLogo)
    {
        wordmark.setBounds(area.removeFromLeft(logoWidth));
        area.removeFromLeft(juce::jmax(6, area.getHeight() / 3));
    }

    previousButton.setBounds(area.removeFromLeft(chevronWidth));
    nextButton.setBounds(area.removeFromRight(chevronWidth));
    nameDisplay.setBounds(area);
}
} // namespace ember::gui
