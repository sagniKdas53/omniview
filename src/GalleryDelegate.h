#pragma once

#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include "ThumbnailWorker.h"

namespace OmniView {

class GalleryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GalleryDelegate(ThumbnailManager* thumbMgr, QObject* parent = nullptr);

    void setCardSize(int size);
    int cardSize() const { return m_cardSize; }

    void setSelectMode(bool enabled);
    bool selectMode() const { return m_selectMode; }

    void setDarkMode(bool dark);
    bool darkMode() const { return m_darkMode; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    ThumbnailManager* m_thumbMgr;
    int m_cardSize = 220;
    bool m_selectMode = false;
    bool m_darkMode = true;
};

} // namespace OmniView
