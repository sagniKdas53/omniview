#pragma once

#include <QString>
#include <QColor>
#include <QImage>
#include <QSize>
#include <QVector>
#include <QPair>
#include <QMetaType>

namespace OmniView {

struct ColorPaletteItem {
    QString id;
    QString label;
    QString hexColor;
};

struct ImageFeatures {
    int width = 0;
    int height = 0;
    QString aspectType = QStringLiteral("square");
    QString colorName = QStringLiteral("gray");
    int colorR = 0;
    int colorG = 0;
    int colorB = 0;
    // Empty means "no hash". "0000000000000000" is a legitimate hash (flat images).
    QString dhash;
    QString thumbPath;
    // Decoded thumbnail, handed to the GUI thread so it never re-reads the file.
    QImage thumbnail;
    bool valid = false;
    // The source itself could not be decoded (as opposed to e.g. the
    // thumbnail write failing): only this is remembered across sessions.
    bool decodeFailed = false;
};

struct HsvColor {
    double h = 0.0; // [0, 1)
    double s = 0.0; // [0, 1]
    double v = 0.0; // [0, 1]
};

class ColorUtils {
public:
    // Hard cap on decoded pixels; larger images are rejected before decoding.
    static constexpr qint64 MAX_IMAGE_PIXELS = 64LL * 1024 * 1024;
    // Qt 6 QImageReader allocation limit (MB). Qt 6.0-6.4 default to 128 MB,
    // which rejects ordinary 35+ MP photos.
    static constexpr int IMAGE_ALLOCATION_LIMIT_MB = 1024;

    static const QVector<ColorPaletteItem>& palette();
    static QString getAspectType(int width, int height);
    static QString classifyHsv(double h, double s, double v);
    static HsvColor rgbToHsv(int r, int g, int b);
    static QString computeDHash(const QImage& img);
    static int hammingDistance(const QString& h1, const QString& h2);

    // Process-wide image I/O configuration. Call once from main() before any
    // image is decoded (QImageReader's allocation limit is a global).
    static void configureImageIO();

    // Decodes a plain file or an "archive.zip#entry" path with EXIF orientation
    // applied. If boundingBox is valid and the image is larger, it is decoded
    // at reduced size where the format supports it (JPEG). originalSize
    // receives the full, orientation-corrected size.
    static QImage loadImage(const QString& sourcePath,
                            const QSize& boundingBox = QSize(),
                            QSize* originalSize = nullptr,
                            QString* errorString = nullptr);

    // Largest stored image (in pixels) loadImage accepts from a reader with
    // this format and ScaledSize support when asked for scaledSize (invalid:
    // full-size decode). Only JPEG decodes straight to a reduced size, so only
    // JPEG gets more than MAX_IMAGE_PIXELS, and only if the output fits it.
    static qint64 decodePixelLimit(const QByteArray& format, bool handlerSupportsScaledSize,
                                   const QSize& scaledSize);

    // Fills colour and dHash fields of feat from a decoded thumbnail.
    static void computeFeaturesFromThumbnail(const QImage& thumb, ImageFeatures& feat);

    static ImageFeatures generateThumbnailAndFeatures(
        const QString& sourcePath,
        const QString& targetThumbPath,
        QSize maxSize = QSize(420, 420)
    );

    // Encodes thumb as WebP (falling back to JPEG bytes when no WebP encoder
    // is available, keeping the shared cache file name) and atomically
    // replaces targetPath.
    static bool writeThumbnailAtomically(const QImage& thumb, const QString& targetPath);
};

} // namespace OmniView

Q_DECLARE_METATYPE(OmniView::ImageFeatures)
