#include "Database.h"

#include <QTemporaryDir>
#include <QFile>
#include <QSqlQuery>
#include <QDir>
#include <QtTest>

using namespace OmniView;

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
            {pathA, QStringLiteral("a.png"), QString(), 10, 1.0},
            {pathB, QStringLiteral("b.png"), QString(), 20, 2.0}
        };
        QVERIFY(db.batchSyncFiles(rows));
        QVERIFY(db.updateImageFeatures(pathA, 10, 10, QStringLiteral("square"), QStringLiteral("red"), 1, 2, 3, QStringLiteral("abcd"), QStringLiteral("thumb-a")));
        QVERIFY(db.toggleFavorite(pathA));

        QueryFilter slash;
        slash.rootDir = QStringLiteral("/");
        const auto all = db.queryImages(slash);
        QCOMPARE(all.size(), 2);
        const Stats rootStats = db.getStats(rootA);
        QCOMPARE(rootStats.totalImages, 1);
        QCOMPARE(rootStats.totalBytes, qint64(10));
        QCOMPARE(rootStats.indexedImages, 1);
        QCOMPARE(rootStats.favoritesCount, 1);
        QCOMPARE(db.getStats(rootB).favoritesCount, 0);
        QCOMPARE(db.getSubfoldersWithCounts(QStringLiteral("/")).first().second, 2);
        QVERIFY(QFile::remove(pathB));
        QCOMPARE(db.pruneMissingFiles(QStringLiteral("/")), 1);
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
            {QStringLiteral("/literal-percent.png"), QStringLiteral("100%_done.png"), QString(), 1, 1.0},
            {QStringLiteral("/wildcard-percent.png"), QStringLiteral("100Xdone.png"), QString(), 1, 2.0},
            {QStringLiteral("/literal-underscore.png"), QStringLiteral("100A_done.png"), QString(), 1, 3.0}
        };
        QVERIFY(db.batchSyncFiles(rows));

        QueryFilter percent;
        percent.searchTerm = QStringLiteral("%");
        QCOMPARE(db.queryImages(percent).size(), 1);

        QueryFilter underscore;
        underscore.searchTerm = QStringLiteral("_");
        QCOMPARE(db.queryImages(underscore).size(), 2);
    }
};

QTEST_GUILESS_MAIN(DatabaseRegressionsTest)
#include "test_database_regressions.moc"
