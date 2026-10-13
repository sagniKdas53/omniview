#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <cstring>

#include "Database.h"
#include "Scanner.h"
#include "TestSupport.h"
#include "ZipUtils.h"
#include "miniz.h"

using namespace OmniView;

namespace {

struct ZipEntrySpec {
    QByteArray rawName;   // bytes written into the archive
    QByteArray data;
    bool asciiFlag = false; // true: do not set the UTF-8 name flag
};

bool writeZip(const QString& path, const QVector<ZipEntrySpec>& entries) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    bool ok = true;
    for (const auto& e : entries) {
        const mz_uint flags = static_cast<mz_uint>(MZ_DEFAULT_LEVEL) | (e.asciiFlag ? MZ_ZIP_FLAG_ASCII_FILENAME : 0);
        ok = ok && mz_zip_writer_add_mem(&zip, e.rawName.constData(), e.data.constData(),
                                         static_cast<size_t>(e.data.size()), flags);
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    return mz_zip_writer_end(&zip) && ok;
}

QByteArray pngBytes(const QColor& c, int w = 8, int h = 8) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

// Patches the central-directory record of entryName: callback gets a pointer
// to the 46-byte fixed header.
template <typename Fn>
bool patchCentralRecord(const QString& zipPath, const QByteArray& entryName, Fn fn) {
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadWrite)) return false;
    QByteArray data = f.readAll();
    decltype(QByteArray().size()) pos = 0; // int on Qt 5, qsizetype on Qt 6
    bool patched = false;
    while ((pos = data.indexOf(QByteArray("PK\x01\x02", 4), pos)) >= 0) {
        const auto nameLen = static_cast<quint8>(data[pos + 28]) | (static_cast<quint8>(data[pos + 29]) << 8);
        if (data.mid(pos + 46, nameLen) == entryName) {
            fn(reinterpret_cast<unsigned char*>(data.data() + pos));
            patched = true;
        }
        pos += 46;
    }
    f.seek(0);
    f.write(data);
    return patched;
}

struct ScanResult {
    bool finished = false;
    int count = 0;
    Stats stats;
    QString error;
};

// Runs a scan and collects its result through a queued connection (the
// scanner emits from its own thread; QSignalSpy is not thread-safe).
ScanResult runScan(const QString& root, const QString& dbPath) {
    ScanResult result;
    Scanner scanner(root, dbPath);
    QEventLoop loop;
    QObject context;
    QObject::connect(&scanner, &Scanner::scanFinished, &context,
                     [&](int count, const Stats& stats, const QString& error) {
        result.finished = true;
        result.count = count;
        result.stats = stats;
        result.error = error;
        loop.quit();
    }, Qt::QueuedConnection);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    scanner.start();
    loop.exec();
    scanner.wait();
    return result;
}

QStringList indexedPaths(Database& db, const QString& root) {
    QueryFilter f;
    f.rootDir = root;
    QStringList out;
    for (const auto& r : db.queryImages(f)) out.append(r.path);
    out.sort();
    return out;
}

} // namespace

class ZipScannerTest : public QObject {
    Q_OBJECT
private slots:
    void compositePathSplitsAtZipMarker() {
        QString zip, inner;
        QVERIFY(ZipUtils::isZipPath(QStringLiteral("/home/u/C# art/set.zip#01.png"), &zip, &inner));
        QCOMPARE(zip, QStringLiteral("/home/u/C# art/set.zip"));
        QCOMPARE(inner, QStringLiteral("01.png"));
        QVERIFY(ZipUtils::isZipPath(QStringLiteral("/d/best#1.ZIP#sub/a.png"), &zip, &inner));
        QCOMPARE(zip, QStringLiteral("/d/best#1.ZIP"));
        QCOMPARE(inner, QStringLiteral("sub/a.png"));
        QVERIFY(!ZipUtils::isZipPath(QStringLiteral("/d/C#/plain.png")));
        QVERIFY(!ZipUtils::isZipPath(QStringLiteral("/d/photo.png")));
    }

    void innerPathRules() {
        QVERIFY(ZipUtils::isSafeInnerPath(QStringLiteral("./img.png")));
        QVERIFY(ZipUtils::isSafeInnerPath(QStringLiteral("1:30.png")));
        QVERIFY(!ZipUtils::isSafeInnerPath(QStringLiteral("C:/evil.png")));
        QVERIFY(!ZipUtils::isSafeInnerPath(QStringLiteral("a/../../evil.png")));
        QVERIFY(ZipUtils::isHiddenInnerPath(QStringLiteral(".thumbs/a.png")));
        QVERIFY(ZipUtils::isHiddenInnerPath(QStringLiteral("sub/.cache/a.png")));
        QVERIFY(ZipUtils::isHiddenInnerPath(QStringLiteral("__MACOSX/a.png")));
        QVERIFY(!ZipUtils::isHiddenInnerPath(QStringLiteral("./sub/a.png")));
    }

    void listingSkipsEntriesThatCanNeverLoad() {
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("mixed.zip"));
        const QByteArray longName = QByteArray(600, 'n') + ".png";
        QVERIFY(writeZip(zip, {
            {"ok.png", pngBytes(Qt::red)},
            {"./dot.png", pngBytes(Qt::red)},
            {"empty.png", QByteArray()},
            {".thumbs/t.png", pngBytes(Qt::red)},
            {"sub/.cache/c.png", pngBytes(Qt::red)},
            {"__MACOSX/m.png", pngBytes(Qt::red)},
            {"secret.png", pngBytes(Qt::green)},
            {"link.png", QByteArray("../../../../etc/passwd")},
            {longName, pngBytes(Qt::blue)},
        }));
        QVERIFY(patchCentralRecord(zip, "secret.png", [](unsigned char* h) { h[8] |= 0x01; }));
        QVERIFY(patchCentralRecord(zip, "link.png", [](unsigned char* h) {
            h[5] = 3;                 // version made by: Unix
            h[40] = 0xFF; h[41] = 0xA1; // external attrs high word: S_IFLNK | 0777
        }));

        bool ok = false;
        const auto entries = ZipUtils::listZipImages(zip, &ok);
        QVERIFY(ok);
        QStringList names;
        for (const auto& e : entries) names.append(e.innerPath);
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("./dot.png"), QString::fromLatin1(longName), QStringLiteral("ok.png")}));
        QVERIFY(!ZipUtils::readZipEntryBytes(zip, QString::fromLatin1(longName)).isEmpty());
        QVERIFY(!ZipUtils::readZipEntryBytes(zip, QStringLiteral("./dot.png")).isEmpty());

        bool badOk = true;
        QVERIFY(ZipUtils::listZipImages(tmp.filePath(QStringLiteral("missing.zip")), &badOk).isEmpty());
        QVERIFY(!badOk);
    }

    void cp437NamesRoundTrip() {
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("dos.zip"));
        // "Bild_ä.png" in CP437 (0x84), no UTF-8 flag.
        QVERIFY(writeZip(zip, {{QByteArray("Bild_\x84.png"), pngBytes(Qt::red), true},
                               {QByteArray("utf8_\xc3\xa9.png"), pngBytes(Qt::blue), true}}));
        const auto entries = ZipUtils::listZipImages(zip);
        QStringList names;
        for (const auto& e : entries) names.append(e.innerPath);
        QVERIFY(names.contains(QString::fromUtf8("Bild_ä.png")));
        QVERIFY(names.contains(QString::fromUtf8("utf8_é.png"))); // unflagged UTF-8 still decodes
        QVERIFY(!ZipUtils::readZipEntryBytes(zip, QString::fromUtf8("Bild_ä.png")).isEmpty());
        QVERIFY(!ZipUtils::readZipEntryBytes(zip, QString::fromUtf8("utf8_é.png")).isEmpty());
    }

    void lookupIsCaseSensitive() {
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("case.zip"));
        QVERIFY(writeZip(zip, {{"A.png", QByteArray("UPPER-CASE-ENTRY")}, {"a.png", QByteArray("lower-case-entry")}}));
        QCOMPARE(ZipUtils::readZipEntryBytes(zip, QStringLiteral("A.png")), QByteArray("UPPER-CASE-ENTRY"));
        QCOMPARE(ZipUtils::readZipEntryBytes(zip, QStringLiteral("a.png")), QByteArray("lower-case-entry"));
    }

    void compressionBombIsRejected() {
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("bomb.zip"));
        QVERIFY(writeZip(zip, {{"zeros.png", QByteArray(static_cast<int>(40LL * 1024 * 1024), '\0')}, {"fine.png", pngBytes(Qt::red)}}));
        const auto entries = ZipUtils::listZipImages(zip);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().innerPath, QStringLiteral("fine.png"));
        QVERIFY(ZipUtils::readZipEntryBytes(zip, QStringLiteral("zeros.png")).isEmpty());
    }

    void extractionKeepsNameAndFollowsArchiveChanges() {
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("set#1.zip"));
        QVERIFY(writeZip(zip, {{"pages/page_001.png", QByteArray("VERSION-1")}}));
        const QString composite = ZipUtils::makeZipPath(zip, QStringLiteral("pages/page_001.png"));
        const QString first = ZipUtils::ensureExtracted(composite);
        QVERIFY(!first.isEmpty());
        QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("page_001.png"));
        QFile f1(first);
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QCOMPARE(f1.readAll(), QByteArray("VERSION-1"));

        QTest::qWait(20); // distinct mtime
        QVERIFY(QFile::remove(zip));
        QVERIFY(writeZip(zip, {{"pages/page_001.png", QByteArray("VERSION-2-CONTENT")}}));
        {
            QFile z(zip);
            QVERIFY(z.open(QIODevice::ReadWrite));
            QVERIFY(z.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
        }
        const QString second = ZipUtils::ensureExtracted(composite);
        QFile f2(second);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f2.readAll(), QByteArray("VERSION-2-CONTENT"));
        QCOMPARE(ZipUtils::ensureExtracted(QStringLiteral("/plain/path.png")), QStringLiteral("/plain/path.png"));
    }

    void duplicateDetectionRequiresMatchingSize() {
        QTemporaryDir tmp;
        const QString dir = tmp.path();
        QFile raw(tmp.filePath(QStringLiteral("001.png")));
        QVERIFY(raw.open(QIODevice::WriteOnly));
        raw.write("vol1-page");
        raw.close();
        QVERIFY(ZipUtils::isDuplicateOfRaw(dir, QStringLiteral("vol1"), QStringLiteral("001.png"), 9));
        QVERIFY(!ZipUtils::isDuplicateOfRaw(dir, QStringLiteral("vol2"), QStringLiteral("001.png"), 12345));
        QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("bs/sub"))));
        QFile nested(tmp.filePath(QStringLiteral("sub/img.png")));
        QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("sub"))));
        QVERIFY(nested.open(QIODevice::WriteOnly));
        nested.close();
        QVERIFY(ZipUtils::isDuplicateOfRaw(dir, QStringLiteral("bs"), QStringLiteral("sub\\img.png")));
    }

    void scanKeepsArchivesWithHashInPath() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        const QString dbPath = tmp.filePath(QStringLiteral("index.db"));
        QVERIFY(writeZip(root + QStringLiteral("/C#/a.zip"), {{"001.png", pngBytes(Qt::red)}}));
        QVERIFY(writeZip(root + QStringLiteral("/best#1.zip"), {{"002.png", pngBytes(Qt::red)}}));
        Database db(dbPath);
        QVERIFY(db.initSchema());
        const ScanResult r = runScan(root, dbPath);
        QVERIFY(r.finished);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.count, 2);
        QCOMPARE(indexedPaths(db, root).size(), 2); // prune did not delete them
        const ScanResult again = runScan(root, dbPath);
        QCOMPARE(again.stats.totalImages, 2);
    }

    void rescanPrunesEntriesRemovedFromArchive() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        const QString dbPath = tmp.filePath(QStringLiteral("index.db"));
        const QString zip = root + QStringLiteral("/z.zip");
        QVERIFY(writeZip(zip, {{"a.png", pngBytes(Qt::red)}, {"b.png", pngBytes(Qt::blue)}}));
        Database db(dbPath);
        QVERIFY(db.initSchema());
        QCOMPARE(runScan(root, dbPath).count, 2);
        QVERIFY(QFile::remove(zip));
        QVERIFY(writeZip(zip, {{"a.png", pngBytes(Qt::red)}}));
        QCOMPARE(runScan(root, dbPath).count, 1);
        QCOMPARE(indexedPaths(db, root), QStringList{ZipUtils::makeZipPath(zip, QStringLiteral("a.png"))});
    }

    void cancelledScanDoesNotReportFinished() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root));
        for (int i = 0; i < 50; ++i) {
            QImage img(4, 4, QImage::Format_RGB32);
            img.fill(Qt::red);
            QVERIFY(img.save(root + QStringLiteral("/%1.png").arg(i)));
        }
        const QString dbPath = tmp.filePath(QStringLiteral("index.db"));
        Database db(dbPath);
        QVERIFY(db.initSchema());
        int finished = 0;
        {
            Scanner scanner(root, dbPath);
            QObject context;
            QObject::connect(&scanner, &Scanner::scanFinished, &context, [&]() { ++finished; }, Qt::QueuedConnection);
            scanner.cancel();
            scanner.start();
            QVERIFY(scanner.wait(5000));
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
        }
        QCOMPARE(finished, 0);
    }

    void databaseErrorsAreReported() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root));
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(root + QStringLiteral("/a.png")));
        // No schema: every insert fails.
        const ScanResult r = runScan(root, tmp.filePath(QStringLiteral("noschema.db")));
        QVERIFY(r.finished);
        QVERIFY(!r.error.isEmpty());
    }

    void missingRootReportsError() {
        QTemporaryDir tmp;
        const ScanResult r = runScan(tmp.filePath(QStringLiteral("nope")), tmp.filePath(QStringLiteral("i.db")));
        QVERIFY(r.finished);
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(r.count, 0);
    }

#ifdef Q_OS_UNIX
    void symlinkLoopsTerminate() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/sub")));
        QVERIFY(QFile::link(root, root + QStringLiteral("/sub/loop")));
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(root + QStringLiteral("/sub/a.png")));
        const QString dbPath = tmp.filePath(QStringLiteral("index.db"));
        Database db(dbPath);
        QVERIFY(db.initSchema());
        const ScanResult r = runScan(root, dbPath);
        QVERIFY(r.finished);
        QCOMPARE(r.count, 1);
    }
#endif
};

OMNIVIEW_TEST_MAIN(ZipScannerTest)
#include "test_zip_scanner.moc"
