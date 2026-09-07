#include "ZipUtils.h"
#include "Config.h"
#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <cstring>

namespace OmniView {

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
    QString innerClean = innerPath;
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
        if (filename.startsWith(QStringLiteral("__MACOSX/")) || filename.contains(QStringLiteral("/."))) {
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

    size_t uncompSize = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(fileIdx), &uncompSize, 0);
    if (!data) {
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

    QFile out(targetFile);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(bytes);
        out.close();
        return targetFile;
    }

    return QString();
}

} // namespace OmniView
