#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

#include "dsp/EmberTypes.h"
#include "dsp/SpectrumFifo.h"
#include "gui/EmberLookAndFeel.h"
#include "dsp/BandFx.h"
#include "plugin/PluginProcessor.h"

/**
    Ember's analyser: the live input and output spectrum, the band regions drawn
    over it, and the crossover editing that happens on top of both.

    WHAT IT IS FOR
    --------------
    This is the one place in the plugin where the user can see what the
    processing is doing and reach in to change where it happens:

      - the dim filled curve is the signal going IN, the bright curve is what
        comes OUT, so the effect of the whole chain is legible at a glance;
      - each active band is a translucent vertical region in its own colour from
        `EmberColours::band`, its intensity driven by how hard that band is being
        driven and how much signal is actually passing through it;
      - the vertical dividers between regions ARE the crossover parameters —
        dragging one writes `pid::crossover(i)` through the parameter object, so
        the host records the automation and the undo manager sees the move;
      - clicking a region selects that band, both in the processor and through
        `onBandSelected`, which the editor uses to re-point the band panel.

    HOW IT TALKS TO THE DSP
    -----------------------
    Only through `EmberAudioProcessor`'s accessors, all of which are atomics or
    wait-free, and only from its own `juce::Timer`. `SpectrumFifo::readLatest`
    returns the most recently published frame, or false while the engine has not
    published anything yet (the editor switches analysis on, but the first frame
    still takes an FFT window to arrive) — until then the analyser draws its
    grid and its bands and simply has no curve, rather than flickering an empty
    one in and out.

    DRAWING
    -------
    Vector only, and proportional throughout: the frequency axis is logarithmic
    between `setFrequencyRange`'s limits and the level axis linear in decibels,
    so every position on screen comes from a mapping function rather than a
    constant. Spectrum bins are combined into ONE-PIXEL SCREEN COLUMNS (peak per
    column, which keeps a narrow peak visible where an average would bury it) and
    each column is smoothed over time with a fast attack and a slow release, so
    the curve is stable to read and identical in character at 800 px wide and at
    3000 px wide.
*/
namespace ember::gui
{
class SpectrumDisplay : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    /** @param processorToUse  kept by reference; the editor owns both and
                               outlives this component. */
    explicit SpectrumDisplay(EmberAudioProcessor& processorToUse);
    ~SpectrumDisplay() override;

    //==========================================================================
    /** Called whenever the user picks a band by clicking its region, and when a
        band-count change forces the selection to move. The band index is
        zero-based. The processor's own selection has already been updated. */
    std::function<void(int)> onBandSelected;

    //==========================================================================
    /** Frame rate for polling and repainting, clamped to 15..60 Hz.
        Default: 50. */
    void setRefreshRateHz(int hz);

    /** Displayed frequency span. Default 20 Hz .. 20 kHz; the low limit is
        clamped to at least 1 Hz and the high limit to above the low one. */
    void setFrequencyRange(float lowHz, float highHz);

    /** Displayed level span. Default -100 .. +6 dB. */
    void setDecibelRange(float lowDb, float highDb);

    /** Re-reads band count, crossovers and the selected band immediately
        instead of waiting for the next timer tick. The editor can call this
        after a preset change so the display never lags the rest of the UI. */
    void refreshFromProcessor();

    //==========================================================================
    // The axis mapping, exposed because it is genuinely useful to a panel that
    // wants to line something up with this one (a band strip underneath it, for
    // instance). Both are in this component's local coordinates.

    /** Screen x for a frequency, clamped into the displayed span. */
    float xForFrequency(float hz) const noexcept;

    /** Frequency at a screen x. Not clamped: an x outside the plot returns a
        frequency outside the displayed span. */
    float frequencyForX(float x) const noexcept;

    /** Screen y for a level in decibels, clamped into the displayed span. */
    float yForDecibels(float decibels) const noexcept;

    /** The rectangle the curves and band regions are drawn in — everything
        except the frequency scale along the bottom. */
    juce::Rectangle<float> getPlotArea() const noexcept { return plotArea; }

    /** What the display is showing.

        The analyser and the EQ curves answer different questions - "what is in
        the signal" and "what am I doing to it" - and drawn together at full
        strength they fight for the same pixels. Both is the default because
        the two together are how you work; either alone is for when one of them
        is in the way. */
    enum class DisplayMode
    {
        spectrum = 0,
        eq,
        both
    };

    void setDisplayMode(DisplayMode);
    DisplayMode getDisplayMode() const noexcept { return displayMode; }

    //==========================================================================
    juce::String getTooltip() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseMove(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    //==========================================================================
    void timerCallback() override;

    /** @returns true if anything that affects the picture moved. */
    bool refreshParameters();
    bool refreshHeat();
    bool refreshSpectrum();

    void rebuildColumns();
    void updateSmoothingCoefficients();
    float currentUiScale() const;

    /** Rebuilds the cached fonts, but only when the UI scale has actually
        moved: constructing a `juce::Font` allocates, and paint runs 50 times a
        second at a scale that changes only on a resize. */
    void updateCachedFonts(float scale);

    /** Repaints the plot only. Everything that moves on the timer or under the
        mouse — curves, band washes, dividers, the drag readout — lives inside
        `plotArea`, so the frequency scale and the well border do not need to be
        redrawn with it. */
    void repaintPlot();

    /** Peak of the bins covering a screen column, interpolating instead where a
        column is narrower than one bin (which it is at the low end of the axis). */
    static float aggregateBins(const std::array<float, static_cast<size_t>(kSpectrumBins)>& bins, float firstBin,
                               float lastBin) noexcept;

    /** An open path along a smoothed curve, one point per screen column. */
    void buildCurvePath(juce::Path& path, const std::vector<float>& curve) const;

    void paintBandRegions(juce::Graphics&, float scale) const;
    void paintGrid(juce::Graphics&, float scale) const;
    void paintSpectra(juce::Graphics&, float scale); // not const: reuses the cached paths
    void paintCrossovers(juce::Graphics&, float scale) const;

    /** Every band's tone curve in its own region, plus the combined response.
        Not const: it reconfigures the per-band drawing filters. */
    void paintEqOverlay(juce::Graphics&, float scale);

    /** The Spectrum / EQ / Both selector, top-right of the plot. A painted hit
        region rather than a child component, like the EQ panel's chips. */
    void paintModeSelector(juce::Graphics&, float scale) const;
    juce::Rectangle<float> modeSelectorBounds(float scale) const;
    int modeSegmentAt(juce::Point<float>, float scale) const;
    void paintDragReadout(juce::Graphics&, float scale) const;

    /** Note name and frequency under the mouse, along the bottom of the plot. */
    void paintMouseFrequency(juce::Graphics&, float scale) const;

    /** Right-click menu: add a band here, distribute evenly, reset. */
    void showDisplayMenu(juce::Point<float> position);

    /** The gear beside the mode selector: resolution, averaging, tilt, freeze
        and peak-hold. */
    void showAnalyserMenu();
    juce::Rectangle<float> gearBounds(float scale) const;
    void paintGear(juce::Graphics&, float scale) const;

    //==========================================================================
    juce::Rectangle<float> regionForBand(int band) const;
    int bandAt(juce::Point<float> position) const;
    int dividerAt(juce::Point<float> position) const;
    float dividerTolerance() const;

    /** Keeps an edge inside 20 Hz .. 20 kHz and at least a third of an octave
        from its neighbours — the same rule the engine enforces, so what the
        user drags is exactly what the crossover ends up being. */
    float clampCrossover(int index, float hz) const;

    /** Where modulation has actually put an edge, given the value the user
        edits. Equal to `baseHz` when nothing is routed to that crossover. */
    float modulatedFrequencyFor(int index, float baseHz) const;

    void setCrossover(int index, float hz);
    void updateHover(juce::Point<float> position);
    void selectBandAt(juce::Point<float> position);
    void setSelectedBandAndNotify(int band);

    //==========================================================================
    EmberAudioProcessor& processor;

    // Cached parameter pointers and IDs: no string lookups per frame beyond the
    // modulation-offset queries, which are hash lookups on a cached key.
    std::array<juce::RangedAudioParameter*, static_cast<size_t>(kMaxCrossovers)> crossoverParams{};
    std::array<juce::String, static_cast<size_t>(kMaxCrossovers)> crossoverIds;
    std::array<juce::RangedAudioParameter*, static_cast<size_t>(kMaxBands)> driveParams{};
    std::array<juce::String, static_cast<size_t>(kMaxBands)> driveIds;
    juce::RangedAudioParameter* numBandsParam{nullptr};

    // Spectrum transport and display state.
    SpectrumFrame frame;
    bool hasFrame{false};
    std::vector<float> inputCurve, outputCurve;
    float attackCoefficient{0.6f};
    float releaseCoefficient{0.06f};

    // Geometry, recomputed in resized().
    juce::Rectangle<float> plotArea;

    DisplayMode displayMode{DisplayMode::both};
    int hoveredModeSegment{-1};
    juce::Point<float> lastMousePosition;
    bool mouseInPlot{false};

    /** One per band, for drawing only - never processes audio. Configured from
        the same parameters the audio path uses, so the drawn curve cannot drift
        from the filters. */
    std::array<ToneStack, static_cast<size_t>(kMaxBands)> toneDrawing;
    std::array<juce::Path, static_cast<size_t>(kMaxBands)> tonePaths;
    juce::Path combinedPath;
    juce::Rectangle<float> axisArea;

    // Paint-path scratch. Held between frames so a 50 Hz repaint reuses the
    // storage it allocated once rather than building a Path and two Fonts every
    // time round: juce::Path::clear() keeps its capacity.
    juce::Path curvePath, fillPath, clipPath;
    juce::Font microFont{EmberFonts::get(EmberFonts::Role::micro)};
    juce::Font valueFont{EmberFonts::get(EmberFonts::Role::value)};
    float cachedFontScale{1.0f};

    // Live state, refreshed on the timer.
    std::array<float, static_cast<size_t>(kMaxCrossovers)> crossoverHz{};
    std::array<float, static_cast<size_t>(kMaxCrossovers)> modulatedHz{};
    std::array<float, static_cast<size_t>(kMaxBands)> bandHeat{};
    int numBands{3};
    int selectedBand{0};

    // Interaction.
    int hoveredBand{-1};
    int hoveredDivider{-1};
    int draggedDivider{-1};
    bool dragFine{false};
    float dragStartX{0.0f};
    float dragStartHz{1000.0f};

    // Axis configuration.
    float minFrequency{20.0f};
    float maxFrequency{20000.0f};
    float logFrequencyRatio{1.0f};
    float minDecibels{-100.0f};
    float maxDecibels{6.0f};

    int refreshRateHz{50};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumDisplay)
};
} // namespace ember::gui
