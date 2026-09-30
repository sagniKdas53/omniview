#include "Database.h"
#include "Config.h"
#include "ZipUtils.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QThread>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QDebug>

namespace OmniView {

namespace {

QString rootPathPattern(const QString& rootDir) {
    const QString absRoot = QDir(rootDir).absolutePath();
    const QString escaped = Database::escapeSqlLike(absRoot);
    return escaped + (absRoot.endsWith(QLatin1Char('/'))
        ? QStringLiteral("%")
        : QStringLiteral("/%"));
}

}

Database::Database(const QString& dbPath) {
    if (dbPath.isEmpty()) {
        m_dbPath = Config::dbPath();
    } else {
        m_dbPath = dbPath;
    }
    // initSchema() is deliberately NOT called here to prevent worker threads
    // from repeatedly executing 7 DDL statements per generated thumbnail.
}

Database::~Database() {
}

QString Database::escapeSqlLike(const QString& str) {
    QString escaped = str;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return escaped;
}

QSqlDatabase Database::getDatabase() {
    const QString connName = QStringLiteral("omniview_conn_%1_%2")
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()))
        .arg(QString::fromUtf8(QCryptographicHash::hash(m_dbPath.toUtf8(), QCryptographicHash::Md5).toHex().left(12)));
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName);
        if (db.isOpen() && db.databaseName() == m_dbPath) {
            return db;
        }
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(m_dbPath);
    if (db.open()) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
        q.exec(QStringLiteral("PRAGMA synchronous = NORMAL;"));
        q.exec(QStringLiteral("PRAGMA cache_size = -64000;"));
    } else {
        qWarning() << "Failed to open SQLite database:" << db.lastError().text();
    }
    return db;
}

bool Database::initSchema() {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    QString schemaError;
    QString schemaNativeCode;
    auto executeSchema = [&db, &schemaError, &schemaNativeCode]() -> bool {
        QSqlQuery q(db);
        const QString schema = QStringLiteral(
            "CREATE TABLE IF NOT EXISTS images ("
            "    path TEXT PRIMARY KEY,"
            "    filename TEXT NOT NULL,"
            "    subfolder TEXT NOT NULL,"
            "    file_size INTEGER NOT NULL,"
            "    mtime REAL NOT NULL,"
            "    width INTEGER DEFAULT 0,"
            "    height INTEGER DEFAULT 0,"
            "    aspect_type TEXT DEFAULT 'square',"
            "    color_name TEXT DEFAULT '',"
            "    color_r INTEGER DEFAULT 0,"
            "    color_g INTEGER DEFAULT 0,"
            "    color_b INTEGER DEFAULT 0,"
            "    dhash TEXT DEFAULT '',"
            "    thumb_path TEXT DEFAULT '',"
            "    is_favorite INTEGER DEFAULT 0,"
            "    indexed INTEGER DEFAULT 0"
            ");"
        );

        if (!q.exec(schema)) {
            schemaError = q.lastError().text();
            schemaNativeCode = q.lastError().nativeErrorCode();
            return false;
        }

        // path already has an index through its PRIMARY KEY.
        const QStringList indexes = {
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_subfolder ON images(subfolder);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_color ON images(color_name);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_aspect ON images(aspect_type);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_mtime ON images(mtime);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_favorite ON images(is_favorite);")
        };
        for (const auto& index : indexes) {
            if (!q.exec(index)) {
                schemaError = q.lastError().text();
                schemaNativeCode = q.lastError().nativeErrorCode();
                return false;
            }
        }
        return true;
    };

    if (executeSchema()) {
        return true;
    }

    if (schemaNativeCode != QStringLiteral("11") && schemaNativeCode != QStringLiteral("26")) {
        qWarning() << "Database schema initialization failed:" << schemaError
                   << "(native code" << schemaNativeCode << ")";
        return false;
    }

    // Recover only from SQLite corruption/not-a-database errors.
    qWarning() << "Database schema initialization failed. Attempting recovery from corruption:"
               << schemaError << "(native code" << schemaNativeCode << ")";
    const QString connName = db.connectionName();
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connName);

    if (QFile::exists(m_dbPath)) {
        const QString backupPath = m_dbPath + QStringLiteral(".corrupt.") + QString::number(QDateTime::currentMSecsSinceEpoch());
        if (!QFile::rename(m_dbPath, backupPath)) {
            qWarning() << "Failed to preserve corrupt database:" << m_dbPath;
            return false;
        }
        QFile::remove(m_dbPath + QStringLiteral("-wal"));
        QFile::remove(m_dbPath + QStringLiteral("-shm"));
    }

    db = getDatabase();
    if (!db.isOpen()) return false;
    return executeSchema();
}

bool Database::batchSyncFiles(const QVector<ImageRecord>& entries) {
    if (entries.isEmpty()) return true;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    if (!db.transaction()) {
        return false;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO images (path, filename, subfolder, file_size, mtime, indexed) "
        "VALUES (?, ?, ?, ?, ?, 0) "
        "ON CONFLICT(path) DO UPDATE SET "
        "    file_size = excluded.file_size, "
        "    mtime = excluded.mtime, indexed = 0, thumb_path = '', "
        "    width = 0, height = 0, aspect_type = 'square', color_name = '', "
        "    color_r = 0, color_g = 0, color_b = 0, dhash = '' "
        "WHERE images.mtime != excluded.mtime OR images.file_size != excluded.file_size;"
    ));

    for (const auto& item : entries) {
        q.bindValue(0, item.path);
        q.bindValue(1, item.filename);
        q.bindValue(2, item.subfolder.isNull() ? QStringLiteral("") : item.subfolder);
        q.bindValue(3, item.fileSize);
        q.bindValue(4, item.mtime);
        if (!q.exec()) {
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        db.rollback();
        return false;
    }
    return true;
}

int Database::pruneMissingFiles(const QString& rootDir) {
    if (rootDir.isEmpty()) return 0;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return 0;

    const QString absRoot = QDir(rootDir).absolutePath();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT path FROM images WHERE path LIKE ? ESCAPE '\\'"));
    q.addBindValue(rootPathPattern(absRoot));
    if (!q.exec()) return 0;

    QStringList missing;
    while (q.next()) {
        const QString path = q.value(0).toString();
        QString zipPath, innerPath;
        if (ZipUtils::isZipPath(path, &zipPath, &innerPath)) {
            if (!QFile::exists(zipPath)) {
                missing.append(path);
            } else {
                const QFileInfo zfi(zipPath);
                if (ZipUtils::isDuplicateOfRaw(zfi.absolutePath(), zfi.completeBaseName(), innerPath)) {
                    missing.append(path); // Prune duplicate so raw file takes precedence
                }
            }
        } else if (!QFile::exists(path)) {
            missing.append(path);
        }
    }

    if (missing.isEmpty()) return 0;

    if (!db.transaction()) {
        return 0;
    }

    QSqlQuery delQ(db);
    delQ.prepare(QStringLiteral("DELETE FROM images WHERE path = ?"));
    for (const QString& p : missing) {
        delQ.bindValue(0, p);
        if (!delQ.exec()) {
            db.rollback();
            return 0;
        }
    }

    if (!db.commit()) {
        db.rollback();
        return 0;
    }

    return missing.size();
}

QVector<ImageRecord> Database::getUnindexedPaths() {
    QVector<ImageRecord> list;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return list;

    QSqlQuery q(db);
    const QString sql = QStringLiteral(
        "SELECT path, mtime, file_size FROM images "
        "WHERE indexed = 0 OR thumb_path = '' "
        "ORDER BY mtime DESC;"
    );

    if (q.exec(sql)) {
        while (q.next()) {
            ImageRecord rec;
            rec.path = q.value(0).toString();
            rec.mtime = q.value(1).toDouble();
            rec.fileSize = q.value(2).toLongLong();
            list.append(rec);
        }
    }

    return list;
}

bool Database::updateImageFeatures(
    const QString& path, int width, int height,
    const QString& aspect, const QString& colorName,
    int r, int g, int b, const QString& dhash,
    const QString& thumbPath
) {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE images SET "
        "    width = ?, height = ?, aspect_type = ?, "
        "    color_name = ?, color_r = ?, color_g = ?, color_b = ?, "
        "    dhash = ?, thumb_path = ?, indexed = 1 "
        "WHERE path = ?;"
    ));

    q.addBindValue(width);
    q.addBindValue(height);
    q.addBindValue(aspect);
    q.addBindValue(colorName);
    q.addBindValue(r);
    q.addBindValue(g);
    q.addBindValue(b);
    q.addBindValue(dhash);
    q.addBindValue(thumbPath);
    q.addBindValue(path);

    return q.exec();
}

bool Database::toggleFavorite(const QString& path, bool* outNewVal) {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT is_favorite FROM images WHERE path = ?;"));
    q.addBindValue(path);
    if (!q.exec() || !q.next()) {
        return false;
    }

    const int current = q.value(0).toInt();
    const int updated = (current == 1) ? 0 : 1;

    QSqlQuery u(db);
    u.prepare(QStringLiteral("UPDATE images SET is_favorite = ? WHERE path = ?;"));
    u.addBindValue(updated);
    u.addBindValue(path);
    const bool ok = u.exec();
    if (ok && outNewVal) {
        *outNewVal = (updated == 1);
    }
    return ok;
}

QVector<ImageRecord> Database::queryImages(const QueryFilter& filter) {
    QVector<ImageRecord> results;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return results;

    QStringList clauses;
    QVector<QVariant> params;

    if (!filter.exactPath.isEmpty()) {
        clauses.append(QStringLiteral("path = ?"));
        params.append(filter.exactPath);
    }

    if (!filter.rootDir.isEmpty()) {
        const QString absRoot = QDir(filter.rootDir).absolutePath();
        clauses.append(QStringLiteral("path LIKE ? ESCAPE '\\'"));
        params.append(rootPathPattern(absRoot));
    }

    if (!filter.subfolder.isEmpty() && filter.subfolder != QStringLiteral("__all__")) {
        if (filter.subfolder == QStringLiteral("__root__")) {
            clauses.append(QStringLiteral("subfolder = ''"));
        } else {
            clauses.append(QStringLiteral("subfolder = ?"));
            params.append(filter.subfolder);
        }
    }

    if (!filter.colorName.isEmpty() && filter.colorName != QStringLiteral("all")) {
        clauses.append(QStringLiteral("color_name = ?"));
        params.append(filter.colorName);
    }

    if (!filter.aspectType.isEmpty() && filter.aspectType != QStringLiteral("all")) {
        clauses.append(QStringLiteral("aspect_type = ?"));
        params.append(filter.aspectType);
    }

    if (filter.favoriteOnly) {
        clauses.append(QStringLiteral("is_favorite = 1"));
    }

    if (!filter.searchTerm.isEmpty()) {
        clauses.append(QStringLiteral("(filename LIKE ? ESCAPE '\\' OR subfolder LIKE ? ESCAPE '\\')"));
        const QString pattern = QStringLiteral("%") + escapeSqlLike(filter.searchTerm.trimmed()) + QStringLiteral("%");
        params.append(pattern);
        params.append(pattern);
    }

    QString whereSql;
    if (!clauses.isEmpty()) {
        whereSql = QStringLiteral("WHERE ") + clauses.join(QStringLiteral(" AND "));
    }

    QString sortSql = QStringLiteral("mtime DESC");
    if (filter.sortBy == QStringLiteral("mtime_asc")) {
        sortSql = QStringLiteral("mtime ASC");
    } else if (filter.sortBy == QStringLiteral("size_desc")) {
        sortSql = QStringLiteral("file_size DESC");
    } else if (filter.sortBy == QStringLiteral("size_asc")) {
        sortSql = QStringLiteral("file_size ASC");
    } else if (filter.sortBy == QStringLiteral("name_asc")) {
        sortSql = QStringLiteral("filename ASC");
    } else if (filter.sortBy == QStringLiteral("random")) {
        sortSql = QStringLiteral("RANDOM()");
    }

    const QString sql = QStringLiteral(
        "SELECT path, filename, subfolder, file_size, mtime, "
        "       width, height, aspect_type, color_name, "
        "       color_r, color_g, color_b, dhash, thumb_path, is_favorite, indexed "
        "FROM images "
    ) + whereSql + QStringLiteral(" ORDER BY ") + sortSql + QStringLiteral(";");

    QSqlQuery q(db);
    q.prepare(sql);
    for (const auto& p : params) {
        q.addBindValue(p);
    }

    if (q.exec()) {
        while (q.next()) {
            ImageRecord r;
            r.path = q.value(0).toString();
            r.filename = q.value(1).toString();
            r.subfolder = q.value(2).toString();
            r.fileSize = q.value(3).toLongLong();
            r.mtime = q.value(4).toDouble();
            r.width = q.value(5).toInt();
            r.height = q.value(6).toInt();
            r.aspectType = q.value(7).toString();
            r.colorName = q.value(8).toString();
            r.colorR = q.value(9).toInt();
            r.colorG = q.value(10).toInt();
            r.colorB = q.value(11).toInt();
            r.dhash = q.value(12).toString();
            r.thumbPath = q.value(13).toString();
            r.isFavorite = (q.value(14).toInt() == 1);
            r.indexed = (q.value(15).toInt() == 1);
            results.append(r);
        }
    }

    return results;
}

QVector<QPair<QString, int>> Database::getSubfoldersWithCounts(const QString& rootDir) {
    QVector<QPair<QString, int>> list;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return list;

    QString whereSql;
    QString rootPrefix;
    if (!rootDir.isEmpty()) {
        whereSql = QStringLiteral("WHERE path LIKE ? ESCAPE '\\' ");
        rootPrefix = rootPathPattern(rootDir);
    }

    const QString sql = QStringLiteral(
        "SELECT "
        "    CASE WHEN subfolder = '' THEN '__root__' ELSE subfolder END as folder, "
        "    COUNT(*) as count "
        "FROM images "
    ) + whereSql + QStringLiteral("GROUP BY folder ORDER BY count DESC;");

    QSqlQuery q(db);
    q.prepare(sql);
    if (!rootPrefix.isEmpty()) {
        q.addBindValue(rootPrefix);
    }

    if (q.exec()) {
        while (q.next()) {
            list.append({q.value(0).toString(), q.value(1).toInt()});
        }
    }

    return list;
}

Stats Database::getStats(const QString& rootDir) {
    Stats s;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return s;

    QString whereSql;
    QString rootPattern;
    if (!rootDir.isEmpty()) {
        whereSql = QStringLiteral(" WHERE path LIKE ? ESCAPE '\\'");
        rootPattern = rootPathPattern(rootDir);
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(file_size), 0) FROM images") + whereSql + QStringLiteral(";"));
    if (!rootPattern.isEmpty()) q.addBindValue(rootPattern);
    if (q.exec() && q.next()) {
        s.totalImages = q.value(0).toInt();
        s.totalBytes = q.value(1).toLongLong();
    }

    q.prepare(QStringLiteral("SELECT COUNT(*) FROM images WHERE indexed = 1")
              + (rootDir.isEmpty() ? QStringLiteral(";") : QStringLiteral(" AND path LIKE ? ESCAPE '\\';")));
    if (!rootPattern.isEmpty()) q.addBindValue(rootPattern);
    if (q.exec() && q.next()) {
        s.indexedImages = q.value(0).toInt();
    }

    q.prepare(QStringLiteral("SELECT COUNT(*) FROM images WHERE is_favorite = 1")
              + (rootDir.isEmpty() ? QStringLiteral(";") : QStringLiteral(" AND path LIKE ? ESCAPE '\\';")));
    if (!rootPattern.isEmpty()) q.addBindValue(rootPattern);
    if (q.exec() && q.next()) {
        s.favoritesCount = q.value(0).toInt();
    }

    return s;
}

QMap<QString, QString> Database::getAllHashesForPaths(const QStringList& paths) {
    QMap<QString, QString> map;
    if (paths.isEmpty()) return map;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return map;

    const int chunkSize = 800;
    for (int i = 0; i < paths.size(); i += chunkSize) {
        const int count = qMin(chunkSize, static_cast<int>(paths.size()) - i);
        QStringList placeholders;
        placeholders.reserve(count);
        for (int k = 0; k < count; ++k) placeholders.append(QStringLiteral("?"));

        const QString sql = QStringLiteral(
            "SELECT path, dhash FROM images WHERE path IN ("
        ) + placeholders.join(QStringLiteral(",")) + QStringLiteral(") AND dhash != '';");

        QSqlQuery q(db);
        q.prepare(sql);
        for (int k = 0; k < count; ++k) {
            q.addBindValue(paths[i + k]);
        }

        if (q.exec()) {
            while (q.next()) {
                map.insert(q.value(0).toString(), q.value(1).toString());
            }
        }
    }

    return map;
}

} // namespace OmniView
