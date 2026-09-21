#include "gui/EmberTheme.h"
#include <atomic>
#include <algorithm>
#include <cmath>

namespace ember::gui
{
namespace
{
//==============================================================================
// The two palettes. These are the only colour literals in the GUI.
//
// Everything below this block reads them through EmberTheme::tokens(), and
// tests/test_theme.cpp enforces that for the rest of src/gui.

ThemeTokens makeDefaultTokens()
{
    ThemeTokens t;

    t.bgDeep = juce::Colour(0xff0b0b0d);
    t.panel = juce::Colour(0xff15151a);
    t.panelRaised = juce::Colour(0xff1d1d24);
    t.panelEdge = juce::Colour(0xff2a2a33);
    t.panelShadow = juce::Colour(0xff050507);

    t.text = juce::Colour(0xffe8e4dc);
    t.textDim = juce::Colour(0xff8a8780);
    t.textMuted = juce::Colour(0xff55534e);

    t.ember = juce::Colour(0xffff7a1a);
    t.emberHot = juce::Colour(0xffffb347);
    t.emberDeep = juce::Colour(0xffb3420b);
    t.tubeGlow = juce::Colour(0xffff5a00);
    t.signal = juce::Colour(0xfff2c14e);

    // The brief specifies #5A7FA8, which measures 4.36:1 on `panel` and so
    // fails the brief's own 4.5:1 requirement. Lifted 1 % in lightness, same
    // hue and saturation, to 4.54:1. Accessibility wins the tie.
    t.cold = juce::Colour(0xff5d82aa);
    t.danger = juce::Colour(0xffff3b30);

    return t;
}

ThemeTokens makeCoolEmberTokens()
{
    // Only the accent ramp moves. Surfaces, text, modulation and clipping are
    // shared, which is the point: if a component hard-codes an accent, this
    // variant exposes it and nothing else changes to mask the difference.
    auto t = makeDefaultTokens();
    t.ember = juce::Colour(0xffffb347);
    t.emberHot = juce::Colour(0xffffd98a);
    t.emberDeep = juce::Colour(0xffc4761a);
    t.tubeGlow = juce::Colour(0xffff9a2e);
    return t;
}

const ThemeTokens& defaultTokens()
{
    static const ThemeTokens t = makeDefaultTokens();
    return t;
}

const ThemeTokens& coolEmberTokens()
{
    static const ThemeTokens t = makeCoolEmberTokens();
    return t;
}

std::atomic<ThemeVariant> currentVariant{ThemeVariant::emberDefault};
std::atomic<bool> motionReduced{false};

} // namespace

//==============================================================================
juce::Colour ThemeTokens::heatTint(float heat01) const noexcept
{
    const auto h = juce::jlimit(0.0f, 1.0f, heat01);

    // Two segments rather than one, so the midpoint lands exactly on the accent
    // rather than somewhere between deep and hot. A band at half heat should
    // look like Ember's accent colour, not like an interpolation.
    if (h <= 0.5f)
        return emberDeep.interpolatedWith(ember, h * 2.0f);

    return ember.interpolatedWith(emberHot, (h - 0.5f) * 2.0f);
}

//==============================================================================
const juce::Image& TextureCache::brushed()
{
    static const juce::Image sheet = []
    {
        // 256x256 tiles cleanly and costs 256 KB once. Larger would show less
        // repetition but this is drawn at 6 % alpha, where repetition is
        // invisible; smaller starts to read as a pattern.
        constexpr int size = 256;
        juce::Image img{juce::Image::ARGB, size, size, true};

        juce::Random rng{0x5eed1234};

        // White noise smeared horizontally: generate one row of noise per line,
        // then run a short box blur along x. That is what makes it read as
        // brushed metal rather than as film grain - the grain has a direction.
        std::vector<float> row(static_cast<size_t>(size));
        juce::Image::BitmapData data{img, juce::Image::BitmapData::writeOnly};

        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
                row[static_cast<size_t>(x)] = rng.nextFloat();

            // Wrap-around blur so the tile is seamless left-to-right.
            constexpr int radius = 6;
            for (int x = 0; x < size; ++x)
            {
                float acc = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                    acc += row[static_cast<size_t>((x + k + size) % size)];

                const auto v = acc / static_cast<float>(2 * radius + 1);

                // Centre on mid-grey: the sheet is composited over the panel, so
                // it must darken and lighten equally or it shifts the surface.
                const auto level = juce::jlimit(0, 255, juce::roundToInt(v * 255.0f));
                data.setPixelColour(x, y, juce::Colour::fromRGBA(255, 255, 255, static_cast<juce::uint8>(level)));
            }
        }

        return img;
    }();

    return sheet;
}

void TextureCache::fillBrushed(juce::Graphics& g, juce::Rectangle<int> area, float opacity)
{
    if (area.isEmpty() || opacity <= 0.0f)
        return;

    const auto& sheet = brushed();

    juce::Graphics::ScopedSaveState save{g};
    g.reduceClipRegion(area);
    g.setTiledImageFill(sheet, area.getX(), area.getY(), juce::jlimit(0.0f, 1.0f, opacity));
    g.fillRect(area);
}

//==============================================================================
namespace
{
juce::Image makeGlowSprite(int diameter)
{
    juce::Image img{juce::Image::ARGB, diameter, diameter, true};
    juce::Image::BitmapData data{img, juce::Image::BitmapData::writeOnly};

    const auto radius = static_cast<float>(diameter) * 0.5f;
    const auto centre = radius;

    for (int y = 0; y < diameter; ++y)
    {
        for (int x = 0; x < diameter; ++x)
        {
            const auto dx = (static_cast<float>(x) + 0.5f) - centre;
            const auto dy = (static_cast<float>(y) + 0.5f) - centre;
            const auto d = std::sqrt(dx * dx + dy * dy) / radius;

            // A Gaussian-ish falloff that actually reaches zero at the edge. A
            // true Gaussian does not, and the resulting hard circular cut is
            // visible once several sprites overlap.
            const auto t = juce::jlimit(0.0f, 1.0f, 1.0f - d);
            const auto a = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); // smootherstep

            data.setPixelColour(
                x, y,
                juce::Colour::fromRGBA(255, 255, 255,
                                       static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(a * 255.0f)))));
        }
    }

    return img;
}
} // namespace

const juce::Image& GlowCache::sprite(Size size)
{
    static const juce::Image small = makeGlowSprite(32);
    static const juce::Image medium = makeGlowSprite(64);
    static const juce::Image large = makeGlowSprite(128);

    switch (size)
    {
    case Size::small:
        return small;
    case Size::medium:
        return medium;
    case Size::large:
        return large;
    }

    return medium;
}

void GlowCache::draw(juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour tint, float intensity)
{
    const auto alpha = juce::jlimit(0.0f, 1.0f, intensity);

    if (alpha <= 0.001f || radius <= 0.0f || EmberTheme::reduceMotion())
        return;

    const auto size = radius <= 16.0f ? Size::small : radius <= 32.0f ? Size::medium : Size::large;

    const auto& s = sprite(size);
    const auto d = radius * 2.0f;

    juce::Graphics::ScopedSaveState save{g};
    g.setColour(tint.withAlpha(alpha));
    g.drawImageTransformed(
        s,
        juce::AffineTransform::scale(d / static_cast<float>(s.getWidth()), d / static_cast<float>(s.getHeight()))
            .translated(centre.x - radius, centre.y - radius),
        true);
}

void GlowCache::drawForRect(juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius, juce::Colour tint,
                            float intensity)
{
    const auto alpha = juce::jlimit(0.0f, 1.0f, intensity);

    if (alpha <= 0.001f || area.isEmpty() || EmberTheme::reduceMotion())
        return;

    // Three expanding rounded rectangles at falling alpha approximate a blur
    // around a shape far more cheaply than blurring one, and unlike a sprite it
    // follows the shape's aspect ratio.
    for (int i = 3; i >= 1; --i)
    {
        const auto grow = static_cast<float>(i) * 3.0f;
        const auto a = alpha * (0.16f / static_cast<float>(i));
        g.setColour(tint.withAlpha(a));
        g.drawRoundedRectangle(area.expanded(grow), cornerRadius + grow, 2.0f);
    }
}

//==============================================================================
const ThemeTokens& EmberTheme::tokens() noexcept
{
    return currentVariant.load(std::memory_order_relaxed) == ThemeVariant::coolEmber ? coolEmberTokens()
                                                                                     : defaultTokens();
}

ThemeVariant EmberTheme::variant() noexcept
{
    return currentVariant.load(std::memory_order_relaxed);
}

void EmberTheme::setVariant(ThemeVariant v) noexcept
{
    currentVariant.store(v, std::memory_order_relaxed);
}

bool EmberTheme::reduceMotion() noexcept
{
    return motionReduced.load(std::memory_order_relaxed);
}

void EmberTheme::setReduceMotion(bool shouldReduce) noexcept
{
    motionReduced.store(shouldReduce, std::memory_order_relaxed);
}

float EmberTheme::contrastRatio(juce::Colour a, juce::Colour b) noexcept
{
    const auto luminance = [](juce::Colour c)
    {
        const auto channel = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };

        return 0.2126f * channel(c.getFloatRed()) + 0.7152f * channel(c.getFloatGreen()) +
               0.0722f * channel(c.getFloatBlue());
    };

    const auto la = luminance(a);
    const auto lb = luminance(b);

    return (juce::jmax(la, lb) + 0.05f) / (juce::jmin(la, lb) + 0.05f);
}
} // namespace ember::gui
