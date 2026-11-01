#include "ToolbarWidget.h"

#include "SliderPolicy.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// RECT -> QRectF. The one conversion between the ported geometry and Qt's, and
// it is exact: RECT's right/bottom are exclusive, which is what QRectF's width
// and height are, so the width and height carry straight across.
QRectF ToRectF(const RECT& r)
{
    return QRectF(qreal(r.left), qreal(r.top), qreal(r.Width()), qreal(r.Height()));
}

QColor ToColor(COLORREF colour)
{
    return QColor(int(GetRValue(colour)), int(GetGValue(colour)), int(GetBValue(colour)));
}

// GlyphForIcon returns a std::wstring of code points - U+101A4 for the subtitle
// icons, past the BMP. On Windows that string holds a surrogate pair, because
// wchar_t is 16 bits; here wchar_t is 32 bits and holds the code point itself,
// so the conversion is a straight copy rather than a decode.
QString GlyphText(UiIcon icon)
{
    const std::wstring glyph = GlyphForIcon(icon);
    QString text;
    text.reserve(int(glyph.size()));
    for (wchar_t unit : glyph) text.append(QChar(char32_t(unit)));
    return text;
}

// Which glyph an action carries. This is the one mapping that has to be decided
// per action, because the Windows build decides it inside its ButtonContent()
// switch alongside the label and the enabled state - three things this widget
// does not own. The icons themselves are identical.
UiIcon IconFor(ToolbarAction action)
{
    switch (action) {
    case ToolbarAction::Open: return UiIcon::Open;
    case ToolbarAction::OpenYouTube: return UiIcon::YouTube;
    case ToolbarAction::Back10: return UiIcon::Rewind;
    case ToolbarAction::PlayPause: return UiIcon::Play;
    case ToolbarAction::Stop: return UiIcon::Stop;
    case ToolbarAction::Forward10: return UiIcon::FastForward;
    case ToolbarAction::Mute: return UiIcon::Volume;
    case ToolbarAction::ToggleNeuralRendering: return UiIcon::Sparkles;
    case ToolbarAction::ToggleUpscaling: return UiIcon::Upscaling;
    case ToolbarAction::ToggleFrameGeneration: return UiIcon::FrameGeneration;
    case ToolbarAction::TogglePlaybackFrameGeneration: return UiIcon::FrameInterpolation;
    case ToolbarAction::Aspect: return UiIcon::Crop;
    case ToolbarAction::Adjustments: return UiIcon::Adjustments;
    case ToolbarAction::DebugView: return UiIcon::Debug;
    case ToolbarAction::Fullscreen: return UiIcon::Maximize;
    case ToolbarAction::ToggleSubtitles: return UiIcon::SubtitlesAi;
    case ToolbarAction::None: break;
    }
    return UiIcon::Warning;
}

// The four actions whose pill carries a state word rather than a name, so a
// narrowed bar can show "On" instead of ellipsising "Neural Rendering · On".
bool IsFeaturePill(ToolbarAction action)
{
    return action == ToolbarAction::ToggleNeuralRendering ||
           action == ToolbarAction::ToggleUpscaling ||
           action == ToolbarAction::ToggleFrameGeneration ||
           action == ToolbarAction::TogglePlaybackFrameGeneration;
}

// The icon font, registered once. tabler-icons.ttf is the same file the Windows
// build embeds as a resource; here it is loaded from disk.
//
// Three places are tried, in order, so the binary works where it is built and
// where it is copied:
//
//   1. beside the executable (a bundle would put it in Resources/, this covers
//      an installed layout),
//   2. the build-time absolute path, which is the development case,
//   3. the two paths relative to the executable that the source tree implies.
//
// A build with none of them still runs: the toolbar draws with empty icon boxes
// rather than failing to start, which is the same degradation the Windows build
// has when its font resource is missing.
QFont IconFont(int pixelSize)
{
    static const QString family = [] {
        QStringList candidates;
        const QString dir = QCoreApplication::applicationDirPath();
        candidates << dir + QStringLiteral("/assets/tabler/tabler-icons.ttf")
                   << dir + QStringLiteral("/../assets/tabler/tabler-icons.ttf");
#ifdef DISPLAYED_ICON_FONT_PATH
        candidates << QString::fromUtf8(DISPLAYED_ICON_FONT_PATH);
#endif
        candidates << QStringLiteral("assets/tabler/tabler-icons.ttf");

        for (const QString& path : candidates) {
            if (!QFileInfo::exists(path)) continue;
            const int id = QFontDatabase::addApplicationFont(path);
            if (id < 0) continue;
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) return families.first();
        }
        return QString();
    }();

    QFont font(family.isEmpty() ? QStringLiteral("Helvetica") : family);
    font.setPixelSize(pixelSize);
    return font;
}

// The label font. The Windows build creates it at -MulDiv(15, dpi, 96), which is
// a 15 px cell at 96 DPI; QFont::setPixelSize is the same request.
QFont LabelFont(int pixelSize)
{
    QFont font;
    font.setPixelSize(pixelSize);
    return font;
}

int Dip(int value, int dpi)
{
    return MulDiv(value, dpi, int(USER_DEFAULT_SCREEN_DPI));
}

} // namespace

ToolbarWidget::ToolbarWidget(QWidget* parent) : QWidget(parent)
{
    // Every action gets a slot, plus None. Index by int so a new enum value costs
    // nothing here.
    m_states.resize(size_t(ToolbarAction::None) + 1);
    m_labels.resize(size_t(ToolbarAction::None) + 1);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_animationTimer = new QTimer(this);
    m_animationTimer->setTimerType(Qt::PreciseTimer);
    m_animationTimer->setInterval(int(chrome_motion::kFrameMs));
    connect(m_animationTimer, &QTimer::timeout, this, [this] {
        // Repaint only while something is moving, which is the same rule the
        // Windows build applies with its own timer: an idle chrome costs
        // nothing, and playback never pays for a hover.
        bool animating = false;
        const auto now = chrome_motion::Clock::now();
        for (auto& fade : m_hoverFades) {
            if (fade.Animating(now)) { animating = true; break; }
        }
        if (m_volumeBubble.Animating(now)) animating = true;
        if (anyWorking()) animating = true;
        update();
        if (!animating) m_animationTimer->stop();
    });
}

bool ToolbarWidget::anyWorking() const
{
    for (const auto& state : m_states)
        if (state.working) return true;
    return false;
}

int ToolbarWidget::heightForDpi(int dpi)
{
    // The Windows build reserves the outer gutter above and below the hit row;
    // LayoutToolbar places the pills inside it.
    const int gutter = Dip(kToolbarOuterGutterDip, dpi);
    const int minHit = Dip(kToolbarMinHitHeightDip, dpi);
    return gutter + minHit + gutter;
}

int ToolbarWidget::controlHeightForDpi(int dpi)
{
    // CONTROL_H_DIP in main.cpp is 96: the strip the toolbar, the timeline and
    // the volume slider all share.
    return Dip(96, dpi);
}

void ToolbarWidget::setVolume(double level)
{
    const double clamped = std::clamp(level, 0.0, 1.0);
    if (std::abs(clamped - m_volumeLevel) < 1e-6) return;
    m_volumeLevel = clamped;
    m_volumeBubble.Set(true, chrome_motion::Clock::now(), true);
    startAnimationIfNeeded();
    update();
}

void ToolbarWidget::setAvailability(const ToolbarAvailability& availability)
{
    m_availability = availability;
    relayout();
    update();
}

void ToolbarWidget::setActive(ToolbarAction action, bool active)
{
    const auto index = size_t(action);
    if (index >= m_states.size()) return;
    if (m_states[index].active == active) return;
    m_states[index].active = active;
    update();
}

void ToolbarWidget::setWorking(ToolbarAction action, bool working, float progress)
{
    const auto index = size_t(action);
    if (index >= m_states.size()) return;
    if (m_states[index].working == working && m_states[index].progress == progress) return;
    m_states[index].working = working;
    m_states[index].progress = progress;
    // A working pill's lane sweeps continuously, so the animation timer has to
    // run while it does.
    if (working) startAnimationIfNeeded();
    update();
}

void ToolbarWidget::setLabel(ToolbarAction action, const QString& label)
{
    const auto index = size_t(action);
    if (index >= m_labels.size()) return;
    if (m_labels[index] == label) return;
    m_labels[index] = label;
    update();
}

QString ToolbarWidget::label(ToolbarAction action) const
{
    const auto index = size_t(action);
    return index < m_labels.size() ? m_labels[index] : QString();
}

void ToolbarWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_dpi = logicalDpiX();
    relayout();
}

void ToolbarWidget::relayout()
{
    // The portable layout function does the work. It takes the client size and a
    // DPI and returns one entry per visible control, already positioned -
    // including the rule that a narrow window switches pills to icons, which is
    // why `compact` comes back per item rather than being decided here.
    m_items = LayoutToolbar(width(), height(), UINT(m_dpi));
}

RECT ToolbarWidget::volumeSliderRect() const
{
    // LayoutVolumeSlider takes the client size and the toolbar items, and answers
    // where the slider goes - or nothing when the bar has no room, which is what
    // the Windows build checks too.
    const auto rect = LayoutVolumeSlider(width(), height(), UINT(m_dpi), m_items);
    return rect ? *rect : RECT{};
}

ButtonState ToolbarWidget::stateFor(ToolbarAction action) const
{
    ButtonState state;
    // Availability is the portable layer's decision, not this widget's: which
    // actions are legal right now is a property of the player's state, and
    // IsToolbarActionEnabled() is where that rule already lives.
    state.enabled = IsToolbarActionEnabled(action, m_availability);

    const auto index = size_t(action);
    if (index < m_states.size()) {
        // The subtitle switch is lit while it is still working - the viewer asked
        // for subtitles and the answer is that they are coming - and it says so
        // in the same blue as every other lit control. Ported from DrawButton's
        // `accent` rule.
        state.active = m_states[index].active;
        state.working = m_states[index].working && !state.active;
    }
    state.hover = (m_hover == action);
    state.pressed = (m_pressed == action) && state.hover;
    return state;
}

ToolbarAction ToolbarWidget::actionAt(const QPoint& point) const
{
    // HitTestToolbar takes the items and a POINT; it is the same call the Windows
    // build makes on WM_LBUTTONDOWN, so a click lands on the same control in
    // both.
    const POINT p{LONG(point.x()), LONG(point.y())};
    return HitTestToolbar(m_items, p);
}

void ToolbarWidget::startAnimationIfNeeded()
{
    if (!m_animationTimer->isActive()) m_animationTimer->start();
}

void ToolbarWidget::setHover(ToolbarAction action)
{
    if (m_hover == action) return;
    const auto now = chrome_motion::Clock::now();

    // The fade that is leaving starts from where it currently is, so a change of
    // mind halfway does not jump - chrome_motion::Fade's own rule.
    if (m_hover != ToolbarAction::None) {
        const auto index = size_t(m_hover);
        if (index < m_hoverFades.size()) m_hoverFades[index].Set(false, now, true);
    }
    m_hover = action;
    if (action != ToolbarAction::None) {
        const auto index = size_t(action);
        if (index < m_hoverFades.size()) m_hoverFades[index].Set(true, now, true);
    }
    startAnimationIfNeeded();
    update();
}

void ToolbarWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // The strip's own background and its hairline top border, which the Windows
    // build paints before any control: ControlSurface, then a 1 px line at
    // RGB(54,56,61).
    painter.fillRect(rect(), ToColor(ui_palette::ControlSurface));
    painter.setPen(ToColor(RGB(54, 56, 61)));
    painter.drawLine(0, 0, width(), 0);

    for (const auto& item : m_items) paintButton(painter, item);
    paintVolumeSlider(painter);
}

void ToolbarWidget::paintButton(QPainter& painter, const ToolbarItem& item)
{
    const ToolbarAction action = item.action;
    const auto index = size_t(action);
    const ActionState state = index < m_states.size() ? m_states[index] : ActionState{};
    const bool enabled = IsToolbarActionEnabled(action, m_availability);

    // The subtitle switch is lit while it is still working; every other pill is
    // lit only when active. Ported from DrawButton's `accent`.
    const bool accent = state.active || (state.working && action == ToolbarAction::ToggleSubtitles);
    const bool pressed = (m_pressed == action) && (m_hover == action);

    // The resting and hovered looks mixed by how far the hover fade has got, so
    // the tint eases rather than snapping; at 0 or 1 it is either look.
    const ButtonVisual rest = ResolveButtonVisual(
        ButtonState{enabled, accent, state.working && !accent, false, pressed, false});
    const ButtonVisual hot = ResolveButtonVisual(
        ButtonState{enabled, accent, state.working && !accent, true, pressed, false});

    const auto now = chrome_motion::Clock::now();
    const double hoverLevel = index < m_hoverFades.size()
                                  ? m_hoverFades[index].Level(now)
                                  : (m_hover == action ? 1.0 : 0.0);

    ButtonVisual visual = hot;
    visual.fill = chrome_motion::Mix(rest.fill, hot.fill, hoverLevel);
    visual.border = chrome_motion::Mix(rest.border, hot.border, hoverLevel);
    visual.text = chrome_motion::Mix(rest.text, hot.text, hoverLevel);

    const QRectF box = ToRectF(item.bounds);
    const qreal radius = qreal(std::max(1, Dip(kToolbarCornerRadiusDip, m_dpi)));

    // The pill: filled and stroked with the same 1 px border, which is what
    // RoundRect + CreatePen(PS_SOLID,1,...) produces.
    QPainterPath pill;
    pill.addRoundedRect(box, radius, radius);
    painter.fillPath(pill, ToColor(visual.fill));
    painter.setPen(QPen(ToColor(visual.border), 1.0));
    painter.drawPath(pill);

    // ---- content: icon, then label, laid out by the portable layer ---------
    const QString glyph = GlyphText(IconFor(action));
    const bool showIcon = !glyph.isEmpty();
    const QString fullLabel = label(action);
    const bool featureLabel = IsFeaturePill(action);

    const int iconPixel = std::max(8, int(std::lround(box.height() * 0.47)));
    const int labelPixel = std::max(8, Dip(15, m_dpi));

    // Measure exactly as GetTextExtentPoint32W does, because the decision to drop
    // the label is made on these numbers.
    painter.setFont(IconFont(iconPixel));
    const QFontMetricsF iconMetrics(painter.font());
    const QSizeF iconSize = showIcon ? iconMetrics.size(Qt::TextSingleLine, glyph) : QSizeF();

    painter.setFont(LabelFont(labelPixel));
    const QFontMetricsF labelMetrics(painter.font());
    QSizeF textSize = labelMetrics.size(Qt::TextSingleLine, fullLabel);

    const int inset = Dip(kButtonHorizontalInsetDip, m_dpi);
    const int gap = Dip(kButtonIconLabelGapDip, m_dpi);
    int neededWidth = int(std::ceil(iconSize.width() + textSize.width())) + 2 * inset + gap;

    // A feature pill the layout has narrowed keeps its state and drops its name,
    // rather than ellipsising the state - the one part of the label that changes
    // - off the end. The icon and the tooltip carry the name.
    QString shown = fullLabel;
    if (featureLabel && neededWidth > box.width()) {
        shown = QString::fromStdWString(std::wstring(FeaturePillStateLabel(fullLabel.toStdWString())));
        textSize = labelMetrics.size(Qt::TextSingleLine, shown);
        neededWidth = int(std::ceil(iconSize.width() + textSize.width())) + 2 * inset + gap;
    }

    const bool showText = !showIcon || featureLabel || neededWidth <= box.width();
    const bool stacked = item.compact && showIcon && showText;

    // LayoutButtonContent is the same function the Windows build calls, so the
    // icon and label land on the same pixels.
    const ButtonContentLayout content = LayoutButtonContent(
        item.bounds,
        showIcon ? SIZE{LONG(std::ceil(iconSize.width())), LONG(std::ceil(iconSize.height()))}
                 : SIZE{},
        showText ? SIZE{LONG(std::ceil(textSize.width())), LONG(std::ceil(textSize.height()))}
                 : SIZE{},
        stacked, UINT(m_dpi));

    // Pressed sinks the glyph and label by one pixel into the darker fill, the
    // inset a physical key gives under a finger. No timer: a press is as long as
    // the button is held.
    RECT iconRect = content.icon;
    RECT textRect = content.text;
    if (pressed && enabled) {
        const int sink = std::max(1, Dip(1, m_dpi));
        OffsetRect(&iconRect, 0, sink);
        OffsetRect(&textRect, 0, sink);
    }

    painter.setPen(ToColor(visual.text));

    if (showIcon) {
        painter.setFont(IconFont(iconPixel));
        painter.drawText(ToRectF(iconRect), Qt::AlignCenter, glyph);
    }

    if (showText && !shown.isEmpty()) {
        painter.setFont(LabelFont(labelPixel));
        // DT_END_ELLIPSIS, which is what the Windows build asks for.
        const QRectF labelBox = ToRectF(textRect);
        const QString elided = labelMetrics.elidedText(shown, Qt::ElideRight, int(labelBox.width()));
        painter.drawText(labelBox, Qt::AlignCenter, elided);
    }

    // The focus ring is a keyboard cue only: the Windows build gates it on
    // m_keyboardCues && GetFocus() == hwnd && focused action == this one, which
    // is what hasFocus() plus the hover/pressed pairing stands in for here.
    if (hasFocus() && m_pressed == action && action != ToolbarAction::None) {
        paintFocusRing(painter, box, radius, ToColor(visual.text));
    }

    if (state.working && enabled && box.width() > 0) {
        paintWorkingLane(painter, box, accent, state.progress);
    }
}

void ToolbarWidget::paintFocusRing(QPainter& painter, const QRectF& box, qreal radius,
                                   const QColor& colour)
{
    // DrawRoundedFocusRing insets by 2 dip and strokes with the text colour, so
    // the ring reads on any fill.
    const qreal inset = qreal(Dip(2, m_dpi));
    QPainterPath ring;
    ring.addRoundedRect(box.adjusted(inset, inset, -inset, -inset),
                        std::max(qreal(1), radius - inset), std::max(qreal(1), radius - inset));
    QColor ringColour = colour;
    ringColour.setAlphaF(0.9);
    painter.setPen(QPen(ringColour, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(ring);
}

void ToolbarWidget::paintWorkingLane(QPainter& painter, const QRectF& box, bool accent,
                                     float progress)
{
    // The wait for the first line is the one thing this button owes an answer
    // for, and it owes it where the eye already is: a lane along the bottom. A
    // pulse while an engine loads its model - nothing knows how long that takes,
    // and a bar claiming a number would be lying - and a fill once there is a
    // total to be a fraction of.
    const qreal laneHeight = std::max<qreal>(2, Dip(2, m_dpi));
    const qreal inset = std::max<qreal>(2, Dip(3, m_dpi));
    const qreal bottomInset = qreal(Dip(1, m_dpi));

    RECT lane{LONG(std::lround(box.left() + inset)),
              LONG(std::lround(box.bottom() - laneHeight - bottomInset)),
              LONG(std::lround(box.right() - inset)), LONG(std::lround(box.bottom() - bottomInset))};
    if (lane.right <= lane.left) return;

    // The track: the pill's fill mixed 40% toward the window colour, so the lane
    // is a shade of the button rather than a new shape. The button's own fill is
    // not passed in, so the mix is taken from the palette entry that stands for
    // it in every state - ControlSurface is what an unlit pill already is.
    const COLORREF track = chrome_motion::Mix(ui_palette::ControlSurface, ui_palette::Window, 0.4);
    painter.fillRect(ToRectF(lane), ToColor(track));

    RECT fill = lane;
    const int span = fill.right - fill.left;
    if (progress >= 0.0f) {
        // A definite progress is drawn as a fraction of the lane.
        fill.right = fill.left + int(span * std::clamp(double(progress), 0.0, 1.0));
    } else {
        // Unknown amount: a lit segment sweeping the lane, which says "working"
        // without pretending to know how much is left.
        const int segment = std::max(6, Dip(10, m_dpi));
        const int travel = span + segment;
        const double ticks = std::chrono::duration<double>(
                                 chrome_motion::Clock::now().time_since_epoch())
                                 .count() *
                             1000.0;
        const int head = int(std::fmod(ticks / 26.0, double(travel)));
        fill.left = std::min(fill.right - segment, fill.left + std::max(0, head - segment / 2));
        fill.right = fill.left + segment;
    }
    if (fill.right <= fill.left) return;

    // The lane is ink on the button's own colour: on the blue of a lit control a
    // teal bar would be a third colour again, only smaller.
    const COLORREF ink = accent ? RGB(136, 197, 250) : ui_palette::NeuralCoverage;
    painter.fillRect(ToRectF(fill), ToColor(ink));
}

void ToolbarWidget::paintVolumeSlider(QPainter& painter)
{
    const RECT rect = volumeSliderRect();
    if (rect.Empty()) return;

    const auto now = chrome_motion::Clock::now();
    const auto geometry =
        slider::Layout(rect, m_volumeLevel, 0.0, m_volumeHover ? 1.0 : 0.0, UINT(m_dpi));

    // The rail, at rest.
    const QRectF track = ToRectF(geometry.track);
    const qreal railRadius = track.height() / 2.0;
    QPainterPath rail;
    rail.addRoundedRect(track, railRadius, railRadius);
    painter.fillPath(rail, ToColor(ui_palette::Inactive));

    // The filled part, up to the value. SliderPolicy already answers it as
    // geometry.fill, with the mixed origin the player's volume slider uses.
    const QRectF fill = ToRectF(geometry.fill);
    if (fill.width() > 0.0) {
        QPainterPath filledPath;
        filledPath.addRoundedRect(fill, railRadius, railRadius);
        painter.fillPath(filledPath, ToColor(ui_palette::PrimaryText));
    }

    // The knob, which grows on hover - that is what the `hot` parameter to
    // slider::Layout is for. It arrives as a centre point and a radius, not a
    // rect.
    painter.setPen(Qt::NoPen);
    painter.setBrush(ToColor(ui_palette::PrimaryText));
    const qreal knobRadius = qreal(std::max(1, geometry.knobRadius));
    painter.drawEllipse(QPointF(qreal(geometry.knob.x), qreal(geometry.knob.y)), knobRadius,
                        knobRadius);

    // The percentage bubble, which fades in with the fade the Windows build keeps
    // in m_volumeBubble.
    const double bubble = m_volumeBubble.Level(now);
    if (bubble > 0.0 && !geometry.bubble.Empty()) {
        const QRectF bubbleBox = ToRectF(geometry.bubble);
        QColor bubbleFill = ToColor(ui_palette::ControlSurface);
        bubbleFill.setAlphaF(bubble);
        QColor bubbleText = ToColor(ui_palette::PrimaryText);
        bubbleText.setAlphaF(bubble);

        QPainterPath bubblePath;
        bubblePath.addRoundedRect(bubbleBox, 4, 4);
        painter.fillPath(bubblePath, bubbleFill);
        QColor edge = ToColor(ui_palette::Inactive);
        edge.setAlphaF(bubble);
        painter.setPen(QPen(edge, 1.0));
        painter.drawPath(bubblePath);

        painter.setPen(bubbleText);
        painter.setFont(LabelFont(std::max(8, Dip(12, m_dpi))));
        painter.drawText(bubbleBox, Qt::AlignCenter,
                         QStringLiteral("%1%").arg(int(std::lround(m_volumeLevel * 100.0))));
    }
}

void ToolbarWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    // The volume slider takes precedence where it overlaps the bar, which is what
    // the Windows build does before hit-testing the toolbar.
    const RECT sliderRect = volumeSliderRect();
    if (!sliderRect.Empty() &&
        PtInRect(sliderRect, POINT{LONG(event->position().x()), LONG(event->position().y())})) {
        m_volumeDragging = true;
        const double fraction = std::clamp(
            (event->position().x() - sliderRect.left) / double(std::max(1, int(sliderRect.Width()))),
            0.0, 1.0);
        emit volumeChanged(fraction);
        return;
    }

    const ToolbarAction action = actionAt(event->position().toPoint());
    if (action == ToolbarAction::None) return;
    if (!IsToolbarActionEnabled(action, m_availability)) return;
    m_pressed = action;
    update();
}

void ToolbarWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_volumeDragging) {
        m_volumeDragging = false;
        return;
    }

    const ToolbarAction action = actionAt(event->position().toPoint());
    const bool same = (action == m_pressed) && action != ToolbarAction::None;
    m_pressed = ToolbarAction::None;
    update();
    // A press that ends on a different control is not a press of either one, which
    // is what every other toolbar does.
    if (same && IsToolbarActionEnabled(action, m_availability)) emit actionTriggered(action);
}

void ToolbarWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();

    if (m_volumeDragging) {
        const RECT sliderRect = volumeSliderRect();
        if (!sliderRect.Empty()) {
            const double fraction = std::clamp(
                (point.x() - sliderRect.left) / double(std::max(1, int(sliderRect.Width()))), 0.0, 1.0);
            emit volumeChanged(fraction);
        }
        return;
    }

    // The slider's hover is its own state: it grows its knob and shows the bubble,
    // which the toolbar's own hover does not do.
    const RECT sliderRect = volumeSliderRect();
    const bool overSlider =
        !sliderRect.Empty() && PtInRect(sliderRect, POINT{LONG(point.x()), LONG(point.y())});
    if (overSlider != m_volumeHover) {
        m_volumeHover = overSlider;
        m_volumeBubble.Set(overSlider, chrome_motion::Clock::now(), true);
        emit volumeHoverChanged(overSlider);
        startAnimationIfNeeded();
        update();
    }
    if (overSlider) {
        setHover(ToolbarAction::None);
        return;
    }

    const ToolbarAction action = actionAt(point);
    setHover(action);
    if (action != ToolbarAction::None) {
        const QString text = label(action);
        if (!text.isEmpty()) QToolTip::showText(event->globalPosition().toPoint(), text, this);
    } else {
        QToolTip::hideText();
    }
}

void ToolbarWidget::wheelEvent(QWheelEvent* event)
{
    // A wheel over the bar is not a scroll: the toolbar has nothing to scroll. It
    // is a volume nudge, which is what every player does with it.
    const double step = event->angleDelta().y() > 0 ? 0.05 : -0.05;
    emit volumeChanged(std::clamp(m_volumeLevel + step, 0.0, 1.0));
    event->accept();
}

void ToolbarWidget::leaveEvent(QEvent* event)
{
    Q_UNUSED(event);
    setHover(ToolbarAction::None);
    if (m_volumeHover) {
        m_volumeHover = false;
        m_volumeBubble.Set(false, chrome_motion::Clock::now(), true);
        emit volumeHoverChanged(false);
        startAnimationIfNeeded();
    }
    if (m_pressed == ToolbarAction::None) update();
}

void ToolbarWidget::keyPressEvent(QKeyEvent* event)
{
    // The bar itself takes no keys: the player's shortcuts own the keyboard, and a
    // toolbar that swallowed Tab would break focus traversal.
    event->ignore();
}
