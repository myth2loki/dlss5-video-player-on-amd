#include "TimelineWidget.h"

#include "UiTheme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

// The bar's own metrics, taken from main.cpp's TimelineRect and the lane heights
// its paint code uses. They are the only numbers in this file that are not from a
// policy header, because the Windows build hard-codes them beside its own paint
// code rather than in one.
constexpr int kTrackHeightDip = 18;    // TimelineRect: bottom-30 to bottom-12
constexpr int kSideInsetDip = 18;      // TimelineRect: Dip(18) each side
constexpr int kHandleRadiusDip = 6;

QColor Colour(COLORREF colour)
{
    return QColor(int(GetRValue(colour)), int(GetGValue(colour)), int(GetBValue(colour)));
}

} // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kTrackHeightDip + 12);
}

QSize TimelineWidget::sizeHint() const
{
    return QSize(400, kTrackHeightDip + 12);
}

void TimelineWidget::setDuration(double seconds)
{
    m_duration = std::max(0.0, seconds);
    update();
}

void TimelineWidget::setPosition(double seconds)
{
    // A drag owns the handle until it is released: letting the playhead move it
    // would fight the user's own finger.
    if (m_scrubTo >= 0.0) return;
    if (std::abs(m_position - seconds) < 1e-4) return;
    m_position = std::max(0.0, seconds);
    update();
}

void TimelineWidget::setCoverage(const QVector<std::pair<double, double>>& spans)
{
    m_coverage = spans;
    update();
}

void TimelineWidget::setMarks(double inSeconds, double outSeconds)
{
    m_markIn = inSeconds;
    m_markOut = outSeconds;
    update();
}

QRectF TimelineWidget::trackRect() const
{
    const double inset = kSideInsetDip;
    const double trackHeight = std::min<double>(kTrackHeightDip, height() - 4);
    return QRectF(inset, (height() - trackHeight) / 2.0, width() - inset * 2, trackHeight);
}

double TimelineWidget::secondsAt(qreal x) const
{
    if (m_duration <= 0.0) return 0.0;
    const QRectF track = trackRect();
    if (track.width() <= 0.0) return 0.0;
    return std::clamp((x - track.left()) / track.width(), 0.0, 1.0) * m_duration;
}

qreal TimelineWidget::xFor(double seconds) const
{
    const QRectF track = trackRect();
    if (m_duration <= 0.0) return track.left();
    return track.left() + track.width() * std::clamp(seconds / m_duration, 0.0, 1.0);
}

QString TimelineWidget::TimeText(double seconds)
{
    // h:mm:ss or m:ss, the Windows build's own clock format: hours only when
    // there are hours, so a short clip does not carry a leading 0:.
    const double total = std::max(0.0, seconds);
    const int hours = int(total / 3600.0);
    const int minutes = int(std::fmod(total, 3600.0) / 60.0);
    const int wholeSeconds = int(std::fmod(total, 60.0));

    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(wholeSeconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(wholeSeconds, 2, 10, QLatin1Char('0'));
}

void TimelineWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = trackRect();
    const qreal radius = track.height() / 2.0;

    // The groove. A source that reports no length greys it: there is nothing to
    // scale a position against, so it shows no progress and takes no click.
    const bool lengthKnown = m_duration > 0.0;
    QPainterPath groove;
    groove.addRoundedRect(track, radius, radius);
    painter.fillPath(groove, Colour(lengthKnown ? RGB(68, 71, 77) : ui_palette::Inactive));

    if (!lengthKnown) return;

    const double shown = m_scrubTo >= 0.0 ? m_scrubTo : m_position;

    // ---- lane 3 (bottom): render coverage -------------------------------
    //
    // Drawn first so the played progress covers it, which is the stacking the
    // Windows build's three lanes have. Empty in this port - there is no render
    // cache - and drawn only when it is not: a lane that is always there and
    // always empty is a claim the port cannot back.
    if (!m_coverage.isEmpty()) {
        painter.save();
        painter.setClipPath(groove);
        const qreal laneHeight = std::max<qreal>(2, track.height() / 4.0);
        for (const auto& span : m_coverage) {
            const qreal from = xFor(span.first);
            const qreal to = xFor(span.second);
            painter.fillRect(
                QRectF(from, track.bottom() - laneHeight, std::max<qreal>(0, to - from), laneHeight),
                Colour(ui_palette::NeuralCoverage));
        }
        painter.restore();
    }

    // ---- lane 2 (middle): played progress -------------------------------
    painter.save();
    painter.setClipPath(groove);
    QRectF played = track;
    played.setWidth(xFor(shown) - track.left());
    painter.fillRect(played, Colour(ui_palette::PrimaryBlue));
    painter.restore();

    // ---- lane 1 (top): the In/Out selection ------------------------------
    //
    // Violet, the loudest of the three, because it is what a render acts on. The
    // Windows build draws it as a band along the top of the track.
    if (m_markOut > m_markIn && m_markIn >= 0.0) {
        painter.save();
        painter.setClipPath(groove);
        const qreal laneHeight = std::max<qreal>(2, track.height() / 4.0);
        const qreal from = xFor(m_markIn);
        const qreal to = xFor(m_markOut);
        painter.fillRect(QRectF(from, track.top(), std::max<qreal>(0, to - from), laneHeight),
                         Colour(ui_palette::MarkedRange));
        painter.restore();
    }

    // ---- the handle ------------------------------------------------------
    const QPointF centre(xFor(shown), track.center().y());
    const qreal handleRadius =
        std::max<qreal>(3, kHandleRadiusDip * track.height() / kTrackHeightDip);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Colour(ui_palette::PrimaryText));
    painter.drawEllipse(centre, handleRadius, handleRadius);
    painter.setBrush(Colour(ui_palette::PrimaryBlue));
    painter.drawEllipse(centre, handleRadius * 0.5, handleRadius * 0.5);

    // ---- the hover time bubble -------------------------------------------
    //
    // The Windows build shows the timecode under the cursor while it is over the
    // bar, so a viewer can find a moment without seeking to it.
    if (m_hoverX >= 0.0 && m_scrubTo < 0.0) {
        const double at = secondsAt(m_hoverX);
        const QString text = TimeText(at);

        QFont font;
        font.setPixelSize(11);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const qreal textWidth = metrics.horizontalAdvance(text) + 12;
        const qreal textHeight = metrics.height() + 4;

        const qreal left = std::clamp(m_hoverX - textWidth / 2.0, 0.0, width() - textWidth);
        QRectF bubble(left, 0, textWidth, textHeight);
        if (bubble.bottom() > track.top() - 2) bubble.moveBottom(track.top() - 2);

        QPainterPath bubblePath;
        bubblePath.addRoundedRect(bubble, 4, 4);
        painter.fillPath(bubblePath, Colour(ui_palette::ControlSurface));
        painter.setPen(QPen(Colour(ui_palette::Inactive), 1.0));
        painter.drawPath(bubblePath);
        painter.setPen(Colour(ui_palette::PrimaryText));
        painter.drawText(bubble, Qt::AlignCenter, text);
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_duration <= 0.0) return;
    m_scrubTo = secondsAt(event->position().x());
    emit scrubbing(m_scrubTo);
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubTo >= 0.0) {
        m_scrubTo = secondsAt(event->position().x());
        emit scrubbing(m_scrubTo);
        update();
        return;
    }

    const qreal x = event->position().x();
    if (std::abs(x - m_hoverX) < 1.0) return;
    m_hoverX = x;
    update();
}

void TimelineWidget::leaveEvent(QEvent* event)
{
    Q_UNUSED(event);
    if (m_hoverX < 0.0) return;
    m_hoverX = -1.0;
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_scrubTo < 0.0) return;
    const double target = secondsAt(event->position().x());
    m_scrubTo = -1.0;
    m_position = target;
    emit seekRequested(target);
    update();
}
