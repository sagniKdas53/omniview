#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "Config.h"
#include "Database.h"
#include "TestSupport.h"
#include "ThumbnailWorker.h"

using namespace OmniView;

namespace {

// Creates n images plus their DB rows and returns their paths.
QStringList makeLibrary(const QString& dir, Database& db, int n, int size = 64) {
    QStringList paths;
    QVector<ImageRecord> rows;
    for (int i = 0; i < n; ++i) {
        const QString path = dir + QStringLiteral("/img_%1.png").arg(i, 4, 10, QLatin1Char('0'));
        QImage img(size, size, QImage::Format_RGB32);
        img.fill(QColor::fromHsv((i * 37) % 360, 200, 200));
        if (!img.save(path)) return {};
        ImageRecord r;
        r.path = path;
        r.filename = QFileInfo(path).fileName();
        r.fileSize = QFileInfo(path).size();
        r.mtime = 1000.0 + i; // newest-first ordering = reverse index order
        rows.append(r);
        paths.append(path);
    }
    if (!db.batchSyncFiles(rows)) return {};
    return paths;
}

void clearThumbnailCache() {
    QDir(Config::thumbnailsDir()).removeRecursively();
    QDir().mkpath(Config::thumbnailsDir());
}

} // namespace

class ThumbnailTest : public QObject {
    Q_OBJECT
private slots:
    void init() { clearThumbnailCache(); }

    void destructorDropsQueuedWork() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QStringList paths = makeLibrary(tmp.path(), db, 200, 800);
        QCOMPARE(paths.size(), 200);
        QElapsedTimer t;
        {
            ThumbnailManager mgr(db.dbPath());
            mgr.startBackgroundIndexing();
            QCOMPARE(mgr.pendingCount(), 200);
            t.start();
        } // must not process the whole queue
        const qint64 elapsed = t.elapsed();
        const int written = static_cast<int>(QDir(Config::thumbnailsDir()).entryList(QDir::Files).size());
        QVERIFY2(written < 100, qPrintable(QStringLiteral("%1 thumbnails written during shutdown").arg(written)));
        QVERIFY2(elapsed < 5000, qPrintable(QString::number(elapsed)));
    }

    void visibleRequestPromotesQueuedBackgroundTask() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QStringList paths = makeLibrary(tmp.path(), db, 160, 600);
        ThumbnailManager mgr(db.dbPath());
        QStringList order;
        connect(&mgr, &ThumbnailManager::thumbnailReady, this,
                [&](const QString& p) { order.append(p); });
        mgr.startBackgroundIndexing();
        // Oldest image = last in the newest-first background queue.
        mgr.requestPriorityThumbnail(paths.first());
        QTRY_VERIFY_WITH_TIMEOUT(order.contains(paths.first()), 30000);
        const int position = static_cast<int>(order.indexOf(paths.first()));
        QVERIFY2(position < 40, qPrintable(QStringLiteral("visible card finished at %1").arg(position)));
        mgr.cancelPending();
    }

    void failedImagesAreNotRetriedUntilTheyChange() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString broken = tmp.filePath(QStringLiteral("broken.png"));
        QFile f(broken);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\x89PNG\r\n\x1a\nnot-really");
        f.close();
        ImageRecord rec;
        rec.path = broken;
        rec.filename = QStringLiteral("broken.png");
        rec.fileSize = 18;
        rec.mtime = 5.0;
        QVERIFY(db.batchSyncFiles({rec}));

        ThumbnailManager mgr(db.dbPath());
        QSignalSpy ready(&mgr, &ThumbnailManager::thumbnailReady);
        mgr.requestPriorityThumbnail(broken);
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(!ready.first().at(2).value<ImageFeatures>().valid);
        QVERIFY(mgr.hasFailed(broken));

        for (int i = 0; i < 10; ++i) mgr.requestPriorityThumbnail(broken); // repaints
        mgr.startBackgroundIndexing();                                       // rescans
        QCOMPARE(mgr.pendingCount(), 0);
        QTest::qWait(100);
        QCOMPARE(ready.count(), 1);

        rec.fileSize = 19; // file changed on disk
        QVERIFY(db.batchSyncFiles({rec}));
        mgr.startBackgroundIndexing();
        QCOMPARE(mgr.pendingCount(), 1);
        QTRY_COMPARE(ready.count(), 2);
    }

    void corruptCachedThumbnailIsRegenerated() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QStringList paths = makeLibrary(tmp.path(), db, 1, 80);
        const QString& source = paths.first();
        const QString thumb = Config::canonicalThumbPath(source);
        // Another implementation left a truncated thumbnail and marked the row indexed.
        QFile f(thumb);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("RIFF\x10\x00\x00\x00WEBPtruncated");
        f.close();
        QVERIFY(db.updateImageFeatures(source, 80, 80, QStringLiteral("square"), QStringLiteral("red"), 1, 2, 3,
                                       QStringLiteral("0011223344556677"), thumb));

        ThumbnailManager mgr(db.dbPath());
        QSignalSpy ready(&mgr, &ThumbnailManager::thumbnailReady);
        mgr.requestPriorityThumbnail(source);
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(ready.first().at(2).value<ImageFeatures>().valid);
        QVERIFY(!mgr.getCachedPixmap(source).isNull());
        QVERIFY(!QImage(thumb).isNull());
        mgr.requestPriorityThumbnail(source); // served from the fixed file
        QTRY_COMPARE(ready.count(), 2);
        QTest::qWait(100);
        QCOMPARE(ready.count(), 2); // no self-sustaining loop
    }

    void legacyThumbnailIsNeverOverwritten() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QStringList paths = makeLibrary(tmp.path(), db, 1, 50);
        const QString& source = paths.first();
        const QString md5 = QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Md5).toHex());
        const QString legacy = Config::thumbnailsDir() + QStringLiteral("/") + md5 + QStringLiteral(".jpg");
        QImage old(10, 10, QImage::Format_RGB32);
        old.fill(Qt::black);
        QVERIFY(old.save(legacy, "JPEG"));
        QFile legacyFile(legacy);
        QVERIFY(legacyFile.open(QIODevice::ReadOnly));
        const QByteArray legacyBefore = legacyFile.readAll();
        legacyFile.close();

        ThumbnailManager mgr(db.dbPath());
        QSignalSpy ready(&mgr, &ThumbnailManager::thumbnailReady);
        mgr.requestPriorityThumbnail(source); // row unindexed -> regenerate
        QTRY_COMPARE(ready.count(), 1);
        const auto feat = ready.first().at(2).value<ImageFeatures>();
        QVERIFY(feat.valid);
        QCOMPARE(feat.thumbPath, Config::canonicalThumbPath(source));
        QFile l(legacy);
        QVERIFY(l.open(QIODevice::ReadOnly));
        QCOMPARE(l.readAll(), legacyBefore);
    }

    void clearedHashIsRecomputedFromThumbnail() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QStringList paths = makeLibrary(tmp.path(), db, 1, 90);
        const QString& source = paths.first();
        ThumbnailManager mgr(db.dbPath());
        QSignalSpy ready(&mgr, &ThumbnailManager::thumbnailReady);
        mgr.requestPriorityThumbnail(source);
        QTRY_COMPARE(ready.count(), 1);
        const QString hash = ready.first().at(2).value<ImageFeatures>().dhash;
        QVERIFY(!hash.isEmpty());

        // Simulate the start-up fix-up of a broken Qt 5 hash.
        QueryFilter f;
        f.exactPath = source;
        const ImageRecord rec = db.queryImages(f).first();
        QVERIFY(db.updateImageFeatures(source, rec.width, rec.height, rec.aspectType, rec.colorName,
                                       rec.colorR, rec.colorG, rec.colorB, QString(), rec.thumbPath));
        const qint64 thumbMtime = QFileInfo(rec.thumbPath).lastModified().toMSecsSinceEpoch();
        QTest::qWait(20);
        mgr.startBackgroundIndexing();
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(ready.last().at(2).value<ImageFeatures>().dhash, hash);
        QCOMPARE(db.queryImages(f).first().dhash, hash);
        QCOMPARE(QFileInfo(rec.thumbPath).lastModified().toMSecsSinceEpoch(), thumbMtime); // not regenerated
    }

    void cancelPendingEmptiesQueue() {
        QTemporaryDir tmp;
        Database db(tmp.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QCOMPARE(makeLibrary(tmp.path(), db, 120, 800).size(), 120);
        ThumbnailManager mgr(db.dbPath());
        int ready = 0;
        connect(&mgr, &ThumbnailManager::thumbnailReady, this, [&]() { ++ready; });
        mgr.startBackgroundIndexing();
        mgr.cancelPending();
        QTRY_COMPARE(mgr.pendingCount(), 0);
        QVERIFY(ready < 120);
    }
};

OMNIVIEW_TEST_MAIN(ThumbnailTest)
#include "test_thumbnails.moc"
