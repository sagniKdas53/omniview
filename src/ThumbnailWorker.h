#pragma once

#include <QObject>
#include <QRunnable>
#include <QThreadPool>
#include <QSet>
#include <QMutex>
#include <QCache>
#include <QPixmap>
#include "ColorUtils.h"
#include "Database.h"

namespace OmniView {

class ThumbnailManager;

class ThumbnailTask : public QRunnable {
public:
    ThumbnailTask(const QString& sourcePath, const QString& dbPath, ThumbnailManager* manager);
    void run() override;

private:
    QString m_sourcePath;
    QString m_dbPath;
    ThumbnailManager* m_manager;
};

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailManager(const QString& dbPath = QString(), QObject* parent = nullptr);
    ~ThumbnailManager() override;

    void requestPriorityThumbnail(const QString& sourcePath);
    void startBackgroundIndexing();

    QPixmap getCachedPixmap(const QString& sourcePath);
    void storeCachedPixmap(const QString& sourcePath, const QPixmap& pixmap);

signals:
    void thumbnailReady(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat);
    void allCompleted();

private slots:
    void onThumbnailGenerated(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat);

private:
    friend class ThumbnailTask;
    void notifyThumbnailReady(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat);

    QString m_dbPath;
    QThreadPool m_threadPool;
    QSet<QString> m_pending;
    QMutex m_mutex;
    QCache<QString, QPixmap> m_memCache;
    bool m_isBackgroundRunning = false;
};

} // namespace OmniView
