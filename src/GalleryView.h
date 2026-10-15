#pragma once

#include <QListView>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QStringList>
#include "Database.h"
#include "ThumbnailWorker.h"

namespace OmniView {

class GalleryModel;

class GalleryView : public QListView {
    Q_OBJECT
public:
    explicit GalleryView(ThumbnailManager* thumbMgr, QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model) override;

    void setSelectMode(bool enabled);
    bool selectMode() const { return m_selectMode; }

    void setDarkMode(bool dark);
    bool darkMode() const { return m_darkMode; }

    QStringList getSelectedPaths() const;
    QVector<int> getSelectedRows() const;

    void copyImageBitmap(const QString& path);
    void startDragFiles(const QStringList& paths);

signals:
    void openViewerRequested(const OmniView::ImageRecord& item, int row);
    void selectionCountChanged(int count, const QStringList& paths);
    // Identified by path: rows can move if the model refreshes meanwhile
    // (e.g. while a context menu is open).
    void favoriteToggled(int row, const QString& path);
    void filterToSubfolderRequested(const QString& subfolder);
    void findSimilarRequested(const QString& dhash, const QString& filename);

public slots:
    void selectAllItems();
    void clearAllSelection();
    void invertSelection();
    void copySelectedFiles();
    void copySelectedPaths();

protected:
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent* event) override;
#else
    void enterEvent(QEvent* event) override;
#endif
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    const GalleryModel* galleryModel() const;
    void startDragForIndex(const QModelIndex& index);
    void resetPressState();
    void forwardToBase(QMouseEvent* event, bool additive);
    void emitSelectionCount();
    void rememberSelection();
    void restoreSelection();

    ThumbnailManager* m_thumbMgr;
    QPoint m_dragStartPos;
    bool m_pressActive = false;          // a left press is in progress
    bool m_pressConsumed = false;        // ...and this view handled it itself
    bool m_pressAdditive = false;        // ...on empty space in batch mode
    bool m_bandPending = false;          // batch-mode band not started yet (below drag distance)
    // QListView saw this gesture's press. Only then may it see the moves:
    // it builds rubber-band rectangles from the last press *it* received,
    // which is stale (possibly hundreds of rows away) when we consumed it.
    bool m_baseOwnsPress = false;
    QPersistentModelIndex m_pressIndex;  // card under the press, for dragging
    QPersistentModelIndex m_pendingToggleOnRelease;
    QPersistentModelIndex m_pendingSingleSelectOnRelease;
    QStringList m_selectionBeforeReset;
    bool m_selectMode = false;
    bool m_darkMode = true;
};

} // namespace OmniView
