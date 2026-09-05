#include "Database.h"
#include "Config.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QThread>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace OmniView {

Database::Database(const QString& dbPath) {
    if (dbPath.isEmpty()) {
        m_dbPath = Config::dbPath();
    } else {
        m_dbPath = dbPath;
    }
    initSchema();
}

Database::~Database() {
}

QSqlDatabase Database::getDatabase() {
    const QString connName = QStringLiteral("omniview_conn_%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName);
        if (db.isOpen()) {
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
        qWarning() << "Schema creation failed:" << q.lastError().text();
        return false;
    }

    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_path ON images(path);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_subfolder ON images(subfolder);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_color ON images(color_name);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_aspect ON images(aspect_type);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_mtime ON images(mtime);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_favorite ON images(is_favorite);"));

    return true;
}

bool Database::batchSyncFiles(const QVector<ImageRecord>& entries) {
    if (entries.isEmpty()) return true;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO images (path, filename, subfolder, file_size, mtime, indexed) "
        "VALUES (?, ?, ?, ?, ?, 0) "
        "ON CONFLICT(path) DO UPDATE SET "
        "    file_size = excluded.file_size, "
        "    mtime = excluded.mtime "
        "WHERE images.mtime != excluded.mtime;"
    ));

    for (const auto& item : entries) {
        q.bindValue(0, item.path);
        q.bindValue(1, item.filename);
        q.bindValue(2, item.subfolder);
        q.bindValue(3, item.fileSize);
        q.bindValue(4, item.mtime);
        q.exec();
    }

    return db.commit();
}

int Database::pruneMissingFiles(const QString& rootDir) {
    if (rootDir.isEmpty()) return 0;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return 0;

    const QString absRoot = QDir(rootDir).absolutePath();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT path FROM images WHERE path LIKE ?"));
    q.addBindValue(absRoot + QStringLiteral("/%"));
    if (!q.exec()) return 0;

    QStringList missing;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (!QFile::exists(path)) {
            missing.append(path);
        }
    }

    if (missing.isEmpty()) return 0;

    db.transaction();
    QSqlQuery delQ(db);
    delQ.prepare(QStringLiteral("DELETE FROM images WHERE path = ?"));
    for (const QString& p : missing) {
        delQ.bindValue(0, p);
        delQ.exec();
    }
    db.commit();

    return missing.size();
}

QVector<ImageRecord> Database::getUnindexedPaths() {
    QVector<ImageRecord> list;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return list;

    QSqlQuery q(db);
    const QString sql = QStringLiteral(
        "SELECT path, mtime, file_size FROM images "
        "WHERE indexed = 0 OR thumb_path = '' OR thumb_path LIKE '%.jpg' OR thumb_path LIKE '%pixiv_gallery%' "
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

    if (!filter.rootDir.isEmpty()) {
        const QString absRoot = QDir(filter.rootDir).absolutePath();
        clauses.append(QStringLiteral("path LIKE ?"));
        params.append(absRoot + QStringLiteral("/%"));
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
        clauses.append(QStringLiteral("(filename LIKE ? OR subfolder LIKE ?)"));
        const QString pattern = QStringLiteral("%%1%").arg(filter.searchTerm.trimmed());
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
        whereSql = QStringLiteral("WHERE path LIKE ? ");
        rootPrefix = QDir(rootDir).absolutePath() + QStringLiteral("/%");
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

Stats Database::getStats() {
    Stats s;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return s;

    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT COUNT(*), SUM(file_size) FROM images;")) && q.next()) {
        s.totalImages = q.value(0).toInt();
        s.totalBytes = q.value(1).toLongLong();
    }

    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM images WHERE indexed = 1;")) && q.next()) {
        s.indexedImages = q.value(0).toInt();
    }

    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM images WHERE is_favorite = 1;")) && q.next()) {
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
        const int count = qMin(chunkSize, paths.size() - i);
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
