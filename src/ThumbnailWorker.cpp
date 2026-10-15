#include "ThumbnailWorker.h"
#include "Config.h"

#include <QFileInfo>
#include <QFile>
#include <QImageReader>
#include <QMetaObject>
#include <QThread>

#include <utility>

namespace OmniView {

ThumbnailTask::ThumbnailTask(QString sourcePath, QString dbPath, ThumbnailManager* manager,
                             quint64 generation, int priority)
    : m_sourcePath(std::move(sourcePath))
    , m_dbPath(std::move(dbPath))
    , m_generation(generation)
    , m_priority(priority)
    , m_manager(nullptr)
{
    setAutoDelete(true);
    m_manager.store(manager, std::memory_order_release);
}

void ThumbnailTask::run() {
    ThumbnailManager* const manager = m_manager.load(std::memory_order_acquire);
    if (!manager->taskStarting(this)) {
        return; // cancelled after being dequeued
    }

    Database db(m_dbPath);
    QueryFilter filter;
    filter.exactPath = m_sourcePath;
    const auto records = db.queryImages(filter);
    if (records.isEmpty()) {
        // Row pruned since the task was queued: nothing to do.
        manager->taskFinished(m_sourcePath, ImageFeatures(), ThumbnailManager::Outcome::Stale, 0.0, 0);
        return;
    }
    const ImageRecord& rec = records.first();

    // Fast path: reuse an existing thumbnail, but only if it actually decodes
    // (another OmniView implementation may have left a truncated file).
    const QString existingThumb = Config::getThumbPath(m_sourcePath);
    if (rec.indexed && QFileInfo(existingThumb).size() > 0) {
        QImageReader reader(existingThumb);
        const QImage thumb = reader.read();
        if (!thumb.isNull()) {
            ImageFeatures feat;
            feat.width = rec.width;
            feat.height = rec.height;
            feat.aspectType = rec.aspectType;
            feat.colorName = rec.colorName;
            feat.colorR = rec.colorR;
            feat.colorG = rec.colorG;
            feat.colorB = rec.colorB;
            feat.dhash = rec.dhash;
            feat.thumbPath = existingThumb;
            feat.thumbnail = thumb;
            feat.valid = true;
            if (feat.dhash.isEmpty()) {
                // Hash was cleared (e.g. written by the broken Qt 5 dHash):
                // recompute it from the thumbnail instead of the source.
                ImageFeatures recomputed;
                ColorUtils::computeFeaturesFromThumbnail(thumb, recomputed);
                feat.dhash = recomputed.dhash;
                db.updateImageFeaturesIfUnchanged(m_sourcePath, rec.mtime, rec.fileSize, feat);
            }
            manager->taskFinished(m_sourcePath, feat, ThumbnailManager::Outcome::Ready,
                                    rec.mtime, rec.fileSize);
            return;
        }
    }

    // Known not to decode (possibly from an earlier session): report the
    // failure without decoding again until the file changes.
    if (db.hasDecodeFailure(m_sourcePath, rec.mtime, rec.fileSize)) {
        manager->taskFinished(m_sourcePath, ImageFeatures(), ThumbnailManager::Outcome::Failed,
                                rec.mtime, rec.fileSize);
        return;
    }

    // Always write the canonical shared-cache location; legacy paths are read-only.
    ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(
        m_sourcePath, Config::canonicalThumbPath(m_sourcePath),
        QSize(Config::THUMBNAIL_WIDTH, Config::THUMBNAIL_HEIGHT)
    );

    if (!feat.valid) {
        if (feat.decodeFailed) {
            db.recordDecodeFailure(m_sourcePath, rec.mtime, rec.fileSize);
        }
        manager->taskFinished(m_sourcePath, feat, ThumbnailManager::Outcome::Failed,
                                rec.mtime, rec.fileSize);
        return;
    }

    const Database::UpdateResult stored =
        db.updateImageFeaturesIfUnchanged(m_sourcePath, rec.mtime, rec.fileSize, feat);
    if (stored == Database::UpdateResult::Stale) {
        // The file changed while we decoded it; the rescan already queued the
        // new version, so drop this result instead of marking it current.
        feat.valid = false;
        manager->taskFinished(m_sourcePath, feat, ThumbnailManager::Outcome::Stale,
                                rec.mtime, rec.fileSize);
        return;
    }
    // On a DB error the thumbnail is still shown; the row stays unindexed and
    // is retried by the next background pass.
    manager->taskFinished(m_sourcePath, feat, ThumbnailManager::Outcome::Ready,
                            rec.mtime, rec.fileSize);
}

ThumbnailManager::ThumbnailManager(const QString& dbPath, QObject* parent)
    : QObject(parent)
    , m_dbPath(dbPath.isEmpty() ? Config::dbPath() : dbPath)
{
    m_threadPool.setMaxThreadCount(qMax(2, qMin(8, QThread::idealThreadCount())));
    m_memCache.setMaxCost(MEMORY_CACHE_KIB);
}

ThumbnailManager::~ThumbnailManager() {
    // Drop the queue first: waitForDone() would otherwise run every queued
    // task (potentially tens of thousands) before the process can exit.
    cancelPending();
    m_threadPool.waitForDone();
}

int ThumbnailManager::pendingCount() const {
    return static_cast<int>(m_pending.size());
}

void ThumbnailManager::cancelPending() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_generation;
    m_threadPool.clear(); // deletes queued (auto-delete) tasks
    for (auto it = m_queued.cbegin(); it != m_queued.cend(); ++it) {
        m_pending.remove(it.key());
    }
    m_queued.clear();
}

bool ThumbnailManager::taskStarting(ThumbnailTask* task) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_queued.find(task->sourcePath());
    if (it != m_queued.end() && it.value() == task) {
        m_queued.erase(it);
    }
    return task->generation() == m_generation;
}

void ThumbnailManager::taskFinished(const QString& sourcePath, const ImageFeatures& feat,
                                    Outcome outcome, double mtime, qint64 fileSize) {
    QMetaObject::invokeMethod(this, [this, sourcePath, feat, outcome, mtime, fileSize]() {
        handleResult(sourcePath, feat, outcome, mtime, fileSize);
    }, Qt::QueuedConnection);
}

void ThumbnailManager::handleResult(const QString& sourcePath, const ImageFeatures& feat,
                                    Outcome outcome, double mtime, qint64 fileSize) {
    m_pending.remove(sourcePath);
    if (outcome == Outcome::Failed) {
        // Remembered until the file changes, so a broken image is not
        // re-decoded on every repaint and every rescan.
        m_failed.insert(sourcePath, FileVersion{mtime, fileSize});
    } else if (outcome == Outcome::Ready) {
        m_failed.remove(sourcePath);
    }

    if (feat.valid && !feat.thumbnail.isNull()) {
        const QImage& img = feat.thumbnail;
        const int costKiB = qMax(1, static_cast<int>(static_cast<qint64>(img.width()) * img.height() * 4 / 1024));
        m_memCache.insert(sourcePath, new QPixmap(QPixmap::fromImage(img)), costKiB);
    }

    const bool completed = m_isBackgroundRunning && m_pending.isEmpty();
    if (completed) m_isBackgroundRunning = false;

    ImageFeatures published = feat;
    published.thumbnail = QImage(); // the pixmap cache owns the pixels now
    emit thumbnailReady(sourcePath, feat.thumbPath, published);
    if (completed) emit allCompleted();
}

QPixmap ThumbnailManager::getCachedPixmap(const QString& sourcePath) const {
    if (const QPixmap* p = m_memCache.object(sourcePath)) {
        return *p;
    }
    return QPixmap();
}

void ThumbnailManager::enqueue(const QString& sourcePath, int priority) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (ThumbnailTask* queued = m_queued.value(sourcePath)) {
        // Already waiting at background priority: promote it instead of
        // leaving the visible card behind the whole background queue.
        if (queued->priority() < priority && m_threadPool.tryTake(queued)) {
            queued->setPriority(priority);
            m_threadPool.start(queued, priority);
        }
        return;
    }
    if (m_pending.contains(sourcePath)) {
        return; // running right now
    }
    auto* task = new ThumbnailTask(sourcePath, m_dbPath, this, m_generation, priority);
    m_queued.insert(sourcePath, task);
    m_pending.insert(sourcePath);
    m_threadPool.start(task, priority);
}

void ThumbnailManager::requestPriorityThumbnail(const QString& sourcePath) {
    if (sourcePath.isEmpty() || m_failed.contains(sourcePath)) {
        return;
    }
    enqueue(sourcePath, PRIORITY_VISIBLE);
}

void ThumbnailManager::startBackgroundIndexing(const QString& rootDir) {
    m_isBackgroundRunning = true;
    Database db(m_dbPath);
    const QVector<ImageRecord> unindexed = db.getUnindexedPaths(rootDir);
    for (const auto& rec : unindexed) {
        const auto failed = m_failed.constFind(rec.path);
        if (failed != m_failed.cend()) {
            if (qAbs(failed->mtime - rec.mtime) < Database::MTIME_TOLERANCE_SECONDS
                && failed->fileSize == rec.fileSize) {
                continue; // same broken file as last time
            }
            m_failed.remove(rec.path); // file changed: try again
        }
        enqueue(rec.path, PRIORITY_BACKGROUND);
    }
    if (m_pending.isEmpty()) {
        m_isBackgroundRunning = false;
        emit allCompleted();
    }
}

} // namespace OmniView
