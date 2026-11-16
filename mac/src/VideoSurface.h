#pragma once

// The video surface, as a Qt widget.
//
// Phase 4 drew one decoded BGRA frame. It now holds TWO - the source and the
// rendered member of the pair SegmentPlayback produces - and composes them
// according to policy/CompareCompose.h. That is what makes Split, Wipe,
// Difference, Side by side and 2x2 possible without a second graphics API:
// QPainter clips each pane and draws a QImage, and the few views that need a
// per-pixel decision go through CompareCompositor's CPU pass once per frame.
//
// Format note: the pipe produces BGRA, little-endian, top-down. Qt's
// Format_RGB32 is 0xFFRRGGBB in a uint32, which on a little-endian machine is
// stored B,G,R,A - the same bytes. Frames are wrapped and COPIED, because the
// decoder reuses its buffer on the next read.

#include <QImage>
#include <QWidget>

#include "CompareCompose.h"
#include "CompareViewPolicy.h"

#include <array>
#include <cstdint>

class QPainter;
class QTimer;

class VideoSurface : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget* parent = nullptr);

    // Plain playback: one frame, no pair. Clearing the pair is deliberate - a
    // stale rendered frame from the previous source must never be composited
    // over the new one.
    void setFrame(const uint8_t* bgra, int width, int height);

    // The two members of one moment. `neural` is scaled to the original's size
    // when a 2x frame generation segment was decoded at a different resolution.
    void setPair(const uint8_t* original, const uint8_t* neural, int width, int height);
    bool hasPair() const { return m_hasPair; }

    // Fit (letterbox) or fill (crop), which is the player's A key.
    void setFill(bool fill);
    bool fill() const { return m_fill; }

    // The source's own size, known from the probe before the first frame
    // arrives. Without it the window has no aspect ratio to letterbox against.
    void setSourceWidth(int width, int height);

    // Where the picture actually landed, for mapping a click back to a frame
    // coordinate. Empty when nothing is shown.
    QRect videoRect() const;

    // ---- the comparison switch ------------------------------------------
    // Drawn over the picture. `available` is false when the playhead is on
    // ground that has not been rendered: the buttons are then dimmed and a
    // press explains how to start a conversion instead of doing nothing.
    void setComparison(bool available, bool showingNeural);
    bool comparisonAvailable() const { return m_comparisonAvailable; }
    bool showingNeural() const { return m_showingNeural; }
    QRect comparisonRect() const;

    // ---- the spatial views -----------------------------------------------
    void setCompareParams(const compare_compose::Params& params);
    const compare_compose::Params& compareParams() const { return m_params; }

    // ---- subtitles --------------------------------------------------------
    // The line active at the playhead, or empty. Set by the window every tick;
    // drawn at the size the picture has on screen, never upscaled from a
    // raster the size of the source.
    void setSubtitle(const QString& text);
    QString subtitle() const { return m_subtitle; }

    // ---- export -----------------------------------------------------------
    // The picture AS SHOWN - panes, divider, tags and subtitles - without the
    // two click buttons, which are chrome rather than part of the comparison.
    // Null when there is no frame yet.
    QImage comparisonImage() const;

    QSize sizeHint() const override;

signals:
    void comparisonRequested(bool neural);
    void comparisonUnavailable();
    // A press or drag on the picture in Split/Wipe. `u` is the divider in [0,1].
    void dividerChangeRequested(float u);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRect targetRect() const;
    static QRect targetRectIn(const QRect& area, int sourceWidth, int sourceHeight, bool fill);

    void comparisonButtonRects(QRect& neural, QRect& original) const;
    void paintComparison(QPainter& painter) const;
    void paintSubtitle(QPainter& painter, const compare_compose::Rect& picture) const;
    void paintPaneTags(QPainter& painter, const compare_compose::Rect& picture,
                       const std::vector<compare_compose::Pane>& panes) const;

    // Shared by paintEvent and comparisonImage(); `picture` is the rectangle
    // the composited image occupies, `area` the whole canvas.
    void paintInto(QPainter& painter, const QRect& area,
                   const compare_compose::Rect& picture, bool chrome) const;

    void invalidateComposite();
    const QImage& layerImage(compare_compose::Layer layer) const;

    QImage m_frame;      // the source, always present while playing
    QImage m_neural;     // the rendered member, when the pair exists
    bool m_hasPair = false;
    bool m_fill = false;
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;

    bool m_comparisonAvailable = false;
    bool m_showingNeural = false;
    int m_hover = 0;             // 0 none, 1 neural, 2 original
    bool m_draggingDivider = false;

    // Press-and-hold A/B. The ported compare_gesture decides when a press has
    // become a peek, a drag or nothing; this only holds the timer and the
    // resulting "show the original for as long as the button is down" flag.
    compare_gesture::State m_gesture;
    QTimer* m_holdTimer = nullptr;
    bool m_peeking = false;

    compare_compose::Params m_params;
    QString m_subtitle;

    // Composite results, one per layer, computed lazily and thrown away when
    // either frame or any parameter changes. A 2118x1046 pass costs a few ms,
    // so this is the difference between a usable Split view and a slideshow.
    mutable std::array<QImage, 6> m_layerImages;
    mutable std::array<bool, 6> m_layerReady{};
};
