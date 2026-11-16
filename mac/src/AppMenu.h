#pragma once

// The menu bar.
//
// The Windows build builds an HMENU in AppMenu.cpp: 949 lines, eleven submenus,
// about a hundred and twenty commands. Most of them switch on features this port
// does not have - the eight spatial comparison modes, RTX VSR, upscaling engines,
// subtitle translation, export. Recreating those as greyed rows would be a menu
// that is mostly a list of things that do not work.
//
// So the shape is the Windows menu's, and the contents are what this port can
// actually do:
//
//   File      open, recent, exit
//   Playback  play/pause, stop, ±10 s, mute, timecode
//   Video     aspect fit/fill/pixels, fullscreen
//   View      the five image views (greyed: no renderer layers)
//   Compare   DLSS 5 / Original - which frame of the pair is on screen
//   Neural    the two range renders, live-session toggle, block-size rungs,
//             clear cache, open cache folder, cancel running job
//   Help      about, keyboard shortcuts
//
// The groups and the separators follow AppMenu.cpp's rule - separators between
// logical groups, two to seven items per group - because that reasoning applies
// whether or not the features behind them exist.
//
// On macOS a QMenuBar with no parent window is the system menu bar at the top of
// the screen, which is where a Mac user looks for File and Playback. That is not
// a deviation from the Windows layout; it is what the platform does with the same
// structure.

#include <QObject>
#include <QStringList>

class QAction;
class QActionGroup;
class QMenu;
class QMenuBar;

// The commands the port has, by the names AppMenu's command ids would have. The
// numbering follows the Windows enum's order where a command exists, so a reader
// comparing the two files can find counterparts.
enum class MenuCommand {
    Open,
    OpenYouTube,
    RecentFirst,          // + index
    SaveComparisonImage = 40,
    Exit = 50,

    PlayPause = 100,
    Stop,
    Back10,
    Forward10,
    Mute,
    MarkIn,
    MarkOut,
    ClearMarks,
    GotoTimecode,

    AspectFit = 200,
    AspectFill,
    AspectPixels,
    Fullscreen,

    ViewFinal = 300,
    ViewInput,
    ViewMV,
    ViewDepth,
    ViewFrameGenerated,

    // The comparison view: which of the pair is on screen. Two rows in a mutually
    // exclusive group, the same two the Windows build's Compare menu leads with -
    // `menu.compare_neural` reads "DLSS 5" there, and the pair is what a viewer
    // presses to see what the render did to a frame they are looking at.
    //
    // Distinct from the View rows above, which name renderer layers this port does
    // not have. These name the two frames the pairing already holds.
    CompareDlss5 = 320,
    CompareOriginal,
    // One command plus the mode index; the eight modes are a radio group.
    CompareMode = 325,
    CompareMixLess = 334,
    CompareMixMore,
    CompareGainLess,
    CompareGainMore,
    CompareSwap,
    CompareNextMode,
    ComparePrevMode,
    CompareLuma,
    CompareSecondMix = 345,   // + index into compare_settings::kSecondMixes

    // The neural exports. One menu of their own rather than entries scattered
    // through Video, because they are the only two things in this port that
    // produce a new file rather than change how the current one is shown.
    FrameGeneration = 350,
    NeuralRendering,
    CancelNeuralJob,
    // The live session: a checked row means the player keeps converting blocks
    // as the viewer watches, which is what the Windows build's neural toggle
    // does. Stopping it leaves every finished segment in the cache.
    LiveSession = 360,
    ClearNeuralCache,
    OpenCacheFolder,
    // One command plus the index of the chosen rung (0..4), the way the recent
    // files carry theirs.
    RenderWindow = 370,

    // Rebuilt from ffprobe on every load. Index is the position among AUDIO
    // streams, which is what `ffmpeg -map 0:a:N` takes.
    AudioTrack = 380,
    // Rebuilt from the sidecar files beside the video. Index into the list the
    // window passes to setSubtitleFiles; -1 is not passed, Off has its own id.
    SubtitlesOff = 390,
    SubtitleFile = 391,
    LoadSubtitleFile = 392,

    About = 400,
    Shortcuts,
    OpenSettingsFile,
};

class AppMenu : public QObject {
    Q_OBJECT

public:
    // `window` is the window the menu acts on; it is not reparented, so the menu
    // bar is owned by this object rather than by the window's layout.
    explicit AppMenu(QObject* parent = nullptr);
    ~AppMenu() override;

    QMenuBar* bar() { return m_bar; }

    // Rebuilds the recent-files submenu from `paths`, newest first.
    void setRecentFiles(const QStringList& paths);

    // Reflects the player's state in the checkmarks and the enabled state. Cheap
    // enough to call on every state change, which is what the Windows build's
    // SyncFeatureMenuState does.
    void setMediaLoaded(bool loaded);
    void setPlaying(bool playing);
    void setMuted(bool muted);
    void setAspect(int aspect);            // 0 fit, 1 fill, 2 pixels
    void setFullscreen(bool fullscreen);

    // Whether the neural exports can run, and whether one is running now. The
    // rows are enabled from this rather than from setMediaLoaded, because a
    // loaded file is not enough: the weights have to be on disk too.
    void setNeuralAvailability(bool frameGeneration, bool neuralRendering);
    void setNeuralBusy(bool busy);
    void setSessionActive(bool active);
    void setRenderWindow(double seconds);
    void setCacheCommandsEnabled(bool enabled);

    // Which of the pair is on screen, and whether there is a pair to choose
    // between. Without rendered ground under the playhead there is nothing to
    // compare, so both rows go quiet rather than offering a switch that would not
    // change the picture.
    void setComparisonAvailable(bool available);
    void setComparisonView(bool dlss5);

    // The spatial comparison views. `mode` is a compare_compose::Mode value;
    // the menu only needs to tick a row, so it takes the int rather than
    // depending on the policy header.
    void setCompareMode(int mode);
    void setCompareOptions(bool swapped, bool lumaOnly, float mix, float gain);
    void setSecondMix(float value);

    // Rebuilt on every file load. `descriptions` and `paths` are in the same
    // order the window will act on them, so an index is all the signal carries.
    void setAudioTracks(const QStringList& descriptions, int selected);
    void setSubtitleFiles(const QStringList& paths, int selected);

signals:
    void commandTriggered(MenuCommand command, int index);
    void recentFileChosen(const QString& path);

private:
    void build();
    void updateNeuralEnabled();
    QAction* add(QMenu* menu, MenuCommand command, const char* key, int index = 0,
                 const QString& shortcut = QString());

    QMenuBar* m_bar = nullptr;

    QMenu* m_recentMenu = nullptr;
    QAction* m_youtubeAction = nullptr;

    QAction* m_playPause = nullptr;
    QAction* m_stop = nullptr;
    QAction* m_markIn = nullptr;
    QAction* m_markOut = nullptr;
    QAction* m_clearMarks = nullptr;
    QAction* m_gotoTimecode = nullptr;
    QAction* m_mute = nullptr;

    QAction* m_aspectFit = nullptr;
    QAction* m_aspectFill = nullptr;
    QAction* m_aspectPixels = nullptr;
    QAction* m_fullscreen = nullptr;

    QAction* m_saveComparison = nullptr;
    QAction* m_frameGeneration = nullptr;
    QAction* m_neuralRendering = nullptr;
    QAction* m_cancelNeural = nullptr;
    QAction* m_liveSession = nullptr;
    QAction* m_clearCache = nullptr;
    QAction* m_openCacheFolder = nullptr;
    QAction* m_renderWindows[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};

    QAction* m_compareDlss5 = nullptr;
    QAction* m_compareOriginal = nullptr;
    QAction* m_compareModes[8] = {nullptr};
    QAction* m_compareSwap = nullptr;
    QAction* m_compareLuma = nullptr;
    QAction* m_compareMixValue = nullptr;
    QAction* m_compareGainValue = nullptr;
    QAction* m_compareSecondMix[5] = {nullptr};
    QMenu* m_audioTracksMenu = nullptr;
    QMenu* m_subtitlesMenu = nullptr;
    QActionGroup* m_audioGroup = nullptr;
    QActionGroup* m_subtitleGroup = nullptr;

    bool m_comparisonAvailable = false;
    bool m_frameGenerationAvailable = false;
    bool m_neuralRenderingAvailable = false;
    bool m_neuralBusy = false;
    bool m_mediaLoaded = false;
    bool m_neuralSessionActive = false;
    bool m_cacheCommandsAllowed = false;
};
