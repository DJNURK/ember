#pragma once
#include <juce_graphics/juce_graphics.h>

/**
    Ember's visual vocabulary: colour tokens, the spacing scale, the type scale
    and the two caches that make the glowing look affordable to draw.

    THE ONE RULE
    ------------
    No colour literal may appear anywhere else in `src/gui/`. Every colour comes
    from `EmberTheme::tokens()`. `tests/test_theme.cpp` greps the GUI sources for
    `Colour (0x`, `Colour::fromRGB` and friends and fails if it finds one outside
    this file. That is what makes the "Cool Ember" variant a one-line change
    instead of an archaeology exercise.

    THE COLOUR SEMANTICS
    --------------------
      warm  = audio        (ember, emberHot, emberDeep, tubeGlow, signal)
      cool  = modulation   (cold)
      red   = clipping     (danger)

    There are no other hues. If something needs to stand out and is neither
    audio, modulation nor clipping, it stands out by brightness or weight, not
    by colour.

    WHY HEAT, NOT BAND HUE
    ----------------------
    Bands do not each own a hue. A band's tint is its *heat* — how much harmonic
    energy it is adding — ramping emberDeep -> ember -> emberHot. Band identity
    comes from number and position instead. This replaces the older flame ramp,
    whose blue-white top band would now read as "modulation".

    @see docs/UI-DESIGN.md for the full token table and the layout it serves.
*/
namespace ember::gui
{
//==============================================================================
/** Every colour Ember draws with. Instances are immutable; get the live one
    from `EmberTheme::tokens()`.

    `juce::Colour` is not a literal type, so these are plain members rather than
    constexpr, built once into the two static variants in EmberTheme.cpp.
*/
struct ThemeTokens
{
    // ---- chassis and panels, darkest first ---------------------------------
    juce::Colour bgDeep;      ///< the window itself; near-black, faintly warm
    juce::Colour panel;       ///< a recessed surface
    juce::Colour panelRaised; ///< a module or control sitting proud of the panel
    juce::Colour panelEdge;   ///< 1 px bevel highlight along a top edge
    juce::Colour panelShadow; ///< the shadow edge beneath a raised surface

    // ---- text ---------------------------------------------------------------
    juce::Colour text;      ///< values and primary labels
    juce::Colour textDim;   ///< captions, units, axis labels
    juce::Colour textMuted; ///< grid lines and separators. NEVER text: it is
                            ///< 2.3:1 on panel and fails the contrast gate.

    // ---- warm: audio --------------------------------------------------------
    juce::Colour ember;     ///< the primary accent
    juce::Colour emberHot;  ///< maximum drive, glow peaks
    juce::Colour emberDeep; ///< a cooled, barely-driven band
    juce::Colour tubeGlow;  ///< bloom layers; used at 0-60 % alpha
    juce::Colour signal;    ///< meters and the spectrum trace at moderate level

    // ---- cool: modulation ---------------------------------------------------
    juce::Colour cold; ///< sources, connections, modulation arcs

    // ---- alarm --------------------------------------------------------------
    juce::Colour danger; ///< clip indication, and nothing else

    /** The tint for a band at a given heat, 0 (cold) to 1 (glowing).

        emberDeep -> ember -> emberHot, interpolated in a way that keeps the
        midpoint on the accent hue so a half-driven band looks like Ember rather
        than like an interpolation artefact. */
    [[nodiscard]] juce::Colour heatTint(float heat01) const noexcept;
};

//==============================================================================
/** Which palette is live. Both are dark: Ember is a tube unit, so there is no
    light theme. The variant exists to prove the token indirection is real. */
enum class ThemeVariant
{
    emberDefault, ///< #FF7A1A hot orange
    coolEmber     ///< accent shifted to #FFB347 amber-gold
};

//==============================================================================
/** The spacing scale. Every gap in the UI is one of these six numbers.

    Anything that is not on the scale is a mistake, not a refinement: the whole
    point is that a reviewer can spot an off-grid gap by eye. */
namespace Spacing
{
inline constexpr int xs = 4;   ///< control to its own label
inline constexpr int sm = 8;   ///< within a control group; between regions
inline constexpr int md = 12;  ///< between control groups; module padding
inline constexpr int lg = 16;  ///< panel inner padding
inline constexpr int xl = 24;  ///< major separation inside a panel
inline constexpr int xxl = 32; ///< reserved for the widest layouts
} // namespace Spacing

/** Corner radii and stroke weights, in unscaled pixels. */
namespace Metrics
{
inline constexpr float panelRadius = 6.0f;
inline constexpr float controlRadius = 4.0f;
inline constexpr float bevel = 1.0f;       ///< top-edge highlight
inline constexpr float hairline = 1.0f;    ///< separators
inline constexpr float arcTrack = 3.0f;    ///< knob arc, unfilled
inline constexpr float arcFill = 3.0f;     ///< knob arc, filled
inline constexpr float arcGlow = 2.0f;     ///< the bloom under the fill
inline constexpr int shadowRadius = 10;    ///< raised-panel drop shadow
inline constexpr int shadowOffsetY = 2;    ///< light comes from above-left
inline constexpr int headerHeight = 52;
inline constexpr int footerHeight = 44;
inline constexpr int modRailCollapsed = 36;

/** The three middle regions split the remaining height in this ratio. */
inline constexpr int displayWeight = 38;
inline constexpr int bandStripWeight = 34;
inline constexpr int modRailWeight = 18;

inline constexpr int defaultWidth = 1180;
inline constexpr int defaultHeight = 700;
inline constexpr int minWidth = 860;
/** 860 at the locked 1180:700 aspect. The brief asks for 520, which that aspect
    cannot produce; width is taken as binding. See docs/UI-DESIGN.md. */
inline constexpr int minHeight = 510;
} // namespace Metrics

//==============================================================================
/** Brushed-metal panel texture, generated once and tiled.

    Directional noise: white noise smeared horizontally so it reads as a
    machined surface under a light from the top-left. Drawn at ~6 % opacity, it
    is the difference between "dark grey rectangle" and "metal". */
class TextureCache
{
public:
    /** A tileable brushed sheet. Generated on first call, then shared. */
    [[nodiscard]] static const juce::Image& brushed();

    /** Fills `area` with the brushed sheet at `opacity`, tiling as needed. */
    static void fillBrushed(juce::Graphics&, juce::Rectangle<int> area, float opacity = 0.06f);
};

//==============================================================================
/** Pre-blurred glow sprites.

    Blurring per frame is what makes a glowing UI expensive, so Ember never does
    it: a small set of radial sprites is rendered once at startup and composited
    additively. `paint()` on the hot paths does no allocation and no filtering.

    Sprites are single-channel white; tint them when drawing. */
class GlowCache
{
public:
    enum class Size
    {
        small,  ///< 32 px - LEDs, small knob rings
        medium, ///< 64 px - knob caps, module title bars
        large   ///< 128 px - spectrum band regions, the window vignette
    };

    /** The sprite for a size. Built on first use. */
    [[nodiscard]] static const juce::Image& sprite(Size);

    /** Draws a tinted glow centred on `centre` with the given radius.

        `intensity` scales alpha, not radius, so a brightening glow does not
        change layout. Radius picks the sprite; anything between sizes is
        scaled, which is cheap because it is a blit, not a blur. */
    static void draw(juce::Graphics&, juce::Point<float> centre, float radius, juce::Colour tint,
                     float intensity);

    /** A glow smeared along a rounded rectangle rather than a point — the band
        regions and module title bars use this. */
    static void drawForRect(juce::Graphics&, juce::Rectangle<float> area, float cornerRadius,
                            juce::Colour tint, float intensity);
};

//==============================================================================
/** The live theme. Message thread only, which is where painting happens. */
class EmberTheme
{
public:
    /** The colours currently in force. */
    [[nodiscard]] static const ThemeTokens& tokens() noexcept;

    [[nodiscard]] static ThemeVariant variant() noexcept;

    /** Switches palette. Every component repaints from `tokens()`, so the caller
        only has to repaint the editor. */
    static void setVariant(ThemeVariant) noexcept;

    /** When true, bloom and transitions are suppressed; meters, values and heat
        stay live. Honours the user's own "reduce motion" setting. */
    [[nodiscard]] static bool reduceMotion() noexcept;
    static void setReduceMotion(bool) noexcept;

    /** Contrast ratio between two opaque colours, WCAG 2.1 relative luminance.
        Used by the theme test, and by anything that needs to prove a pairing. */
    [[nodiscard]] static float contrastRatio(juce::Colour a, juce::Colour b) noexcept;
};
} // namespace ember::gui
