#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/**
    Ember's visual foundation: the palette, the type scale and the LookAndFeel
    that every panel in the plugin shares.

    Nothing in this file knows about the processor, parameters or DSP — it is
    pure presentation, so it can be unit-drawn, screenshotted and reasoned about
    on its own. Controls live in Widgets.h and build on top of this.

    HOW TO USE IT FROM A PANEL
    --------------------------
      - Colours: `ember::gui::EmberColours::panel`, `::accent`, `::band (i)`.
        Never invent a colour locally; if something is missing, add it here so
        every panel changes together.
      - Fonts:   `ember::gui::EmberFonts::get (Role::section, uiScale)` for the
        type scale, or `EmberFonts::forHeight (h)` to fit a control.
      - The editor owns ONE `EmberLookAndFeel` and calls
        `juce::LookAndFeel::setDefaultLookAndFeel (&lnf)` (or `setLookAndFeel`
        on the top-level editor component) before adding children. Panels do not
        create their own.
      - To tint a control with the band colour instead of the global accent,
        call `EmberStyleProps::setAccentColour (component, EmberColours::band (b))`.
        Every drawing routine here honours that override.

    DRAWING RULES THIS FILE FOLLOWS (keep them if you extend it)
    -----------------------------------------------------------
      - Vector only. No images, no external fonts, no assets.
      - Nothing is positioned in absolute pixels: every radius, thickness and
        text size is derived from the bounds it is given, so the same code looks
        deliberate on a 28 px knob and a 96 px one.
      - The accent colour is an accent: it marks the selected band, a value in
        motion, and modulation. Everything else is grey.
*/
namespace ember::gui
{
//==============================================================================
/**
    The whole palette, in one place.

    Static members (not constexpr) because `juce::Colour` is not a literal type;
    they are defined once in EmberLookAndFeel.cpp. Read them from component
    constructors and paint routines — do not copy them into namespace-scope
    statics of your own translation unit, or you inherit a static-initialisation
    order dependency for no benefit.
*/
struct EmberColours
{
    // ---- surfaces, darkest to lightest -------------------------------------
    static const juce::Colour backgroundDeep; ///< behind everything; the editor's fill
    static const juce::Colour background;     ///< default component background
    static const juce::Colour panelSunken;    ///< wells: meters, displays, text fields
    static const juce::Colour panel;          ///< a card / grouped region
    static const juce::Colour panelRaised;    ///< a control sitting on a card

    // ---- lines -------------------------------------------------------------
    static const juce::Colour outline;       ///< hairlines and control borders
    static const juce::Colour outlineStrong; ///< region separators, focused borders
    static const juce::Colour track;         ///< unfilled slider/knob track

    // ---- text --------------------------------------------------------------
    static const juce::Colour textPrimary;   ///< values, button labels
    static const juce::Colour textSecondary; ///< captions, units, secondary rows
    static const juce::Colour textDisabled;  ///< greyed-out controls

    // ---- the one accent ----------------------------------------------------
    static const juce::Colour accent;     ///< warm amber, #FF8A3D
    static const juce::Colour accentDim;  ///< the same hue, held back
    static const juce::Colour accentGlow; ///< highlight wash / hover fills
    static const juce::Colour warning;    ///< clipping, destructive menu items

    // ---- per-band ramp -----------------------------------------------------
    /** Number of distinct band hues; matches `ember::kMaxBands`. */
    static constexpr int kNumBandColours = 6;

    /** Colour for band `bandIndex` (0-based, clamped into range).

        The ramp is flame temperature: band 0 is a deep ember red and band 5 a
        blue-white flame tip, so the low band reads as the heaviest and the air
        band as the coolest without any legend. The middle of the ramp sits on
        the accent hue, which is why the two never fight. */
    static juce::Colour band(int bandIndex) noexcept;

    /** The band colour held back for fills and inactive states. */
    static juce::Colour bandDim(int bandIndex) noexcept;
};

//==============================================================================
/** How an Ember toggle is drawn. Set with `EmberStyleProps::setToggleLook`. */
enum class ToggleLook
{
    pill = 0, ///< sliding capsule switch, text to the right
    led,      ///< small round lamp, text to the right
    check     ///< square tick box, text to the right
};

//==============================================================================
/**
    Per-component styling hints.

    These live in `juce::Component::getProperties()`, so they work on any JUCE
    component without subclassing and survive being set before or after the
    component is attached. The look-and-feel reads them while drawing.
*/
struct EmberStyleProps
{
    /** Overrides the accent colour used when drawing this component (knob value
        arc, toggle lamp, meter fill, section rule). Band panels set this to
        `EmberColours::band (b)`. */
    static void setAccentColour(juce::Component& component, juce::Colour colour);

    /** Removes an override set by `setAccentColour`. */
    static void clearAccentColour(juce::Component& component);

    /** The component's accent override, or `EmberColours::accent` if it has none. */
    static juce::Colour accentColourFor(const juce::Component& component);

    /** Chooses how `drawToggleButton` renders this button. Default: `pill`. */
    static void setToggleLook(juce::Button& button, ToggleLook look);
    static ToggleLook toggleLookFor(const juce::Button& button);
};

//==============================================================================
/**
    Ember's type scale.

    Sizes are given for a reference editor height of 640 px (the default) and
    multiplied by `uiScale`, so text grows with the window instead of being
    re-specified per panel. Use `scaleFor (editorHeight)` once per resize and
    pass the result around, or read `EmberLookAndFeel::getUiScale()`.
*/
struct EmberFonts
{
    enum class Role
    {
        display = 0, ///< the plugin name, once
        title,       ///< panel titles
        section,     ///< small-caps group headers
        body,        ///< running text, menu items
        label,       ///< control captions
        value,       ///< numeric readouts
        micro        ///< meter scales, footnotes
    };

    /** The height the scale is authored against. */
    static constexpr float kReferenceHeight = 640.0f;

    /** UI scale for an editor of `editorHeight` px, clamped to a sane 0.72–2.0
        so an extreme window never produces unreadable or absurd text. */
    static float scaleFor(float editorHeight) noexcept;

    /** Point size for a role at a given UI scale. */
    static float sizeFor(Role role, float uiScale = 1.0f) noexcept;

    /** Font for a role at a given UI scale. */
    static juce::Font get(Role role, float uiScale = 1.0f);

    /** A font that fits a control `componentHeight` px tall: `proportion` of the
        height, clamped to the legible range. This is what the look-and-feel uses
        for buttons, combo boxes and labels, so a control's text always tracks
        the box it lives in. */
    static juce::Font forHeight(float componentHeight, float proportion = 0.52f, bool bold = false);

    /** A plain font of an exact size, for the rare case you have measured it. */
    static juce::Font sized(float pointHeight, bool bold = false);
};

//==============================================================================
/**
    Geometry of an Ember rotary control, shared by the look-and-feel (which
    draws track, value arc and pointer) and `ModulatableKnob` (which draws the
    modulation ring in the reserved outer band).

    Both must derive their geometry from the SAME rectangle to line up: that
    rectangle is `LookAndFeel::getSliderLayout (slider).sliderBounds`, which for
    a rotary slider with no text box is simply the slider's local bounds.

    Angles follow JUCE's convention — radians, clockwise, zero at 12 o'clock.
*/
struct RotaryGeometry
{
    juce::Point<float> centre;    ///< centre of the knob
    float outerRadius{0.0f};      ///< radius of the largest circle that fits
    float modRingRadius{0.0f};    ///< centre-line radius of the modulation ring
    float modRingThickness{0.0f}; ///< stroke width for the modulation ring
    float arcRadius{0.0f};        ///< centre-line radius of track and value arcs
    float trackThickness{0.0f};   ///< stroke width of the unfilled track
    float valueThickness{0.0f};   ///< stroke width of the filled value arc
    float bodyRadius{0.0f};       ///< radius of the knob cap the pointer sits on
    float pointerThickness{0.0f}; ///< stroke width of the pointer line

    /** Square area the knob body occupies, for hit-testing or extra decoration. */
    juce::Rectangle<float> bodyBounds() const noexcept;
};

//==============================================================================
/**
    The plugin's look-and-feel.

    Every JUCE ColourId the plugin uses is given a value in the constructor, so
    nothing anywhere falls back to the stock JUCE appearance. If you add a
    control type, add its colours here rather than calling `setColour` on the
    component.
*/
class EmberLookAndFeel : public juce::LookAndFeel_V4
{
public:
    EmberLookAndFeel();
    ~EmberLookAndFeel() override;

    //==========================================================================
    /** The editor calls this from `resized()` with `getHeight()`. It only
        affects the type scale and corner radii; it never invalidates anything,
        so follow it with a `repaint()` on the editor. */
    void setUiScaleForEditorHeight(float editorHeight) noexcept;

    /** Current UI scale (1.0 at the 640 px reference height). */
    float getUiScale() const noexcept { return uiScale; }

    //==========================================================================
    // Shared drawing helpers. Panels should use these instead of hand-rolling
    // surfaces, so every card in the plugin has the same corner and edge.

    /** Fills a card: subtle vertical gradient plus a hairline border. */
    static void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, float cornerSize, bool raised = false);

    /** Fills a well (meters, displays, text fields): darker than the panel it
        sits on, with an inner top edge so it reads as recessed. */
    static void drawWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerSize);

    /** A one-pixel (device-independent) separator line. */
    static void drawHairline(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour,
                             bool vertical = false);

    /** Rotary geometry for a slider area — see `RotaryGeometry`. */
    static RotaryGeometry rotaryGeometry(juce::Rectangle<float> sliderArea) noexcept;

    /** Angle in radians for a 0–1 proportion across a rotary's sweep. */
    static float rotaryAngle(float proportion, float startAngle, float endAngle) noexcept;

    /** An arc centred on `centre`, stroked at `radius`, from one angle to
        another. Returns an empty path for a zero-length sweep. */
    static juce::Path arcPath(juce::Point<float> centre, float radius, float fromAngle, float toAngle);

    /** The rotary sweep Ember uses: 270°, from 7:30 round to 4:30. Apply with
        `slider.setRotaryParameters (kRotaryStartAngle, kRotaryEndAngle, true)` —
        `ModulatableKnob` already does. */
    static const float kRotaryStartAngle;
    static const float kRotaryEndAngle;

    //==========================================================================
    // juce::LookAndFeel overrides. Documented only where the behaviour is not
    // the obvious one.

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;

    int getSliderThumbRadius(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;
    juce::Font getSliderPopupFont(juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    /** Honours `EmberStyleProps::setToggleLook`; defaults to the pill switch. */
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;
    void drawTickBox(juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool isEnabled,
                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackgroundWithOptions(juce::Graphics&, int width, int height,
                                            const juce::PopupMenu::Options&) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText, const juce::Drawable* icon,
                           const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;

    void drawLabel(juce::Graphics&, juce::Label&) override;
    juce::Font getLabelFont(juce::Label&) override;

    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;

    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

private:
    float uiScale{1.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmberLookAndFeel)
};
} // namespace ember::gui
