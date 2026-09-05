#include <QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>
#include <QColor>

#include "Config.h"
#include "ColorUtils.h"
#include "Database.h"
#include "GalleryModel.h"

using namespace OmniView;

class TestOmniView : public QObject {
    Q_OBJECT
private slots:
    void testConfig() {
        QCOMPARE(Config::appName(), QStringLiteral("OmniView"));
        QCOMPARE(Config::appId(), QStringLiteral("omniview"));
        QVERIFY(Config::isSupportedExtension(QStringLiteral(".png")));
        QVERIFY(Config::isSupportedExtension(QStringLiteral(".jpg")));
        QVERIFY(Config::isSupportedExtension(QStringLiteral(".jpeg")));
        QVERIFY(Config::isSupportedExtension(QStringLiteral(".webp")));
        QVERIFY(!Config::isSupportedExtension(QStringLiteral(".txt")));
    }

    void testColorUtilsAspectAndHsv() {
        QCOMPARE(ColorUtils::getAspectType(1920, 1080), QStringLiteral("landscape"));
        QCOMPARE(ColorUtils::getAspectType(1080, 1920), QStringLiteral("portrait"));
        QCOMPARE(ColorUtils::getAspectType(1000, 1000), QStringLiteral("square"));

        QCOMPARE(ColorUtils::classifyHsv(0.0f, 1.0f, 1.0f), QStringLiteral("red"));
        QCOMPARE(ColorUtils::classifyHsv(0.33f, 1.0f, 1.0f), QStringLiteral("green"));
        QCOMPARE(ColorUtils::classifyHsv(0.66f, 1.0f, 1.0f), QStringLiteral("blue"));
        QCOMPARE(ColorUtils::classifyHsv(0.0f, 0.0f, 0.0f), QStringLiteral("black"));
        QCOMPARE(ColorUtils::classifyHsv(0.0f, 0.0f, 1.0f), QStringLiteral("white"));
    }

    void testThumbnailAndDHash() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString srcPath = tmp.filePath(QStringLiteral("sample.png"));
        const QString thumbPath = tmp.filePath(QStringLiteral("thumb.webp"));

        QImage testImg(400, 300, QImage::Format_RGB888);
        testImg.fill(QColor(0, 120, 255)); // Blueish
        QVERIFY(testImg.save(srcPath, "PNG"));

        ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(srcPath, thumbPath, QSize(200, 200));
        QVERIFY(feat.valid);
        QCOMPARE(feat.width, 400);
        QCOMPARE(feat.height, 300);
        QCOMPARE(feat.aspectType, QStringLiteral("landscape"));
        QCOMPARE(feat.colorName, QStringLiteral("blue"));
        QVERIFY(QFile::exists(thumbPath));
        QVERIFY(QFileInfo(thumbPath).size() > 0);
        QCOMPARE(feat.dhash.length(), 16);
    }

    void testDatabaseOperations() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dbPath = tmp.filePath(QStringLiteral("test.db"));

        Database db(dbPath);
        QVERIFY(db.initSchema());

        QVector<ImageRecord> entries = {
            {QStringLiteral("/media/img1.jpg"), QStringLiteral("img1.jpg"), QStringLiteral("folder_a"), 1024 * 100, 1700000000, 0, 0, QStringLiteral("square"), QString(), 0, 0, 0, QString(), QString(), false, false},
            {QStringLiteral("/media/img2.png"), QStringLiteral("img2.png"), QStringLiteral("folder_b"), 1024 * 200, 1700001000, 0, 0, QStringLiteral("square"), QString(), 0, 0, 0, QString(), QString(), false, false},
            {QStringLiteral("/media/img3.webp"), QStringLiteral("img3.webp"), QStringLiteral("folder_a"), 1024 * 300, 1700002000, 0, 0, QStringLiteral("square"), QString(), 0, 0, 0, QString(), QString(), false, false}
        };

        QVERIFY(db.batchSyncFiles(entries));

        QVERIFY(db.updateImageFeatures(
            QStringLiteral("/media/img1.jpg"), 1920, 1080, QStringLiteral("landscape"),
            QStringLiteral("blue"), 0, 100, 255, QStringLiteral("0000000000000000"),
            QStringLiteral("/thumb/1.webp")
        ));

        QueryFilter filter;
        filter.aspectType = QStringLiteral("landscape");
        const auto landscapes = db.queryImages(filter);
        QCOMPARE(landscapes.size(), 1);
        QCOMPARE(landscapes.first().filename, QStringLiteral("img1.jpg"));

        filter = QueryFilter();
        filter.searchTerm = QStringLiteral("img3");
        const auto searched = db.queryImages(filter);
        QCOMPARE(searched.size(), 1);
        QCOMPARE(searched.first().filename, QStringLiteral("img3.webp"));

        bool newFav = false;
        QVERIFY(db.toggleFavorite(QStringLiteral("/media/img1.jpg"), &newFav));
        QVERIFY(newFav);

        filter = QueryFilter();
        filter.favoriteOnly = true;
        const auto favs = db.queryImages(filter);
        QCOMPARE(favs.size(), 1);
        QCOMPARE(favs.first().path, QStringLiteral("/media/img1.jpg"));

        const auto subfolders = db.getSubfoldersWithCounts(QStringLiteral("/media"));
        QMap<QString, int> counts;
        for (const auto& p : subfolders) {
            counts.insert(p.first, p.second);
        }
        QCOMPARE(counts.value(QStringLiteral("folder_a")), 2);
        QCOMPARE(counts.value(QStringLiteral("folder_b")), 1);

        const Stats stats = db.getStats();
        QCOMPARE(stats.totalImages, 3);
        QCOMPARE(stats.favoritesCount, 1);
        QVERIFY(stats.totalBytes > 0);
    }

    void testGalleryModel() {
        GalleryModel model;
        QVector<ImageRecord> items = {
            {QStringLiteral("/p1.jpg"), QStringLiteral("p1.jpg"), QString(), 1000, 100.0, 800, 600, QStringLiteral("landscape"), QStringLiteral("red"), 255, 0, 0, QString(), QString(), false, true},
            {QStringLiteral("/p2.jpg"), QStringLiteral("p2.jpg"), QString(), 2000, 200.0, 600, 800, QStringLiteral("portrait"), QStringLiteral("blue"), 0, 0, 255, QString(), QString(), true, true}
        };
        model.setItems(items);

        QCOMPARE(model.rowCount(), 2);
        const auto* it = model.getItem(1);
        QVERIFY(it != nullptr);
        QCOMPARE(it->filename, QStringLiteral("p2.jpg"));
        QVERIFY(it->isFavorite);

        QCOMPARE(model.rowForPath(QStringLiteral("/p2.jpg")), 1);
        QCOMPARE(model.rowForPath(QStringLiteral("/nonexistent")), -1);
    }
};

QTEST_MAIN(TestOmniView)
#include "test_omniview.moc"
