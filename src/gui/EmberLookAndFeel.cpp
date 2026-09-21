#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include <BinaryData.h>
#include <algorithm>

#include <cmath>

namespace ember::gui
{
//==============================================================================
// Palette.
//
// EmberColours is now a view onto EmberTheme's tokens rather than a second
// palette. Keeping the old names means the redesign does not have to touch 298
// call sites to change how the plugin looks, and it leaves exactly one place
// where a colour is decided.
//
// These are static const, so they capture the default variant at load time. The
// "Cool Ember" variant therefore reaches a component only once that component
// has been rebuilt to read EmberTheme::tokens() directly - which is the order
// the rebuild proceeds in anyway.
//==============================================================================
namespace
{
const ThemeTokens& tk()
{
    return EmberTheme::tokens();
}
} // namespace

const juce::Colour EmberColours::backgroundDeep{tk().bgDeep};
const juce::Colour EmberColours::background{tk().panel};
const juce::Colour EmberColours::panelSunken{tk().bgDeep.brighter(0.02f)};
const juce::Colour EmberColours::panel{tk().panel};
const juce::Colour EmberColours::panelRaised{tk().panelRaised};

const juce::Colour EmberColours::outline{tk().panelEdge};
const juce::Colour EmberColours::outlineStrong{tk().panelEdge.brighter(0.18f)};
const juce::Colour EmberColours::track{tk().panelEdge.brighter(0.10f)};

const juce::Colour EmberColours::textPrimary{tk().text};
const juce::Colour EmberColours::textSecondary{tk().textDim};
const juce::Colour EmberColours::textDisabled{tk().textMuted};

const juce::Colour EmberColours::accent{tk().ember};
const juce::Colour EmberColours::accentDim{tk().emberDeep};
const juce::Colour EmberColours::accentGlow{tk().ember.withAlpha(0.20f)};
const juce::Colour EmberColours::warning{tk().danger};

namespace
{
/** Band tint by position.

    The old ramp gave each band its own hue and ran deep red to blue-white. That
    is gone: a blue-white band would now read as "modulation", and hue is
    reserved for heat. Bands instead sit at even steps along the ember ramp, so
    the set stays unmistakably warm and a band is told apart by its number and
    its position on screen.

    Once heat is plumbed through, a band's live tint comes from its own heat and
    this becomes only the resting colour. */
juce::Colour bandRampColour(int index)
{
    constexpr int last = EmberColours::kNumBandColours - 1;
    const auto position = static_cast<float>(juce::jlimit(0, last, index)) / static_cast<float>(last);
    return EmberTheme::tokens().heatTint(position);
}

/** Component property keys. Function-local statics so there is no
    static-initialisation order dependency between translation units. */
const juce::Identifier& accentPropertyId()
{
    static const juce::Identifier id("emberAccentColour");
    return id;
}

const juce::Identifier& toggleLookPropertyId()
{
    static const juce::Identifier id("emberToggleLook");
    return id;
}

/** Where a value arc starts from: the bottom of the range normally, the zero
    point for a bipolar control so cut and boost read differently. */
float valueArcAnchor(juce::Slider& slider)
{
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
        return static_cast<float>(juce::jlimit(0.0, 1.0, slider.valueToProportionOfLength(0.0)));

    return 0.0f;
}

bool isBipolar(const juce::Slider& slider) noexcept
{
    return slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
}

/** A vertical two-stop gradient, used for every raised surface in the plugin. */
juce::ColourGradient surfaceGradient(juce::Rectangle<float> area, juce::Colour base, float lift)
{
    return juce::ColourGradient(base.brighter(lift), area.getCentreX(), area.getY(), base.darker(lift * 1.6f),
                                area.getCentreX(), area.getBottom(), false);
}

/** Rounded-corner radius for a control of a given height. */
float cornerFor(float height, float scale)
{
    return juce::jlimit(2.0f, 8.0f * scale, height * 0.26f);
}

/** Lays out tooltip text. Goes through juce::TextLayout rather than
    `drawFittedText` so that embedded newlines — which every Ember tooltip uses
    to separate the value from the interaction hints — wrap correctly, and so
    that measuring and drawing cannot disagree. */
juce::TextLayout layoutTooltipText(const juce::String& text, const juce::Font& font, juce::Colour colour,
                                   float maximumWidth)
{
    juce::AttributedString attributed;
    attributed.setJustification(juce::Justification::centredLeft);
    attributed.append(text, font, colour);

    juce::TextLayout layout;
    layout.createLayout(attributed, maximumWidth);
    return layout;
}
} // namespace

//==============================================================================
juce::Colour EmberColours::band(int bandIndex) noexcept
{
    return bandRampColour(bandIndex);
}

juce::Colour EmberColours::bandDim(int bandIndex) noexcept
{
    return band(bandIndex).withMultipliedSaturation(0.62f).withMultipliedBrightness(0.55f);
}

//==============================================================================
void EmberStyleProps::setAccentColour(juce::Component& component, juce::Colour colour)
{
    component.getProperties().set(accentPropertyId(), static_cast<juce::int64>(colour.getARGB()));
}

void EmberStyleProps::clearAccentColour(juce::Component& component)
{
    component.getProperties().remove(accentPropertyId());
}

juce::Colour EmberStyleProps::accentColourFor(const juce::Component& component)
{
    if (const auto* value = component.getProperties().getVarPointer(accentPropertyId()))
        return juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(*value)));

    return EmberColours::accent;
}

void EmberStyleProps::setToggleLook(juce::Button& button, ToggleLook look)
{
    button.getProperties().set(toggleLookPropertyId(), static_cast<int>(look));
}

ToggleLook EmberStyleProps::toggleLookFor(const juce::Button& button)
{
    if (const auto* value = button.getProperties().getVarPointer(toggleLookPropertyId()))
        return static_cast<ToggleLook>(juce::jlimit(0, 2, static_cast<int>(*value)));

    return ToggleLook::pill;
}

//==============================================================================
namespace
{
/** The four embedded faces, loaded once.

    Ember ships its own type so it renders identically on Windows, macOS and
    Linux rather than inheriting whatever the host calls a UI font. If loading
    fails the roles fall back to system faces and `usingEmbeddedFaces()` reports
    false, so a screenshot test can say why it does not match. */
juce::Typeface::Ptr loadFace(const char* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
}
} // namespace

juce::Typeface::Ptr EmberFonts::embeddedFace(Family family)
{
    struct Faces
    {
        juce::Typeface::Ptr interMedium{loadFace(BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize)};
        juce::Typeface::Ptr interSemiBold{loadFace(BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)};
        juce::Typeface::Ptr barlowMedium{
            loadFace(BinaryData::BarlowCondensedMedium_ttf, BinaryData::BarlowCondensedMedium_ttfSize)};
        juce::Typeface::Ptr barlowSemiBold{
            loadFace(BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize)};
    };

    static const Faces faces;

    switch (family)
    {
    case Family::interMedium:
        return faces.interMedium;
    case Family::interSemiBold:
        return faces.interSemiBold;
    case Family::barlowMedium:
        return faces.barlowMedium;
    case Family::barlowSemiBold:
        return faces.barlowSemiBold;
    }

    return faces.interMedium;
}

/** Enables tabular figures where the face offers them.

    Without `tnum` a value re-flows while it is being dragged, because "1" is
    narrower than "8". That reads as the layout twitching, and it is the most
    noticeable typographic defect in an interface full of live numbers. */
juce::Font EmberFonts::withTabularFigures(juce::Font f)
{
    if (auto tf = f.getTypefacePtr())
    {
        const auto supported = tf->getSupportedFeatures();
        const juce::FontFeatureTag tnum{"tnum"};

        if (std::find(supported.begin(), supported.end(), tnum) != supported.end())
            f.setFeatureSetting({tnum, juce::FontFeatureSetting::featureEnabled});
    }

    return f;
}

float EmberFonts::scaleFor(float editorHeight) noexcept
{
    return juce::jlimit(0.72f, 2.0f, editorHeight / kReferenceHeight);
}

float EmberFonts::sizeFor(Role role, float uiScale) noexcept
{
    float base = 13.0f;

    switch (role)
    {
    case Role::display:
        base = 25.0f;
        break;
    case Role::title:
        base = 16.5f;
        break;
    case Role::section:
        base = 11.0f;
        break;
    case Role::body:
        base = 13.0f;
        break;
    case Role::label:
        base = 11.5f;
        break;
    case Role::value:
        base = 12.5f;
        break;
    case Role::micro:
        base = 10.0f;
        break;
    case Role::logo:
        base = 22.0f;
        break;
    case Role::footer:
        base = 11.0f;
        break;
    }

    return base * juce::jmax(0.5f, uiScale);
}

juce::Font EmberFonts::get(Role role, float uiScale)
{
    bool bold = false;

    switch (role)
    {
    case Role::display:
    case Role::title:
    case Role::section:
    case Role::logo:
        bold = true;
        break;

    case Role::body:
    case Role::label:
    case Role::value:
    case Role::micro:
    case Role::footer:
        bold = false;
        break;
    }

    switch (role)
    {
    case Role::display:
    case Role::title:
    case Role::section:
    case Role::logo:
        // The condensed face carries the hardware-panel feel; body text stays
        // on the grotesk so it does not turn into a stylistic exercise.
        return condensed(sizeFor(role, uiScale), bold);

    case Role::body:
    case Role::label:
    case Role::value:
    case Role::micro:
    case Role::footer:
        break;
    }

    return sized(sizeFor(role, uiScale), bold);
}

juce::Font EmberFonts::forHeight(float componentHeight, float proportion, bool bold)
{
    return sized(juce::jlimit(9.0f, 26.0f, componentHeight * proportion), bold);
}

juce::Font EmberFonts::sized(float pointHeight, bool bold)
{
    // The design floor is 10 px: below that the condensed face loses its
    // counters and the whole UI starts to read as smudged.
    auto options = juce::FontOptions().withHeight(juce::jmax(10.0f, pointHeight));

    if (auto face = embeddedFace(bold ? Family::interSemiBold : Family::interMedium))
        options = options.withTypeface(face);
    else if (bold)
        options = options.withStyle("Bold");

    return withTabularFigures(juce::Font(options));
}

juce::Font EmberFonts::condensed(float pointHeight, bool bold)
{
    auto options = juce::FontOptions().withHeight(juce::jmax(10.0f, pointHeight));

    if (auto face = embeddedFace(bold ? Family::barlowSemiBold : Family::barlowMedium))
        options = options.withTypeface(face);
    else if (bold)
        options = options.withStyle("Bold");

    return juce::Font(options);
}

bool EmberFonts::usingEmbeddedFaces()
{
    return embeddedFace(Family::interMedium) != nullptr && embeddedFace(Family::interSemiBold) != nullptr
           && embeddedFace(Family::barlowMedium) != nullptr && embeddedFace(Family::barlowSemiBold) != nullptr;
}

//==============================================================================
juce::Rectangle<float> RotaryGeometry::bodyBounds() const noexcept
{
    return juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);
}

//==============================================================================
const float EmberLookAndFeel::kRotaryStartAngle = juce::MathConstants<float>::pi * 1.25f;
const float EmberLookAndFeel::kRotaryEndAngle = juce::MathConstants<float>::pi * 2.75f;

EmberLookAndFeel::EmberLookAndFeel()
{
    // Every ColourId the plugin can reach, so nothing falls back to stock JUCE.
    setColour(juce::ResizableWindow::backgroundColourId, EmberColours::backgroundDeep);
    setColour(juce::DocumentWindow::textColourId, EmberColours::textPrimary);

    setColour(juce::Slider::backgroundColourId, EmberColours::track);
    setColour(juce::Slider::trackColourId, EmberColours::accent);
    setColour(juce::Slider::thumbColourId, EmberColours::textPrimary);
    setColour(juce::Slider::rotarySliderFillColourId, EmberColours::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, EmberColours::track);
    setColour(juce::Slider::textBoxTextColourId, EmberColours::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, EmberColours::panelSunken);
    setColour(juce::Slider::textBoxHighlightColourId, EmberColours::accent.withAlpha(0.35f));
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setColour(juce::Label::textColourId, EmberColours::textSecondary);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textWhenEditingColourId, EmberColours::textPrimary);
    setColour(juce::Label::backgroundWhenEditingColourId, EmberColours::panelSunken);
    setColour(juce::Label::outlineWhenEditingColourId, EmberColours::accent);

    setColour(juce::TextButton::buttonColourId, EmberColours::panelRaised);
    setColour(juce::TextButton::buttonOnColourId, EmberColours::accentDim);
    setColour(juce::TextButton::textColourOffId, EmberColours::textSecondary);
    setColour(juce::TextButton::textColourOnId, EmberColours::textPrimary);

    setColour(juce::ToggleButton::textColourId, EmberColours::textSecondary);
    setColour(juce::ToggleButton::tickColourId, EmberColours::accent);
    setColour(juce::ToggleButton::tickDisabledColourId, EmberColours::textDisabled);

    setColour(juce::ComboBox::backgroundColourId, EmberColours::panelRaised);
    setColour(juce::ComboBox::textColourId, EmberColours::textPrimary);
    setColour(juce::ComboBox::outlineColourId, EmberColours::outline);
    setColour(juce::ComboBox::buttonColourId, EmberColours::textSecondary);
    setColour(juce::ComboBox::arrowColourId, EmberColours::textSecondary);
    setColour(juce::ComboBox::focusedOutlineColourId, EmberColours::accent);

    setColour(juce::PopupMenu::backgroundColourId, EmberColours::panelRaised);
    setColour(juce::PopupMenu::textColourId, EmberColours::textPrimary);
    setColour(juce::PopupMenu::headerTextColourId, EmberColours::textSecondary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, EmberColours::accent.withAlpha(0.20f));
    setColour(juce::PopupMenu::highlightedTextColourId, EmberColours::accent);

    setColour(juce::TextEditor::backgroundColourId, EmberColours::panelSunken);
    setColour(juce::TextEditor::textColourId, EmberColours::textPrimary);
    setColour(juce::TextEditor::outlineColourId, EmberColours::outline);
    setColour(juce::TextEditor::focusedOutlineColourId, EmberColours::accent);
    setColour(juce::TextEditor::highlightColourId, EmberColours::accent.withAlpha(0.32f));
    setColour(juce::TextEditor::highlightedTextColourId, EmberColours::textPrimary);
    setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, EmberColours::accent);

    setColour(juce::TooltipWindow::backgroundColourId, EmberColours::panelRaised);
    setColour(juce::TooltipWindow::textColourId, EmberColours::textPrimary);
    setColour(juce::TooltipWindow::outlineColourId, EmberColours::outlineStrong);

    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, EmberColours::outlineStrong);
    setColour(juce::ScrollBar::trackColourId, EmberColours::panelSunken);

    setColour(juce::AlertWindow::backgroundColourId, EmberColours::panel);
    setColour(juce::AlertWindow::textColourId, EmberColours::textPrimary);
    setColour(juce::AlertWindow::outlineColourId, EmberColours::outlineStrong);

    setColour(juce::BubbleComponent::backgroundColourId, EmberColours::panelRaised);
    setColour(juce::BubbleComponent::outlineColourId, EmberColours::outlineStrong);

    setColour(juce::HyperlinkButton::textColourId, EmberColours::accent);
    setColour(juce::GroupComponent::outlineColourId, EmberColours::outline);
    setColour(juce::GroupComponent::textColourId, EmberColours::textSecondary);
}

EmberLookAndFeel::~EmberLookAndFeel() = default;

void EmberLookAndFeel::setUiScaleForEditorHeight(float editorHeight) noexcept
{
    uiScale = EmberFonts::scaleFor(editorHeight);
}

//==============================================================================
void EmberLookAndFeel::drawPanel(juce::Graphics& g, juce::Rectangle<float> area, float cornerSize, bool raised)
{
    if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
        return;

    const auto& tk = EmberTheme::tokens();
    const auto base = raised ? tk.panelRaised : tk.panel;

    // A raised panel casts a shadow. Two soft passes beneath it read as a
    // machined plate sitting on the chassis rather than as a coloured
    // rectangle, and cost two fills.
    if (raised)
    {
        g.setColour(tk.panelShadow.withAlpha(0.5f));
        g.fillRoundedRectangle(area.translated(0.0f, static_cast<float>(Metrics::shadowOffsetY)), cornerSize);
        g.setColour(tk.panelShadow.withAlpha(0.25f));
        g.fillRoundedRectangle(area.translated(0.0f, static_cast<float>(Metrics::shadowOffsetY) * 2.0f).expanded(1.0f),
                               cornerSize + 1.0f);
    }

    g.setGradientFill(surfaceGradient(area, base, raised ? 0.09f : 0.05f));
    g.fillRoundedRectangle(area, cornerSize);

    // Brushed metal, clipped to the panel. At 5 % it is invisible as texture
    // and entirely responsible for the surface not looking like flat fill.
    {
        juce::Graphics::ScopedSaveState save{g};
        juce::Path clip;
        clip.addRoundedRectangle(area, cornerSize);
        g.reduceClipRegion(clip);
        TextureCache::fillBrushed(g, area.getSmallestIntegerContainer(), raised ? 0.055f : 0.04f);
    }

    // The light comes from above-left, so the top edge catches it and the
    // bottom edge falls into shadow. Drawn as two arcs of the same rounded
    // rectangle rather than a single outline.
    juce::Path topEdge;
    topEdge.startNewSubPath(area.getX() + cornerSize, area.getY() + 0.5f);
    topEdge.lineTo(area.getRight() - cornerSize, area.getY() + 0.5f);
    g.setColour(tk.panelEdge.brighter(raised ? 0.22f : 0.08f));
    g.strokePath(topEdge, juce::PathStrokeType(Metrics::bevel));

    juce::Path bottomEdge;
    bottomEdge.startNewSubPath(area.getX() + cornerSize, area.getBottom() - 0.5f);
    bottomEdge.lineTo(area.getRight() - cornerSize, area.getBottom() - 0.5f);
    g.setColour(tk.panelShadow.withAlpha(0.6f));
    g.strokePath(bottomEdge, juce::PathStrokeType(Metrics::bevel));

    g.setColour(tk.panelEdge);
    g.drawRoundedRectangle(area.reduced(0.5f), cornerSize, 1.0f);
}

void EmberLookAndFeel::drawWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerSize)
{
    if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
        return;

    const auto& tk = EmberTheme::tokens();

    // A well is the inverse of a panel: darkest at the top, where the lip
    // above it would cast a shadow into the recess.
    g.setGradientFill(juce::ColourGradient(tk.bgDeep.darker(0.25f), area.getCentreX(), area.getY(), tk.bgDeep,
                                           area.getCentreX(), area.getBottom(), false));
    g.fillRoundedRectangle(area, cornerSize);

    juce::Path innerTop;
    innerTop.startNewSubPath(area.getX() + cornerSize, area.getY() + 0.5f);
    innerTop.lineTo(area.getRight() - cornerSize, area.getY() + 0.5f);
    g.setColour(tk.panelShadow);
    g.strokePath(innerTop, juce::PathStrokeType(Metrics::bevel));

    g.setColour(tk.panelEdge.withAlpha(0.8f));
    g.drawRoundedRectangle(area.reduced(0.5f), cornerSize, 1.0f);
}

void EmberLookAndFeel::drawHairline(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool vertical)
{
    g.setColour(colour);

    if (vertical)
        g.fillRect(area.withWidth(1.0f).withX(area.getCentreX() - 0.5f));
    else
        g.fillRect(area.withHeight(1.0f).withY(area.getCentreY() - 0.5f));
}

RotaryGeometry EmberLookAndFeel::rotaryGeometry(juce::Rectangle<float> sliderArea) noexcept
{
    RotaryGeometry geo;

    const float size = juce::jmin(sliderArea.getWidth(), sliderArea.getHeight());
    geo.centre = sliderArea.getCentre();
    geo.outerRadius = size * 0.5f;

    if (geo.outerRadius <= 1.0f)
        return geo;

    // Everything below is a proportion of the knob's own size, clamped to a
    // range that still reads at 28 px and does not turn clumsy at 96 px.
    geo.modRingThickness = juce::jlimit(1.5f, 3.5f, size * 0.042f);
    geo.valueThickness = juce::jlimit(2.5f, 7.0f, size * 0.085f);
    geo.trackThickness = juce::jmax(1.5f, geo.valueThickness * 0.58f);

    const float ringGap = juce::jmax(1.5f, size * 0.035f);
    const float bodyGap = juce::jmax(2.0f, size * 0.05f);

    geo.modRingRadius = geo.outerRadius - geo.modRingThickness * 0.5f - 0.5f;
    geo.arcRadius = juce::jmax(2.0f, geo.outerRadius - geo.modRingThickness - ringGap - geo.valueThickness * 0.5f);
    geo.bodyRadius = juce::jmax(2.0f, geo.arcRadius - geo.valueThickness * 0.5f - bodyGap);
    geo.pointerThickness = juce::jlimit(1.25f, 3.0f, size * 0.035f);

    return geo;
}

float EmberLookAndFeel::rotaryAngle(float proportion, float startAngle, float endAngle) noexcept
{
    return startAngle + juce::jlimit(0.0f, 1.0f, proportion) * (endAngle - startAngle);
}

juce::Path EmberLookAndFeel::arcPath(juce::Point<float> centre, float radius, float fromAngle, float toAngle)
{
    juce::Path path;

    if (radius > 0.5f && std::abs(toAngle - fromAngle) > 1.0e-4f)
        path.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, fromAngle, toAngle, true);

    return path;
}

//==============================================================================
void EmberLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                        juce::Slider& slider)
{
    const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                      static_cast<float>(height));
    const auto geo = rotaryGeometry(area);

    if (geo.outerRadius <= 1.0f)
        return;

    const auto& tk = EmberTheme::tokens();

    const bool enabled = slider.isEnabled();
    const bool hovered = enabled && slider.isMouseOverOrDragging();
    const bool active = hovered || (enabled && slider.hasKeyboardFocus(false));
    const auto accent = enabled ? EmberStyleProps::accentColourFor(slider) : tk.textMuted;

    const auto body = geo.bodyBounds();

    //--------------------------------------------------------------- the groove
    // The track is recessed, not drawn on top: a dark stroke with a lighter
    // hairline along its upper edge reads as a channel cut into the panel,
    // which is what makes the value arc look like it sits *in* something.
    const juce::PathStrokeType trackStroke(geo.trackThickness, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
    const auto trackPath = arcPath(geo.centre, geo.arcRadius, rotaryStartAngle, rotaryEndAngle);

    g.setColour(tk.bgDeep);
    g.strokePath(trackPath, trackStroke);

    // The groove has to stay legible or the control loses its range: on a dark
    // panel a pure shadow-coloured track simply disappears. A rim along the
    // whole arc keeps the sweep readable while still reading as recessed.
    g.setColour(tk.panelEdge.withAlpha(0.85f));
    g.strokePath(trackPath, juce::PathStrokeType(juce::jmax(1.0f, geo.trackThickness * 0.30f),
                                                 juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(tk.panelShadow.withAlpha(0.7f));
    g.strokePath(arcPath(geo.centre, geo.arcRadius - geo.trackThickness * 0.40f, rotaryStartAngle, rotaryEndAngle),
                 juce::PathStrokeType(0.8f, juce::PathStrokeType::curved));

    //----------------------------------------------------------- the value arc
    const float anchorProportion = valueArcAnchor(slider);
    const float anchorAngle = rotaryAngle(anchorProportion, rotaryStartAngle, rotaryEndAngle);

    if (isBipolar(slider))
    {
        // A bipolar control fills from its centre, so mark where zero is.
        const auto notch = geo.centre.getPointOnCircumference(geo.arcRadius, anchorAngle);
        const float notchRadius = juce::jmax(1.0f, geo.trackThickness * 0.55f);
        g.setColour(tk.textMuted);
        g.fillEllipse(juce::Rectangle<float>(notchRadius * 2.0f, notchRadius * 2.0f).withCentre(notch));
    }

    const float valueAngle = rotaryAngle(sliderPosProportional, rotaryStartAngle, rotaryEndAngle);

    if (enabled && std::abs(valueAngle - anchorAngle) > 1.0e-3f)
    {
        const auto valuePath = arcPath(geo.centre, geo.arcRadius, anchorAngle, valueAngle);

        // The bloom under the fill. A wider translucent stroke of the same path
        // is a real glow and costs one extra stroke - blurring per frame to get
        // the same effect is what makes glowing interfaces expensive.
        if (! EmberTheme::reduceMotion())
        {
            const float bloom = active ? 0.34f : 0.20f;
            g.setColour(tk.tubeGlow.withAlpha(bloom));
            g.strokePath(valuePath, juce::PathStrokeType(geo.valueThickness + Metrics::arcGlow * 2.0f,
                                                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Cooler where the arc starts, hotter where it ends, so a knob that is
        // wound up looks hotter than one barely moved even at a glance.
        const auto from = geo.centre.getPointOnCircumference(geo.arcRadius, anchorAngle);
        const auto to = geo.centre.getPointOnCircumference(geo.arcRadius, valueAngle);
        juce::ColourGradient arcFill(accent, from, accent.interpolatedWith(tk.emberHot, 0.55f), to, false);

        g.setGradientFill(arcFill);
        g.strokePath(valuePath, juce::PathStrokeType(geo.valueThickness, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    //-------------------------------------------------------------- the cap
    // Seated in a shadow, lit from the top-left, with the brushed sheet clipped
    // to it. The 6 % hover lift is the whole hover affordance - no outline, no
    // colour change, just the metal catching more light.
    g.setColour(tk.panelShadow.withAlpha(0.55f));
    g.fillEllipse(body.translated(0.0f, 1.2f).expanded(0.8f));

    const auto capBase = hovered ? tk.panelRaised.brighter(0.06f) : tk.panelRaised;
    juce::ColourGradient cap(capBase.brighter(0.16f), body.getX() + body.getWidth() * 0.28f,
                             body.getY() + body.getHeight() * 0.18f, capBase.darker(0.34f), body.getRight(),
                             body.getBottom(), true);
    g.setGradientFill(cap);
    g.fillEllipse(body);

    {
        juce::Graphics::ScopedSaveState save{g};
        juce::Path clip;
        clip.addEllipse(body);
        g.reduceClipRegion(clip);
        TextureCache::fillBrushed(g, body.getSmallestIntegerContainer().expanded(1), 0.05f);
    }

    // Rim: bright along the top-left where the light falls, dark below.
    g.setColour(tk.panelEdge.brighter(hovered ? 0.30f : 0.16f));
    g.drawEllipse(body.reduced(0.5f), 1.0f);
    g.setColour(tk.panelShadow.withAlpha(0.45f));
    g.strokePath(arcPath(geo.centre, geo.bodyRadius - 0.5f, juce::MathConstants<float>::halfPi * 0.6f,
                         juce::MathConstants<float>::pi * 1.1f),
                 juce::PathStrokeType(1.0f));

    //---------------------------------------------------------- the indicator
    // One bright line, the only pure-white-ish mark on the control.
    const auto inner = geo.centre.getPointOnCircumference(geo.bodyRadius * 0.30f, valueAngle);
    const auto outer = geo.centre.getPointOnCircumference(geo.bodyRadius * 0.88f, valueAngle);

    if (enabled && ! EmberTheme::reduceMotion())
    {
        g.setColour(tk.tubeGlow.withAlpha(active ? 0.30f : 0.16f));
        g.drawLine(juce::Line<float>(inner, outer), geo.pointerThickness + 2.0f);
    }

    g.setColour(enabled ? tk.text : tk.textMuted);
    g.drawLine(juce::Line<float>(inner, outer), geo.pointerThickness);
}

//==============================================================================
void EmberLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                        juce::Slider& slider)
{
    juce::ignoreUnused(style);

    const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                      static_cast<float>(height));

    if (area.getWidth() < 2.0f || area.getHeight() < 2.0f)
        return;

    const bool enabled = slider.isEnabled();
    const auto accent = enabled ? EmberStyleProps::accentColourFor(slider) : EmberColours::textDisabled;
    const bool horizontal = !slider.isVertical();

    if (slider.isBar())
    {
        const float corner = cornerFor(area.getHeight(), uiScale);
        drawWell(g, area, corner);

        auto filled = area.reduced(1.0f);

        if (horizontal)
            filled = filled.withWidth(juce::jmax(0.0f, sliderPos - filled.getX()));
        else
            filled = filled.withTop(juce::jmax(filled.getY(), sliderPos));

        if (filled.getWidth() > 0.5f && filled.getHeight() > 0.5f)
        {
            g.setColour(accent.withAlpha(0.75f));
            g.fillRoundedRectangle(filled, juce::jmax(1.0f, corner - 1.0f));
        }

        return;
    }

    const float thickness = juce::jlimit(3.0f, 7.0f, (horizontal ? area.getHeight() : area.getWidth()) * 0.22f);
    const auto trackArea =
        horizontal
            ? juce::Rectangle<float>(area.getX(), area.getCentreY() - thickness * 0.5f, area.getWidth(), thickness)
            : juce::Rectangle<float>(area.getCentreX() - thickness * 0.5f, area.getY(), thickness, area.getHeight());

    g.setColour(EmberColours::track);
    g.fillRoundedRectangle(trackArea, thickness * 0.5f);

    // Filled span: between the two thumbs for a range slider, from the anchor
    // to the value for an ordinary one.
    float fromPos = 0.0f;
    float toPos = 0.0f;

    if (slider.isTwoValue() || slider.isThreeValue())
    {
        fromPos = minSliderPos;
        toPos = maxSliderPos;
    }
    else
    {
        const float anchorProportion = valueArcAnchor(slider);
        fromPos = horizontal ? area.getX() + anchorProportion * area.getWidth()
                             : area.getBottom() - anchorProportion * area.getHeight();
        toPos = sliderPos;
    }

    auto filled = horizontal ? trackArea.withLeft(juce::jmin(fromPos, toPos)).withRight(juce::jmax(fromPos, toPos))
                             : trackArea.withTop(juce::jmin(fromPos, toPos)).withBottom(juce::jmax(fromPos, toPos));

    if (filled.getWidth() > 0.5f && filled.getHeight() > 0.5f)
    {
        g.setColour(accent);
        g.fillRoundedRectangle(filled, thickness * 0.5f);
    }

    // Thumbs.
    const float thumbLong = juce::jmax(10.0f, thickness * 2.6f);
    const float thumbShort = juce::jmax(5.0f, thickness * 1.25f);

    auto drawThumb = [&](float position)
    {
        const auto thumb =
            horizontal ? juce::Rectangle<float>(thumbShort, thumbLong).withCentre({position, area.getCentreY()})
                       : juce::Rectangle<float>(thumbLong, thumbShort).withCentre({area.getCentreX(), position});

        g.setColour(enabled ? EmberColours::textPrimary : EmberColours::textDisabled);
        g.fillRoundedRectangle(thumb, thumbShort * 0.42f);
        g.setColour(EmberColours::backgroundDeep.withAlpha(0.6f));
        g.drawRoundedRectangle(thumb.reduced(0.5f), thumbShort * 0.42f, 1.0f);
    };

    if (slider.isTwoValue() || slider.isThreeValue())
    {
        drawThumb(minSliderPos);
        drawThumb(maxSliderPos);

        if (slider.isThreeValue())
            drawThumb(sliderPos);
    }
    else
    {
        drawThumb(sliderPos);
    }
}

int EmberLookAndFeel::getSliderThumbRadius(juce::Slider& slider)
{
    const int shortest = juce::jmin(slider.getWidth(), slider.getHeight());
    return juce::jlimit(4, 11, juce::roundToInt(static_cast<float>(shortest) * 0.25f));
}

juce::Label* EmberLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = new juce::Label();

    label->setJustificationType(juce::Justification::centred);
    label->setKeyboardType(juce::TextInputTarget::decimalKeyboard);
    label->setFont(EmberFonts::get(EmberFonts::Role::value, uiScale));
    label->setColour(juce::Label::textColourId, slider.findColour(juce::Slider::textBoxTextColourId));
    label->setColour(juce::Label::backgroundColourId, slider.findColour(juce::Slider::textBoxBackgroundColourId));
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::textWhenEditingColourId, EmberColours::textPrimary);
    label->setColour(juce::Label::backgroundWhenEditingColourId, EmberColours::panelSunken);
    label->setColour(juce::Label::outlineWhenEditingColourId, EmberStyleProps::accentColourFor(slider));
    label->setColour(juce::TextEditor::textColourId, EmberColours::textPrimary);
    label->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour(juce::TextEditor::highlightColourId, EmberColours::accent.withAlpha(0.32f));
    label->setColour(juce::TextEditor::highlightedTextColourId, EmberColours::textPrimary);
    label->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);

    return label;
}

juce::Font EmberLookAndFeel::getSliderPopupFont(juce::Slider&)
{
    return EmberFonts::get(EmberFonts::Role::value, uiScale);
}

//==============================================================================
void EmberLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted,
                                            bool shouldDrawButtonAsDown)
{
    const auto area = button.getLocalBounds().toFloat().reduced(0.5f);

    if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
        return;

    const float corner = cornerFor(area.getHeight(), uiScale);
    const auto accent = EmberStyleProps::accentColourFor(button);
    const bool on = button.getToggleState();

    auto fill = backgroundColour;

    if (!button.isEnabled())
        fill = fill.withMultipliedSaturation(0.25f).withMultipliedBrightness(0.7f);
    else if (shouldDrawButtonAsDown)
        fill = fill.brighter(0.22f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter(0.10f);

    g.setGradientFill(surfaceGradient(area, fill, 0.06f));
    g.fillRoundedRectangle(area, corner);

    // A fully transparent fill is a "ghost" button: the edge is all it has, so
    // it gets the stronger one.
    const auto restingOutline = backgroundColour.isTransparent() || shouldDrawButtonAsHighlighted
                                    ? EmberColours::outlineStrong
                                    : EmberColours::outline;

    g.setColour(on && button.isEnabled() ? accent.withAlpha(0.85f) : restingOutline);
    g.drawRoundedRectangle(area, corner, 1.0f);
}

void EmberLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                                      bool shouldDrawButtonAsDown)
{
    const auto font = getTextButtonFont(button, button.getHeight());
    g.setFont(font);

    auto colour = button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                            : juce::TextButton::textColourOffId);

    if (!button.isEnabled())
        colour = EmberColours::textDisabled;
    else if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
        colour = colour.brighter(0.25f);

    g.setColour(colour);

    const int inset = juce::jmax(2, juce::roundToInt(static_cast<float>(button.getHeight()) * 0.2f));
    const auto area = button.getLocalBounds().reduced(inset, 1);

    g.drawFittedText(button.getButtonText(), area, juce::Justification::centred, 1, 0.75f);
}

juce::Font EmberLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return EmberFonts::forHeight(static_cast<float>(buttonHeight), 0.45f, false);
}

//==============================================================================
void EmberLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                        bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto area = button.getLocalBounds().toFloat();

    if (area.getWidth() < 2.0f || area.getHeight() < 2.0f)
        return;

    const bool enabled = button.isEnabled();
    const bool on = button.getToggleState();
    const auto& tk = EmberTheme::tokens();
    const auto accent = enabled ? EmberStyleProps::accentColourFor(button) : tk.textMuted;
    const auto look = EmberStyleProps::toggleLookFor(button);

    float textLeft = area.getX();

    switch (look)
    {
    case ToggleLook::pill:
    {
        // A backlit panel switch, not a phone toggle: a recessed rectangular
        // window with a filament inside it that lights when the switch is on.
        const float windowHeight = juce::jlimit(12.0f, 22.0f, area.getHeight() * 0.60f);
        const float windowWidth = windowHeight * 1.9f;
        const auto window = juce::Rectangle<float>(windowWidth, windowHeight)
                                .withCentre({area.getX() + windowWidth * 0.5f, area.getCentreY()});
        const float radius = Metrics::controlRadius;

        // The well, and the shadow it casts inside its own top edge.
        g.setColour(tk.bgDeep);
        g.fillRoundedRectangle(window, radius);
        g.setColour(tk.panelShadow.withAlpha(0.8f));
        g.drawRoundedRectangle(window.reduced(0.5f).withTrimmedBottom(window.getHeight() * 0.5f), radius, 1.0f);

        // The filament: a short horizontal element across the middle.
        const auto filament = juce::Rectangle<float>(window.getWidth() * 0.54f, juce::jmax(2.0f, windowHeight * 0.16f))
                                  .withCentre(window.getCentre());

        if (on && enabled)
        {
            if (! EmberTheme::reduceMotion())
            {
                g.setColour(tk.tubeGlow.withAlpha(0.30f));
                g.fillRoundedRectangle(filament.expanded(windowHeight * 0.30f), radius);
                g.setColour(tk.tubeGlow.withAlpha(0.22f));
                g.fillRoundedRectangle(window.reduced(1.0f), radius);
            }

            g.setColour(accent.interpolatedWith(tk.emberHot, 0.5f));
            g.fillRoundedRectangle(filament, filament.getHeight() * 0.5f);
        }
        else
        {
            // Cold filament: visible, so the switch reads as "off" rather than
            // as "empty".
            g.setColour(enabled ? tk.textMuted : tk.textMuted.withAlpha(0.5f));
            g.fillRoundedRectangle(filament, filament.getHeight() * 0.5f);
        }

        g.setColour(shouldDrawButtonAsHighlighted ? tk.panelEdge.brighter(0.25f) : tk.panelEdge);
        g.drawRoundedRectangle(window.reduced(0.5f), radius, 1.0f);

        textLeft = window.getRight() + juce::jmax(6.0f, windowHeight * 0.4f);
        break;
    }

    case ToggleLook::led:
    {
        // The round sibling of the same idea: a lamp inset into the panel.
        const float diameter = juce::jlimit(8.0f, 15.0f, area.getHeight() * 0.46f);
        const auto lamp = juce::Rectangle<float>(diameter, diameter)
                              .withCentre({area.getX() + diameter * 0.5f + 1.0f, area.getCentreY()});

        g.setColour(tk.bgDeep);
        g.fillEllipse(lamp);

        if (on && enabled)
        {
            if (! EmberTheme::reduceMotion())
                GlowCache::draw(g, lamp.getCentre(), diameter * 1.5f, tk.tubeGlow, 0.45f);

            juce::ColourGradient lit(accent.interpolatedWith(tk.emberHot, 0.45f), lamp.getCentreX(),
                                     lamp.getCentreY() - diameter * 0.15f, accent.darker(0.4f), lamp.getCentreX(),
                                     lamp.getBottom(), true);
            g.setGradientFill(lit);
            g.fillEllipse(lamp.reduced(1.0f));
        }
        else
        {
            g.setColour(enabled ? tk.textMuted.withAlpha(0.55f) : tk.textMuted.withAlpha(0.3f));
            g.fillEllipse(lamp.reduced(diameter * 0.30f));
        }

        g.setColour(shouldDrawButtonAsHighlighted ? tk.panelEdge.brighter(0.25f) : tk.panelEdge);
        g.drawEllipse(lamp.reduced(0.5f), 1.0f);

        textLeft = lamp.getRight() + juce::jmax(6.0f, diameter * 0.5f);
        break;
    }

    case ToggleLook::check:
    {
        const float boxSize = juce::jlimit(11.0f, 20.0f, area.getHeight() * 0.62f);
        drawTickBox(g, button, area.getX() + 1.0f, area.getCentreY() - boxSize * 0.5f, boxSize, boxSize, on, enabled,
                    shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        textLeft = area.getX() + boxSize + juce::jmax(5.0f, boxSize * 0.35f);
        break;
    }
    }

    const auto textArea = juce::Rectangle<float>(textLeft, area.getY(), area.getRight() - textLeft, area.getHeight());

    if (button.getButtonText().isNotEmpty() && textArea.getWidth() > 4.0f)
    {
        auto colour = button.findColour(juce::ToggleButton::textColourId);

        if (!enabled)
            colour = EmberColours::textDisabled;
        else if (on)
            colour = EmberColours::textPrimary;

        g.setColour(colour);
        g.setFont(EmberFonts::forHeight(area.getHeight(), 0.46f, false));
        g.drawFittedText(button.getButtonText(), textArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }
}

void EmberLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component& component, float x, float y, float w, float h,
                                   bool ticked, bool isEnabled, bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown)
{
    const juce::Rectangle<float> box(x, y, w, h);
    const auto accent = isEnabled ? EmberStyleProps::accentColourFor(component) : EmberColours::textDisabled;
    const float corner = juce::jmax(2.0f, w * 0.22f);

    g.setColour(shouldDrawButtonAsDown ? EmberColours::panelRaised : EmberColours::panelSunken);
    g.fillRoundedRectangle(box, corner);
    g.setColour(shouldDrawButtonAsHighlighted ? EmberColours::outlineStrong : EmberColours::outline);
    g.drawRoundedRectangle(box.reduced(0.5f), corner, 1.0f);

    if (ticked)
    {
        auto tick = getTickShape(0.72f);
        g.setColour(accent);
        g.fillPath(tick, tick.getTransformToScaleToFit(box.reduced(w * 0.2f), true));
    }
}

//==============================================================================
void EmberLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown, int buttonX,
                                    int buttonY, int buttonW, int buttonH, juce::ComboBox& box)
{
    const juce::Rectangle<float> area(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const float corner = cornerFor(area.getHeight(), uiScale);
    const bool enabled = box.isEnabled();
    const auto accent = EmberStyleProps::accentColourFor(box);

    g.setGradientFill(surfaceGradient(area.reduced(0.5f), box.findColour(juce::ComboBox::backgroundColourId), 0.07f));
    g.fillRoundedRectangle(area.reduced(0.5f), corner);

    const bool highlighted = isButtonDown || box.isPopupActive() || box.hasKeyboardFocus(false);
    g.setColour(!enabled ? EmberColours::outline : (highlighted ? accent.withAlpha(0.8f) : EmberColours::outline));
    g.drawRoundedRectangle(area.reduced(0.5f), corner, 1.0f);

    // Chevron, sized from the button area JUCE hands us.
    const juce::Rectangle<float> buttonArea(static_cast<float>(buttonX), static_cast<float>(buttonY),
                                            static_cast<float>(buttonW), static_cast<float>(buttonH));
    const float chevronWidth = juce::jlimit(5.0f, 9.0f, buttonArea.getWidth() * 0.4f);
    const auto centre = buttonArea.getCentre();

    juce::Path chevron;
    chevron.startNewSubPath(centre.x - chevronWidth * 0.5f, centre.y - chevronWidth * 0.26f);
    chevron.lineTo(centre.x, centre.y + chevronWidth * 0.34f);
    chevron.lineTo(centre.x + chevronWidth * 0.5f, centre.y - chevronWidth * 0.26f);

    g.setColour(!enabled ? EmberColours::textDisabled
                         : (highlighted ? accent : box.findColour(juce::ComboBox::arrowColourId)));
    g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font EmberLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    return EmberFonts::forHeight(static_cast<float>(box.getHeight()), 0.46f, false);
}

void EmberLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    const int arrowWidth = juce::jlimit(14, 30, juce::roundToInt(static_cast<float>(box.getHeight()) * 0.72f));
    const int inset = juce::jmax(5, juce::roundToInt(static_cast<float>(box.getHeight()) * 0.28f));

    label.setBounds(inset, 1, juce::jmax(0, box.getWidth() - arrowWidth - inset), box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId,
                    box.isEnabled() ? box.findColour(juce::ComboBox::textColourId) : EmberColours::textDisabled);
}

//==============================================================================
void EmberLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                                          const juce::PopupMenu::Options&)
{
    g.fillAll(EmberColours::panelRaised);

    g.setColour(EmberColours::outlineStrong);
    g.drawRect(0, 0, width, height, 1);
}

void EmberLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                         bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                         const juce::String& text, const juce::String& shortcutKeyText,
                                         const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        const auto line = area.reduced(area.getHeight() / 2, 0).toFloat();
        drawHairline(g, line, EmberColours::outline);
        return;
    }

    const auto bounds = area.toFloat().reduced(2.0f, 1.0f);

    if (isHighlighted && isActive)
    {
        g.setColour(EmberColours::accent.withAlpha(0.16f));
        g.fillRoundedRectangle(bounds, 3.0f);
    }

    auto colour = textColour != nullptr ? *textColour : EmberColours::textPrimary;

    if (!isActive)
        colour = EmberColours::textDisabled;
    else if (isHighlighted)
        colour = EmberColours::accent;

    g.setColour(colour);
    g.setFont(getPopupMenuFont());

    const float tickZone = static_cast<float>(area.getHeight()) * 0.9f;
    auto textArea = bounds.reduced(tickZone * 0.35f, 0.0f).withTrimmedLeft(tickZone * 0.65f);

    if (isTicked)
    {
        const auto tickArea = juce::Rectangle<float>(bounds.getX() + 3.0f, bounds.getY(), tickZone, bounds.getHeight())
                                  .reduced(tickZone * 0.28f);
        auto tick = getTickShape(1.0f);
        g.fillPath(tick, tick.getTransformToScaleToFit(tickArea, true));
    }

    if (icon != nullptr)
    {
        const auto iconArea = juce::Rectangle<float>(bounds.getX() + 3.0f, bounds.getY(), tickZone, bounds.getHeight())
                                  .reduced(tickZone * 0.2f);
        icon->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
    }

    if (hasSubMenu)
    {
        const float arrowSize = static_cast<float>(area.getHeight()) * 0.28f;
        const auto arrowCentre = juce::Point<float>(textArea.getRight() - arrowSize, textArea.getCentreY());

        juce::Path arrow;
        arrow.startNewSubPath(arrowCentre.x - arrowSize * 0.25f, arrowCentre.y - arrowSize * 0.5f);
        arrow.lineTo(arrowCentre.x + arrowSize * 0.35f, arrowCentre.y);
        arrow.lineTo(arrowCentre.x - arrowSize * 0.25f, arrowCentre.y + arrowSize * 0.5f);

        g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        textArea = textArea.withTrimmedRight(arrowSize * 2.0f);
    }
    else if (shortcutKeyText.isNotEmpty())
    {
        const auto shortcutFont = EmberFonts::get(EmberFonts::Role::micro, uiScale);
        const float shortcutWidth = juce::GlyphArrangement::getStringWidth(shortcutFont, shortcutKeyText) + 6.0f;

        g.setFont(shortcutFont);
        g.setColour(EmberColours::textSecondary);
        g.drawFittedText(shortcutKeyText, textArea.removeFromRight(shortcutWidth).toNearestInt(),
                         juce::Justification::centredRight, 1, 1.0f);
        g.setColour(colour);
        g.setFont(getPopupMenuFont());
    }

    g.drawFittedText(text, textArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}

void EmberLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                                 int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 60;
        idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight / 2 : 7;
        return;
    }

    const auto font = getPopupMenuFont();
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : juce::roundToInt(font.getHeight() * 1.9f);
    idealWidth = juce::GlyphArrangement::getStringWidthInt(font, text) + idealHeight * 2;
}

juce::Font EmberLookAndFeel::getPopupMenuFont()
{
    return EmberFonts::get(EmberFonts::Role::body, uiScale);
}

int EmberLookAndFeel::getPopupMenuBorderSize()
{
    return juce::jmax(3, juce::roundToInt(5.0f * uiScale));
}

//==============================================================================
void EmberLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));

    if (!label.isBeingEdited())
    {
        const float alpha = label.isEnabled() ? 1.0f : 0.55f;
        const auto font = getLabelFont(label);
        const auto area = label.getBorderSize().subtractedFrom(label.getLocalBounds());

        g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
        g.setFont(font);
        g.drawFittedText(label.getText(), area, label.getJustificationType(),
                         juce::jmax(1, juce::roundToInt(static_cast<float>(area.getHeight()) / font.getHeight())),
                         label.getMinimumHorizontalScale());

        g.setColour(label.findColour(juce::Label::outlineColourId).withMultipliedAlpha(alpha));
    }
    else
    {
        g.setColour(label.findColour(juce::Label::outlineColourId));
    }

    g.drawRect(label.getLocalBounds());
}

juce::Font EmberLookAndFeel::getLabelFont(juce::Label& label)
{
    // Labels carry their own font so panels can pick a type-scale role with
    // `label.setFont (EmberFonts::get (...))`; JUCE's default is only used when
    // nobody has chosen.
    return label.getFont();
}

//==============================================================================
void EmberLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const juce::Rectangle<float> area(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(area, cornerFor(area.getHeight(), uiScale));
}

void EmberLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (editor.isReadOnly() || !editor.isEnabled())
        return;

    const juce::Rectangle<float> area(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const bool focused = editor.hasKeyboardFocus(true);

    g.setColour(focused ? EmberStyleProps::accentColourFor(editor)
                        : editor.findColour(juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(area.reduced(0.5f), cornerFor(area.getHeight(), uiScale), focused ? 1.5f : 1.0f);
}

//==============================================================================
void EmberLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    const juce::Rectangle<float> area(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

    g.setColour(EmberColours::panelRaised);
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(EmberColours::outlineStrong);
    g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);

    const auto textArea = area.reduced(8.0f, 5.0f);
    const auto layout = layoutTooltipText(text, EmberFonts::get(EmberFonts::Role::body, uiScale),
                                          EmberColours::textPrimary, juce::jmax(10.0f, textArea.getWidth()));
    layout.draw(g, textArea);
}

juce::Rectangle<int> EmberLookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                        juce::Rectangle<int> parentArea)
{
    const int padX = 8;
    const int padY = 5;
    const float maxTextWidth = juce::jmax(120.0f, 300.0f * uiScale);
    const auto layout = layoutTooltipText(tipText, EmberFonts::get(EmberFonts::Role::body, uiScale),
                                          EmberColours::textPrimary, maxTextWidth);

    const int w = juce::roundToInt(std::ceil(layout.getWidth())) + padX * 2;
    const int h = juce::roundToInt(std::ceil(layout.getHeight())) + padY * 2;

    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 18,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 8, w, h)
        .constrainedWithin(parentArea);
}

//==============================================================================
void EmberLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                     bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool isMouseOver,
                                     bool isMouseDown)
{
    juce::ignoreUnused(scrollbar);

    const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                      static_cast<float>(height));

    g.setColour(EmberColours::panelSunken.withAlpha(0.5f));
    g.fillRect(area);

    if (thumbSize <= 0)
        return;

    const auto thumb =
        isScrollbarVertical
            ? juce::Rectangle<float>(area.getX() + area.getWidth() * 0.25f, static_cast<float>(thumbStartPosition),
                                     area.getWidth() * 0.5f, static_cast<float>(thumbSize))
            : juce::Rectangle<float>(static_cast<float>(thumbStartPosition), area.getY() + area.getHeight() * 0.25f,
                                     static_cast<float>(thumbSize), area.getHeight() * 0.5f);

    g.setColour(isMouseDown ? EmberColours::accent
                            : (isMouseOver ? EmberColours::textSecondary : EmberColours::outlineStrong));
    g.fillRoundedRectangle(thumb, juce::jmin(thumb.getWidth(), thumb.getHeight()) * 0.5f);
}
} // namespace ember::gui
