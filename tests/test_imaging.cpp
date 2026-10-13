#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QLinearGradient>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include "ColorUtils.h"
#include "miniz.h"
#include "TestSupport.h"

using namespace OmniView;

namespace {

QImage horizontalGradient(int w, int h, bool leftToRight) {
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, w, 0);
    g.setColorAt(0, leftToRight ? Qt::black : Qt::white);
    g.setColorAt(1, leftToRight ? Qt::white : Qt::black);
    p.fillRect(img.rect(), g);
    return img;
}

void appendBigEndian(QByteArray& out, quint32 v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.append(static_cast<char>((v >> shift) & 0xFF));
}

void appendPngChunk(QByteArray& out, const char* type, const QByteArray& data) {
    appendBigEndian(out, static_cast<quint32>(data.size()));
    const QByteArray body = QByteArray(type, 4) + data;
    out.append(body);
    appendBigEndian(out, static_cast<quint32>(mz_crc32(MZ_CRC32_INIT,
        reinterpret_cast<const unsigned char*>(body.constData()), static_cast<size_t>(body.size()))));
}

// A PNG whose header claims w x h; enough for QImageReader::size().
QByteArray pngHeaderOnly(quint32 w, quint32 h) {
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    QByteArray ihdr;
    appendBigEndian(ihdr, w);
    appendBigEndian(ihdr, h);
    ihdr.append(QByteArray("\x08\x00\x00\x00\x00", 5)); // 8-bit greyscale
    appendPngChunk(png, "IHDR", ihdr);
    appendPngChunk(png, "IDAT", QByteArray("\x78\x9c", 2));
    appendPngChunk(png, "IEND", QByteArray());
    return png;
}

// A lossless WebP (VP8L) whose header claims w x h (each <= 16384); enough
// for QImageReader::size() without decoding any pixels.
QByteArray webpHeaderOnly(quint32 w, quint32 h) {
    const quint32 bits = (w - 1) | ((h - 1) << 14); // no alpha, version 0
    QByteArray vp8l(1, static_cast<char>(0x2f)); // VP8L signature
    for (int shift = 0; shift < 32; shift += 8) vp8l.append(static_cast<char>((bits >> shift) & 0xFF));
    vp8l.append(QByteArray(11, '\0')); // a little payload; chunk size stays even
    QByteArray chunk("VP8L");
    for (int shift = 0; shift < 32; shift += 8) {
        chunk.append(static_cast<char>((static_cast<quint32>(vp8l.size()) >> shift) & 0xFF));
    }
    const QByteArray body = QByteArray("WEBP") + chunk + vp8l;
    QByteArray riff("RIFF");
    for (int shift = 0; shift < 32; shift += 8) {
        riff.append(static_cast<char>((static_cast<quint32>(body.size()) >> shift) & 0xFF));
    }
    return riff + body;
}

QImage noise(int w, int h, int seed) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            img.setPixel(x, y, qRgb((x * 37 + y * 91 + seed) % 256, (x * 13 + seed * 7) % 256, (y * 7 + seed) % 256));
        }
    }
    return img;
}

// Writes img as JPEG with an EXIF APP1 segment carrying the given orientation.
bool writeJpegWithOrientation(const QImage& img, const QString& path, quint16 orientation) {
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "JPEG", 95)) return false;

    QByteArray tiff;
    tiff.append("II*\0", 4);                                  // little endian TIFF header
    tiff.append("\x08\x00\x00\x00", 4);                       // IFD0 offset
    tiff.append("\x01\x00", 2);                               // one entry
    tiff.append("\x12\x01\x03\x00\x01\x00\x00\x00", 8);       // 0x0112 SHORT x1
    tiff.append(static_cast<char>(orientation & 0xff));
    tiff.append(static_cast<char>(orientation >> 8));
    tiff.append("\x00\x00", 2);
    tiff.append("\x00\x00\x00\x00", 4);                       // next IFD
    QByteArray app1("Exif\0\0", 6);
    app1.append(tiff);
    const int len = static_cast<int>(app1.size()) + 2;
    QByteArray segment("\xff\xe1", 2);
    segment.append(static_cast<char>((len >> 8) & 0xff));
    segment.append(static_cast<char>(len & 0xff));
    segment.append(app1);
    jpeg.insert(2, segment); // right after SOI

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(jpeg) == jpeg.size();
}

} // namespace

class ImagingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { ColorUtils::configureImageIO(); }

    void dhashDistinguishesImages() {
        const QString ltr = ColorUtils::computeDHash(horizontalGradient(200, 100, true));
        const QString rtl = ColorUtils::computeDHash(horizontalGradient(200, 100, false));
        const QString n1 = ColorUtils::computeDHash(noise(200, 100, 1));
        QCOMPARE(ltr, QStringLiteral("ffffffffffffffff"));
        QCOMPARE(rtl, QStringLiteral("0000000000000000"));
        QCOMPARE(n1.length(), 16);
        QVERIFY(n1 != ltr && n1 != rtl);
        QVERIFY(n1 != QStringLiteral("2222222222222222"));
    }

    void dhashIndependentOfSourceFormat() {
        const QImage rgb = horizontalGradient(180, 90, true);
        const QString expected = ColorUtils::computeDHash(rgb);
        QCOMPARE(ColorUtils::computeDHash(rgb.convertToFormat(QImage::Format_Grayscale8)), expected);
        QCOMPARE(ColorUtils::computeDHash(rgb.convertToFormat(QImage::Format_ARGB32)), expected);
        QCOMPARE(ColorUtils::computeDHash(rgb.convertToFormat(QImage::Format_Indexed8)), expected);
    }

    void dhashOfNullImageIsEmpty() {
        QVERIFY(ColorUtils::computeDHash(QImage()).isEmpty());
        QCOMPARE(ColorUtils::hammingDistance(QString(), QStringLiteral("ffff")), 64);
    }

    void hammingDistanceCountsBits() {
        QCOMPARE(ColorUtils::hammingDistance(QStringLiteral("0000000000000000"), QStringLiteral("ffffffffffffffff")), 64);
        QCOMPARE(ColorUtils::hammingDistance(QStringLiteral("00000000000000f0"), QStringLiteral("0000000000000010")), 3);
        QCOMPARE(ColorUtils::hammingDistance(QStringLiteral("8000000000000001"), QStringLiteral("8000000000000001")), 0);
    }

    void hsvBoundariesUseExactMaths() {
        // Exactly 345 degrees -> red; exactly 260 degrees -> purple.
        HsvColor c = ColorUtils::rgbToHsv(46, 2, 13);
        QCOMPARE(ColorUtils::classifyHsv(c.h, c.s, c.v), QStringLiteral("red"));
        c = ColorUtils::rgbToHsv(16, 0, 48);
        QCOMPARE(ColorUtils::classifyHsv(c.h, c.s, c.v), QStringLiteral("purple"));
        c = ColorUtils::rgbToHsv(255, 255, 255);
        QCOMPARE(ColorUtils::classifyHsv(c.h, c.s, c.v), QStringLiteral("white"));
        c = ColorUtils::rgbToHsv(0, 0, 0);
        QCOMPARE(ColorUtils::classifyHsv(c.h, c.s, c.v), QStringLiteral("black"));
        c = ColorUtils::rgbToHsv(0, 0, 255);
        QCOMPARE(ColorUtils::classifyHsv(c.h, c.s, c.v), QStringLiteral("blue"));
    }

    void exifRotatedDimensionsAreOriented() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("rot6.jpg"));
        QImage stored(400, 300, QImage::Format_RGB32);
        stored.fill(Qt::darkGreen);
        QVERIFY(writeJpegWithOrientation(stored, src, 6));
        QImageReader probe(src);
        probe.setAutoTransform(true);
        QCOMPARE(probe.read().size(), QSize(300, 400)); // fixture sanity

        const ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(
            src, tmp.filePath(QStringLiteral("t.webp")), QSize(360, 360));
        QVERIFY(feat.valid);
        QCOMPARE(QSize(feat.width, feat.height), QSize(300, 400));
        QCOMPARE(feat.aspectType, QStringLiteral("portrait"));
        QCOMPARE(feat.thumbnail.size(), QSize(270, 360));
    }

    void reducedDecodeKeepsOrientationAndAspect() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("big_rot6.jpg"));
        QImage stored(2400, 1200, QImage::Format_RGB32);
        stored.fill(Qt::blue);
        QVERIFY(writeJpegWithOrientation(stored, src, 6));

        QSize original;
        const QImage decoded = ColorUtils::loadImage(src, QSize(720, 720), &original);
        QVERIFY(!decoded.isNull());
        QCOMPARE(original, QSize(1200, 2400));
        QVERIFY(decoded.height() > decoded.width());
        QVERIFY(decoded.height() <= 1200); // decoded at reduced size

        const ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(
            src, tmp.filePath(QStringLiteral("t.webp")), QSize(360, 360));
        QVERIFY(feat.valid);
        QCOMPARE(QSize(feat.width, feat.height), QSize(1200, 2400));
        QCOMPARE(feat.thumbnail.size(), QSize(180, 360));
        QCOMPARE(feat.colorName, QStringLiteral("blue"));
    }

    void transparencyDoesNotDarkenColour() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("logo.png"));
        QImage img(400, 400, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.fillRect(QRect(160, 160, 155, 155), QColor(230, 20, 20)); // ~15% coverage
        p.end();
        QVERIFY(img.save(src));
        ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(src, tmp.filePath(QStringLiteral("t.webp")), QSize(200, 200));
        QVERIFY(feat.valid);
        QCOMPARE(feat.colorName, QStringLiteral("red"));

        QImage clear(50, 50, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        const QString clearPath = tmp.filePath(QStringLiteral("clear.png"));
        QVERIFY(clear.save(clearPath));
        feat = ColorUtils::generateThumbnailAndFeatures(clearPath, tmp.filePath(QStringLiteral("c.webp")), QSize(200, 200));
        QVERIFY(feat.valid);
        QCOMPARE(feat.colorName, QStringLiteral("gray"));
    }

    void tinyAndGreyscaleImages() {
        QTemporaryDir tmp;
        QImage one(1, 1, QImage::Format_RGB32);
        one.fill(QColor(250, 250, 250));
        const QString onePath = tmp.filePath(QStringLiteral("one.png"));
        QVERIFY(one.save(onePath));
        ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(onePath, tmp.filePath(QStringLiteral("1.webp")), QSize(360, 360));
        QVERIFY(feat.valid);
        QCOMPARE(QSize(feat.width, feat.height), QSize(1, 1));
        QCOMPARE(feat.colorName, QStringLiteral("white"));
        QCOMPARE(feat.dhash, QStringLiteral("0000000000000000")); // flat image: a real hash, not "missing"

        const QImage grey = horizontalGradient(64, 64, true).convertToFormat(QImage::Format_Grayscale8);
        const QString greyPath = tmp.filePath(QStringLiteral("grey.png"));
        QVERIFY(grey.save(greyPath));
        feat = ColorUtils::generateThumbnailAndFeatures(greyPath, tmp.filePath(QStringLiteral("g.webp")), QSize(360, 360));
        QVERIFY(feat.valid);
        QCOMPARE(feat.dhash, QStringLiteral("ffffffffffffffff"));
    }

    void paletteImagesAreNotRequantised() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("palette.png"));
        QVERIFY(horizontalGradient(800, 600, true).convertToFormat(QImage::Format_Indexed8).save(src));
        const ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(src, tmp.filePath(QStringLiteral("p.webp")), QSize(360, 360));
        QVERIFY(feat.valid);
        QVERIFY(feat.thumbnail.format() != QImage::Format_Indexed8);
        QVERIFY(feat.thumbnail.format() != QImage::Format_Mono);
    }

    void emptyMaxSizeFailsCleanly() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("a.png"));
        QVERIFY(noise(20, 20, 3).save(src));
        const ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(src, tmp.filePath(QStringLiteral("a.webp")), QSize(0, 0));
        QVERIFY(!feat.valid);
    }

    void thumbnailReplacesExistingFileAtomically() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("a.png"));
        const QString target = tmp.filePath(QStringLiteral("thumb.webp"));
        QFile stale(target);
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("stale-bytes");
        stale.close();
        QVERIFY(noise(100, 50, 9).save(src));
        const ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(src, target, QSize(64, 64));
        QVERIFY(feat.valid);
        QImage written(target);
        QCOMPARE(written.size(), QSize(64, 32));
        QVERIFY(QDir(tmp.path()).entryList({QStringLiteral("*.tmp*")}).isEmpty());
    }

    void hiDpiFileNameDoesNotChangePixelRatio() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("icon@2x.png"));
        QVERIFY(noise(40, 30, 2).save(src));
        const QImage img = ColorUtils::loadImage(src);
        QCOMPARE(img.devicePixelRatio(), 1.0);
        QCOMPARE(img.size(), QSize(40, 30));
    }

    void hugeImageDecodedAtReducedSize() {
        // Over MAX_IMAGE_PIXELS (e.g. a 108 MP phone photo), but JPEG decodes
        // straight to a fraction of its size, so a thumbnail is affordable.
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("huge.jpg"));
        {
            QImage img(8200, 8200, QImage::Format_Grayscale8);
            QVERIFY(!img.isNull());
            img.fill(128);
            QVERIFY(img.save(src, "JPEG", 50));
        }
        QSize original;
        QString error;
        const QImage thumb = ColorUtils::loadImage(src, QSize(256, 256), &original, &error);
        QVERIFY2(!thumb.isNull(), qPrintable(error));
        QCOMPARE(original, QSize(8200, 8200));
        QVERIFY(thumb.width() <= 256 && thumb.height() <= 256);
    }

    void hugeImageWithUnreducedBoxIsRejected() {
        // A box at least as large as the image means a full-size decode, so
        // the relaxed limit for reduced decodes must not apply.
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("huge.jpg"));
        {
            QImage img(8200, 8200, QImage::Format_Grayscale8);
            QVERIFY(!img.isNull());
            img.fill(128);
            QVERIFY(img.save(src, "JPEG", 50));
        }
        QString error;
        QVERIFY(ColorUtils::loadImage(src, QSize(9000, 9000), nullptr, &error).isNull());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
    }

    void hugeImageWithoutReducedDecodeIsRejected() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("huge.png"));
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(pngHeaderOnly(9000, 9000));
        f.close();
        QCOMPARE(QImageReader(src).size(), QSize(9000, 9000));
        QString error;
        QVERIFY(ColorUtils::loadImage(src, QSize(256, 256), nullptr, &error).isNull());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
        error.clear();
        QVERIFY(ColorUtils::loadImage(src, QSize(), nullptr, &error).isNull());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
    }

    void hugeWebpIsRejectedEvenWithBox() {
        // Qt's WebP handler has no ScaledSize support: it decodes in full.
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("huge.webp"));
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(webpHeaderOnly(9000, 9000));
        f.close();
        QVERIFY2(QImageReader::supportedImageFormats().contains("webp"),
                 "Prerequisite: the Qt WebP image plugin (qt5/qt6-image-formats-plugins, "
                 "or the qtimageformats module) must be installed");
        QImageReader probe(src);
        QVERIFY2(probe.size() == QSize(9000, 9000),
                 qPrintable(QStringLiteral("WebP header not understood: size %1x%2, %3")
                                .arg(probe.size().width()).arg(probe.size().height())
                                .arg(probe.errorString())));
        QString error;
        QVERIFY(ColorUtils::loadImage(src, QSize(256, 256), nullptr, &error).isNull());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
    }

    void undecodableSourceReportsError() {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("broken.png"));
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\x89PNG\r\n\x1a\n-truncated");
        f.close();
        QString error;
        QVERIFY(ColorUtils::loadImage(src, QSize(), nullptr, &error).isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(!ColorUtils::generateThumbnailAndFeatures(src, tmp.filePath(QStringLiteral("b.webp"))).valid);
    }
};

OMNIVIEW_TEST_MAIN(ImagingTest)
#include "test_imaging.moc"
