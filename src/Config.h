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

    // OMNIVIEW_CACHE_DIR if set, else defaultCacheDir().
    static QString cacheDir();
    // ~/.cache/omniview on every platform (the location the Rust version uses).
    static QString defaultCacheDir();
    // Where earlier C++ builds kept the cache: Qt's GenericCacheLocation, which
    // differs from defaultCacheDir() on macOS, Windows and with XDG_CACHE_HOME.
    static QString previousDefaultCacheDir();
    // Moves (or, if that is impossible, copies) a cache from `from` to `to`
    // and rewrites stored thumbnail paths. Refuses to overwrite an index.
    static bool migrateCacheDir(const QString& from, const QString& to);
    static QString thumbnailsDir();
    static QString legacyThumbnailsDir();
    static QString dbPath();
    static QString legacyDbPath();

    static void ensureDirectories();
    // Snapshot of the legacy pixiv_gallery index (WAL included) into targetDb.
    static bool migrateLegacyDatabase(const QString& legacyDb, const QString& targetDb);
    static bool isSupportedExtension(const QString& ext);
    // Where an existing thumbnail for sourcePath can be read from: the shared
    // {sha256[:24]}.webp if present, else a legacy MD5 thumbnail, else the
    // shared path. Use canonicalThumbPath() for writing.
    static QString getThumbPath(const QString& sourcePath);
    // The shared cache file name: thumbnails/{sha256(utf8 path)[:24]}.webp.
    static QString canonicalThumbPath(const QString& sourcePath);

    static constexpr int DEFAULT_GRID_SIZE = 220;
    static constexpr int MIN_GRID_SIZE = 110;
    static constexpr int MAX_GRID_SIZE = 480;
    static constexpr int THUMBNAIL_WIDTH = 360;
    static constexpr int THUMBNAIL_HEIGHT = 360;
};

} // namespace OmniView
