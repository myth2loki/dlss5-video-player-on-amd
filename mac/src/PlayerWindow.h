#pragma once

// The player window, assembled from the pieces proved in phases 1 and 2.
//
// What this class is: a QMainWindow that owns the two engines (the ffmpeg video
// pipe and the CoreAudio sink), drives them from one clock, and lays out a
// video surface, a toolbar and a timeline. On top of that sits the ported
// Windows neural pipeline: a per-range renderer, a cache of converted segments,
// a pairing of two decoders, a DLSS 5 / Original switch and a live session that
// keeps converting blocks while the viewer watches.
//
// What it is still not: the Windows player's full renderer. Subtitles, HDR,
// the eight spatial comparison modes and live frame interpolation need GPU
// presentation code this port does not have. The decisions that do not need a
// renderer - coverage, scheduling, caching, pairing - do live here, and their
// arithmetic lives in mac/policy/ rather than in this window.

#include "AudioPlayer.h"
#include "AppMenu.h"
#include "CompareCompose.h"
#include "FfmpegPipeSource.h"
#include "NeuralProcessor.h"
#include "PlaybackClock.h"
#include "RenderCache.h"
#include "SegmentPlayback.h"
#include "SubtitleTrack.h"
#include "UiLayout.h"

#include <QMainWindow>
#include <QTimer>

#include <memory>

class VideoSurface;
class ToolbarWidget;
class TimelineWidget;

class PlayerWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit PlayerWindow(QWidget* parent = nullptr);
    ~PlayerWindow() override;

    // Opens `path` and starts playing it. False when the source cannot be
    // probed, in which case the window says so rather than showing black.
    bool open(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onTick();
    void onToolbarAction(ToolbarAction action);
    void onMenuCommand(MenuCommand command, int index);
    void onRecentFile(const QString& path);

private:
    void togglePlayPause();
    void seekBy(double seconds);
    void updateAvailability();
    void updateLabels();
    void pumpVideo();
    void rememberRecent(const QString& path);
    void syncMenuState();

    // Runs one of the neural exports on the current file and loads the result
    // when it finishes. `neural` owns the child process; this only wires its
    // signals to the status bar and the progress pill.
    void runNeuralJob(NeuralFeature feature);
    void onNeuralProgress(double fraction, const QString& detail);
    void onNeuralFinished(const NeuralJobResult& result);
    void updateTimelineCoverage();

    // The read path when rendered segments exist. `setupPairing` borrows the
    // window's own decoder as the original member and builds a segment factory;
    // `teardownPairing` releases both.
    void pumpPaired();
    void setupPairing();
    void teardownPairing();

    // Draws one pair according to m_view. Separate from the read so a paused
    // player can show the other member of the pair it already holds - which is what
    // a paused comparison is.
    void presentPair(const PlaybackPair& pair);

    // The live session: convert, play, convert the next block, until the film ends
    // or the viewer turns it off. `serviceSession` runs every tick and asks the
    // ported policy both when and where the next block goes; `startNextBlock`
    // only launches it.
    void serviceSession();
    bool startNextBlock(double start, double end);
    void endSession(const QString& message, bool cancelRunning);
    void scheduleSessionService();

    // Settings and the cache's two menu commands.
    void loadSettings();
    void saveSettings() const;
    void setRenderWindow(double seconds);
    void clearConvertedCache();
    void openCacheFolder();

    // Moves the clock, both decoders and the pairing to `seconds`. Every seek goes
    // through it, because a seek that moves the pipe but not the pairing leaves the
    // pair pointing at where the viewer was.
    void seekPlayback(double seconds);

    // Which of the pair the surface shows, and the keyboard's swap. Neither seeks:
    // both frames are already held, so the change is one read away.
    void setComparisonView(PlaybackView view);
    void toggleComparisonView();

    // The spatial comparison views. One params struct is the whole state; the
    // surface reads it on every paint and the menu tick follows it.
    void applyCompareParams(bool persist);
    void setCompareMode(compare_compose::Mode mode);
    void stepCompareMode(int direction);
    void stepMix(float delta);
    void stepGain(int direction);
    void saveComparisonImage();

    // Media lists. Both are rebuilt on open; the audio one also on a track
    // switch, because the checkmark has to follow the stream actually playing.
    void setAudioTrack(int index);
    void refreshSubtitle(double position);
    void setSubtitleIndex(int index);
    void loadSubtitleFile(const QString& path);

    std::unique_ptr<FfmpegPipeSource> m_source;
    std::unique_ptr<AudioPlayer> m_audio;
    PlaybackClock m_clock;
    std::unique_ptr<AppMenu> m_menu;
    NeuralProcessor m_neural;
    // Probed once and refreshed only when a job fails or the file changes, because
    // each probe stats four paths and the availability is asked for on every menu
    // sync.
    NeuralAvailability m_neuralAvailability;
    QString m_currentPath;

    // The cache entry for the open file, opened when a job first runs. Nullable
    // because a viewer who never presses a neural command should pay nothing for
    // the feature existing.
    std::unique_ptr<RenderCache> m_renderCache;

    // The range the running job was asked for, so the finished segment can be
    // recorded against the right part of the timeline.
    std::pair<double, double> m_pendingRange{0.0, 0.0};

    // The pairing, built on first use. `m_borrowedOriginal` wraps the window's own
    // decoder so the pairing can drive it without opening a second one, and is
    // declared BEFORE the pairing because the pairing holds a reference to it and
    // must be destroyed first.
    std::unique_ptr<BorrowedFrameSource> m_borrowedOriginal;
    std::unique_ptr<SegmentPlayback> m_pairing;
    // The cache revision the pairing was last given, so segments that arrive during
    // playback are picked up without rebuilding it.
    int m_pairingRevision = -1;
    QString m_lastPairFault;

    // Which frame the surface shows when both are available. Persists across seeks
    // and across segments: a viewer who asked for the rendered picture keeps asking
    // for it, and only the two menu rows change it.
    PlaybackView m_view = PlaybackView::Original;
    // Whether the last read had both halves, which is what the comparison rows are
    // enabled from. Tracked here so the menu is only touched when it changes.
    bool m_viewAvailable = false;

    // The pair currently on screen, kept so a pause can re-present it under the
    // other view. Copying two frames per presented frame would be wasteful; this
    // copies them once per READ, which is what the surface needs anyway.
    PlaybackPair m_lastPair;

    // Spatial comparison state. The surface holds a copy so its paint path is
    // self-contained; this one is what the menu and QSettings talk to.
    compare_compose::Params m_compareParams;

    // Audio tracks and subtitle sidecars for the open file.
    QStringList m_audioTrackLabels;
    int m_audioTrackIndex = -1;
    std::unique_ptr<SubtitleTrack> m_subtitles;
    QStringList m_subtitlePaths;
    int m_subtitleIndex = -1;

    // The live session. On while a conversion session is following the viewer;
    // `m_sessionStart` is where it began, which the policy measures the range from.
    bool m_neuralSession = false;
    double m_sessionStart = 0.0;
    NeuralFeature m_sessionFeature = NeuralFeature::NeuralRendering;

    // A seek that happened while a block was running. The running job is taken
    // away only if the ported ShouldRetarget says the playhead now needs a
    // different hole; the flag is consumed on the next tick.
    bool m_sessionSeekPending = false;
    bool m_sessionRetargetPending = false;
    // How far the running job has rendered inside its own block, from the CLI's
    // progress. ShouldRetarget needs this rather than the cache's head: a job
    // that has nearly finished its hole must not be cancelled for a seek.
    double m_sessionJobHead = 0.0;
    // Whether the picture is currently on converted ground. ShouldAttach drives
    // it; it is what keeps the session's decisions about the buffer honest.
    bool m_sessionAttached = false;
    bool m_sessionBehindNotified = false;
    // A finished signal that arrives after a cancel or a file change belongs to
    // a job nobody is waiting for. Cleared by endSession/open, set on launch.
    bool m_pendingRangeValid = false;

    // Block sizing, persisted in QSettings. The Windows build reads these from
    // its ini and offers a menu of rungs; this port does the same with QSettings.
    double m_sessionBlockSeconds = 30.0;
    double m_sessionLowWaterSeconds = 15.0;
    RenderKind m_activeKind = RenderKind::NeuralRendering;
    float m_savedVolume = 1.0f;

    VideoSurface* m_surface = nullptr;
    ToolbarWidget* m_toolbar = nullptr;
    TimelineWidget* m_timeline = nullptr;

    // The presentation timer. 60 Hz rather than the frame rate: the clock
    // decides which frame belongs on screen, and a timer that matches the
    // source rate would drift against it.
    QTimer m_timer;

    MediaInfo m_info;
    VideoFrame m_current;
    bool m_hasFrame = false;
    bool m_audioDriven = false;
};
