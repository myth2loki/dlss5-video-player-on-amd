#include "PlayerWindow.h"

#include "AudioTrackPolicy.h"
#include "CompareViewPolicy.h"
#include "LiveSessionPolicy.h"
#include "SessionBlockPolicy.h"

namespace {

// Measured on this machine: 95 ms per frame at 2118x1046, about 0.35x real time
// at 30 fps. The Windows build's 2.3x is a different regime - there a session
// runs AHEAD of the viewer and the low-water mark is what stops it converting a
// film nobody has reached. Here the render is behind from the first frame, so a
// lead of zero is the normal state and must not end the session.
//
// The ratio is still used by StartLead / WindowLowWater, so the ported policy
// keeps answering the attach and pacing questions rather than this window
// guessing at them.
constexpr double kRenderRealtimeRatio = 0.35;
constexpr double kColdStartSeconds = 0.7;

}  // namespace

#include "TimelineWidget.h"
#include "ToolbarWidget.h"
#include "UiLayout.h"
#include "UiTheme.h"
#include "VideoSurface.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QUrl>
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

    // The picture carries the comparison switch, so it is drawn over the frames
    // rather than in the toolbar - whose layout is a verbatim port of the Windows
    // bar's measurement and has no room for a control this port invents.
    m_surface->setMouseTracking(true);
    connect(m_surface, &VideoSurface::comparisonRequested, this, [this](bool neural) {
        setComparisonView(neural ? PlaybackView::Neural : PlaybackView::Original);
    });
    connect(m_surface, &VideoSurface::comparisonUnavailable, this, [this]() {
        statusBar()->showMessage(
            tr("Nothing converted at this moment — press Ctrl+Shift+L (or use the Neural menu) "
               "to keep converting while you watch."),
            12000);
    });
    connect(m_surface, &VideoSurface::dividerChangeRequested, this, [this](float u) {
        m_compareParams.divider = u;
        // No QSettings write here: this signal fires on every mouse move during
        // a drag. The destructor persists the last position once.
        applyCompareParams(false);
    });

    m_subtitles = std::make_unique<SubtitleTrack>();

    setCentralWidget(central);

    connect(m_toolbar, &ToolbarWidget::actionTriggered, this, &PlayerWindow::onToolbarAction);
    connect(m_toolbar, &ToolbarWidget::volumeChanged, this, [this](double value) {
        if (!m_audio) return;
        m_audio->SetVolume(float(value));
        m_savedVolume = float(value);
        saveSettings();
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

    connect(&m_neural, &NeuralProcessor::progress, this, &PlayerWindow::onNeuralProgress);
    connect(&m_neural, &NeuralProcessor::finished, this, &PlayerWindow::onNeuralFinished);

    // One probe at startup. What it finds decides whether the neural pills are
    // live, and the answer is a property of the installation rather than of the
    // file that is open.
    m_neuralAvailability = NeuralProcessor::probe();

    connect(m_timeline, &TimelineWidget::seekRequested, this, [this](double seconds) {
        // The timeline's own seek, which is the same path as the keyboard's:
        // the clock and both engines move together or the picture and the sound
        // disagree about where they are.
        seekPlayback(seconds);
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

    loadSettings();
    updateAvailability();
    updateLabels();
    syncMenuState();
}

PlayerWindow::~PlayerWindow()
{
    m_timer.stop();
    // The divider is dragged continuously and is deliberately not saved on
    // every move; one write here is enough and it also catches a Mix or gain
    // stepped by key between menu actions.
    saveSettings();
    if (m_source) m_source->Close();
    if (m_audio) m_audio->Stop();
}

bool PlayerWindow::open(const QString& path)
{
    if (m_neural.busy()) {
        // Opening a file mid-job would leave the job writing a result for a clip
        // nobody is looking at, and the two would then compete for the GPU.
        statusBar()->showMessage(tr("Finish or cancel the neural job first."), 8000);
        return false;
    }

    const std::string utf8 = path.toStdString();

    MediaInfo info;
    if (!FfmpegPipeSource::Probe(utf8, info)) {
        statusBar()->showMessage(tr("Cannot read this file."));
        return false;
    }
    m_info = info;
    // What a neural job would act on. Recorded before the source opens, so a file
    // that fails to open is not offered as a candidate.
    m_currentPath = path;
    // Let the new file choose its own default track. An index from the last
    // film could name a commentary here, or nothing at all.
    m_audioTrackIndex = -1;

    // A live session belongs to the file that started it. Opening another one
    // tears down every trace of it - including a session that is merely armed
    // and waiting for its buffer to drain - or the next tick would start a block
    // for the OLD feature at the OLD session start against the NEW file.
    m_neuralSession = false;
    m_sessionRetargetPending = false;
    m_sessionSeekPending = false;
    m_sessionAttached = false;
    m_sessionStart = 0.0;
    m_sessionJobHead = 0.0;
    m_pendingRange = {0.0, 0.0};
    m_pendingRangeValid = false;
    m_lastPairFault.clear();
    m_lastPair = PlaybackPair{};
    m_hasFrame = false;

    // Anything converted by an earlier session is picked up here, before playback
    // starts, so a film already partly rendered opens showing the rendered parts.
    // The pairing is torn down first because it borrows the decoder about to be
    // replaced.
    teardownPairing();
    m_renderCache = std::make_unique<RenderCache>(path, m_activeKind);
    if (m_renderCache->usable()) {
        // load() before any setter. The setters persist, and this process's segment
        // list is empty until load has filled it - so a setter first would write an
        // empty manifest over the one on disk and the film would silently lose the
        // parts already converted.
        m_renderCache->load();
        m_renderCache->setSourceDuration(info.duration);
    } else {
        m_renderCache.reset();
    }
    updateTimelineCoverage();

    // The comparison switch is drawn from the first frame, dimmed, even though
    // nothing has been converted yet. That is deliberate: a viewer who does not know
    // the feature exists will never go looking in a menu for it, and two dimmed
    // buttons say "this is where the comparison lives" without claiming anything is
    // available now. Which side the viewer last chose survives the file change.
    m_viewAvailable = false;
    if (m_surface) m_surface->setComparison(false, m_view == PlaybackView::Neural);
    if (m_menu) {
        m_menu->setSessionActive(false);
        m_menu->setComparisonView(m_view == PlaybackView::Neural);
        m_menu->setCacheCommandsEnabled(m_renderCache &&
                                        !m_renderCache->coverage().empty());
    }

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
    m_audioDriven = m_audio->Start(utf8, 0.0, AudioStartState::Playing, m_audioTrackIndex);
    m_audio->SetVolume(m_savedVolume);
    if (m_toolbar) m_toolbar->setVolume(double(m_savedVolume));
    m_clock.SetAudioSource(m_audioDriven ? m_audio.get() : nullptr);

    // The menu's Audio submenu is built from what ffprobe actually found, named
    // by the ported AudioTrackPolicy so two English tracks are still tellable.
    m_audioTrackLabels.clear();
    for (const AudioTrackInfo& info : m_audio->Tracks()) {
        audio_track::Track track;
        track.audioIndex = info.streamIndex;
        track.language = info.language;
        track.title = info.title;
        track.codec = info.codec;
        track.channels = info.channels;
        track.sampleRate = info.sampleRate;
        track.isDefault = info.isDefault;
        track.comment = info.comment;
        track.visualImpaired = info.visualImpaired;
        track.descriptions = info.descriptions;
        track.hearingImpaired = info.hearingImpaired;
        m_audioTrackLabels.append(QString::fromStdString(audio_track::Describe(track)));
    }
    m_audioTrackIndex = m_audio->TrackIndex();
    if (m_menu) m_menu->setAudioTracks(m_audioTrackLabels, m_audioTrackIndex);

    // Sidecar subtitles belong to this file. Off is the default, as in the
    // Windows build: a file with several language tracks should not surprise
    // the viewer by choosing one.
    m_subtitlePaths = SubtitleTrack::discover(path);
    m_subtitleIndex = -1;
    m_subtitles->clear();
    m_surface->setSubtitle(QString());
    if (m_menu) m_menu->setSubtitleFiles(m_subtitlePaths, m_subtitleIndex);

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
    serviceSession();
}

// Keeps a live session fed.
//
// Every question this asks is about the playhead, so it runs on the presentation
// tick rather than on a timer of its own. Three decisions, all from the ported
// policy layer: whether playback has attached to converted ground, whether a
// seek has made the running job's hole the wrong one, and whether the buffer has
// drained enough to start another block. WHERE that block goes is answered by
// SessionBlockPolicy, not here.
void PlayerWindow::serviceSession()
{
    if (!m_neuralSession || !m_renderCache || !m_renderCache->usable()) return;

    const double duration = m_info.duration;
    if (duration <= 0.0) return;

    const double position = m_clock.Position();
    const double head = m_renderCache->renderHead();
    const std::vector<CoverageSpan> coverage = m_renderCache->coverage();
    const double frameDuration = 1.0 / std::max(m_info.fps, 1.0);
    const int64_t frameTicks = SecondsToTicks(frameDuration);
    const int64_t durationTicks = SecondsToTicks(duration);
    const int64_t positionTicks = SecondsToTicks(std::clamp(position, 0.0, duration));

    live_session::SessionView view;
    view.positionSec = position;
    view.rangeStartSec = m_sessionStart;
    view.headSec = head;
    view.attached = m_sessionAttached;
    view.finished = head >= duration - frameDuration;
    view.seeking = m_sessionSeekPending;
    view.paused = !m_clock.IsPlaying();

    const std::optional<CoverageSpan> span = SpanContaining(coverage, positionTicks);
    if (!m_sessionAttached && span &&
        live_session::ShouldAttach(view, live_session::StartLead(kRenderRealtimeRatio))) {
        m_sessionAttached = true;
    } else if (m_sessionAttached && !span) {
        m_sessionAttached = false;
    }

    // A sub-real-time render never builds a lead, so the picture is the source
    // for most of the film. Saying so once is the difference between an honest
    // status bar and a viewer who thinks the feature did nothing.
    if (!span && !live_session::ShouldResume(view) && !m_sessionBehindNotified) {
        m_sessionBehindNotified = true;
        statusBar()->showMessage(
            tr("The render is behind playback — showing the original until converted ground is reached."),
            8000);
    }
    if (span) m_sessionBehindNotified = false;

    const bool seeking = m_sessionSeekPending;
    m_sessionSeekPending = false;

    // A job is running. A seek may have made its hole the wrong one; the ported
    // ShouldRetarget decides whether taking the job away is worth another CLI
    // startup. A finished signal from a cancelled job never arrives, so the
    // retarget flag is what makes the next tick start a block at the new place.
    if (m_neural.busy()) {
        if (seeking && m_pendingRangeValid) {
            const std::vector<CoverageSpan> holes =
                UncoveredSpans(coverage, {0, durationTicks}, frameTicks);
            const std::optional<CoverageSpan> wanted = NextRenderTarget(holes, positionTicks);
            const CoverageSpan target{SecondsToTicks(m_pendingRange.first),
                                      SecondsToTicks(m_pendingRange.second)};
            live_session::SessionView deciding = view;
            deciding.seeking = false;   // the seek is being decided, not still in flight
            if (wanted && live_session::ShouldRetarget(deciding, target, *wanted, frameTicks,
                                                       m_sessionJobHead)) {
                m_neural.cancel();
                m_pendingRangeValid = false;
                m_sessionRetargetPending = true;
                statusBar()->showMessage(tr("Conversion redirected to where you are watching."),
                                         5000);
            }
        }
        return;
    }

    // No new work while the picture is still: a paused viewer is not draining
    // the buffer, and converting the rest of a film they may never resume is
    // what the Windows low-water mark exists to prevent. A seek retarget is the
    // exception - the viewer asked for the frame they just jumped to.
    if (view.paused && !m_sessionRetargetPending) return;

    const bool force = m_sessionRetargetPending;
    m_sessionRetargetPending = false;
    const double lowWater =
        force ? 1e9
              : live_session::WindowLowWater(m_sessionBlockSeconds, kRenderRealtimeRatio,
                                             kColdStartSeconds, m_sessionLowWaterSeconds);

    const session_block::BlockPlan plan = session_block::PlanNextBlock(
        coverage, position, head, duration, m_sessionBlockSeconds, lowWater, frameDuration);
    if (plan.done) {
        endSession(tr("The whole film is converted."), false);
        return;
    }
    if (plan.start) startNextBlock(plan.startSec, plan.endSec);
}

void PlayerWindow::pumpVideo()
{
    if (!m_source) return;

    // With rendered segments on disk the picture can come from either decoder, so
    // the read goes through the pairing instead of straight out of the pipe. That
    // is the whole point of the cache: a converted stretch shows the rendered
    // frames and everything else shows the source.
    //
    // The pairing is built lazily and torn down on open, because it holds a second
    // decoder and most sessions never convert anything.
    if (m_renderCache && !m_renderCache->coverage().empty()) {
        pumpPaired();
        return;
    }

    // Plain playback: no rendered ground at all, so the comparison switch is drawn
    // but dimmed. Setting it here rather than only at open covers the case where a
    // cache exists but not under this playhead - the same dim state, reached by
    // playing out of the rendered stretch.
    if (m_viewAvailable) {
        m_viewAvailable = false;
        if (m_menu) m_menu->setComparisonAvailable(false);
        if (m_surface) m_surface->setComparison(false, m_view == PlaybackView::Neural);
    }

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
    // Subtitles are a HUD on the picture, not part of the decoded frame: they
    // follow the clock here too, in the plain path that has no pair.
    refreshSubtitle(m_clock.Position());

    // The bar follows the clock, not the frames: the frames lag it by however
    // long the pipe takes, and a bar that jumped with them would look like it
    // was stuttering.
    if (m_timeline) m_timeline->setPosition(m_clock.Position());

    if (m_clock.AtEnd() && m_clock.IsPlaying()) {
        m_clock.Pause();
        if (m_audio) m_audio->Pause(true);
        // The film is over, so there is no playhead for a live session to
        // follow. A job already running is left to finish and be cached; no new
        // block is started.
        if (m_neuralSession) {
            m_neuralSession = false;
            if (m_menu) m_menu->setSessionActive(false);
        }
        updateLabels();
    }
}

// The read path when rendered segments exist.
//
// The original decoder is the one this window already owns; the segments get their
// own through the factory. `SegmentPlayback` decides which picture belongs on
// screen for the current moment and reports why it could not when it cannot, so
// this function is only the part that talks to the clock and the surface.
// Draws one pair, according to the view.
//
// Separate from the read so a paused player can present the same pair again after
// the view changes - or after the switch is pressed twice, or after a resize.
// Without it the only way to change which frame is on screen is to read another
// one, which moves the picture.
void PlayerWindow::presentPair(const PlaybackPair& pair)
{
    if (!m_surface) return;

    // Both halves go to the surface whenever they exist. Even a viewer asking
    // for "Original" may be one press away from Split, and a surface holding
    // only one frame would have to re-read (and therefore advance) the pair to
    // answer that press - which is precisely what a paused comparison must not
    // do.
    const bool both = pair.neuralValid && !pair.neural.bgra.empty() &&
                      !pair.original.bgra.empty();
    if (both) {
        m_surface->setPair(pair.original.bgra.data(), pair.neural.bgra.data(),
                           int(m_info.width), int(m_info.height));
        m_hasFrame = true;
    } else {
        // The pair carries the view it was read under. A paused re-presentation
        // must not consult a newer state: the viewer pressed pause on THIS pair.
        const bool showNeural = pair.view == PlaybackView::Neural && pair.neuralValid &&
                                !pair.neural.bgra.empty();
        const VideoFrame& frame = showNeural ? pair.neural : pair.original;
        if (frame.bgra.empty() && !m_hasFrame) return;
        if (!frame.bgra.empty()) {
            m_surface->setFrame(frame.bgra.data(), int(m_info.width), int(m_info.height));
            m_hasFrame = true;
        }
    }

    refreshSubtitle(m_clock.Position());

    // The comparison switch is live only while both halves of this pair exist.
    // Reported per frame rather than per seek, so it follows the picture as
    // playback crosses into rendered ground.
    if (m_viewAvailable != both) {
        m_viewAvailable = both;
        if (m_menu) m_menu->setComparisonAvailable(m_viewAvailable);
        // Always DRAWN - its absence over unrendered ground would be a puzzle to
        // a viewer who converted a film earlier - but it only responds when there
        // is something to switch between.
        m_surface->setComparison(m_viewAvailable,
                                 m_compareParams.mode == compare_compose::Mode::Neural);
    }
}

void PlayerWindow::pumpPaired()
{
    // Built on first use. A viewer who never converts anything pays nothing, and a
    // viewer who does pays a second decoder only once there is something to pair.
    if (!m_pairing) setupPairing();
    if (!m_pairing) return;

    // Paused: show the frame already held, and do not read another one.
    //
    // This is what makes a paused comparison work. The viewer presses pause on a
    // frame, then presses DLSS 5 / ORIGINAL to examine that frame twice; a read
    // here would advance the pair and the second picture would be of a different
    // moment, which is exactly what a comparison must not do. The Windows build
    // holds its pair and re-presents a member of it for the same reason
    // (CurrentPairShared and SetView).
    if (!m_clock.IsPlaying()) {
        presentPair(m_lastPair);
        return;
    }

    // Segments arrive as renders finish, so the pairing's view of them is refreshed
    // whenever the cache has changed rather than once at open: a viewer watching a
    // film being converted should see each new stretch appear where it belongs.
    if (m_renderCache && m_renderCache->revision() != m_pairingRevision) {
        m_pairing->setSegments(m_renderCache->segments(), m_info.fps);
        m_pairingRevision = m_renderCache->revision();
    }

    PlaybackPair pair;
    const PlaybackReadResult result = m_pairing->readNext(pair);

    switch (result) {
    case PlaybackReadResult::PairReady:
        // Held so a pause can re-present it without reading: the comparison switch
        // needs the same pair twice.
        m_lastPair = pair;
        presentPair(pair);
        break;

    case PlaybackReadResult::WaitingForRender:
        // The playhead has reached the render head. Playback waits rather than
        // ending: the film is not over, its next seconds have not been made yet.
        statusBar()->showMessage(tr("Waiting for the render to reach this point…"), 2000);
        break;

    case PlaybackReadResult::NotReady:
        // A decoder is filling its queue. Nothing to do this tick.
        break;

    case PlaybackReadResult::EndOfStream:
        if (m_clock.IsPlaying()) {
            m_clock.Pause();
            if (m_audio) m_audio->Pause(true);
            if (m_neuralSession) {
                m_neuralSession = false;
                if (m_menu) m_menu->setSessionActive(false);
            }
            updateLabels();
        }
        break;

    case PlaybackReadResult::Error: {
        // A render that cannot be paired is not a reason to stop the film: fall back
        // to the source decoder, which is what the viewer came for. The reason is
        // reported once rather than every tick.
        const QString fault = m_pairing->lastFault();
        if (fault != m_lastPairFault) {
            m_lastPairFault = fault;
            statusBar()->showMessage(tr("Rendered frames stopped pairing (%1) — showing the source").arg(fault),
                                     8000);
        }
        teardownPairing();
        break;
    }
    }

    if (m_timeline) m_timeline->setPosition(m_clock.Position());
}

// Builds the pairing for the file that is now open.
//
// The original member is the decoder this window already runs, borrowed rather
// than duplicated: the window's source is open for the whole session, and a second
// ffmpeg on the same file would double the decoding for no gain. The segments get
// their own decoder per segment, because a closed FfmpegPipeSource cannot be
// reopened - its stop flag stays set - so the factory makes a fresh one each time.
void PlayerWindow::setupPairing()
{
    if (!m_source || !m_renderCache) return;

    m_borrowedOriginal = std::make_unique<BorrowedFrameSource>(*m_source, m_info);
    const MediaInfo info = m_info;
    m_pairing = std::make_unique<SegmentPlayback>(
        *m_borrowedOriginal,
        [info]() -> std::unique_ptr<FrameSource> { return std::make_unique<FfmpegFrameSource>(info); });

    m_pairing->setSegments(m_renderCache->segments(), m_info.fps);
    m_pairingRevision = m_renderCache->revision();
    m_lastPairFault.clear();
}

void PlayerWindow::teardownPairing()
{
    // The borrowed original is released before the pairing, because the pairing
    // holds a reference to it.
    m_pairing.reset();
    m_borrowedOriginal.reset();
    m_pairingRevision = -1;
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
        seekPlayback(0.0);
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
    case ToolbarAction::ToggleFrameGeneration:
        // The pill looks like a switch because the Windows build's is one, but
        // there is nothing to switch: this starts an export of the current file
        // and loads the result. A 2x interpolation of a 90-minute film is an
        // hour of GPU time, so it cannot be a playback mode.
        runNeuralJob(NeuralFeature::FrameGeneration);
        break;
    case ToolbarAction::ToggleNeuralRendering:
        runNeuralJob(NeuralFeature::NeuralRendering);
        break;
    case ToolbarAction::Fullscreen:
        isFullScreen() ? showNormal() : showFullScreen();
        break;
    case ToolbarAction::Aspect:
        m_surface->setFill(!m_surface->fill());
        break;
    default:
        // Upscaling and LIVE frame interpolation are present because the layout
        // function places them, and disabled because this port has no engine to
        // hand them to. The offline 2x export above is a real command.
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
    m_menu->setNeuralAvailability(m_neuralAvailability.canDo(NeuralFeature::FrameGeneration),
                                  m_neuralAvailability.canDo(NeuralFeature::NeuralRendering));
    m_menu->setNeuralBusy(m_neural.busy());
    m_menu->setSessionActive(m_neuralSession);
    // Only when a pair is actually in hand: an empty film, an unrendered stretch and
    // an unloaded window all offer nothing to compare.
    m_menu->setComparisonAvailable(m_viewAvailable);
    // One mode enum is the truth for both the two lead rows and the eight mode
    // rows. Setting it here too (rather than only when it changes) is cheap and
    // keeps the menu right after a file change or a cache clear.
    m_menu->setCompareMode(compare_compose::IndexOf(m_compareParams.mode));
    m_menu->setCompareOptions(m_compareParams.swapped, m_compareParams.lumaOnly,
                              m_compareParams.mix, m_compareParams.gain);
    m_menu->setSecondMix(m_compareParams.secondMix);
}

void PlayerWindow::onMenuCommand(MenuCommand command, int index)
{
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
        seekPlayback(0.0);
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
        if (seconds >= 0.0) seekPlayback(seconds);
        break;
    }
    case MenuCommand::Fullscreen:
        isFullScreen() ? showNormal() : showFullScreen();
        syncMenuState();
        break;
    case MenuCommand::SaveComparisonImage:
        saveComparisonImage();
        break;
    case MenuCommand::CompareDlss5:
        setComparisonView(PlaybackView::Neural);
        break;
    case MenuCommand::CompareOriginal:
        setComparisonView(PlaybackView::Original);
        break;
    case MenuCommand::CompareMode:
        setCompareMode(compare_compose::FromIndex(index));
        break;
    case MenuCommand::CompareMixLess:
        stepMix(-0.1f);
        break;
    case MenuCommand::CompareMixMore:
        stepMix(+0.1f);
        break;
    case MenuCommand::CompareGainLess:
        stepGain(-1);
        break;
    case MenuCommand::CompareGainMore:
        stepGain(+1);
        break;
    case MenuCommand::CompareSwap:
        m_compareParams.swapped = !m_compareParams.swapped;
        applyCompareParams(true);
        break;
    case MenuCommand::CompareLuma:
        m_compareParams.lumaOnly = !m_compareParams.lumaOnly;
        applyCompareParams(true);
        break;
    case MenuCommand::CompareNextMode:
        stepCompareMode(+1);
        break;
    case MenuCommand::ComparePrevMode:
        stepCompareMode(-1);
        break;
    case MenuCommand::CompareSecondMix:
        if (index >= 0 && index < int(compare_settings::kSecondMixes.size())) {
            m_compareParams.secondMix = compare_settings::kSecondMixes[size_t(index)];
            applyCompareParams(true);
        }
        break;
    case MenuCommand::AudioTrack:
        setAudioTrack(index);
        break;
    case MenuCommand::SubtitlesOff:
        m_subtitles->clear();
        m_subtitleIndex = -1;
        m_surface->setSubtitle(QString());
        if (m_menu) m_menu->setSubtitleFiles(m_subtitlePaths, -1);
        updateAvailability();
        statusBar()->showMessage(tr("Subtitles off."), 4000);
        break;
    case MenuCommand::SubtitleFile:
        setSubtitleIndex(index);
        break;
    case MenuCommand::LoadSubtitleFile:
        loadSubtitleFile(QFileDialog::getOpenFileName(
            this, tr("Load subtitle file"), QString(), SubtitleTrack::fileFilter()));
        break;
    case MenuCommand::FrameGeneration:
        runNeuralJob(NeuralFeature::FrameGeneration);
        break;
    case MenuCommand::NeuralRendering:
        runNeuralJob(NeuralFeature::NeuralRendering);
        break;
    case MenuCommand::CancelNeuralJob:
        // Cancelling the JOB is not the same as cancelling the SESSION. The
        // first version left the session armed, so the next tick started
        // another block - a cancel that restarted the work it was cancelling.
        endSession(tr("Neural job cancelled; converted segments are kept."), true);
        break;
    case MenuCommand::LiveSession:
        if (m_neuralSession) {
            endSession(tr("Conversion stopped; converted segments are kept."), true);
        } else {
            runNeuralJob(m_activeKind == RenderKind::FrameGeneration
                             ? NeuralFeature::FrameGeneration
                             : NeuralFeature::NeuralRendering);
        }
        break;
    case MenuCommand::RenderWindow:
        if (index >= 0 && index < session_block::kBlockChoiceCount)
            setRenderWindow(session_block::kBlockChoices[index]);
        break;
    case MenuCommand::ClearNeuralCache:
        clearConvertedCache();
        break;
    case MenuCommand::OpenCacheFolder:
        openCacheFolder();
        break;
    case MenuCommand::Shortcuts:
        QMessageBox::information(
            this, tr("Keyboard shortcuts"),
            tr("Space\tPlay / Pause\n"
               "Left / Right\tBack / forward 10 seconds\n"
               ".\tStep one frame\n"
               "D\tSwap DLSS 5 / Original on the same frame\n"
               "C / Shift+C\tNext / previous comparison mode\n"
               "[\tMix less      ]\tMix more\n"
               "Shift+[ / Shift+]\tLess / more difference gain\n"
               "X\tSwap the two sides (Split, Wipe, Side by side, 2x2)\n"
               "Hold on the picture\tPeek at the original\n"
               "A\tAspect: fit / fill\n"
               "M\tMute\n"
               "F\tFullscreen\n"
               "Ctrl+O\tOpen a video\n"
               "Ctrl+G\tGo to timecode\n"
               "Ctrl+D / Ctrl+Shift+D\tDLSS 5 / Original\n"
               "Ctrl+Shift+S\tSave the comparison as shown\n"
               "Ctrl+Shift+L\tKeep converting while playing"));
        break;
    case MenuCommand::About:
        QMessageBox::about(
            this, tr("About DLSS 5 Video Player"),
            tr("<b>DLSS 5 Video Player</b><br>"
               "macOS port.<br><br>"
               "Video is decoded by an ffmpeg child process over a pipe and drawn "
               "by Qt. The audio clock is the master clock; the picture follows it.<br><br>"
               "Neural rendering and 2x frame generation run as cached, per-range "
               "jobs: the player converts the seconds ahead of the playhead, keeps "
               "every finished segment, pairs two decoders and lets you switch "
               "between DLSS 5 and the original on one frame.<br><br>"
               "The pair can also be shown as Blend, Split, Wipe, Difference, "
               "Side by side or 2x2; a comparison image can be saved as PNG, "
               "audio tracks can be switched, and SRT / WebVTT / ASS sidecar "
               "subtitles are drawn over the picture.<br><br>"
               "Upscaling, live frame interpolation, embedded subtitle streams, "
               "HDR and the comparison loupe are not in this build."));
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
    seekPlayback(target);
}

// Every seek goes through here.
//
// The pairing holds state that a seek invalidates: a frame read but not yet paired,
// and a segment decoder positioned for the old playhead. Seeking the pipe alone
// would leave both behind, and the next read would pair the new original position
// against the old segment one - which shows as a still frame or a jump backwards
// right after every scrub.
// Switches which of the pair is on screen.
//
// This does not seek and does not re-render. The pairing has both frames already,
// so the next read hands over the other one - the same moment, one press apart,
// which is what makes a comparison usable. The Windows build does the same thing
// through SynchronizedPlayback::SetView and hands the renderer the other member of
// the pair it is holding.
void PlayerWindow::setComparisonView(PlaybackView view)
{
    m_view = view;
    if (m_pairing) m_pairing->setView(view);
    // The held pair is re-presented immediately below, so it has to carry the
    // new choice too. Otherwise a paused viewer would press the switch and see
    // the old frame until the next read - which never comes while paused.
    m_lastPair.view = view;
    // The two single-frame buttons ARE the first two comparison modes. Keeping
    // one mode enum as the source of truth means the buttons, the menu ticks
    // and the picture can never disagree about which view is active.
    setCompareMode(view == PlaybackView::Neural ? compare_compose::Mode::Neural
                                                : compare_compose::Mode::Original);
    // The switch on the picture shows the same state as the menu, because a viewer
    // looks at one or the other and the two disagreeing would be worse than either
    // being absent.
    if (m_surface) m_surface->setComparison(m_viewAvailable, view == PlaybackView::Neural);

    // Repaint the pair already held, now rather than on the next tick. Paused, there
    // is no next tick that would read a frame - and even while playing, waiting
    // 16 ms to show the frame the viewer just asked for is a delay the switch does
    // not need. This is the whole point of holding the pair: changing which member
    // is shown costs no decoding.
    if (m_pairing && m_viewAvailable) presentPair(m_lastPair);
}

// Swaps the two, for the keyboard shortcut.
void PlayerWindow::toggleComparisonView()
{
    setComparisonView(m_view == PlaybackView::Neural ? PlaybackView::Original
                                                     : PlaybackView::Neural);
}

// ---- the spatial comparison views -----------------------------------------

void PlayerWindow::applyCompareParams(bool persist)
{
    if (m_surface) m_surface->setCompareParams(m_compareParams);
    if (m_menu) {
        m_menu->setCompareMode(compare_compose::IndexOf(m_compareParams.mode));
        m_menu->setCompareOptions(m_compareParams.swapped, m_compareParams.lumaOnly,
                                  m_compareParams.mix, m_compareParams.gain);
        m_menu->setSecondMix(m_compareParams.secondMix);
    }
    if (persist) saveSettings();
    // A paused viewer changing the mode is looking at the frame already held.
    // Re-present it now rather than waiting for a read that will never come.
    if (m_pairing && m_viewAvailable) presentPair(m_lastPair);
}

void PlayerWindow::setCompareMode(compare_compose::Mode mode)
{
    const bool single = mode == compare_compose::Mode::Neural ||
                        mode == compare_compose::Mode::Original;
    if (!single && !m_viewAvailable) {
        statusBar()->showMessage(
            tr("Compare views need a converted frame here — press Ctrl+Shift+L "
               "to keep converting while you watch."),
            10000);
        return;
    }
    m_compareParams.mode = mode;
    if (single) m_view = mode == compare_compose::Mode::Neural
                             ? PlaybackView::Neural : PlaybackView::Original;
    applyCompareParams(true);
    statusBar()->showMessage(
        tr("Compare: %1").arg(QString::fromUtf8(compare_compose::ModeName(mode))), 4000);
}

void PlayerWindow::stepCompareMode(int direction)
{
    if (!m_viewAvailable) {
        statusBar()->showMessage(tr("Nothing converted at this moment to compare."), 8000);
        return;
    }
    setCompareMode(compare_compose::Cycle(m_compareParams.mode, direction));
}

void PlayerWindow::stepMix(float delta)
{
    m_compareParams.mix = compare_settings::StepMix(m_compareParams.mix, delta);
    applyCompareParams(true);
    statusBar()->showMessage(tr("Mix %1%").arg(int(std::lround(m_compareParams.mix * 100))), 4000);
}

void PlayerWindow::stepGain(int direction)
{
    m_compareParams.gain =
        compare_settings::StepDifferenceGain(m_compareParams.gain, direction);
    applyCompareParams(true);
    statusBar()->showMessage(
        tr("Difference gain %1x").arg(m_compareParams.gain, 0, 'f', 0), 4000);
}

void PlayerWindow::saveComparisonImage()
{
    if (!m_surface || !m_surface->hasPair()) {
        statusBar()->showMessage(
            tr("Save a comparison where a converted frame exists at the playhead."), 10000);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save comparison image"), QString(),
        QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
    if (path.isEmpty()) return;

    const QImage image = m_surface->comparisonImage();
    if (image.isNull() || !image.save(path)) {
        QMessageBox::warning(this, tr("Save comparison image"),
                             tr("The comparison image could not be saved."));
        return;
    }
    statusBar()->showMessage(tr("Comparison saved: %1").arg(path), 10000);
}

// ---- audio tracks and subtitles -------------------------------------------

void PlayerWindow::setAudioTrack(int index)
{
    if (!m_audio || index < 0 || index >= m_audioTrackLabels.size()) return;
    const double position = m_clock.Position();
    const bool playing = m_clock.IsPlaying();
    m_audioTrackIndex = index;

    // Restarting only the audio helper is enough: the video decoder and the
    // pairing stay where they are, and the new stream is started at the current
    // position so the two do not drift apart.
    m_audioDriven = m_audio->Start(m_currentPath.toStdString(), position,
                                   playing ? AudioStartState::Playing : AudioStartState::Paused,
                                   index);
    m_audio->SetVolume(m_savedVolume);
    m_clock.SetAudioSource(m_audioDriven ? m_audio.get() : nullptr);
    if (m_menu) m_menu->setAudioTracks(m_audioTrackLabels, index);
    statusBar()->showMessage(tr("Audio: %1").arg(m_audioTrackLabels.at(index)), 5000);
}

void PlayerWindow::refreshSubtitle(double position)
{
    if (!m_surface) return;
    const QString text = (m_subtitles && m_subtitles->loaded())
                             ? m_subtitles->at(position) : QString();
    if (text != m_surface->subtitle()) m_surface->setSubtitle(text);
}

void PlayerWindow::setSubtitleIndex(int index)
{
    if (!m_subtitles || index < 0 || index >= m_subtitlePaths.size()) return;
    if (!m_subtitles->load(m_subtitlePaths.at(index))) {
        statusBar()->showMessage(
            tr("Subtitle file could not be read: %1").arg(m_subtitlePaths.at(index)), 10000);
        return;
    }
    m_subtitleIndex = index;
    if (m_menu) m_menu->setSubtitleFiles(m_subtitlePaths, index);
    refreshSubtitle(m_clock.Position());
    updateAvailability();
    statusBar()->showMessage(
        tr("Subtitles: %1 (%2 cues)")
            .arg(QFileInfo(m_subtitlePaths.at(index)).fileName())
            .arg(m_subtitles->count()),
        5000);
}

void PlayerWindow::loadSubtitleFile(const QString& path)
{
    if (path.isEmpty()) return;
    if (!m_subtitlePaths.contains(path)) m_subtitlePaths.append(path);
    setSubtitleIndex(m_subtitlePaths.indexOf(path));
}

void PlayerWindow::seekPlayback(double seconds)
{
    m_clock.Seek(seconds);
    if (m_source) m_source->Seek(seconds);
    if (m_audio) m_audio->Seek(seconds);
    if (m_pairing) m_pairing->seek(seconds);
    m_hasFrame = false;
    // A live session has to hear about this. The next tick asks ShouldRetarget
    // whether the block currently running is still the one the viewer needs.
    m_sessionSeekPending = true;
}

namespace {


RenderKind cacheKindFor(NeuralFeature feature)
{
    return feature == NeuralFeature::FrameGeneration ? RenderKind::FrameGeneration
                                                     : RenderKind::NeuralRendering;
}

} // namespace

void PlayerWindow::runNeuralJob(NeuralFeature feature)
{
    if (m_currentPath.isEmpty()) return;

    if (m_neural.busy()) {
        statusBar()->showMessage(tr("A neural job is already running."));
        return;
    }

    if (!m_neuralAvailability.canDo(feature)) {
        statusBar()->showMessage(tr("%1 is not installed on this machine.")
                                    .arg(feature == NeuralFeature::FrameGeneration
                                             ? tr("Frame generation") : tr("Neural rendering")),
                                10000);
        return;
    }

    // The cache is keyed by source AND feature. Reused when it is already the
    // right one: replacing it would reset the revision the pairing tracks, so a
    // viewer would see the segments re-announced - or lose the ones already
    // loaded - every time a new range was started.
    const RenderKind kind = cacheKindFor(feature);
    const double duration = m_info.duration;
    if (!m_renderCache || m_renderCache->kind() != kind ||
        m_renderCache->sourcePath() != m_currentPath || !m_renderCache->usable()) {
        teardownPairing();
        m_renderCache = std::make_unique<RenderCache>(m_currentPath, kind);
        if (!m_renderCache->usable()) {
            m_renderCache.reset();
            QMessageBox::information(
                this, tr("Cannot start"),
                tr("The render cache directory is not writable, so converted segments could not be kept."));
            return;
        }
        // load() before any setter, for the reason spelled out in open().
        m_renderCache->load();
        m_renderCache->setSourceDuration(duration);
        updateTimelineCoverage();
    }
    m_activeKind = kind;
    saveSettings();

    // The first block uses the same plan as every later one: the hole under the
    // playhead when the viewer is behind the render head, otherwise the render
    // head itself. A one-shot job and a live session must not disagree about
    // where converted ground begins.
    const double frameDuration = 1.0 / std::max(m_info.fps, 1.0);
    const session_block::BlockPlan plan = session_block::PlanNextBlock(
        m_renderCache->coverage(), m_clock.Position(), m_renderCache->renderHead(), duration,
        m_sessionBlockSeconds, 1e9, frameDuration);
    if (plan.done) {
        statusBar()->showMessage(tr("Everything here is already converted."), 8000);
        return;
    }
    if (!plan.start) {
        statusBar()->showMessage(tr("Nothing left to convert here."), 8000);
        return;
    }

    // The session stays on after this block. That is the difference between
    // "convert this stretch" and "keep the picture converted as I watch".
    m_neuralSession = true;
    m_sessionFeature = feature;
    m_sessionStart = plan.startSec;
    m_sessionAttached = false;
    m_sessionBehindNotified = false;
    m_sessionRetargetPending = false;
    m_sessionSeekPending = false;

    if (!startNextBlock(plan.startSec, plan.endSec)) {
        endSession(QString(), false);
        return;
    }

    // Deliberately NOT pausing playback. The first version of the session paused
    // on start and never resumed: the lead then stayed at a full block forever,
    // above the low-water mark, so no second block ever started. That is the
    // "cache played out and the render stopped" report, and it was a scheduler
    // bug, not a GPU limit.
    if (!m_clock.IsPlaying()) {
        statusBar()->showMessage(
            tr("Resume playback to keep converting the next block; Ctrl+Shift+L stops the session."),
            12000);
    }
}

void PlayerWindow::onNeuralProgress(double fraction, const QString& detail)
{
    // The bar's own text, because the toolbar's lane has no room for a timecode
    // and the status bar is where the Windows build puts it too.
    if (fraction < 0.0) {
        statusBar()->showMessage(tr("Working… %1").arg(detail));
        return;
    }
    // ShouldRetarget needs to know how far the RUNNING job has got, not where
    // the cache's head is. A job that has nearly finished its hole must not be
    // cancelled for a seek that lands one second away.
    if (m_pendingRangeValid) {
        m_sessionJobHead = m_pendingRange.first +
                           std::clamp(fraction, 0.0, 1.0) *
                               (m_pendingRange.second - m_pendingRange.first);
    }
    statusBar()->showMessage(tr("%1%  ·  %2").arg(int(fraction * 100.0)).arg(detail));
}

void PlayerWindow::onNeuralFinished(const NeuralJobResult& result)
{
    m_toolbar->setWorking(ToolbarAction::ToggleFrameGeneration, false);
    m_toolbar->setWorking(ToolbarAction::ToggleNeuralRendering, false);

    // A cancel or a file change can still deliver a late signal. That output
    // belongs to a job nobody is waiting for now, and recording it would put a
    // segment for the OLD file into the NEW file's cache.
    if (!m_pendingRangeValid) {
        syncMenuState();
        return;
    }
    m_pendingRangeValid = false;

    if (!result.ok) {
        // A failure is usually "the build or the weights moved", so re-check what
        // is available. Ending the session here also stops a failed block from
        // being retried sixty times a second by the presentation tick.
        m_neuralAvailability = NeuralProcessor::probe();
        endSession(tr("Neural job failed: %1").arg(result.error), false);
        QMessageBox::warning(this, tr("Neural job failed"), result.error);
        updateAvailability();
        return;
    }

    // Record the range so the timeline can show it and the next job can skip it.
    // The segment's own file starts at its own zero, which is why the range goes
    // with it rather than being read back out of the file.
    if (m_renderCache) {
        RenderSegment segment;
        segment.path = result.outputPath;
        segment.start100ns = SecondsToTicks(m_pendingRange.first);
        segment.end100ns = SecondsToTicks(m_pendingRange.second);
        segment.frameCount = result.outputFrames;
        // Frame generation doubles the frame rate. Pairing such a segment with
        // the source's rate made every frame drift further behind until the
        // whole run was rejected; the segment carries its own rate now.
        segment.frameRate = m_sessionFeature == NeuralFeature::FrameGeneration
                                ? m_info.fps * 2.0
                                : 0.0;

        const int before = m_renderCache->revision();
        m_renderCache->addSegment(segment);
        if (m_renderCache->revision() == before) {
            // A job can report success with no playable file on disk. Treat it
            // as a failure rather than as coverage that will never pair.
            endSession(tr("The render produced no playable file — conversion stopped."), false);
            return;
        }
        updateTimelineCoverage();
        if (m_menu) m_menu->setCacheCommandsEnabled(true);

        // Hand the new segment to the pairing now rather than waiting for the
        // revision check on the next tick. A viewer who converted the frame they are
        // looking at expects the comparison switch to light up immediately, and the
        // pause they are probably in would otherwise skip that check entirely.
        if (m_pairing) {
            m_pairing->setSegments(m_renderCache->segments(), m_info.fps);
            m_pairingRevision = m_renderCache->revision();
        }
    }

    statusBar()->showMessage(
        tr("Converted %1 s in %2 s · kept for playback")
            .arg(m_pendingRange.second - m_pendingRange.first, 0, 'f', 1)
            .arg(result.elapsedSeconds, 0, 'f', 1),
        12000);

    // The next block, if the session is still on. Deferred by a tick because
    // this runs from the job's own finished signal and startRange refuses while
    // busy() is true - the process object is cleared after the signal is emitted.
    // Service, rather than a direct startNextBlock, so the low-water decision is
    // made in exactly one place.
    if (m_neuralSession) scheduleSessionService();
}

// Launches one block. The decision about WHICH block belongs to serviceSession
// and SessionBlockPolicy; this function only starts the job and remembers what
// it asked for, so a finished signal can be matched to a range.
bool PlayerWindow::startNextBlock(double start, double end)
{
    if (!m_neuralSession || !m_renderCache || !m_renderCache->usable()) return false;
    if (m_neural.busy()) return false;
    if (!(end > start) || end - start < 0.05) return false;

    const QString segmentPath = m_renderCache->directory() +
                                QStringLiteral("/segments/%1-%2.mp4")
                                    .arg(int64_t(start * 1000.0))
                                    .arg(int64_t(end * 1000.0));

    QString error;
    const NeuralFeature feature = m_sessionFeature;
    if (!m_neural.startRange(m_currentPath, feature, start, end, segmentPath, m_info.fps,
                             &error)) {
        // A block that cannot start ends the session rather than retrying every
        // tick: the reason is usually the CLI or the weights having moved, and
        // repeating it 60 times a second would be worse than stopping.
        endSession(tr("Conversion stopped: %1").arg(error), false);
        return false;
    }

    m_pendingRange = {start, end};
    m_pendingRangeValid = true;
    m_sessionJobHead = start;

    const ToolbarAction pill = feature == NeuralFeature::FrameGeneration
                                   ? ToolbarAction::ToggleFrameGeneration
                                   : ToolbarAction::ToggleNeuralRendering;
    m_toolbar->setWorking(pill, true, -1.0f);
    if (m_menu) m_menu->setSessionActive(true);
    syncMenuState();
    statusBar()->showMessage(
        tr("Converting %1–%2 s").arg(start, 0, 'f', 0).arg(end, 0, 'f', 0));
    return true;
}

// One place to stop a session. A cancel, a failure, a cleared cache, the end of
// the film and the menu's toggle all go through here so none of them leaves an
// armed session behind for the next tick to restart.
void PlayerWindow::endSession(const QString& message, bool cancelRunning)
{
    m_neuralSession = false;
    m_sessionRetargetPending = false;
    m_sessionSeekPending = false;
    m_sessionAttached = false;
    m_sessionBehindNotified = false;
    m_pendingRangeValid = false;
    m_sessionJobHead = 0.0;

    // cancel() does not emit finished, so a running block's output is discarded
    // by design: a stopped session means the viewer does not want more GPU work.
    if (cancelRunning && m_neural.busy()) m_neural.cancel();

    m_toolbar->setWorking(ToolbarAction::ToggleFrameGeneration, false);
    m_toolbar->setWorking(ToolbarAction::ToggleNeuralRendering, false);
    if (m_menu) m_menu->setSessionActive(false);
    if (!message.isEmpty()) statusBar()->showMessage(message, 10000);
    syncMenuState();
}

void PlayerWindow::scheduleSessionService()
{
    QTimer::singleShot(0, this, [this]() { serviceSession(); });
}

// The timeline's bottom lane: what has been converted, in teal, over the blue of
// what has been played. Empty when nothing has, which is the honest state rather
// than a green bar claiming otherwise.
void PlayerWindow::updateTimelineCoverage()
{
    if (!m_timeline) return;
    if (!m_renderCache) {
        m_timeline->setCoverage({});
        return;
    }

    QVector<std::pair<double, double>> spans;
    for (const CoverageSpan& span : m_renderCache->coverage()) {
        spans.append({TicksToSeconds(span.start100ns), TicksToSeconds(span.end100ns)});
    }
    m_timeline->setCoverage(spans);
}

// Settings that survive a restart. The Windows build keeps a DLSSVideoPlayer.ini
// with a [Playback] NeuralWindowSeconds key and a [Comparison] view; QSettings is
// the macOS equivalent, and the same four facts matter here.
void PlayerWindow::loadSettings()
{
    QSettings settings;
    double block = settings.value(QStringLiteral("neural/blockSeconds"),
                                  session_block::kDefaultBlockSeconds).toDouble();
    bool isRung = false;
    for (int i = 0; i < session_block::kBlockChoiceCount; ++i) {
        if (qFuzzyCompare(block, session_block::kBlockChoices[i])) isRung = true;
    }
    if (!isRung) block = session_block::kDefaultBlockSeconds;
    m_sessionBlockSeconds = block;
    m_sessionLowWaterSeconds = block * session_block::kLowWaterFraction;

    m_view = settings.value(QStringLiteral("comparison/view"), 0).toInt() == 1
                 ? PlaybackView::Neural : PlaybackView::Original;
    m_activeKind = settings.value(QStringLiteral("neural/kind"), 0).toInt() == 1
                       ? RenderKind::FrameGeneration : RenderKind::NeuralRendering;
    m_savedVolume = std::clamp(
        float(settings.value(QStringLiteral("audio/volume"), 1.0).toDouble()), 0.0f, 1.0f);

    // The comparison settings the Windows build keeps in [Comparison]. The Mix
    // and gains are snapped to their ladders on load, so a hand-edited value
    // cannot ask for a gain the UI never offers.
    if (settings.contains(QStringLiteral("comparison/mode"))) {
        m_compareParams.mode = compare_compose::FromIndex(
            settings.value(QStringLiteral("comparison/mode")).toInt());
        // A saved single-frame mode IS the old view setting, so the two stay
        // in step on the first run after upgrading.
        if (m_compareParams.mode == compare_compose::Mode::Neural) m_view = PlaybackView::Neural;
        else if (m_compareParams.mode == compare_compose::Mode::Original)
            m_view = PlaybackView::Original;
    } else {
        m_compareParams.mode = m_view == PlaybackView::Neural
                                   ? compare_compose::Mode::Neural
                                   : compare_compose::Mode::Original;
    }
    m_compareParams.mix = std::clamp(
        float(settings.value(QStringLiteral("comparison/mix"), 1.0).toDouble()), 0.0f, 2.0f);
    m_compareParams.gain = compare_settings::LoadDifferenceGain(
        float(settings.value(QStringLiteral("comparison/gain"),
                             compare_settings::kDefaultDifferenceGain).toDouble()));
    m_compareParams.divider = std::clamp(
        float(settings.value(QStringLiteral("comparison/divider"), 0.5).toDouble()), 0.02f, 0.98f);
    m_compareParams.secondMix = compare_settings::LoadSecondMix(
        float(settings.value(QStringLiteral("comparison/secondMix"),
                             compare_settings::kDefaultSecondMix).toDouble()));
    m_compareParams.swapped = settings.value(QStringLiteral("comparison/swapped"), false).toBool();
    m_compareParams.lumaOnly = settings.value(QStringLiteral("comparison/lumaOnly"), false).toBool();

    if (m_menu) {
        m_menu->setRenderWindow(m_sessionBlockSeconds);
        m_menu->setCompareMode(compare_compose::IndexOf(m_compareParams.mode));
        m_menu->setCompareOptions(m_compareParams.swapped, m_compareParams.lumaOnly,
                                  m_compareParams.mix, m_compareParams.gain);
        m_menu->setSecondMix(m_compareParams.secondMix);
    }
    if (m_surface) {
        m_surface->setCompareParams(m_compareParams);
        m_surface->setComparison(false, m_compareParams.mode == compare_compose::Mode::Neural);
    }
    if (m_toolbar) m_toolbar->setVolume(double(m_savedVolume));
}

void PlayerWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("neural/blockSeconds"), m_sessionBlockSeconds);
    settings.setValue(QStringLiteral("neural/kind"),
                      m_activeKind == RenderKind::FrameGeneration ? 1 : 0);
    settings.setValue(QStringLiteral("comparison/view"),
                      m_view == PlaybackView::Neural ? 1 : 0);
    settings.setValue(QStringLiteral("comparison/mode"),
                      compare_compose::IndexOf(m_compareParams.mode));
    settings.setValue(QStringLiteral("comparison/mix"), double(m_compareParams.mix));
    settings.setValue(QStringLiteral("comparison/gain"), double(m_compareParams.gain));
    settings.setValue(QStringLiteral("comparison/divider"), double(m_compareParams.divider));
    settings.setValue(QStringLiteral("comparison/secondMix"), double(m_compareParams.secondMix));
    settings.setValue(QStringLiteral("comparison/swapped"), m_compareParams.swapped);
    settings.setValue(QStringLiteral("comparison/lumaOnly"), m_compareParams.lumaOnly);
    settings.setValue(QStringLiteral("audio/volume"), double(m_savedVolume));
}

void PlayerWindow::setRenderWindow(double seconds)
{
    if (!(seconds > 0.0)) return;
    m_sessionBlockSeconds = seconds;
    m_sessionLowWaterSeconds = seconds * session_block::kLowWaterFraction;
    saveSettings();
    if (m_menu) m_menu->setRenderWindow(seconds);
    statusBar()->showMessage(tr("Render window: %1 s per block").arg(int(seconds)), 5000);
}

void PlayerWindow::clearConvertedCache()
{
    if (m_currentPath.isEmpty()) return;
    if (m_neural.busy()) {
        statusBar()->showMessage(tr("Finish or cancel the running conversion first."), 8000);
        return;
    }

    const QString root = RenderCache::rootDirectory();
    const int count = m_renderCache ? int(m_renderCache->segments().size()) : 0;
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Clear converted video"),
        tr("Delete %1 converted segment(s) for %2 from:\n%3\n\n"
           "Only this player's cache directory is affected. The film will be "
           "converted again as you watch, and this cannot be undone.")
            .arg(count).arg(QFileInfo(m_currentPath).fileName(), root),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    endSession(QString(), false);
    teardownPairing();
    const bool ok = RenderCache::clearAllForSource(m_currentPath);

    m_renderCache = std::make_unique<RenderCache>(m_currentPath, m_activeKind);
    if (m_renderCache->usable()) {
        m_renderCache->load();
        m_renderCache->setSourceDuration(m_info.duration);
    } else {
        m_renderCache.reset();
    }
    m_viewAvailable = false;
    m_lastPair = PlaybackPair{};
    m_hasFrame = false;
    if (m_surface) m_surface->setComparison(false, m_view == PlaybackView::Neural);
    if (m_menu) {
        m_menu->setComparisonAvailable(false);
        m_menu->setCacheCommandsEnabled(false);
    }
    updateTimelineCoverage();
    syncMenuState();
    statusBar()->showMessage(ok ? tr("Converted video cleared.")
                                : tr("Some converted files could not be removed."),
                             8000);
}

void PlayerWindow::openCacheFolder()
{
    const QString root = RenderCache::rootDirectory();
    if (root.isEmpty()) {
        statusBar()->showMessage(tr("The cache directory is not available."), 8000);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(root));
}

void PlayerWindow::updateAvailability()
{
    ToolbarAvailability availability;
    availability.mediaLoaded = (m_source != nullptr);

    // The neural feature pills are now real: MLX-DLSS is built beside this
    // project and its weights are extracted, so frame generation and neural
    // rendering can actually run. What they are not is real-time - measured at
    // 38 ms and 95 ms per frame respectively - so pressing one starts an export
    // job rather than toggling playback. IsToolbarActionEnabled() reads these,
    // which is why the pills light up only when the weights are on disk.
    availability.rendererReady = m_neuralAvailability.canDo(NeuralFeature::NeuralRendering);
    availability.neuralRenderingAvailable = m_neuralAvailability.canDo(NeuralFeature::NeuralRendering);
    // A session - conversion that follows the playhead in blocks - is real now,
    // so the pill may offer it. It is not real-time, and the status bar says so.
    availability.neuralSessionAvailable =
        m_neuralAvailability.canDo(NeuralFeature::NeuralRendering) ||
        m_neuralAvailability.canDo(NeuralFeature::FrameGeneration);
    // Upscaling is the one feature still missing: both of its weight sources
    // (RTX VSR and DLSS SR) need a library this machine does not have.
    availability.upscalingAvailable = false;
    availability.frameGenerationAvailable = m_neuralAvailability.canDo(NeuralFeature::FrameGeneration);
    // The offline 2x export is real; LIVE interpolation during playback is not -
    // it needs a presentation path that doubles the swapchain rate.
    availability.frameGenerationConverting =
        m_neural.busy() && m_sessionFeature == NeuralFeature::FrameGeneration;
    availability.playbackFrameGenerationAvailable = false;
    // Sidecar text subtitles are real now; embedded/bitmap streams are not.
    availability.subtitlesAvailable = m_subtitles && m_subtitles->loaded();
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

    // The feature pills keep their Windows labels. The two range renders are
    // live when their weights are installed; upscaling and live interpolation
    // stay disabled because their engines are not in this build.
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
    case Qt::Key_D:
        // The Windows build's compare key. Unmodified only: Ctrl+D belongs to
        // the menu's "DLSS 5" row and must keep working as its own command.
        if (event->modifiers() == Qt::NoModifier) {
            toggleComparisonView();
            return;
        }
        break;
    case Qt::Key_A:
        m_surface->setFill(!m_surface->fill());
        return;
    case Qt::Key_M:
        if (m_audio) {
            m_audio->SetVolume(m_audio->Volume() > 0.0f ? 0.0f : 1.0f);
            m_toolbar->setActive(ToolbarAction::Mute, m_audio->Volume() == 0.0f);
        }
        return;
    case Qt::Key_C:
        // The Windows build's mode cycle. Shift walks backwards.
        stepCompareMode(event->modifiers() & Qt::ShiftModifier ? -1 : +1);
        return;
    case Qt::Key_X:
        if (event->modifiers() == Qt::NoModifier) {
            m_compareParams.swapped = !m_compareParams.swapped;
            applyCompareParams(true);
            return;
        }
        break;
    case Qt::Key_BracketLeft:
        if (event->modifiers() & Qt::ShiftModifier) stepGain(-1);
        else stepMix(-0.1f);
        return;
    case Qt::Key_BracketRight:
        if (event->modifiers() & Qt::ShiftModifier) stepGain(+1);
        else stepMix(+0.1f);
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
