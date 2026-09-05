#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QFileInfo>
#include <QDir>
#include "MainWindow.h"
#include "Config.h"

int main(int argc, char* argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);

    QApplication app(argc, argv);
    app.setApplicationName(OmniView::Config::appName());
    app.setApplicationDisplayName(OmniView::Config::appTitle());
    app.setDesktopFileName(QStringLiteral("omniview.desktop"));

    // App Icon
    QString iconPath;
    const QString appDir = QApplication::applicationDirPath();
    const QStringList candidatePaths = {
        appDir + QStringLiteral("/assets/icon_256.png"),
        appDir + QStringLiteral("/../assets/icon_256.png"),
        appDir + QStringLiteral("/assets/icon.png"),
        appDir + QStringLiteral("/../assets/icon.png"),
        QDir::homePath() + QStringLiteral("/.local/share/icons/hicolor/256x256/apps/omniview.png"),
        QStringLiteral("/home/sagnik/Projects/omniview-cpp/assets/icon_256.png")
    };
    for (const QString& p : candidatePaths) {
        if (QFile::exists(p)) {
            iconPath = p;
            break;
        }
    }
    if (!iconPath.isEmpty()) {
        app.setWindowIcon(QIcon(iconPath));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OmniView — High Performance C++ Image Gallery with Native Drag & Drop"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("dir"), QStringLiteral("Root directory containing images"));

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    QString targetDir = args.isEmpty() ? OmniView::Config::defaultRootDir() : args.first();
    if (!QDir(targetDir).exists()) {
        targetDir = QDir::homePath();
    }

    OmniView::MainWindow window(targetDir);
    if (!iconPath.isEmpty()) {
        window.setWindowIcon(QIcon(iconPath));
    }
    window.show();

    return app.exec();
}
