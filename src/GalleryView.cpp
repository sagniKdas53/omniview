#include "GalleryView.h"
#include "GalleryModel.h"
#include "GalleryDelegate.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QProcess>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QDesktopServices>

namespace OmniView {

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
    viewport()->update();
}

QVector<int> GalleryView::getSelectedRows() const {
    QVector<int> rows;
    const auto sel = selectionModel()->selectedIndexes();
    for (const auto& idx : sel) {
        if (idx.isValid() && !rows.contains(idx.row())) {
            rows.append(idx.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QStringList GalleryView::getSelectedPaths() const {
    QStringList paths;
    const auto* gModel = qobject_cast<const GalleryModel*>(model());
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
    for (int r = 0; r < count; ++r) {
        const QModelIndex idx = model()->index(r, 0);
        selectionModel()->select(idx, QItemSelectionModel::Toggle);
    }
}

void GalleryView::copyImageBitmap(const QString& path) {
    QImage img(path);
    if (!img.isNull()) {
        QApplication::clipboard()->setImage(img);
    }
}

void GalleryView::copySelectedImage() {
    const QStringList paths = getSelectedPaths();
    if (!paths.isEmpty()) {
        copyImageBitmap(paths.first());
    } else {
        const QModelIndex idx = currentIndex();
        if (idx.isValid()) {
            const auto* gModel = qobject_cast<const GalleryModel*>(model());
            if (gModel) {
                if (const auto* it = gModel->getItem(idx.row())) {
                    copyImageBitmap(it->path);
                }
            }
        }
    }
}

void GalleryView::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) {
    QListView::selectionChanged(selected, deselected);
    const QStringList paths = getSelectedPaths();
    emit selectionCountChanged(paths.size(), paths);
}

void GalleryView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        const QModelIndex idx = indexAt(event->pos());

        if (m_selectMode) {
            if (idx.isValid() && selectionModel()) {
                const bool isAlreadySelected = selectionModel()->isSelected(idx);
                if (!isAlreadySelected) {
                    selectionModel()->select(idx, QItemSelectionModel::Select);
                    selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
                    m_pendingToggleOnRelease = QPersistentModelIndex();
                } else {
                    m_pendingToggleOnRelease = QPersistentModelIndex(idx);
                }
                viewport()->update();
                return;
            } else {
                m_pendingToggleOnRelease = QPersistentModelIndex();
            }
        } else {
            if (idx.isValid() && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
                const QVector<int> selRows = getSelectedRows();
                if (selRows.size() > 1 && selRows.contains(idx.row())) {
                    m_pendingSingleSelectOnRelease = QPersistentModelIndex(idx);
                    return;
                }
            }
            m_pendingSingleSelectOnRelease = QPersistentModelIndex();
        }
    }
    QListView::mousePressEvent(event);
}

void GalleryView::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        m_dragStartPos = QPoint();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    } else if (!m_dragStartPos.isNull()) {
        const int dist = (event->pos() - m_dragStartPos).manhattanLength();
        if (dist >= QApplication::startDragDistance()) {
            const QModelIndex idx = indexAt(m_dragStartPos);
            if (idx.isValid()) {
                m_pendingToggleOnRelease = QPersistentModelIndex();
                m_pendingSingleSelectOnRelease = QPersistentModelIndex();
                setState(QAbstractItemView::NoState);
                QMouseEvent fakeRelease(QEvent::MouseButtonRelease, m_dragStartPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QListView::mouseReleaseEvent(&fakeRelease);
                startDragForIndex(idx);
                m_dragStartPos = QPoint();
                setState(QAbstractItemView::NoState);
                return;
            }
        }
    }
    QListView::mouseMoveEvent(event);
}

void GalleryView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_pendingToggleOnRelease.isValid() && selectionModel()) {
            selectionModel()->select(m_pendingToggleOnRelease, QItemSelectionModel::Deselect);
            selectionModel()->setCurrentIndex(m_pendingToggleOnRelease, QItemSelectionModel::NoUpdate);
            m_pendingToggleOnRelease = QPersistentModelIndex();
            viewport()->update();
        } else if (m_pendingSingleSelectOnRelease.isValid() && selectionModel()) {
            selectionModel()->select(m_pendingSingleSelectOnRelease, QItemSelectionModel::ClearAndSelect);
            selectionModel()->setCurrentIndex(m_pendingSingleSelectOnRelease, QItemSelectionModel::NoUpdate);
            m_pendingSingleSelectOnRelease = QPersistentModelIndex();
            viewport()->update();
        }
        m_dragStartPos = QPoint();
    }
    QListView::mouseReleaseEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

void GalleryView::enterEvent(QEvent* event) {
    QListView::enterEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        m_dragStartPos = QPoint();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

void GalleryView::leaveEvent(QEvent* event) {
    QListView::leaveEvent(event);
    if (QApplication::mouseButtons() == Qt::NoButton) {
        m_dragStartPos = QPoint();
        if (state() != QAbstractItemView::NoState) {
            setState(QAbstractItemView::NoState);
        }
    }
}

void GalleryView::startDragFiles(const QStringList& paths) {
    QStringList validPaths;
    for (const QString& p : paths) {
        if (QFile::exists(p)) {
            validPaths.append(p);
        }
    }
    if (validPaths.isEmpty()) return;

    auto* drag = new QDrag(this);
    auto* mimeData = new QMimeData();

    // 1. File URLs for external applications (Discord, Slack, Chrome, Dolphin, Nautilus)
    QList<QUrl> urls;
    for (const QString& p : validPaths) {
        urls.append(QUrl::fromLocalFile(p));
    }
    mimeData->setUrls(urls);

    // 2. Plain text paths
    mimeData->setText(validPaths.join(QStringLiteral("\n")));

    // 3. GNOME / KDE file manager copy format
    QStringList urlStrings;
    for (const QUrl& u : urls) {
        urlStrings.append(u.toString());
    }
    const QByteArray gnomeData = "copy\n" + urlStrings.join(QStringLiteral("\n")).toUtf8();
    mimeData->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);

    // Pre-populate clipboard so user can also instant-paste
    QApplication::clipboard()->setMimeData(mimeData);

    // 4. Create drag thumbnail badge
    QPixmap thumbPix;
    if (m_thumbMgr) {
        thumbPix = m_thumbMgr->getCachedPixmap(validPaths.first());
    }
    if (thumbPix.isNull()) {
        thumbPix.load(validPaths.first());
    }

    const int badgeMax = 120;
    if (!thumbPix.isNull()) {
        const QPixmap scaled = thumbPix.scaled(badgeMax, badgeMax, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap badge(scaled.width() + 12, scaled.height() + 12);
        badge.fill(Qt::transparent);

        QPainter p(&badge);
        p.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath shape;
        shape.addRoundedRect(QRectF(1, 1, scaled.width() + 10, scaled.height() + 10), 8, 8);
        p.fillPath(shape, QColor(20, 24, 38, 230));
        p.setPen(QPen(QColor(99, 102, 241), 2));
        p.drawPath(shape);
        p.drawPixmap(6, 6, scaled);

        if (validPaths.size() > 1) {
            const QString countStr = QStringLiteral("📦 %1 files").arg(validPaths.size());
            QFont f = p.font();
            f.setBold(true);
            f.setPointSize(9);
            p.setFont(f);
            QFontMetrics fm(f);
            const int pillW = fm.horizontalAdvance(countStr) + 12;
            const QRectF pill(badge.width() - pillW - 4, badge.height() - 22, pillW, 18);
            QPainterPath pp;
            pp.addRoundedRect(pill, 6, 6);
            p.fillPath(pp, QColor(79, 70, 229, 240));
            p.setPen(QColor(255, 255, 255));
            p.drawText(pill, Qt::AlignCenter, countStr);
        }
        p.end();

        drag->setPixmap(badge);
        drag->setHotSpot(QPoint(badge.width() / 2, badge.height() / 2));
    }

    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
}

void GalleryView::startDragForIndex(const QModelIndex& index) {
    const auto* gModel = qobject_cast<const GalleryModel*>(model());
    if (!gModel) return;

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
        const auto* gModel = qobject_cast<const GalleryModel*>(model());
        if (gModel) {
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

    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid()) {
            const auto* gModel = qobject_cast<const GalleryModel*>(model());
            if (gModel) {
                if (const auto* it = gModel->getItem(idx.row())) {
                    emit openViewerRequested(*it, idx.row());
                    return;
                }
            }
        }
    } else if (mods == Qt::ControlModifier && key == Qt::Key_A) {
        selectAllItems();
        return;
    } else if (key == Qt::Key_Escape) {
        if (!getSelectedPaths().isEmpty()) {
            clearAllSelection();
            return;
        }
    } else if (key == Qt::Key_F || key == Qt::Key_S) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid()) {
            const auto* gModel = qobject_cast<const GalleryModel*>(model());
            if (gModel) {
                if (const auto* it = gModel->getItem(idx.row())) {
                    emit favoriteToggled(idx.row(), it->path);
                    return;
                }
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
    const QStringList paths = getSelectedPaths();
    if (paths.isEmpty()) return;

    QList<QUrl> urls;
    for (const QString& p : paths) {
        if (QFile::exists(p)) {
            urls.append(QUrl::fromLocalFile(p));
        }
    }
    if (urls.isEmpty()) return;

    auto* mimeData = new QMimeData();
    mimeData->setUrls(urls);
    mimeData->setText(paths.join(QStringLiteral("\n")));

    QStringList urlStrings;
    for (const QUrl& u : urls) {
        urlStrings.append(u.toString());
    }
    const QByteArray gnomeData = "copy\n" + urlStrings.join(QStringLiteral("\n")).toUtf8();
    mimeData->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);

    QApplication::clipboard()->setMimeData(mimeData);
}

void GalleryView::copySelectedPaths() {
    const QStringList paths = getSelectedPaths();
    if (paths.isEmpty()) return;
    QApplication::clipboard()->setText(paths.join(QStringLiteral("\n")));
}

void GalleryView::contextMenuEvent(QContextMenuEvent* event) {
    const QModelIndex idx = indexAt(event->pos());
    const auto* gModel = qobject_cast<const GalleryModel*>(model());
    if (!gModel) return;

    const QStringList selectedPaths = getSelectedPaths();
    const bool isMulti = selectedPaths.size() > 1 && (idx.isValid() && selectedPaths.contains(gModel->getItem(idx.row())->path));

    QMenu menu(this);
    if (m_darkMode) {
        menu.setStyleSheet(QStringLiteral(
            "QMenu {"
            "    background-color: #1e2230;"
            "    color: #e2e8f0;"
            "    border: 1px solid #334155;"
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
            "    background-color: #334155;"
            "    margin: 4px 8px;"
            "}"
        ));
    } else {
        menu.setStyleSheet(QStringLiteral(
            "QMenu {"
            "    background-color: #ffffff;"
            "    color: #0f172a;"
            "    border: 1px solid #cbd5e1;"
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
            "    background-color: #e2e8f0;"
            "    margin: 4px 8px;"
            "}"
        ));
    }

    if (isMulti) {
        auto* actCount = menu.addAction(QStringLiteral("📦 %1 files selected").arg(selectedPaths.size()));
        actCount->setEnabled(false);

        auto* actDrag = menu.addAction(QStringLiteral("📤 Drag to Share / Attach (%1 files)").arg(selectedPaths.size()));
        connect(actDrag, &QAction::triggered, this, [this, selectedPaths]() {
            startDragFiles(selectedPaths);
        });

        menu.addSeparator();

        auto* actCopyFiles = menu.addAction(QStringLiteral("📁 Copy %1 Files (Attach) (Ctrl+C)").arg(selectedPaths.size()));
        connect(actCopyFiles, &QAction::triggered, this, &GalleryView::copySelectedFiles);

        auto* actCopyPaths = menu.addAction(QStringLiteral("📋 Copy %1 File Paths (Ctrl+Shift+C)").arg(selectedPaths.size()));
        connect(actCopyPaths, &QAction::triggered, this, &GalleryView::copySelectedPaths);

        menu.addSeparator();

        auto* actInvert = menu.addAction(QStringLiteral("🔄 Invert Selection"));
        connect(actInvert, &QAction::triggered, this, &GalleryView::invertSelection);

        auto* actClear = menu.addAction(QStringLiteral("✕ Clear Selection (Esc)"));
        connect(actClear, &QAction::triggered, this, &GalleryView::clearAllSelection);

    } else if (idx.isValid()) {
        const auto* item = gModel->getItem(idx.row());
        if (item) {
            const QString path = item->path;
            const bool isSelected = selectionModel() && selectionModel()->isSelected(idx);

            auto* actOpen = menu.addAction(QStringLiteral("🔍 Open in Viewer (Return)"));
            connect(actOpen, &QAction::triggered, this, [this, item, idx]() {
                emit openViewerRequested(*item, idx.row());
            });

            auto* actDrag = menu.addAction(QStringLiteral("📤 Drag to Share / Attach"));
            connect(actDrag, &QAction::triggered, this, [this, path]() {
                startDragFiles(QStringList{path});
            });

            auto* actToggleSel = menu.addAction(isSelected ? QStringLiteral("☑️ Deselect") : QStringLiteral("☑️ Select"));
            connect(actToggleSel, &QAction::triggered, this, [this, idx, isSelected]() {
                if (selectionModel()) {
                    selectionModel()->select(idx, isSelected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select);
                }
            });

            menu.addSeparator();

            auto* actCopyFile = menu.addAction(QStringLiteral("📁 Copy File (Attach) (Ctrl+C)"));
            connect(actCopyFile, &QAction::triggered, this, &GalleryView::copySelectedFiles);

            auto* actCopyPath = menu.addAction(QStringLiteral("📋 Copy File Path (Ctrl+Shift+C)"));
            connect(actCopyPath, &QAction::triggered, this, &GalleryView::copySelectedPaths);

            auto* actCopyImg = menu.addAction(QStringLiteral("🖼️ Copy Image Bitmap"));
            connect(actCopyImg, &QAction::triggered, this, [this, path]() {
                copyImageBitmap(path);
            });

            menu.addSeparator();

            if (!item->subfolder.isEmpty()) {
                const QString sub = item->subfolder;
                auto* actFolder = menu.addAction(QStringLiteral("📂 Show Only Folder: %1").arg(sub));
                connect(actFolder, &QAction::triggered, this, [this, sub]() {
                    emit filterToSubfolderRequested(sub);
                });
            }

            auto* actFav = menu.addAction(item->isFavorite ? QStringLiteral("★ Unfavorite (F)") : QStringLiteral("⭐ Favorite (F)"));
            connect(actFav, &QAction::triggered, this, [this, idx, item]() {
                emit favoriteToggled(idx.row(), item->path);
            });

            menu.addSeparator();

            auto* actOpenDef = menu.addAction(QStringLiteral("↗️ Open in Default Viewer"));
            connect(actOpenDef, &QAction::triggered, this, [path]() {
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });

            auto* actReveal = menu.addAction(QStringLiteral("🗂️ Reveal in File Manager"));
            connect(actReveal, &QAction::triggered, this, [path]() {
                const QFileInfo fi(path);
                if (!QProcess::startDetached(QStringLiteral("dolphin"), QStringList{QStringLiteral("--select"), path})) {
                    if (!QProcess::startDetached(QStringLiteral("nautilus"), QStringList{QStringLiteral("--select"), path})) {
                        QProcess::startDetached(QStringLiteral("xdg-open"), QStringList{fi.absolutePath()});
                    }
                }
            });
        }
    } else {
        auto* actSelectAll = menu.addAction(QStringLiteral("✓ Select All (Ctrl+A)"));
        connect(actSelectAll, &QAction::triggered, this, &GalleryView::selectAllItems);

        auto* actInvert = menu.addAction(QStringLiteral("🔄 Invert Selection"));
        connect(actInvert, &QAction::triggered, this, &GalleryView::invertSelection);

        auto* actClear = menu.addAction(QStringLiteral("✕ Clear Selection (Esc)"));
        connect(actClear, &QAction::triggered, this, &GalleryView::clearAllSelection);
    }

    menu.exec(event->globalPos());
}

} // namespace OmniView
