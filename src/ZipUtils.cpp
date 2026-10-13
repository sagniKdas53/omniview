#include "ZipUtils.h"
#include "Config.h"
#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSaveFile>
#include <cstring>
#include <vector>

namespace OmniView {

namespace {

// IBM code page 437, bytes 0x80-0xFF. The ZIP spec's default name encoding
// when general-purpose bit 11 (UTF-8) is not set.
const char16_t kCp437High[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

constexpr mz_uint kUtf8NameFlag = 1u << 11;

QString decodeCp437(const QByteArray& raw) {
    QString out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out.append(b < 0x80 ? QChar(b) : QChar(kCp437High[b - 0x80]));
    }
    return out;
}

bool isValidUtf8(const QByteArray& raw) {
    const QString decoded = QString::fromUtf8(raw);
    return !decoded.contains(QChar(QChar::ReplacementCharacter)) && decoded.toUtf8() == raw;
}

// Decodes an entry name the way the archive declared it. Many tools write
// UTF-8 without setting bit 11, so valid UTF-8 is accepted either way and
// CP437 is only used for byte sequences that are not UTF-8.
QString decodeEntryName(const QByteArray& raw, mz_uint bitFlags) {
    if ((bitFlags & kUtf8NameFlag) || isValidUtf8(raw)) {
        return QString::fromUtf8(raw);
    }
    return decodeCp437(raw);
}

QByteArray rawEntryName(mz_zip_archive* zip, mz_uint index) {
    // m_filename in the stat struct is truncated to 511 bytes.
    const mz_uint needed = mz_zip_reader_get_filename(zip, index, nullptr, 0);
    if (needed <= 1) return QByteArray();
    std::vector<char> buf(needed);
    mz_zip_reader_get_filename(zip, index, buf.data(), needed);
    return QByteArray(buf.data());
}

bool isSymlinkEntry(const mz_zip_archive_file_stat& stat) {
    const bool madeOnUnix = ((stat.m_version_made_by >> 8) & 0xFF) == 3;
    return madeOnUnix && ((stat.m_external_attr >> 16) & 0xF000) == 0xA000;
}

bool exceedsSizeLimits(const mz_zip_archive_file_stat& stat) {
    const auto uncompressed = static_cast<qint64>(stat.m_uncomp_size);
    const auto compressed = static_cast<qint64>(stat.m_comp_size);
    if (uncompressed > ZipUtils::MAX_UNCOMPRESSED_ENTRY_SIZE) return true;
    return uncompressed > ZipUtils::RATIO_CHECK_THRESHOLD
           && compressed * ZipUtils::MAX_COMPRESSION_RATIO < uncompressed;
}

class ZipReader {
public:
    explicit ZipReader(const QString& zipPath) {
        std::memset(&m_zip, 0, sizeof(m_zip));
        m_open = mz_zip_reader_init_file(&m_zip, zipPath.toUtf8().constData(), 0);
    }
    ~ZipReader() {
        if (m_open) mz_zip_reader_end(&m_zip);
    }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
    ZipReader(ZipReader&&) = delete;
    ZipReader& operator=(ZipReader&&) = delete;

    bool isOpen() const { return m_open; }
    mz_zip_archive* get() { return &m_zip; }

    // Exact (case-sensitive, decoded-name) lookup. miniz's locate_file with
    // flags 0 is case-insensitive and compares raw bytes.
    int findEntry(const QString& innerPath) {
        const mz_uint count = mz_zip_reader_get_num_files(&m_zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&m_zip, i, &stat)) continue;
            if (decodeEntryName(rawEntryName(&m_zip, i), stat.m_bit_flag) == innerPath) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

private:
    mz_zip_archive m_zip{};
    bool m_open = false;
};

QString safeFileName(const QString& innerPath) {
    QString normalized = innerPath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString name = normalized.section(QLatin1Char('/'), -1);
    static const QString forbidden = QStringLiteral("<>:\"|?*");
    for (QChar& c : name) {
        if (forbidden.contains(c) || c.unicode() < 0x20) c = QLatin1Char('_');
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        name = QStringLiteral("image");
    }
    return name;
}

} // namespace

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

    // Absolute paths and drive letters (e.g. /etc or C:/). "1:30.png" is a
    // legal relative name on Unix and is allowed.
    if (normalized.startsWith(QLatin1Char('/'))) return false;
    if (normalized.length() >= 2 && normalized.at(1) == QLatin1Char(':')
        && normalized.at(0).isLetter() && normalized.at(0).unicode() < 0x80) {
        return false;
    }

    const QStringList segments = normalized.split(QLatin1Char('/'));
    for (const QString& seg : segments) {
        if (seg == QStringLiteral("..")) {
            return false;
        }
    }
    return true;
}

bool ZipUtils::isHiddenInnerPath(const QString& innerPath) {
    QString normalized = innerPath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = normalized.split(QLatin1Char('/'));
    for (const QString& seg : segments) {
        if (seg.isEmpty() || seg == QStringLiteral(".")) continue;
        if (seg.startsWith(QLatin1Char('.')) || seg == QStringLiteral("__MACOSX")) return true;
    }
    return false;
}

bool ZipUtils::isZipPath(const QString& path, QString* outZipPath, QString* outInnerPath) {
    static const QString marker = QStringLiteral(".zip#");
    const int idx = static_cast<int>(path.indexOf(marker, 0, Qt::CaseInsensitive));
    if (idx <= 0) return false;

    const int zipEnd = idx + 4; // up to and including ".zip"
    if (outZipPath) *outZipPath = path.left(zipEnd);
    if (outInnerPath) *outInnerPath = path.mid(zipEnd + 1);
    return true;
}

QString ZipUtils::makeZipPath(const QString& zipPath, const QString& innerPath) {
    return zipPath + QLatin1Char('#') + innerPath;
}

bool ZipUtils::isDuplicateOfRaw(const QString& zipDirPath, const QString& zipStem,
                                const QString& innerPath, qint64 expectedSize) {
    if (!isSafeInnerPath(innerPath)) return false;

    QString innerClean = innerPath;
    innerClean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString innerBeside = innerClean;
    const QString stemSlash = zipStem + QStringLiteral("/");
    if (innerClean.startsWith(stemSlash, Qt::CaseInsensitive)) {
        innerClean = innerClean.mid(stemSlash.length());
    }

    const QStringList candidates = {
        zipDirPath + QStringLiteral("/") + zipStem + QStringLiteral("/") + innerClean,
        zipDirPath + QStringLiteral("/") + innerBeside,
    };
    for (const QString& candidate : candidates) {
        const QFileInfo fi(candidate);
        if (fi.isFile() && (expectedSize < 0 || fi.size() == expectedSize)) {
            return true;
        }
    }
    return false;
}

QVector<ZipImageEntry> ZipUtils::listZipImages(const QString& zipPath, bool* ok) {
    QVector<ZipImageEntry> results;
    ZipReader reader(zipPath);
    if (ok) *ok = reader.isOpen();
    if (!reader.isOpen()) {
        return results;
    }
    mz_zip_archive* zip = reader.get();

    const mz_uint numFiles = mz_zip_reader_get_num_files(zip);
    results.reserve(static_cast<int>(numFiles));

    for (mz_uint i = 0; i < numFiles; ++i) {
        if (mz_zip_reader_is_file_a_directory(zip, i)) {
            continue;
        }

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(zip, i, &stat)) {
            continue;
        }
        // Entries miniz cannot extract (encrypted, bzip2/LZMA, ...), empty
        // entries and symlinks would become images that can never load.
        if (stat.m_is_encrypted || !stat.m_is_supported || stat.m_uncomp_size == 0
            || isSymlinkEntry(stat)) {
            continue;
        }
        if (exceedsSizeLimits(stat)) {
            continue;
        }

        const QString filename = decodeEntryName(rawEntryName(zip, i), stat.m_bit_flag);
        if (!isSafeInnerPath(filename) || isHiddenInnerPath(filename)) {
            continue;
        }

        const QString suffix = QStringLiteral(".") + QFileInfo(filename).suffix().toLower();
        if (!Config::isSupportedExtension(suffix)) {
            continue;
        }

        ZipImageEntry entry;
        entry.innerPath = filename;
        entry.uncompressedSize = static_cast<qint64>(stat.m_uncomp_size);
        entry.mtime = static_cast<qint64>(stat.m_time);
        results.append(entry);
    }

    return results;
}

QByteArray ZipUtils::readZipEntryBytes(const QString& zipPath, const QString& innerPath) {
    if (!isSafeInnerPath(innerPath)) {
        return QByteArray();
    }

    ZipReader reader(zipPath);
    if (!reader.isOpen()) {
        return QByteArray();
    }

    const int fileIdx = reader.findEntry(innerPath);
    if (fileIdx < 0) {
        return QByteArray();
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(reader.get(), static_cast<mz_uint>(fileIdx), &stat)) {
        return QByteArray();
    }

    const auto uncompressed = static_cast<qint64>(stat.m_uncomp_size);
    if (uncompressed <= 0 || exceedsSizeLimits(stat) || stat.m_is_encrypted || !stat.m_is_supported) {
        return QByteArray();
    }

    // Decompress straight into the result: one allocation of the entry size
    // instead of a miniz heap buffer plus a QByteArray copy.
    QByteArray bytes(static_cast<int>(uncompressed), Qt::Uninitialized);
    if (!mz_zip_reader_extract_to_mem(reader.get(), static_cast<mz_uint>(fileIdx),
                                      bytes.data(), static_cast<size_t>(bytes.size()), 0)) {
        return QByteArray();
    }
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

    const QFileInfo zipInfo(zipPath);
    if (!zipInfo.isFile()) {
        return QString();
    }

    // The key covers the archive's size and mtime so a replaced archive never
    // serves the previous version's bytes.
    const QByteArray keySource = zipCompositePath.toUtf8() + '\n'
        + QByteArray::number(zipInfo.size()) + '\n'
        + QByteArray::number(zipInfo.lastModified().toMSecsSinceEpoch());
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(keySource, QCryptographicHash::Sha256).toHex().left(24));

    // Keep the entry's own file name so drag targets (chat apps, file
    // managers) see "page_001.png", not a hash.
    const QString targetDir = extractedCacheDir() + QLatin1Char('/') + key;
    QString targetFile = targetDir + QLatin1Char('/') + safeFileName(innerPath);

    if (QFileInfo(targetFile).isFile()) {
        return targetFile;
    }

    const QByteArray bytes = readZipEntryBytes(zipPath, innerPath);
    if (bytes.isEmpty() || !QDir().mkpath(targetDir)) {
        return QString();
    }

    // QSaveFile writes to a temporary file and renames it into place only if
    // every write succeeded, so a full disk can never leave a truncated file
    // that later lookups would trust.
    QSaveFile out(targetFile);
    if (!out.open(QIODevice::WriteOnly)) {
        return QString();
    }
    if (out.write(bytes) != bytes.size()) {
        out.cancelWriting();
        return QString();
    }
    if (!out.commit()) {
        return QFileInfo(targetFile).isFile() ? targetFile : QString();
    }
    return targetFile;
}

} // namespace OmniView
