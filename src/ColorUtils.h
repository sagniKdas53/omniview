#pragma once

#include <QString>
#include <QColor>
#include <QImage>
#include <QSize>
#include <QVector>
#include <QPair>

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
    QString dhash = QStringLiteral("0000000000000000");
    QString thumbPath;
    bool valid = false;
};

class ColorUtils {
public:
    static const QVector<ColorPaletteItem>& palette();
    static QString getAspectType(int width, int height);
    static QString classifyHsv(double h, double s, double v);
    static QString computeDHash(const QImage& img);
    static int hammingDistance(const QString& h1, const QString& h2);
    static ImageFeatures generateThumbnailAndFeatures(
        const QString& sourcePath,
        const QString& targetThumbPath,
        QSize maxSize = QSize(360, 360)
    );
};

} // namespace OmniView
