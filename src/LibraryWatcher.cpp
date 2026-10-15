#include "LibraryWatcher.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace OmniView {

LibraryWatcher::LibraryWatcher(QObject* parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &LibraryWatcher::onDebounceTimeout);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &LibraryWatcher::onDirectoryChanged);
}

void LibraryWatcher::setRoot(const QString& rootDir) {
    const QString absRoot = rootDir.isEmpty() ? QString() : QDir(rootDir).absolutePath();
    if (absRoot != m_root) {
        cancelPending();
        m_root = absRoot;
    }
    refresh();
}

QStringList LibraryWatcher::collectDirectories() const {
    QStringList dirs;
    if (m_root.isEmpty()) return dirs;

    if (!QDir(m_root).exists()) {
        // Watch the closest existing ancestor to notice the root returning.
        QString ancestor = m_root;
        while (!QFileInfo(ancestor).isDir()) {
            const QString parent = QFileInfo(ancestor).absolutePath();
            if (parent == ancestor) return dirs;
            ancestor = parent;
        }
        dirs.append(ancestor);
        return dirs;
    }

    dirs.append(m_root);
    QDirIterator it(m_root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    const QDir rootDir(m_root);
    while (it.hasNext()) {
        const QString dir = it.next();
        const QString rel = rootDir.relativeFilePath(dir);
        if (rel.startsWith(QLatin1Char('.')) || rel.contains(QStringLiteral("/."))) continue;
        dirs.append(dir);
    }
    return dirs;
}

QStringList LibraryWatcher::watchedDirectories() const {
    QStringList dirs(m_watched.cbegin(), m_watched.cend());
    dirs.sort();
    return dirs;
}

void LibraryWatcher::refresh() {
    QStringList wanted = collectDirectories();
    m_unwatched.clear();
    if (wanted.size() > m_maxWatches) {
        m_unwatched = wanted.mid(m_maxWatches);
        wanted = wanted.mid(0, m_maxWatches);
    }
    const QSet<QString> target(wanted.cbegin(), wanted.cend());

    QStringList toRemove;
    for (const QString& dir : std::as_const(m_watched)) {
        if (!target.contains(dir)) toRemove.append(dir);
    }
    if (!toRemove.isEmpty()) {
        // A backend may refuse to remove a directory that no longer exists;
        // it can't produce events any more, so it is dropped either way.
        m_watcher.removePaths(toRemove);
        for (const QString& dir : std::as_const(toRemove)) m_watched.remove(dir);
    }

    QStringList toAdd;
    for (const QString& dir : std::as_const(wanted)) {
        if (!m_watched.contains(dir)) toAdd.append(dir);
    }
    if (!toAdd.isEmpty()) {
        QStringList failed = m_watcher.addPaths(toAdd);
        // "Already watched" (a stale backend entry for a re-created path)
        // is not a failure.
        const QStringList backendDirs = m_watcher.directories();
        failed.erase(std::remove_if(failed.begin(), failed.end(),
                                    [&](const QString& d) { return backendDirs.contains(d); }),
                     failed.end());
        for (const QString& dir : std::as_const(toAdd)) {
            if (!failed.contains(dir)) m_watched.insert(dir);
        }
        m_unwatched += failed;
    }
}

void LibraryWatcher::cancelPending() {
    m_debounce.stop();
}

void LibraryWatcher::onDirectoryChanged(const QString& /*path*/) {
    if (!m_debounce.isActive()) {
        m_firstEvent.start();
    }
    const qint64 waited = m_firstEvent.isValid() ? m_firstEvent.elapsed() : 0;
    const int remaining = static_cast<int>(qMax<qint64>(0, m_maxDelayMs - waited));
    m_debounce.start(qMin(m_debounceMs, remaining));
}

void LibraryWatcher::onDebounceTimeout() {
    m_firstEvent.invalidate();
    emit rescanRequested();
}

} // namespace OmniView
