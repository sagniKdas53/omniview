#include "GalleryView.h"
#include "ColorUtils.h"
#include "FileActions.h"
#include "GalleryModel.h"
#include "GalleryDelegate.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QSet>

namespace OmniView {

namespace {

QString menuStyleSheet(bool dark) {
    return QStringLiteral(
        "QMenu {"
        "    background-color: %1;"
        "    color: %2;"
        "    border: 1px solid %3;"
        "    border-radius: 6px;"
        "    padding: 4px;"
        "}"
        "QMenu::item {"
        "    padding: 6px 20px 6px 12px;"
        "    border-radius: 4px;"
        "}"
        "QMenu::item:selected {"
        "    background-color: #4f46e5;"
        "    color: #ffffff;"
        "}"
        "QMenu::separator {"
        "    height: 1px;"
        "    background-color: %4;"
        "    margin: 4px 8px;"
        "}"
    ).arg(dark ? QStringLiteral("#1e2230") : QStringLiteral("#ffffff"),
          dark ? QStringLiteral("#e2e8f0") : QStringLiteral("#0f172a"),
          dark ? QStringLiteral("#334155") : QStringLiteral("#cbd5e1"),
          dark ? QStringLiteral("#334155") : QStringLiteral("#e2e8f0"));
}

QPointF localPosOf(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

QPointF globalPosOf(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition();
#else
    return e->screenPos();
#endif
}

bool hasNoModifiers(Qt::KeyboardModifiers mods) {
    return (mods & ~Qt::KeypadModifier) == Qt::NoModifier;
}

} // namespace

GalleryView::GalleryView(ThumbnailManager* thumbMgr, QWidget* parent)
    : QListView(parent)
    , m_thumbMgr(thumbMgr)
{
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true); // O(1) layout calculation
    setSpacing(8);
    setMouseTracking(true);
    setVerticalScrollMode(QListView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QListView::ExtendedSelection);
}

void GalleryView::setModel(QAbstractItemModel* newModel) {
    if (model()) {
        disconnect(model(), nullptr, this, nullptr);
    }
    QListView::setModel(newModel);
    if (newModel) {
        // A reset (every gallery refresh) clears the selection without any
        // selectionChanged signal. Carry the selection over by path and tell
        // listeners (batch bar, drag button) what is selected now.
        connect(newModel, &QAbstractItemModel::modelAboutToBeReset, this, &GalleryView::rememberSelection);
        connect(newModel, &QAbstractItemModel::modelReset, this, &GalleryView::restoreSelection);
    }
    emitSelectionCount();
}

const GalleryModel* GalleryView::galleryModel() const {
    return qobject_cast<const GalleryModel*>(model());
}

void GalleryView::rememberSelection() {
    m_selectionBeforeReset = getSelectedPaths();
    resetPressState();
}

void GalleryView::restoreSelection() {
    const QStringList paths = m_selectionBeforeReset;
    m_selectionBeforeReset.clear();
    const GalleryModel* gModel = galleryModel();
    if (gModel && selectionModel() && !paths.isEmpty()) {
        QItemSelection restored;
        for (const QString& p : paths) {
            const int row = gModel->rowForPath(p);
            if (row >= 0) {
                const QModelIndex idx = gModel->index(row, 0);
                restored.select(idx, idx);
            }
        }
        const QSignalBlocker blocker(selectionModel());
        selectionModel()->select(restored, QItemSelectionModel::ClearAndSelect);
        viewport()->update();
    }
    emitSelectionCount();
}

void GalleryView::emitSelectionCount() {
    const QStringList paths = getSelectedPaths();
    emit selectionCountChanged(static_cast<int>(paths.size()), paths);
}

void GalleryView::setSelectMode(bool enabled) {
    m_selectMode = enabled;
    if (auto* del = qobject_cast<GalleryDelegate*>(itemDelegate())) {
        del->setSelectMode(enabled);
    }
    viewport()->update();
}

void GalleryView::setDarkMode(bool dark) {
    m_darkMode = dark;
    if (auto* del = qobject_cast<GalleryDelegate*>(itemDelegate())) {
        del->setDarkMode(dark);
    }
    QPalette p = palette();
    const QColor bg = dark ? QColor(22, 25, 34) : QColor(248, 250, 252);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::Window, bg);
    setPalette(p);
    viewport()->setPalette(p);
    viewport()->update();
}

QVector<int> GalleryView::getSelectedRows() const {
    QVector<int> rows;
    if (!selectionModel()) return rows;
    // Single-column model: every selected index is a distinct row.
    const auto sel = selectionModel()->selectedIndexes();
    rows.reserve(sel.size());
    for (const auto& idx : sel) {
        if (idx.isValid() && idx.column() == 0) {
            rows.append(idx.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QStringList GalleryView::getSelectedPaths() const {
    QStringList paths;
    const GalleryModel* gModel = galleryModel();
    if (!gModel) return paths;

    for (int r : getSelectedRows()) {
        if (const auto* it = gModel->getItem(r)) {
            if (!it->path.isEmpty()) {
                paths.append(it->path);
            }
        }
    }
    return paths;
}

void GalleryView::selectAllItems() {
    if (!model() || !selectionModel()) return;
    const int count = model()->rowCount();
    if (count == 0) return;
    const QItemSelection sel(model()->index(0, 0), model()->index(count - 1, 0));
    selectionModel()->select(sel, QItemSelectionModel::Select);
}

void GalleryView::clearAllSelection() {
    if (selectionModel()) {
        selectionModel()->clearSelection();
    }
}

void GalleryView::invertSelection() {
    if (!model() || !selectionModel()) return;
    const int count = model()->rowCount();
    if (count == 0) return;
    // One Toggle over the whole range: a single selectionChanged instead of
    // one per row.
    const QItemSelection all(model()->index(0, 0), model()->index(count - 1, 0));
    selectionModel()->select(all, QItemSelectionModel::Toggle);
}

void GalleryView::copyImageBitmap(const QString& path) {
    const QImage img = ColorUtils::loadImage(path);
    if (!img.isNull()) {
        QApplication::clipboard()->setImage(img);
    }
}

void GalleryView::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) {
    QListView::selectionChanged(selected, deselected);
    emitSelectionCount();
}

void GalleryView::resetPressState() {
    m_dragStartPos = QPoint();
    m_pressActive = false;
    m_pressConsumed = false;
    m_pressAdditive = false;
    m_bandPending = false;
    m_baseOwnsPress = false;
    m_pressIndex = QPersistentModelIndex();
    m_pendingToggleOnRelease = QPersistentModelIndex();
    m_pendingSingleSelectOnRelease = QPersistentModelIndex();
}

void GalleryView::forwardToBase(QMouseEvent* event, bool additive) {
    if (!additive) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: QListView::mousePressEvent(event); break;
        case QEvent::MouseMove: QListView::mouseMoveEvent(event); break;
        case QEvent::MouseButtonRelease: QListView::mouseReleaseEvent(event); break;
        default: break;
        }
        return;
    }
    // Qt derives the selection command from every event's modifiers, so the
    // press, each move and the release must all carry Ctrl for the rubber
    // band to add to the batch instead of replacing it.
    QMouseEvent copy(event->type(), localPosOf(event), globalPosOf(event),
                     event->button(), event->buttons(), event->modifiers() | Qt::ControlModifier);
    forwardToBase(&copy, false);
}

void GalleryView::mousePressEvent(QMouseEvent* event) {
    const QModelIndex idx = indexAt(event->pos());

    if (event->button() == Qt::RightButton && m_selectMode && idx.isValid() && selectionModel()) {
        // Keep the batch intact: the context menu acts on the clicked card.
        selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        return;
    }

    if (event->button() != Qt::LeftButton) {
        resetPressState();
        QListView::mousePressEvent(event);
        return;
    }

    resetPressState();
    m_dragStartPos = event->pos();
    m_pressActive = true;
    m_pressIndex = QPersistentModelIndex(idx);

    if (m_selectMode) {
        if (idx.isValid() && selectionModel()) {
            if (!selectionModel()->isSelected(idx)) {
                selectionModel()->select(idx, QItemSelectionModel::Select);
                selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
            } else {
                m_pendingToggleOnRelease = QPersistentModelIndex(idx);
            }
            m_pressConsumed = true;
            viewport()->update();
            return;
        }
        // Empty space in batch mode: nothing happens for a click or a small
        // wobble; past the drag distance a rubber band *adds* to the batch.
        m_pressAdditive = true;
        m_bandPending = true;
        return;
    }

    if (idx.isValid() && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        const QVector<int> selRows = getSelectedRows();
        if (selRows.size() > 1 && selRows.contains(idx.row())) {
            // Pressing inside a multi-selection: keep it (for dragging) and
            // collapse to this card only on a plain click release.
            m_pendingSingleSelectOnRelease = QPersistentModelIndex(idx);
            m_pressConsumed = true;
            return;
        }
    }
    m_baseOwnsPress = true;
    QListView::mousePressEvent(event);
}

void GalleryView::mouseMoveEvent(QMouseEvent* event) {
    const QModelIndex hoverIdx = indexAt(event->pos());
    viewport()->setCursor(hoverIdx.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);

    if (!(event->buttons() & Qt::LeftButton)) {
        if (m_pressActive) resetPressState();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
        QListView::mouseMoveEvent(event);
        return;
    }

    if (m_pressActive && m_pressIndex.isValid()) {
        const int dist = (event->pos() - m_dragStartPos).manhattanLength();
        if (dist >= QApplication::startDragDistance()) {
            const QModelIndex pressed = m_pressIndex;
            const bool consumed = m_pressConsumed;
            const QPoint start = m_dragStartPos;
            resetPressState();
            setState(QAbstractItemView::NoState);
            if (!consumed) {
                // Let the base class finish its own press handling first.
                QMouseEvent fakeRelease(QEvent::MouseButtonRelease, QPointF(start),
                                        QPointF(viewport()->mapToGlobal(start)),
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QListView::mouseReleaseEvent(&fakeRelease);
            }
            startDragForIndex(pressed);
            setState(QAbstractItemView::NoState);
            return;
        }
        if (m_pressConsumed) {
            // We own this press: small movements before the drag threshold
            // must not start a rubber band that replaces the selection.
            return;
        }
    }

    if (m_bandPending) {
        if ((event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
            return; // stray wobble on empty space
        }
        m_bandPending = false;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(m_dragStartPos),
                          QPointF(viewport()->mapToGlobal(m_dragStartPos)),
                          Qt::LeftButton, Qt::LeftButton, event->modifiers());
        forwardToBase(&press, true);
        m_baseOwnsPress = true;
    }

    if (!m_baseOwnsPress) {
        // The button is down but QListView did not see this gesture's press
        // (we consumed it, it was a double-click, or it started elsewhere):
        // its remembered press position is stale and would select every card
        // between there and the cursor.
        return;
    }
    forwardToBase(event, m_pressAdditive);
}

void GalleryView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const bool consumed = m_pressConsumed;
        const bool additive = m_pressAdditive;
        const bool baseOwnsPress = m_baseOwnsPress;
        if (m_pendingToggleOnRelease.isValid() && selectionModel()) {
            selectionModel()->select(m_pendingToggleOnRelease, QItemSelectionModel::Deselect);
            selectionModel()->setCurrentIndex(m_pendingToggleOnRelease, QItemSelectionModel::NoUpdate);
            viewport()->update();
        } else if (m_pendingSingleSelectOnRelease.isValid() && selectionModel()) {
            selectionModel()->select(m_pendingSingleSelectOnRelease, QItemSelectionModel::ClearAndSelect);
            selectionModel()->setCurrentIndex(m_pendingSingleSelectOnRelease, QItemSelectionModel::NoUpdate);
            viewport()->update();
        }
        resetPressState();
        if (consumed) {
            setState(QAbstractItemView::NoState);
            return;
        }
        if (!baseOwnsPress || additive) {
            if (baseOwnsPress) {
                forwardToBase(event, true); // finish the additive rubber band
            }
            // Otherwise QListView never saw the press: a release would act on
            // its stale press state.
            setState(QAbstractItemView::NoState);
            return;
        }
    } else if (event->button() == Qt::RightButton && m_selectMode) {
        return; // the press was not forwarded either
    }
    QListView::mouseReleaseEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void GalleryView::enterEvent(QEnterEvent* event) {
#else
void GalleryView::enterEvent(QEvent* event) {
#endif
    QListView::enterEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        resetPressState();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

void GalleryView::leaveEvent(QEvent* event) {
    QListView::leaveEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        resetPressState();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

void GalleryView::startDragFiles(const QStringList& paths) {
    const QStringList localFiles = FileActions::resolveLocalFiles(paths);
    if (localFiles.isEmpty()) return;

    auto* drag = new QDrag(this);
    drag->setMimeData(FileActions::createFileMimeData(localFiles));

    // Badge from the thumbnail cache (keyed by the gallery path, not the
    // extracted file); fall back to a reduced decode, never a full one.
    QPixmap thumbPix = m_thumbMgr ? m_thumbMgr->getCachedPixmap(paths.first()) : QPixmap();
    if (thumbPix.isNull()) {
        const QImage small = ColorUtils::loadImage(paths.first(), QSize(240, 240));
        if (!small.isNull()) {
            thumbPix = QPixmap::fromImage(small.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    const QPixmap badge = FileActions::makeDragBadge(thumbPix, static_cast<int>(localFiles.size()));
    if (!badge.isNull()) {
        drag->setPixmap(badge);
        drag->setHotSpot(QPoint(badge.width() / 2, badge.height() / 2));
    }
    drag->exec(Qt::CopyAction);
}

void GalleryView::startDragForIndex(const QModelIndex& index) {
    const GalleryModel* gModel = galleryModel();
    if (!gModel || !index.isValid()) return;

    const auto* item = gModel->getItem(index.row());
    if (!item) return;

    const QVector<int> selectedRows = getSelectedRows();
    QStringList paths;
    if (selectedRows.contains(index.row()) && selectedRows.size() > 1) {
        paths = getSelectedPaths();
    } else {
        paths.append(item->path);
    }
    startDragFiles(paths);
}

void GalleryView::mouseDoubleClickEvent(QMouseEvent* event) {
    const QModelIndex idx = indexAt(event->pos());
    if (idx.isValid()) {
        if (const GalleryModel* gModel = galleryModel()) {
            if (const auto* it = gModel->getItem(idx.row())) {
                emit openViewerRequested(*it, idx.row());
                return;
            }
        }
    }
    QListView::mouseDoubleClickEvent(event);
}

void GalleryView::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const auto mods = event->modifiers();
    const GalleryModel* gModel = galleryModel();

    if ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) && hasNoModifiers(mods)) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid() && gModel) {
            if (const auto* it = gModel->getItem(idx.row())) {
                emit openViewerRequested(*it, idx.row());
                return;
            }
        }
    } else if (mods == Qt::ControlModifier && key == Qt::Key_A) {
        selectAllItems();
        return;
    } else if (key == Qt::Key_Escape && hasNoModifiers(mods)) {
        if (!getSelectedPaths().isEmpty()) {
            clearAllSelection();
            return;
        }
    } else if ((key == Qt::Key_F || key == Qt::Key_S) && hasNoModifiers(mods)) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid() && gModel) {
            if (const auto* it = gModel->getItem(idx.row())) {
                emit favoriteToggled(idx.row(), it->path);
                return;
            }
        }
    } else if (mods == Qt::ControlModifier && key == Qt::Key_C) {
        copySelectedFiles();
        return;
    } else if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) {
        copySelectedPaths();
        return;
    }

    QListView::keyPressEvent(event);
}

void GalleryView::copySelectedFiles() {
    FileActions::copyFilesToClipboard(getSelectedPaths());
}

void GalleryView::copySelectedPaths() {
    const QStringList paths = getSelectedPaths();
    if (paths.isEmpty()) return;
    QApplication::clipboard()->setText(paths.join(QStringLiteral("\n")));
}

void GalleryView::contextMenuEvent(QContextMenuEvent* event) {
    const QModelIndex idx = indexAt(event->pos());
    const GalleryModel* gModel = galleryModel();
    if (!gModel) return;

    const QStringList selectedPaths = getSelectedPaths();
    const ImageRecord* clicked = idx.isValid() ? gModel->getItem(idx.row()) : nullptr;
    const bool isMulti = selectedPaths.size() > 1 && clicked && selectedPaths.contains(clicked->path);

    QMenu menu(this);
    menu.setStyleSheet(menuStyleSheet(m_darkMode));

    if (isMulti) {
        auto* actCount = menu.addAction(QStringLiteral("📦 %1 files selected").arg(selectedPaths.size()));
        actCount->setEnabled(false);

        const auto* actDrag = menu.addAction(QStringLiteral("📤 Drag to Share / Attach (%1 files)").arg(selectedPaths.size()));
        connect(actDrag, &QAction::triggered, this, [this, selectedPaths]() {
            startDragFiles(selectedPaths);
        });

        menu.addSeparator();

        const auto* actCopyFiles = menu.addAction(QStringLiteral("📁 Copy %1 Files (Attach) (Ctrl+C)").arg(selectedPaths.size()));
        connect(actCopyFiles, &QAction::triggered, this, [selectedPaths]() {
            FileActions::copyFilesToClipboard(selectedPaths);
        });

        const auto* actCopyPaths = menu.addAction(QStringLiteral("📋 Copy %1 File Paths (Ctrl+Shift+C)").arg(selectedPaths.size()));
        connect(actCopyPaths, &QAction::triggered, this, [selectedPaths]() {
            QApplication::clipboard()->setText(selectedPaths.join(QStringLiteral("\n")));
        });

        menu.addSeparator();

        const auto* actInvert = menu.addAction(QStringLiteral("🔄 Invert Selection"));
        connect(actInvert, &QAction::triggered, this, &GalleryView::invertSelection);

        const auto* actClear = menu.addAction(QStringLiteral("✕ Clear Selection (Esc)"));
        connect(actClear, &QAction::triggered, this, &GalleryView::clearAllSelection);

    } else if (clicked) {
        // Copy the record and resolve the row by path when an action runs:
        // menu.exec() runs an event loop, and a refresh meanwhile resets the
        // model, which invalidates even persistent indexes.
        const ImageRecord itemRecord = *clicked;
        const QString path = itemRecord.path;
        const auto currentRow = [this, path]() {
            const GalleryModel* m = galleryModel();
            return m ? m->rowForPath(path) : -1;
        };
        const bool isSelected = selectionModel() && selectionModel()->isSelected(idx);

        const auto* actOpen = menu.addAction(QStringLiteral("🔍 Open in Viewer (Return)"));
        connect(actOpen, &QAction::triggered, this, [this, itemRecord, currentRow]() {
            const int row = currentRow();
            if (row >= 0) emit openViewerRequested(itemRecord, row);
        });

        const auto* actDrag = menu.addAction(QStringLiteral("📤 Drag to Share / Attach"));
        connect(actDrag, &QAction::triggered, this, [this, path]() {
            startDragFiles(QStringList{path});
        });

        const auto* actToggleSel = menu.addAction(isSelected ? QStringLiteral("☑️ Deselect") : QStringLiteral("☑️ Select"));
        connect(actToggleSel, &QAction::triggered, this, [this, currentRow, isSelected]() {
            const int row = currentRow();
            if (selectionModel() && model() && row >= 0) {
                selectionModel()->select(model()->index(row, 0), isSelected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select);
            }
        });

        menu.addSeparator();

        // These act on the clicked card, like every other entry here.
        const auto* actCopyFile = menu.addAction(QStringLiteral("📁 Copy File (Attach)"));
        connect(actCopyFile, &QAction::triggered, this, [path]() {
            FileActions::copyFilesToClipboard({path});
        });

        const auto* actCopyPath = menu.addAction(QStringLiteral("📋 Copy File Path"));
        connect(actCopyPath, &QAction::triggered, this, [path]() {
            QApplication::clipboard()->setText(path);
        });

        const auto* actCopyImg = menu.addAction(QStringLiteral("🖼️ Copy Image Bitmap"));
        connect(actCopyImg, &QAction::triggered, this, [this, path]() {
            copyImageBitmap(path);
        });

        if (!itemRecord.dhash.isEmpty()) {
            const auto* actSim = menu.addAction(QStringLiteral("✨ Find Visually Similar Images"));
            connect(actSim, &QAction::triggered, this, [this, itemRecord]() {
                emit findSimilarRequested(itemRecord.dhash, itemRecord.filename);
            });
        }

        menu.addSeparator();

        if (!itemRecord.subfolder.isEmpty()) {
            const QString sub = itemRecord.subfolder;
            const auto* actFolder = menu.addAction(
                QStringLiteral("📂 Show Only Folder: %1").arg(FileActions::escapeMnemonics(sub)));
            connect(actFolder, &QAction::triggered, this, [this, sub]() {
                emit filterToSubfolderRequested(sub);
            });
        }

        const auto* actFav = menu.addAction(itemRecord.isFavorite ? QStringLiteral("★ Unfavorite (F)") : QStringLiteral("⭐ Favorite (F)"));
        connect(actFav, &QAction::triggered, this, [this, currentRow, path]() {
            emit favoriteToggled(currentRow(), path);
        });

        menu.addSeparator();

        const auto* actOpenDef = menu.addAction(QStringLiteral("↗️ Open in Default Viewer"));
        connect(actOpenDef, &QAction::triggered, this, [path]() {
            FileActions::openWithDefaultApp(path);
        });

        const auto* actReveal = menu.addAction(QStringLiteral("🗂️ Reveal in File Manager"));
        connect(actReveal, &QAction::triggered, this, [path]() {
            FileActions::revealInFileManager(path);
        });
    } else {
        const auto* actSelectAll = menu.addAction(QStringLiteral("✓ Select All (Ctrl+A)"));
        connect(actSelectAll, &QAction::triggered, this, &GalleryView::selectAllItems);

        const auto* actInvert = menu.addAction(QStringLiteral("🔄 Invert Selection"));
        connect(actInvert, &QAction::triggered, this, &GalleryView::invertSelection);

        const auto* actClear = menu.addAction(QStringLiteral("✕ Clear Selection (Esc)"));
        connect(actClear, &QAction::triggered, this, &GalleryView::clearAllSelection);
    }

    menu.exec(event->globalPos());
}

} // namespace OmniView
