#include "Scanner.h"
#include "Config.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>

namespace OmniView {

Scanner::Scanner(const QString& rootDir, const QString& dbPath, QObject* parent)
    : QThread(parent)
    , m_rootDir(rootDir)
    , m_dbPath(dbPath)
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
        emit finished(0, Stats());
        return;
    }

    Database db(m_dbPath);
    QVector<ImageRecord> batch;
    batch.reserve(500);
    int count = 0;

    const QDir rootQDir(m_rootDir);
    const QString absRoot = rootQDir.absolutePath();

    QDirIterator it(absRoot, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);

    while (it.hasNext() && !m_cancelled.load()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString fname = fi.fileName();

        if (fname.startsWith('.')) continue;

        const QString relPath = rootQDir.relativeFilePath(fi.absolutePath());
        if (relPath.startsWith('.') && relPath != QStringLiteral(".")) continue;
        if (relPath.contains(QStringLiteral("/."))) continue;

        const QString suffix = QStringLiteral(".") + fi.suffix().toLower();
        if (!Config::isSupportedExtension(suffix)) continue;

        ImageRecord rec;
        rec.path = fi.absoluteFilePath();
        rec.filename = fname;
        rec.subfolder = (relPath == QStringLiteral(".")) ? QString() : relPath;
        rec.fileSize = fi.size();
        rec.mtime = static_cast<double>(fi.lastModified().toSecsSinceEpoch());

        batch.append(rec);
        count++;

        if (batch.size() >= 500) {
            db.batchSyncFiles(batch);
            batch.clear();
            emit progress(count);
        }
    }

    if (!batch.isEmpty() && !m_cancelled.load()) {
        db.batchSyncFiles(batch);
        emit progress(count);
    }

    if (!m_cancelled.load()) {
        db.pruneMissingFiles(absRoot);
    }

    const Stats stats = db.getStats();
    emit finished(count, stats);
}

} // namespace OmniView
