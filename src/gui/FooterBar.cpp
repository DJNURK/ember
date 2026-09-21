#include "gui/FooterBar.h"
#include "gui/EmberLookAndFeel.h"
#include "gui/EmberTheme.h"
#include "plugin/ParameterIDs.h"
#include "plugin/PluginProcessor.h"

namespace ember::gui
{
FooterBar::FooterBar(EmberAudioProcessor& processorToUse)
    : processor(processorToUse), inputKnob(processorToUse, pid::inputGain, "Input"),
      outputKnob(processorToUse, pid::outputGain, "Output"), mixKnob(processorToUse, pid::globalMix, "Mix")
{
    inputKnob.getKnob().setExtraTooltipText("Level into the crossover, before any saturation.");
    outputKnob.getKnob().setExtraTooltipText("Level after the bands are summed.");
    mixKnob.getKnob().setExtraTooltipText("Dry/wet blend across the whole plugin.");

    for (auto* knob : {&inputKnob, &outputKnob, &mixKnob})
        addAndMakeVisible(knob);

    autoGainToggle.attachTo(processorToUse.getAPVTS(), pid::autoGain);
    autoGainToggle.setButtonText("AUTO GAIN");
    autoGainToggle.setTooltip("Auto Gain\nCompensates the level the drive adds, so styles and drive "
                              "settings can be compared by character rather than by loudness.");
    addAndMakeVisible(autoGainToggle);

    startTimerHz(20);
}

FooterBar::~FooterBar() = default;

int FooterBar::preferredHeight(float uiScale)
{
    return juce::jlimit(36, 72, juce::roundToInt(static_cast<float>(Metrics::footerHeight) * uiScale));
}

void FooterBar::setHint(const juce::String& text)
{
    if (hint == text)
        return;

    hint = text;
    repaint(hintArea);
}

juce::String FooterBar::hintForComponentUnderMouse() const
{
    auto* top = getTopLevelComponent();

    if (top == nullptr)
        return {};

    const auto screenPosition = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    auto* component = top->getComponentAt(top->getLocalPoint(nullptr, screenPosition));

    // Walk up rather than asking only the leaf: a knob's caption label and its
    // value label are separate components with no tooltip of their own, and
    // hovering either of them should still explain the knob.
    while (component != nullptr)
    {
        if (auto* client = dynamic_cast<juce::TooltipClient*>(component))
        {
            const auto text = client->getTooltip();

            if (text.isNotEmpty())
                return text;
        }

        component = component->getParentComponent();
    }

    return {};
}

void FooterBar::timerCallback()
{
    setHint(hintForComponentUnderMouse());

    const auto cpu = processor.getCpuLoadPercent();
    const auto samples = processor.getLatencySamples();
    const auto rate = processor.getSampleRate();

    if (std::abs(cpu - smoothedCpuPercent) > 0.05f || samples != latencySamples || std::abs(rate - latencyRate) > 1.0)
    {
        smoothedCpuPercent = cpu;
        latencySamples = samples;
        latencyRate = rate;
        repaint(readoutArea);
    }
}

void FooterBar::resized()
{
    const float scale =
        juce::jlimit(0.75f, 2.0f, static_cast<float>(getHeight()) / static_cast<float>(Metrics::footerHeight));

    auto area = getLocalBounds().reduced(juce::roundToInt(static_cast<float>(Spacing::md) * scale),
                                         juce::roundToInt(static_cast<float>(Spacing::xs) * scale));

    if (area.getWidth() < 80 || area.getHeight() < 16)
    {
        hintArea = {};
        readoutArea = {};
        return;
    }

    const int gap = juce::jmax(6, juce::roundToInt(static_cast<float>(Spacing::md) * scale));

    // CPU and latency from the right: they are a fixed width, so taking them
    // first means the hint line gets whatever honestly remains rather than
    // being clipped by them later.
    readoutArea = area.removeFromRight(juce::jmin(area.getWidth() / 3, juce::roundToInt(130.0f * scale)));
    area.removeFromRight(gap);

    const int knobWidth = juce::jlimit(44, 96, juce::roundToInt(62.0f * scale));
    const int toggleWidth = juce::jlimit(70, 140, juce::roundToInt(96.0f * scale));

    for (auto* knob : {&inputKnob, &outputKnob, &mixKnob})
    {
        knob->setBounds(area.removeFromLeft(juce::jmin(knobWidth, area.getWidth())));
        area.removeFromLeft(juce::jmin(gap / 2, area.getWidth()));
    }

    area.removeFromLeft(juce::jmin(gap / 2, area.getWidth()));
    autoGainToggle.setBounds(area.removeFromLeft(juce::jmin(toggleWidth, area.getWidth()))
                                 .withSizeKeepingCentre(juce::jmin(toggleWidth, area.getWidth() + toggleWidth),
                                                        juce::jmin(area.getHeight(), juce::roundToInt(24.0f * scale))));
    area.removeFromLeft(juce::jmin(gap, area.getWidth()));

    hintArea = area;
}

void FooterBar::paint(juce::Graphics& g)
{
    const auto& tk = EmberTheme::tokens();
    const auto area = getLocalBounds().toFloat();

    g.setColour(tk.panel);
    g.fillRect(area);

    // A hairline along the top separates the footer from the chassis without
    // the weight of a full panel edge.
    g.setColour(tk.panelEdge);
    g.fillRect(area.withHeight(Metrics::hairline));

    TextureCache::fillBrushed(g, getLocalBounds(), 0.035f);

    const float scale =
        juce::jlimit(0.75f, 2.0f, static_cast<float>(getHeight()) / static_cast<float>(Metrics::footerHeight));

    if (!readoutArea.isEmpty())
    {
        // Tabular figures, so the percentage does not shuffle the latency text
        // sideways every time it changes.
        g.setFont(EmberFonts::get(EmberFonts::Role::footer, scale));

        const auto cpuText = juce::String(smoothedCpuPercent, 1) + " %";
        const auto latencyText = latencySamples > 0 && latencyRate > 0.0
                                     ? juce::String(1000.0 * latencySamples / latencyRate, 1) + " ms"
                                     : juce::String("0.0 ms");

        auto row = readoutArea;
        auto cpuCell = row.removeFromTop(row.getHeight() / 2);

        // Over 80 % of a core the plugin is close to dropping out, and the
        // number stops being trivia.
        g.setColour(smoothedCpuPercent > 80.0f ? tk.danger : tk.textDim);
        g.drawText(cpuText, cpuCell, juce::Justification::centredRight, false);

        g.setColour(tk.textDim);
        g.drawText(latencyText, row, juce::Justification::centredRight, false);
    }

    if (!hintArea.isEmpty() && hint.isNotEmpty())
    {
        g.setFont(EmberFonts::get(EmberFonts::Role::footer, scale));
        g.setColour(tk.textDim);

        // Only the first line: tooltips carry their interaction hints after a
        // newline, and the footer is one line by design.
        g.drawText(hint.upToFirstOccurrenceOf("\n", false, false), hintArea, juce::Justification::centredLeft, true);
    }
}
} // namespace ember::gui
