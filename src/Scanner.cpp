#include "Scanner.h"
#include "Config.h"
#include "ZipUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

#include <utility>

namespace OmniView {

namespace {
constexpr int kBatchSize = 500;
}

Scanner::Scanner(QString rootDir, QString dbPath, QObject* parent)
    : QThread(parent)
    , m_rootDir(std::move(rootDir))
    , m_dbPath(std::move(dbPath))
{
}

Scanner::~Scanner() {
    cancel();
    wait();
}

void Scanner::cancel() {
    m_cancelled.store(true);
}

void Scanner::run() {
    if (m_rootDir.isEmpty() || !QDir(m_rootDir).exists()) {
        if (!m_cancelled.load()) {
            emit scanFinished(0, Stats(), QStringLiteral("Folder does not exist: %1").arg(m_rootDir));
        }
        return;
    }

    Database db(m_dbPath);
    QVector<ImageRecord> batch;
    batch.reserve(kBatchSize);
    ScanObservation observation;
    int count = 0;
    QString error;

    const QDir rootQDir(m_rootDir);
    const QString absRoot = rootQDir.absolutePath();

    auto flush = [&]() -> bool {
        if (batch.isEmpty()) return true;
        if (!db.batchSyncFiles(batch)) {
            error = QStringLiteral("Database write failed: %1").arg(db.lastError());
            return false;
        }
        batch.clear();
        emit progress(count);
        return true;
    };

    auto add = [&](ImageRecord rec) -> bool {
        observation.seenPaths.insert(rec.path);
        batch.append(std::move(rec));
        ++count;
        return batch.size() < kBatchSize || flush();
    };

    // Symlinks are not followed, which also rules out symlink loops.
    QDirIterator it(absRoot, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);

    while (it.hasNext() && !m_cancelled.load() && error.isEmpty()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString fname = fi.fileName();

        if (fname.startsWith(QLatin1Char('.'))) continue;

        const QString relPath = rootQDir.relativeFilePath(fi.absolutePath());
        if (relPath.startsWith(QLatin1Char('.')) && relPath != QStringLiteral(".")) continue;
        if (relPath.contains(QStringLiteral("/."))) continue;
        const QString subfolder = (relPath == QStringLiteral(".")) ? QString() : relPath;

        const QString suffix = QStringLiteral(".") + fi.suffix().toLower();

        if (suffix == QStringLiteral(".zip")) {
            const QString zipAbs = fi.absoluteFilePath();
            const QString relZip = subfolder.isEmpty() ? fname : (subfolder + QStringLiteral("/") + fname);
            const QString zipStem = fi.completeBaseName();
            const QString zipDirPath = fi.absolutePath();
            bool opened = false;
            const QVector<ZipImageEntry> zipEntries = ZipUtils::listZipImages(zipAbs, &opened);
            if (opened) {
                observation.listedArchives.insert(zipAbs);
            }

            for (const auto& ze : zipEntries) {
                if (m_cancelled.load()) break;

                // Prefer the raw extracted copy when one exists.
                if (ZipUtils::isDuplicateOfRaw(zipDirPath, zipStem, ze.innerPath, ze.uncompressedSize)) {
                    continue;
                }

                ImageRecord rec;
                rec.path = ZipUtils::makeZipPath(zipAbs, ze.innerPath);
                rec.filename = QFileInfo(ze.innerPath).fileName();
                rec.subfolder = relZip;
                rec.fileSize = ze.uncompressedSize;
                rec.mtime = (ze.mtime > 0) ? static_cast<double>(ze.mtime)
                                           : static_cast<double>(fi.lastModified().toSecsSinceEpoch());
                if (!add(std::move(rec))) break;
            }
            continue;
        }

        if (!Config::isSupportedExtension(suffix)) continue;

        ImageRecord rec;
        rec.path = fi.absoluteFilePath();
        rec.filename = fname;
        rec.subfolder = subfolder.isNull() ? QStringLiteral("") : subfolder;
        rec.fileSize = fi.size();
        rec.mtime = static_cast<double>(fi.lastModified().toSecsSinceEpoch());
        add(std::move(rec));
    }

    if (m_cancelled.load()) return;
    if (error.isEmpty()) flush();

    // Only prune after a complete walk of a root that still exists: a drive
    // that disappeared mid-scan must not wipe the index (and favourites).
    if (error.isEmpty() && QDir(absRoot).exists()) {
        db.pruneMissingFiles(absRoot, &observation, [this]() { return m_cancelled.load(); });
    }
    if (m_cancelled.load()) return;

    const Stats stats = db.getStats(absRoot);
    if (!error.isEmpty()) {
        qWarning() << "Scan of" << absRoot << "failed:" << error;
    }
    emit scanFinished(count, stats, error);
}

} // namespace OmniView
