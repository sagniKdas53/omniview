#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QMap>
#include <QSqlDatabase>

namespace OmniView {

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

struct QueryFilter {
    QString rootDir;
    QString subfolder = QStringLiteral("__all__");
    QString colorName = QStringLiteral("all");
    QString aspectType = QStringLiteral("all");
    QString searchTerm;
    bool favoriteOnly = false;
    QString sortBy = QStringLiteral("mtime_desc");
};

struct Stats {
    int totalImages = 0;
    qint64 totalBytes = 0;
    int indexedImages = 0;
    int favoritesCount = 0;
};

class Database {
public:
    explicit Database(const QString& dbPath = QString());
    ~Database();

    bool initSchema();
    bool batchSyncFiles(const QVector<ImageRecord>& entries);
    int pruneMissingFiles(const QString& rootDir);
    QVector<ImageRecord> getUnindexedPaths();
    bool updateImageFeatures(const QString& path, int width, int height,
                             const QString& aspect, const QString& colorName,
                             int r, int g, int b, const QString& dhash,
                             const QString& thumbPath);
    bool toggleFavorite(const QString& path, bool* outNewVal = nullptr);
    QVector<ImageRecord> queryImages(const QueryFilter& filter);
    QVector<QPair<QString, int>> getSubfoldersWithCounts(const QString& rootDir);
    Stats getStats();
    QMap<QString, QString> getAllHashesForPaths(const QStringList& paths);

    QString dbPath() const { return m_dbPath; }

private:
    QSqlDatabase getDatabase();

    QString m_dbPath;
};

} // namespace OmniView
