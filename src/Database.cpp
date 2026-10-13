#include "Database.h"
#include "ColorUtils.h"
#include "Config.h"
#include "ZipUtils.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QThread>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDebug>

#include <atomic>

namespace OmniView {

namespace {

// Source of unique connection names; deliberately mutable process state.
std::atomic<quint64> s_connectionCounter{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

const QString kRootRangeClause = QStringLiteral("(path >= ? AND path < ?)");

// A row hashed by builds whose Qt 5 dHash read RGB32 pixels as bytes; every
// image got this value. Cleared on start-up so it is recomputed.
const QString kBrokenQt5DHash = QStringLiteral("2222222222222222");

} // namespace

Database::Database(const QString& dbPath)
    : m_dbPath(dbPath.isEmpty() ? Config::dbPath() : dbPath)
    , m_connectionName(QStringLiteral("omniview_conn_%1").arg(++s_connectionCounter))
{
    // initSchema() is deliberately NOT called here so worker threads do not
    // re-run DDL for every generated thumbnail.
}

Database::~Database() {
    closeConnection();
}

void Database::closeConnection() {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString Database::escapeSqlLike(const QString& str) {
    QString escaped = str;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return escaped;
}

QPair<QString, QString> Database::rootPathRange(const QString& rootDir) {
    QString prefix = QDir(rootDir).absolutePath();
    if (!prefix.endsWith(QLatin1Char('/'))) prefix += QLatin1Char('/');
    // '0' is the character right after '/', so [prefix, upper) is exactly the
    // set of strings starting with prefix.
    QString upper = prefix;
    upper[upper.size() - 1] = QLatin1Char('0');
    return {prefix, upper};
}

QSqlDatabase Database::getDatabase() {
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen()) {
            return db;
        }
        if (db.open()) {
            return db;
        }
        m_lastError = db.lastError().text();
        return db;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_dbPath);
    // Wait for other writers (scanner, thumbnail workers, other OmniView apps
    // sharing the DB) instead of failing with SQLITE_BUSY immediately.
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (db.open()) {
        QSqlQuery q(db);
        for (const QString& pragma : {QStringLiteral("PRAGMA journal_mode = WAL;"),
                                      QStringLiteral("PRAGMA synchronous = NORMAL;"),
                                      QStringLiteral("PRAGMA cache_size = -16000;")}) {
            if (!q.exec(pragma)) {
                // Not fatal (e.g. a corrupt file is detected and recovered in
                // initSchema), but worth knowing about.
                qWarning() << "SQLite" << pragma << "failed:" << q.lastError().text();
            }
        }
    } else {
        m_lastError = db.lastError().text();
        qWarning() << "Failed to open SQLite database:" << m_lastError;
    }
    return db;
}

bool Database::recoverCorruptDatabase() {
    const QString backupPath = m_dbPath + QStringLiteral(".corrupt.")
                               + QString::number(QDateTime::currentMSecsSinceEpoch());
    // Copy the WAL before closing: committed changes not yet checkpointed
    // (e.g. favourites set before a crash) live only there, and SQLite
    // deletes the WAL when the last connection closes.
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (QFile::exists(m_dbPath + suffix)) {
            QFile::copy(m_dbPath + suffix, backupPath + suffix);
        }
    }
    closeConnection();
    if (QFile::exists(m_dbPath)) {
        if (!QFile::rename(m_dbPath, backupPath)) {
            qWarning() << "Failed to preserve corrupt database:" << m_dbPath;
            return false;
        }
        QFile::remove(m_dbPath + QStringLiteral("-wal"));
        QFile::remove(m_dbPath + QStringLiteral("-shm"));
    }
    return getDatabase().isOpen();
}

bool Database::initSchema() {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    QString schemaError;
    QString schemaNativeCode;
    auto executeSchema = [this, &schemaError, &schemaNativeCode]() -> bool {
        QSqlQuery q(getDatabase());
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
        const QStringList statements = {
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_subfolder ON images(subfolder);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_color ON images(color_name);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_aspect ON images(aspect_type);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_mtime ON images(mtime);"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_favorite ON images(is_favorite);"),
        };
        for (const auto& statement : statements) {
            if (!q.exec(statement)) {
                schemaError = q.lastError().text();
                schemaNativeCode = q.lastError().nativeErrorCode();
                return false;
            }
        }

        // Data fix-up (no schema change): forget hashes written by the broken
        // Qt 5 dHash so they are recomputed from the cached thumbnail.
        q.prepare(QStringLiteral("UPDATE images SET dhash = '' WHERE dhash = ?;"));
        q.addBindValue(kBrokenQt5DHash);
        if (!q.exec()) {
            qWarning() << "Could not reset legacy dHash values:" << q.lastError().text();
        }
        return true;
    };

    if (executeSchema()) {
        return true;
    }

    if (schemaNativeCode != QStringLiteral("11") && schemaNativeCode != QStringLiteral("26")) {
        m_lastError = schemaError;
        qWarning() << "Database schema initialization failed:" << schemaError
                   << "(native code" << schemaNativeCode << ")";
        return false;
    }

    // Recover only from SQLite corruption/not-a-database errors.
    qWarning() << "Database schema initialization failed. Attempting recovery from corruption:"
               << schemaError << "(native code" << schemaNativeCode << ")";
    db = QSqlDatabase();
    if (!recoverCorruptDatabase()) return false;
    return executeSchema();
}

bool Database::batchSyncFiles(const QVector<ImageRecord>& entries) {
    if (entries.isEmpty()) return true;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        return false;
    }

    // The subfolder is relative to the root that was scanned. Opening a
    // parent or child folder of an earlier root must re-home existing rows,
    // without discarding their thumbnails and features.
    QSqlQuery rehome(db);
    rehome.prepare(QStringLiteral(
        "UPDATE images SET subfolder = ?, filename = ? WHERE path = ? AND (subfolder != ? OR filename != ?);"));

    QSqlQuery upsert(db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO images (path, filename, subfolder, file_size, mtime, indexed) "
        "VALUES (?, ?, ?, ?, ?, 0) "
        "ON CONFLICT(path) DO UPDATE SET "
        "    file_size = excluded.file_size, "
        "    mtime = excluded.mtime, indexed = 0, thumb_path = '', "
        "    width = 0, height = 0, aspect_type = 'square', color_name = '', "
        "    color_r = 0, color_g = 0, color_b = 0, dhash = '' "
        "WHERE abs(images.mtime - excluded.mtime) >= ? OR images.file_size != excluded.file_size;"
    ));

    for (const auto& item : entries) {
        const QString subfolder = item.subfolder.isNull() ? QStringLiteral("") : item.subfolder;
        rehome.bindValue(0, subfolder);
        rehome.bindValue(1, item.filename);
        rehome.bindValue(2, item.path);
        rehome.bindValue(3, subfolder);
        rehome.bindValue(4, item.filename);
        upsert.bindValue(0, item.path);
        upsert.bindValue(1, item.filename);
        upsert.bindValue(2, subfolder);
        upsert.bindValue(3, item.fileSize);
        upsert.bindValue(4, item.mtime);
        upsert.bindValue(5, MTIME_TOLERANCE_SECONDS);
        if (!rehome.exec() || !upsert.exec()) {
            m_lastError = rehome.lastError().isValid() ? rehome.lastError().text()
                                                       : upsert.lastError().text();
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

int Database::pruneMissingFiles(const QString& rootDir, const ScanObservation* observation,
                                const std::function<bool()>& isCancelled) {
    if (rootDir.isEmpty()) return 0;
    const QString absRoot = QDir(rootDir).absolutePath();
    // A root that vanished (unmounted drive, NAS offline) must not wipe the
    // index, favourites included.
    if (!QDir(absRoot).exists()) return 0;

    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return 0;

    const auto range = rootPathRange(absRoot);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT path, file_size FROM images WHERE ") + kRootRangeClause);
    q.addBindValue(range.first);
    q.addBindValue(range.second);
    if (!q.exec()) return 0;

    QHash<QString, bool> archiveExists;
    QStringList missing;
    while (q.next()) {
        if (isCancelled && isCancelled()) return 0;
        const QString path = q.value(0).toString();
        const qint64 size = q.value(1).toLongLong();
        QString zipPath, innerPath;
        if (ZipUtils::isZipPath(path, &zipPath, &innerPath)) {
            auto it = archiveExists.find(zipPath);
            if (it == archiveExists.end()) {
                it = archiveExists.insert(zipPath, QFile::exists(zipPath));
            }
            const bool archiveGone = !it.value();
            // Removed from (or renamed inside) an archive that was listed fine.
            const bool entryGone = observation && observation->listedArchives.contains(zipPath)
                                   && !observation->seenPaths.contains(path);
            // A raw extracted copy takes precedence over the archive entry.
            const bool duplicate = !archiveGone && !entryGone
                && ZipUtils::isDuplicateOfRaw(QFileInfo(zipPath).absolutePath(),
                                              QFileInfo(zipPath).completeBaseName(), innerPath, size);
            if (archiveGone || entryGone || duplicate) {
                missing.append(path);
            }
        } else if (!QFile::exists(path)) {
            missing.append(path);
        }
    }
    q.finish();

    if (missing.isEmpty()) return 0;
    if (isCancelled && isCancelled()) return 0;

    if (!db.transaction()) {
        return 0;
    }

    QSqlQuery delQ(db);
    delQ.prepare(QStringLiteral("DELETE FROM images WHERE path = ?"));
    for (const QString& p : missing) {
        delQ.bindValue(0, p);
        if (!delQ.exec()) {
            m_lastError = delQ.lastError().text();
            db.rollback();
            return 0;
        }
    }

    if (!db.commit()) {
        db.rollback();
        return 0;
    }

    return static_cast<int>(missing.size());
}

QVector<ImageRecord> Database::getUnindexedPaths(const QString& rootDir) {
    QVector<ImageRecord> list;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return list;

    QSqlQuery q(db);
    QString sql = QStringLiteral(
        "SELECT path, mtime, file_size FROM images "
        "WHERE (indexed = 0 OR COALESCE(thumb_path, '') = '' OR COALESCE(dhash, '') = '')");
    if (!rootDir.isEmpty()) {
        sql += QStringLiteral(" AND ") + kRootRangeClause;
    }
    sql += QStringLiteral(" ORDER BY mtime DESC;");
    q.prepare(sql);
    if (!rootDir.isEmpty()) {
        const auto range = rootPathRange(rootDir);
        q.addBindValue(range.first);
        q.addBindValue(range.second);
    }

    if (q.exec()) {
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

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

Database::UpdateResult Database::updateImageFeaturesIfUnchanged(
    const QString& path, double mtime, qint64 fileSize, const ImageFeatures& feat) {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return UpdateResult::Error;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE images SET "
        "    width = ?, height = ?, aspect_type = ?, "
        "    color_name = ?, color_r = ?, color_g = ?, color_b = ?, "
        "    dhash = ?, thumb_path = ?, indexed = 1 "
        "WHERE path = ? AND abs(mtime - ?) < ? AND file_size = ?;"
    ));
    q.addBindValue(feat.width);
    q.addBindValue(feat.height);
    q.addBindValue(feat.aspectType);
    q.addBindValue(feat.colorName);
    q.addBindValue(feat.colorR);
    q.addBindValue(feat.colorG);
    q.addBindValue(feat.colorB);
    q.addBindValue(feat.dhash);
    q.addBindValue(feat.thumbPath);
    q.addBindValue(path);
    q.addBindValue(mtime);
    q.addBindValue(MTIME_TOLERANCE_SECONDS);
    q.addBindValue(fileSize);

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return UpdateResult::Error;
    }
    return q.numRowsAffected() > 0 ? UpdateResult::Updated : UpdateResult::Stale;
}

bool Database::toggleFavorite(const QString& path, bool* outNewVal) {
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return false;

    // Write first: a SELECT left open before the UPDATE would make SQLite
    // upgrade a read transaction, which fails immediately with SQLITE_BUSY
    // (no busy-timeout wait) whenever the scanner or a worker is writing.
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        return false;
    }
    QSqlQuery u(db);
    u.prepare(QStringLiteral(
        "UPDATE images SET is_favorite = CASE WHEN is_favorite = 1 THEN 0 ELSE 1 END WHERE path = ?;"));
    u.addBindValue(path);
    if (!u.exec() || u.numRowsAffected() != 1) {
        m_lastError = u.lastError().text();
        db.rollback();
        return false;
    }
    u.finish();

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT is_favorite FROM images WHERE path = ?;"));
    q.addBindValue(path);
    const bool readOk = q.exec() && q.next();
    const bool newVal = readOk && q.value(0).toInt() == 1;
    q.finish();
    if (!readOk || !db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        return false;
    }
    if (outNewVal) *outNewVal = newVal;
    return true;
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
        const auto range = rootPathRange(filter.rootDir);
        clauses.append(kRootRangeClause);
        params.append(range.first);
        params.append(range.second);
    }

    switch (filter.folderScope) {
    case FolderScope::All:
        break;
    case FolderScope::Root:
        clauses.append(QStringLiteral("subfolder = ''"));
        break;
    case FolderScope::Subfolder:
        clauses.append(QStringLiteral("subfolder = ?"));
        params.append(filter.subfolder);
        break;
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

    const QString term = filter.searchTerm.trimmed();
    if (!term.isEmpty()) {
        clauses.append(QStringLiteral("(filename LIKE ? ESCAPE '\\' OR subfolder LIKE ? ESCAPE '\\')"));
        const QString pattern = QStringLiteral("%") + escapeSqlLike(term) + QStringLiteral("%");
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
        sortSql = QStringLiteral("filename COLLATE NOCASE ASC, filename ASC");
    } else if (filter.sortBy == QStringLiteral("random")) {
        sortSql = QStringLiteral("RANDOM()");
    }
    // Deterministic tie-break so equal keys keep a stable order across refreshes.
    if (filter.sortBy != QStringLiteral("random")) {
        sortSql += QStringLiteral(", path ASC");
    }

    const QString sql = QStringLiteral(
        "SELECT path, filename, subfolder, file_size, mtime, "
        "       width, height, aspect_type, color_name, "
        "       color_r, color_g, color_b, dhash, thumb_path, is_favorite, indexed "
        "FROM images "
    ) + whereSql + QStringLiteral(" ORDER BY ") + sortSql + QStringLiteral(";");

    QSqlQuery q(db);
    q.setForwardOnly(true);
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
    } else {
        m_lastError = q.lastError().text();
    }

    return results;
}

QVector<QPair<QString, int>> Database::getSubfoldersWithCounts(const QString& rootDir) {
    QVector<QPair<QString, int>> list;
    QSqlDatabase db = getDatabase();
    if (!db.isOpen()) return list;

    QString whereSql;
    if (!rootDir.isEmpty()) {
        whereSql = QStringLiteral("WHERE ") + kRootRangeClause + QStringLiteral(" ");
    }

    const QString sql = QStringLiteral(
        "SELECT subfolder, COUNT(*) AS count FROM images "
    ) + whereSql + QStringLiteral("GROUP BY subfolder ORDER BY count DESC, subfolder ASC;");

    QSqlQuery q(db);
    q.prepare(sql);
    if (!rootDir.isEmpty()) {
        const auto range = rootPathRange(rootDir);
        q.addBindValue(range.first);
        q.addBindValue(range.second);
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
    QPair<QString, QString> range;
    if (!rootDir.isEmpty()) {
        whereSql = QStringLiteral(" WHERE ") + kRootRangeClause;
        range = rootPathRange(rootDir);
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*), COALESCE(SUM(file_size), 0), "
        "       COALESCE(SUM(indexed = 1), 0), COALESCE(SUM(is_favorite = 1), 0) "
        "FROM images") + whereSql + QStringLiteral(";"));
    if (!rootDir.isEmpty()) {
        q.addBindValue(range.first);
        q.addBindValue(range.second);
    }
    if (q.exec() && q.next()) {
        s.totalImages = q.value(0).toInt();
        s.totalBytes = q.value(1).toLongLong();
        s.indexedImages = q.value(2).toInt();
        s.favoritesCount = q.value(3).toInt();
    }

    return s;
}

} // namespace OmniView
