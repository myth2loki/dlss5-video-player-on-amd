#pragma once

// The video surface, as a Qt widget.
//
// Two ways to get a decoded BGRA frame onto a Qt window, and this uses the
// first:
//
//   1. Paint it. The frame is already a QImage-compatible buffer, so the widget
//      draws it with QPainter and Qt's own raster or Metal-backed backing store
//      does the upload. No second graphics API, no surface sharing.
//   2. A native child window with a CAMetalLayer, painted by MetalVideoPresenter.
//      Faster in principle, but it means managing a native handle inside Qt's
//      layout, and the presenter cannot be composited with Qt widgets over it.
//
// (1) is what the Qt path is for: it keeps one graphics API. The presenter from
// phase 1 stays in the tree and still runs the offscreen test, because it is
// the thing that proves the frame arithmetic; it is simply not the way the Qt
// window draws.
//
// Note the format. The pipe produces BGRA, little-endian, top-down. Qt's
// Format_RGB32 is 0xFFRRGGBB in a uint32, which on a little-endian machine is
// stored B,G,R,A - the same bytes. So the frame is wrapped, not converted, and
// the wrap is valid as long as the QImage does not outlive the frame buffer.

#include <QImage>
#include <QWidget>

#include <cstdint>

class VideoSurface : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget* parent = nullptr);

    // `bgra` must be width*height*4 bytes, top-down. The image is copied, not
    // referenced, because the decoder hands the same buffer back to ffmpeg on
    // the next read - a QImage that pointed into it would show the next frame
    // while pretending to show this one.
    void setFrame(const uint8_t* bgra, int width, int height);

    // Fit (letterbox) or fill (crop), which is the player's A key.
    void setFill(bool fill);
    bool fill() const { return m_fill; }

    // The source's own size, known from the probe before the first frame
    // arrives. Without it the window has no aspect ratio to letterbox against
    // until ffmpeg produces something, and it would show one frame of black
    // at the wrong shape.
    void setSourceWidth(int width, int height);

    // Where the picture actually landed, for mapping a click back to a frame
    // coordinate. Empty when nothing is shown.
    QRect videoRect() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QRect targetRect() const;

    QImage m_frame;
    bool m_fill = false;
    // The probe's answer, used for the aspect ratio until the first frame
    // lands.
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
};
