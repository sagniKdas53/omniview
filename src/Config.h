#pragma once

#include <QString>
#include <QStringList>
#include <QSet>
#include <QSize>

namespace OmniView {

class Config {
public:
    static QString appName();
    static QString appId();
    static QString appTitle();
    static QString defaultRootDir();

    static QString cacheDir();
    static QString thumbnailsDir();
    static QString legacyThumbnailsDir();
    static QString dbPath();
    static QString legacyDbPath();

    static void ensureDirectories();
    static bool isSupportedExtension(const QString& ext);
    static QString getThumbPath(const QString& sourcePath);

    static constexpr int DEFAULT_GRID_SIZE = 220;
    static constexpr int MIN_GRID_SIZE = 110;
    static constexpr int MAX_GRID_SIZE = 480;
    static constexpr int THUMBNAIL_WIDTH = 360;
    static constexpr int THUMBNAIL_HEIGHT = 360;
};

} // namespace OmniView
