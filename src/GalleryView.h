#pragma once

#include <QListView>
#include <QPoint>
#include "Database.h"
#include "ThumbnailWorker.h"

namespace OmniView {

class GalleryModel;

class GalleryView : public QListView {
    Q_OBJECT
public:
    explicit GalleryView(ThumbnailManager* thumbMgr, QWidget* parent = nullptr);

    void setSelectMode(bool enabled);
    bool selectMode() const { return m_selectMode; }

    void setDarkMode(bool dark);
    bool darkMode() const { return m_darkMode; }

    QStringList getSelectedPaths() const;
    QVector<int> getSelectedRows() const;

    void selectAllItems();
    void clearAllSelection();

signals:
    void openViewerRequested(const ImageRecord& item, int row);
    void selectionCountChanged(int count, const QStringList& paths);
    void favoriteToggled(int row, const QString& path);

public slots:
    void copySelectedFiles();
    void copySelectedPaths();

protected:
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void startDragForIndex(const QModelIndex& index);

    ThumbnailManager* m_thumbMgr;
    QPoint m_dragStartPos;
    QPersistentModelIndex m_pendingToggleOnRelease;
    QPersistentModelIndex m_pendingSingleSelectOnRelease;
    bool m_selectMode = false;
    bool m_darkMode = true;
};

} // namespace OmniView
