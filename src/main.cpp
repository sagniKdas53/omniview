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

    // Register custom types for cross-thread Qt signal/slot delivery
    qRegisterMetaType<OmniView::Stats>("OmniView::Stats");
    qRegisterMetaType<OmniView::Stats>("Stats");
    qRegisterMetaType<OmniView::ImageFeatures>("OmniView::ImageFeatures");
    qRegisterMetaType<OmniView::ImageFeatures>("ImageFeatures");
    qRegisterMetaType<OmniView::ImageRecord>("OmniView::ImageRecord");
    qRegisterMetaType<OmniView::ImageRecord>("ImageRecord");

    // Configure system font fallbacks so emojis and symbols render properly without tofu (▯)
    QFont appFont = app.font();
    QStringList fontFamilies = appFont.families();
    if (fontFamilies.isEmpty() && !appFont.family().isEmpty()) {
        fontFamilies.append(appFont.family());
    }
    const QStringList emojiFallbackFamilies = {
        QStringLiteral("Noto Sans"),
        QStringLiteral("DejaVu Sans"),
        QStringLiteral("Ubuntu"),
        QStringLiteral("Noto Color Emoji"),
        QStringLiteral("Symbola"),
        QStringLiteral("Segoe UI Emoji"),
        QStringLiteral("Apple Color Emoji"),
        QStringLiteral("Noto Sans Symbols 2")
    };
    for (const QString& fam : emojiFallbackFamilies) {
        if (!fontFamilies.contains(fam)) {
            fontFamilies.append(fam);
        }
    }
    appFont.setFamilies(fontFamilies);
    app.setFont(appFont);

    // Register font substitutions for universal glyph fallback across all Qt widgets
    QFont::insertSubstitutions(appFont.family(), emojiFallbackFamilies);
    QFont::insertSubstitutions(QStringLiteral("Noto Sans"), emojiFallbackFamilies);
    QFont::insertSubstitutions(QStringLiteral("Ubuntu"), emojiFallbackFamilies);
    QFont::insertSubstitutions(QStringLiteral("DejaVu Sans"), emojiFallbackFamilies);
    QFont::insertSubstitutions(QStringLiteral("Sans Serif"), emojiFallbackFamilies);

    // App Icon
    QString iconPath;
    const QString appDir = QApplication::applicationDirPath();
    const QStringList candidatePaths = {
        appDir + QStringLiteral("/assets/icon_256.png"),
        appDir + QStringLiteral("/../assets/icon_256.png"),
        appDir + QStringLiteral("/assets/icon.png"),
        appDir + QStringLiteral("/../assets/icon.png"),
        QDir::homePath() + QStringLiteral("/.local/share/icons/hicolor/256x256/apps/omniview.png")
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
