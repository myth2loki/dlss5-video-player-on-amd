#include "PlayerWindow.h"

#include "TimelineWidget.h"
#include "ToolbarWidget.h"
#include "UiLayout.h"
#include "UiTheme.h"
#include "VideoSurface.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

// hh:mm:ss.mmm, mm:ss, or a bare number of seconds. The Windows build parses its
// timecode box the same three ways, and a viewer who types "90" means ninety
// seconds rather than an error.
double ParseTimecode(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return -1.0;

    const QStringList parts = trimmed.split(QLatin1Char(':'));
    if (parts.size() > 3) return -1.0;

    double seconds = 0.0;
    for (const QString& part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (!ok || value < 0.0) return -1.0;
        seconds = seconds * 60.0 + value;
    }
    return seconds;
}

} // namespace

PlayerWindow::PlayerWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("DLSS 5 Video Player"));
    setMinimumSize(640, 400);
    resize(1280, 720);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    // No margins: the video surface is meant to reach the edges, and the
    // Windows build has the same rule.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_surface = new VideoSurface(central);
    m_timeline = new TimelineWidget(central);
    m_toolbar = new ToolbarWidget(central);
    layout->addWidget(m_surface, 1);
    layout->addWidget(m_timeline, 0);
    layout->addWidget(m_toolbar, 0);

    setCentralWidget(central);

    connect(m_toolbar, &ToolbarWidget::actionTriggered, this, &PlayerWindow::onToolbarAction);
    connect(m_toolbar, &ToolbarWidget::volumeChanged, this, [this](double value) {
        if (!m_audio) return;
        m_audio->SetVolume(float(value));
        // The toolbar shows the slider; the menu shows the mute checkmark, and
        // the two have to agree about what the volume is.
        m_toolbar->setVolume(value);
        syncMenuState();
    });

    // The menu bar. A QMenuBar with no parent window is the system menu bar on
    // macOS, which is where a Mac user looks for File and Playback - the same
    // structure the Windows build puts inside its window frame.
    m_menu = std::make_unique<AppMenu>(this);
    setMenuBar(m_menu->bar());
    connect(m_menu.get(), &AppMenu::commandTriggered, this, &PlayerWindow::onMenuCommand);
    connect(m_menu.get(), &AppMenu::recentFileChosen, this, &PlayerWindow::onRecentFile);

    connect(m_timeline, &TimelineWidget::seekRequested, this, [this](double seconds) {
        // The timeline's own seek, which is the same path as the keyboard's:
        // the clock and both engines move together or the picture and the sound
        // disagree about where they are.
        m_clock.Seek(seconds);
        if (m_source) m_source->Seek(seconds);
        if (m_audio) m_audio->Seek(seconds);
        m_hasFrame = false;
    });

    // The clock is driven by the display, not by the video. 16 ms is the
    // presentation timer; the audio position is what decides which frame it is.
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &PlayerWindow::onTick);
    m_timer.start();

    // The recent list lives in QSettings, so it survives a restart the way the
    // Windows build's ini does.
    {
        QSettings settings;
        m_menu->setRecentFiles(settings.value(QStringLiteral("recentVideos")).toStringList());
    }

    updateAvailability();
    updateLabels();
    syncMenuState();
}

PlayerWindow::~PlayerWindow()
{
    m_timer.stop();
    if (m_source) m_source->Close();
    if (m_audio) m_audio->Stop();
}

bool PlayerWindow::open(const QString& path)
{
    const std::string utf8 = path.toStdString();

    MediaInfo info;
    if (!FfmpegPipeSource::Probe(utf8, info)) {
        statusBar()->showMessage(tr("Cannot read this file."));
        return false;
    }
    m_info = info;

    m_source = std::make_unique<FfmpegPipeSource>();
    if (!m_source->Open(utf8, info)) return false;

    m_clock.SetFrameRate(info.fps);
    m_clock.SetDuration(info.duration);
    if (m_timeline) {
        m_timeline->setDuration(info.duration);
        m_timeline->setPosition(0.0);
    }

    // Audio first: the clock it provides is what the picture follows, and
    // starting the picture first would show it running on the fallback timer
    // for the length of the audio's start-up.
    m_audio = std::make_unique<AudioPlayer>();
    m_audioDriven = m_audio->Start(utf8, 0.0, AudioStartState::Playing);
    m_clock.SetAudioSource(m_audioDriven ? m_audio.get() : nullptr);

    m_hasFrame = false;
    m_clock.Play();

    m_surface->setSourceWidth(int(info.width), int(info.height));

    statusBar()->showMessage(
        m_audioDriven
            ? tr("%1 × %2 · %3 fps · audio drives the clock")
                  .arg(info.width).arg(info.height).arg(info.fps, 0, 'f', 2)
            : tr("%1 × %2 · %3 fps · silent, timer drives the clock")
                  .arg(info.width).arg(info.height).arg(info.fps, 0, 'f', 2));

    updateAvailability();
    updateLabels();
    rememberRecent(path);
    syncMenuState();
    return true;
}

void PlayerWindow::onTick()
{
    pumpVideo();
}

void PlayerWindow::pumpVideo()
{
    if (!m_source) return;

    const int64_t want = m_clock.CurrentFrame();
    if (want > 1) m_source->DiscardBefore(want - 1);

    // Peek before popping. The previous version popped and then broke on a
    // frame that was ahead of the clock - which THREW THAT FRAME AWAY, because
    // TryPop has already removed it from the queue. The producer is faster than
    // the clock, so the head was almost always ahead, and the picture froze on
    // the first frame while the queue drained frame by frame into the bin.
    //
    // That is why a short clip looked fine and a 12-minute film did not: with
    // the whole file already decoded into the pipe, the race was won or lost in
    // the first second.
    VideoFrame candidate;
    bool got = false;
    while (true) {
        int64_t next = 0;
        if (!m_source->PeekFrontNumber(next)) break;   // nothing decoded yet
        if (next > want) break;                        // leave it for later
        if (!m_source->TryPop(candidate)) break;       // lost it to nobody else
        m_current = std::move(candidate);
        got = true;
    }

    if (got && !m_current.bgra.empty()) {
        m_surface->setFrame(m_current.bgra.data(), int(m_info.width), int(m_info.height));
        m_hasFrame = true;
    }

    // The bar follows the clock, not the frames: the frames lag it by however
    // long the pipe takes, and a bar that jumped with them would look like it
    // was stuttering.
    if (m_timeline) m_timeline->setPosition(m_clock.Position());

    if (m_clock.AtEnd() && m_clock.IsPlaying()) {
        m_clock.Pause();
        if (m_audio) m_audio->Pause(true);
        updateLabels();
    }
}

void PlayerWindow::onToolbarAction(ToolbarAction action)
{
    switch (action) {
    case ToolbarAction::Open: {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open a video"), QString(),
            tr("Video (*.mp4 *.mkv *.mov *.webm *.avi *.gif *.png *.jpg *.jpeg)"));
        if (!path.isEmpty()) open(path);
        break;
    }
    case ToolbarAction::PlayPause:
        togglePlayPause();
        break;
    case ToolbarAction::Stop:
        m_clock.Pause();
        m_clock.Seek(0.0);
        if (m_source) m_source->Seek(0.0);
        if (m_audio) m_audio->Seek(0.0);
        updateLabels();
        break;
    case ToolbarAction::Back10:
        seekBy(-10.0);
        break;
    case ToolbarAction::Forward10:
        seekBy(10.0);
        break;
    case ToolbarAction::Mute: {
        if (!m_audio) break;
        m_audio->SetVolume(m_audio->Volume() > 0.0f ? 0.0f : 1.0f);
        m_toolbar->setActive(ToolbarAction::Mute, m_audio->Volume() == 0.0f);
        break;
    }
    case ToolbarAction::Fullscreen:
        isFullScreen() ? showNormal() : showFullScreen();
        break;
    case ToolbarAction::Aspect:
        m_surface->setFill(!m_surface->fill());
        break;
    default:
        // The neural, upscaling and frame-generation pills are present in the
        // bar because the layout function places them, and disabled because
        // this port has no renderer to hand them to. Their availability says so.
        break;
    }
}

void PlayerWindow::onRecentFile(const QString& path)
{
    if (path.isEmpty()) {
        // Clearing the list is the only other thing this signal carries.
        QSettings settings;
        settings.setValue(QStringLiteral("recentVideos"), QStringList());
        m_menu->setRecentFiles({});
        return;
    }
    open(path);
}

void PlayerWindow::rememberRecent(const QString& path)
{
    if (path.isEmpty()) return;
    QSettings settings;

    // The Windows build keeps its recent list in the ini beside the placement;
    // here it is the same idea in QSettings. Newest first, deduplicated, and
    // capped so the menu stays a menu.
    QStringList recent = settings.value(QStringLiteral("recentVideos")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    constexpr int kMaxRecent = 8;
    while (recent.size() > kMaxRecent) recent.removeLast();
    settings.setValue(QStringLiteral("recentVideos"), recent);
    m_menu->setRecentFiles(recent);
}

void PlayerWindow::syncMenuState()
{
    if (!m_menu) return;
    m_menu->setMediaLoaded(m_source != nullptr);
    m_menu->setPlaying(m_clock.IsPlaying());
    m_menu->setMuted(m_audio && m_audio->Volume() <= 0.0f);
    m_menu->setFullscreen(isFullScreen());
    m_menu->setAspect(m_surface && m_surface->fill() ? 1 : 0);
}

void PlayerWindow::onMenuCommand(MenuCommand command, int index)
{
    Q_UNUSED(index);
    switch (command) {
    case MenuCommand::Open: {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open a video"), QString(),
            tr("Video (*.mp4 *.mkv *.mov *.webm *.avi *.gif *.png *.jpg *.jpeg)"));
        if (!path.isEmpty()) open(path);
        break;
    }
    case MenuCommand::Exit:
        close();
        break;
    case MenuCommand::PlayPause:
        togglePlayPause();
        break;
    case MenuCommand::Stop:
        m_clock.Pause();
        m_clock.Seek(0.0);
        if (m_source) m_source->Seek(0.0);
        if (m_audio) m_audio->Seek(0.0);
        updateLabels();
        break;
    case MenuCommand::Back10:
        seekBy(-10.0);
        break;
    case MenuCommand::Forward10:
        seekBy(10.0);
        break;
    case MenuCommand::Mute: {
        if (!m_audio) break;
        const float next = m_audio->Volume() > 0.0f ? 0.0f : 1.0f;
        m_audio->SetVolume(next);
        m_toolbar->setVolume(next);
        m_toolbar->setActive(ToolbarAction::Mute, next == 0.0f);
        syncMenuState();
        break;
    }
    case MenuCommand::AspectFit:
        m_surface->setFill(false);
        syncMenuState();
        break;
    case MenuCommand::AspectFill:
        m_surface->setFill(true);
        syncMenuState();
        break;
    case MenuCommand::AspectPixels:
        // One-to-one is a third mode the surface does not have yet; it is claimed
        // as checked so the radio group stays consistent, but the picture is fit.
        m_surface->setFill(false);
        syncMenuState();
        break;
    case MenuCommand::GotoTimecode: {
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, tr("Go to timecode"), tr("hh:mm:ss.mmm"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || text.isEmpty()) break;
        const double seconds = ParseTimecode(text);
        if (seconds >= 0.0) {
            m_clock.Seek(seconds);
            if (m_source) m_source->Seek(seconds);
            if (m_audio) m_audio->Seek(seconds);
        }
        break;
    }
    case MenuCommand::Fullscreen:
        isFullScreen() ? showNormal() : showFullScreen();
        syncMenuState();
        break;
    case MenuCommand::Shortcuts:
        QMessageBox::information(
            this, tr("Keyboard shortcuts"),
            tr("Space\tPlay / Pause\n"
               "Left / Right\tBack / forward 10 seconds\n"
               ".\tStep one frame\n"
               "A\tAspect: fit / fill\n"
               "M\tMute\n"
               "F\tFullscreen\n"
               "Ctrl+O\tOpen a video\n"
               "Ctrl+G\tGo to timecode"));
        break;
    case MenuCommand::About:
        QMessageBox::about(
            this, tr("About DLSS 5 Video Player"),
            tr("<b>DLSS 5 Video Player</b><br>"
               "macOS port, phase 4.<br><br>"
               "Video is decoded by an ffmpeg child process over a pipe and drawn "
               "by Qt. The audio clock is the master clock; the picture follows it.<br><br>"
               "Neural rendering, the comparison views, upscaling and export are "
               "not in this build."));
        break;
    default:
        // Every remaining command is a row this port deliberately leaves greyed:
        // it is present so the menu reads like the Windows player's, and disabled
        // because the engine behind it does not exist here.
        break;
    }
}

void PlayerWindow::togglePlayPause()
{
    if (m_clock.IsPlaying()) {
        m_clock.Pause();
        if (m_audio) m_audio->Pause(true);
    } else {
        if (m_audio) m_audio->Pause(false);
        m_clock.Play();
    }
    updateLabels();
}

void PlayerWindow::seekBy(double seconds)
{
    const double target = std::max(0.0, m_clock.Position() + seconds);
    m_clock.Seek(target);
    if (m_source) m_source->Seek(target);
    if (m_audio) m_audio->Seek(target);
    m_hasFrame = false;
}

void PlayerWindow::updateAvailability()
{
    ToolbarAvailability availability;
    availability.mediaLoaded = (m_source != nullptr);
    // A renderer is what the neural, upscaling and frame-generation pills need
    // and this port has none, so they stay unavailable - which is honest rather
    // than a bug: IsToolbarActionEnabled() reads these to decide.
    availability.rendererReady = false;
    availability.neuralRenderingAvailable = false;
    availability.neuralSessionAvailable = false;
    availability.upscalingAvailable = false;
    availability.frameGenerationAvailable = false;
    availability.playbackFrameGenerationAvailable = false;
    availability.subtitlesAvailable = false;
    m_toolbar->setAvailability(availability);
}

void PlayerWindow::updateLabels()
{
    // Labels come from the player's state, and the two that change while
    // playing are the transport and the sound.
    const bool playing = m_clock.IsPlaying();
    m_toolbar->setLabel(ToolbarAction::PlayPause, playing ? tr("Pause") : tr("Play"));
    m_toolbar->setActive(ToolbarAction::PlayPause, playing);

    m_toolbar->setLabel(ToolbarAction::Open, tr("Open"));
    m_toolbar->setLabel(ToolbarAction::Back10, QStringLiteral("10s"));
    m_toolbar->setLabel(ToolbarAction::Forward10, QStringLiteral("10s"));
    m_toolbar->setLabel(ToolbarAction::Mute, tr("Mute"));
    m_toolbar->setLabel(ToolbarAction::Fullscreen, tr("Fullscreen"));

    // The feature pills keep their Windows labels and stay disabled; naming them
    // is what makes the bar look like the same player rather than a stub.
    m_toolbar->setLabel(ToolbarAction::ToggleNeuralRendering, tr("Neural Rendering"));
    m_toolbar->setLabel(ToolbarAction::ToggleUpscaling, tr("Upscaling"));
    m_toolbar->setLabel(ToolbarAction::ToggleFrameGeneration, tr("Frame Generation"));
    m_toolbar->setLabel(ToolbarAction::TogglePlaybackFrameGeneration, tr("Frame Interpolation"));
    m_toolbar->setLabel(ToolbarAction::Aspect, tr("Aspect"));
}

void PlayerWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayPause();
        return;
    case Qt::Key_Left:
        seekBy(-10.0);
        return;
    case Qt::Key_Right:
        seekBy(10.0);
        return;
    case Qt::Key_Period:
        seekBy(1.0 / m_clock.FrameRate());
        return;
    case Qt::Key_A:
        m_surface->setFill(!m_surface->fill());
        return;
    case Qt::Key_M:
        if (m_audio) {
            m_audio->SetVolume(m_audio->Volume() > 0.0f ? 0.0f : 1.0f);
            m_toolbar->setActive(ToolbarAction::Mute, m_audio->Volume() == 0.0f);
        }
        return;
    default:
        break;
    }
    QMainWindow::keyPressEvent(event);
}

void PlayerWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // The bar's height is the portable layout's own answer for this DPI, so the
    // window does not guess at a toolbar height.
    const int dpi = int(std::lround(logicalDpiX()));
    if (m_toolbar) m_toolbar->setFixedHeight(ToolbarWidget::heightForDpi(dpi));
}
