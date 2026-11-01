// The Qt entry point.
//
// Deliberately thin: parse a path if there is one, hand it to PlayerWindow, run.
// Everything a player does happens in the window and the engines behind it, so
// there is nothing here to test.

#include "PlayerWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileDialog>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DLSS 5 Video Player"));
    QApplication::setApplicationVersion(QStringLiteral("0.1-macos"));

    // Fusion rather than the macOS native style. The player's chrome is drawn by
    // hand from a fixed palette; the native style would tint the frames and
    // dialogs around it and the hand-drawn bar would no longer match its own
    // surroundings.
    QApplication::setStyle(QStringLiteral("Fusion"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Plays a video through the ffmpeg pipe with the player's own UI."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                QStringLiteral("Video to open."));
    parser.process(app);

    PlayerWindow window;

    const QStringList arguments = parser.positionalArguments();
    QString path = arguments.isEmpty() ? QString() : arguments.first();
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(
            &window, QStringLiteral("Open a video"), QString(),
            QStringLiteral("Video (*.mp4 *.mkv *.mov *.webm *.avi *.gif *.png *.jpg *.jpeg)"));
    }

    window.show();
    if (!path.isEmpty()) window.open(path);

    return app.exec();
}
