#include "ColorUtils.h"
#include "ZipUtils.h"

#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QDebug>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QtAlgorithms>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <cmath>

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
    if (v < 0.18) {
        return QStringLiteral("black");
    }
    if (s < 0.16) {
        if (v > 0.80) {
            return QStringLiteral("white");
        }
        return QStringLiteral("gray");
    }

    if (h < 0.0) h = 0.0;
    const double deg = std::fmod(h * 360.0, 360.0);
    if (deg < 15.0 || deg >= 345.0) {
        return QStringLiteral("red");
    } else if (deg < 45.0) {
        return QStringLiteral("orange");
    } else if (deg < 70.0) {
        return QStringLiteral("yellow");
    } else if (deg < 160.0) {
        return QStringLiteral("green");
    } else if (deg < 200.0) {
        return QStringLiteral("cyan");
    } else if (deg < 260.0) {
        return QStringLiteral("blue");
    } else if (deg < 310.0) {
        return QStringLiteral("purple");
    } else {
        return QStringLiteral("pink");
    }
}

HsvColor ColorUtils::rgbToHsv(int r, int g, int b) {
    // Computed in double straight from 8-bit values: QColor's *F() accessors
    // return float on Qt 6 and quantise saturation to 1/65535, which moves
    // colours sitting exactly on a classification boundary between buckets.
    const double rf = qBound(0, r, 255) / 255.0;
    const double gf = qBound(0, g, 255) / 255.0;
    const double bf = qBound(0, b, 255) / 255.0;
    const double maxC = std::max({rf, gf, bf});
    const double minC = std::min({rf, gf, bf});
    const double delta = maxC - minC;

    HsvColor hsv;
    hsv.v = maxC;
    hsv.s = (maxC > 0.0) ? delta / maxC : 0.0;
    if (delta <= 0.0) {
        hsv.h = 0.0;
        return hsv;
    }
    double deg = 0.0;
    if (maxC == rf) {
        deg = 60.0 * std::fmod((gf - bf) / delta, 6.0);
    } else if (maxC == gf) {
        deg = 60.0 * ((bf - rf) / delta + 2.0);
    } else {
        deg = 60.0 * ((rf - gf) / delta + 4.0);
    }
    if (deg < 0.0) deg += 360.0;
    hsv.h = deg / 360.0;
    return hsv;
}

QString ColorUtils::computeDHash(const QImage& img) {
    if (img.isNull()) {
        return QString();
    }

    // Scale in a fixed 32-bit format and compute luma ourselves. Qt 5 returns
    // RGB32 from smooth-scaling a Grayscale8 image, so reading the scaled image
    // as one byte per pixel (as this code once did) hashed every image to
    // 2222222222222222. Rec.601 integer luma matches PIL's convert("L").
    const QImage small = img.convertToFormat(QImage::Format_RGB32)
                            .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                            .convertToFormat(QImage::Format_RGB32);
    if (small.isNull() || small.width() != 9 || small.height() != 8) {
        return QString();
    }

    quint64 val = 0;
    for (int y = 0; y < 8; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(small.constScanLine(y));
        int luma[9];
        for (int x = 0; x < 9; ++x) {
            const QRgb px = line[x];
            luma[x] = (qRed(px) * 299 + qGreen(px) * 587 + qBlue(px) * 114) / 1000;
        }
        for (int x = 0; x < 8; ++x) {
            val = (val << 1) | (luma[x + 1] > luma[x] ? 1ULL : 0ULL);
        }
    }

    return QStringLiteral("%1").arg(val, 16, 16, QLatin1Char('0'));
}

int ColorUtils::hammingDistance(const QString& h1, const QString& h2) {
    bool ok1 = false;
    bool ok2 = false;
    const quint64 v1 = h1.toULongLong(&ok1, 16);
    const quint64 v2 = h2.toULongLong(&ok2, 16);
    if (!ok1 || !ok2) {
        return 64;
    }
    return static_cast<int>(qPopulationCount(v1 ^ v2));
}

void ColorUtils::configureImageIO() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QImageReader::setAllocationLimit(IMAGE_ALLOCATION_LIMIT_MB);
#endif
}

qint64 ColorUtils::decodePixelLimit(const QByteArray& format, bool handlerSupportsScaledSize,
                                   const QSize& scaledSize) {
    // libjpeg reduces by up to 1/8 per side while decoding. PNG advertises
    // ScaledSize but decodes in full and scales afterwards; WebP has no
    // ScaledSize support at all (Qt 5.15 / 6.x), so both keep the plain cap.
    const bool decodesReduced = scaledSize.isValid()
        && static_cast<qint64>(scaledSize.width()) * scaledSize.height() <= MAX_IMAGE_PIXELS
        && (format == "jpeg" || format == "jpg")
        && handlerSupportsScaledSize;
    return decodesReduced ? MAX_IMAGE_PIXELS * 64 : MAX_IMAGE_PIXELS;
}

QImage ColorUtils::loadImage(const QString& sourcePath, const QSize& boundingBox,
                             QSize* originalSize, QString* errorString) {
    auto fail = [errorString](const QString& why) {
        if (errorString) *errorString = why;
        return QImage();
    };

    QBuffer buffer;
    QImageReader reader;
    QString zipPath, innerPath;
    if (ZipUtils::isZipPath(sourcePath, &zipPath, &innerPath)) {
        const QByteArray bytes = ZipUtils::readZipEntryBytes(zipPath, innerPath);
        if (bytes.isEmpty()) {
            return fail(QStringLiteral("Could not read archive entry"));
        }
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        reader.setDevice(&buffer);
    } else {
        reader.setFileName(sourcePath);
    }
    reader.setAutoTransform(true);

    const QSize storedSize = reader.size();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Without a size there is no pre-decode bound, and Qt 5 has no allocation
    // limit either (Qt 6's, set in configureImageIO, still applies there).
    if (!storedSize.isValid()) {
        return fail(QStringLiteral("Image size is unknown before decoding (%1)")
                        .arg(QString::fromLatin1(reader.format())));
    }
#endif

    // Decode straight to the bounding box when the image exceeds it.
    QSize scaledSize;
    if (boundingBox.isValid() && storedSize.isValid()
        && (storedSize.width() > boundingBox.width() || storedSize.height() > boundingBox.height())) {
        // The box is used for both orientations, so make it square.
        const int side = qMax(boundingBox.width(), boundingBox.height());
        scaledSize = storedSize.scaled(side, side, Qt::KeepAspectRatio);
    }

    const qint64 pixelLimit = decodePixelLimit(
        reader.format(), reader.supportsOption(QImageIOHandler::ScaledSize), scaledSize);
    if (storedSize.isValid()
        && static_cast<qint64>(storedSize.width()) * storedSize.height() > pixelLimit) {
        return fail(QStringLiteral("Image is too large (%1×%2)")
                        .arg(storedSize.width()).arg(storedSize.height()));
    }

    // QImageReader::size() reports the stored (pre-EXIF-rotation) size.
    QSize orientedSize = storedSize;
    if (storedSize.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90)) {
        orientedSize.transpose();
    }

    if (scaledSize.isValid()) {
        reader.setScaledSize(scaledSize);
    }

    QImage img = reader.read();
    if (img.isNull()) {
        return fail(reader.errorString());
    }
    // QImageReader assigns a device pixel ratio of 2/3 to "name@2x.png" files.
    img.setDevicePixelRatio(1.0);

    // Palette, mono and 16-bit images take format-specific (and Qt-version
    // specific) scaling paths; normalise once so results are stable.
    const QImage::Format target = img.hasAlphaChannel() ? QImage::Format_ARGB32
                                                         : QImage::Format_RGB32;
    if (img.format() != target) {
        img = img.convertToFormat(target);
        if (img.isNull()) {
            return fail(QStringLiteral("Out of memory while converting image"));
        }
    }

    if (originalSize) {
        *originalSize = orientedSize.isValid() && reader.scaledSize().isValid()
            ? orientedSize
            : img.size();
    }
    return img;
}

void ColorUtils::computeFeaturesFromThumbnail(const QImage& thumb, ImageFeatures& feat) {
    // Alpha-weighted mean colour: premultiplied channels summed over the alpha
    // sum, so transparent areas do not drag the average towards black.
    const QImage tiny = thumb.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                             .scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    quint64 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    if (!tiny.isNull()) {
        for (int y = 0; y < tiny.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(tiny.constScanLine(y));
            for (int x = 0; x < tiny.width(); ++x) {
                sumR += static_cast<quint64>(qRed(line[x]));
                sumG += static_cast<quint64>(qGreen(line[x]));
                sumB += static_cast<quint64>(qBlue(line[x]));
                sumA += static_cast<quint64>(qAlpha(line[x]));
            }
        }
    }

    if (sumA == 0) {
        feat.colorR = feat.colorG = feat.colorB = 0;
        feat.colorName = QStringLiteral("gray");
    } else {
        feat.colorR = static_cast<int>((sumR * 255 + sumA / 2) / sumA);
        feat.colorG = static_cast<int>((sumG * 255 + sumA / 2) / sumA);
        feat.colorB = static_cast<int>((sumB * 255 + sumA / 2) / sumA);
        const HsvColor hsv = rgbToHsv(feat.colorR, feat.colorG, feat.colorB);
        feat.colorName = classifyHsv(hsv.h, hsv.s, hsv.v);
    }

    feat.dhash = computeDHash(thumb);
}

bool ColorUtils::writeThumbnailAtomically(const QImage& thumb, const QString& targetPath) {
    if (thumb.isNull()) return false;

    QByteArray encoded;
    {
        QBuffer buf(&encoded);
        buf.open(QIODevice::WriteOnly);
        QImageWriter writer(&buf, "webp");
        writer.setQuality(90);
        if (!writer.write(thumb)) {
            // Thumbnails are written from several pool threads.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                qWarning() << "WebP encoding unavailable (" << writer.errorString()
                           << "); writing JPEG thumbnails. Install the Qt image formats plugin.";
            }
            encoded.clear();
        }
    }
    if (encoded.isEmpty()) {
        // A fresh writer: QImageWriter keeps its first handler, so switching
        // the format of the failed writer would retry WebP.
        QBuffer buf(&encoded);
        buf.open(QIODevice::WriteOnly);
        QImageWriter jpeg(&buf, "jpeg");
        jpeg.setQuality(90);
        if (!jpeg.write(thumb.convertToFormat(QImage::Format_RGB32))) {
            return false;
        }
    }

    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    QSaveFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (out.write(encoded) != encoded.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

ImageFeatures ColorUtils::generateThumbnailAndFeatures(
    const QString& sourcePath,
    const QString& targetThumbPath,
    QSize maxSize
) {
    ImageFeatures feat;
    if (maxSize.isEmpty()) {
        return feat;
    }

    QSize originalSize;
    // Decode at up to twice the thumbnail size, then smooth-scale down.
    const QImage img = loadImage(sourcePath, maxSize * 2, &originalSize);
    if (img.isNull()) {
        feat.decodeFailed = true;
        return feat;
    }

    feat.width = originalSize.width();
    feat.height = originalSize.height();
    feat.aspectType = getAspectType(feat.width, feat.height);

    QImage thumb = img.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (thumb.isNull()) {
        return feat;
    }

    computeFeaturesFromThumbnail(thumb, feat);

    if (!writeThumbnailAtomically(thumb, targetThumbPath)) {
        return feat;
    }

    feat.thumbPath = targetThumbPath;
    feat.thumbnail = thumb;
    feat.valid = true;
    return feat;
}

} // namespace OmniView
