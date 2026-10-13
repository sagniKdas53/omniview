#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

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
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
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

bool Config::migrateLegacyDatabase(const QString& legacyDb, const QString& targetDb) {
    // Copy through SQLite (VACUUM INTO) rather than QFile::copy: a plain copy
    // misses committed changes still in the legacy -wal file and can be torn
    // if the legacy app is writing. Write to a temp name, then rename.
    const QString tmp = targetDb + QStringLiteral(".migrating");
    QFile::remove(tmp);
    const QString conn = QStringLiteral("omniview_legacy_migration");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(legacyDb);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("VACUUM INTO ?"));
            q.addBindValue(tmp);
            ok = q.exec();
            if (!ok) {
                qWarning() << "Legacy database migration failed:" << q.lastError().text();
            }
            q.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    if (!ok || !QFile::rename(tmp, targetDb)) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

void Config::ensureDirectories() {
    QDir().mkpath(cacheDir());
    QDir().mkpath(thumbnailsDir());

    // Legacy DB migration if omniview DB does not exist
    const QString targetDb = dbPath();
    const QString legacyDb = legacyDbPath();
    if (qEnvironmentVariableIsEmpty("OMNIVIEW_CACHE_DIR") && !QFile::exists(targetDb) && QFile::exists(legacyDb)) {
        migrateLegacyDatabase(legacyDb, targetDb);
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

QString Config::canonicalThumbPath(const QString& sourcePath) {
    const QByteArray sha = QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return thumbnailsDir() + QStringLiteral("/") + QString::fromLatin1(sha.left(24)) + QStringLiteral(".webp");
}

QString Config::getThumbPath(const QString& sourcePath) {
    // 1. Standard SHA-256 WebP path (shared with the Python and Rust versions)
    QString standardWebp = canonicalThumbPath(sourcePath);
    if (QFile::exists(standardWebp)) {
        return standardWebp;
    }

    // 2. Legacy MD5 WebP / JPG in the current cache
    const QString md5Hex = QString::fromLatin1(
        QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Md5).toHex());
    QString md5Webp = thumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".webp");
    if (QFile::exists(md5Webp)) {
        return md5Webp;
    }
    QString currentJpg = thumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".jpg");
    if (QFile::exists(currentJpg)) {
        return currentJpg;
    }

    // 3. The legacy pixiv_gallery cache, unless a separate cache was requested.
    if (qEnvironmentVariableIsEmpty("OMNIVIEW_CACHE_DIR")) {
        QString legacyJpg = legacyThumbnailsDir() + QStringLiteral("/") + md5Hex + QStringLiteral(".jpg");
        if (QFile::exists(legacyJpg)) {
            return legacyJpg;
        }
    }

    return standardWebp;
}

} // namespace OmniView
