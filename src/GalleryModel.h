#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QHash>
#include "Database.h"
#include "ColorUtils.h"

namespace OmniView {

namespace GalleryRoles {
    enum {
        PathRole = Qt::UserRole + 1,
        FilenameRole,
        SubfolderRole,
        FileSizeRole,
        MTimeRole,
        WidthRole,
        HeightRole,
        AspectTypeRole,
        ColorNameRole,
        ColorRRole,
        ColorGRole,
        ColorBRole,
        DHashRole,
        ThumbPathRole,
        IsFavoriteRole,
        IndexedRole
    };
}

class GalleryModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit GalleryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(const QVector<ImageRecord>& items);
    const ImageRecord* getItem(int row) const;
    int rowForPath(const QString& path) const;

    void updateItemFeatures(const QString& path, const ImageFeatures& feat);
    void setItemFavorite(int row, bool fav);

private:
    QVector<ImageRecord> m_items;
    QHash<QString, int> m_pathToRow;
};

} // namespace OmniView
