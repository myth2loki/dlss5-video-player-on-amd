#pragma once

// The player window, assembled from the pieces proved in phases 1 and 2.
//
// What this class is: a QMainWindow that owns the two engines (the ffmpeg video
// pipe and the CoreAudio sink), drives them from one clock, and lays out a
// video surface, a toolbar and a timeline. What it is not: a reimplementation
// of the Windows player's 16k-line PlayerApp. The features that make that app
// wide - neural rendering, the compare view, export, subtitles - are all still
// absent, because they need the render pipeline this port deliberately has not
// built.
//
// The point of this window is different. It is the answer to "can the ported
// layer drive a real Qt UI", and every coordinate and colour in it comes from
// mac/policy/ rather than from numbers written here.

#include "AudioPlayer.h"
#include "AppMenu.h"
#include "FfmpegPipeSource.h"
#include "PlaybackClock.h"
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

    std::unique_ptr<FfmpegPipeSource> m_source;
    std::unique_ptr<AudioPlayer> m_audio;
    PlaybackClock m_clock;
    std::unique_ptr<AppMenu> m_menu;

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
