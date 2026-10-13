#pragma once

#include <QObject>
#include <QRunnable>
#include <QThreadPool>
#include <QSet>
#include <QHash>
#include <QCache>
#include <QPixmap>
#include <atomic>
#include <mutex>
#include "ColorUtils.h"
#include "Database.h"

namespace OmniView {

class ThumbnailManager;

// Generates (or loads) one thumbnail on a pool thread. Holds a raw pointer to
// its manager: ThumbnailManager's destructor cancels queued tasks and waits for
// running ones, so a task never outlives it.
class ThumbnailTask : public QRunnable {
public:
    ThumbnailTask(QString sourcePath, QString dbPath, ThumbnailManager* manager,
                  quint64 generation, int priority);
    void run() override;

    QString sourcePath() const { return m_sourcePath; }
    int priority() const { return m_priority; }
    void setPriority(int priority) { m_priority = priority; }
    quint64 generation() const { return m_generation; }

private:
    QString m_sourcePath;
    QString m_dbPath;
    quint64 m_generation;
    int m_priority;
    // Stored last with release semantics and loaded first in run() with
    // acquire semantics: makes the hand-off to the pool thread an explicit
    // happens-before edge (QThreadPool's own synchronisation is inside Qt).
    std::atomic<ThumbnailManager*> m_manager;
};

// Owns the thumbnail thread pool and the in-memory pixmap cache.
// Threading contract: every public method must be called on the thread that
// owns the manager (the GUI thread). Tasks only touch the manager through
// taskStarting()/taskFinished(), which are thread-safe; results are delivered
// back to the owner thread with a queued call, so QPixmaps are only ever
// created there.
class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    static constexpr int PRIORITY_VISIBLE = 10;
    static constexpr int PRIORITY_BACKGROUND = 0;
    // Pixmap cache budget in KiB (cost of each entry is its size in KiB).
    static constexpr int MEMORY_CACHE_KIB = 96 * 1024;

    enum class Outcome { Ready, Failed, Stale };

    explicit ThumbnailManager(const QString& dbPath = QString(), QObject* parent = nullptr);
    ~ThumbnailManager() override;

    Q_DISABLE_COPY_MOVE(ThumbnailManager)

    // Queues (or promotes an already queued background task) at visible priority.
    void requestPriorityThumbnail(const QString& sourcePath);
    // Queues every row under rootDir (all rows if empty) that still needs a thumbnail.
    void startBackgroundIndexing(const QString& rootDir = QString());
    // Drops every queued (not yet running) task, e.g. when the root changes.
    void cancelPending();

    QPixmap getCachedPixmap(const QString& sourcePath) const;
    bool hasFailed(const QString& sourcePath) const { return m_failed.contains(sourcePath); }
    int pendingCount() const;

signals:
    void thumbnailReady(const QString& sourcePath, const QString& thumbPath, const OmniView::ImageFeatures& feat);
    void allCompleted();

private:
    friend class ThumbnailTask;
    struct FileVersion {
        double mtime = 0.0;
        qint64 fileSize = 0;
    };

    void enqueue(const QString& sourcePath, int priority);
    bool taskStarting(ThumbnailTask* task);
    void taskFinished(const QString& sourcePath, const ImageFeatures& feat, Outcome outcome,
                      double mtime, qint64 fileSize);
    void handleResult(const QString& sourcePath, const ImageFeatures& feat, Outcome outcome,
                      double mtime, qint64 fileSize);

    QString m_dbPath;
    QThreadPool m_threadPool;

    // Guarded by m_mutex (touched from pool threads in taskStarting()).
    // std::mutex rather than QMutex so ThreadSanitizer can see the locking
    // on Qt 5 too (Qt 5's QMutex is not annotated for TSan).
    mutable std::mutex m_mutex;
    QHash<QString, ThumbnailTask*> m_queued; // queued, not yet started
    quint64 m_generation = 0;

    // Owner-thread only.
    QSet<QString> m_pending;                  // queued or running
    QHash<QString, FileVersion> m_failed;     // decode failures, keyed to the file version
    QCache<QString, QPixmap> m_memCache;
    bool m_isBackgroundRunning = false;
};

} // namespace OmniView
