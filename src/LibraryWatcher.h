#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace OmniView {

// Watches a library root for changes and asks for a rescan, debounced.
//
// - Watches the root and its non-hidden subdirectories recursively (symlinks
//   are not followed, matching the Scanner), up to maxWatches directories so
//   huge trees cannot exhaust inotify's per-user watch limit.
// - Re-arming is diffed: unchanged directories keep their watch, so events
//   that are already queued for them are not lost.
// - Bursts are coalesced: each event restarts the debounce timer, but a rescan
//   is forced after maxDelay so a steady stream of events cannot postpone it
//   forever.
// - If the root disappears, the nearest existing ancestor is watched so the
//   root coming back (remount, restore) still triggers a rescan.
class LibraryWatcher : public QObject {
    Q_OBJECT
public:
    explicit LibraryWatcher(QObject* parent = nullptr);

    void setRoot(const QString& rootDir);
    QString root() const { return m_root; }

    // Re-reads the directory tree and updates the watch set (call after each scan).
    void refresh();
    // Cancels a pending debounced rescan (e.g. a manual rescan just started).
    void cancelPending();

    void setDebounceInterval(int ms) { m_debounceMs = ms; }
    void setMaxDelay(int ms) { m_maxDelayMs = ms; }
    void setMaxWatches(int count) { m_maxWatches = count; }

    QStringList watchedDirectories() const { return m_watcher.directories(); }
    // Directories that could not be watched (e.g. inotify limit reached) or
    // were left out because of maxWatches. Non-empty means live updates are
    // incomplete.
    QStringList unwatchedDirectories() const { return m_unwatched; }
    bool isPending() const { return m_debounce.isActive(); }

signals:
    void rescanRequested();

private:
    void onDirectoryChanged(const QString& path);
    void onDebounceTimeout();
    QStringList collectDirectories() const;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QElapsedTimer m_firstEvent;
    QString m_root;
    QStringList m_unwatched;
    int m_debounceMs = 1200;
    int m_maxDelayMs = 10000;
    int m_maxWatches = 4096;
};

} // namespace OmniView
