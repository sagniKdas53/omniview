#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QMetaType>
#include <QSqlDatabase>

#include <functional>

namespace OmniView {

struct ImageFeatures;

struct ImageRecord {
    QString path;
    QString filename;
    QString subfolder;
    qint64 fileSize = 0;
    double mtime = 0.0;
    int width = 0;
    int height = 0;
    QString aspectType = QStringLiteral("square");
    QString colorName;
    int colorR = 0;
    int colorG = 0;
    int colorB = 0;
    QString dhash;
    QString thumbPath;
    bool isFavorite = false;
    bool indexed = false;
};

// Which folder(s) under the root a query covers. Typed instead of in-band
// marker strings so real folders named "__all__"/"__root__" still work.
enum class FolderScope {
    All,        // every image under the root
    Root,       // only images directly in the root (subfolder == "")
    Subfolder   // only images whose subfolder equals QueryFilter::subfolder
};

struct QueryFilter {
    QString rootDir;
    FolderScope folderScope = FolderScope::All;
    QString subfolder;
    QString colorName = QStringLiteral("all");
    QString aspectType = QStringLiteral("all");
    QString searchTerm;
    bool favoriteOnly = false;
    QString sortBy = QStringLiteral("mtime_desc");
    QString exactPath;
};

struct Stats {
    int totalImages = 0;
    qint64 totalBytes = 0;
    int indexedImages = 0;
    int favoritesCount = 0;
};

// What a scan observed, so pruning can remove archive entries that vanished
// from an archive that still exists.
struct ScanObservation {
    QSet<QString> listedArchives;   // absolute .zip paths that were opened successfully
    QSet<QString> seenPaths;        // every composite/plain path the scan reported
};

// A Database instance owns one SQLite connection, opened lazily on first use
// and removed when the instance is destroyed. QSqlDatabase connections are
// bound to a thread: use each instance from a single thread only (create one
// per worker run, as Scanner and ThumbnailTask do).
class Database {
public:
    enum class UpdateResult { Updated, Stale, Error };

    explicit Database(const QString& dbPath = QString());
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    bool initSchema();
    bool batchSyncFiles(const QVector<ImageRecord>& entries);
    int pruneMissingFiles(const QString& rootDir,
                          const ScanObservation* observation = nullptr,
                          const std::function<bool()>& isCancelled = {});
    // Rows that still need a thumbnail; limited to rootDir when non-empty.
    QVector<ImageRecord> getUnindexedPaths(const QString& rootDir = QString());
    bool updateImageFeatures(const QString& path, int width, int height,
                             const QString& aspect, const QString& colorName,
                             int r, int g, int b, const QString& dhash,
                             const QString& thumbPath);
    // Stores features only if the row still has the mtime/size the worker
    // decoded; returns Stale when a rescan changed the file in the meantime.
    UpdateResult updateImageFeaturesIfUnchanged(const QString& path, double mtime,
                                                qint64 fileSize, const ImageFeatures& feat);
    bool toggleFavorite(const QString& path, bool* outNewVal = nullptr);
    QVector<ImageRecord> queryImages(const QueryFilter& filter);
    // Subfolder name ("" for the root folder itself) and image count.
    QVector<QPair<QString, int>> getSubfoldersWithCounts(const QString& rootDir);
    Stats getStats(const QString& rootDir = QString());

    QString dbPath() const { return m_dbPath; }
    QString lastError() const { return m_lastError; }

    static QString escapeSqlLike(const QString& str);

    // Inclusive lower / exclusive upper bound of all paths below rootDir.
    // Byte-wise (case-sensitive) and able to use the primary-key index,
    // unlike LIKE.
    static QPair<QString, QString> rootPathRange(const QString& rootDir);

    // Two mtimes closer than this are the same file version. Other OmniView
    // implementations sharing the DB store fractional seconds; this scanner
    // stores whole seconds.
    static constexpr double MTIME_TOLERANCE_SECONDS = 1.0;

private:
    QSqlDatabase getDatabase();
    void closeConnection();
    bool recoverCorruptDatabase();

    QString m_dbPath;
    QString m_connectionName;
    QString m_lastError;
};

} // namespace OmniView

Q_DECLARE_METATYPE(OmniView::ImageRecord)
Q_DECLARE_METATYPE(OmniView::Stats)
