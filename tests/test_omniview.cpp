#include <QTest>
#include <QSignalSpy>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>
#include <QColor>

#include "Config.h"
#include "ColorUtils.h"
#include "ZipUtils.h"
#include "Scanner.h"
#include "Database.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "ViewerWindow.h"

using namespace OmniView;

class TestOmniView : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<OmniView::Stats>("OmniView::Stats");
        qRegisterMetaType<OmniView::Stats>("Stats");
        qRegisterMetaType<OmniView::ImageFeatures>("OmniView::ImageFeatures");
        qRegisterMetaType<OmniView::ImageFeatures>("ImageFeatures");
        qRegisterMetaType<OmniView::ImageRecord>("OmniView::ImageRecord");
        qRegisterMetaType<OmniView::ImageRecord>("ImageRecord");
    }

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

    void testDragButton() {
        DragButton btn(QStringLiteral("Test"));
        btn.setFilePath(QStringLiteral("/tmp/a.jpg"));
        QCOMPARE(btn.filePath(), QStringLiteral("/tmp/a.jpg"));
        QCOMPARE(btn.filePaths(), QStringList{QStringLiteral("/tmp/a.jpg")});

        const QStringList multi = {QStringLiteral("/tmp/1.jpg"), QStringLiteral("/tmp/2.png")};
        btn.setFilePaths(multi);
        QCOMPARE(btn.filePaths(), multi);
        QCOMPARE(btn.filePath(), QStringLiteral("/tmp/1.jpg"));
    }

    void testGalleryViewSelectionAndInvert() {
        GalleryView view(nullptr);
        GalleryModel model;
        QVector<ImageRecord> items;
        for (int i = 0; i < 5; ++i) {
            ImageRecord rec;
            rec.path = QStringLiteral("/media/img%1.jpg").arg(i);
            rec.filename = QStringLiteral("img%1.jpg").arg(i);
            items.append(rec);
        }
        model.setItems(items);
        view.setModel(&model);
        view.setSelectMode(true);

        auto* sel = view.selectionModel();
        QVERIFY(sel != nullptr);

        // Select items 0, 1, 2
        for (int r : {0, 1, 2}) {
            sel->select(model.index(r, 0), QItemSelectionModel::Select);
        }
        QCOMPARE(view.getSelectedRows(), (QVector<int>{0, 1, 2}));
        QCOMPARE(view.getSelectedPaths().size(), 3);

        // Invert selection -> items 3 and 4 should be selected
        view.invertSelection();
        QCOMPARE(view.getSelectedRows(), (QVector<int>{3, 4}));

        // Select all
        view.selectAllItems();
        QCOMPARE(view.getSelectedRows().size(), 5);

        // Clear selection
        view.clearAllSelection();
        QCOMPARE(view.getSelectedRows().size(), 0);
    }

    void testZipUtilsAndScannerDeduplication() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        const QString root = tmpDir.path();
        const QString dbPath = tmpDir.filePath(QStringLiteral("test_zip.db"));

        // 1. Create a dummy image file
        QImage sample(100, 100, QImage::Format_RGB888);
        sample.fill(QColor(255, 0, 128));
        const QString rawImgPath = tmpDir.filePath(QStringLiteral("raw_only.png"));
        QVERIFY(sample.save(rawImgPath, "PNG"));

        // 2. Create archive folder and extracted directory "bundle1"
        const QString bundle1Dir = tmpDir.filePath(QStringLiteral("bundle1"));
        QDir().mkpath(bundle1Dir);
        const QString extracted0 = bundle1Dir + QStringLiteral("/0.png");
        QVERIFY(sample.save(extracted0, "PNG"));

        // Create zip archive "bundle1.zip" containing 0.png (duplicate) and 1.png (unique)
        const QString zipPath = tmpDir.filePath(QStringLiteral("bundle1.zip"));
        QTemporaryDir srcTmp;
        QVERIFY(srcTmp.isValid());
        const QString tempZipSrc = srcTmp.path();
        QVERIFY(sample.save(tempZipSrc + QStringLiteral("/0.png"), "PNG"));
        QVERIFY(sample.save(tempZipSrc + QStringLiteral("/1.png"), "PNG"));

        // Run zip command to create bundle1.zip
        QProcess proc;
        proc.setWorkingDirectory(tempZipSrc);
        proc.start(QStringLiteral("zip"), QStringList{QStringLiteral("-q"), zipPath, QStringLiteral("0.png"), QStringLiteral("1.png")});
        QVERIFY(proc.waitForFinished(5000));
        QVERIFY(QFile::exists(zipPath));

        // Test ZipUtils::listZipImages
        const QVector<ZipImageEntry> entries = ZipUtils::listZipImages(zipPath);
        QCOMPARE(entries.size(), 2);

        // Test duplicate detection: 0.png has raw extracted counterpart at bundle1/0.png -> true
        // 1.png has no extracted counterpart -> false
        const QFileInfo zipFi(zipPath);
        QVERIFY(ZipUtils::isDuplicateOfRaw(zipFi.absolutePath(), zipFi.completeBaseName(), QStringLiteral("0.png")));
        QVERIFY(!ZipUtils::isDuplicateOfRaw(zipFi.absolutePath(), zipFi.completeBaseName(), QStringLiteral("1.png")));

        // Test reading entry bytes directly from zip
        const QByteArray bytes0 = ZipUtils::readZipEntryBytes(zipPath, QStringLiteral("0.png"));
        QVERIFY(!bytes0.isEmpty());
        QImage loadedFromZip = QImage::fromData(bytes0);
        QCOMPARE(loadedFromZip.size(), QSize(100, 100));

        // Test ensureExtracted
        const QString composite = ZipUtils::makeZipPath(zipPath, QStringLiteral("1.png"));
        const QString extractedCache = ZipUtils::ensureExtracted(composite);
        QVERIFY(!extractedCache.isEmpty());
        QVERIFY(QFile::exists(extractedCache));

        // Test Scanner integration
        Database db(dbPath);
        QVERIFY(db.initSchema());

        Scanner scanner(root, dbPath);
        QSignalSpy finishedSpy(&scanner, &Scanner::finished);
        scanner.start();
        QVERIFY(finishedSpy.wait(5000));

        // Total scanned should be:
        // - raw_only.png (raw file)
        // - bundle1/0.png (raw file)
        // - bundle1.zip#1.png (zip image whose raw duplicate does not exist)
        // Note: bundle1.zip#0.png MUST BE OMITTED because bundle1/0.png exists!
        scanner.wait();
        QueryFilter allFilter;
        allFilter.rootDir = root;
        const auto allItems = db.queryImages(allFilter);
        QCOMPARE(allItems.size(), 3);

        QStringList scannedPaths;
        for (const auto& itm : allItems) {
            scannedPaths.append(itm.path);
        }

        QVERIFY(scannedPaths.contains(rawImgPath));
        QVERIFY(scannedPaths.contains(extracted0));
        QVERIFY(scannedPaths.contains(composite));
        QVERIFY(!scannedPaths.contains(ZipUtils::makeZipPath(zipPath, QStringLiteral("0.png"))));

        // Test thumbnail generation for the zip composite path
        const QString thumbPath = tmpDir.filePath(QStringLiteral("zip_thumb.webp"));
        ImageFeatures feat = ColorUtils::generateThumbnailAndFeatures(composite, thumbPath, QSize(150, 150));
        QVERIFY(feat.valid);
        QCOMPARE(feat.width, 100);
        QCOMPARE(feat.height, 100);
        QVERIFY(QFile::exists(thumbPath));
    }

    void testPixivZipsDatasetIfPresent() {
        const QString pixivDir = QStringLiteral("/home/sagnik/Documents/pixiv_zips");
        if (!QDir(pixivDir).exists()) {
            QSKIP("/home/sagnik/Documents/pixiv_zips does not exist, skipping real dataset test");
        }

        QTemporaryDir tmpDir;
        const QString dbPath = tmpDir.filePath(QStringLiteral("pixiv_test.db"));
        Database db(dbPath);
        QVERIFY(db.initSchema());

        Scanner scanner(pixivDir, dbPath);
        QSignalSpy finishedSpy(&scanner, &Scanner::finished);
        scanner.start();
        QVERIFY(finishedSpy.wait(30000));
        scanner.wait();

        QueryFilter filter;
        filter.rootDir = pixivDir;
        const auto items = db.queryImages(filter);

        // Expect 1,155 items (109 raw + 1,046 zip, with 109 duplicates suppressed)
        QCOMPARE(items.size(), 1155);

        int rawCount = 0;
        int zipCount = 0;
        for (const auto& item : items) {
            if (ZipUtils::isZipPath(item.path)) {
                zipCount++;
            } else {
                rawCount++;
            }
        }
        QCOMPARE(rawCount, 109);
        QCOMPARE(zipCount, 1046);
    }
};

QTEST_MAIN(TestOmniView)
#include "test_omniview.moc"
