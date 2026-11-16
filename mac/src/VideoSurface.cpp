#include "VideoSurface.h"

#include "CompareCompositor.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>

// ui_palette::Window: the same background the Windows build clears to.
static const QColor kWindowBackground(18, 19, 21);

using compare_compose::Layer;
using compare_compose::Mode;
using compare_compose::Rect;

namespace {

constexpr int kButtonHeight = 26;
constexpr int kButtonGap = 6;
constexpr int kHorizontalPadding = 12;
constexpr int kMargin = 12;

QRect toQRect(const Rect& rect)
{
    return QRect(rect.left, rect.top, rect.width(), rect.height());
}

Rect fromQRect(const QRect& rect)
{
    // QRect's right()/bottom() are INCLUSIVE; compare_compose::Rect is a
    // half-open [left,right) x [top,bottom) box. Forgetting the +1 here makes
    // every pane and the divider one pixel narrower than the picture.
    return Rect{rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
}

int buttonWidth(const QString& text)
{
    // Brace-initialised: `QFontMetrics metrics(QFont())` is a function
    // declaration (the most vexing parse) and fails at the use instead.
    const QFontMetrics metrics{QFont()};
    return metrics.horizontalAdvance(text) + kHorizontalPadding * 2;
}

QString layerTag(Layer layer, const compare_compose::Params& params)
{
    switch (layer) {
    case Layer::Original: return QStringLiteral("ORIGINAL");
    case Layer::Neural: return QStringLiteral("DLSS 5");
    case Layer::Mix: return QStringLiteral("MIX %1%").arg(int(std::lround(params.mix * 100)));
    case Layer::Difference:
        return QStringLiteral("DIFFERENCE x%1").arg(params.gain, 0, 'f', 0);
    case Layer::SecondMix:
        return QStringLiteral("DLSS 5 %1%").arg(int(std::lround(params.secondMix * 100)));
    case Layer::Spatial: return {};
    }
    return {};
}

} // namespace

VideoSurface::VideoSurface(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The switch starts dimmed. setComparison() has an early return for an
    // unchanged state, so the first call with (false, false) would leave the
    // tooltip unset and a hovering viewer with no explanation.
    setToolTip(QStringLiteral("Nothing converted here yet — press Ctrl+Shift+L "
                              "to keep converting while you watch"));

    // Topaz / ICAT taught people to hold the picture down to see the original.
    // The ported compare_gesture says when a press has become a hold rather
    // than a drag; a single-shot timer is all the widget has to add.
    m_holdTimer = new QTimer(this);
    m_holdTimer->setSingleShot(true);
    m_holdTimer->setInterval(int(compare_gesture::kHoldMs));
    connect(m_holdTimer, &QTimer::timeout, this, [this]() {
        const auto step = compare_gesture::HoldElapsed(m_gesture);
        if (step.startPeek) {
            m_peeking = true;
            update();
        }
    });
}

void VideoSurface::setFrame(const uint8_t* bgra, int width, int height)
{
    if (!bgra || width <= 0 || height <= 0) return;
    const QImage wrapped(bgra, width, height, width * 4, QImage::Format_RGB32);
    // copy() rather than assignment: Qt shares implicitly, and the decoder
    // overwrites this buffer on its next read.
    m_frame = wrapped.copy();
    m_neural = QImage();
    m_hasPair = false;
    m_sourceWidth = width;
    m_sourceHeight = height;
    invalidateComposite();
    update();
}

void VideoSurface::setPair(const uint8_t* original, const uint8_t* neural, int width, int height)
{
    if (!original || width <= 0 || height <= 0) return;
    setFrame(original, width, height);
    if (!neural) return;

    const QImage rendered(neural, width, height, width * 4, QImage::Format_RGB32);
    // A 2x frame-generation segment can carry a different width/height. The
    // compositor needs two equal-sized images; scaling once here is cheaper
    // than doing it per pane per frame.
    m_neural = rendered.copy();
    if (m_neural.size() != m_frame.size()) {
        m_neural = m_neural.scaled(m_frame.size(), Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    m_hasPair = true;
    invalidateComposite();
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

QRect VideoSurface::targetRectIn(const QRect& area, int sourceWidth, int sourceHeight, bool fill)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || area.isEmpty()) return QRect();
    const double sourceAspect = double(sourceWidth) / double(sourceHeight);
    const double targetAspect = double(area.width()) / double(area.height());

    if (fill) {
        int width = area.width();
        int height = area.height();
        if (sourceAspect > targetAspect) width = int(std::lround(height * sourceAspect));
        else height = int(std::lround(width / sourceAspect));
        return QRect(area.center().x() - width / 2, area.center().y() - height / 2, width, height);
    }

    int width = area.width();
    int height = int(std::lround(width / sourceAspect));
    if (height > area.height()) {
        height = area.height();
        width = int(std::lround(height * sourceAspect));
    }
    return QRect(area.center().x() - width / 2, area.center().y() - height / 2, width, height);
}

QRect VideoSurface::targetRect() const
{
    int width = 0, height = 0;
    if (!m_frame.isNull()) {
        width = m_frame.width();
        height = m_frame.height();
    } else {
        width = m_sourceWidth;
        height = m_sourceHeight;
    }
    return targetRectIn(rect(), width, height, m_fill);
}

void VideoSurface::setComparison(bool available, bool showingNeural)
{
    if (m_comparisonAvailable == available && m_showingNeural == showingNeural) return;
    m_comparisonAvailable = available;
    m_showingNeural = showingNeural;
    setToolTip(available
                   ? QStringLiteral("Switch between DLSS 5 and the original (D). "
                                    "Compare menu: split, wipe, difference, side by side, 2x2")
                   : QStringLiteral("Nothing converted here yet — press Ctrl+Shift+L "
                                    "to keep converting while you watch"));
    update();
}

void VideoSurface::setCompareParams(const compare_compose::Params& params)
{
    if (compare_compose::SameParams(m_params, params)) return;
    m_params = params;
    invalidateComposite();
    update();
}

void VideoSurface::setSubtitle(const QString& text)
{
    if (m_subtitle == text) return;
    m_subtitle = text;
    update();
}

void VideoSurface::invalidateComposite()
{
    m_layerReady.fill(false);
}

const QImage& VideoSurface::layerImage(Layer layer) const
{
    const size_t index = size_t(layer);
    if (!m_layerReady[index]) {
        m_layerImages[index] = compare_compositor::make(m_frame, m_neural, layer, m_params);
        m_layerReady[index] = true;
    }
    return m_layerImages[index];
}

void VideoSurface::comparisonButtonRects(QRect& neural, QRect& original) const
{
    neural = QRect();
    original = QRect();
    const QRect target = targetRect();
    if (target.isEmpty()) return;

    const int neuralWidth = buttonWidth(QStringLiteral("DLSS 5"));
    const int originalWidth = buttonWidth(QStringLiteral("ORIGINAL"));
    const int baseline = target.bottom() - kMargin - kButtonHeight;
    neural = QRect(target.left() + kMargin, baseline, neuralWidth, kButtonHeight);
    original = QRect(neural.right() + kButtonGap, baseline, originalWidth, kButtonHeight);
}

QRect VideoSurface::comparisonRect() const
{
    QRect neural;
    QRect original;
    comparisonButtonRects(neural, original);
    if (neural.isNull() || original.isNull()) return QRect();
    return neural.united(original);
}

// The one paint path. paintEvent and comparisonImage() both end up here, so an
// exported PNG is the same picture the viewer is looking at rather than a
// second implementation that drifts.
void VideoSurface::paintInto(QPainter& painter, const QRect& area,
                             const Rect& picture, bool chrome) const
{
    painter.fillRect(area, kWindowBackground);
    if (m_frame.isNull() || picture.empty()) return;

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const bool usePair = m_hasPair && !m_neural.isNull() &&
                         m_neural.size() == m_frame.size();

    if (!usePair) {
        painter.drawImage(toQRect(picture), m_frame);
    } else if (m_peeking) {
        // Hold-to-peek: the original replaces whatever composite was showing,
        // for exactly as long as the button stays down. No mode change, no
        // re-read of the pair, and no settings write.
        painter.drawImage(toQRect(picture), m_frame);
    } else {
        const std::vector<compare_compose::Pane> panes =
            compare_compose::Compose(picture, m_frame.width(), m_frame.height(), m_params);
        for (const compare_compose::Pane& pane : panes) {
            const QImage* image = nullptr;
            if (pane.layer == Layer::Original) image = &m_frame;
            else if (pane.layer == Layer::Neural) image = &m_neural;
            else image = &layerImage(pane.layer);
            if (!image || image->isNull()) image = &m_frame;

            const QRect source(int(pane.u0 * m_frame.width()), int(pane.v0 * m_frame.height()),
                               int((pane.u1 - pane.u0) * m_frame.width()),
                               int((pane.v1 - pane.v0) * m_frame.height()));
            const QRect destination = toQRect(pane.bounds);
            painter.save();
            painter.setClipRect(destination);
            painter.drawImage(destination, *image, source);
            painter.restore();
        }

        // A dark gutter where panes meet, so four pictures read as four.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(10, 11, 13));
        if (m_params.mode == Mode::SideBySide || m_params.mode == Mode::Quad) {
            const int middleX = picture.left + picture.width() / 2;
            painter.drawRect(QRect(middleX - 1, picture.top, 2, picture.height()));
        }
        if (m_params.mode == Mode::Quad) {
            const int middleY = picture.top + picture.height() / 2;
            painter.drawRect(QRect(picture.left, middleY - 1, picture.width(), 2));
        }
        // Wipe is Split with the divider drawn; Windows' PSPresentScaled does
        // exactly that (a black line with a white hairline inside it).
        if (m_params.mode == Mode::Wipe) {
            const int x = compare_compose::DividerX(picture, m_params.divider);
            painter.drawRect(QRect(x - 2, picture.top, 4, picture.height()));
            painter.setBrush(QColor(235, 238, 242));
            painter.drawRect(QRect(x, picture.top, 1, picture.height()));
        }
        paintPaneTags(painter, picture, panes);
    }

    paintSubtitle(painter, picture);
    if (chrome) paintComparison(painter);
}

void VideoSurface::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    paintInto(painter, rect(), fromQRect(targetRect()), true);
}

QImage VideoSurface::comparisonImage() const
{
    if (m_frame.isNull()) return QImage();
    const bool usePair = m_hasPair && !m_neural.isNull() &&
                         m_neural.size() == m_frame.size();
    QSize size = m_frame.size();
    if (usePair && m_params.mode == Mode::SideBySide) size = QSize(size.width() * 2, size.height());
    if (usePair && m_params.mode == Mode::Quad) size = QSize(size.width() * 2, size.height() * 2);

    QImage out(size, QImage::Format_RGB32);
    out.fill(kWindowBackground);
    QPainter painter(&out);
    // Export's canvas is laid out FOR the panes, so the picture occupies all of
    // it: 2W x H gives two W x H halves, 2W x 2H gives four W x H quarters.
    const Rect whole{0, 0, size.width(), size.height()};
    paintInto(painter, QRect(0, 0, size.width(), size.height()), whole, false);
    painter.end();
    return out;
}

void VideoSurface::paintComparison(QPainter& painter) const
{
    QRect neural;
    QRect original;
    comparisonButtonRects(neural, original);
    if (neural.isNull() || original.isNull()) return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto drawButton = [&](const QRect& box, const QString& text, bool selected, bool hovered) {
        QColor fill;
        QColor textColour;
        if (selected) {
            fill = QColor(64, 156, 255);
            textColour = QColor(255, 255, 255);
        } else {
            fill = QColor(0, 0, 0, hovered ? 170 : 130);
            textColour = QColor(255, 255, 255, m_comparisonAvailable ? 210 : 90);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(box);
        painter.setPen(textColour);
        QFont font = painter.font();
        font.setPointSizeF(11.0);
        font.setBold(selected);
        painter.setFont(font);
        painter.drawText(box, Qt::AlignCenter, text);
    };

    // In a spatial view neither single-frame button is "the" view; the pane
    // tags say what is on screen. Leaving both unselected is honest, and the
    // buttons still answer a press by returning to that single frame.
    const bool spatial = m_hasPair && m_params.mode != Mode::Neural &&
                         m_params.mode != Mode::Original;
    const bool neuralSelected = m_comparisonAvailable && !spatial &&
                                (m_hasPair ? m_params.mode == Mode::Neural : m_showingNeural);
    const bool originalSelected = m_comparisonAvailable && !spatial && !neuralSelected &&
                                  (m_hasPair ? m_params.mode == Mode::Original : !m_showingNeural);

    drawButton(neural, QStringLiteral("DLSS 5"), neuralSelected, m_hover == 1);
    drawButton(original, QStringLiteral("ORIGINAL"), originalSelected, m_hover == 2);
}

void VideoSurface::paintPaneTags(QPainter& painter, const Rect& picture,
                                 const std::vector<compare_compose::Pane>& panes) const
{
    QFont font = painter.font();
    font.setPointSizeF(10.0);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics{font};

    if (m_params.mode == Mode::Split || m_params.mode == Mode::Wipe) {
        const int x = compare_compose::DividerX(picture, m_params.divider);
        const QString left = m_params.swapped ? QStringLiteral("DLSS 5") : QStringLiteral("ORIGINAL");
        const QString right = m_params.swapped ? QStringLiteral("ORIGINAL") : QStringLiteral("DLSS 5");
        const int y = picture.top + kMargin + metrics.height();
        const auto tag = [&](const QString& text, const Rect& side) {
            const int width = metrics.horizontalAdvance(text) + 12;
            const int leftEdge = std::clamp(side.left + kMargin, side.left, side.right - width);
            painter.fillRect(QRect(leftEdge, y - metrics.height(), width, metrics.height() + 4),
                             QColor(0, 0, 0, 150));
            painter.setPen(QColor(240, 243, 248));
            painter.drawText(QRect(leftEdge + 6, y - metrics.height(), width - 6, metrics.height()),
                             Qt::AlignVCenter | Qt::AlignLeft, text);
        };
        tag(left, Rect{picture.left, picture.top, x, picture.bottom});
        tag(right, Rect{x, picture.top, picture.right, picture.bottom});
        return;
    }

    for (const compare_compose::Pane& pane : panes) {
        const QString text = layerTag(pane.layer, m_params);
        if (text.isEmpty()) continue;
        const int width = metrics.horizontalAdvance(text) + 12;
        const int x = pane.bounds.left + 6;
        const int y = pane.bounds.top + 6;
        painter.fillRect(QRect(x, y, width, metrics.height() + 4), QColor(0, 0, 0, 150));
        painter.setPen(QColor(240, 243, 248));
        painter.drawText(QRect(x + 6, y, width - 6, metrics.height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text);
    }
}

void VideoSurface::paintSubtitle(QPainter& painter, const Rect& picture) const
{
    if (m_subtitle.isEmpty() || picture.empty()) return;
    const int height = picture.height();
    QFont font = painter.font();
    font.setPointSizeF(std::clamp(height * 0.042, 13.0, 46.0));
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics{font};
    const int lineHeight = metrics.height() + 2;
    const QStringList lines = m_subtitle.split(QLatin1Char('\n'));
    const int blockHeight = lineHeight * lines.size();
    const int baseline = picture.bottom - kMargin - 14 - blockHeight + metrics.ascent();

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        const int width = metrics.horizontalAdvance(line);
        const int x = picture.left + (picture.width() - width) / 2;
        const int y = baseline + i * lineHeight;
        // A stroked path rather than a solid box: text over a bright frame
        // needs an outline, and a plate would cover half the picture.
        QPainterPath path;
        path.addText(x, y, font, line);
        painter.setPen(QPen(QColor(0, 0, 0, 230), 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::white);
        painter.drawPath(path);
    }
}

void VideoSurface::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    QRect neural;
    QRect original;
    comparisonButtonRects(neural, original);
    const bool onSwitch = neural.contains(event->pos()) || original.contains(event->pos());
    if (onSwitch) {
        if (!m_comparisonAvailable) emit comparisonUnavailable();
        else emit comparisonRequested(neural.contains(event->pos()));
        event->accept();
        return;
    }

    // Windows' compare_gesture: in Split/Wipe a press anywhere on the picture
    // puts the divider under the pointer, and a drag keeps it there.
    if (m_hasPair && (m_params.mode == Mode::Split || m_params.mode == Mode::Wipe)) {
        const Rect picture = fromQRect(targetRect());
        const QRect box = toQRect(picture);
        if (box.contains(event->pos())) {
            m_draggingDivider = true;
            emit dividerChangeRequested(compare_compose::DividerFromX(picture, event->pos().x()));
            event->accept();
            return;
        }
    }

    // Outside Split/Wipe a press is the A/B hold. compare_gesture keeps the
    // three outcomes apart (hold, drag, nothing) so a scroll-like drag does
    // not flash the original.
    if (m_hasPair) {
        const POINT point{LONG(event->pos().x()), LONG(event->pos().y())};
        compare_gesture::Press(m_gesture, point, false, false);
        m_holdTimer->start();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoSurface::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingDivider) {
        emit dividerChangeRequested(
            compare_compose::DividerFromX(fromQRect(targetRect()), event->pos().x()));
        event->accept();
        return;
    }

    if (m_gesture.phase != compare_gesture::Phase::Idle) {
        const POINT point{LONG(event->pos().x()), LONG(event->pos().y())};
        const auto step = compare_gesture::Move(m_gesture, point, compare_gesture::kSlopDip);
        if (step.endPeek && m_peeking) {
            m_peeking = false;
            update();
        }
    }

    QRect neural;
    QRect original;
    comparisonButtonRects(neural, original);
    int hover = neural.contains(event->pos()) ? 1 : (original.contains(event->pos()) ? 2 : 0);
    if (hover == 0 && m_hasPair &&
        (m_params.mode == Mode::Split || m_params.mode == Mode::Wipe)) {
        const Rect picture = fromQRect(targetRect());
        if (compare_compose::NearDivider(picture, m_params.divider,
                                         event->pos().x(), event->pos().y()))
            hover = 3;   // divider: a horizontal resize cursor
    }
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover == 3 ? Qt::SplitHCursor
                             : (hover ? Qt::PointingHandCursor : Qt::ArrowCursor));
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void VideoSurface::mouseReleaseEvent(QMouseEvent* event)
{
    const auto step = compare_gesture::Release(m_gesture);
    m_holdTimer->stop();
    if (step.endPeek && m_peeking) {
        m_peeking = false;
        update();
    }
    if (m_draggingDivider) {
        m_draggingDivider = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoSurface::leaveEvent(QEvent* event)
{
    // A pointer that leaves the picture ends a hold, just as a release does.
    if (m_gesture.phase != compare_gesture::Phase::Idle) {
        m_holdTimer->stop();
        m_peeking = false;
        m_gesture = compare_gesture::State{};
        update();
    }
    if (m_hover != 0) {
        m_hover = 0;
        setCursor(Qt::ArrowCursor);
        update();
    }
    QWidget::leaveEvent(event);
}
