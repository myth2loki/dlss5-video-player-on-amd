#include "AppMenu.h"

#include "Localization.h"

#include <QAction>
#include <QActionGroup>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>

namespace {

// The Windows menu's shortcut column. macOS takes these from the platform menu
// bar, which shows them beside the item; the strings are Qt's own notation so
// they render as the platform's glyphs (⌘, ⌥, ⇧).
struct ShortcutSpec {
    MenuCommand command;
    const char* sequence;
};

// Only the ones AppMenu.cpp documents as reachable by key. The transport keys
// are also handled by the window itself, because a Mac user expects space to
// play from anywhere rather than only from the menu.
constexpr ShortcutSpec kShortcuts[] = {
    {MenuCommand::Open, "Ctrl+O"},
    {MenuCommand::PlayPause, "Space"},
    {MenuCommand::Stop, "Ctrl+."},
    {MenuCommand::Back10, "Left"},
    {MenuCommand::Forward10, "Right"},
    {MenuCommand::Mute, "Ctrl+M"},
    {MenuCommand::MarkIn, "I"},
    {MenuCommand::MarkOut, "O"},
    {MenuCommand::ClearMarks, "Ctrl+Shift+M"},
    {MenuCommand::GotoTimecode, "Ctrl+G"},
    {MenuCommand::AspectFit, "Ctrl+1"},
    {MenuCommand::AspectFill, "Ctrl+2"},
    {MenuCommand::AspectPixels, "Ctrl+3"},
    {MenuCommand::Fullscreen, "Ctrl+F"},
};

QString ShortcutFor(MenuCommand command)
{
    for (const auto& spec : kShortcuts)
        if (spec.command == command) return QString::fromUtf8(spec.sequence);
    return QString();
}

// The Windows build's ids for the recent-file rows, which QAction needs as data
// rather than as an id. Kept as a property so the handler can tell a recent
// entry from any other action.
constexpr const char* kRecentPathProperty = "recentPath";

} // namespace

AppMenu::AppMenu(QObject* parent) : QObject(parent)
{
    build();
}

AppMenu::~AppMenu() = default;

QAction* AppMenu::add(QMenu* menu, MenuCommand command, const char* key, int index,
                      const QString& shortcut)
{
    const Localizer localizer;
    const std::wstring wideKey = QString::fromUtf8(key).toStdWString();
    const QString text = QString::fromStdWString(localizer.Get(wideKey.c_str()));

    auto* action = new QAction(text, this);
    if (!shortcut.isEmpty()) {
        action->setShortcut(QKeySequence(shortcut));
        // Menu-bar shortcuts on macOS are delivered by the system, and a Window
        // shortcut would shadow the window's own key handling.
        action->setShortcutContext(Qt::WindowShortcut);
    }
    action->setData(int(command) + index);
    connect(action, &QAction::triggered, this,
            [this, command, index] { emit commandTriggered(command, index); });
    menu->addAction(action);
    return action;
}

void AppMenu::build()
{
    const Localizer localizer;
    m_bar = new QMenuBar(nullptr);

    // ---- File --------------------------------------------------------------
    QMenu* file = m_bar->addMenu(QStringLiteral("&File"));
    add(file, MenuCommand::Open, "menu.open", 0, ShortcutFor(MenuCommand::Open));

    // YouTube is a spinner in the Windows build too: it resolves a URL and then
    // downloads. This port has neither, so the row exists and says so rather
    // than starting a dialog that cannot finish.
    m_youtubeAction = add(file, MenuCommand::OpenYouTube, "menu.open_youtube");
    m_youtubeAction->setEnabled(false);
    m_youtubeAction->setToolTip(QStringLiteral("Not available in this build: no network resolver."));

    m_recentMenu = file->addMenu(QStringLiteral("Recent videos"));
    setRecentFiles({});

    file->addSeparator();
    // A picture of what is on screen is the one thing File writes rather than
    // opens, and it is a comparison image - which needs two images to compare.
    m_saveComparison = add(file, MenuCommand::SaveComparisonImage, "menu.save_comparison");
    m_saveComparison->setEnabled(false);
    m_saveComparison->setToolTip(
        QStringLiteral("Not available in this build: nothing is rendered to compare against."));

    file->addSeparator();
    add(file, MenuCommand::Exit, "menu.exit");

    // ---- Playback ----------------------------------------------------------
    QMenu* play = m_bar->addMenu(QStringLiteral("&Playback"));
    m_playPause = add(play, MenuCommand::PlayPause, "menu.playpause", 0,
                      ShortcutFor(MenuCommand::PlayPause));
    m_stop = add(play, MenuCommand::Stop, "menu.stop", 0, ShortcutFor(MenuCommand::Stop));
    play->addSeparator();
    add(play, MenuCommand::Back10, "menu.back10", 0, ShortcutFor(MenuCommand::Back10));
    add(play, MenuCommand::Forward10, "menu.forward10", 0, ShortcutFor(MenuCommand::Forward10));
    play->addSeparator();
    m_mute = add(play, MenuCommand::Mute, "menu.mute", 0, ShortcutFor(MenuCommand::Mute));
    m_mute->setCheckable(true);

    // Audio tracks: the Windows menu builds this list from ffprobe and rebuilds
    // it on every load. This port reads the tracks too, so the submenu is real -
    // it is filled by the window, not here, because which tracks exist is a
    // property of the file that is open.
    QMenu* audioTracks = play->addMenu(QString::fromStdWString(localizer.Get(L"menu.audio")));
    audioTracks->setObjectName(QStringLiteral("audioTracks"));
    QAction* noTracks = audioTracks->addAction(QStringLiteral("No audio tracks"));
    noTracks->setEnabled(false);

    play->addSeparator();
    // The range tools. Mark in/out is what the timeline's top lane draws, and the
    // port has the lane; it does not yet have the three commands behind it, so
    // they are present and greyed with the reason stated.
    m_markIn = add(play, MenuCommand::MarkIn, "menu.mark_in", 0, ShortcutFor(MenuCommand::MarkIn));
    m_markOut = add(play, MenuCommand::MarkOut, "menu.mark_out", 0, ShortcutFor(MenuCommand::MarkOut));
    m_clearMarks = add(play, MenuCommand::ClearMarks, "menu.clear_marks", 0,
                       ShortcutFor(MenuCommand::ClearMarks));
    m_gotoTimecode = add(play, MenuCommand::GotoTimecode, "menu.goto_timecode", 0,
                         ShortcutFor(MenuCommand::GotoTimecode));
    for (QAction* action : {m_markIn, m_markOut, m_clearMarks}) {
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Not available in this build: no export range."));
    }

    // ---- Video -------------------------------------------------------------
    QMenu* video = m_bar->addMenu(QStringLiteral("&Video"));
    auto* aspectGroup = new QActionGroup(this);
    aspectGroup->setExclusive(true);
    m_aspectFit = add(video, MenuCommand::AspectFit, "menu.aspectfit", 0, ShortcutFor(MenuCommand::AspectFit));
    m_aspectFill = add(video, MenuCommand::AspectFill, "menu.aspectfill", 0, ShortcutFor(MenuCommand::AspectFill));
    m_aspectPixels = add(video, MenuCommand::AspectPixels, "menu.aspect_pixels", 0,
                         ShortcutFor(MenuCommand::AspectPixels));
    for (QAction* action : {m_aspectFit, m_aspectFill, m_aspectPixels}) {
        action->setCheckable(true);
        aspectGroup->addAction(action);
    }
    m_aspectFit->setChecked(true);

    video->addSeparator();
    m_fullscreen = add(video, MenuCommand::Fullscreen, "menu.fullscreen", 0,
                       ShortcutFor(MenuCommand::Fullscreen));
    m_fullscreen->setCheckable(true);

    // ---- View --------------------------------------------------------------
    //
    // The five debug views are one radio group, exactly as AppMenu.cpp builds
    // them. They are greyed here for the honest reason: a view is a picture the
    // renderer produced, and this port has no renderer. The rows stay because a
    // reader of the Windows player looks for them in this menu, and their absence
    // would read as a missing feature rather than as an absent engine.
    QMenu* view = m_bar->addMenu(QStringLiteral("&View"));
    auto* viewGroup = new QActionGroup(this);
    viewGroup->setExclusive(true);
    struct ViewRow {
        MenuCommand command;
        const char* key;
        const char* shortcut;
    };
    constexpr ViewRow kViews[] = {
        {MenuCommand::ViewFinal, "menu.final", "1"},
        {MenuCommand::ViewInput, "menu.input", "2"},
        {MenuCommand::ViewMV, "menu.mv", "3"},
        {MenuCommand::ViewDepth, "menu.depth", "4"},
        {MenuCommand::ViewFrameGenerated, "menu.generated", "5"},
    };
    for (const auto& row : kViews) {
        // No shortcut: the number keys are the window's, and a menu shortcut that
        // is always greyed would swallow them.
        QAction* action = add(view, row.command, row.key);
        action->setCheckable(true);
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Not available in this build: nothing is rendered."));
        viewGroup->addAction(action);
    }
    viewGroup->actions().first()->setChecked(true);

    // ---- Help --------------------------------------------------------------
    QMenu* help = m_bar->addMenu(QStringLiteral("&Help"));
    add(help, MenuCommand::Shortcuts, "menu.shortcuts");
    help->addSeparator();
    add(help, MenuCommand::About, "menu.about");

    setMediaLoaded(false);
}

void AppMenu::setRecentFiles(const QStringList& paths)
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();

    if (paths.isEmpty()) {
        // "No recent videos", greyed - the Windows build's own placeholder, kept
        // so the submenu is never an empty popup.
        QAction* none = m_recentMenu->addAction(QStringLiteral("No recent videos"));
        none->setEnabled(false);
        return;
    }

    int index = 0;
    for (const QString& path : paths) {
        auto* action = m_recentMenu->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        action->setProperty(kRecentPathProperty, path);
        connect(action, &QAction::triggered, this, [this, path] { emit recentFileChosen(path); });
        ++index;
    }
    m_recentMenu->addSeparator();
    auto* clear = m_recentMenu->addAction(QStringLiteral("Clear recent"));
    connect(clear, &QAction::triggered, this, [this] { emit recentFileChosen(QString()); });
    Q_UNUSED(index);
}

void AppMenu::setMediaLoaded(bool loaded)
{
    // AppMenu's rule: a transport command with nothing loaded is meaningless, and
    // a greyed row that explains itself is better than a live row that does
    // nothing. Open is always live; the rest follow the source.
    for (QAction* action : {m_playPause, m_stop, m_mute, m_gotoTimecode}) {
        if (action) action->setEnabled(loaded);
    }
    for (QAction* action : {m_aspectFit, m_aspectFill, m_aspectPixels, m_fullscreen}) {
        if (action) action->setEnabled(loaded);
    }
}

void AppMenu::setPlaying(bool playing)
{
    if (!m_playPause) return;
    // The label carries the state the way the toolbar's does: "Play" while
    // playing is what the Windows build avoids too.
    m_playPause->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
}

void AppMenu::setMuted(bool muted)
{
    if (m_mute) m_mute->setChecked(muted);
}

void AppMenu::setAspect(int aspect)
{
    if (aspect == 0 && m_aspectFit) m_aspectFit->setChecked(true);
    else if (aspect == 1 && m_aspectFill) m_aspectFill->setChecked(true);
    else if (aspect == 2 && m_aspectPixels) m_aspectPixels->setChecked(true);
}

void AppMenu::setFullscreen(bool fullscreen)
{
    if (m_fullscreen) m_fullscreen->setChecked(fullscreen);
}
