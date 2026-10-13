#pragma once

#include <QPixmap>
#include <QString>
#include <QStringList>

class QMimeData;

namespace OmniView {

// Shared file operations for the gallery, the viewer and the batch toolbar
// (previously copied four times with diverging behaviour).
namespace FileActions {

// Maps gallery paths to real files: archive entries are extracted into the
// cache, missing files are dropped. Order is preserved.
QStringList resolveLocalFiles(const QStringList& paths);

// MIME data for dragging or copying files: file URLs, plain-text paths and
// the GNOME/KDE "x-special/gnome-copied-files" copy format. Caller owns it.
QMimeData* createFileMimeData(const QStringList& localFiles);

// Puts the files on the clipboard. Returns false if none of them exist.
bool copyFilesToClipboard(const QStringList& paths);

// Drag preview: the thumbnail on a rounded card, plus a "N files" pill.
QPixmap makeDragBadge(const QPixmap& thumbnail, int fileCount);

// Shows the file (or, for archive entries, the archive) in the platform file
// manager: Explorer on Windows, Finder on macOS, Dolphin/Nautilus/xdg-open on
// Linux.
bool revealInFileManager(const QString& path);

// Opens the file (extracting archive entries first) with the default app.
bool openWithDefaultApp(const QString& path);

// "&" in user-provided text would become a mnemonic in menu/button labels.
QString escapeMnemonics(QString text);

} // namespace FileActions
} // namespace OmniView
