// Renders the assembled Ember editor offscreen to a PNG.
//
// The plugin GUI is otherwise only inspectable by opening it on a machine with
// a display and the right permissions, which rules out CI and any headless
// session. This walks the real editor - the same one createEditor() returns,
// with a real processor behind it - and writes what it paints.
//
//   ember_rendereditor <output.png> [width] [height] [scale]
//
// Defaults to the plugin's own default size at 2x. It is worth rendering at the
// minimum supported size too (800x480): that is where a layout gives out, and a
// clipped panel is invisible to every unit test in the suite.
#include <juce_gui_extra/juce_gui_extra.h>
#include <cstdio>
#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

int main(int argc, char** argv)
{
    const juce::File out(
        argc > 1 ? juce::String(argv[1])
                 : juce::File::getCurrentWorkingDirectory().getChildFile("ember-editor.png").getFullPathName());
    const int width = argc > 2 ? juce::String(argv[2]).getIntValue() : 1100;
    const int height = argc > 3 ? juce::String(argv[3]).getIntValue() : 640;
    const float scale = argc > 4 ? juce::String(argv[4]).getFloatValue() : 2.0f;

    ember::EmberAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    // Push audio through first so the spectrum display and the meters have real
    // data to draw, rather than photographing an idle plugin.
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

    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    if (editor == nullptr)
    {
        std::puts("createEditor() returned null");
        return 1;
    }
    editor->setSize(width, height);

    // Let the panels' timers run so spectra, meters and modulation readouts
    // populate. The plugin is built with JUCE_MODAL_LOOPS_PERMITTED=0, so a
    // dispatch loop is unavailable; this drives the same callbacks without one.
    for (int i = 0; i < 80; ++i)
    {
        juce::Thread::sleep(20);
        juce::Timer::callPendingTimersSynchronously();
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
