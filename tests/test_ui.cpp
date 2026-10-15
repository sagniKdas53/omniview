#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtTest>

#include <functional>

#include "Config.h"
#include "Database.h"
#include "FilterState.h"
#include "GalleryDelegate.h"
#include "GalleryModel.h"
#include "GalleryView.h"
#include "LibraryWatcher.h"
#include "MainWindow.h"
#include "TestSupport.h"
#include "ViewerWindow.h"

using namespace OmniView;

namespace {

QVector<ImageRecord> records(int n, const QString& prefix = QStringLiteral("/media/img")) {
    QVector<ImageRecord> out;
    for (int i = 0; i < n; ++i) {
        ImageRecord r;
        r.path = QStringLiteral("%1%2.jpg").arg(prefix).arg(i);
        r.filename = QFileInfo(r.path).fileName();
        out.append(r);
    }
    return out;
}

bool saveImage(const QString& path, int w = 32, int h = 32, const QColor& c = Qt::red) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    return img.save(path);
}

void sendMouse(QWidget* target, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(type, QPointF(pos), QPointF(target->mapToGlobal(pos)), button, buttons, mods);
    QApplication::sendEvent(target, &ev);
}

QPoint centerOf(GalleryView& view, int row) {
    return view.visualRect(view.model()->index(row, 0)).center();
}

// Opens the card context menu at row, runs duringMenu while it is open, then
// triggers the first action whose text contains actionText.
void runCardMenu(GalleryView& view, int row, const QString& actionText,
                 const std::function<void()>& duringMenu) {
    QTimer::singleShot(0, &view, [&view, actionText, duringMenu]() {
        auto* menu = view.findChild<QMenu*>();
        if (!menu) return;
        duringMenu();
        for (QAction* a : menu->actions()) {
            if (a->text().contains(actionText)) {
                a->trigger();
                break;
            }
        }
        menu->close();
    });
    const QPoint pos = centerOf(view, row);
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, view.viewport()->mapToGlobal(pos));
    QApplication::sendEvent(view.viewport(), &ev);
}

struct ViewFixture {
    GalleryModel model;
    GalleryDelegate delegate{nullptr};
    GalleryView view{nullptr};
    explicit ViewFixture(int n) {
        delegate.setCardSize(110);
        view.setItemDelegate(&delegate);
        model.setItems(records(n));
        view.setModel(&model);
        view.resize(900, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
    }
};

} // namespace

class UiTest : public QObject {
    Q_OBJECT
private slots:
    // ---------------------------------------------------------------- FilterState
    void favouritesScopeQueriesAllFolders() {
        FilterState f;
        f.selectFavorites();
        const QueryFilter q = f.toQueryFilter(QStringLiteral("/root"));
        QVERIFY(q.favoriteOnly);
        QCOMPARE(q.folderScope, FolderScope::All);
        QCOMPARE(q.rootDir, QStringLiteral("/root"));
    }

    void folderScopesMapToQueries() {
        FilterState f;
        f.selectSubfolder(QStringLiteral("__root__")); // a real folder with a marker-like name
        QueryFilter q = f.toQueryFilter(QString());
        QCOMPARE(q.folderScope, FolderScope::Subfolder);
        QCOMPARE(q.subfolder, QStringLiteral("__root__"));
        QVERIFY(!q.favoriteOnly);
        f.selectSubfolder(QString());
        QCOMPARE(f.scope(), FilterState::Scope::RootFolder);
        QCOMPARE(f.toQueryFilter(QString()).folderScope, FolderScope::Root);
    }

    void newRootResetsScopeButKeepsPreferences() {
        FilterState f;
        f.selectFavorites();
        f.toggleColor(QStringLiteral("red"));
        f.setSortBy(QStringLiteral("size_desc"));
        f.setSimilarityTarget(QStringLiteral("ffff000000000000"), QStringLiteral("a.png"));
        f.resetForNewRoot();
        QCOMPARE(f.scope(), FilterState::Scope::AllImages);
        QVERIFY(!f.hasSimilarityTarget());
        QVERIFY(!f.toQueryFilter(QString()).favoriteOnly);
        QCOMPARE(f.colorName(), QStringLiteral("red"));
        QCOMPARE(f.sortBy(), QStringLiteral("size_desc"));
        f.toggleColor(QStringLiteral("red"));
        QCOMPARE(f.colorName(), QStringLiteral("all"));
    }

    void similarityRankingIsStableAndKeepsFlatHashes() {
        QVector<ImageRecord> in = records(5);
        in[0].dhash = QStringLiteral("0000000000000003"); // distance 2
        in[1].dhash = QStringLiteral("0000000000000000"); // distance 0 (flat image)
        in[2].dhash = QString();                          // no hash
        in[3].dhash = QStringLiteral("0000000000000001"); // distance 1
        in[4].dhash = QStringLiteral("0000000000000002"); // distance 1, after [3] in input order
        const auto out = FilterState::rankBySimilarity(in, QStringLiteral("0000000000000000"));
        QStringList paths;
        for (const auto& r : out) paths.append(r.path);
        QCOMPARE(paths, (QStringList{in[1].path, in[3].path, in[4].path, in[0].path}));
        QCOMPARE(FilterState::rankBySimilarity(in, QStringLiteral("ffffffffffffffff")).size(), 0);
    }

    // ------------------------------------------------------------ LibraryWatcher
    void watcherCoversNestedFoldersButNotHiddenOrSymlinks() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/a/b/c")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/.hidden/x")));
        QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("elsewhere"))));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(tmp.filePath(QStringLiteral("elsewhere")), root + QStringLiteral("/link")));
#endif
        LibraryWatcher w;
        w.setRoot(root);
        const QStringList dirs = w.watchedDirectories();
        QVERIFY(dirs.contains(root + QStringLiteral("/a/b/c")));
        for (const QString& d : dirs) {
            QVERIFY2(!d.contains(QStringLiteral("/.hidden")), qPrintable(d));
            QVERIFY2(!d.endsWith(QStringLiteral("/link")), qPrintable(d));
        }

        w.setDebounceInterval(50);
        QSignalSpy spy(&w, &LibraryWatcher::rescanRequested);
        QVERIFY(saveImage(root + QStringLiteral("/a/b/c/new.png")));
        QVERIFY(spy.wait(5000));
    }

    void watcherRefreshIsDiffed() {
        QTemporaryDir tmp;
        const QString root = tmp.path();
        QVERIFY(QDir().mkpath(root + QStringLiteral("/keep")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/gone")));
        LibraryWatcher w;
        w.setRoot(root);
        QVERIFY(w.watchedDirectories().contains(root + QStringLiteral("/gone")));
        QVERIFY(QDir(root + QStringLiteral("/gone")).removeRecursively());
        QVERIFY(QDir().mkpath(root + QStringLiteral("/added")));
        w.refresh();
        const QStringList dirs = w.watchedDirectories();
        QVERIFY(dirs.contains(root + QStringLiteral("/keep")));
        QVERIFY(dirs.contains(root + QStringLiteral("/added")));
        QVERIFY(!dirs.contains(root + QStringLiteral("/gone")));
    }

    void watcherCapsWatchCount() {
        QTemporaryDir tmp;
        for (int i = 0; i < 10; ++i) QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("d%1").arg(i))));
        LibraryWatcher w;
        w.setMaxWatches(4);
        w.setRoot(tmp.path());
        QCOMPARE(w.watchedDirectories().size(), 4);
        QCOMPARE(w.unwatchedDirectories().size(), 7);
    }

    void watcherDebounceHasMaximumDelay() {
        QTemporaryDir tmp;
        LibraryWatcher w;
        w.setDebounceInterval(200);
        w.setMaxDelay(500);
        w.setRoot(tmp.path());
        QSignalSpy spy(&w, &LibraryWatcher::rescanRequested);
        QElapsedTimer t;
        t.start();
        int i = 0;
        while (spy.isEmpty() && t.elapsed() < 4000) {
            QFile f(tmp.filePath(QStringLiteral(".tmp-%1").arg(i++)));
            QVERIFY(f.open(QIODevice::WriteOnly)); // steady stream of changes
            f.close();
            QTest::qWait(60);
        }
        QVERIFY2(!spy.isEmpty(), "rescan postponed forever by continuous events");
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
    }

    void watcherNoticesRootComingBack() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("mount/photos"));
        LibraryWatcher w;
        w.setDebounceInterval(50);
        w.setRoot(root); // does not exist yet
        QVERIFY(w.watchedDirectories().contains(tmp.path()));
        QSignalSpy spy(&w, &LibraryWatcher::rescanRequested);
        QVERIFY(QDir().mkpath(root));
        QVERIFY(spy.wait(5000));
    }

    // --------------------------------------------------------------- GalleryView
    void selectionSurvivesModelRefresh() {
        ViewFixture f(6);
        QSignalSpy counts(&f.view, &GalleryView::selectionCountChanged);
        auto* sel = f.view.selectionModel();
        sel->select(f.model.index(1, 0), QItemSelectionModel::Select);
        sel->select(f.model.index(4, 0), QItemSelectionModel::Select);
        QVector<ImageRecord> reordered = records(6);
        std::reverse(reordered.begin(), reordered.end());
        reordered.removeAt(1); // drops img4
        counts.clear();
        f.model.setItems(reordered);
        QCOMPARE(f.view.getSelectedPaths(), QStringList{QStringLiteral("/media/img1.jpg")});
        QVERIFY(!counts.isEmpty());
        QCOMPARE(counts.last().at(0).toInt(), 1);
        QCOMPARE(counts.last().at(1).toStringList(), QStringList{QStringLiteral("/media/img1.jpg")});
    }

    void invertSelectionIsOneChange() {
        ViewFixture f(2000);
        f.view.selectionModel()->select(f.model.index(0, 0), QItemSelectionModel::Select);
        QSignalSpy counts(&f.view, &GalleryView::selectionCountChanged);
        QElapsedTimer t;
        t.start();
        f.view.invertSelection();
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(counts.count(), 1);
        QCOMPARE(f.view.getSelectedRows().size(), 1999);
    }

    void cardResizeKeepsSelection() {
        ViewFixture f(10);
        f.view.selectAllItems();
        f.delegate.setCardSize(200);
        QCoreApplication::processEvents();
        QCOMPARE(f.view.getSelectedRows().size(), 10);
        QTRY_COMPARE(f.view.visualRect(f.model.index(0, 0)).width(), 200);
    }

    void smallMovesInBatchModeDoNotRubberBand() {
        ViewFixture f(30);
        f.view.setSelectMode(true);
        auto* vp = f.view.viewport();
        for (int r : {2, 5, 9}) {
            sendMouse(vp, QEvent::MouseButtonPress, centerOf(f.view, r), Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, centerOf(f.view, r), Qt::LeftButton, Qt::NoButton);
        }
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{2, 5, 9}));
        const QPoint p = centerOf(f.view, 12);
        sendMouse(vp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, p + QPoint(3, 0), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, p + QPoint(5, 1), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, p + QPoint(5, 1), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{2, 5, 9, 12}));
    }

    void rightClickAndEmptyClickKeepBatch() {
        ViewFixture f(12);
        f.view.setSelectMode(true);
        auto* sel = f.view.selectionModel();
        for (int r : {1, 3, 5}) sel->select(f.model.index(r, 0), QItemSelectionModel::Select);
        auto* vp = f.view.viewport();
        sendMouse(vp, QEvent::MouseButtonPress, centerOf(f.view, 7), Qt::RightButton, Qt::RightButton);
        sendMouse(vp, QEvent::MouseButtonRelease, centerOf(f.view, 7), Qt::RightButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{1, 3, 5}));
        const QPoint empty(vp->width() - 2, vp->height() - 2);
        QVERIFY(!f.view.indexAt(empty).isValid());
        sendMouse(vp, QEvent::MouseButtonPress, empty, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, empty, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{1, 3, 5}));
    }

    // Regression: stray clicks / tiny drags in batch mode selected hundreds
    // of cards. Batch must only ever grow by what the user deliberately
    // covers, never be replaced.
    void strayDragOnEmptySpaceKeepsBatch() {
        ViewFixture f(300);
        f.view.setSelectMode(true);
        auto* sel = f.view.selectionModel();
        for (int r : {2, 5, 9}) sel->select(f.model.index(r, 0), QItemSelectionModel::Select);
        auto* vp = f.view.viewport();
        // A point in the spacing between two cards of the first row.
        const QRect r0 = f.view.visualRect(f.model.index(0, 0));
        const QPoint gap(r0.right() + 4, r0.center().y());
        QVERIFY(!f.view.indexAt(gap).isValid());
        sendMouse(vp, QEvent::MouseButtonPress, gap, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, gap + QPoint(2, 3), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, gap + QPoint(4, 6), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, gap + QPoint(4, 6), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{2, 5, 9}));
    }

    void deliberateRubberBandInBatchModeAdds() {
        ViewFixture f(300);
        f.view.setSelectMode(true);
        f.view.selectionModel()->select(f.model.index(9, 0), QItemSelectionModel::Select);
        auto* vp = f.view.viewport();
        const QRect r0 = f.view.visualRect(f.model.index(0, 0));
        const QRect r1 = f.view.visualRect(f.model.index(1, 0));
        const QPoint start(r0.right() + 4, r0.top() + 2);   // gap left of card 1
        const QPoint end(r1.center());                        // over card 1 only
        sendMouse(vp, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, (start + end) / 2, Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), (QVector<int>{1, 9}));
    }

    void dragAfterDoubleClickDoesNotUseStalePressPosition() {
        ViewFixture f(300);
        auto* vp = f.view.viewport();
        // A normal click at the top (the base class remembers this press).
        sendMouse(vp, QEvent::MouseButtonPress, centerOf(f.view, 0), Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, centerOf(f.view, 0), Qt::LeftButton, Qt::NoButton);
        f.view.setSelectMode(true);
        f.view.clearAllSelection();
        f.view.scrollTo(f.model.index(250, 0));
        QCoreApplication::processEvents();
        const QPoint p = centerOf(f.view, 250);
        QVERIFY(vp->rect().contains(p));
        // Stray double click on a card, mouse moves a little before release.
        sendMouse(vp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, p + QPoint(3, 2), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, p + QPoint(3, 2), Qt::LeftButton, Qt::NoButton);
        QVERIFY2(f.view.getSelectedRows().size() <= 1,
                 qPrintable(QStringLiteral("%1 cards selected").arg(f.view.getSelectedRows().size())));
    }

    void normalModeRubberBandStillWorks() {
        ViewFixture f(300);
        auto* vp = f.view.viewport();
        f.view.selectionModel()->select(f.model.index(9, 0), QItemSelectionModel::Select);
        const QRect r0 = f.view.visualRect(f.model.index(0, 0));
        const QRect r1 = f.view.visualRect(f.model.index(1, 0));
        const QPoint start(r0.right() + 4, r0.top() + 2);
        sendMouse(vp, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, r1.center(), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, r1.center(), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), QVector<int>{1}); // standard: band replaces
        // And a plain click on a card selects just that card.
        sendMouse(vp, QEvent::MouseButtonPress, centerOf(f.view, 4), Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, centerOf(f.view, 4), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.view.getSelectedRows(), QVector<int>{4});
    }

    void normalModeDoubleClickWobbleIsHarmless() {
        ViewFixture f(300);
        auto* vp = f.view.viewport();
        sendMouse(vp, QEvent::MouseButtonPress, centerOf(f.view, 0), Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, centerOf(f.view, 0), Qt::LeftButton, Qt::NoButton);
        f.view.scrollTo(f.model.index(250, 0));
        QCoreApplication::processEvents();
        const QPoint p = centerOf(f.view, 250);
        QSignalSpy open(&f.view, &GalleryView::openViewerRequested);
        sendMouse(vp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, p + QPoint(3, 2), Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, p + QPoint(3, 2), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(open.count(), 1);
        QCOMPARE(f.view.getSelectedRows(), QVector<int>{250});
    }

    void cardMenuActionsSurviveModelRefresh() {
        // A refresh while the menu is open resets the model, which invalidates
        // persistent indexes; the actions must still find the clicked card.
        ViewFixture f(6);
        QVector<ImageRecord> reversed = records(6);
        std::reverse(reversed.begin(), reversed.end()); // img2 moves to row 3
        const QString img2 = QStringLiteral("/media/img2.jpg");

        QSignalSpy open(&f.view, &GalleryView::openViewerRequested);
        runCardMenu(f.view, 2, QStringLiteral("Open in Viewer"), [&]() { f.model.setItems(reversed); });
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.first().at(0).value<ImageRecord>().path, img2);
        QCOMPARE(open.first().at(1).toInt(), 3);

        f.model.setItems(records(6));
        f.view.selectionModel()->clearSelection();
        runCardMenu(f.view, 2, QStringLiteral("Select"), [&]() { f.model.setItems(reversed); });
        QCOMPARE(f.view.getSelectedPaths(), QStringList{img2});

        f.model.setItems(records(6));
        QSignalSpy fav(&f.view, &GalleryView::favoriteToggled);
        runCardMenu(f.view, 2, QStringLiteral("Favorite"), [&]() { f.model.setItems(reversed); });
        QCOMPARE(fav.count(), 1);
        QCOMPARE(fav.first().at(0).toInt(), 3);
        QCOMPARE(fav.first().at(1).toString(), img2);
    }

    void favouriteShortcutRequiresNoModifiers() {
        ViewFixture f(3);
        f.view.setCurrentIndex(f.model.index(1, 0));
        QSignalSpy fav(&f.view, &GalleryView::favoriteToggled);
        QTest::keyClick(&f.view, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(&f.view, Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(fav.count(), 0);
        QTest::keyClick(&f.view, Qt::Key_F);
        QCOMPARE(fav.count(), 1);
        QCOMPARE(fav.first().at(1).toString(), QStringLiteral("/media/img1.jpg"));
    }

    // --------------------------------------------------------------- ViewerWindow
    void viewerArrowKeysNavigateWhateverHasFocus() {
        QTemporaryDir tmp;
        QVector<ImageRecord> items = records(3, tmp.filePath(QStringLiteral("v")));
        for (auto& r : items) {
            r.path = r.path.replace(QStringLiteral(".jpg"), QStringLiteral(".png"));
            QVERIFY(saveImage(r.path));
        }
        ViewerWindow viewer;
        viewer.showImage(items, 1);
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        for (auto* b : viewer.findChildren<QPushButton*>()) {
            QCOMPARE(b->focusPolicy(), Qt::NoFocus);
        }
        QTest::keyClick(&viewer, Qt::Key_Right);
        QCOMPARE(viewer.currentIndex(), 2);
        QTest::keyClick(&viewer, Qt::Key_Left);
        QTest::keyClick(&viewer, Qt::Key_Left);
        QCOMPARE(viewer.currentIndex(), 0);
        QVERIFY(viewer.fitMode());
        QTest::keyClick(&viewer, Qt::Key_Space);
        QVERIFY(!viewer.fitMode());
    }

    void viewerShowsFailureForUndecodableImage() {
        QTemporaryDir tmp;
        ImageRecord good;
        good.path = tmp.filePath(QStringLiteral("good.png"));
        good.filename = QStringLiteral("good.png");
        good.isFavorite = true;
        QVERIFY(saveImage(good.path));
        ImageRecord bad;
        bad.path = tmp.filePath(QStringLiteral("bad%2C.png"));
        bad.filename = QStringLiteral("bad%2C.png");
        QFile f(bad.path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("garbage");
        f.close();

        ViewerWindow viewer;
        viewer.showImage({good, bad}, 0);
        QPushButton* favButton = nullptr;
        for (auto* b : viewer.findChildren<QPushButton*>()) {
            if (b->text().contains(QStringLiteral("Favorite"))) favButton = b;
        }
        QVERIFY(favButton);
        QVERIFY(favButton->text().contains(QStringLiteral("Favorited")));
        QTest::keyClick(&viewer, Qt::Key_Right);
        QVERIFY2(viewer.windowTitle().startsWith(QStringLiteral("bad%2C.png")), qPrintable(viewer.windowTitle()));
        QVERIFY(!favButton->text().contains(QStringLiteral("Favorited")));
        bool reported = false;
        for (auto* l : viewer.findChildren<QLabel*>()) {
            reported = reported || l->text().contains(QStringLiteral("cannot display"));
        }
        QVERIFY(reported);
    }

    void viewerZoomOutNeverEnlarges() {
        QTemporaryDir tmp;
        ImageRecord tall;
        tall.path = tmp.filePath(QStringLiteral("tall.png"));
        tall.filename = QStringLiteral("tall.png");
        QVERIFY(saveImage(tall.path, 200, 9000));
        ViewerWindow viewer;
        viewer.resize(900, 600);
        viewer.showImage({tall}, 0);
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QCoreApplication::processEvents();
        const double fit = viewer.zoomFactor();
        QVERIFY(fit < 0.1);
        QTest::keyClick(&viewer, Qt::Key_Minus);
        QVERIFY(viewer.zoomFactor() < fit);
    }

    void viewerBackingStoreIsBounded() {
        // Also run with QT_SCALE_FACTOR=2 (ui_tests_hidpi): the cap must hold
        // in device pixels, not just logical ones.
        QTemporaryDir tmp;
        ImageRecord wide;
        wide.path = tmp.filePath(QStringLiteral("wide.png"));
        wide.filename = QStringLiteral("wide.png");
        QVERIFY(saveImage(wide.path, 1000, 100));
        ViewerWindow viewer;
        viewer.resize(900, 600);
        viewer.showImage({wide}, 0);
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        for (int i = 0; i < 100 && viewer.zoomFactor() < ViewerWindow::MAX_ZOOM; ++i) {
            QTest::keyClick(&viewer, Qt::Key_Plus);
        }
        QCOMPARE(viewer.zoomFactor(), ViewerWindow::MAX_ZOOM);
        const QLabel* label = nullptr;
        for (const QLabel* l : viewer.findChildren<QLabel*>()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPixmap pix = l->pixmap();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            const QPixmap pix = l->pixmap(Qt::ReturnByValue);
#else
            const QPixmap pix = l->pixmap() ? *l->pixmap() : QPixmap();
#endif
            if (!pix.isNull()) {
                label = l;
                QVERIFY2(pix.width() <= ViewerWindow::MAX_VIEW_DIMENSION
                             && pix.height() <= ViewerWindow::MAX_VIEW_DIMENSION,
                         qPrintable(QStringLiteral("%1x%2").arg(pix.width()).arg(pix.height())));
                // Exact fit, no sub-1 ratio (Qt 6.11 drops those).
                QVERIFY2(pix.devicePixelRatio() >= 1.0, qPrintable(QString::number(pix.devicePixelRatio())));
                QCOMPARE(qRound(pix.width() / pix.devicePixelRatio()), label->width());
            }
        }
        QVERIFY(label);
    }

    void viewerWheelZoomsInsideScrollArea() {
        QTemporaryDir tmp;
        ImageRecord big;
        big.path = tmp.filePath(QStringLiteral("big.png"));
        big.filename = QStringLiteral("big.png");
        QVERIFY(saveImage(big.path, 3000, 3000));
        ViewerWindow viewer;
        viewer.resize(900, 700);
        viewer.showImage({big}, 0);
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QTest::keyClick(&viewer, Qt::Key_0); // 1:1, so the scroll area can scroll
        QCOMPARE(viewer.zoomFactor(), 1.0);
        auto* vp = viewer.findChild<QScrollArea*>()->viewport();
        const QPoint pos(vp->width() / 2, vp->height() / 2);
        QWheelEvent down(QPointF(pos), QPointF(vp->mapToGlobal(pos)), QPoint(), QPoint(0, -120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(vp, &down);
        QVERIFY(viewer.zoomFactor() < 1.0);
    }

    // ----------------------------------------------------------------- MainWindow
    void mainWindowFavouritesViewAndRootChange() {
        QTemporaryDir tmp;
        const QString rootA = tmp.filePath(QStringLiteral("a"));
        const QString rootB = tmp.filePath(QStringLiteral("b"));
        QVERIFY(saveImage(rootA + QStringLiteral("/1.png")));
        QVERIFY(saveImage(rootA + QStringLiteral("/sub/2.png")));
        QVERIFY(saveImage(rootA + QStringLiteral("/3.png")));
        QVERIFY(saveImage(rootB + QStringLiteral("/x.png")));

        MainWindow w(rootA);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        auto* view = w.findChild<GalleryView*>();
        QVERIFY(view);
        QCOMPARE(view->model()->rowCount(), 3);

        Database db(Config::dbPath());
        QVERIFY(db.toggleFavorite(rootA + QStringLiteral("/1.png")));
        QVERIFY(db.toggleFavorite(rootA + QStringLiteral("/sub/2.png")));

        auto* folders = w.findChild<QListWidget*>();
        QVERIFY(folders);
        QListWidgetItem* favItem = nullptr;
        for (int i = 0; i < folders->count(); ++i) {
            if (folders->item(i)->text().startsWith(QStringLiteral("⭐"))) favItem = folders->item(i);
        }
        QVERIFY(favItem);
        emit folders->itemClicked(favItem);
        QCOMPARE(w.filterState().scope(), FilterState::Scope::Favorites);
        QCOMPARE(view->model()->rowCount(), 2);

        w.changeRootDir(rootB);
        QCOMPARE(w.filterState().scope(), FilterState::Scope::AllImages);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        QCOMPARE(view->model()->rowCount(), 1);
    }

    void mainWindowDropsFilterForVanishedFolder() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("r"));
        QVERIFY(saveImage(root + QStringLiteral("/top.png")));
        QVERIFY(saveImage(root + QStringLiteral("/sub1/a.png")));
        MainWindow w(root);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        auto* folders = w.findChild<QListWidget*>();
        QListWidgetItem* subItem = nullptr;
        for (int i = 0; i < folders->count(); ++i) {
            if (folders->item(i)->text().contains(QStringLiteral("sub1"))) subItem = folders->item(i);
        }
        QVERIFY(subItem);
        emit folders->itemClicked(subItem);
        QCOMPARE(w.filterState().scope(), FilterState::Scope::Subfolder);
        QVERIFY(QDir(root + QStringLiteral("/sub1")).removeRecursively());
        w.changeRootDir(root); // same root: plain rescan
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        QCOMPARE(w.filterState().scope(), FilterState::Scope::AllImages);
        QCOMPARE(w.findChild<GalleryView*>()->model()->rowCount(), 1);
    }

    void mainWindowSupersededScanIsIgnored() {
        QTemporaryDir tmp;
        const QString big = tmp.filePath(QStringLiteral("big"));
        for (int i = 0; i < 200; ++i) QVERIFY(saveImage(big + QStringLiteral("/%1.png").arg(i), 4, 4));
        const QString small = tmp.filePath(QStringLiteral("small"));
        QVERIFY(saveImage(small + QStringLiteral("/only.png")));
        MainWindow w(big);
        w.changeRootDir(small); // supersedes the first scan immediately
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        QCOMPARE(w.currentRootDir(), QDir(small).absolutePath());
        QCOMPARE(w.findChild<GalleryView*>()->model()->rowCount(), 1);
        bool sawTotal = false;
        for (auto* l : w.findChildren<QLabel*>()) {
            if (l->text().startsWith(QStringLiteral("Total:"))) {
                sawTotal = true;
                QVERIFY2(l->text().startsWith(QStringLiteral("Total: 1 images")), qPrintable(l->text()));
            }
        }
        QVERIFY(sawTotal);
    }

    void folderLabelsKeepPercentSequences() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("r"));
        QVERIFY(saveImage(root + QStringLiteral("/50%1off/a.png")));
        MainWindow w(root);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isScanInProgress(), 10000);
        auto* folders = w.findChild<QListWidget*>();
        bool found = false;
        for (int i = 0; i < folders->count(); ++i) {
            found = found || folders->item(i)->text() == QStringLiteral("📁 50%1off (1)");
        }
        QVERIFY(found);
    }
};

OMNIVIEW_TEST_MAIN(UiTest)
#include "test_ui.moc"
