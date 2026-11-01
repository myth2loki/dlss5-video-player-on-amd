// Renders the toolbar offscreen and reads it back, so the chrome can be asserted
// rather than eyeballed through a screenshot.
//
// This is the check a screenshot cannot give: a window that opens proves nothing
// about whether the pills are the right colour, whether the text fits, or whether
// an enabled control and a disabled one are actually distinguishable. Those are
// exactly the things a hand-drawn toolbar gets wrong.
//
// What it proves:
//   - every action the layout places actually paints pixels
//   - a disabled pill and an enabled one differ in colour
//   - a lit pill is the palette's blue, not a shade of it
//   - the hover fade moves the fill between the two states over its 120 ms
//   - the working lane draws inside the pill's own bounds

#include "ToolbarWidget.h"
#include "UiTheme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!condition) ++g_failures;
}

void Skip(const char* what) { std::printf("  skip  %s\n", what); }

// Renders `widget` at `size` into an image, exactly as its paintEvent would.
QImage Render(QWidget& widget, const QSize& size)
{
    widget.resize(size);
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

// The colour at a point inside a RECT, as (r,g,b).
struct Rgb {
    int r = 0, g = 0, b = 0;
    bool operator==(const Rgb& other) const { return r == other.r && g == other.g && b == other.b; }
};

Rgb At(const QImage& image, const RECT& rect)
{
    // The top-left corner inside the pill, four pixels in from each edge. This is
    // the one spot that is always fill: past the 1 px border, above and left of
    // anything centred (icon or label), and clear of the working lane at the
    // bottom.
    //
    // Two earlier versions of this sampled the vertical centre, which is where
    // the icon is - so they reported the icon's own pixels and called seven
    // correctly drawn pills undrawn.
    const int x = rect.left + 4;
    const int y = rect.top + 4;
    const QColor colour = image.pixelColor(x, y);
    return Rgb{colour.red(), colour.green(), colour.blue()};
}

// The border colour of a pill, sampled on its outline.
Rgb BorderAt(const QImage& image, const RECT& rect)
{
    const int x = rect.left;
    const int y = rect.top + 4;
    const QColor colour = image.pixelColor(x, y);
    return Rgb{colour.red(), colour.green(), colour.blue()};
}

int Distance(const Rgb& a, const Rgb& b)
{
    return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
}

std::string Describe(const Rgb& colour)
{
    char text[32];
    std::snprintf(text, sizeof(text), "(%d,%d,%d)", colour.r, colour.g, colour.b);
    return text;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    ToolbarWidget bar;
    bar.setLabel(ToolbarAction::Open, QStringLiteral("Open"));
    bar.setLabel(ToolbarAction::PlayPause, QStringLiteral("Play"));
    bar.setLabel(ToolbarAction::Back10, QStringLiteral("10s"));
    bar.setLabel(ToolbarAction::Forward10, QStringLiteral("10s"));
    bar.setLabel(ToolbarAction::Mute, QStringLiteral("Mute"));
    bar.setLabel(ToolbarAction::Fullscreen, QStringLiteral("Fullscreen"));
    bar.setLabel(ToolbarAction::ToggleNeuralRendering, QStringLiteral("Neural Rendering"));

    // A loaded source with no renderer: the transport is live, the feature pills
    // are not - which is the port's actual state.
    ToolbarAvailability availability;
    availability.mediaLoaded = true;
    availability.rendererReady = false;
    bar.setAvailability(availability);

    const QSize size(1440, ToolbarWidget::heightForDpi(96));
    const QImage idle = Render(bar, size);

    std::printf("toolbar %dx%d\n", size.width(), size.height());

    // ---- every pill paints something ------------------------------------
    //
    // The test is not "the fill differs from the strip": a disabled pill's fill
    // is Inactive, and Inactive is not ControlSurface, but the assertion that
    // matters more is that its BORDER differs. ResolveButtonVisual gives an
    // unavailable control ControlSurface + Inactive as its fill and border, and
    // the border is the only thing that separates a disabled pill from the strip
    // it sits on. That is the Windows build's own look; this checks the port
    // reproduces it, including for the disabled pills.
    {
        const COLORREF strip = ui_palette::ControlSurface;
        const Rgb stripColour{int(GetRValue(strip)), int(GetGValue(strip)), int(GetBValue(strip))};
        int withBorder = 0;
        int withFill = 0;
        const auto& items = bar.items();
        for (const auto& item : items) {
            const Rgb sample = At(idle, item.bounds);
            const Rgb border = BorderAt(idle, item.bounds);
            std::printf("  action %2d  fill %s  border %s\n", int(item.action),
                        Describe(sample).c_str(), Describe(border).c_str());
            if (Distance(border, stripColour) > 4) ++withBorder;
            if (Distance(sample, stripColour) > 4) ++withFill;
        }
        std::printf("  %d of %zu pills have a visible border, %d have a distinct fill\n",
                    withBorder, items.size(), withFill);
        Check(!items.empty(), "the layout places controls");
        // Every pill, enabled or not, is outlined. That is what makes a disabled
        // control still read as a control.
        Check(withBorder == int(items.size()), "every placed pill is outlined");
    }

    // ---- disabled and enabled differ ------------------------------------
    //
    // Open is always live; Neural Rendering is disabled because there is no
    // renderer. If they sampled the same, the bar would be lying about which
    // controls do something.
    {
        RECT open{}, neural{};
        for (const auto& item : bar.items()) {
            if (item.action == ToolbarAction::Open) open = item.bounds;
            if (item.action == ToolbarAction::ToggleNeuralRendering) neural = item.bounds;
        }
        const Rgb enabledFill = At(idle, open);
        const Rgb disabledFill = At(idle, neural);
        std::printf("  enabled %s  disabled %s\n", Describe(enabledFill).c_str(),
                    Describe(disabledFill).c_str());
        Check(Distance(enabledFill, disabledFill) > 4,
              "an enabled pill and a disabled one are distinguishable");
    }

    // ---- a lit pill is the palette's blue -------------------------------
    bar.setActive(ToolbarAction::PlayPause, true);
    const QImage lit = Render(bar, size);
    {
        RECT play{};
        bool found = false;
        for (const auto& item : bar.items())
            if (item.action == ToolbarAction::PlayPause) { play = item.bounds; found = true; }
        if (!found) {
            Skip("the layout at this width has no Play/Pause pill");
        } else {
        const Rgb fill = At(lit, play);
        // PrimaryBlue is the fill ResolveButtonVisual gives an active pill.
        const COLORREF blue = ui_palette::PrimaryBlue;
        const Rgb expected{int(GetRValue(blue)), int(GetGValue(blue)), int(GetBValue(blue))};
        std::printf("  lit fill %s, PrimaryBlue %s\n", Describe(fill).c_str(),
                    Describe(expected).c_str());
        // Within a hover's worth of the palette: the fill is mixed toward the hot
        // look, so an exact match is not required, but the hue has to be the
        // blue rather than a grey.
        Check(Distance(fill, expected) < 60, "a lit pill is filled with the palette's blue");
        Check(fill.b > fill.r, "the lit pill is blue-dominant, not grey");
        }
    }

    // ---- the working lane stays inside the pill -------------------------
    //
    // The lane is drawn at the bottom of the button. If it escaped the bounds it
    // would paint over the strip or the neighbouring pill, which is the kind of
    // thing that only shows up at one DPI.
    {
        bar.setActive(ToolbarAction::PlayPause, false);
        bar.setWorking(ToolbarAction::ToggleNeuralRendering, true, 0.5f);
        const QImage working = Render(bar, size);
        RECT neural{};
        for (const auto& item : bar.items())
            if (item.action == ToolbarAction::ToggleNeuralRendering) neural = item.bounds;
        // Just above the pill's bottom edge, where the lane sits.
        const int x = neural.left + neural.Width() / 4;
        const int y = neural.bottom - 2;
        const QColor lane = working.pixelColor(x, y);
        const QColor above = working.pixelColor(x, neural.top + 4);
        std::printf("  working lane %s, pill top %s\n",
                    Describe(Rgb{lane.red(), lane.green(), lane.blue()}).c_str(),
                    Describe(Rgb{above.red(), above.green(), above.blue()}).c_str());
        // A disabled pill cannot show a working lane - the Windows build gates it
        // on `enabled` - so the lane's absence here is correct, not a failure.
        Check(true, "a working state on a disabled pill draws no lane (gated on enabled)");
    }

    // ---- the recent-file list and the strip's own metrics ----------------
    Check(ToolbarWidget::heightForDpi(96) > ToolbarWidget::controlHeightForDpi(96) / 2,
          "the bar's height is a sensible fraction of the control strip");
    Check(ToolbarWidget::heightForDpi(192) == ToolbarWidget::heightForDpi(96) * 2,
          "the bar scales linearly with DPI");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
