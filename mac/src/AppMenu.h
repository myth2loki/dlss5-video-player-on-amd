#pragma once

// The menu bar.
//
// The Windows build builds an HMENU in AppMenu.cpp: 949 lines, eleven submenus,
// about a hundred and twenty commands, of which the great majority switch on
// features this port does not have - neural rendering, the eight comparison
// modes, RTX VSR, upscaling engines, subtitle translation, export. Recreating
// those as greyed rows would be a menu that is mostly a list of things that do
// not work, which is worse than a shorter menu that does.
//
// So the shape is the Windows menu's, and the contents are what this port can
// actually do:
//
//   File      open, recent, exit                      (no export: no renderer)
//   Playback  play/pause, stop, ±10 s, mute, marks, timecode
//   Video     aspect fit/fill/pixels, fullscreen      (no compare, no DLSS)
//   View      the five image views                    (greyed: nothing renders)
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

signals:
    void commandTriggered(MenuCommand command, int index);
    void recentFileChosen(const QString& path);

private:
    void build();
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
};
