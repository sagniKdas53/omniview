#include "ColorUtils.h"
#include "ZipUtils.h"

#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <QtMath>

namespace OmniView {

const QVector<ColorPaletteItem>& ColorUtils::palette() {
    static const QVector<ColorPaletteItem> s_palette = {
        {QStringLiteral("all"),    QStringLiteral("All Colors"),      QStringLiteral("#888888")},
        {QStringLiteral("red"),    QStringLiteral("Red"),             QStringLiteral("#ef4444")},
        {QStringLiteral("orange"), QStringLiteral("Orange"),          QStringLiteral("#f97316")},
        {QStringLiteral("yellow"), QStringLiteral("Yellow"),          QStringLiteral("#eab308")},
        {QStringLiteral("green"),  QStringLiteral("Green"),           QStringLiteral("#22c55e")},
        {QStringLiteral("cyan"),   QStringLiteral("Cyan"),            QStringLiteral("#06b6d4")},
        {QStringLiteral("blue"),   QStringLiteral("Blue"),            QStringLiteral("#3b82f6")},
        {QStringLiteral("purple"), QStringLiteral("Purple"),          QStringLiteral("#a855f7")},
        {QStringLiteral("pink"),   QStringLiteral("Pink"),            QStringLiteral("#ec4899")},
        {QStringLiteral("white"),  QStringLiteral("Light / White"),   QStringLiteral("#f1f5f9")},
        {QStringLiteral("gray"),   QStringLiteral("Neutral / Gray"),  QStringLiteral("#64748b")},
        {QStringLiteral("black"),  QStringLiteral("Dark / Black"),    QStringLiteral("#1e293b")}
    };
    return s_palette;
}

QString ColorUtils::getAspectType(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QStringLiteral("square");
    }
    const double ratio = static_cast<double>(width) / static_cast<double>(height);
    if (ratio < 0.85) {
        return QStringLiteral("portrait");
    } else if (ratio > 1.18) {
        return QStringLiteral("landscape");
    }
    return QStringLiteral("square");
}

QString ColorUtils::classifyHsv(double h, double s, double v) {
    if (v < 0.18f) {
        return QStringLiteral("black");
    }
    if (s < 0.16f) {
        if (v > 0.80f) {
            return QStringLiteral("white");
        }
        return QStringLiteral("gray");
    }

    if (h < 0.0f) h = 0.0f;
    const double deg = std::fmod(h * 360.0, 360.0);
    if (deg < 15.0f || deg >= 345.0f) {
        return QStringLiteral("red");
    } else if (deg < 45.0f) {
        return QStringLiteral("orange");
    } else if (deg < 70.0f) {
        return QStringLiteral("yellow");
    } else if (deg < 160.0f) {
        return QStringLiteral("green");
    } else if (deg < 200.0f) {
        return QStringLiteral("cyan");
    } else if (deg < 260.0f) {
        return QStringLiteral("blue");
    } else if (deg < 310.0f) {
        return QStringLiteral("purple");
    } else {
        return QStringLiteral("pink");
    }
}

QString ColorUtils::computeDHash(const QImage& img) {
    if (img.isNull()) {
        return QStringLiteral("0000000000000000");
    }

    const QImage gray = img.convertToFormat(QImage::Format_Grayscale8)
                           .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    quint64 val = 0;
    for (int y = 0; y < 8; ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < 8; ++x) {
            const int left = line[x];
            const int right = line[x + 1];
            val = (val << 1) | (right > left ? 1ULL : 0ULL);
        }
    }

    return QStringLiteral("%1").arg(val, 16, 16, QChar('0'));
}

int ColorUtils::hammingDistance(const QString& h1, const QString& h2) {
    bool ok1 = false;
    bool ok2 = false;
    const quint64 v1 = h1.toULongLong(&ok1, 16);
    const quint64 v2 = h2.toULongLong(&ok2, 16);
    if (!ok1 || !ok2) {
        return 64;
    }
    quint64 diff = v1 ^ v2;
    int count = 0;
    while (diff > 0) {
        count += (diff & 1ULL);
        diff >>= 1;
    }
    return count;
}

ImageFeatures ColorUtils::generateThumbnailAndFeatures(
    const QString& sourcePath,
    const QString& targetThumbPath,
    QSize maxSize
) {
    ImageFeatures feat;
    QImage img;

    QString zipPath, innerPath;
    if (ZipUtils::isZipPath(sourcePath, &zipPath, &innerPath)) {
        const QByteArray bytes = ZipUtils::readZipEntryBytes(zipPath, innerPath);
        if (bytes.isEmpty()) {
            return feat;
        }
        QBuffer buf;
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        const QSize origSize = reader.size();
        if (origSize.isValid()) {
            if (static_cast<qint64>(origSize.width()) * origSize.height() > 64 * 1024 * 1024) {
                return feat;
            }
            feat.width = origSize.width();
            feat.height = origSize.height();
        }
        img = reader.read();
    } else {
        QImageReader reader(sourcePath);
        reader.setAutoTransform(true);
        const QSize origSize = reader.size();
        if (origSize.isValid()) {
            if (static_cast<qint64>(origSize.width()) * origSize.height() > 64 * 1024 * 1024) {
                return feat;
            }
            feat.width = origSize.width();
            feat.height = origSize.height();
        }
        img = reader.read();
    }

    if (img.isNull()) {
        return feat;
    }

    if (feat.width <= 0 || feat.height <= 0) {
        feat.width = img.width();
        feat.height = img.height();
    }

    feat.aspectType = getAspectType(feat.width, feat.height);

    // Generate crisp thumbnail first
    const QImage thumb = img.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // Dominant color via 16x16 downsample from thumbnail
    const QImage tiny = thumb.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB888);
    quint64 sumR = 0, sumG = 0, sumB = 0;
    const int totalPixels = 16 * 16;
    for (int y = 0; y < 16; ++y) {
        const uchar* scan = tiny.constScanLine(y);
        for (int x = 0; x < 16; ++x) {
            sumR += scan[x * 3 + 0];
            sumG += scan[x * 3 + 1];
            sumB += scan[x * 3 + 2];
        }
    }
    feat.colorR = static_cast<int>(sumR / totalPixels);
    feat.colorG = static_cast<int>(sumG / totalPixels);
    feat.colorB = static_cast<int>(sumB / totalPixels);

    QColor avgColor(feat.colorR, feat.colorG, feat.colorB);
    feat.colorName = classifyHsv(avgColor.hsvHueF(), avgColor.hsvSaturationF(), avgColor.valueF());

    // dHash from thumbnail (perceptually equivalent, 20x faster)
    feat.dhash = computeDHash(thumb);

    // Ensure parent directory exists
    QFileInfo fi(targetThumbPath);
    QDir().mkpath(fi.absolutePath());

    const QString format = targetThumbPath.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive)
                           ? QStringLiteral("WEBP")
                           : QStringLiteral("JPEG");

    // Atomic thumbnail write via UUID-v7 temp file
    const QString tmpThumb = targetThumbPath + QStringLiteral(".tmp.") + ZipUtils::generateUuidV7();
    bool written = false;
    {
        QImageWriter writer(tmpThumb, format.toLatin1());
        writer.setQuality(90);
        written = writer.write(thumb);
        if (!written && format == QStringLiteral("WEBP")) {
            writer.setFormat("JPEG");
            written = writer.write(thumb);
        }
    } // Close the writer's file handle before renaming on Windows.

    if (!written) {
        QFile::remove(tmpThumb);
        return feat;
    }
    if (!QFile::rename(tmpThumb, targetThumbPath)) {
        if (!QFile::remove(targetThumbPath) || !QFile::rename(tmpThumb, targetThumbPath)) {
            QFile::remove(tmpThumb);
            return feat;
        }
    }

    feat.thumbPath = targetThumbPath;
    feat.valid = true;
    return feat;
}

} // namespace OmniView
