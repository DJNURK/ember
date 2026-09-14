#include "gui/EmberLookAndFeel.h"

#include <cmath>

namespace ember::gui
{
//==============================================================================
// Palette. Near-black desaturated surfaces, one warm accent, and a band ramp
// that runs along flame temperature: deep ember red at the bottom, blue-white
// flame tip at the top.
//==============================================================================
const juce::Colour EmberColours::backgroundDeep{0xff0a0a0c};
const juce::Colour EmberColours::background{0xff121215};
const juce::Colour EmberColours::panelSunken{0xff0e0e11};
const juce::Colour EmberColours::panel{0xff17171b};
const juce::Colour EmberColours::panelRaised{0xff1f1f24};

const juce::Colour EmberColours::outline{0xff2b2b32};
const juce::Colour EmberColours::outlineStrong{0xff3c3c46};
const juce::Colour EmberColours::track{0xff3a3a45};

const juce::Colour EmberColours::textPrimary{0xffe9e7e3};
const juce::Colour EmberColours::textSecondary{0xff9a9aa5};
const juce::Colour EmberColours::textDisabled{0xff585862};

const juce::Colour EmberColours::accent{0xffff8a3d};
const juce::Colour EmberColours::accentDim{0xffb05f2c};
const juce::Colour EmberColours::accentGlow{0x33ff8a3d};
const juce::Colour EmberColours::warning{0xffff4f45};

namespace
{
/** The band ramp. Six hues, each clearly separable at a glance, all sitting on
    the same warm axis apart from the deliberately cool top band. */
const juce::Colour kBandRamp[EmberColours::kNumBandColours] = {
    juce::Colour(0xffb8442f), // 1 — deep ember red
    juce::Colour(0xffdd6a33), // 2 — orange
    juce::Colour(0xfff59333), // 3 — amber, sibling of the accent
    juce::Colour(0xffe6bc4a), // 4 — gold
    juce::Colour(0xffe2dcc4), // 5 — pale ash white
    juce::Colour(0xff86b3d2)  // 6 — blue flame tip
};

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
    return kBandRamp[static_cast<size_t>(juce::jlimit(0, kNumBandColours - 1, bandIndex))];
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
        base = 9.5f;
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
        bold = true;
        break;

    case Role::body:
    case Role::label:
    case Role::value:
    case Role::micro:
        bold = false;
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
    auto options = juce::FontOptions().withHeight(juce::jmax(6.0f, pointHeight));

    if (bold)
        options = options.withStyle("Bold");

    return juce::Font(options);
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

    const auto base = raised ? EmberColours::panelRaised : EmberColours::panel;
    g.setGradientFill(surfaceGradient(area, base, raised ? 0.09f : 0.05f));
    g.fillRoundedRectangle(area, cornerSize);

    g.setColour(EmberColours::outline);
    g.drawRoundedRectangle(area.reduced(0.5f), cornerSize, 1.0f);
}

void EmberLookAndFeel::drawWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerSize)
{
    if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
        return;

    g.setGradientFill(juce::ColourGradient(EmberColours::backgroundDeep, area.getCentreX(), area.getY(),
                                           EmberColours::panelSunken, area.getCentreX(), area.getBottom(), false));
    g.fillRoundedRectangle(area, cornerSize);

    g.setColour(EmberColours::outline.withAlpha(0.8f));
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

    const bool enabled = slider.isEnabled();
    const bool active = enabled && (slider.isMouseOverOrDragging() || slider.hasKeyboardFocus(false));
    const auto accent = enabled ? EmberStyleProps::accentColourFor(slider) : EmberColours::textDisabled;

    // --- the cap the pointer sits on
    const auto body = geo.bodyBounds();
    g.setGradientFill(surfaceGradient(body, EmberColours::panelRaised, 0.12f));
    g.fillEllipse(body);
    g.setColour(active ? accent.withAlpha(0.55f) : EmberColours::outline);
    g.drawEllipse(body.reduced(0.5f), 1.0f);

    // --- unfilled track
    const juce::PathStrokeType trackStroke(geo.trackThickness, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
    g.setColour(EmberColours::track);
    g.strokePath(arcPath(geo.centre, geo.arcRadius, rotaryStartAngle, rotaryEndAngle), trackStroke);

    // --- a notch where a bipolar control's value arc grows from
    const float anchorProportion = valueArcAnchor(slider);
    const float anchorAngle = rotaryAngle(anchorProportion, rotaryStartAngle, rotaryEndAngle);

    if (isBipolar(slider))
    {
        const auto notch = geo.centre.getPointOnCircumference(geo.arcRadius, anchorAngle);
        const float notchRadius = juce::jmax(1.0f, geo.trackThickness * 0.7f);
        g.setColour(EmberColours::outlineStrong);
        g.fillEllipse(juce::Rectangle<float>(notchRadius * 2.0f, notchRadius * 2.0f).withCentre(notch));
    }

    // --- filled value arc
    const float valueAngle = rotaryAngle(sliderPosProportional, rotaryStartAngle, rotaryEndAngle);

    if (std::abs(valueAngle - anchorAngle) > 1.0e-3f)
    {
        g.setColour(accent);
        g.strokePath(
            arcPath(geo.centre, geo.arcRadius, anchorAngle, valueAngle),
            juce::PathStrokeType(geo.valueThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // --- pointer
    const auto inner = geo.centre.getPointOnCircumference(geo.bodyRadius * 0.34f, valueAngle);
    const auto outer = geo.centre.getPointOnCircumference(geo.bodyRadius * 0.94f, valueAngle);
    g.setColour(enabled ? EmberColours::textPrimary : EmberColours::textDisabled);
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
    const auto accent = enabled ? EmberStyleProps::accentColourFor(button) : EmberColours::textDisabled;
    const auto look = EmberStyleProps::toggleLookFor(button);

    float textLeft = area.getX();

    switch (look)
    {
    case ToggleLook::pill:
    {
        const float trackHeight = juce::jlimit(11.0f, 22.0f, area.getHeight() * 0.58f);
        const float trackWidth = trackHeight * 1.85f;
        const auto trackArea = juce::Rectangle<float>(trackWidth, trackHeight)
                                   .withCentre({area.getX() + trackWidth * 0.5f, area.getCentreY()});

        g.setColour(on ? accent.withAlpha(enabled ? 0.9f : 0.4f) : EmberColours::panelSunken);
        g.fillRoundedRectangle(trackArea, trackHeight * 0.5f);
        g.setColour(shouldDrawButtonAsHighlighted ? EmberColours::outlineStrong : EmberColours::outline);
        g.drawRoundedRectangle(trackArea.reduced(0.5f), trackHeight * 0.5f, 1.0f);

        const float knobRadius = trackHeight * 0.5f - 2.0f;
        const float knobX = on ? trackArea.getRight() - knobRadius - 2.0f : trackArea.getX() + knobRadius + 2.0f;
        const auto knob =
            juce::Rectangle<float>(knobRadius * 2.0f, knobRadius * 2.0f).withCentre({knobX, trackArea.getCentreY()});

        g.setColour(shouldDrawButtonAsDown ? EmberColours::textSecondary : EmberColours::textPrimary);
        g.fillEllipse(knob);

        textLeft = trackArea.getRight() + juce::jmax(5.0f, trackHeight * 0.35f);
        break;
    }

    case ToggleLook::led:
    {
        const float diameter = juce::jlimit(7.0f, 14.0f, area.getHeight() * 0.44f);
        const auto lamp = juce::Rectangle<float>(diameter, diameter)
                              .withCentre({area.getX() + diameter * 0.5f + 1.0f, area.getCentreY()});

        if (on && enabled)
        {
            g.setColour(accent.withAlpha(0.22f));
            g.fillEllipse(lamp.expanded(diameter * 0.45f));
        }

        g.setColour(on ? accent : EmberColours::panelSunken);
        g.fillEllipse(lamp);
        g.setColour(shouldDrawButtonAsHighlighted ? EmberColours::outlineStrong : EmberColours::outline);
        g.drawEllipse(lamp.reduced(0.5f), 1.0f);

        textLeft = lamp.getRight() + juce::jmax(5.0f, diameter * 0.5f);
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
