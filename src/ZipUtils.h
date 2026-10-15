#pragma once

#include <QString>
#include <QVector>
#include <QByteArray>

namespace OmniView {

struct ZipImageEntry {
    QString innerPath;
    qint64 uncompressedSize = 0;
    qint64 mtime = 0;
};

class ZipUtils {
public:
    // Composite paths look like "/dir/archive.zip#inner/name.png". The split
    // happens at the first ".zip#" (case-insensitive), so '#' elsewhere in the
    // directory or archive name is fine.
    static bool isZipPath(const QString& path, QString* outZipPath = nullptr, QString* outInnerPath = nullptr);
    static QString makeZipPath(const QString& zipPath, const QString& innerPath);
    // True if a raw copy of the entry exists next to the archive (either in a
    // folder named after the archive or directly beside it). When
    // expectedSize >= 0 the raw file must also have that size, so unrelated
    // files that merely share a name are not treated as duplicates.
    static bool isDuplicateOfRaw(const QString& zipDirPath, const QString& zipStem,
                                 const QString& innerPath, qint64 expectedSize = -1);
    // Lists image entries; *ok is false if the archive could not be opened
    // (as opposed to opening fine and containing no images).
    static QVector<ZipImageEntry> listZipImages(const QString& zipPath, bool* ok = nullptr);
    static QByteArray readZipEntryBytes(const QString& zipPath, const QString& innerPath);
    // Returns a real file for a composite path (extracting it into the cache
    // under its original file name) or the input unchanged for plain paths.
    static QString ensureExtracted(const QString& zipCompositePath);
    static QString generateUuidV7();
    // Rejects traversal ("..") and absolute/drive-letter paths.
    static bool isSafeInnerPath(const QString& innerPath);
    // True for entries inside hidden folders or hidden files (".x", "__MACOSX").
    static bool isHiddenInnerPath(const QString& innerPath);
    static QString extractedCacheDir();

    static constexpr qint64 MAX_UNCOMPRESSED_ENTRY_SIZE = 128LL * 1024 * 1024; // per entry
    // Entries above this size must not be compressed more than MAX_RATIO:1;
    // real image formats are already compressed, so huge ratios mean a bomb.
    static constexpr qint64 RATIO_CHECK_THRESHOLD = 32LL * 1024 * 1024;
    static constexpr qint64 MAX_COMPRESSION_RATIO = 200;
};

} // namespace OmniView
