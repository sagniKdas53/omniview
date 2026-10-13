#include "ColorUtils.h"
#include "Config.h"
#include "Database.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QSemaphore>
#include <QThread>
#include <QtTest>

#include <memory>

using namespace OmniView;

namespace {

ImageRecord row(const QString& path, const QString& subfolder, qint64 size = 10, double mtime = 100.0) {
    ImageRecord r;
    r.path = path;
    r.filename = QFileInfo(path).fileName();
    r.subfolder = subfolder;
    r.fileSize = size;
    r.mtime = mtime;
    return r;
}

ImageRecord fetch(Database& db, const QString& path) {
    QueryFilter f;
    f.exactPath = path;
    const auto rows = db.queryImages(f);
    return rows.isEmpty() ? ImageRecord() : rows.first();
}

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
}

} // namespace

class DatabaseRegressionsTest : public QObject {
    Q_OBJECT

private slots:
    void rootSlashAndStatsAreScoped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());

        const QString rootA = dir.filePath(QStringLiteral("root-a"));
        const QString rootB = dir.filePath(QStringLiteral("root-b"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        const QString pathA = rootA + QStringLiteral("/a.png");
        const QString pathB = rootB + QStringLiteral("/b.png");
        QVERIFY(QFile(pathA).open(QIODevice::WriteOnly));
        QVERIFY(QFile(pathB).open(QIODevice::WriteOnly));
        QVector<ImageRecord> rows = {
            row(pathA, QString(), 10, 1.0),
            row(pathB, QString(), 20, 2.0)
        };
        QVERIFY(db.batchSyncFiles(rows));
        QVERIFY(db.updateImageFeatures(pathA, 10, 10, QStringLiteral("square"), QStringLiteral("red"), 1, 2, 3, QStringLiteral("abcd"), QStringLiteral("thumb-a")));
        QVERIFY(db.toggleFavorite(pathA));

        QueryFilter slash;
        slash.rootDir = QDir::rootPath();
        const auto all = db.queryImages(slash);
        QCOMPARE(all.size(), 2);
        const Stats rootStats = db.getStats(rootA);
        QCOMPARE(rootStats.totalImages, 1);
        QCOMPARE(rootStats.totalBytes, qint64(10));
        QCOMPARE(rootStats.indexedImages, 1);
        QCOMPARE(rootStats.favoritesCount, 1);
        QCOMPARE(db.getStats(rootB).favoritesCount, 0);
        QCOMPARE(db.getSubfoldersWithCounts(QDir::rootPath()).first().second, 2);
        QVERIFY(QFile::remove(pathB));
        QCOMPARE(db.pruneMissingFiles(QDir::rootPath()), 1);
        QCOMPARE(db.getStats().totalImages, 1);
    }

    void schemaErrorPreservesDatabase() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("index.db");
        const QString connection = "schema-regression";
        {
            QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", connection);
            seed.setDatabaseName(path);
            QVERIFY(seed.open());
            QSqlQuery query(seed);
            QVERIFY(query.exec("CREATE VIEW images AS SELECT 1 AS value"));
            seed.close();
        }
        QSqlDatabase::removeDatabase(connection);
        Database db(path);
        QVERIFY(!db.initSchema());
        QVERIFY(QFile::exists(path));
        QVERIFY(QDir(dir.path()).entryList({"*.corrupt.*"}, QDir::Files).isEmpty());
    }

    void searchTreatsWildcardsLiterally() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());

        QVector<ImageRecord> rows = {
            row(QStringLiteral("/literal/100%_done.png"), QString(), 1, 1.0),
            row(QStringLiteral("/wildcard/100Xdone.png"), QString(), 1, 2.0),
            row(QStringLiteral("/underscore/100A_done.png"), QString(), 1, 3.0)
        };
        QVERIFY(db.batchSyncFiles(rows));

        QueryFilter percent;
        percent.searchTerm = QStringLiteral("%");
        QCOMPARE(db.queryImages(percent).size(), 1);

        QueryFilter underscore;
        underscore.searchTerm = QStringLiteral("_");
        QCOMPARE(db.queryImages(underscore).size(), 2);
    }

    void favouritesWithAllFoldersScope() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/r/a.png"), QString()), row(QStringLiteral("/r/s/b.png"), QStringLiteral("s")),
                                   row(QStringLiteral("/r/c.png"), QString())}));
        QVERIFY(db.toggleFavorite(QStringLiteral("/r/a.png")));
        QVERIFY(db.toggleFavorite(QStringLiteral("/r/s/b.png")));
        QueryFilter f;
        f.rootDir = QStringLiteral("/r");
        f.favoriteOnly = true;
        QCOMPARE(db.queryImages(f).size(), 2);
        QCOMPARE(db.getStats(QStringLiteral("/r")).favoritesCount, 2);
    }

    void markerNamedFoldersAreOrdinaryFolders() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/s/top.png"), QString()),
                                   row(QStringLiteral("/s/__root__/x.png"), QStringLiteral("__root__")),
                                   row(QStringLiteral("/s/__all__/y.png"), QStringLiteral("__all__"))}));
        const auto folders = db.getSubfoldersWithCounts(QStringLiteral("/s"));
        QCOMPARE(folders.size(), 3);
        QueryFilter f;
        f.rootDir = QStringLiteral("/s");
        f.folderScope = FolderScope::Root;
        QCOMPARE(db.queryImages(f).size(), 1);
        f.folderScope = FolderScope::Subfolder;
        f.subfolder = QStringLiteral("__root__");
        QCOMPARE(db.queryImages(f).size(), 1);
        f.subfolder = QStringLiteral("__all__");
        QCOMPARE(db.queryImages(f).first().path, QStringLiteral("/s/__all__/y.png"));
    }

    void rescanFromNestedRootRehomesSubfolder() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString path = QStringLiteral("/pics/wall/x.png");
        QVERIFY(db.batchSyncFiles({row(path, QStringLiteral("wall"))}));
        QVERIFY(db.updateImageFeatures(path, 10, 10, QStringLiteral("square"), QStringLiteral("red"), 1, 2, 3,
                                       QStringLiteral("00ff00ff00ff00ff"), QStringLiteral("/t.webp")));
        // Same file, unchanged, scanned from the child root.
        QVERIFY(db.batchSyncFiles({row(path, QString())}));
        const ImageRecord rec = fetch(db, path);
        QCOMPARE(rec.subfolder, QString());
        QVERIFY(rec.indexed); // features kept
        QCOMPARE(rec.colorName, QStringLiteral("red"));
        QueryFilter rootOnly;
        rootOnly.rootDir = QStringLiteral("/pics/wall");
        rootOnly.folderScope = FolderScope::Root;
        QCOMPARE(db.queryImages(rootOnly).size(), 1);
    }

    void subSecondMtimeDifferencesDoNotInvalidate() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString path = QStringLiteral("/m/a.png");
        // Another OmniView implementation stored a fractional mtime.
        QVERIFY(db.batchSyncFiles({row(path, QString(), 10, 1700000000.73)}));
        QVERIFY(db.updateImageFeatures(path, 10, 10, QStringLiteral("square"), QStringLiteral("red"), 1, 2, 3,
                                       QStringLiteral("00ff00ff00ff00ff"), QStringLiteral("/t.webp")));
        QVERIFY(db.batchSyncFiles({row(path, QString(), 10, 1700000000.0)}));
        QVERIFY(fetch(db, path).indexed);
        QVERIFY(db.getUnindexedPaths().isEmpty());
        QVERIFY(db.batchSyncFiles({row(path, QString(), 10, 1700000005.0)}));
        QVERIFY(!fetch(db, path).indexed);
    }

    void toggleFavoriteWaitsForOtherWriters() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("index.db"));
        Database db(path);
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/f/a.png"), QString())}));
        // Make sure the GUI-side connection has an idle read state, as in the app.
        QCOMPARE(db.queryImages(QueryFilter()).size(), 1);

        QSemaphore locked;
        std::unique_ptr<QThread> writer(QThread::create([&]() {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("locker"));
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            {
                QSqlQuery q(raw);
                QVERIFY(q.exec(QStringLiteral("BEGIN IMMEDIATE")));
                locked.release();
                QThread::msleep(600);
                QVERIFY(q.exec(QStringLiteral("COMMIT")));
            }
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase(QStringLiteral("locker"));
        }));
        writer->start();
        QVERIFY(locked.tryAcquire(1, 5000));
        QElapsedTimer t;
        t.start();
        bool fav = false;
        QVERIFY2(db.toggleFavorite(QStringLiteral("/f/a.png"), &fav), qPrintable(db.lastError()));
        QVERIFY(fav);
        QVERIFY(t.elapsed() >= 300); // it waited for the lock instead of failing
        QVERIFY(writer->wait(5000));
    }

    void staleWorkerResultIsNotStored() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString path = QStringLiteral("/w/a.png");
        QVERIFY(db.batchSyncFiles({row(path, QString(), 10, 100.0)}));
        // The file changes while a worker is decoding the old version.
        QVERIFY(db.batchSyncFiles({row(path, QString(), 11, 101.0)}));
        ImageFeatures feat;
        feat.width = 80;
        feat.height = 40;
        feat.colorName = QStringLiteral("red");
        feat.dhash = QStringLiteral("0123456789abcdef");
        feat.thumbPath = QStringLiteral("/t.webp");
        QCOMPARE(db.updateImageFeaturesIfUnchanged(path, 100.0, 10, feat), Database::UpdateResult::Stale);
        QCOMPARE(db.getUnindexedPaths().size(), 1);
        QCOMPARE(db.updateImageFeaturesIfUnchanged(path, 101.0, 11, feat), Database::UpdateResult::Updated);
        QVERIFY(db.getUnindexedPaths().isEmpty());
    }

    void rootRangeIsCaseSensitiveAndExact() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/data/Photos/a.png"), QString()),
                                   row(QStringLiteral("/data/photos/b.png"), QString()),
                                   row(QStringLiteral("/data/Photos2/c.png"), QString()),
                                   row(QStringLiteral("/data/Photos/d%_.png"), QString())}));
        QueryFilter f;
        f.rootDir = QStringLiteral("/data/Photos");
        QCOMPARE(db.queryImages(f).size(), 2);
        QCOMPARE(db.getStats(QStringLiteral("/data/Photos")).totalImages, 2);
        QCOMPARE(db.getStats(QStringLiteral("/data/photos")).totalImages, 1);
        const auto range = Database::rootPathRange(QStringLiteral("/"));
        QCOMPARE(range.first, QStringLiteral("/"));
        QCOMPARE(range.second, QStringLiteral("0"));
    }

    void unindexedPathsAreScopedToRoot() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/a/1.png"), QString()), row(QStringLiteral("/b/2.png"), QString())}));
        QCOMPARE(db.getUnindexedPaths(QStringLiteral("/a")).size(), 1);
        QCOMPARE(db.getUnindexedPaths().size(), 2);
    }

    void pruneRemovesEntriesGoneFromListedArchive() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString root = dir.filePath(QStringLiteral("root"));
        const QString zip = root + QStringLiteral("/z.zip");
        touch(zip);
        const QString keep = zip + QStringLiteral("#a.png");
        const QString gone = zip + QStringLiteral("#b.png");
        QVERIFY(db.batchSyncFiles({row(keep, QStringLiteral("z.zip")), row(gone, QStringLiteral("z.zip"))}));

        // Archive could not be listed this time: keep everything.
        ScanObservation unreadable;
        QCOMPARE(db.pruneMissingFiles(root, &unreadable), 0);

        ScanObservation obs;
        obs.listedArchives.insert(zip);
        obs.seenPaths.insert(keep);
        QCOMPARE(db.pruneMissingFiles(root, &obs), 1);
        QVERIFY(!fetch(db, keep).path.isEmpty());
        QVERIFY(fetch(db, gone).path.isEmpty());
    }

    void pruneKeepsIndexWhenRootIsMissing() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString root = dir.filePath(QStringLiteral("unmounted"));
        QVERIFY(db.batchSyncFiles({row(root + QStringLiteral("/a.png"), QString())}));
        QVERIFY(db.toggleFavorite(root + QStringLiteral("/a.png")));
        QCOMPARE(db.pruneMissingFiles(root), 0);
        QVERIFY(fetch(db, root + QStringLiteral("/a.png")).isFavorite);
    }

    void pruneHonoursCancellation() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        const QString root = dir.filePath(QStringLiteral("r"));
        QVERIFY(QDir().mkpath(root));
        QVERIFY(db.batchSyncFiles({row(root + QStringLiteral("/missing.png"), QString())}));
        QCOMPARE(db.pruneMissingFiles(root, nullptr, []() { return true; }), 0);
        QCOMPARE(db.getStats().totalImages, 1);
    }

    void connectionsAreRemovedWithTheirOwner() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("index.db"));
        const QStringList before = QSqlDatabase::connectionNames();
        {
            Database db(path);
            QVERIFY(db.initSchema());
            QCOMPARE(QSqlDatabase::connectionNames().size(), before.size() + 1);
        }
        for (int i = 0; i < 4; ++i) {
            std::unique_ptr<QThread> t(QThread::create([path, i]() {
                Database db(path);
                QVERIFY(db.batchSyncFiles({row(QStringLiteral("/t/%1.png").arg(i), QString())}));
            }));
            t->start();
            QVERIFY(t->wait(5000));
        }
        QCOMPARE(QSqlDatabase::connectionNames(), before);
        Database check(path);
        QCOMPARE(check.getStats().totalImages, 4);
    }

    void corruptionRecoveryKeepsWalWithBackup() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("index.db"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("THIS IS NOT A VALID SQLITE DATABASE FILE - CORRUPTED HEADER DATA 1234567890");
        f.close();
        QFile wal(path + QStringLiteral("-wal"));
        QVERIFY(wal.open(QIODevice::WriteOnly));
        wal.write("wal-bytes");
        wal.close();
        Database db(path);
        QVERIFY(db.initSchema());
        const QStringList backups = QDir(dir.path()).entryList({QStringLiteral("index.db.corrupt.*")}, QDir::Files);
        QVERIFY(backups.size() >= 2);
        bool walKept = false;
        for (const QString& b : backups) walKept = walKept || b.endsWith(QStringLiteral("-wal"));
        QVERIFY(walKept);
    }

    void nameSortIgnoresCase() {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("index.db")));
        QVERIFY(db.initSchema());
        QVERIFY(db.batchSyncFiles({row(QStringLiteral("/n/Zebra.png"), QString()), row(QStringLiteral("/n/apple.png"), QString()),
                                   row(QStringLiteral("/n/Mango.png"), QString())}));
        QueryFilter f;
        f.sortBy = QStringLiteral("name_asc");
        const auto rows = db.queryImages(f);
        QCOMPARE(rows.at(0).filename, QStringLiteral("apple.png"));
        QCOMPARE(rows.at(1).filename, QStringLiteral("Mango.png"));
        QCOMPARE(rows.at(2).filename, QStringLiteral("Zebra.png"));
    }

    void legacyMigrationIncludesWalContents() {
        QTemporaryDir dir;
        const QString legacy = dir.filePath(QStringLiteral("pixiv_index.db"));
        const QString target = dir.filePath(QStringLiteral("omniview_index.db"));
        Database writer(legacy);
        QVERIFY(writer.initSchema());
        // Keep the change in the WAL: the writer connection stays open, so no
        // checkpoint has copied it into the main file yet.
        QVERIFY(writer.batchSyncFiles({row(QStringLiteral("/l/a.png"), QString())}));
        QVERIFY(writer.toggleFavorite(QStringLiteral("/l/a.png")));
        QVERIFY(QFile::exists(legacy + QStringLiteral("-wal")));
        QVERIFY(Config::migrateLegacyDatabase(legacy, target));
        Database migrated(target);
        QVERIFY(migrated.initSchema());
        QVERIFY(fetch(migrated, QStringLiteral("/l/a.png")).isFavorite);
        QVERIFY(!QFile::exists(target + QStringLiteral(".migrating")));
    }

    void brokenQt5HashesAreClearedOnStartup() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("index.db"));
        {
            Database db(path);
            QVERIFY(db.initSchema());
            QVERIFY(db.batchSyncFiles({row(QStringLiteral("/h/a.png"), QString())}));
            QVERIFY(db.updateImageFeatures(QStringLiteral("/h/a.png"), 1, 1, QStringLiteral("square"), QStringLiteral("red"),
                                           1, 2, 3, QStringLiteral("2222222222222222"), QStringLiteral("/t.webp")));
        }
        Database db(path);
        QVERIFY(db.initSchema());
        QVERIFY(fetch(db, QStringLiteral("/h/a.png")).dhash.isEmpty());
        QCOMPARE(db.getUnindexedPaths().size(), 1);
    }
};

QTEST_GUILESS_MAIN(DatabaseRegressionsTest)
#include "test_database_regressions.moc"
