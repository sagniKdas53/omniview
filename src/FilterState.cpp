#include "FilterState.h"
#include "ColorUtils.h"

#include <algorithm>

namespace OmniView {

bool FilterState::isSelected(Scope scope, const QString& subfolder) const {
    if (scope != m_scope) return false;
    return scope != Scope::Subfolder || subfolder == m_subfolder;
}

void FilterState::selectAllImages() {
    m_scope = Scope::AllImages;
    m_subfolder.clear();
}

void FilterState::selectFavorites() {
    m_scope = Scope::Favorites;
    m_subfolder.clear();
}

void FilterState::selectRootFolder() {
    m_scope = Scope::RootFolder;
    m_subfolder.clear();
}

void FilterState::selectSubfolder(const QString& subfolder) {
    if (subfolder.isEmpty()) {
        selectRootFolder();
        return;
    }
    m_scope = Scope::Subfolder;
    m_subfolder = subfolder;
}

void FilterState::toggleColor(const QString& colorId) {
    if (colorId.isEmpty() || colorId == QStringLiteral("all") || colorId == m_colorName) {
        m_colorName = QStringLiteral("all");
    } else {
        m_colorName = colorId;
    }
}

void FilterState::setAspectType(const QString& aspectId) {
    m_aspectType = aspectId.isEmpty() ? QStringLiteral("all") : aspectId;
}

void FilterState::setSimilarityTarget(const QString& dhash, const QString& filename) {
    m_similarityHash = dhash;
    m_similarityName = filename;
}

void FilterState::clearSimilarityTarget() {
    m_similarityHash.clear();
    m_similarityName.clear();
}

void FilterState::resetForNewRoot() {
    selectAllImages();
    clearSimilarityTarget();
}

QueryFilter FilterState::toQueryFilter(const QString& rootDir) const {
    QueryFilter filter;
    filter.rootDir = rootDir;
    switch (m_scope) {
    case Scope::AllImages:
        filter.folderScope = FolderScope::All;
        break;
    case Scope::Favorites:
        filter.folderScope = FolderScope::All;
        filter.favoriteOnly = true;
        break;
    case Scope::RootFolder:
        filter.folderScope = FolderScope::Root;
        break;
    case Scope::Subfolder:
        filter.folderScope = FolderScope::Subfolder;
        filter.subfolder = m_subfolder;
        break;
    }
    filter.colorName = m_colorName;
    filter.aspectType = m_aspectType;
    filter.searchTerm = m_searchTerm;
    filter.sortBy = m_sortBy;
    return filter;
}

QVector<ImageRecord> FilterState::rankBySimilarity(const QVector<ImageRecord>& records,
                                                   const QString& dhash, int threshold) {
    QVector<QPair<int, ImageRecord>> scored;
    for (const auto& rec : records) {
        if (rec.dhash.isEmpty()) continue;
        const int dist = ColorUtils::hammingDistance(dhash, rec.dhash);
        if (dist <= threshold) {
            scored.append(qMakePair(dist, rec));
        }
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const QPair<int, ImageRecord>& a, const QPair<int, ImageRecord>& b) {
        return a.first < b.first;
    });
    QVector<ImageRecord> out;
    out.reserve(scored.size());
    for (const auto& pair : scored) {
        out.append(pair.second);
    }
    return out;
}

} // namespace OmniView
