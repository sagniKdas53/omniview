#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QStandardPaths>

namespace OmniView {

QString Config::appName() {
    return QStringLiteral("OmniView");
}

QString Config::appId() {
    return QStringLiteral("omniview");
}

QString Config::appTitle() {
    return QStringLiteral("OmniView — High Performance Image Gallery");
}

QString Config::defaultRootDir() {
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!pictures.isEmpty() && QDir(pictures).exists()) {
        return pictures;
    }
    return QDir::homePath();
}

QString Config::cacheDir() {
    const QString overrideDir = qEnvironmentVariable("OMNIVIEW_CACHE_DIR");
    if (!overrideDir.isEmpty()) return QDir(overrideDir).absolutePath();
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!cache.isEmpty()) {
        return cache + QStringLiteral("/omniview");
    }
    return QDir::homePath() + QStringLiteral("/.cache/omniview");
}

QString Config::thumbnailsDir() {
    return cacheDir() + QStringLiteral("/thumbnails");
}

QString Config::legacyThumbnailsDir() {
    return QDir::homePath() + QStringLiteral("/.cache/pixiv_gallery/thumbnails");
}

QString Config::dbPath() {
    return cacheDir() + QStringLiteral("/omniview_index.db");
}

QString Config::legacyDbPath() {
    return QDir::homePath() + QStringLiteral("/.cache/pixiv_gallery/pixiv_index.db");
}

void Config::ensureDirectories() {
    QDir().mkpath(cacheDir());
    QDir().mkpath(thumbnailsDir());

    // Legacy DB migration if omniview DB does not exist
    const QString targetDb = dbPath();
    const QString legacyDb = legacyDbPath();
    if (!QFile::exists(targetDb) && QFile::exists(legacyDb)) {
        QFile::copy(legacyDb, targetDb);
    }
}

bool Config::isSupportedExtension(const QString& ext) {
    static const QSet<QString> supported = {
        QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"),
        QStringLiteral(".png"),
        QStringLiteral(".webp"),
        QStringLiteral(".bmp"),
        QStringLiteral(".gif")
    };
    return supported.contains(ext.toLower());
}

QString Config::getThumbPath(const QString& sourcePath) {
    const QByteArray utf8 = sourcePath.toUtf8();

    // 1. Standard SHA-256 WebP path
    const QString sha256Hex = QString::fromUtf8(QCryptographicHash::hash(utf8, QCryptographicHash::Sha256).toHex());
    const QString sha256Prefix = sha256Hex.left(24);
    const QString standardWebp = thumbnailsDir() + QStringLiteral("/") + sha256Prefix + QStringLiteral(".webp");

    if (QFile::exists(standardWebp)) {
        return standardWebp;
    }

    // 2. Check legacy MD5 WebP
    const QString md5Hex = QString::fromUtf8(QCryptographicHash::hash(utf8, QCryptographicHash::Md5).toHex());
    const QString md5Webp = thumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".webp");
    if (QFile::exists(md5Webp)) {
        return md5Webp;
    }

    // 3. Check legacy JPG (both in current and legacy cache folder)
    const QString currentJpg = thumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".jpg");
    if (QFile::exists(currentJpg)) {
        return currentJpg;
    }
    const QString legacyJpg = legacyThumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".jpg");
    if (QFile::exists(legacyJpg)) {
        return legacyJpg;
    }

    // Default to standard SHA-256 WebP
    return standardWebp;
}

} // namespace OmniView
