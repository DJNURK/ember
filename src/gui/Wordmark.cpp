#include "gui/Wordmark.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include "plugin/PluginProcessor.h"

namespace ember::gui
{
namespace
{
constexpr int kRefreshHz = 30;

/** The bulb-and-hairpin glyph, drawn into a unit square.

    Built by hand rather than taken from a font: a filament has to be a path we
    can stroke at varying brightness, and no typeface glyph gives us the wire
    separately from the glass. */
juce::Path makeFilamentGlass()
{
    juce::Path p;

    // The envelope: a rounded bulb with a short neck, flattened at the base.
    p.startNewSubPath(0.5f, 0.06f);
    p.cubicTo(0.86f, 0.06f, 0.96f, 0.36f, 0.78f, 0.58f);
    p.lineTo(0.70f, 0.74f);
    p.lineTo(0.30f, 0.74f);
    p.lineTo(0.22f, 0.58f);
    p.cubicTo(0.04f, 0.36f, 0.14f, 0.06f, 0.5f, 0.06f);
    p.closeSubPath();

    return p;
}

juce::Path makeFilamentWire()
{
    juce::Path p;

    // A hairpin: down one side, a loop at the bottom, back up the other. The
    // asymmetry is deliberate - a perfectly symmetric wire reads as a drawing,
    // a slightly uneven one reads as a real component.
    p.startNewSubPath(0.37f, 0.72f);
    p.lineTo(0.37f, 0.44f);
    p.cubicTo(0.37f, 0.26f, 0.63f, 0.26f, 0.63f, 0.46f);
    p.lineTo(0.63f, 0.72f);

    return p;
}
} // namespace

Wordmark::Wordmark(EmberAudioProcessor& processorToUse) : processor(processorToUse)
{
    setInterceptsMouseClicks(false, false);
    startTimerHz(kRefreshHz);
}

Wordmark::~Wordmark() = default;

int Wordmark::preferredWidth(int forHeight) const
{
    // The glyph is square, then a gap, then the word at roughly 2.6 times its
    // own height in width. Measured from the cached path where we have one.
    const auto h = static_cast<float>(juce::jmax(1, forHeight));

    if (!wordPath.isEmpty())
        return juce::roundToInt(h * 0.62f + h * 0.22f + wordPath.getBounds().getWidth());

    return juce::roundToInt(h * 3.0f);
}

void Wordmark::resized()
{
    rebuildPath();
}

void Wordmark::rebuildPath()
{
    wordPath.clear();

    const auto bounds = getLocalBounds().toFloat();

    if (bounds.getHeight() < 6.0f || bounds.getWidth() < 12.0f)
        return;

    const float glyphSize = bounds.getHeight() * 0.62f;
    filamentArea = juce::Rectangle<float>(glyphSize, glyphSize)
                       .withCentre({bounds.getX() + glyphSize * 0.55f, bounds.getCentreY()});

    // The letterforms are cached as a path so paint() never lays out text. At
    // 30 Hz with a glow behind it, re-shaping a string every frame is the kind
    // of cost that does not show up until someone opens six instances.
    const auto font = EmberFonts::get(EmberFonts::Role::logo, bounds.getHeight() / 22.0f);

    juce::GlyphArrangement arrangement;
    arrangement.addLineOfText(font, "EMBER", 0.0f, 0.0f);
    arrangement.createPath(wordPath);

    if (wordPath.isEmpty())
        return;

    const auto wordBounds = wordPath.getBounds();
    const float targetHeight = bounds.getHeight() * 0.38f;

    if (wordBounds.getHeight() > 0.1f)
    {
        const float wordScale = targetHeight / wordBounds.getHeight();
        wordPath.applyTransform(juce::AffineTransform::scale(wordScale));
    }

    // Fit the word into whatever width is actually left beside the glyph.
    // preferredWidth() has to answer before the path exists, so it can only
    // estimate - and when the estimate is short the word gets clipped to
    // "EMBE". Measuring here, where the real letterforms are known, is the
    // only place this can be got right.
    const float wordLeft = filamentArea.getRight() + bounds.getHeight() * 0.22f;
    const float roomForWord = bounds.getRight() - wordLeft;

    if (roomForWord < 4.0f)
    {
        wordPath.clear();
        return;
    }

    auto scaled = wordPath.getBounds();

    if (scaled.getWidth() > roomForWord)
        wordPath.applyTransform(juce::AffineTransform::scale(roomForWord / scaled.getWidth()));

    scaled = wordPath.getBounds();
    wordPath.applyTransform(
        juce::AffineTransform::translation(wordLeft - scaled.getX(), bounds.getCentreY() - scaled.getCentreY()));
}

void Wordmark::timerCallback()
{
    const float target = processor.getGlobalHeat();

    // The same 30 ms attack / 400 ms release the bands use, so the logo and the
    // spectrum warm and cool together rather than drifting apart.
    const float attack = 1.0f - std::exp(-1.0f / (0.030f * static_cast<float>(kRefreshHz)));
    const float release = 1.0f - std::exp(-1.0f / (0.400f * static_cast<float>(kRefreshHz)));

    const float previous = heat;
    heat += (target - heat) * (target > heat ? attack : release);

    if (std::abs(heat - previous) > 0.002f)
        repaint();
}

void Wordmark::paint(juce::Graphics& g)
{
    const auto& tk = EmberTheme::tokens();

    if (filamentArea.isEmpty())
        return;

    const float lit = juce::jlimit(0.0f, 1.0f, heat);

    // ---- the glass -------------------------------------------------------
    auto glass = makeFilamentGlass();
    glass.applyTransform(juce::AffineTransform::scale(filamentArea.getWidth(), filamentArea.getHeight())
                             .translated(filamentArea.getX(), filamentArea.getY()));

    if (lit > 0.01f && !EmberTheme::reduceMotion())
        GlowCache::draw(g, filamentArea.getCentre(), filamentArea.getWidth() * (0.7f + lit * 0.6f), tk.tubeGlow,
                        lit * 0.75f);

    g.setColour(tk.panelEdge.withAlpha(0.7f));
    g.strokePath(glass, juce::PathStrokeType(juce::jmax(1.0f, filamentArea.getWidth() * 0.05f)));

    // ---- the wire --------------------------------------------------------
    auto wire = makeFilamentWire();
    wire.applyTransform(juce::AffineTransform::scale(filamentArea.getWidth(), filamentArea.getHeight())
                            .translated(filamentArea.getX(), filamentArea.getY()));

    const float wireWidth = juce::jmax(1.2f, filamentArea.getWidth() * 0.085f);

    // Cold wire is visible but dead; hot wire runs through the ember ramp to
    // its brightest, which is the same ramp the bands use.
    g.setColour(tk.textMuted.withAlpha(0.8f));
    g.strokePath(wire, juce::PathStrokeType(wireWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (lit > 0.01f)
    {
        g.setColour(tk.heatTint(lit).withAlpha(juce::jlimit(0.0f, 1.0f, 0.25f + lit * 0.75f)));
        g.strokePath(wire,
                     juce::PathStrokeType(wireWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ---- the word --------------------------------------------------------
    if (!wordPath.isEmpty())
    {
        // The word warms slightly too, but far less than the filament: the
        // filament is the indicator, the word is identity.
        g.setColour(tk.text.interpolatedWith(tk.emberHot, lit * 0.35f));
        g.fillPath(wordPath);
    }
}
} // namespace ember::gui
