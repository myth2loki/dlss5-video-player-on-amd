#pragma once

// The timeline, as a Qt widget.
//
// This is the third of the three surfaces and the one the Windows build spends
// the most care on: it carries three lanes in one track so no state hides
// another - the In/Out selection on top (violet), played progress in the middle
// (blue), and cached render coverage along the bottom (teal).
//
// This port has no render cache, so the bottom lane is empty and only drawn when
// it has something in it. The In/Out lane is drawn whenever marks exist; the
// commands that set them are greyed in the menu because there is no export to
// act on them, so in practice this bar shows progress alone - which is what a
// viewer of this build can actually do with it.
//
// What is reused rather than reimplemented: the metrics in main.cpp's
// TimelineRect (18 dip tall, 18 dip inset each side), which are taken from that
// function rather than invented here. What is new is the QPainter drawing, the
// hover timecode, and the mouse handling.

#include <QVector>
#include <QWidget>

#include <utility>

class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setDuration(double seconds);
    void setPosition(double seconds);

    // Coverage of what the renderer has produced, in seconds. Empty here: this
    // port has no render cache, and the lane stays empty rather than showing a
    // bar that claims otherwise.
    void setCoverage(const QVector<std::pair<double, double>>& spans);

    // The range a render would act on. Negative means "not set", which is what
    // the port always has - see the header comment.
    void setMarks(double inSeconds, double outSeconds);

    double position() const { return m_position; }
    double duration() const { return m_duration; }

    QSize sizeHint() const override;

    // h:mm:ss or m:ss, the Windows build's own clock format.
    static QString TimeText(double seconds);

signals:
    // Emitted while the handle is dragged, for a preview, and on release for the
    // seek that actually happens.
    void scrubbing(double seconds);
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF trackRect() const;
    double secondsAt(qreal x) const;
    qreal xFor(double seconds) const;

    double m_duration = 0.0;
    double m_position = 0.0;
    double m_scrubTo = -1.0;   // < 0 when not dragging
    double m_markIn = -1.0;
    double m_markOut = -1.0;
    qreal m_hoverX = -1.0;     // < 0 when the cursor is away
    QVector<std::pair<double, double>> m_coverage;
};
