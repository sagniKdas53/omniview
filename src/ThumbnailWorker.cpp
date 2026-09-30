#include "ThumbnailWorker.h"
#include "Config.h"

#include <QFileInfo>
#include <QFile>
#include <QMetaObject>
#include <QThread>

namespace OmniView {

ThumbnailTask::ThumbnailTask(const QString& sourcePath, const QString& dbPath, ThumbnailManager* manager)
    : m_sourcePath(sourcePath)
    , m_dbPath(dbPath)
    , m_manager(manager)
{
    setAutoDelete(true);
}

void ThumbnailTask::run() {
    const QString thumbPath = Config::getThumbPath(m_sourcePath);
    QFileInfo fi(thumbPath);
    Database db(m_dbPath);
    QueryFilter filter;
    filter.exactPath = m_sourcePath;
    const auto records = db.queryImages(filter);
    if (fi.exists() && fi.size() > 0 && !records.isEmpty() && records.first().indexed) {
        const auto& rec = records.first();
        ImageFeatures feat;
        feat.width = rec.width;
        feat.height = rec.height;
        feat.aspectType = rec.aspectType;
        feat.colorName = rec.colorName;
        feat.colorR = rec.colorR;
        feat.colorG = rec.colorG;
        feat.colorB = rec.colorB;
        feat.dhash = rec.dhash;
        feat.thumbPath = thumbPath;
        feat.valid = true;
        m_manager->notifyThumbnailReady(m_sourcePath, thumbPath, feat);
        return;
    }

    ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(
        m_sourcePath, thumbPath, QSize(Config::THUMBNAIL_WIDTH, Config::THUMBNAIL_HEIGHT)
    );

    if (feat.valid) {
        db.updateImageFeatures(
            m_sourcePath, feat.width, feat.height, feat.aspectType,
            feat.colorName, feat.colorR, feat.colorG, feat.colorB,
            feat.dhash, feat.thumbPath
        );
    }

    m_manager->notifyThumbnailReady(m_sourcePath, thumbPath, feat);
}

ThumbnailManager::ThumbnailManager(const QString& dbPath, QObject* parent)
    : QObject(parent)
    , m_dbPath(dbPath)
{
    m_threadPool.setMaxThreadCount(qMax(2, qMin(8, QThread::idealThreadCount())));
    m_memCache.setMaxCost(1500);
    connect(this, &ThumbnailManager::thumbnailReady, this, &ThumbnailManager::onThumbnailGenerated);
}

ThumbnailManager::~ThumbnailManager() {
    m_threadPool.waitForDone();
}

void ThumbnailManager::notifyThumbnailReady(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat) {
    QMetaObject::invokeMethod(this, [this, sourcePath, thumbPath, feat]() {
        bool completed = false;
        {
            QMutexLocker lock(&m_mutex);
            m_pending.remove(sourcePath);
            completed = m_pending.isEmpty() && m_isBackgroundRunning;
            if (completed) m_isBackgroundRunning = false;
        }
        emit thumbnailReady(sourcePath, thumbPath, feat);
        if (completed) emit allCompleted();
    }, Qt::QueuedConnection);
}

void ThumbnailManager::onThumbnailGenerated(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& /*feat*/) {
    QMutexLocker lock(&m_mutex);
    if (QFile::exists(thumbPath)) {
        // QPixmap(filename) can reuse a global cache entry after an in-place rewrite.
        QPixmap p = QPixmap::fromImage(QImage(thumbPath));
        if (!p.isNull()) {
            m_memCache.insert(sourcePath, new QPixmap(p));
        }
    }
}

QPixmap ThumbnailManager::getCachedPixmap(const QString& sourcePath) {
    QMutexLocker lock(&m_mutex);
    if (QPixmap* p = m_memCache.object(sourcePath)) {
        return *p;
    }
    return QPixmap();
}

void ThumbnailManager::storeCachedPixmap(const QString& sourcePath, const QPixmap& pixmap) {
    QMutexLocker lock(&m_mutex);
    m_memCache.insert(sourcePath, new QPixmap(pixmap));
}

void ThumbnailManager::requestPriorityThumbnail(const QString& sourcePath) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_pending.contains(sourcePath)) return;
        m_pending.insert(sourcePath);
    }
    m_threadPool.start(new ThumbnailTask(sourcePath, m_dbPath, this), 10);
}

void ThumbnailManager::startBackgroundIndexing() {
    m_isBackgroundRunning = true;
    Database db(m_dbPath);
    const QVector<ImageRecord> unindexed = db.getUnindexedPaths();
    for (const auto& rec : unindexed) {
        {
            QMutexLocker lock(&m_mutex);
            if (m_pending.contains(rec.path)) continue;
            m_pending.insert(rec.path);
        }
        m_threadPool.start(new ThumbnailTask(rec.path, m_dbPath, this));
    }
    bool completed = false;
    {
        QMutexLocker lock(&m_mutex);
        completed = m_pending.isEmpty();
        if (completed) m_isBackgroundRunning = false;
    }
    if (completed) emit allCompleted();
}

} // namespace OmniView
