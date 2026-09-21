// The theme's two structural guarantees, enforced rather than documented:
//
//   1. Every text/surface pairing the UI actually uses clears WCAG AA (4.5:1).
//   2. No colour literal exists in src/gui outside EmberTheme.cpp.
//
// The second is what makes the first meaningful. A contrast table proves
// nothing if a panel can quietly call juce::Colour (0xff808080) — and that is
// exactly the drift that turns a designed interface back into a grey one.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "gui/EmberTheme.h"
#include "gui/EmberLookAndFeel.h"

using namespace ember::gui;

namespace
{
/** Every pairing the design actually puts on screen. If a component invents a
    new one it belongs here first. */
struct Pairing
{
    const char* what;
    juce::Colour fg;
    juce::Colour bg;
};

std::vector<Pairing> textPairings(const ThemeTokens& t)
{
    return {
        {"text on bgDeep", t.text, t.bgDeep},
        {"text on panel", t.text, t.panel},
        {"text on panelRaised", t.text, t.panelRaised},
        {"textDim on bgDeep", t.textDim, t.bgDeep},
        {"textDim on panel", t.textDim, t.panel},
        {"textDim on panelRaised", t.textDim, t.panelRaised},
        {"ember on panel", t.ember, t.panel},
        {"ember on bgDeep", t.ember, t.bgDeep},
        {"emberHot on panel", t.emberHot, t.panel},
        {"signal on panel", t.signal, t.panel},
        {"cold on panel", t.cold, t.panel},
        {"danger on panel", t.danger, t.panel},
    };
}
} // namespace

TEST_CASE("every text pairing clears WCAG AA", "[theme]")
{
    for (auto variant : {ThemeVariant::emberDefault, ThemeVariant::coolEmber})
    {
        EmberTheme::setVariant(variant);
        const auto& t = EmberTheme::tokens();

        for (const auto& p : textPairings(t))
        {
            INFO("variant " << static_cast<int>(variant) << ", pairing: " << p.what);
            REQUIRE(EmberTheme::contrastRatio(p.fg, p.bg) >= 4.5f);
        }
    }

    EmberTheme::setVariant(ThemeVariant::emberDefault);
}

TEST_CASE("textMuted is decorative and says so", "[theme]")
{
    // textMuted exists for grid lines and separators. It is deliberately below
    // the text threshold, and this test pins that intent: if someone raises it
    // to make it usable for text, they have to come here and decide that on
    // purpose rather than by accident.
    const auto& t = EmberTheme::tokens();
    REQUIRE(EmberTheme::contrastRatio(t.textMuted, t.panel) < 4.5f);
}

TEST_CASE("heat ramps from deep through accent to hot", "[theme]")
{
    const auto& t = EmberTheme::tokens();

    REQUIRE(t.heatTint(0.0f) == t.emberDeep);
    REQUIRE(t.heatTint(0.5f) == t.ember);
    REQUIRE(t.heatTint(1.0f) == t.emberHot);

    // Out of range must clamp, not wrap: heat arrives from a smoother that can
    // overshoot slightly.
    REQUIRE(t.heatTint(-1.0f) == t.emberDeep);
    REQUIRE(t.heatTint(2.0f) == t.emberHot);

    // Brightness must rise monotonically, or a band getting hotter would
    // visually dip partway.
    float previous = -1.0f;
    for (int i = 0; i <= 20; ++i)
    {
        const auto brightness = t.heatTint(static_cast<float>(i) / 20.0f).getPerceivedBrightness();
        REQUIRE(brightness >= previous - 1.0e-4f);
        previous = brightness;
    }
}

TEST_CASE("the accent variant moves the accent and nothing else", "[theme]")
{
    EmberTheme::setVariant(ThemeVariant::emberDefault);
    const auto base = EmberTheme::tokens();

    EmberTheme::setVariant(ThemeVariant::coolEmber);
    const auto cool = EmberTheme::tokens();

    REQUIRE(cool.ember != base.ember);

    // Surfaces, text, modulation and clipping are shared. If a future variant
    // starts moving these, the variant stops being a proof that the token
    // indirection works and becomes a second theme to maintain.
    REQUIRE(cool.bgDeep == base.bgDeep);
    REQUIRE(cool.panel == base.panel);
    REQUIRE(cool.panelRaised == base.panelRaised);
    REQUIRE(cool.text == base.text);
    REQUIRE(cool.textDim == base.textDim);
    REQUIRE(cool.cold == base.cold);
    REQUIRE(cool.danger == base.danger);

    EmberTheme::setVariant(ThemeVariant::emberDefault);
}

TEST_CASE("the embedded typefaces are present", "[theme]")
{
    // If this fails the UI still runs, but it has fallen back to system faces
    // and stops being identical across platforms - which is the whole reason
    // the fonts are embedded.
    REQUIRE(EmberFonts::usingEmbeddedFaces());
}

TEST_CASE("values use tabular figures", "[theme]")
{
    // Proportional digits make a dragged value jitter, because "1" is narrower
    // than "8". Measuring is the only honest check that tnum actually applied:
    // asking the Font for its feature list would just echo what we set.
    const auto value = EmberFonts::get(EmberFonts::Role::value);

    const auto ones = juce::GlyphArrangement::getStringWidth(value, "111111");
    const auto eights = juce::GlyphArrangement::getStringWidth(value, "888888");

    REQUIRE_THAT(ones, Catch::Matchers::WithinAbs(eights, 0.05));
}

TEST_CASE("no role falls below the 10 px legibility floor", "[theme]")
{
    for (auto role : {EmberFonts::Role::logo, EmberFonts::Role::section, EmberFonts::Role::label,
                      EmberFonts::Role::value, EmberFonts::Role::display, EmberFonts::Role::footer})
    {
        for (auto scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
        {
            INFO("role " << static_cast<int>(role) << " at scale " << scale);
            REQUIRE(EmberFonts::get(role, scale).getHeight() >= 10.0f);
        }
    }
}

TEST_CASE("colour literals live only in EmberTheme.cpp", "[theme]")
{
    // A ratchet, not a wish. Files still carrying pre-redesign literals are
    // listed here with their current count; the check fails if any count goes
    // UP, or if a file not on the list gains a literal at all.
    //
    // As each panel is rebuilt against EmberTheme::tokens() its entry drops to
    // zero and is deleted. When the list is empty the redesign's token rule is
    // fully enforced and this becomes the strict test it is named after.
    struct Budget
    {
        const char* file;
        int allowed;
    };

    const Budget budgets[] = {};

    const juce::File guiDir{juce::String{EMBER_SOURCE_DIR} + "/src/gui"};
    REQUIRE(guiDir.isDirectory());

    for (const auto& entry : juce::RangedDirectoryIterator{guiDir, false, "*.cpp;*.h"})
    {
        const auto file = entry.getFile();
        const auto name = file.getFileName();

        if (name.startsWith("EmberTheme."))
            continue;

        int found = 0;

        for (const auto& line : juce::StringArray::fromLines(file.loadFileAsString()))
        {
            // transparentBlack is not a colour decision - it is JUCE's way of
            // saying "draw nothing", used to switch off a stock component's
            // background or outline. Counting it would force every such line
            // through the palette to express an absence.
            const auto stripped = line.replace("juce::Colours::transparentBlack", "");

            if (stripped.contains("juce::Colour(0x") || stripped.contains("juce::Colour (0x")
                || stripped.contains("Colour::fromRGB") || stripped.contains("Colour::fromHSV")
                || stripped.contains("juce::Colours::"))
                ++found;
        }

        int allowed = 0;
        for (const auto& b : budgets)
            if (name == b.file)
                allowed = b.allowed;

        INFO(name << " holds " << found << " colour literals, budget " << allowed
                  << ". If you removed some, lower the budget in tests/test_theme.cpp.");
        REQUIRE(found <= allowed);
    }
}
