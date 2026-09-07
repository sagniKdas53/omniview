#include "ZipUtils.h"
#include "Config.h"
#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <limits>
#include <cstring>

namespace OmniView {

QString ZipUtils::generateUuidV7() {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const quint16 randA = static_cast<quint16>(QRandomGenerator::global()->generate() & 0x0FFF);
    const quint16 verAndRandA = 0x7000 | randA; // version 7
    const quint16 randB = static_cast<quint16>(QRandomGenerator::global()->generate());
    const quint16 varAndRandB = 0x8000 | (randB & 0x3FFF); // variant 1 (RFC 4122 / 9562)
    const quint32 randC = QRandomGenerator::global()->generate();
    const quint16 randD = static_cast<quint16>(QRandomGenerator::global()->generate());

    return QString::asprintf("%08x-%04x-%04x-%04x-%08x%04x",
        static_cast<quint32>((nowMs >> 16) & 0xFFFFFFFF),
        static_cast<quint16>(nowMs & 0xFFFF),
        verAndRandA,
        varAndRandB,
        randC,
        randD
    );
}

bool ZipUtils::isSafeInnerPath(const QString& innerPath) {
    if (innerPath.isEmpty()) return false;
    QString normalized = innerPath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // Reject absolute paths and drive letters (e.g. /etc or C:/)
    if (normalized.startsWith(QLatin1Char('/'))) return false;
    if (normalized.length() >= 2 && normalized.at(1) == QLatin1Char(':')) return false;

    // Check path segments for directory traversal
    const QStringList segments = normalized.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& seg : segments) {
        if (seg == QStringLiteral("..") || seg == QStringLiteral(".")) {
            return false;
        }
    }
    return true;
}

bool ZipUtils::isZipPath(const QString& path, QString* outZipPath, QString* outInnerPath) {
    const int hashIdx = path.indexOf(QLatin1Char('#'));
    if (hashIdx <= 0) return false;

    const QString zip = path.left(hashIdx);
    if (!zip.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        return false;
    }

    if (outZipPath) *outZipPath = zip;
    if (outInnerPath) *outInnerPath = path.mid(hashIdx + 1);
    return true;
}

QString ZipUtils::makeZipPath(const QString& zipPath, const QString& innerPath) {
    return zipPath + QLatin1Char('#') + innerPath;
}

bool ZipUtils::isDuplicateOfRaw(const QString& zipDirPath, const QString& zipStem, const QString& innerPath) {
    if (!isSafeInnerPath(innerPath)) return false;

    QString innerClean = innerPath;
    innerClean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString stemSlash = zipStem + QStringLiteral("/");
    if (innerClean.startsWith(stemSlash, Qt::CaseInsensitive)) {
        innerClean = innerClean.mid(stemSlash.length());
    }

    const QString cand1 = zipDirPath + QStringLiteral("/") + zipStem + QStringLiteral("/") + innerClean;
    const QString cand2 = zipDirPath + QStringLiteral("/") + innerPath;

    return QFile::exists(cand1) || QFile::exists(cand2);
}

QVector<ZipImageEntry> ZipUtils::listZipImages(const QString& zipPath) {
    QVector<ZipImageEntry> results;

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    const QByteArray utf8Path = zipPath.toUtf8();
    if (!mz_zip_reader_init_file(&zip, utf8Path.constData(), 0)) {
        return results;
    }

    const mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    results.reserve(static_cast<int>(numFiles));

    for (mz_uint i = 0; i < numFiles; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        const QString filename = QString::fromUtf8(stat.m_filename);
        if (!isSafeInnerPath(filename)) {
            continue;
        }
        if (filename.startsWith(QStringLiteral("__MACOSX/"))) {
            continue;
        }

        const QFileInfo fi(filename);
        if (fi.fileName().startsWith(QLatin1Char('.'))) {
            continue;
        }

        const QString suffix = QStringLiteral(".") + fi.suffix().toLower();
        if (!Config::isSupportedExtension(suffix)) {
            continue;
        }

        if (stat.m_uncomp_size > static_cast<mz_uint64>(MAX_UNCOMPRESSED_ENTRY_SIZE)) {
            continue;
        }

        ZipImageEntry entry;
        entry.innerPath = filename;
        entry.uncompressedSize = static_cast<qint64>(stat.m_uncomp_size);
        entry.mtime = static_cast<qint64>(stat.m_time);
        results.append(entry);
    }

    mz_zip_reader_end(&zip);
    return results;
}

QByteArray ZipUtils::readZipEntryBytes(const QString& zipPath, const QString& innerPath) {
    if (!isSafeInnerPath(innerPath)) {
        return QByteArray();
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    const QByteArray utf8Zip = zipPath.toUtf8();
    if (!mz_zip_reader_init_file(&zip, utf8Zip.constData(), 0)) {
        return QByteArray();
    }

    const QByteArray utf8Inner = innerPath.toUtf8();
    int fileIdx = mz_zip_reader_locate_file(&zip, utf8Inner.constData(), nullptr, 0);
    if (fileIdx < 0) {
        // Try replacing backslashes with forward slashes if any
        QString alt = innerPath;
        alt.replace(QLatin1Char('\\'), QLatin1Char('/'));
        fileIdx = mz_zip_reader_locate_file(&zip, alt.toUtf8().constData(), nullptr, 0);
    }

    if (fileIdx < 0) {
        mz_zip_reader_end(&zip);
        return QByteArray();
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(fileIdx), &stat)) {
        mz_zip_reader_end(&zip);
        return QByteArray();
    }

    if (stat.m_uncomp_size > static_cast<mz_uint64>(MAX_UNCOMPRESSED_ENTRY_SIZE)) {
        mz_zip_reader_end(&zip);
        return QByteArray();
    }

    size_t uncompSize = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(fileIdx), &uncompSize, 0);
    if (!data) {
        mz_zip_reader_end(&zip);
        return QByteArray();
    }

    if (uncompSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        mz_free(data);
        mz_zip_reader_end(&zip);
        return QByteArray();
    }

    QByteArray bytes(reinterpret_cast<const char*>(data), static_cast<int>(uncompSize));
    mz_free(data);
    mz_zip_reader_end(&zip);
    return bytes;
}

QString ZipUtils::extractedCacheDir() {
    return Config::cacheDir() + QStringLiteral("/extracted");
}

QString ZipUtils::ensureExtracted(const QString& zipCompositePath) {
    QString zipPath, innerPath;
    if (!isZipPath(zipCompositePath, &zipPath, &innerPath)) {
        return zipCompositePath;
    }

    const QString cacheDir = extractedCacheDir();
    QDir().mkpath(cacheDir);

    const QByteArray hash = QCryptographicHash::hash(zipCompositePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString prefix = QString::fromUtf8(hash.left(16));
    const QString ext = QFileInfo(innerPath).suffix();
    const QString targetFile = cacheDir + QStringLiteral("/") + prefix + (ext.isEmpty() ? QString() : (QStringLiteral(".") + ext));

    if (QFile::exists(targetFile) && QFileInfo(targetFile).size() > 0) {
        return targetFile;
    }

    const QByteArray bytes = readZipEntryBytes(zipPath, innerPath);
    if (bytes.isEmpty()) {
        return QString();
    }

    // Atomic write via UUID-v7 temporary file to eliminate race conditions
    const QString tmpFile = targetFile + QStringLiteral(".tmp.") + generateUuidV7();
    QFile out(tmpFile);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(bytes);
        out.flush();
        out.close();

        if (QFile::rename(tmpFile, targetFile)) {
            return targetFile;
        } else if (QFile::exists(targetFile) && QFileInfo(targetFile).size() > 0) {
            QFile::remove(tmpFile);
            return targetFile;
        } else {
            QFile::remove(targetFile);
            if (QFile::rename(tmpFile, targetFile)) {
                return targetFile;
            }
            QFile::remove(tmpFile);
        }
    }

    return QString();
}

} // namespace OmniView
