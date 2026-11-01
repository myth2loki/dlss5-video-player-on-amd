#pragma once

// The toolbar, as a Qt widget - and the one place the Qt port spends its
// advantage.
//
// The Windows toolbar is 305 GDI calls in main.cpp painting pills by hand. Here
// the geometry is not reimplemented at all: LayoutToolbar(), HitTestToolbar(),
// LayoutButtonContent() and IsToolbarActionEnabled() come from the ported policy
// layer, unchanged. The colours are not chosen here either -
// ResolveButtonVisual() decides them from a ButtonState, which is the same
// four-state precedence the Windows build uses (unavailable, working, active,
// off), ported verbatim in UiTheme.cpp.
//
// What is new is drawing and events, and the drawing is a line-by-line port of
// PlayerApp::DrawButton: the hover fade through chrome_motion::Fade, the pressed
// one-pixel sink, the feature pill that drops its name before it ellipsises its
// state, the working lane with its sweeping segment, and the focus ring.

#include "ChromeMotionPolicy.h"
#include "UiLayout.h"
#include "UiTheme.h"

#include <QWidget>

#include <array>
#include <vector>

class QTimer;

class ToolbarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ToolbarWidget(QWidget* parent = nullptr);

    // The state the layer needs to lay the bar out and to decide what is
    // enabled. A copy rather than a reference: it is a POD of bools and the
    // widget outlives any particular snapshot of it.
    void setAvailability(const ToolbarAvailability& availability);
    const ToolbarAvailability& availability() const { return m_availability; }

    // Which actions are lit. The player owns this state; the toolbar only
    // renders it.
    void setActive(ToolbarAction action, bool active);
    void setWorking(ToolbarAction action, bool working, float progress = -1.0f);

    // Labels come from the player's own vocabulary rather than being invented
    // here, because the Windows build picks them in the same switch as the icon
    // and the enabled state.
    void setLabel(ToolbarAction action, const QString& label);
    QString label(ToolbarAction action) const;

    // Height the bar wants at `dpi`. The layout function needs a client size, so
    // the player asks for this before deciding its own height.
    static int heightForDpi(int dpi);

    // The bar's own metric for the window's bottom strip, which is what the
    // Windows build calls CONTROL_H_DIP.
    static int controlHeightForDpi(int dpi);

    // The volume slider's rectangle for the current layout, or a null rect when
    // the bar has no room for one. The player draws the slider itself: it is a
    // separate control with its own bubble, and LayoutVolumeSlider() already
    // answers where it goes.
    RECT volumeSliderRect() const;

    // The items the bar actually laid out, so a test can sample exactly where the
    // bar drew rather than recomputing the layout and hoping it matches. The two
    // drift apart the moment the widget's DPI differs from the one a test
    // assumed, and that is a bug in the test rather than in the bar.
    const std::vector<ToolbarItem>& items() const { return m_items; }

    // The level the slider shows, 0..1. The player owns the actual volume; this
    // is only what the bar is asked to draw.
    void setVolume(double level);
    double volume() const { return m_volumeLevel; }

signals:
    void actionTriggered(ToolbarAction action);

    // The volume slider was dragged or scrolled. `value` is 0..1.
    void volumeChanged(double value);
    void volumeHoverChanged(bool hovered);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void relayout();
    ToolbarAction actionAt(const QPoint& point) const;
    ButtonState stateFor(ToolbarAction action) const;
    bool anyWorking() const;

    // The port of DrawButton. One action, one pill, no Qt widgets involved.
    void paintButton(QPainter& painter, const ToolbarItem& item);
    void paintFocusRing(QPainter& painter, const QRectF& box, qreal radius, const QColor& colour);
    void paintWorkingLane(QPainter& painter, const QRectF& box, bool accent, float progress);
    void paintVolumeSlider(QPainter& painter);

    void setHover(ToolbarAction action);
    void startAnimationIfNeeded();

    ToolbarAvailability m_availability;
    std::vector<ToolbarItem> m_items;

    struct ActionState {
        bool active = false;
        bool working = false;
        float progress = -1.0f;
    };
    std::vector<ActionState> m_states;
    std::vector<QString> m_labels;

    // One fade per action, so a cursor crossing a row of pills tints each one on
    // its own schedule rather than the row moving as a block.
    std::array<chrome_motion::Fade, size_t(ToolbarAction::None) + 1> m_hoverFades;
    ToolbarAction m_hover = ToolbarAction::None;
    ToolbarAction m_pressed = ToolbarAction::None;

    // The volume slider's own state: whether the cursor is over it, how far the
    // bubble has faded in, and whether a drag is in progress.
    bool m_volumeHover = false;
    bool m_volumeDragging = false;
    double m_volumeLevel = 1.0;
    chrome_motion::Fade m_volumeBubble;

    QTimer* m_animationTimer = nullptr;
    int m_dpi = 96;
};
