// Renders the assembled Ember editor offscreen to a PNG.
//
// The plugin GUI is otherwise only inspectable by opening it on a machine with
// a display and the right permissions, which rules out CI and any headless
// session. This walks the real editor - the same one createEditor() returns,
// with a real processor behind it - and writes what it paints.
//
//   ember_rendereditor <output.png> [width] [height] [scale] [preset] [driveDb] [variant] [clickID]
//
// `clickID` is the component id of a button to click before the snapshot, so
// panels that are hidden until asked for can be photographed too:
//
//   header.help  the Learn panel        header.mod  the modulation rail
//
// Without one of these, a whole v2.1 feature can only be inspected by opening
// the plugin in a host, which is exactly the gap that let an empty band strip
// ship in two releases of documentation.
//
// `variant` is 0 for Ember and 1 for Cool Ember. Rendering both is how the
// token system gets checked: if any component hard-codes an accent it shows up
// immediately as the one thing that did not move.
//
// `preset` is a factory program index; without it the editor is photographed
// in its default state, which shows no preset name and few active controls.
//
// Defaults to the plugin's own default size at 2x. It is worth rendering at the
// minimum supported size too (800x480): that is where a layout gives out, and a
// clipped panel is invisible to every unit test in the suite.
#include <juce_gui_extra/juce_gui_extra.h>
#include <cstdio>
#include <functional>
#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"
#include "plugin/ParameterIDs.h"
#include "gui/EmberTheme.h"

int main(int argc, char** argv)
{
    const juce::File out(
        argc > 1 ? juce::String(argv[1])
                 : juce::File::getCurrentWorkingDirectory().getChildFile("ember-editor.png").getFullPathName());
    const int width = argc > 2 ? juce::String(argv[2]).getIntValue() : 1100;
    const int height = argc > 3 ? juce::String(argv[3]).getIntValue() : 640;
    const float scale = argc > 4 ? juce::String(argv[4]).getFloatValue() : 2.0f;
    const int preset = argc > 5 ? juce::String(argv[5]).getIntValue() : -1;
    // Applied to every band, for the heat renders: the display should look
    // visibly different at 0, 15 and 35 dB or the heat metaphor is not working.
    const float driveDb = argc > 6 ? juce::String(argv[6]).getFloatValue() : -1000.0f;
    const int variant = argc > 7 ? juce::String(argv[7]).getIntValue() : 0;
    const juce::String clickID = argc > 8 ? juce::String(argv[8]) : juce::String();

    ember::gui::EmberTheme::setVariant(variant == 1 ? ember::gui::ThemeVariant::coolEmber
                                                    : ember::gui::ThemeVariant::emberDefault);

    ember::EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    if (preset >= 0 && preset < proc.getNumPrograms())
    {
        proc.setCurrentProgram(preset);
        std::printf("preset %d: %s\n", preset, proc.getProgramName(preset).toRawUTF8());
    }

    if (driveDb > -999.0f)
    {
        for (int b = 0; b < ember::kMaxBands; ++b)
            if (auto* p = proc.getAPVTS().getParameter(ember::pid::drive(b)))
                p->setValueNotifyingHost(p->convertTo0to1(driveDb));

        std::printf("drive %.1f dB on every band\n", static_cast<double>(driveDb));
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    if (editor == nullptr)
    {
        std::puts("createEditor() returned null");
        return 1;
    }
    editor->setSize(width, height);

    // Comma-separated, so a panel two clicks deep can be reached: open the
    // modulation rail, then select the source whose controls you want to see.
    for (const auto& oneID : juce::StringArray::fromTokens(clickID, ",", ""))
    {
        const auto clickID = oneID.trim();

        if (clickID.isEmpty())
            continue;

        // Depth-first, because ids are unique and the first match is the only
        // match; a miss is a hard error rather than a silently ordinary
        // screenshot, which would be indistinguishable from the panel failing
        // to open.
        std::function<juce::Component*(juce::Component&)> find = [&](juce::Component& c) -> juce::Component*
        {
            if (c.getComponentID() == clickID)
                return &c;

            for (auto* child : c.getChildren())
                if (auto* found = find(*child))
                    return found;

            return nullptr;
        };

        auto* target = find(*editor);

        // Not every clickable thing claims a tour id - the modulation rail's
        // source list does not - so fall back to matching a button's label.
        if (target == nullptr)
        {
            std::function<juce::Component*(juce::Component&)> byText = [&](juce::Component& c) -> juce::Component*
            {
                if (auto* b = dynamic_cast<juce::Button*>(&c); b != nullptr && b->getButtonText() == clickID)
                    return b;

                for (auto* child : c.getChildren())
                    if (auto* found = byText(*child))
                        return found;

                return nullptr;
            };

            target = byText(*editor);
        }

        if (target == nullptr)
        {
            std::printf("no component with id '%s'\n", clickID.toRawUTF8());
            return 1;
        }

        auto* button = dynamic_cast<juce::Button*>(target);

        if (button == nullptr)
        {
            std::printf("'%s' is not a button\n", clickID.toRawUTF8());
            return 1;
        }

        // triggerClick() posts the callback through the message queue, and this
        // build has JUCE_MODAL_LOOPS_PERMITTED=0, so there is no dispatch loop
        // to drain it with and the click would never arrive. Button::onClick is
        // public and is what the queued callback ends up invoking, so call it
        // directly - synchronously, which is what a screenshot needs.
        if (button->onClick)
            button->onClick();
        else
            button->triggerClick();

        editor->resized();
        std::printf("clicked %s\n", clickID.toRawUTF8());
    }

    // Audio runs AFTER the editor exists. The spectrum analyser is only
    // switched on while an editor is open - it is skipped otherwise, which is
    // what keeps a headless instance cheap - so pushing audio first produced a
    // perfectly empty display in every screenshot taken so far.
    {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        juce::Random rng(1234);
        for (int i = 0; i < 400; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < 512; ++n)
                    buf.setSample(ch, n, (rng.nextFloat() * 2.0f - 1.0f) * 0.35f);
            proc.processBlock(buf, midi);
        }
    }

    for (int b = 0; b < 3; ++b)
        std::printf("band %d heat %.3f\n", b, static_cast<double>(proc.getBandHeat(b)));

    // Let the panels' timers run so spectra, meters and modulation readouts
    // populate. The plugin is built with JUCE_MODAL_LOOPS_PERMITTED=0, so a
    // dispatch loop is unavailable; this drives the same callbacks without one.
    for (int i = 0; i < 80; ++i)
    {
        juce::Thread::sleep(20);
        juce::Timer::callPendingTimersSynchronously();
    }

    // EMBER_DUMP_BOUNDS=1 prints every identified component's rectangle in
    // editor coordinates. A screenshot shows that two things overlap; this says
    // which one is in the wrong place, which is otherwise a guess.
    if (juce::SystemStats::getEnvironmentVariable("EMBER_DUMP_BOUNDS", {}).getIntValue() == 1)
    {
        std::function<void(juce::Component&)> dump = [&](juce::Component& c)
        {
            if (c.getComponentID().isNotEmpty())
            {
                const auto r = editor->getLocalArea(&c, c.getLocalBounds());
                std::printf("  %-28s %4d %4d %4dx%-4d %s\n", c.getComponentID().toRawUTF8(), r.getX(), r.getY(),
                            r.getWidth(), r.getHeight(), c.isVisible() ? "" : "(hidden)");
            }

            for (auto* child : c.getChildren())
                dump(*child);
        };

        std::puts("bounds:");
        dump(*editor);
    }

    const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, scale);

    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::PNGImageFormat png;
    std::unique_ptr<juce::OutputStream> stream(out.createOutputStream().release());
    if (stream == nullptr || !png.writeImageToStream(image, *stream))
    {
        std::printf("could not write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf("wrote %s (%d x %d)\n", out.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
