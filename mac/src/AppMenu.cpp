#include "AppMenu.h"

#include "CompareCompose.h"
#include "CompareViewPolicy.h"
#include "Localization.h"
#include "SessionBlockPolicy.h"

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
    {MenuCommand::FrameGeneration, "Ctrl+Shift+F"},
    {MenuCommand::NeuralRendering, "Ctrl+Shift+N"},
    // The comparison toggle. The Windows build binds its own to D; Cmd+D is the
    // macOS duplicate shortcut in some apps, but this player has no documents, so
    // it is free here and it is the key a Windows user already presses.
    {MenuCommand::CompareDlss5, "Ctrl+D"},
    {MenuCommand::CompareOriginal, "Ctrl+Shift+D"},
    {MenuCommand::LiveSession, "Ctrl+Shift+L"},
    {MenuCommand::SaveComparisonImage, "Ctrl+Shift+S"},
    // C / X / [ / ] are handled by the window's keyPressEvent rather than as
    // QAction shortcuts: those keys belong to the picture, and a menu shortcut
    // would also fire inside the timecode dialog. Their text already carries
    // the key from the localization table.
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
    m_saveComparison = add(file, MenuCommand::SaveComparisonImage, "menu.save_comparison", 0,
                           ShortcutFor(MenuCommand::SaveComparisonImage));
    m_saveComparison->setEnabled(false);
    m_saveComparison->setToolTip(
        QStringLiteral("Save the picture as shown, including the comparison panes. "
                       "Needs a rendered frame at the playhead."));

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

    // Audio tracks and subtitles: both lists are rebuilt from the file that is
    // open, so the menu holds the QMenu and the window supplies the contents.
    m_audioTracksMenu = play->addMenu(QString::fromStdWString(localizer.Get(L"menu.audio")));
    m_audioTracksMenu->setObjectName(QStringLiteral("audioTracks"));
    m_audioGroup = new QActionGroup(this);
    m_audioGroup->setExclusive(true);
    setAudioTracks({}, -1);

    m_subtitlesMenu = play->addMenu(QString::fromStdWString(localizer.Get(L"menu.subtitles")));
    m_subtitlesMenu->setObjectName(QStringLiteral("subtitles"));
    m_subtitleGroup = new QActionGroup(this);
    m_subtitleGroup->setExclusive(true);
    setSubtitleFiles({}, -1);

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

    // ---- Compare -----------------------------------------------------------
    //
    // The Windows Compare menu's two lead rows and its eight modes. Every mode
    // composites the two frames the pairing already holds; the arithmetic is in
    // policy/CompareCompose.h and the pixels in CompareCompositor, so no second
    // renderer is needed for any of them.
    //
    // Both lead rows start disabled: a comparison needs rendered ground under
    // the playhead, and there is none until a render has run.
    QMenu* compare = m_bar->addMenu(QStringLiteral("&Compare"));
    auto* compareGroup = new QActionGroup(this);
    compareGroup->setExclusive(true);

    m_compareDlss5 = add(compare, MenuCommand::CompareDlss5, "menu.compare_neural");
    m_compareDlss5->setCheckable(true);
    m_compareDlss5->setEnabled(false);
    compareGroup->addAction(m_compareDlss5);

    m_compareOriginal = add(compare, MenuCommand::CompareOriginal, "menu.compare_original");
    m_compareOriginal->setCheckable(true);
    m_compareOriginal->setEnabled(false);
    // Original is the state the player starts in, so it is the one checked: the
    // picture is the source until a render is chosen, and a menu with neither row
    // ticked would say the player is in a third state it does not have.
    m_compareOriginal->setChecked(true);
    compareGroup->addAction(m_compareOriginal);

    compare->addSeparator();
    // The modes. One radio group, in Windows' ComparisonMode order, so a
    // viewer walking them with C sees the same sequence as the Windows build.
    auto* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    for (int i = 0; i < compare_compose::kModeCount; ++i) {
        const auto mode = compare_compose::FromIndex(i);
        const char* key = compare_compose::ModeKey(mode);
        if (mode == compare_compose::Mode::Blend) key = "menu.compare_blend";
        QAction* action = add(compare, MenuCommand::CompareMode, key, i);
        action->setCheckable(true);
        action->setEnabled(false);
        modeGroup->addAction(action);
        m_compareModes[i] = action;
    }
    m_compareModes[0]->setChecked(true);

    compare->addSeparator();
    // The two dials. The value rows are labels rather than controls: the mix
    // and the gain are stepped by [ ] and Shift+[ ], which the window handles.
    m_compareMixValue = compare->addAction(QStringLiteral("Mix 100%"));
    m_compareMixValue->setEnabled(false);
    add(compare, MenuCommand::CompareMixLess, "menu.compare_blend_less");
    add(compare, MenuCommand::CompareMixMore, "menu.compare_blend_more");
    m_compareGainValue = compare->addAction(QStringLiteral("Difference gain 4x"));
    m_compareGainValue->setEnabled(false);
    add(compare, MenuCommand::CompareGainLess, "menu.compare_difference_less");
    add(compare, MenuCommand::CompareGainMore, "menu.compare_difference_more");
    add(compare, MenuCommand::CompareNextMode, "menu.compare_next_mode");
    add(compare, MenuCommand::ComparePrevMode, "menu.compare_previous_mode");

    compare->addSeparator();
    m_compareSwap = add(compare, MenuCommand::CompareSwap, "menu.compare_swap");
    m_compareSwap->setCheckable(true);
    m_compareLuma = add(compare, MenuCommand::CompareLuma, "menu.compare_difference_luma");
    m_compareLuma->setCheckable(true);

    QMenu* secondMix = compare->addMenu(
        QString::fromStdWString(Localizer().Get(L"menu.compare_second_mix")));
    auto* secondGroup = new QActionGroup(this);
    secondGroup->setExclusive(true);
    static constexpr const char* kSecondMixKeys[] = {
        "menu.compare_second_mix_0", "menu.compare_second_mix_1", "menu.compare_second_mix_2",
        "menu.compare_second_mix_3", "menu.compare_second_mix_4"};
    for (int i = 0; i < int(compare_settings::kSecondMixes.size()); ++i) {
        QAction* action = add(secondMix, MenuCommand::CompareSecondMix,
                              kSecondMixKeys[i], i);
        action->setText(QStringLiteral("%1%").arg(
            int(std::lround(compare_settings::kSecondMixes[size_t(i)] * 100))));
        action->setCheckable(true);
        action->setEnabled(false);
        secondGroup->addAction(action);
        m_compareSecondMix[i] = action;
    }
    m_compareSecondMix[1]->setChecked(true);   // 50%, the Windows default

    // ---- Neural ------------------------------------------------------------
    //
    // The two exports this port can actually run, next to each other. The
    // Windows build's DLSS menu has eleven blocks - presets, styles, runtimes,
    // processing scale, encoders - and every one of them configures a renderer
    // this port does not have. What it does have is the two jobs themselves, so
    // that is what the menu offers.
    QMenu* neural = m_bar->addMenu(QStringLiteral("&Neural"));
    m_frameGeneration = add(neural, MenuCommand::FrameGeneration, "menu.frame_generation",
                            0, ShortcutFor(MenuCommand::FrameGeneration));
    m_neuralRendering = add(neural, MenuCommand::NeuralRendering, "menu.neural_rendering",
                            0, ShortcutFor(MenuCommand::NeuralRendering));
    neural->addSeparator();

    // A session is a toggle, not a one-shot: while it is on the player keeps
    // converting the next block as the viewer watches. Unchecking it leaves
    // every finished segment in the cache, which is the Windows build's
    // StopLiveSession behaviour.
    m_liveSession = add(neural, MenuCommand::LiveSession, "menu.live_session",
                        0, ShortcutFor(MenuCommand::LiveSession));
    m_liveSession->setCheckable(true);
    m_liveSession->setEnabled(false);

    QMenu* windowMenu = neural->addMenu(
        QString::fromStdWString(Localizer().Get(L"menu.render_window")));
    auto* windowGroup = new QActionGroup(this);
    windowGroup->setExclusive(true);
    for (int i = 0; i < session_block::kBlockChoiceCount; ++i) {
        const double seconds = session_block::kBlockChoices[i];
        QAction* action = add(windowMenu, MenuCommand::RenderWindow, "menu.render_window", i);
        action->setText(QStringLiteral("%1 s").arg(int(seconds)));
        action->setCheckable(true);
        windowGroup->addAction(action);
        m_renderWindows[i] = action;
    }
    m_renderWindows[2]->setChecked(true);   // 30 s, the default rung

    neural->addSeparator();
    m_clearCache = add(neural, MenuCommand::ClearNeuralCache, "menu.clear_neural_cache");
    m_openCacheFolder = add(neural, MenuCommand::OpenCacheFolder, "menu.open_cache_folder");
    m_clearCache->setEnabled(false);
    m_openCacheFolder->setEnabled(false);

    neural->addSeparator();
    m_cancelNeural = add(neural, MenuCommand::CancelNeuralJob, "menu.cancel_export_running");
    m_cancelNeural->setEnabled(false);

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
    m_mediaLoaded = loaded;
    updateNeuralEnabled();
}

void AppMenu::updateNeuralEnabled()
{
    const bool canConvert = m_frameGenerationAvailable || m_neuralRenderingAvailable;
    if (m_liveSession) m_liveSession->setEnabled(m_mediaLoaded && (canConvert || m_neuralSessionActive));
    if (m_clearCache) m_clearCache->setEnabled(m_mediaLoaded && m_cacheCommandsAllowed && !m_neuralBusy);
    if (m_openCacheFolder) m_openCacheFolder->setEnabled(true);
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

void AppMenu::setNeuralAvailability(bool frameGeneration, bool neuralRendering)
{
    // Remembered, because busy and available are two separate facts and an
    // enabled row needs both. Reading isEnabled() to recover the first one, as an
    // earlier version did, loses it the moment a job starts.
    m_frameGenerationAvailable = frameGeneration;
    m_neuralRenderingAvailable = neuralRendering;
    if (m_frameGeneration) m_frameGeneration->setEnabled(frameGeneration && !m_neuralBusy);
    if (m_neuralRendering) m_neuralRendering->setEnabled(neuralRendering && !m_neuralBusy);
    updateNeuralEnabled();
}

void AppMenu::setNeuralBusy(bool busy)
{
    // Both commands stop while one runs: the CLI writes an output beside the input
    // and a second job on the same file would collide with the first, and the GPU
    // is already saturated.
    m_neuralBusy = busy;
    if (m_frameGeneration) m_frameGeneration->setEnabled(m_frameGenerationAvailable && !busy);
    if (m_neuralRendering) m_neuralRendering->setEnabled(m_neuralRenderingAvailable && !busy);
    if (m_cancelNeural) m_cancelNeural->setEnabled(busy);
    updateNeuralEnabled();
}

void AppMenu::setSessionActive(bool active)
{
    m_neuralSessionActive = active;
    if (m_liveSession) m_liveSession->setChecked(active);
    updateNeuralEnabled();
}

void AppMenu::setRenderWindow(double seconds)
{
    for (int i = 0; i < session_block::kBlockChoiceCount; ++i) {
        if (m_renderWindows[i] && qFuzzyCompare(seconds, session_block::kBlockChoices[i]))
            m_renderWindows[i]->setChecked(true);
    }
}

void AppMenu::setCacheCommandsEnabled(bool enabled)
{
    // Clearing is offered only when this film actually has converted ground;
    // "Open cache folder" stays live so the viewer can see where renders live
    // even before the first one exists.
    m_cacheCommandsAllowed = enabled;
    updateNeuralEnabled();
}

void AppMenu::setComparisonAvailable(bool available)
{
    m_comparisonAvailable = available;
    if (m_compareDlss5) m_compareDlss5->setEnabled(available);
    if (m_compareOriginal) m_compareOriginal->setEnabled(available);
    if (m_saveComparison) m_saveComparison->setEnabled(available);
    for (QAction* action : m_compareModes)
        if (action) action->setEnabled(available);
    for (QAction* action : m_compareSecondMix)
        if (action) action->setEnabled(available);
    // The dials stay live even without a pair: setting a Mix before the render
    // finishes is a reasonable thing to do, and the rows are harmless when the
    // picture has only one frame.
}

void AppMenu::setCompareMode(int mode)
{
    for (int i = 0; i < compare_compose::kModeCount; ++i) {
        if (m_compareModes[i]) m_compareModes[i]->setChecked(i == mode);
    }
    // The two lead rows tick with the two single-frame modes; in a spatial
    // view neither is "the" view and both stay unchecked.
    if (m_compareDlss5) m_compareDlss5->setChecked(mode == int(compare_compose::Mode::Neural));
    if (m_compareOriginal) m_compareOriginal->setChecked(mode == int(compare_compose::Mode::Original));
}

void AppMenu::setCompareOptions(bool swapped, bool lumaOnly, float mix, float gain)
{
    if (m_compareSwap) m_compareSwap->setChecked(swapped);
    if (m_compareLuma) m_compareLuma->setChecked(lumaOnly);
    if (m_compareMixValue)
        m_compareMixValue->setText(QStringLiteral("Mix %1%").arg(int(std::lround(mix * 100))));
    if (m_compareGainValue)
        m_compareGainValue->setText(QStringLiteral("Difference gain %1x").arg(gain, 0, 'f', 0));
}

void AppMenu::setSecondMix(float value)
{
    int nearest = 1;
    float distance = 1e9f;
    for (int i = 0; i < int(compare_settings::kSecondMixes.size()); ++i) {
        const float candidate = compare_settings::kSecondMixes[size_t(i)];
        if (std::abs(candidate - value) < distance) {
            distance = std::abs(candidate - value);
            nearest = i;
        }
    }
    if (m_compareSecondMix[nearest]) m_compareSecondMix[nearest]->setChecked(true);
}

void AppMenu::setAudioTracks(const QStringList& descriptions, int selected)
{
    if (!m_audioTracksMenu) return;
    m_audioTracksMenu->clear();

    if (descriptions.isEmpty()) {
        QAction* none = m_audioTracksMenu->addAction(
            QString::fromStdWString(Localizer().Get(L"menu.audio_none")));
        none->setEnabled(false);
        return;
    }
    for (int i = 0; i < descriptions.size(); ++i) {
        QAction* action = m_audioTracksMenu->addAction(descriptions.at(i));
        action->setCheckable(true);
        action->setChecked(i == selected);
        if (m_audioGroup) m_audioGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, i] { emit commandTriggered(MenuCommand::AudioTrack, i); });
    }
}

void AppMenu::setSubtitleFiles(const QStringList& paths, int selected)
{
    if (!m_subtitlesMenu) return;
    m_subtitlesMenu->clear();
    const Localizer localizer;

    QAction* off = m_subtitlesMenu->addAction(
        QString::fromStdWString(localizer.Get(L"menu.subtitles_off")));
    off->setCheckable(true);
    off->setChecked(selected < 0);
    if (m_subtitleGroup) m_subtitleGroup->addAction(off);
    connect(off, &QAction::triggered, this,
            [this] { emit commandTriggered(MenuCommand::SubtitlesOff, 0); });

    if (paths.isEmpty()) {
        QAction* none = m_subtitlesMenu->addAction(
            QString::fromStdWString(localizer.Get(L"menu.subtitles_none")));
        none->setEnabled(false);
    } else {
        for (int i = 0; i < paths.size(); ++i) {
            QAction* action = m_subtitlesMenu->addAction(
                QString::fromStdWString(localizer.Get(L"menu.subtitles_file")) +
                QFileInfo(paths.at(i)).fileName());
            action->setToolTip(paths.at(i));
            action->setCheckable(true);
            action->setChecked(i == selected);
            if (m_subtitleGroup) m_subtitleGroup->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, i] { emit commandTriggered(MenuCommand::SubtitleFile, i); });
        }
    }
    m_subtitlesMenu->addSeparator();
    add(m_subtitlesMenu, MenuCommand::LoadSubtitleFile, "menu.subtitles_load");
}

void AppMenu::setComparisonView(bool dlss5)
{
    // Checked state only. Which frame is shown is the player's business; the menu
    // reports it so the tick and the picture cannot disagree.
    if (m_compareDlss5) m_compareDlss5->setChecked(dlss5);
    if (m_compareOriginal) m_compareOriginal->setChecked(!dlss5);
}
