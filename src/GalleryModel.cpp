#include "GalleryModel.h"

namespace OmniView {

GalleryModel::GalleryModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int GalleryModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_items.size();
}

QVariant GalleryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return QVariant();
    }

    const ImageRecord& item = m_items.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
    case GalleryRoles::FilenameRole:
        return item.filename;
    case GalleryRoles::PathRole:
        return item.path;
    case GalleryRoles::SubfolderRole:
        return item.subfolder;
    case GalleryRoles::FileSizeRole:
        return item.fileSize;
    case GalleryRoles::MTimeRole:
        return item.mtime;
    case GalleryRoles::WidthRole:
        return item.width;
    case GalleryRoles::HeightRole:
        return item.height;
    case GalleryRoles::AspectTypeRole:
        return item.aspectType;
    case GalleryRoles::ColorNameRole:
        return item.colorName;
    case GalleryRoles::ColorRRole:
        return item.colorR;
    case GalleryRoles::ColorGRole:
        return item.colorG;
    case GalleryRoles::ColorBRole:
        return item.colorB;
    case GalleryRoles::DHashRole:
        return item.dhash;
    case GalleryRoles::ThumbPathRole:
        return item.thumbPath;
    case GalleryRoles::IsFavoriteRole:
        return item.isFavorite;
    case GalleryRoles::IndexedRole:
        return item.indexed;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GalleryModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[GalleryRoles::PathRole] = "path";
    roles[GalleryRoles::FilenameRole] = "filename";
    roles[GalleryRoles::SubfolderRole] = "subfolder";
    roles[GalleryRoles::FileSizeRole] = "fileSize";
    roles[GalleryRoles::MTimeRole] = "mtime";
    roles[GalleryRoles::WidthRole] = "width";
    roles[GalleryRoles::HeightRole] = "height";
    roles[GalleryRoles::AspectTypeRole] = "aspectType";
    roles[GalleryRoles::ColorNameRole] = "colorName";
    roles[GalleryRoles::ColorRRole] = "colorR";
    roles[GalleryRoles::ColorGRole] = "colorG";
    roles[GalleryRoles::ColorBRole] = "colorB";
    roles[GalleryRoles::DHashRole] = "dhash";
    roles[GalleryRoles::ThumbPathRole] = "thumbPath";
    roles[GalleryRoles::IsFavoriteRole] = "isFavorite";
    roles[GalleryRoles::IndexedRole] = "indexed";
    return roles;
}

void GalleryModel::setItems(const QVector<ImageRecord>& items) {
    beginResetModel();
    m_items = items;
    m_pathToRow.clear();
    m_pathToRow.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        m_pathToRow.insert(m_items[i].path, i);
    }
    endResetModel();
}

const ImageRecord* GalleryModel::getItem(int row) const {
    if (row >= 0 && row < m_items.size()) {
        return &m_items.at(row);
    }
    return nullptr;
}

int GalleryModel::rowForPath(const QString& path) const {
    return m_pathToRow.value(path, -1);
}

void GalleryModel::updateItemFeatures(const QString& path, const ImageFeatures& feat) {
    const int row = rowForPath(path);
    if (row < 0 || row >= m_items.size()) return;

    if (!feat.valid) return;
    ImageRecord& item = m_items[row];
    if (feat.width <= 0 || feat.height <= 0) {
        item.thumbPath = feat.thumbPath;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {GalleryRoles::ThumbPathRole});
        return;
    }
    item.width = feat.width;
    item.height = feat.height;
    item.aspectType = feat.aspectType;
    item.colorName = feat.colorName;
    item.colorR = feat.colorR;
    item.colorG = feat.colorG;
    item.colorB = feat.colorB;
    item.dhash = feat.dhash;
    item.thumbPath = feat.thumbPath;
    item.indexed = true;

    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx);
}

void GalleryModel::setItemFavorite(int row, bool fav) {
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].isFavorite = fav;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {GalleryRoles::IsFavoriteRole});
}

} // namespace OmniView
