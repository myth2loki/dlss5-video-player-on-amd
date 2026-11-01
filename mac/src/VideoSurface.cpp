#include "VideoSurface.h"

#include <QPainter>
#include <QPaintEvent>

// ui_palette::Window: the same background the Windows build clears to.
static const QColor kWindowBackground(18, 19, 21);

VideoSurface::VideoSurface(QWidget* parent) : QWidget(parent)
{
    // The frame is drawn at its own size and scaled by the painter, so nothing
    // here needs a paint engine of its own.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoSurface::setFrame(const uint8_t* bgra, int width, int height)
{
    if (!bgra || width <= 0 || height <= 0) return;

    // Wrapped and then copied. Formats: Format_RGB32 is 0xffRRGGBB in a
    // uint32, which little-endian stores as B,G,R,0xff - byte-for-byte what the
    // pipe emits. No channel swap anywhere in this file.
    const QImage wrapped(bgra, width, height, width * 4, QImage::Format_RGB32);
    // copy() rather than assignment: assignment is implicit sharing and would
    // keep pointing at the decoder's buffer, which ffmpeg overwrites.
    m_frame = wrapped.copy();
    update();
}

void VideoSurface::setFill(bool fill)
{
    if (m_fill == fill) return;
    m_fill = fill;
    update();
}

QRect VideoSurface::videoRect() const
{
    return targetRect();
}

void VideoSurface::setSourceWidth(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (m_sourceWidth == width && m_sourceHeight == height) return;
    m_sourceWidth = width;
    m_sourceHeight = height;
    updateGeometry();
    update();
}

QSize VideoSurface::sizeHint() const
{
    if (!m_frame.isNull()) return m_frame.size();
    if (m_sourceWidth > 0 && m_sourceHeight > 0) return QSize(m_sourceWidth, m_sourceHeight);
    return QSize(1280, 720);
}

QRect VideoSurface::targetRect() const
{
    // The source's aspect from the frame when there is one, from the probe
    // before that: the letterbox has to be right on the first paint, not only
    // once ffmpeg has caught up.
    int sourceWidth = 0;
    int sourceHeight = 0;
    if (!m_frame.isNull()) {
        sourceWidth = m_frame.width();
        sourceHeight = m_frame.height();
    } else if (m_sourceWidth > 0 && m_sourceHeight > 0) {
        sourceWidth = m_sourceWidth;
        sourceHeight = m_sourceHeight;
    } else {
        return QRect();
    }

    const QRect area = rect();
    // devicePixelRatio is not applied here: the painter is scaled for it, and
    // these are widget coordinates.
    const double sourceAspect = double(sourceWidth) / double(sourceHeight);
    const double targetAspect = double(area.width()) / double(area.height());

    if (m_fill) {
        // Crop: the picture covers the whole widget and the overflow is cut.
        int width = area.width();
        int height = area.height();
        if (sourceAspect > targetAspect) {
            // Source is wider, so crop the sides: scale to height.
            width = int(std::lround(height * sourceAspect));
        } else {
            height = int(std::lround(width / sourceAspect));
        }
        return QRect(area.center().x() - width / 2, area.center().y() - height / 2, width, height);
    }

    // Fit: letterbox, keeping the whole picture.
    int width = area.width();
    int height = int(std::lround(width / sourceAspect));
    if (height > area.height()) {
        height = area.height();
        width = int(std::lround(height * sourceAspect));
    }
    return QRect(area.center().x() - width / 2, area.center().y() - height / 2, width, height);
}

void VideoSurface::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), kWindowBackground);

    if (m_frame.isNull()) return;

    const QRect target = targetRect();
    // Smooth scaling is what makes a 2118-wide frame read as video rather than
    // as aliasing; the cost is a bilinear pass the GPU-backed raster does
    // anyway.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, m_frame);
}
