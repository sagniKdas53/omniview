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
    static bool isZipPath(const QString& path, QString* outZipPath = nullptr, QString* outInnerPath = nullptr);
    static QString makeZipPath(const QString& zipPath, const QString& innerPath);
    static bool isDuplicateOfRaw(const QString& zipDirPath, const QString& zipStem, const QString& innerPath);
    static QVector<ZipImageEntry> listZipImages(const QString& zipPath);
    static QByteArray readZipEntryBytes(const QString& zipPath, const QString& innerPath);
    static QString ensureExtracted(const QString& zipCompositePath);
    static QString generateUuidV7();
    static bool isSafeInnerPath(const QString& innerPath);
    static QString extractedCacheDir();

    static constexpr qint64 MAX_UNCOMPRESSED_ENTRY_SIZE = 128 * 1024 * 1024; // 128 MB max per entry
};

} // namespace OmniView
